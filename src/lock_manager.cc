#include "lock_manager.h"
#include "toydb_table.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>

#include "my_base.h"
#include "my_dbug.h"
#include "sql/sql_class.h"

bool LockResourceId::operator<(const LockResourceId &rhs) const {
  if (this->type != rhs.type) return this->type < rhs.type;
  if (this->table != rhs.table) return this->table < rhs.table;
  if (this->sec_index_no != rhs.sec_index_no)
    return this->sec_index_no < rhs.sec_index_no;
  return this->row_key < rhs.row_key;
}

bool LockResourceId::operator==(const LockResourceId &rhs) const {
  return this->type == rhs.type && this->table == rhs.table &&
         this->sec_index_no == rhs.sec_index_no && this->row_key == rhs.row_key;
}

/**
 * @brief Clustered Index全体のロックIDを生成
 */
LockResourceId LockResourceId::clustered_index(const ToydbTable *table) {
  return {LockResourceType::ClusteredIndexTree, table, std::nullopt,
          std::nullopt};
}

/**
 * @brief Secondary Index全体のロックIDを生成
 */
LockResourceId LockResourceId::secondary_index(const ToydbTable *table,
                                               uint sec_index_no) {
  return {LockResourceType::SecondaryIndexTree, table, sec_index_no,
          std::nullopt};
}

LockResourceId LockResourceId::row(const ToydbTable *table, ToydbIndexKey key) {
  return {LockResourceType::Row, table, std::nullopt, std::move(key)};
}

LockManager::LockEntry &LockManager::get_or_create(const LockResourceId &res) {
  auto it = this->resources.find(res);
  if (it != this->resources.end()) return *it->second;

  auto [inserted, _] =
      this->resources.emplace(res, std::make_unique<LockEntry>());
  return *inserted->second;
}

/**
 * @brief Xロックがない or Xロックが自分なら許可
 */
bool LockManager::can_grant_shared(const LockEntry &entry, THD *tx) const {
  return entry.exclusive_holder == std::nullopt || entry.exclusive_holder == tx;
}

/**
 * @brief SHロックが自分だけ or SHロックなし、かつXロックがない or
 * Xロックが自分なら許可
 */
bool LockManager::can_grant_exclusive(const LockEntry &entry, THD *tx) const {
  if (entry.exclusive_holder != std::nullopt && entry.exclusive_holder != tx) {
    return false;
  }
  return std::ranges::all_of(entry.shared_holders,
                             [&](THD *h) { return h == tx; });
}

void LockManager::register_held(THD *tx, const LockResourceId &res) {
  auto &held = this->tx_locks[tx];

  const bool notHeld = std::find(held.begin(), held.end(), res) == held.end();
  if (notHeld) {
    held.push_back(res);
  }
}

int LockManager::slock(const LockResourceId &res, THD *tx,
                       std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(this->mgr_mutex);
  LockEntry &entry = this->get_or_create(res);

  // 既にXロック所持ならno-op
  if (entry.exclusive_holder == tx) {
    this->register_held(tx, res);
    return 0;
  }
  // 既にSロック所持ならno-op
  if (entry.shared_holders.contains(tx)) {
    this->register_held(tx, res);
    return 0;
  }

  const bool got = entry.cv.wait_for(
      lock, timeout, [&] { return this->can_grant_shared(entry, tx); });
  if (!got) {
    DBUG_PRINT("toydb", ("LockManager::slock timeout"));
    return HA_ERR_LOCK_WAIT_TIMEOUT;
  }

  entry.shared_holders.insert(tx);
  this->register_held(tx, res);
  return 0;
}

int LockManager::xlock(const LockResourceId &res, THD *tx,
                       std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(this->mgr_mutex);

  LockEntry &entry = this->get_or_create(res);

  // 既にXロック所持ならno-op
  if (entry.exclusive_holder == tx) {
    this->register_held(tx, res);
    return 0;
  }

  const bool got = entry.cv.wait_for(
      lock, timeout, [&] { return this->can_grant_exclusive(entry, tx); });
  if (!got) {
    DBUG_PRINT("toydb", ("LockManager::xlock timeout"));
    return HA_ERR_LOCK_WAIT_TIMEOUT;
  }

  // S→X upgrade時は自分のSH holderエントリを削除
  entry.shared_holders.erase(tx);
  entry.exclusive_holder = tx;
  this->register_held(tx, res);

  return 0;
}

void LockManager::unlock(const LockResourceId &res, THD *tx) {
  std::unique_lock<std::mutex> lock(this->mgr_mutex);

  auto it = this->resources.find(res);
  if (it == this->resources.end()) return;
  LockEntry &entry = *it->second;

  bool changed = false;
  if (entry.exclusive_holder == tx) {
    entry.exclusive_holder = std::nullopt;
    changed = true;
  }
  if (entry.shared_holders.erase(tx) > 0) changed = true;

  auto tx_it = this->tx_locks.find(tx);
  if (tx_it != this->tx_locks.end()) {
    auto &held = tx_it->second;
    held.erase(std::remove(held.begin(), held.end(), res), held.end());
    if (held.empty()) this->tx_locks.erase(tx_it);
  }

  if (changed) entry.cv.notify_all();
}

void LockManager::unlock_all(THD *tx) {
  std::unique_lock<std::mutex> lock(this->mgr_mutex);

  auto tx_it = this->tx_locks.find(tx);
  if (tx_it == this->tx_locks.end()) return;

  auto held = std::move(tx_it->second);
  this->tx_locks.erase(tx_it);

  for (const auto &res : held) {
    auto it = this->resources.find(res);
    if (it == this->resources.end()) continue;

    LockEntry &entry = *it->second;
    bool changed = false;
    if (entry.exclusive_holder == tx) {
      entry.exclusive_holder = std::nullopt;
      changed = true;
    }
    if (entry.shared_holders.erase(tx) > 0) changed = true;
    if (changed) entry.cv.notify_all();
  }
}

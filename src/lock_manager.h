#ifndef LOCK_MANAGER_H
#define LOCK_MANAGER_H

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "my_inttypes.h"
#include "sql/sql_class.h"
#include "toydb_table.h"

/**
 * @brief ロックマネージャが扱うリソースの種別
 */
enum class LockResourceType {
  ClusteredIndexTree,  // Clustered Index全体
  SecondaryIndexTree,  // 各 secondary index全体
  Row,                 // Clustered Indexの行
  SecRow,              // セカンダリインデックスの行
};

/**
 * @brief ロック対象リソースの識別子
 */
struct LockResourceId {
  LockResourceType type;
  const ToydbTable *table{nullptr};

  /**
   * Secondary Indexのロック時はインデックスの番号を指定する
   */
  std::optional<uint> sec_index_no;
  std::optional<ToydbIndexKey> row_key;

  bool operator<(const LockResourceId &rhs) const;
  bool operator==(const LockResourceId &rhs) const;

  static LockResourceId clustered_index(const ToydbTable *table);
  static LockResourceId secondary_index(const ToydbTable *table,
                                        uint sec_index_no);
  static LockResourceId row(const ToydbTable *table, ToydbIndexKey key);
};

enum class LockMode {
  SHARED,
  EXCLUSIVE,
};

/**
 * @brief トランザクション単位でロックを保持するマネージャ
 */
class LockManager final {
 public:
  static constexpr auto default_timeout = std::chrono::seconds(10);

  LockManager() = default;
  ~LockManager() = default;

  LockManager(const LockManager &) = delete;
  LockManager &operator=(const LockManager &) = delete;

  // 成功=0 / タイムアウト=HA_ERR_LOCK_WAIT_TIMEOUT
  int slock(const LockResourceId &res, THD *tx,
            std::chrono::milliseconds timeout = default_timeout);
  int xlock(const LockResourceId &res, THD *tx,
            std::chrono::milliseconds timeout = default_timeout);

  void unlock(const LockResourceId &res, THD *tx);
  void unlock_all(THD *tx);

 private:
  struct LockEntry {
    std::condition_variable cv;
    std::unordered_set<THD *> shared_holders;
    std::optional<THD *> exclusive_holder{std::nullopt};
  };

  std::map<LockResourceId, std::unique_ptr<LockEntry>> resources;

  // あるtxが保持するロックリスト
  std::unordered_map<THD *, std::vector<LockResourceId>> tx_locks;

  // manager全体を保護するmutex
  std::mutex mgr_mutex;

  LockEntry &get_or_create(const LockResourceId &res);
  bool can_grant_shared(const LockEntry &entry, THD *tx) const;
  bool can_grant_exclusive(const LockEntry &entry, THD *tx) const;
  void register_held(THD *tx, const LockResourceId &res);
};

#endif  // LOCK_MANAGER_H

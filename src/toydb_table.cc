/* Copyright (c) 2004, 2025, Oracle and/or its affiliates.
   Copyright (c) 2026, sakurai-ryo

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is designed to work with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have either included with
  the program or referenced in the documentation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "toydb_table.h"

#include <cstddef>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "field_types.h"
#include "my_base.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "mysqld_error.h"

static bool check_type_match(enum_field_types expected, ColumnFlag flags,
                             const SupportedDBValue &value) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  if (std::holds_alternative<std::monostate>(value)) {
    return has_flag(flags, ColumnFlag::NOT_NULL);
  }

  switch (expected) {
    case enum_field_types::MYSQL_TYPE_TINY:
    case enum_field_types::MYSQL_TYPE_SHORT:
    case enum_field_types::MYSQL_TYPE_LONG:
    case enum_field_types::MYSQL_TYPE_LONGLONG:
      if (has_flag(flags, ColumnFlag::UNSIGNED)) {
        return std::holds_alternative<int64>(value) &&
               std::get<int64>(value) >= 0;
      } else {
        return std::holds_alternative<int64>(value);
      }
    case enum_field_types::MYSQL_TYPE_VAR_STRING:
    case enum_field_types::MYSQL_TYPE_VARCHAR:
    case enum_field_types::MYSQL_TYPE_STRING:
      return std::holds_alternative<std::string>(value);
    default:
      return false;
  }
}

static bool check_supported_type(enum_field_types type) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  switch (type) {
    case enum_field_types::MYSQL_TYPE_TINY:
    case enum_field_types::MYSQL_TYPE_SHORT:
    case enum_field_types::MYSQL_TYPE_LONG:
    case enum_field_types::MYSQL_TYPE_LONGLONG:
    case enum_field_types::MYSQL_TYPE_VAR_STRING:
    case enum_field_types::MYSQL_TYPE_VARCHAR:
    case enum_field_types::MYSQL_TYPE_STRING:
      return true;
    default:
      return false;
  }
}

ToydbTable::ToydbTable(std::string name) : table_name(std::move(name)) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
}

void ToydbTable::set_primary_key(std::vector<uint> indices) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  this->pk_column_indices = std::move(indices);
}

ulonglong ToydbTable::get_next_auto_inc_value() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return this->next_auto_inc_value;
}

void ToydbTable::set_auto_inc_value(ulonglong value) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  this->next_auto_inc_value = value;
}

void ToydbTable::update_auto_inc_value(ulonglong value) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  if (value >= this->next_auto_inc_value) {
    this->next_auto_inc_value = value + 1;
  }
}

/**
 * @brief 行データからインデックスキーを構築する
 */
ToydbIndexKey ToydbTable::build_pk_key_from_row(
    const std::vector<SupportedDBValue> &row) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  // pk_column_indicesにはPKのカラムインデックス値が入っているので
  // そこから対応するカラムの値を取り出してkey_partsに入れる
  ToydbIndexKey key;
  for (uint idx : this->pk_column_indices) {
    key.key_parts.push_back(row.at(idx));
  }
  return key;
}

ToydbTable::RowIterator ToydbTable::rows_begin() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return this->rows.begin();
}

ToydbTable::RowIterator ToydbTable::rows_end() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return this->rows.end();
}

ToydbTable::RowIterator ToydbTable::rows_last() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  auto iter = this->rows.end();
  --iter;
  return iter;
}

ToydbTable::RowIterator ToydbTable::find_row(const ToydbIndexKey &key) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return this->rows.find(key);
}

ToydbTable::RowIterator ToydbTable::lower_bound_row(
    const ToydbIndexKey &key) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return this->rows.lower_bound(key);
}

ToydbTable::RowIterator ToydbTable::upper_bound_row(
    const ToydbIndexKey &key) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return this->rows.upper_bound(key);
}

int ToydbTable::add_column(const std::string &name, enum_field_types type,
                           ColumnFlag flags) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  if (!this->rows.empty()) {
    return HA_ERR_INTERNAL_ERROR;
  }

  if (!check_supported_type(type)) {
    return HA_ERR_UNSUPPORTED;
  }

  this->columns.push_back({name, type, flags});
  return 0;
}

int ToydbTable::insert_row(std::vector<SupportedDBValue> row_data) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  if (row_data.size() != this->columns.size()) {
    return ER_WRONG_VALUE_COUNT;
  }

  for (size_t i = 0; i < this->columns.size(); ++i) {
    const auto &col = this->columns.at(i);
    if (!check_type_match(col.type, col.flags, row_data.at(i))) {
      return ER_INCORRECT_TYPE;
    }
  }

  ToydbIndexKey key;
  if (!this->pk_column_indices.empty()) {
    // PRIMARY KEYのカラム値をClustered Indexのキーとして使用する
    key = build_pk_key_from_row(row_data);
    if (this->rows.contains(key)) {
      return HA_ERR_FOUND_DUPP_KEY;
    }
  } else {
    // PKがない場合は内部のrow_idをキーとして使用する
    const ToydbRowId id = this->next_row_id++;
    key = ToydbIndexKey{{static_cast<int64>(id)}};
  }

  // セカンダリインデックスにエントリを追加
  int sec_ret = insert_secondary_keys(row_data);
  if (sec_ret != 0) return sec_ret;

  const ToydbRowId row_id = this->next_row_id++;
  ToydbRow row{row_id, std::move(row_data)};
  this->rows.emplace(std::move(key), std::move(row));
  return 0;
}

int ToydbTable::update_row(size_t row_index,
                           std::vector<SupportedDBValue> row_data) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  if (row_index >= this->rows.size()) {
    return HA_ERR_KEY_NOT_FOUND;
  }

  if (row_data.size() != this->columns.size()) {
    return ER_WRONG_VALUE_COUNT;
  }

  for (size_t i = 0; i < this->columns.size(); ++i) {
    const auto &col = this->columns.at(i);
    if (!check_type_match(col.type, col.flags, row_data.at(i))) {
      return ER_INCORRECT_TYPE;
    }
  }

  auto it = this->rows.begin();
  std::advance(it, row_index);
  it->second.values = std::move(row_data);
  return 0;
}

int ToydbTable::delete_row(size_t row_index) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  if (row_index >= this->rows.size()) {
    return HA_ERR_KEY_NOT_FOUND;
  }
  auto it = this->rows.begin();
  std::advance(it, row_index);
  this->rows.erase(it);
  return 0;
}

int ToydbTable::update_row_by_key(const ToydbIndexKey &key,
                                  std::vector<SupportedDBValue> row_data) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  auto it = this->rows.find(key);
  if (it == this->rows.end()) return HA_ERR_KEY_NOT_FOUND;

  if (row_data.size() != this->columns.size()) return ER_WRONG_VALUE_COUNT;

  for (size_t i = 0; i < this->columns.size(); ++i) {
    const auto &col = this->columns.at(i);
    if (!check_type_match(col.type, col.flags, row_data.at(i)))
      return ER_INCORRECT_TYPE;
  }

  // 旧セカンダリキーを削除 & 新セカンダリキーを挿入
  const auto &old_values = it->second.values;
  remove_secondary_keys(old_values);
  int sec_ret = insert_secondary_keys(row_data);
  if (sec_ret != 0) {
    // エラー時は旧セカンダリキーを戻す
    insert_secondary_keys(old_values);
    return sec_ret;
  }

  // PKカラムの値が変わった場合はキーを差し替える
  if (!pk_column_indices.empty()) {
    ToydbIndexKey new_key = build_pk_key_from_row(row_data);
    if (!(new_key == key)) {
      // 新しいキーが既に存在する場合は重複エラー
      if (this->rows.contains(new_key)) {
        // セカンダリインデックスもロールバック
        remove_secondary_keys(row_data);
        insert_secondary_keys(old_values);
        return HA_ERR_FOUND_DUPP_KEY;
      }
      ToydbRow row{it->second.id, std::move(row_data)};
      this->rows.erase(it);
      this->rows.emplace(std::move(new_key), std::move(row));
      return 0;
    }
  }

  it->second.values = std::move(row_data);
  return 0;
}

int ToydbTable::delete_row_by_key(const ToydbIndexKey &key) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  auto it = this->rows.find(key);
  if (it == this->rows.end()) return HA_ERR_KEY_NOT_FOUND;
  remove_secondary_keys(it->second.values);
  this->rows.erase(it);
  return 0;
}

size_t ToydbTable::row_count() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return this->rows.size();
}

const std::vector<SupportedDBValue> &ToydbTable::get_row(
    size_t row_index) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  auto it = this->rows.begin();
  std::advance(it, row_index);
  return it->second.values;
}

const std::vector<ToydbColumn> &ToydbTable::get_columns() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return this->columns;
}

void ToydbTable::clear_rows() {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  // Clustered Indexとセカンダリインデックスの両方を全削除する
  this->rows.clear();
  for (auto &[idx_no, sec_idx] : this->secondary_indexes) {
    sec_idx.entries.clear();
  }
}

void ToydbTable::add_secondary_index(uint mysql_index_no, std::string name,
                                     std::vector<uint> col_indices,
                                     bool is_unique) {
  DBUG_PRINT("toydb", ("%s index_no=%u name=%s is_unique=%d", __func__,
                       mysql_index_no, name.c_str(), is_unique));

  ToydbSecondaryIndex idx;
  idx.name = std::move(name);
  idx.column_indices = std::move(col_indices);
  idx.user_key_parts_count = idx.column_indices.size();
  idx.is_unique = is_unique;
  this->secondary_indexes.emplace(mysql_index_no, std::move(idx));
}

/**
 * @brief 行データからセカンダリインデックスキーを構築する
 */
ToydbIndexKey ToydbTable::build_secondary_key(
    uint mysql_index_no, const std::vector<SupportedDBValue> &row_data) const {
  DBUG_PRINT("toydb", ("%s", __func__));

  const ToydbSecondaryIndex &sec_idx =
      this->secondary_indexes.at(mysql_index_no);

  ToydbIndexKey key;
  for (uint col_idx : sec_idx.column_indices) {
    key.key_parts.push_back(row_data.at(col_idx));
  }
  // セカンダリキーの後ろにはPKをくっつける（非ユニークセカンダリインデックスにてキーがかぶるため）
  for (uint pk_idx : this->pk_column_indices) {
    key.key_parts.push_back(row_data.at(pk_idx));
  }
  return key;
}

/**
 * @brief セカンダリインデックスのキーからPK部分を抽出する
 */
ToydbIndexKey ToydbTable::extract_pk_key_from_secondary(
    uint mysql_index_no, const ToydbIndexKey &sec_key) const {
  DBUG_PRINT("toydb", ("%s", __func__));

  const ToydbSecondaryIndex &sec_idx =
      this->secondary_indexes.at(mysql_index_no);

  // PKはセカンダリキーの最後にくっついているので、セカンダリキーだけをスキップしてPK部分だけ抜き取る
  ToydbIndexKey pk_key;
  for (size_t i = sec_idx.user_key_parts_count; i < sec_key.key_parts.size();
       i++) {
    pk_key.key_parts.push_back(sec_key.key_parts.at(i));
  }
  return pk_key;
}

bool key_prefix_matches(const ToydbIndexKey &full_key,
                        const ToydbIndexKey &prefix) {
  if (full_key.key_parts.size() < prefix.key_parts.size()) return false;

  for (size_t i = 0; i < prefix.key_parts.size(); i++) {
    if (full_key.key_parts.at(i) != prefix.key_parts.at(i)) return false;
  }

  return true;
}

/**
 * @brief 行の挿入時にセカンダリインデックスにエントリを追加する
 */
int ToydbTable::insert_secondary_keys(
    const std::vector<SupportedDBValue> &row_data) {
  DBUG_PRINT("toydb", ("%s", __func__));

  // まず全インデックスのユニーク制約を検証する
  for (auto &[idx_no, sec_idx] : this->secondary_indexes) {
    if (!sec_idx.is_unique) continue;

    ToydbIndexKey key = build_secondary_key(idx_no, row_data);
    ToydbIndexKey prefix;
    for (size_t i = 0; i < sec_idx.user_key_parts_count; i++) {
      prefix.key_parts.push_back(key.key_parts.at(i));
    }
    auto it = sec_idx.entries.lower_bound(prefix);
    if (it != sec_idx.entries.end() && key_prefix_matches(*it, prefix)) {
      return HA_ERR_FOUND_DUPP_KEY;
    }
  }

  // 全てのユニーク制約を通過したら挿入する
  for (auto &[idx_no, sec_idx] : this->secondary_indexes) {
    ToydbIndexKey key = build_secondary_key(idx_no, row_data);
    sec_idx.entries.insert(std::move(key));
  }
  return 0;
}

int ToydbTable::remove_secondary_keys(
    const std::vector<SupportedDBValue> &row_data) {
  DBUG_PRINT("toydb", ("%s", __func__));

  for (auto &[idx_no, sec_idx] : this->secondary_indexes) {
    ToydbIndexKey key = build_secondary_key(idx_no, row_data);
    sec_idx.entries.erase(key);
  }
  return 0;
}

bool ToydbTable::has_secondary_index(uint mysql_index_no) const {
  return this->secondary_indexes.contains(mysql_index_no);
}

const ToydbSecondaryIndex &ToydbTable::get_secondary_index(
    uint mysql_index_no) const {
  return this->secondary_indexes.at(mysql_index_no);
}

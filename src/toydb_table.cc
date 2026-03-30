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

static bool check_type_match(enum_field_types expected,
                             const SupportedDBValue &value) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  switch (expected) {
    case enum_field_types::MYSQL_TYPE_TINY:
    case enum_field_types::MYSQL_TYPE_SHORT:
    case enum_field_types::MYSQL_TYPE_LONG:
    case enum_field_types::MYSQL_TYPE_LONGLONG:
      return std::holds_alternative<int64>(value);
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

/**
 * @brief 行データからインデックスキーを構築する
 */
ToydbIndexKey ToydbTable::build_key_from_row(
    const std::vector<SupportedDBValue> &row) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  // pk_column_indicesにはPKのカラムインデックス値が入っているので
  // そこから対応するカラムの値を取り出してkey_partsに入れる
  ToydbIndexKey key;
  for (uint idx : this->pk_column_indices) {
    key.key_parts.push_back(row[idx]);
  }
  return key;
}

ToydbTable::RowIterator ToydbTable::rows_begin() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return rows.begin();
}

ToydbTable::RowIterator ToydbTable::rows_end() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return rows.end();
}

ToydbTable::RowIterator ToydbTable::rows_last() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  auto iter = rows.end();
  --iter;
  return iter;
}

ToydbTable::RowIterator ToydbTable::find_row(const ToydbIndexKey &key) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return rows.find(key);
}

ToydbTable::RowIterator ToydbTable::lower_bound_row(
    const ToydbIndexKey &key) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return rows.lower_bound(key);
}

ToydbTable::RowIterator ToydbTable::upper_bound_row(
    const ToydbIndexKey &key) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return rows.upper_bound(key);
}

int ToydbTable::add_column(const std::string &name, enum_field_types type) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  if (!this->rows.empty()) {
    return HA_ERR_INTERNAL_ERROR;
  }

  if (!check_supported_type(type)) {
    return HA_ERR_UNSUPPORTED;
  }

  columns.push_back({name, type});
  return 0;
}

int ToydbTable::insert_row(std::vector<SupportedDBValue> row_data) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  if (row_data.size() != this->columns.size()) {
    return ER_WRONG_VALUE_COUNT;
  }

  for (size_t i = 0; i < this->columns.size(); ++i) {
    if (!check_type_match(this->columns[i].type, row_data[i])) {
      return ER_INCORRECT_TYPE;
    }
  }

  ToydbIndexKey key;
  if (!this->pk_column_indices.empty()) {
    // PRIMARY KEYのカラム値をClustered Indexのキーとして使用する
    key = build_key_from_row(row_data);
    if (this->rows.contains(key)) {
      return HA_ERR_FOUND_DUPP_KEY;
    }
  } else {
    // PKがない場合は内部のrow_idをキーとして使用する
    ToydbRowId id = this->next_row_id++;
    key = ToydbIndexKey{{static_cast<int64>(id)}};
  }

  ToydbRowId row_id = this->next_row_id++;
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
    if (!check_type_match(this->columns[i].type, row_data[i])) {
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
    if (!check_type_match(this->columns[i].type, row_data[i]))
      return ER_INCORRECT_TYPE;
  }

  // PKカラムの値が変わった場合はキーを差し替える
  if (!pk_column_indices.empty()) {
    ToydbIndexKey new_key = build_key_from_row(row_data);
    if (!(new_key == key)) {
      // 新しいキーが既に存在する場合は重複エラー
      if (this->rows.contains(new_key)) return HA_ERR_FOUND_DUPP_KEY;
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
  this->rows.clear();
}

void ToydbTable::print_all() const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  std::cout << "--- Table: " << this->table_name << " ---\n";

  for (const auto &col : this->columns) {
    std::cout << col.name << "\t| ";
  }
  std::cout << "\n----------------------------------\n";

  for (const auto &[key, row] : this->rows) {
    for (const auto &cell : row.values) {
      std::visit([](const auto &val) { std::cout << val << "\t| "; }, cell);
    }
    std::cout << '\n';
  }
}

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

#ifndef TOYDB_TABLE_H
#define TOYDB_TABLE_H

#include <sys/types.h>
#include <cstddef>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "field_types.h"
#include "my_inttypes.h"

/**
 * @brief サポートするデータ型
 *
 * Nullableは一旦サポート外
 */
using SupportedDBValue = std::variant<int64, std::string>;

/**
 * @brief 各行を一意に参照するためのId
 */
using ToydbRowId = uint16_t;

/**
 * @brief テーブル行
 */
struct ToydbRow {
  ToydbRowId id;
  std::vector<SupportedDBValue> values;
};

/**
 * @brief インデックスのキー
 */
struct ToydbIndexKey {
  // いつか複合キーをサポートできるようにvectorにしておく
  std::vector<SupportedDBValue> key_parts;
};

/**
 * @brief インデックスの比較関数
 *
 * std::mapでClustered Indexを実装する際の比較に利用する
 */
struct ToydbKeyLess {
  bool operator()(const ToydbIndexKey &lhs, const ToydbIndexKey &rhs) const {
    // ここでは単純にkey_partsの辞書順で比較する
    return lhs.key_parts < rhs.key_parts;
  }
};

/**
 * @brief テーブルのカラム
 */
struct ToydbColumn {
  std::string name;
  enum_field_types type;
};

/**
 * @brief テーブルの定義と実データ
 */
class ToydbTable final {
 private:
  std::string table_name;
  std::vector<ToydbColumn> columns;

  ToydbRowId next_row_id{1};

  // PRIMARY KEYを構成するカラムのインデックス番号
  // DDLでのカラムの定義順でインデックス番号が決まる
  std::vector<uint> pk_column_indices;

  std::map<ToydbIndexKey, ToydbRow, ToydbKeyLess> rows;

 public:
  using RowIterator =
      std::map<ToydbIndexKey, ToydbRow, ToydbKeyLess>::const_iterator;

  explicit ToydbTable(std::string name);

  void set_primary_key(std::vector<uint> indices);
  ToydbIndexKey build_key_from_row(
      const std::vector<SupportedDBValue> &row) const;

  int add_column(const std::string &name, enum_field_types type);
  int insert_row(std::vector<SupportedDBValue> row_data);
  int update_row(size_t row_index, std::vector<SupportedDBValue> row_data);
  int delete_row(size_t row_index);
  int update_row_by_key(const ToydbIndexKey &key,
                        std::vector<SupportedDBValue> row_data);
  int delete_row_by_key(const ToydbIndexKey &key);
  size_t row_count() const;
  const std::vector<SupportedDBValue> &get_row(size_t row_index) const;
  const std::vector<ToydbColumn> &get_columns() const;
  void clear_rows();
  void print_all() const;

  RowIterator rows_begin() const;
  RowIterator rows_end() const;
  RowIterator find_row(const ToydbIndexKey &key) const;
  RowIterator lower_bound_row(const ToydbIndexKey &key) const;
  RowIterator upper_bound_row(const ToydbIndexKey &key) const;
};

/**
 * @brief 全テーブルの定義と実データを管理する
 */
struct ToydbTables {
  std::map<std::string, ToydbTable> tables;
};

#endif  // TOYDB_TABLE_H

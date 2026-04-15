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

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "field_types.h"
#include "my_inttypes.h"

/**
 * @brief サポートするデータ型
 *
 * `std::monostate`はNULL値を表す
 */
using SupportedDBValue = std::variant<int64, std::string, std::monostate>;

/**
 * @brief 各行を一意に参照するためのId
 */
using ToydbRowId = uint64_t;

/**
 * @brief テーブル行
 */
struct ToydbRow {
  /// DDLでPKが指定されてない場合はこれをPKとする
  ToydbRowId id;
  std::vector<SupportedDBValue> values;
};

/**
 * @brief インデックスのキー
 */
struct ToydbIndexKey {
  // いつか複合キーをサポートできるようにvectorにしておく
  std::vector<SupportedDBValue> key_parts;

  bool operator<(const ToydbIndexKey &rhs) const {
    return key_parts < rhs.key_parts;
  }
  bool operator==(const ToydbIndexKey &rhs) const {
    return key_parts == rhs.key_parts;
  }
};

/**
 * @brief カラムの状態を表すフラグ
 *
 * 参考: mysql-server/storage/innobase/include/data0type.h
 *
 * enum classでビット演算をするためにオーバーロードしてる
 */
enum class ColumnFlag : uint16_t {
  NONE = 0,
  NOT_NULL = 256,
  UNSIGNED = 512,
};
constexpr ColumnFlag operator|(ColumnFlag a, ColumnFlag b) {
  return static_cast<ColumnFlag>(
      static_cast<uint16_t>(std::to_underlying(a) | std::to_underlying(b)));
}
constexpr ColumnFlag operator&(ColumnFlag a, ColumnFlag b) {
  return static_cast<ColumnFlag>(
      static_cast<uint16_t>(std::to_underlying(a) & std::to_underlying(b)));
}
constexpr ColumnFlag &operator|=(ColumnFlag &a, ColumnFlag b) {
  return a = a | b;
}
constexpr bool has_flag(ColumnFlag flags, ColumnFlag test) {
  return (flags & test) != ColumnFlag::NONE;
}

/**
 * @brief テーブルのカラム
 */
struct ToydbColumn {
  std::string name;
  enum_field_types type;

  ColumnFlag flags{ColumnFlag::NONE};
};

/**
 * @brief セカンダリインデックス
 *
 * InnoDBの実装をまねて、セカンダリインデックスのキーにPKカラムの値をサフィックスとしてくっつける
 * こうしないとnon-uniqueなセカンダリインデックスのキーが重複しちゃうため
 */
struct ToydbSecondaryIndex {
  std::string name;
  std::vector<uint> column_indices;  // ユーザー定義のインデックスカラム番号
  size_t user_key_parts_count;       // ユーザー定義のキーパート数
  bool is_unique;

  // セカンダリインデックスの実データ
  // sec_key + PK suffix
  absl::btree_set<ToydbIndexKey> entries;
};

/**
 * @brief テーブルの定義と実データ
 */
class ToydbTable final {
 private:
  std::string table_name;
  std::vector<ToydbColumn> columns;

  ToydbRowId next_row_id{1};

  ulonglong next_auto_inc_value{1};

  // PRIMARY KEYを構成するカラムのインデックス番号
  // DDLでのカラムの定義順でインデックス番号が決まる
  std::vector<uint> pk_column_indices;

  absl::btree_map<ToydbIndexKey, ToydbRow> rows;

  // MySQLインデックス番号とセカンダリインデックス定義の紐付け
  std::map<uint, ToydbSecondaryIndex> secondary_indexes;

 public:
  using RowIterator = absl::btree_map<ToydbIndexKey, ToydbRow>::const_iterator;
  using SecondaryIndexIterator = absl::btree_set<ToydbIndexKey>::const_iterator;

  explicit ToydbTable(std::string name);

  void set_primary_key(std::vector<uint> indices);
  ulonglong get_next_auto_inc_value() const;
  void set_auto_inc_value(ulonglong value);
  void update_auto_inc_value(ulonglong value);
  ToydbIndexKey build_pk_key_from_row(
      const std::vector<SupportedDBValue> &row) const;

  int add_column(const std::string &name, enum_field_types type,
                 ColumnFlag flags);
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
  RowIterator rows_last() const;
  RowIterator find_row(const ToydbIndexKey &key) const;
  RowIterator lower_bound_row(const ToydbIndexKey &key) const;
  RowIterator upper_bound_row(const ToydbIndexKey &key) const;

  // セカンダリインデックス操作
  void add_secondary_index(uint mysql_index_no, std::string name,
                           std::vector<uint> col_indices, bool is_unique);
  ToydbIndexKey build_secondary_key(
      uint mysql_index_no, const std::vector<SupportedDBValue> &row_data) const;
  ToydbIndexKey extract_pk_key_from_secondary(
      uint mysql_index_no, const ToydbIndexKey &sec_key) const;
  int insert_secondary_keys(const std::vector<SupportedDBValue> &row_data);
  int remove_secondary_keys(const std::vector<SupportedDBValue> &row_data);
  bool has_secondary_index(uint mysql_index_no) const;
  const ToydbSecondaryIndex &get_secondary_index(uint mysql_index_no) const;
};

/**
 * @brief セカンダリキーのPK部分以外がプレフィックスと一致するか判定
 *
 * UNIQUE判定時はPKを無視して判定したいため
 */
bool key_prefix_matches(const ToydbIndexKey &full_key,
                        const ToydbIndexKey &prefix);

/**
 * @brief 全テーブルの定義と実データを管理する
 */
struct ToydbTables {
  std::map<std::string, ToydbTable> tables;
};

#endif  // TOYDB_TABLE_H

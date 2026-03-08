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

#include "ha_toydb.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>

#include "field_types.h"
#include "my_base.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysql/components/services/bits/system_variables_bits.h"
#include "mysql/plugin.h"
#include "mysql/service_thd_alloc.h"
#include "mysql/status_var.h"
#include "mysqld_error.h"
#include "sql/derror.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/mysqld_cs.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_plugin.h"
#include "sql/table.h"
#include "thr_lock.h"
#include "typelib.h"

static bool check_type_match(enum_field_types expected,
                             const SupportedDBValue &value) {
  switch (expected) {
    case enum_field_types::MYSQL_TYPE_LONGLONG:
      return std::holds_alternative<int64>(value);
    case enum_field_types::MYSQL_TYPE_VAR_STRING:
      return std::holds_alternative<std::string>(value);
    default:
      return false;
  }
}

static bool check_supported_type(enum_field_types type) {
  return type == enum_field_types::MYSQL_TYPE_LONGLONG ||
         type == enum_field_types::MYSQL_TYPE_VAR_STRING;
}

ToydbTable::ToydbTable(std::string name) : table_name(std::move(name)) {}

int ToydbTable::add_column(const std::string &name, enum_field_types type) {
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
  if (row_data.size() != this->columns.size()) {
    return ER_WRONG_VALUE_COUNT;
  }

  for (size_t i = 0; i < this->columns.size(); ++i) {
    if (!check_type_match(this->columns[i].type, row_data[i])) {
      return ER_INCORRECT_TYPE;
    }
  }

  rows.push_back(row_data);
  return 0;
}

int ToydbTable::update_row(size_t row_index,
                           std::vector<SupportedDBValue> row_data) {
  if (row_index >= rows.size()) {
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

  rows[row_index] = std::move(row_data);
  return 0;
}

int ToydbTable::delete_row(size_t row_index) {
  if (row_index >= rows.size()) {
    return HA_ERR_KEY_NOT_FOUND;
  }
  rows.erase(rows.begin() + static_cast<ptrdiff_t>(row_index));
  return 0;
}

size_t ToydbTable::row_count() const { return rows.size(); }

const std::vector<SupportedDBValue> &ToydbTable::get_row(
    size_t row_index) const {
  return rows.at(row_index);
}

const std::vector<ToydbColumn> &ToydbTable::get_columns() const {
  return columns;
}

void ToydbTable::clear_rows() { rows.clear(); }

void ToydbTable::print_all() const {
  std::cout << "--- Table: " << this->table_name << " ---\n";

  for (const auto &col : this->columns) {
    std::cout << col.name << "\t| ";
  }
  std::cout << "\n----------------------------------\n";

  for (const auto &row : this->rows) {
    for (const auto &cell : row) {
      std::visit([](const auto &val) { std::cout << val << "\t| "; }, cell);
    }
    std::cout << '\n';
  }
}

// --- Storage Engine ---

static std::unique_ptr<ToydbTables> toydb_tables;

static handler *toydb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                     bool partitioned, MEM_ROOT *mem_root);

static handlerton *toydb_hton;

Toydb_share::Toydb_share() { thr_lock_init(&this->lock); }

/**
 * Storage Engineの初期化を行う
 */
static int toydb_init_func(void *p) {
  DBUG_TRACE;

  toydb_tables = std::make_unique<ToydbTables>();

  toydb_hton = static_cast<handlerton *>(p);
  toydb_hton->create = toydb_create_handler;
  toydb_hton->state = SHOW_OPTION_YES;
  toydb_hton->flags = HTON_CAN_RECREATE;
  // システムテーブルのサポートはしないので常にfalseを返す
  toydb_hton->is_supported_system_table = [](const char *, const char *,
                                             bool) -> bool { return false; };

  return 0;
}

/**
 * Storage Engineのdestructor
 *
 * 今回は特に処理はなし
 */
static int toydb_deinit_func(void *p [[maybe_unused]]) {
  DBUG_TRACE;

  assert(p);

  toydb_tables.reset();

  return 0;
}

Toydb_share *ha_toydb::get_share() {
  Toydb_share *tmp_share = nullptr;

  DBUG_TRACE;

  SharedHaDataLock lock(this);

  tmp_share = dynamic_cast<Toydb_share *>(this->get_ha_share_ptr());
  if (tmp_share == nullptr) {
    // ここはhandlerクラス側がポインタを前提にしている
    tmp_share = new Toydb_share;
    this->set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }

  return tmp_share;
}

static handler *toydb_create_handler(handlerton *hton, TABLE_SHARE *table, bool,
                                     MEM_ROOT *mem_root) {
  return new (mem_root) ha_toydb(hton, table);
}

ha_toydb::ha_toydb(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {
  // ref_lengthはrefに保存する値のサイズを指定するためのメンバ
  // handler::refには行インデックスを保存するためsize_tのサイズを入れる
  this->ref_length = sizeof(size_t);
}

int ha_toydb::open(const char *, int, uint, const dd::Table *) {
  DBUG_TRACE;

  this->share = this->get_share();
  if (this->share == nullptr) return HA_ERR_INTERNAL_ERROR;
  thr_lock_data_init(&this->share->lock, &this->lock, nullptr);

  return 0;
}

int ha_toydb::close(void) {
  DBUG_TRACE;
  return 0;
}

/**
 * @brief テーブルへの行の挿入
 */
int ha_toydb::write_row(uchar *) {
  DBUG_TRACE;

  auto toydb_table = toydb_tables->tables.find(this->table->s->table_name.str);
  if (toydb_table == toydb_tables->tables.end()) {
    DBUG_PRINT("error",
               ("Table '%s' not found", this->table->s->table_name.str));
    return HA_ERR_INTERNAL_ERROR;
  }

  // handler::table::fieldからデータの読み取り（val_int()/val_str()）を呼ぶ前にread_setをセットする必要がある
  // read_setはビットマップで、どのフィールドを読み取るかを指定するためのもの
  // ただ今回は固定で全てのフィールドの読み書きを行うことにするので、全てのビットを立てる
  // dbug_tmp_use_all_columnsはデバッグビルドでのみ有効で、元のビットマップを保存して復元できる
  my_bitmap_map *old_map =
      dbug_tmp_use_all_columns(this->table, this->table->read_set);

  std::vector<SupportedDBValue> row_data;
  int ret = 0;

  for (Field **field = this->table->field; *field != nullptr; field++) {
    if ((*field)->is_null()) {
      my_error(ER_BAD_NULL_ERROR, MYF(0), (*field)->field_name);
      ret = ER_BAD_NULL_ERROR;
      break;
    }

    switch ((*field)->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG: {
        // MySQLの整数型は全てint64_tで扱うことにする
        int64 val = (*field)->val_int();
        row_data.emplace_back(val);
        break;
      }
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING: {
        String val_buf;
        String *val = (*field)->val_str(&val_buf);
        row_data.emplace_back(std::string(val->ptr(), val->length()));
        break;
      }
      default:
        DBUG_PRINT("error", ("Unsupported field type: %d", (*field)->type()));
        ret = HA_ERR_UNSUPPORTED;
        break;
    }
    if (ret != 0) break;
  }

  if (ret == 0) {
    ret = toydb_table->second.insert_row(std::move(row_data));
  }

  dbug_tmp_restore_column_map(this->table->read_set, old_map);
  return ret;
}

/**
 * @brief テーブルの行の更新
 *
 * 引数には更新前と更新後のデータ両方が渡される
 */
int ha_toydb::update_row(const uchar *, uchar *) {
  DBUG_TRACE;

  auto toydb_table = toydb_tables->tables.find(this->table->s->table_name.str);
  if (toydb_table == toydb_tables->tables.end()) {
    return HA_ERR_INTERNAL_ERROR;
  }

  my_bitmap_map *old_map =
      dbug_tmp_use_all_columns(this->table, this->table->read_set);

  std::vector<SupportedDBValue> row_data;
  int ret = 0;

  for (Field **field = this->table->field; *field != nullptr; field++) {
    if ((*field)->is_null()) {
      my_error(ER_BAD_NULL_ERROR, MYF(0), (*field)->field_name);
      ret = ER_BAD_NULL_ERROR;
      break;
    }

    switch ((*field)->type()) {
      case MYSQL_TYPE_LONGLONG: {
        int64 val = (*field)->val_int();
        row_data.emplace_back(val);
        break;
      }
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING: {
        String val_buf;
        String *val = (*field)->val_str(&val_buf);
        row_data.emplace_back(std::string(val->ptr(), val->length()));
        break;
      }
      default:
        ret = HA_ERR_UNSUPPORTED;
        break;
    }
    if (ret != 0) break;
  }

  // 実際に更新する行は直前にrnd_nextで返した行なので、`scan_index-1`の行を更新する
  if (ret == 0) {
    ret = toydb_table->second.update_row(this->scan_index - 1,
                                         std::move(row_data));
  }

  dbug_tmp_restore_column_map(this->table->read_set, old_map);
  return ret;
}

/**
 * @brief テーブルの行の削除
 */
int ha_toydb::delete_row(const uchar *) {
  DBUG_TRACE;

  auto toydb_table = toydb_tables->tables.find(this->table->s->table_name.str);
  if (toydb_table == toydb_tables->tables.end()) {
    return HA_ERR_INTERNAL_ERROR;
  }

  // update_rowと同様に、削除する行は直前にrnd_nextで返した行なので、`scan_index-1`の行を削除する
  return toydb_table->second.delete_row(this->scan_index - 1);
}

int ha_toydb::index_read_map(uchar *, const uchar *, key_part_map,
                             enum ha_rkey_function) {
  int rc = 0;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

int ha_toydb::index_next(uchar *) {
  int rc = 0;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

int ha_toydb::index_prev(uchar *) {
  int rc = 0;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

int ha_toydb::index_first(uchar *) {
  int rc = 0;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

int ha_toydb::index_last(uchar *) {
  int rc = 0;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
 * @brief ToydbTableの行データをMySQLのレコードバッファに書き込む
 */
static int store_row_to_buf(TABLE *table, uchar *buf,
                            const std::vector<SupportedDBValue> &row) {
  // field->store()内でASSERT_COLUMN_MARKED_FOR_WRITEが呼ばれるため、
  // write_setに全カラムのビットを立てる必要がある
  my_bitmap_map *old_map =
      dbug_tmp_use_all_columns(table, table->write_set);

  memset(buf, 0, table->s->null_bytes);

  for (size_t i = 0; i < row.size(); i++) {
    Field *field = table->field[i];
    std::visit(
        [field](const auto &val) {
          using T = std::decay_t<decltype(val)>;
          // *bufとField::ptrは同じポインタなので、field->storeでbufに直接書き込むことができる
          if constexpr (std::is_same_v<T, int64>) {
            field->store(val, false);
          } else if constexpr (std::is_same_v<T, std::string>) {
            field->store(val.c_str(), val.length(), system_charset_info);
          }
        },
        row[i]);
  }

  dbug_tmp_restore_column_map(table->write_set, old_map);
  return 0;
}

/**
 * @brief テーブルスキャン操作の初期化を行う
 */
int ha_toydb::rnd_init(bool) {
  DBUG_TRACE;
  this->scan_index = 0;
  return 0;
}

int ha_toydb::rnd_end() {
  DBUG_TRACE;
  return 0;
}

/**
 * @brief テーブルスキャン操作で次の行を取得する
 */
int ha_toydb::rnd_next(uchar *buf) {
  DBUG_TRACE;

  auto toydb_table = toydb_tables->tables.find(this->table->s->table_name.str);
  if (toydb_table == toydb_tables->tables.end()) return HA_ERR_INTERNAL_ERROR;

  if (this->scan_index >= toydb_table->second.row_count())
    return HA_ERR_END_OF_FILE;

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);

  const auto &row = toydb_table->second.get_row(this->scan_index);
  this->scan_index++;
  return store_row_to_buf(this->table, buf, row);
}

/**
 * @brief テーブルスキャン操作の現在位置を保存する
 *
 * handler::refに現在の行位置を保存して、サーバー層がrnd_posで任意位置から読み出せるようにする
 */
void ha_toydb::position(const uchar *) {
  DBUG_TRACE;
  my_store_ptr(this->ref, this->ref_length, this->scan_index);
}

int ha_toydb::rnd_pos(uchar *buf, uchar *pos) {
  DBUG_TRACE;

  // refから行位置を復元する
  size_t row_index = my_get_ptr(pos, this->ref_length);

  auto toydb_table = toydb_tables->tables.find(this->table->s->table_name.str);
  if (toydb_table == toydb_tables->tables.end()) return HA_ERR_INTERNAL_ERROR;

  if (row_index >= toydb_table->second.row_count()) return HA_ERR_KEY_NOT_FOUND;

  this->scan_index = row_index;
  const auto &row = toydb_table->second.get_row(row_index);
  return store_row_to_buf(this->table, buf, row);
}

/**
 * @brief optimizerへテーブルの統計情報を提供する
 */
int ha_toydb::info(uint) {
  DBUG_TRACE;

  auto toydb_table = toydb_tables->tables.find(this->table->s->table_name.str);
  if (toydb_table != toydb_tables->tables.end()) {
    // 一旦行数だけ追加する
    this->stats.records = toydb_table->second.row_count();
  }
  return 0;
}

/**
 * @brief mysql側からSEに対してヒントを渡すためのメソッド
 *
 * 今回は特になし
 */
int ha_toydb::extra(enum ha_extra_function) {
  DBUG_TRACE;
  return 0;
}

int ha_toydb::delete_all_rows() {
  DBUG_TRACE;

  auto toydb_table = toydb_tables->tables.find(this->table->s->table_name.str);
  if (toydb_table == toydb_tables->tables.end()) return HA_ERR_INTERNAL_ERROR;

  toydb_table->second.clear_rows();
  return 0;
}

int ha_toydb::external_lock(THD *, int) {
  DBUG_TRACE;
  return 0;
}

THR_LOCK_DATA **ha_toydb::store_lock(THD *, THR_LOCK_DATA **to,
                                     enum thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && this->lock.type == TL_UNLOCK)
    this->lock.type = lock_type;
  *to++ = &this->lock;
  return to;
}

int ha_toydb::delete_table(const char *, const dd::Table *) {
  DBUG_TRACE;
  return 0;
}

/**
 * @brief テーブルのリネーム
 *
 * 今回はサポートなし
 */
int ha_toydb::rename_table(const char *, const char *, const dd::Table *,
                           dd::Table *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
 * @brief 渡されたレンジ内のテーブルの行数を返す
 *
 * 今回は適当な行数を返す
 */
ha_rows ha_toydb::records_in_range(uint, key_range *, key_range *) {
  DBUG_TRACE;
  return 10;
}

static MYSQL_THDVAR_STR(last_create_thdvar, PLUGIN_VAR_MEMALLOC, nullptr,
                        nullptr, nullptr, nullptr);

static MYSQL_THDVAR_UINT(create_count_thdvar, 0, nullptr, nullptr, nullptr, 0,
                         0, 1000, 0);

int ha_toydb::create(const char *name, TABLE *table_info, HA_CREATE_INFO *,
                     dd::Table *) {
  DBUG_TRACE;

  const char *table_name = table_info->s->table_name.str;

  if (toydb_tables->tables.contains(table_name)) {
    return HA_ERR_TABLE_EXIST;
  }

  ToydbTable new_table(table_name);

  for (Field **f = table_info->field; *f != nullptr; f++) {
    DBUG_PRINT("field",
               ("Field: name=%s, type=%d", (*f)->field_name, (*f)->type()));
    new_table.add_column((*f)->field_name, (*f)->type());
  }

  toydb_tables->tables.emplace(table_name, std::move(new_table));

  return 0;
}

static struct st_mysql_storage_engine toydb_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(toydb){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &toydb_storage_engine,
    "TOYDB",
    "sakurai-ryo",
    "Toydb storage engine",
    PLUGIN_LICENSE_GPL,
    toydb_init_func,   /* Plugin Init */
    nullptr,           /* Plugin check uninstall */
    toydb_deinit_func, /* Plugin Deinit */
    0x0001 /* 0.1 */,
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    0,       /* flags */
} mysql_declare_plugin_end;

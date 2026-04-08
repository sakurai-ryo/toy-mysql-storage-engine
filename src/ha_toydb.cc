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

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
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
#include "mysql/plugin.h"
#include "mysql/service_thd_alloc.h"
#include "mysql/status_var.h"
#include "mysql_com.h"
#include "mysqld_error.h"
#include "sql/derror.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/key.h"
#include "sql/mysqld_cs.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_plugin.h"
#include "sql/table.h"
#include "thr_lock.h"
#include "typelib.h"

static std::unique_ptr<ToydbTables> toydb_tables;

static handler *toydb_create_handler(handlerton *hton, TABLE_SHARE *table, bool,
                                     MEM_ROOT *mem_root) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return new (mem_root) ha_toydb(hton, table);
}

Toydb_share::Toydb_share() : data_mutex(std::make_unique<std::mutex>()) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  thr_lock_init(&this->lock);
}

/**
 * @brief Storage Engineの初期化を行う
 */
static int toydb_init_func(void *p) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  toydb_tables = std::make_unique<ToydbTables>();

  auto *hton = static_cast<handlerton *>(p);
  hton->create = toydb_create_handler;
  hton->state = SHOW_OPTION_YES;
  // Truncate時はhandler::truncateではなく、delete_table=>createの流れにするためにテーブルの再作成を許可する
  hton->flags = HTON_CAN_RECREATE;
  // システムテーブルのサポートはしないので常にfalseを返す
  hton->is_supported_system_table = [](const char *, const char *,
                                       bool) -> bool { return false; };

  return 0;
}

/**
 * @brief Storage Engineのdestructor
 */
static int toydb_deinit_func(void *p [[maybe_unused]]) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  assert(p);

  toydb_tables.reset();

  return 0;
}

Toydb_share *ha_toydb::get_share() {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  Toydb_share *tmp_share = nullptr;

  SharedHaDataLock lock(this);

  tmp_share = dynamic_cast<Toydb_share *>(this->get_ha_share_ptr());
  if (tmp_share == nullptr) {
    // ここはhandlerクラス側がポインタを前提にしている
    tmp_share = new Toydb_share;
    this->set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }

  // テーブルデータへのポインタをキャッシュする
  if (tmp_share->toydb_table == nullptr) {
    auto it = toydb_tables->tables.find(this->table->s->table_name.str);
    if (it != toydb_tables->tables.end()) {
      tmp_share->toydb_table = &it->second;
    }
  }

  return tmp_share;
}

ha_toydb::ha_toydb(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  // ref_lengthはrefに保存する値のサイズを指定するためのメンバ
  // handler::refには行インデックスを保存するためsize_tのサイズを入れる
  this->ref_length = sizeof(size_t);
}

int ha_toydb::open(const char *, int, uint, const dd::Table *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  this->share = this->get_share();
  if (this->share == nullptr) return HA_ERR_INTERNAL_ERROR;
  if (this->share->toydb_table == nullptr) return HA_ERR_INTERNAL_ERROR;
  thr_lock_data_init(&this->share->lock, &this->lock, nullptr);

  return 0;
}

/**
 * @brief 処理終了時にテーブルを閉じる
 *
 * テーブル削除前に該当テーブルを開いているhandlerがある場合はこのcloseが呼ばれるケースもある
 *
 * 上記は`handler::delete_table`のコメントに記載している
 */
int ha_toydb::close(void) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  // テーブル削除前に該当テーブルのhandlerは全てcloseされるので、複数handlerがいても安全
  if (this->share != nullptr) {
    this->share->toydb_table = nullptr;
  }

  return 0;
}

/**
 * @brief handler::table::fieldからrow_dataへデータを読み出す
 */
int ha_toydb::read_row_from_fields(std::vector<SupportedDBValue> &row_data) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  // handler::table::fieldからデータの読み取り（val_int()/val_str()）を呼ぶ前にread_setをセットする必要がある
  // read_setはビットマップで、どのフィールドを読み取るかを指定するためのもの
  // ただ今回は固定で全てのフィールドの読み書きを行うことにするので、全てのビットを立てる
  // dbug_tmp_use_all_columnsはデバッグビルドでのみ有効で、元のビットマップを保存して復元できる
  my_bitmap_map *old_map =
      dbug_tmp_use_all_columns(this->table, this->table->read_set);

  int ret = 0;
  row_data.reserve(this->table->s->fields);

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

  dbug_tmp_restore_column_map(this->table->read_set, old_map);
  return ret;
}

/**
 * @brief テーブルへの行の挿入
 */
int ha_toydb::write_row(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  // AUTO_INCREMENTカラムがある場合は値の採番を行う
  // table->record[0]: 書き込まれた新しい行
  // table->record[1]: 更新前の行（INSERTの場合はnullptr）
  if (this->table->next_number_field != nullptr &&
      buf == this->table->record[0]) {
    int err = this->update_auto_increment();
    if (err != 0) return err;
  }

  std::vector<SupportedDBValue> row_data;
  int ret = this->read_row_from_fields(row_data);

  if (ret == 0) {
    ret = this->share->toydb_table->insert_row(std::move(row_data));

    // 挿入成功時にAUTO_INCREMENTカウンタを更新する
    if (ret == 0 && this->table->next_number_field != nullptr) {
      ulonglong auto_inc_val =
          static_cast<ulonglong>(this->table->next_number_field->val_int());
      this->share->toydb_table->update_auto_inc_value(auto_inc_val);
    }
  }

  return ret;
}

/**
 * @brief テーブルの行の更新
 *
 * 引数には更新前と更新後のデータ両方が渡される
 */
int ha_toydb::update_row(const uchar *, uchar *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::vector<SupportedDBValue> row_data;
  int ret = this->read_row_from_fields(row_data);

  if (ret == 0) {
    std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
    if (this->index_cursor.positioned &&
        this->index_cursor.current_index_key.has_value()) {
      // インデックススキャン中はキーベースで更新する
      ret = this->share->toydb_table->update_row_by_key(
          *this->index_cursor.current_index_key, std::move(row_data));
    } else {
      // テーブルスキャン中は位置ベースで更新する
      ret = this->share->toydb_table->update_row(
          this->scan_cursor.current_pos - 1, std::move(row_data));
    }
  }

  return ret;
}

/**
 * @brief テーブルの行の削除
 */
int ha_toydb::delete_row(const uchar *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  if (this->index_cursor.positioned &&
      this->index_cursor.current_index_key.has_value()) {
    // インデックススキャン中はキーベースで削除する
    return this->share->toydb_table->delete_row_by_key(
        *this->index_cursor.current_index_key);
  }

  // テーブルスキャン中は位置ベースで削除する
  return this->share->toydb_table->delete_row(this->scan_cursor.current_pos -
                                              1);
}

/**
 * @brief インデックススキャンの初期化
 *
 * idxは利用するインデックスの番号が入る
 */
int ha_toydb::index_init(uint idx, bool) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  this->index_cursor = ToydbIndexCursor{};
  this->index_cursor.mysql_index_no = idx;
  this->scan_cursor.positioned = false;
  this->active_index = idx;
  return 0;
}

/**
 * @brief インデックススキャンの終了
 */
int ha_toydb::index_end() {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  this->index_cursor = ToydbIndexCursor{};
  this->active_index = MAX_KEY;
  return 0;
}

/**
 * @brief MySQLのキーバッファからToydbIndexKeyへデコードする
 *
 * key_restore()でレコードバッファに復元し、Fieldからval_int/val_strで読み取る
 */
int ha_toydb::decode_index_key(const uchar *key, uint key_len,
                               ToydbIndexKey &out_key) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  // 利用するインデックスの情報
  KEY *key_info = &this->table->key_info[this->index_cursor.mysql_index_no];

  key_restore(this->table->record[0], key, key_info, key_len);

  my_bitmap_map *old_map =
      dbug_tmp_use_all_columns(this->table, this->table->read_set);

  int ret = 0;
  for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
    Field *field = key_info->key_part[i].field;

    if (field->is_null()) {
      ret = HA_ERR_UNSUPPORTED;
      break;
    }

    switch (field->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
        out_key.key_parts.emplace_back(field->val_int());
        break;
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING: {
        String buf;
        String *val = field->val_str(&buf);
        out_key.key_parts.emplace_back(std::string(val->ptr(), val->length()));
        break;
      }
      default:
        ret = HA_ERR_UNSUPPORTED;
        break;
    }
    if (ret != 0) break;
  }

  dbug_tmp_restore_column_map(this->table->read_set, old_map);
  return ret;
}

/**
 * @brief ToydbTableの行データをMySQLのレコードバッファに書き込む
 */
static int store_row_to_buf(TABLE *table, uchar *buf,
                            const std::vector<SupportedDBValue> &row) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  // field->store()内でASSERT_COLUMN_MARKED_FOR_WRITEが呼ばれるため、
  // write_setに全カラムのビットを立てる必要がある
  my_bitmap_map *old_map = dbug_tmp_use_all_columns(table, table->write_set);

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
 * @brief Clustered Indexを利用してキーに一致する行を検索する
 */
static const char *ha_rkey_function_name(enum ha_rkey_function flag) {
  switch (flag) {
    case HA_READ_KEY_EXACT:
      return "HA_READ_KEY_EXACT";
    case HA_READ_KEY_OR_NEXT:
      return "HA_READ_KEY_OR_NEXT";
    case HA_READ_KEY_OR_PREV:
      return "HA_READ_KEY_OR_PREV";
    case HA_READ_AFTER_KEY:
      return "HA_READ_AFTER_KEY";
    case HA_READ_BEFORE_KEY:
      return "HA_READ_BEFORE_KEY";
    case HA_READ_PREFIX:
      return "HA_READ_PREFIX";
    case HA_READ_PREFIX_LAST:
      return "HA_READ_PREFIX_LAST";
    case HA_READ_PREFIX_LAST_OR_PREV:
      return "HA_READ_PREFIX_LAST_OR_PREV";
    case HA_READ_MBR_CONTAIN:
      return "HA_READ_MBR_CONTAIN";
    case HA_READ_MBR_INTERSECT:
      return "HA_READ_MBR_INTERSECT";
    case HA_READ_MBR_WITHIN:
      return "HA_READ_MBR_WITHIN";
    case HA_READ_MBR_DISJOINT:
      return "HA_READ_MBR_DISJOINT";
    case HA_READ_MBR_EQUAL:
      return "HA_READ_MBR_EQUAL";
    case HA_READ_NEAREST_NEIGHBOR:
      return "HA_READ_NEAREST_NEIGHBOR";
    default:
      return "HA_READ_INVALID";
  }
}

int ha_toydb::index_read(uchar *buf, const uchar *key, uint key_len,
                         enum ha_rkey_function find_flag) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb",
             ("%s find_flag=%s", __func__, ha_rkey_function_name(find_flag)));

  ToydbIndexKey search_key;
  int ret = decode_index_key(key, key_len, search_key);
  if (ret != 0) return ret;

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  auto *toydb_table = this->share->toydb_table;

  ToydbTable::RowIterator iter;

  // search_keyをもとにClustered Indexを検索する
  switch (find_flag) {
    // 完全一致する最初の行を検索
    case HA_READ_KEY_EXACT:
      iter = toydb_table->find_row(search_key);
      if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;
      break;

    // search_key以上の最初の行を検索
    case HA_READ_KEY_OR_NEXT:
      // 以上なのでそのままlower_boundでOK
      iter = toydb_table->lower_bound_row(search_key);
      if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;
      break;

    // search_keyより大きい最初の行を検索
    case HA_READ_AFTER_KEY:
      iter = toydb_table->upper_bound_row(search_key);
      if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;
      break;

    // search_key以下の最後の行を検索
    case HA_READ_KEY_OR_PREV:
      iter = toydb_table->upper_bound_row(search_key);
      if (iter == toydb_table->rows_begin()) return HA_ERR_KEY_NOT_FOUND;
      --iter;
      break;

    // search_keyより小さい最後の行を検索
    case HA_READ_BEFORE_KEY:
      iter = toydb_table->lower_bound_row(search_key);
      if (iter == toydb_table->rows_begin()) return HA_ERR_KEY_NOT_FOUND;
      --iter;
      break;

    default:
      return HA_ERR_UNSUPPORTED;
  }

  this->index_cursor.current_index_key = iter->first;
  this->index_cursor.positioned = true;

  ha_statistic_increment(&System_status_var::ha_read_key_count);
  return store_row_to_buf(this->table, buf, iter->second.values);
}

/**
 * @brief Clustered Index上で次の行を取得する
 */
int ha_toydb::index_next(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  if (!this->index_cursor.positioned ||
      !this->index_cursor.current_index_key.has_value())
    return HA_ERR_END_OF_FILE;

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  auto *toydb_table = this->share->toydb_table;

  // 現在のキーからイテレータを復元して次に進む
  auto iter = toydb_table->find_row(*this->index_cursor.current_index_key);
  if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;

  ++iter;
  if (iter == toydb_table->rows_end()) return HA_ERR_END_OF_FILE;

  this->index_cursor.current_index_key = iter->first;

  ha_statistic_increment(&System_status_var::ha_read_next_count);
  return store_row_to_buf(this->table, buf, iter->second.values);
}

/**
 * @brief Clustered Index上で前の行を取得する
 */
int ha_toydb::index_prev(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  if (!this->index_cursor.positioned ||
      !this->index_cursor.current_index_key.has_value())
    return HA_ERR_END_OF_FILE;

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  auto *toydb_table = this->share->toydb_table;

  auto iter = toydb_table->find_row(*this->index_cursor.current_index_key);
  if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;

  if (iter == toydb_table->rows_begin()) return HA_ERR_END_OF_FILE;

  --iter;
  this->index_cursor.current_index_key = iter->first;

  ha_statistic_increment(&System_status_var::ha_read_prev_count);
  return store_row_to_buf(this->table, buf, iter->second.values);
}

/**
 * @brief Clustered Indexの先頭行を取得する
 */
int ha_toydb::index_first(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  auto *toydb_table = this->share->toydb_table;

  auto iter = toydb_table->rows_begin();
  if (iter == toydb_table->rows_end()) return HA_ERR_END_OF_FILE;

  this->index_cursor.current_index_key = iter->first;
  this->index_cursor.positioned = true;

  ha_statistic_increment(&System_status_var::ha_read_first_count);
  return store_row_to_buf(this->table, buf, iter->second.values);
}

/**
 * @brief Clustered Indexの末尾行を取得する
 */
int ha_toydb::index_last(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  auto *toydb_table = this->share->toydb_table;

  if (toydb_table->row_count() == 0) return HA_ERR_END_OF_FILE;

  auto iter = toydb_table->rows_last();

  this->index_cursor.current_index_key = iter->first;
  this->index_cursor.positioned = true;

  ha_statistic_increment(&System_status_var::ha_read_last_count);
  return store_row_to_buf(this->table, buf, iter->second.values);
}

/**
 * @brief テーブルスキャン操作の初期化を行う
 */
int ha_toydb::rnd_init(bool) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  this->scan_cursor = ToydbScanCursor{};
  this->index_cursor.positioned = false;
  return 0;
}

int ha_toydb::rnd_end() {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return 0;
}

/**
 * @brief テーブルスキャン操作で次の行を取得する
 */
int ha_toydb::rnd_next(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  auto *toydb_table = this->share->toydb_table;

  if (this->scan_cursor.current_pos >= toydb_table->row_count())
    return HA_ERR_END_OF_FILE;

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);

  const auto &row = toydb_table->get_row(this->scan_cursor.current_pos);
  this->scan_cursor.current_pos++;
  this->scan_cursor.positioned = true;
  return store_row_to_buf(this->table, buf, row);
}

/**
 * @brief テーブルスキャン操作の現在位置を保存する
 *
 * handler::refに現在の行位置を保存して、サーバー層がrnd_posで任意位置から読み出せるようにする
 */
void ha_toydb::position(const uchar *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  my_store_ptr(this->ref, this->ref_length, this->scan_cursor.current_pos);
}

int ha_toydb::rnd_pos(uchar *buf, uchar *pos) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  // refから行位置を復元する
  size_t row_index = my_get_ptr(pos, this->ref_length);

  auto *toydb_table = this->share->toydb_table;

  if (row_index >= toydb_table->row_count()) return HA_ERR_KEY_NOT_FOUND;

  // rnd_nextと同じ規約に合わせて+1する
  // update_row/delete_rowが current_pos - 1 で読み取った行を参照するため
  this->scan_cursor.current_pos = row_index + 1;
  this->scan_cursor.positioned = true;
  const auto &row = toydb_table->get_row(row_index);
  return store_row_to_buf(this->table, buf, row);
}

/**
 * @brief optimizerへテーブルの統計情報を提供する
 */
int ha_toydb::info(uint flag) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  this->stats.records = this->share->toydb_table->row_count();

  // InnoDBがhandler::info内でauto_increment_valueを更新しているのをパクってる
  if ((flag & HA_STATUS_AUTO) != 0) {
    DBUG_PRINT("toydb", ("Updating auto_increment_value in stats: %llu",
                         this->share->toydb_table->get_next_auto_inc_value()));
    this->stats.auto_increment_value =
        this->share->toydb_table->get_next_auto_inc_value();
  }
  return 0;
}

/**
 * @brief mysql側からSEに対してヒントを渡すためのメソッド
 *
 * 今回は特になし
 */
int ha_toydb::extra(enum ha_extra_function) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return 0;
}

int ha_toydb::delete_all_rows() {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  this->share->toydb_table->clear_rows();
  return 0;
}

/**
 * @brief
 * SQLステートメント開始時と終了時に行うべきSE内部リソースのロックとアンロックを行う
 *
 * 今回はデータは全てインメモリなので特になし
 */
int ha_toydb::external_lock(THD *, int) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return 0;
}

/**
 * @brief SQLステートメント開始前に必要なロック情報をMySQLに提供する
 */
THR_LOCK_DATA **ha_toydb::store_lock(THD *, THR_LOCK_DATA **to,
                                     enum thr_lock_type lock_type) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  // 今回は特にロックは実装しないが、ロックの種類が指定された場合はlock構造体のtypeにセットしてMySQLに返す
  if (lock_type != TL_IGNORE && this->lock.type == TL_UNLOCK)
    this->lock.type = lock_type;
  *to++ = &this->lock;
  return to;
}

/**
 * @brief テーブルの削除
 *
 * テーブル削除前にcloseされる可能性もあるので、Toydb_shareには依存しないようにする
 */
int ha_toydb::delete_table(const char *name, const dd::Table *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  // nameはパス形式（例:"./db/table"）なので、最後の'/'以降をテーブル名として取得する
  std::string_view path(name);
  auto pos = path.find_last_of('/');
  std::string_view table_name =
      (pos != std::string_view::npos) ? path.substr(pos + 1) : path;

  toydb_tables->tables.erase(std::string(table_name));
  return 0;
}

/**
 * @brief テーブルのリネーム
 *
 * 今回はサポートなし
 */
int ha_toydb::rename_table(const char *, const char *, const dd::Table *,
                           dd::Table *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));
  return HA_ERR_WRONG_COMMAND;
}

/**
 * @brief 渡されたキーレンジ内のテーブルの行数を返す
 */
ha_rows ha_toydb::records_in_range(uint index_num, key_range *min_key,
                                   key_range *max_key) {
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  active_index = index_num;
  if (active_index != 0) {
    DBUG_PRINT("error", ("Unsupported index_num: %u", index_num));
    return HA_POS_ERROR;
  }

  ToydbIndexKey min_index_key;
  if (decode_index_key(min_key->key, min_key->length, min_index_key) != 0) {
    return HA_POS_ERROR;
  }

  ToydbIndexKey max_index_key;
  if (decode_index_key(max_key->key, max_key->length, max_index_key) != 0) {
    return HA_POS_ERROR;
  }

  ToydbTable *table = this->share->toydb_table;

  auto start = min_key->flag == HA_READ_AFTER_KEY
                   ? table->upper_bound_row(min_index_key)
                   : table->lower_bound_row(min_index_key);

  auto end = max_key->flag == HA_READ_BEFORE_KEY
                 ? table->lower_bound_row(max_index_key)
                 : table->upper_bound_row(max_index_key);

  ha_rows n_rows = std::max(0L, std::distance(start, end));
  return n_rows > 0 ? n_rows : 1;
}

int ha_toydb::create(const char *, TABLE *table_info,
                     HA_CREATE_INFO *create_info, dd::Table *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  const char *table_name = table_info->s->table_name.str;

  // HTON_CAN_RECREATEにより、TRUNCATE時はdelete_tableを経由せず直接create()が呼ばれる
  // なのでその場合は既存テーブルを削除して再作成する
  if (toydb_tables->tables.contains(table_name)) {
    toydb_tables->tables.erase(table_name);
  }

  ToydbTable new_table(table_name);

  int col_index = 0;
  for (Field **f = table_info->field; *f != nullptr; f++, col_index++) {
    DBUG_PRINT("field",
               ("Field: name=%s, type=%d", (*f)->field_name, (*f)->type()));
    int ret = new_table.add_column((*f)->field_name, (*f)->type());
    if (ret != 0) return ret;

    if ((*f)->is_flag_set(AUTO_INCREMENT_FLAG)) {
      // AUTO_INCREMENTなカラムのインデックス番号を記録しておく
      new_table.set_auto_inc_column(col_index);
    }
  }

  // PRIMARY KEYが定義されている場合、そのカラムインデックスを設定する
  if (table_info->s->keys > 0 && table_info->s->primary_key != MAX_KEY) {
    KEY *pk = &table_info->key_info[table_info->s->primary_key];
    std::vector<uint> pk_indices;
    pk_indices.reserve(pk->user_defined_key_parts);
    for (uint i = 0; i < pk->user_defined_key_parts; i++) {
      // fieldnrは1始まりなので-1する
      pk_indices.push_back(pk->key_part[i].fieldnr - 1);
    }
    new_table.set_primary_key(std::move(pk_indices));
  }

  // CREATE TABLE ... AUTO_INCREMENT=N で初期値が指定された場合に反映する
  if (create_info->auto_increment_value > 0) {
    // データ挿入時に+1するので、ここでは-1しとく
    new_table.update_auto_inc_value(create_info->auto_increment_value - 1);
  }

  toydb_tables->tables.emplace(table_name, std::move(new_table));

  return 0;
}

/**
 * @brief AUTO_INCREMENTの値を取得する
 */
void ha_toydb::get_auto_increment(ulonglong offset, ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong *first_value,
                                  ulonglong *nb_reserved_values) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  // 呼び出し元のwrite_row()で既にdata_mutexを取得済みなのでここではロックしない
  *first_value = this->share->toydb_table->get_next_auto_inc_value();
  *nb_reserved_values = ULLONG_MAX;
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

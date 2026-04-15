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
#include "toydb_table.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
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
}

bool ha_toydb::is_primary_key_index(uint idx) const {
  return idx == this->table->s->primary_key;
}

ToydbIndexKey ha_toydb::resolve_pk_key_from_cursor() const {
  // セカンダリインデックススキャン中はセカンダリキーからPK部分を抽出する
  // テーブルスキャン(active_index=MAX_KEY)やPKスキャン中はカーソルのキーがそのままPK
  if (this->active_index != MAX_KEY &&
      !is_primary_key_index(this->active_index)) {
    return this->share->toydb_table->extract_pk_key_from_secondary(
        this->active_index, *this->cursor.current_key);
  }
  return *this->cursor.current_key;
}

int ha_toydb::open(const char *, int, uint, const dd::Table *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  this->share = this->get_share();
  if (this->share == nullptr) return HA_ERR_INTERNAL_ERROR;
  if (this->share->toydb_table == nullptr) return HA_ERR_INTERNAL_ERROR;
  thr_lock_data_init(&this->share->lock, &this->lock, nullptr);

  // ref_lengthはrefに保存する値のサイズを指定するためのメンバ
  // key_copy()でPKキーをrefに書き込むので、ref_lengthはPKキー長に設定する
  if (this->table->s->primary_key != MAX_KEY) {
    const KEY &pk_info = this->table->s->key_info[this->table->s->primary_key];
    this->ref_length = pk_info.key_length;
  } else {
    this->ref_length = sizeof(ToydbRowId);
  }

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

  // TODO:
  // InnoDBだと参照カウントを作って同じTableのhandlerが全てなくなったタイミングでthis->shareをfreeしている

  return 0;
}

/**
 * @brief handler::table::fieldからrow_dataへデータを読み出す
 */
std::expected<std::vector<SupportedDBValue>, int>
ha_toydb::read_row_from_fields() {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  // handler::table::fieldからデータの読み取り（val_int()/val_str()）を呼ぶ前にread_setをセットする必要がある
  // read_setはビットマップで、どのフィールドを読み取るかを指定するためのもの
  // ただ今回は固定で全てのフィールドの読み書きを行うことにするので、全てのビットを立てる
  // dbug_tmp_use_all_columnsはデバッグビルドでのみ有効で、元のビットマップを保存して復元できる
  my_bitmap_map *old_map =
      dbug_tmp_use_all_columns(this->table, this->table->read_set);

  std::vector<SupportedDBValue> row_data;
  row_data.reserve(this->table->s->fields);

  for (Field **field = this->table->field; *field != nullptr; field++) {
    if ((*field)->is_null()) {
      row_data.emplace_back(std::monostate{});
      break;
    }

    switch ((*field)->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG: {
        // MySQLの整数型は全てint64_tで扱うことにする
        const int64 val = (*field)->val_int();
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
        DBUG_PRINT("toydb", ("Unsupported field type: %d", (*field)->type()));
        dbug_tmp_restore_column_map(this->table->read_set, old_map);
        return std::unexpected(HA_ERR_UNSUPPORTED);
    }
  }

  dbug_tmp_restore_column_map(this->table->read_set, old_map);
  return row_data;
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

  auto row_data = this->read_row_from_fields();
  if (!row_data) return row_data.error();

  const int ret = this->share->toydb_table->insert_row(std::move(*row_data));

  // 挿入成功時にAUTO_INCREMENTカウンタを更新する
  if (ret == 0 && this->table->next_number_field != nullptr) {
    const ulonglong auto_inc_val =
        static_cast<ulonglong>(this->table->next_number_field->val_int());
    this->share->toydb_table->update_auto_inc_value(auto_inc_val);
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

  auto row_data = this->read_row_from_fields();
  if (!row_data) return row_data.error();

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  const ToydbIndexKey pk_key = resolve_pk_key_from_cursor();
  return this->share->toydb_table->update_row_by_key(pk_key,
                                                     std::move(*row_data));
}

/**
 * @brief テーブルの行の削除
 */
int ha_toydb::delete_row(const uchar *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  const ToydbIndexKey pk_key = resolve_pk_key_from_cursor();
  return this->share->toydb_table->delete_row_by_key(pk_key);
}

/**
 * @brief Index Condition Pushdownの条件を受け取る
 *
 * ほぼほぼInnoDBの実装をまねてる
 */
Item *ha_toydb::idx_cond_push(uint keyno, Item *idx_cond) {
  DBUG_PRINT("toydb", ("%s keyno=%u", __func__, keyno));

  // 親クラス側に条件をセット
  this->pushed_idx_cond = idx_cond;
  this->pushed_idx_cond_keyno = keyno;
  this->in_range_check_pushed_down = true;

  // nullptrを返すことで条件全体をエンジン側で評価することを示せる
  return nullptr;
}

/**
 * @brief インデックススキャンの初期化
 *
 * idxは利用するインデックスの番号が入る
 */
int ha_toydb::index_init(uint idx, bool) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, idx));
  this->cursor = ToydbCursor{};
  this->active_index = idx;
  return 0;
}

/**
 * @brief インデックススキャンの終了
 */
int ha_toydb::index_end() {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));
  this->cursor = ToydbCursor{};
  this->active_index = MAX_KEY;
  return 0;
}

/**
 * @brief MySQLのキーバッファからToydbIndexKeyへデコードする
 *
 * key_restore()でレコードバッファに復元し、Fieldからval_int/val_strで読み取る
 */
std::expected<ToydbIndexKey, int> ha_toydb::deserialize_index_key(
    const uchar *key, uint key_len) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));

  // 利用するインデックスの情報
  KEY *key_info = &this->table->key_info[this->active_index];

  key_restore(this->table->record[0], key, key_info, key_len);

  my_bitmap_map *old_map =
      dbug_tmp_use_all_columns(this->table, this->table->read_set);

  ToydbIndexKey out_key;
  for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
    Field *field = key_info->key_part[i].field;

    if (field->is_null()) {
      out_key.key_parts.emplace_back(std::monostate{});
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
        DBUG_PRINT("toydb", ("Unsupported field type: %d", field->type()));
        dbug_tmp_restore_column_map(this->table->read_set, old_map);
        return std::unexpected(HA_ERR_UNSUPPORTED);
    }
  }

  dbug_tmp_restore_column_map(this->table->read_set, old_map);
  return out_key;
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
        row.at(i));
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

/**
 * @brief active_indexに応じてインデックスキーから行データを解決する
 *
 * PKスキャン時: Clustered Indexから直接取得
 * セカンダリスキャン時: セカンダリキーからPKを抽出してClustered
 * Indexをルックアップ
 */
std::optional<std::reference_wrapper<const std::vector<SupportedDBValue>>>
ha_toydb::fetch_row_values(const ToydbTable *table,
                           const ToydbIndexKey &index_key) const {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  if (is_primary_key_index(this->active_index)) {
    const auto it = table->find_row(index_key);
    if (it == table->rows_end()) return std::nullopt;
    return it->second.values;
  }

  const ToydbIndexKey pk_key =
      table->extract_pk_key_from_secondary(this->active_index, index_key);
  const auto it = table->find_row(pk_key);
  if (it == table->rows_end()) return std::nullopt;
  return it->second.values;
}

/**
 * @brief 行をレコードバッファに格納してICP条件を評価する
 *
 * @return ICP_MATCH_RESULT（マッチ/ミスマッチ）、エラー時はMySQLエラーコード
 */
std::expected<ICP_MATCH_RESULT, int> ha_toydb::try_icp_match(
    const std::vector<SupportedDBValue> &values, uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));

  const int ret = store_row_to_buf(this->table, buf, values);
  if (ret != 0) return std::unexpected(ret);

  // ItemはSQLのツリー構造を表していて、val_intを呼ぶとTableのレコードバッファ内のデータと条件を評価して0か1を返す
  // `mysql-server/sql/item_cmpfunc.cc`にあるval_intメソッドらへんが参考になる
  if (this->pushed_idx_cond == nullptr ||
      this->pushed_idx_cond->val_int() != 0) {
    return ICP_MATCH_RESULT::MATCH;
  }
  return ICP_MATCH_RESULT::NOT_MATCH;
}

int ha_toydb::index_read(uchar *buf, const uchar *key, uint key_len,
                         enum ha_rkey_function find_flag) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u find_flag=%s", __func__, this->active_index,
                       ha_rkey_function_name(find_flag)));

  auto search_key_result = deserialize_index_key(key, key_len);
  if (!search_key_result) return search_key_result.error();
  auto search_key = std::move(*search_key_result);

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  const auto *toydb_table = this->share->toydb_table;

  if (is_primary_key_index(this->active_index)) {
    // --- Clustered Index検索 ---
    ToydbTable::RowIterator iter;

    switch (find_flag) {
      case HA_READ_KEY_EXACT:
        iter = toydb_table->find_row(search_key);
        if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;
        break;
      case HA_READ_KEY_OR_NEXT:
        iter = toydb_table->lower_bound_row(search_key);
        if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;
        break;
      case HA_READ_AFTER_KEY:
        iter = toydb_table->upper_bound_row(search_key);
        if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;
        break;
      case HA_READ_KEY_OR_PREV:
        iter = toydb_table->upper_bound_row(search_key);
        if (iter == toydb_table->rows_begin()) return HA_ERR_KEY_NOT_FOUND;
        --iter;
        break;
      case HA_READ_BEFORE_KEY:
        iter = toydb_table->lower_bound_row(search_key);
        if (iter == toydb_table->rows_begin()) return HA_ERR_KEY_NOT_FOUND;
        --iter;
        break;
      default:
        return HA_ERR_UNSUPPORTED;
    }

    for (; iter != toydb_table->rows_end(); ++iter) {
      this->cursor.current_key = iter->first;
      const auto result = try_icp_match(iter->second.values, buf);
      if (!result) return result.error();
      if (*result == ICP_MATCH_RESULT::MATCH) {
        ha_statistic_increment(&System_status_var::ha_read_key_count);
        return 0;
      }
    }
    return HA_ERR_KEY_NOT_FOUND;
  }

  // --- Secondary Index検索 ---
  const auto &sec_idx = toydb_table->get_secondary_index(this->active_index);
  const auto &entries = sec_idx.entries;
  ToydbTable::SecondaryIndexIterator sec_iter;

  switch (find_flag) {
    case HA_READ_KEY_EXACT:
      sec_iter = entries.lower_bound(search_key);
      if (sec_iter == entries.end() ||
          !key_prefix_matches(*sec_iter, search_key))
        return HA_ERR_KEY_NOT_FOUND;
      break;
    case HA_READ_KEY_OR_NEXT:
      sec_iter = entries.lower_bound(search_key);
      if (sec_iter == entries.end()) return HA_ERR_KEY_NOT_FOUND;
      break;
    case HA_READ_AFTER_KEY:
      sec_iter = entries.lower_bound(search_key);
      while (sec_iter != entries.end() &&
             key_prefix_matches(*sec_iter, search_key)) {
        ++sec_iter;
      }
      if (sec_iter == entries.end()) return HA_ERR_KEY_NOT_FOUND;
      break;
    case HA_READ_KEY_OR_PREV:
      sec_iter = entries.upper_bound(search_key);
      if (sec_iter == entries.begin()) return HA_ERR_KEY_NOT_FOUND;
      --sec_iter;
      break;
    case HA_READ_BEFORE_KEY:
      sec_iter = entries.lower_bound(search_key);
      if (sec_iter == entries.begin()) return HA_ERR_KEY_NOT_FOUND;
      --sec_iter;
      break;
    default:
      return HA_ERR_UNSUPPORTED;
  }

  for (; sec_iter != entries.end(); ++sec_iter) {
    this->cursor.current_key = *sec_iter;
    const auto values = fetch_row_values(toydb_table, *sec_iter);
    if (!values) return HA_ERR_KEY_NOT_FOUND;
    const auto result = try_icp_match(values->get(), buf);
    if (!result) return result.error();
    if (*result == ICP_MATCH_RESULT::MATCH) {
      ha_statistic_increment(&System_status_var::ha_read_key_count);
      return 0;
    }
  }
  return HA_ERR_KEY_NOT_FOUND;
}

/**
 * @brief インデックス上で次の行を取得する
 */
int ha_toydb::index_next(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));

  if (!this->cursor.current_key.has_value()) return HA_ERR_END_OF_FILE;

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  const auto *toydb_table = this->share->toydb_table;

  if (is_primary_key_index(this->active_index)) {
    auto iter = toydb_table->find_row(*this->cursor.current_key);
    if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;
    for (++iter; iter != toydb_table->rows_end(); ++iter) {
      this->cursor.current_key = iter->first;
      const auto result = try_icp_match(iter->second.values, buf);
      if (!result) return result.error();
      if (*result == ICP_MATCH_RESULT::MATCH) {
        ha_statistic_increment(&System_status_var::ha_read_next_count);
        return 0;
      }
    }
    return HA_ERR_END_OF_FILE;
  }

  const auto &entries =
      toydb_table->get_secondary_index(this->active_index).entries;
  auto sec_iter = entries.find(*this->cursor.current_key);
  if (sec_iter == entries.end()) return HA_ERR_KEY_NOT_FOUND;
  for (++sec_iter; sec_iter != entries.end(); ++sec_iter) {
    this->cursor.current_key = *sec_iter;
    const auto values = fetch_row_values(toydb_table, *sec_iter);
    if (!values) return HA_ERR_KEY_NOT_FOUND;
    const auto result = try_icp_match(values->get(), buf);
    if (!result) return result.error();
    if (*result == ICP_MATCH_RESULT::MATCH) {
      ha_statistic_increment(&System_status_var::ha_read_next_count);
      return 0;
    }
  }
  return HA_ERR_END_OF_FILE;
}

/**
 * @brief インデックス上で前の行を取得する
 */
int ha_toydb::index_prev(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));

  if (!this->cursor.current_key.has_value()) return HA_ERR_END_OF_FILE;

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  const auto *toydb_table = this->share->toydb_table;

  if (is_primary_key_index(this->active_index)) {
    auto iter = toydb_table->find_row(*this->cursor.current_key);
    if (iter == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;
    while (iter != toydb_table->rows_begin()) {
      --iter;
      this->cursor.current_key = iter->first;
      const auto result = try_icp_match(iter->second.values, buf);
      if (!result) return result.error();
      if (*result == ICP_MATCH_RESULT::MATCH) {
        ha_statistic_increment(&System_status_var::ha_read_prev_count);
        return 0;
      }
    }
    return HA_ERR_END_OF_FILE;
  }

  const auto &entries =
      toydb_table->get_secondary_index(this->active_index).entries;
  auto sec_iter = entries.find(*this->cursor.current_key);
  if (sec_iter == entries.end() || sec_iter == entries.begin())
    return HA_ERR_END_OF_FILE;
  while (sec_iter != entries.begin()) {
    --sec_iter;
    this->cursor.current_key = *sec_iter;
    const auto values = fetch_row_values(toydb_table, *sec_iter);
    if (!values) return HA_ERR_KEY_NOT_FOUND;
    const auto result = try_icp_match(values->get(), buf);
    if (!result) return result.error();
    if (*result == ICP_MATCH_RESULT::MATCH) {
      ha_statistic_increment(&System_status_var::ha_read_prev_count);
      return 0;
    }
  }
  return HA_ERR_END_OF_FILE;
}

/**
 * @brief インデックスの先頭行を取得する
 */
int ha_toydb::index_first(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  const auto *toydb_table = this->share->toydb_table;

  if (is_primary_key_index(this->active_index)) {
    for (auto iter = toydb_table->rows_begin(); iter != toydb_table->rows_end();
         ++iter) {
      this->cursor.current_key = iter->first;
      const auto result = try_icp_match(iter->second.values, buf);
      if (!result) return result.error();
      if (*result == ICP_MATCH_RESULT::MATCH) {
        ha_statistic_increment(&System_status_var::ha_read_first_count);
        return 0;
      }
    }
    return HA_ERR_END_OF_FILE;
  }

  const auto &entries =
      toydb_table->get_secondary_index(this->active_index).entries;
  for (auto sec_iter = entries.begin(); sec_iter != entries.end(); ++sec_iter) {
    this->cursor.current_key = *sec_iter;
    const auto values = fetch_row_values(toydb_table, *sec_iter);
    if (!values) return HA_ERR_KEY_NOT_FOUND;
    const auto result = try_icp_match(values->get(), buf);
    if (!result) return result.error();
    if (*result == ICP_MATCH_RESULT::MATCH) {
      ha_statistic_increment(&System_status_var::ha_read_first_count);
      return 0;
    }
  }
  return HA_ERR_END_OF_FILE;
}

/**
 * @brief インデックスの末尾行を取得する
 */
int ha_toydb::index_last(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);
  const auto *toydb_table = this->share->toydb_table;

  if (is_primary_key_index(this->active_index)) {
    if (toydb_table->row_count() == 0) return HA_ERR_END_OF_FILE;
    auto iter = toydb_table->rows_last();
    while (true) {
      this->cursor.current_key = iter->first;
      const auto result = try_icp_match(iter->second.values, buf);
      if (!result) return result.error();
      if (*result == ICP_MATCH_RESULT::MATCH) {
        ha_statistic_increment(&System_status_var::ha_read_last_count);
        return 0;
      }

      if (iter == toydb_table->rows_begin()) break;
      --iter;
    }
    return HA_ERR_END_OF_FILE;
  }

  const auto &entries =
      toydb_table->get_secondary_index(this->active_index).entries;
  if (entries.empty()) return HA_ERR_END_OF_FILE;
  auto sec_iter = entries.end();
  while (true) {
    --sec_iter;
    this->cursor.current_key = *sec_iter;
    const auto values = fetch_row_values(toydb_table, *sec_iter);
    if (!values) return HA_ERR_KEY_NOT_FOUND;
    const auto result = try_icp_match(values->get(), buf);
    if (!result) return result.error();
    if (*result == ICP_MATCH_RESULT::MATCH) {
      ha_statistic_increment(&System_status_var::ha_read_last_count);
      return 0;
    }

    if (sec_iter == entries.begin()) break;
  }
  return HA_ERR_END_OF_FILE;
}

/**
 * @brief テーブルスキャン操作の初期化を行う
 */
int ha_toydb::rnd_init(bool) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));
  this->cursor = ToydbCursor{};
  return 0;
}

int ha_toydb::rnd_end() {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));
  return 0;
}

/**
 * @brief テーブルスキャン操作で次の行を取得する
 */
int ha_toydb::rnd_next(uchar *buf) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  const auto *toydb_table = this->share->toydb_table;

  // Clustered Indexを順スキャンする
  // カーソルが未設定なら先頭から、設定済みならupper_boundで次の行へ
  ToydbTable::RowIterator iter;
  if (!this->cursor.current_key.has_value()) {
    iter = toydb_table->rows_begin();
  } else {
    iter = toydb_table->upper_bound_row(*this->cursor.current_key);
  }

  if (iter == toydb_table->rows_end()) return HA_ERR_END_OF_FILE;

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);

  this->cursor.current_key = iter->first;

  return store_row_to_buf(this->table, buf, iter->second.values);
}

/**
 * @brief データ走査時の現在位置をhandler->refに保存する
 *
 * handler::refに現在の行位置を保存して、サーバー層がrnd_posで任意位置から読み出せるようにする
 *
 * InnoDBと同様にPK値そのものをrowidとして使用する
 */
void ha_toydb::position(const uchar *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));

  // position()はMySQLサーバーが行読み取り直後に呼ぶので
  // table->record[0]に現在の行データが入っている
  // key_copy()でPKをMySQL内部キー形式でrefにコピーする
  const KEY &pk_info = this->table->key_info[this->table->s->primary_key];
  key_copy(this->ref, this->table->record[0], &pk_info, pk_info.key_length);
}

/**
 * @brief handler->refに保存された位置から行を読み出す
 */
int ha_toydb::rnd_pos(uchar *buf, uchar *pos) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s idx=%u", __func__, this->active_index));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  // refからPKキーをデシリアライズしてClustered Indexをルックアップ
  auto pk_result = deserialize_pk_from_ref(pos);
  if (!pk_result) return pk_result.error();
  const auto *toydb_table = this->share->toydb_table;
  const auto it = toydb_table->find_row(*pk_result);
  if (it == toydb_table->rows_end()) return HA_ERR_KEY_NOT_FOUND;

  return store_row_to_buf(this->table, buf, it->second.values);
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
 * @brief ha_extra_functionのフラグに対応する文字列を返す（デバッグ用）
 */
static const char *ha_extra_function_names[] = {
    "HA_EXTRA_NORMAL",                       // 0
    "HA_EXTRA_QUICK",                        // 1
    "HA_EXTRA_NOT_USED",                     // 2
    nullptr,                                 // 3
    nullptr,                                 // 4
    "HA_EXTRA_NO_READCHECK",                 // 5
    "HA_EXTRA_READCHECK",                    // 6
    "HA_EXTRA_KEYREAD",                      // 7
    "HA_EXTRA_NO_KEYREAD",                   // 8
    "HA_EXTRA_NO_USER_CHANGE",               // 9
    nullptr,                                 // 10
    nullptr,                                 // 11
    "HA_EXTRA_WAIT_LOCK",                    // 12
    "HA_EXTRA_NO_WAIT_LOCK",                 // 13
    nullptr,                                 // 14
    nullptr,                                 // 15
    "HA_EXTRA_NO_KEYS",                      // 16
    "HA_EXTRA_KEYREAD_CHANGE_POS",           // 17
    "HA_EXTRA_REMEMBER_POS",                 // 18
    "HA_EXTRA_RESTORE_POS",                  // 19
    nullptr,                                 // 20
    "HA_EXTRA_FORCE_REOPEN",                 // 21
    "HA_EXTRA_FLUSH",                        // 22
    "HA_EXTRA_NO_ROWS",                      // 23
    "HA_EXTRA_RESET_STATE",                  // 24
    "HA_EXTRA_IGNORE_DUP_KEY",               // 25
    "HA_EXTRA_NO_IGNORE_DUP_KEY",            // 26
    "HA_EXTRA_PREPARE_FOR_DROP",             // 27
    "HA_EXTRA_PREPARE_FOR_UPDATE",           // 28
    "HA_EXTRA_PRELOAD_BUFFER_SIZE",          // 29
    "HA_EXTRA_CHANGE_KEY_TO_UNIQUE",         // 30
    "HA_EXTRA_CHANGE_KEY_TO_DUP",            // 31
    "HA_EXTRA_KEYREAD_PRESERVE_FIELDS",      // 32
    "HA_EXTRA_IGNORE_NO_KEY",                // 33
    "HA_EXTRA_NO_IGNORE_NO_KEY",             // 34
    "HA_EXTRA_MARK_AS_LOG_TABLE",            // 35
    "HA_EXTRA_WRITE_CAN_REPLACE",            // 36
    "HA_EXTRA_WRITE_CANNOT_REPLACE",         // 37
    "HA_EXTRA_DELETE_CANNOT_BATCH",          // 38
    "HA_EXTRA_UPDATE_CANNOT_BATCH",          // 39
    "HA_EXTRA_INSERT_WITH_UPDATE",           // 40
    "HA_EXTRA_PREPARE_FOR_RENAME",           // 41
    "HA_EXTRA_ADD_CHILDREN_LIST",            // 42
    "HA_EXTRA_ATTACH_CHILDREN",              // 43
    "HA_EXTRA_IS_ATTACHED_CHILDREN",         // 44
    "HA_EXTRA_DETACH_CHILDREN",              // 45
    "HA_EXTRA_EXPORT",                       // 46
    "HA_EXTRA_SECONDARY_SORT_ROWID",         // 47
    "HA_EXTRA_NO_READ_LOCKING",              // 48
    "HA_EXTRA_BEGIN_ALTER_COPY",             // 49
    "HA_EXTRA_END_ALTER_COPY",               // 50
    "HA_EXTRA_NO_AUTOINC_LOCKING",           // 51
    "HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER",  // 52
    "HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER"  // 53
};

/**
 * @brief mysql側からSEに対してヒントを渡すためのメソッド
 */
int ha_toydb::extra(enum ha_extra_function operation) {
  // DBUG_TRACE;
  const auto op = static_cast<size_t>(operation);
  const char *op_name = (op < std::size(ha_extra_function_names) &&
                         ha_extra_function_names[op] != nullptr)
                            ? ha_extra_function_names[op]
                            : "UNKNOWN";
  DBUG_PRINT("toydb", ("%s operation=%s(%d)", __func__, op_name,
                       static_cast<int>(operation)));

  switch (operation) {
    // TODO: 本来ならoperationに応じて最適化処理を走らせる
    case HA_EXTRA_KEYREAD:
      // Index-onlyスキャンモードにする
      // いまだとカバリングインデックスとか関係なしに全行返しちゃってる
      break;
    case HA_EXTRA_NO_KEYREAD:
      // Index-onlyスキャンモード解除
      break;
    default:
      break;
  }
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
  const std::string_view path(name);
  const auto pos = path.find_last_of('/');
  const std::string_view table_name =
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
 *
 * オプティマイザがレンジ分析をする際にレコード数を見積もるために利用する
 *
 * https://dbstudy.info/files/20120310/mysql_costcalc.pdf
 */
ha_rows ha_toydb::records_in_range(uint index_num, key_range *min_key,
                                   key_range *max_key) {
  DBUG_PRINT("toydb", ("%s", __func__));

  std::lock_guard<std::mutex> data_lock(*this->share->data_mutex);

  active_index = index_num;

  const ToydbTable *toydb_table = this->share->toydb_table;

  // min_key/max_keyはNULLの場合がある（片側のみの範囲条件時）
  std::optional<ToydbIndexKey> min_index_key;
  std::optional<ToydbIndexKey> max_index_key;

  if (min_key != nullptr) {
    auto min_result = deserialize_index_key(min_key->key, min_key->length);
    if (!min_result) return HA_POS_ERROR;
    min_index_key = std::move(*min_result);
  }
  if (max_key != nullptr) {
    auto max_result = deserialize_index_key(max_key->key, max_key->length);
    if (!max_result) return HA_POS_ERROR;
    max_index_key = std::move(*max_result);
  }

  if (is_primary_key_index(index_num)) {
    auto start = min_key != nullptr
                     ? (min_key->flag == HA_READ_AFTER_KEY
                            ? toydb_table->upper_bound_row(*min_index_key)
                            : toydb_table->lower_bound_row(*min_index_key))
                     : toydb_table->rows_begin();
    auto end = max_key != nullptr
                   ? (max_key->flag == HA_READ_BEFORE_KEY
                          ? toydb_table->lower_bound_row(*max_index_key)
                          : toydb_table->upper_bound_row(*max_index_key))
                   : toydb_table->rows_end();
    const ha_rows n_rows = std::max(0L, std::distance(start, end));
    return n_rows > 0 ? n_rows : 1;
  }

  // Secondary Index
  const auto &entries = toydb_table->get_secondary_index(index_num).entries;
  auto start = min_key != nullptr ? (min_key->flag == HA_READ_AFTER_KEY
                                         ? entries.upper_bound(*min_index_key)
                                         : entries.lower_bound(*min_index_key))
                                  : entries.begin();
  auto end = max_key != nullptr ? (max_key->flag == HA_READ_BEFORE_KEY
                                       ? entries.lower_bound(*max_index_key)
                                       : entries.upper_bound(*max_index_key))
                                : entries.end();
  ha_rows n_rows = std::max(0L, std::distance(start, end));
  return n_rows > 0 ? n_rows : 1;
}

int ha_toydb::create(const char *, TABLE *table_info,
                     HA_CREATE_INFO *create_info, dd::Table *) {
  // DBUG_TRACE;
  DBUG_PRINT("toydb", ("%s", __func__));

  const char *const table_name = table_info->s->table_name.str;

  // HTON_CAN_RECREATEにより、TRUNCATE時はdelete_tableを経由せず直接create()が呼ばれる
  // なのでその場合は既存テーブルを削除して再作成する
  if (toydb_tables->tables.contains(table_name)) {
    toydb_tables->tables.erase(table_name);
  }

  ToydbTable new_table(table_name);

  for (Field **f = table_info->field; *f != nullptr; f++) {
    DBUG_PRINT("toydb", ("Field: name=%s, type=%d is_null=%d is_unsigned=%d",
                         (*f)->field_name, (*f)->type(), (*f)->is_null(),
                         (*f)->is_unsigned()));
    ColumnFlag flags = ColumnFlag::NONE;
    if (!(*f)->is_null()) {
      flags |= ColumnFlag::NOT_NULL;
    }
    if ((*f)->is_unsigned()) {
      flags |= ColumnFlag::UNSIGNED;
    }
    int ret = new_table.add_column((*f)->field_name, (*f)->type(), flags);
    if (ret != 0) return ret;
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

  // セカンダリインデックス
  for (uint i = 0; i < table_info->s->keys; i++) {
    if (i == table_info->s->primary_key) continue;

    KEY *key = &table_info->key_info[i];
    std::vector<uint> col_indices;
    col_indices.reserve(key->user_defined_key_parts);
    for (uint j = 0; j < key->user_defined_key_parts; j++) {
      col_indices.push_back(key->key_part[j].fieldnr - 1);
    }
    const bool is_unique = (key->flags & HA_NOSAME) != 0;
    new_table.add_secondary_index(i, key->name, std::move(col_indices),
                                  is_unique);
  }

  // CREATE TABLE ... AUTO_INCREMENT=N で初期値が指定された場合に反映する
  if (create_info->auto_increment_value > 0) {
    new_table.set_auto_inc_value(create_info->auto_increment_value);
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

/**
 * @brief handler::refからPKキーをデシリアライズする
 *
 * 既存のdeserialize_index_key()を再利用してMySQL内部キー形式からToydbIndexKeyに変換する
 */
std::expected<ToydbIndexKey, int> ha_toydb::deserialize_pk_from_ref(
    const uchar *ref_buf) {
  const KEY &pk_info = this->table->key_info[this->table->s->primary_key];
  return deserialize_index_key(ref_buf, pk_info.key_length);
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

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

#include <sys/types.h>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "field_types.h"
#include "my_base.h" /* ha_rows */
#include "my_inttypes.h"
#include "sql/handler.h" /* handler */
#include "thr_lock.h"    /* THR_LOCK, THR_LOCK_DATA */

/**
 * TODO: example実装のコメント修正
 *
 * あとでコメントも英語にする
 */

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
  };
};

static bool check_type_match(enum_field_types expected,
                             const SupportedDBValue &value);
static bool check_supported_type(enum_field_types type);

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

  std::vector<ToydbRow> rows;

 public:
  explicit ToydbTable(std::string name);

  int add_column(const std::string &name, enum_field_types type);
  int insert_row(std::vector<SupportedDBValue> row_data);
  int update_row(size_t row_index, std::vector<SupportedDBValue> row_data);
  int delete_row(size_t row_index);
  size_t row_count() const;
  const std::vector<SupportedDBValue> &get_row(size_t row_index) const;
  const std::vector<ToydbColumn> &get_columns() const;
  void clear_rows();
  void print_all() const;
};

/**
 * @brief 全テーブルの定義と実データを管理する
 */
struct ToydbTables {
  std::map<std::string, ToydbTable> tables;
};

/**
 * @brief 同じテーブルの全てのhandlerインスタンスで共有するデータ
 */
class Toydb_share final : public Handler_share {
 public:
  THR_LOCK lock;

  // TODO:
  // 現状だと、write_rowなどの際にまるっとロックを取ってるので細かい粒度のロックに書き換えたい
  std::unique_ptr<std::mutex> data_mutex;

  // open()時に検索したToydbTableへのポインタをキャッシュする
  // ToydbTableはToydb_shareより長い寿命なので一旦生ポインタで管理する
  ToydbTable *toydb_table{nullptr};

  Toydb_share();
  ~Toydb_share() override { thr_lock_delete(&lock); }
};

struct ToydbCursor {
  bool positioned{false};
  // テーブルスキャン時の現在の行位置
  size_t current_pos{0};
  // active_indexを保持する
  uint mysql_index_no{MAX_KEY};
  std::optional<ToydbIndexKey> current_index_key;
};

/** @brief
  Class definition for the storage engine
*/
class ha_toydb : public handler {
  THR_LOCK_DATA lock;        ///< MySQL lock
  Toydb_share *share{};      ///< Shared lock info
  Toydb_share *get_share();  ///< Get the share

  // テーブルスキャンとインデックススキャンでそれぞれカーソルを持つ
  ToydbCursor scan_cursor;
  ToydbCursor index_cursor;

 public:
  ha_toydb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_toydb() override = default;

  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const override { return "TOYDB"; }

  /**
   * @brief デフォルトのインデックスアルゴリズム
   *
   * 今回はClustered IndexのみサポートするのでB-Treeを返す
   */
  enum ha_key_alg get_default_index_algorithm() const override {
    return HA_KEY_ALG_BTREE;
  }
  bool is_index_algorithm_supported(enum ha_key_alg key_alg) const override {
    return key_alg == HA_KEY_ALG_BTREE;
  }

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const override { return HA_BINLOG_STMT_CAPABLE; }

  /**
   * @brief サポートするIndexの機能を返す
   *
   * 一旦完全一致と前方一致だけサポート
   */
  ulong index_flags(uint, uint, bool) const override {
    return HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER;
  }

  uint max_supported_record_length() const override {
    return HA_MAX_REC_LENGTH;
  }

  uint max_supported_keys() const override { return MAX_KEY; }

  uint max_supported_key_parts() const override { return MAX_REF_PARTS; }

  uint max_supported_key_length() const override { return MAX_KEY_LENGTH; }

  double scan_time() override {
    return (static_cast<double>(stats.records + stats.deleted) / 20.0) + 10;
  }

  double read_time(uint, uint, ha_rows rows) override {
    return (static_cast<double>(rows) / 20.0) + 1;
  }

  int open(const char *name, int mode, uint test_if_locked,
           const dd::Table *table_def) override;  // required

  int close(void) override;  // required

  int write_row(uchar *buf) override;

  int update_row(const uchar *old_data, uchar *new_data) override;

  int delete_row(const uchar *buf) override;

  int index_read(uchar *buf, const uchar *key, uint key_len,
                 enum ha_rkey_function find_flag) override;

  int index_next(uchar *buf) override;

  int index_prev(uchar *buf) override;

  int index_first(uchar *buf) override;

  int index_last(uchar *buf) override;

  int rnd_init(bool scan) override;  // required
  int rnd_end() override;
  int rnd_next(uchar *buf) override;             ///< required
  int rnd_pos(uchar *buf, uchar *pos) override;  ///< required
  void position(const uchar *record) override;   ///< required
  int info(uint) override;                       ///< required
  int extra(enum ha_extra_function operation) override;
  int external_lock(THD *thd, int lock_type) override;  ///< required
  int delete_all_rows(void) override;
  ha_rows records_in_range(uint inx, key_range *min_key,
                           key_range *max_key) override;
  int delete_table(const char *name, const dd::Table *) override;
  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table_def,
                   dd::Table *to_table_def) override;
  int create(const char *, TABLE *, HA_CREATE_INFO *table_info,
             dd::Table *) override;  ///< required

  THR_LOCK_DATA **store_lock(
      THD *thd, THR_LOCK_DATA **to,
      enum thr_lock_type lock_type) override;  ///< required

 private:
  int read_row_from_fields(std::vector<SupportedDBValue> &row_data);

  // Handler_shareのロックをRAIIで管理する
  // 内部ではmysql_mutex_lock,mysql_mutex_unlockを利用していてstd::lock_guardを使えないのでラッパーを作った
  class SharedHaDataLock {
   public:
    explicit SharedHaDataLock(ha_toydb *handler) : handler_(handler) {
      this->handler_->lock_shared_ha_data();
    }
    ~SharedHaDataLock() { this->handler_->unlock_shared_ha_data(); }

    // ロックの所有権はコピーされたくないので明示的に禁止する
    SharedHaDataLock(const SharedHaDataLock &) = delete;
    SharedHaDataLock &operator=(const SharedHaDataLock &) = delete;

   private:
    ha_toydb *handler_;
  };
};

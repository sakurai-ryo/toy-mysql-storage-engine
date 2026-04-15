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

#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "my_base.h"
#include "sql/handler.h"
#include "thr_lock.h"

#include "toydb_table.h"

enum class ICP_MATCH_RESULT {
  MATCH,
  NOT_MATCH,
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

/**
 * @brief Clustered Index or Secondary Index上のカーソル
 *
 * PKスキャン / テーブルスキャン時: Clustered Indexのキー
 * セカンダリスキャン時: セカンダリ複合キー（PKのサフィックス含む）
 */
struct ToydbCursor {
  std::optional<ToydbIndexKey> current_key;
};

/** @brief
  Class definition for the storage engine
*/
class ha_toydb : public handler {
  THR_LOCK_DATA lock;        ///< MySQL lock
  Toydb_share *share{};      ///< Shared lock info
  Toydb_share *get_share();  ///< Get the share

  ToydbCursor cursor;

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

  /**
   * @brief サポートするIndexの機能を返す
   *
   * 一旦完全一致と前方一致だけサポート
   *
   * ICPも追加でサポート
   */
  ulong index_flags(uint, uint, bool) const override {
    return HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER |
           HA_DO_INDEX_COND_PUSHDOWN | HA_KEYREAD_ONLY | HA_READ_RANGE;
  }

  uint max_supported_record_length() const override {
    return HA_MAX_REC_LENGTH;
  }

  /**
   * @brief サポートするインデックスの最大数
   */
  uint max_supported_keys() const override { return MAX_KEY; }

  /**
   * @brief 1インデックスあたりの最大カラム数
   */
  uint max_supported_key_parts() const override { return MAX_REF_PARTS; }

  /**
   * @brief サポートするインデックスの最大長
   */
  uint max_supported_key_length() const override { return MAX_KEY_LENGTH; }

  double scan_time() override {
    return (static_cast<double>(stats.records + stats.deleted) / 20.0) + 10;
  }

  double read_time(uint, uint, ha_rows rows) override {
    return (static_cast<double>(rows) / 20.0) + 1;
  }

  int create(const char *, TABLE *table_info, HA_CREATE_INFO *create_info,
             dd::Table *) override;

  Item *idx_cond_push(uint keyno, Item *idx_cond) override;

  void position(const uchar *record) override;
  int info(uint) override;
  int delete_all_rows(void) override;
  ha_rows records_in_range(uint index_num, key_range *min_key,
                           key_range *max_key) override;

  // 呼び出し元のwrite_row()で既にdata_mutexを取得済みの前提
  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values, ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;

 protected:
  int delete_table(const char *name, const dd::Table *) override;
  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table_def,
                   dd::Table *to_table_def) override;

  int index_read(uchar *buf, const uchar *key, uint key_len,
                 enum ha_rkey_function find_flag) override;
  int index_next(uchar *buf) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;

  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar *buf, uchar *pos) override;

 private:
  /** @brief Storage Engineがサポートする機能のフラグ
   *
   * handler.hに一覧がある
   */
  ulonglong table_flags() const override {
    return HA_BINLOG_STMT_CAPABLE | HA_PRIMARY_KEY_IN_READ_INDEX |
           HA_NULL_IN_KEY;
  }

  int open(const char *name, int mode, uint test_if_locked,
           const dd::Table *table_def) override;
  int close(void) override;

  int write_row(uchar *buf) override;
  int update_row(const uchar *old_data, uchar *new_data) override;
  int delete_row(const uchar *buf) override;

  int index_init(uint idx, bool sorted) override;
  int index_end() override;

  int rnd_init(bool scan) override;
  int rnd_end() override;

  int extra(enum ha_extra_function operation) override;

  int external_lock(THD *thd, int lock_type) override;

  bool is_primary_key_index(uint idx) const;
  ToydbIndexKey resolve_pk_key_from_cursor() const;
  std::expected<std::vector<SupportedDBValue>, int> read_row_from_fields();
  std::expected<ToydbIndexKey, int> deserialize_index_key(const uchar *key,
                                                          uint key_len);

  std::expected<ToydbIndexKey, int> deserialize_pk_from_ref(
      const uchar *ref_buf);

  // インデックスキーから行データを解決する（PK/セカンダリ共通）
  std::optional<std::reference_wrapper<const std::vector<SupportedDBValue>>>
  fetch_row_values(const ToydbTable *table,
                   const ToydbIndexKey &index_key) const;

  // 行をレコードバッファに格納してICP条件を評価する
  std::expected<ICP_MATCH_RESULT, int> try_icp_match(
      const std::vector<SupportedDBValue> &values, uchar *buf);

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

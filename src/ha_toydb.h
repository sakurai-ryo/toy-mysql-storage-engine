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
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "my_base.h" /* ha_rows */
#include "my_inttypes.h"
#include "sql/handler.h" /* handler */
#include "thr_lock.h"    /* THR_LOCK, THR_LOCK_DATA */

// class Toydb_table {
//  public:
// }

/**
  全てのhandlerインスタンスで共有するデータを保持するクラス
*/
class Toydb_share : public Handler_share {
 public:
  THR_LOCK lock;
  std::mutex data_mutex;

  // std::map<std::string,>

  // TODO: 初期実装なので削除する
  std::map<int64_t, std::string> data;

  Toydb_share();
  ~Toydb_share() override { thr_lock_delete(&lock); }
};

/** @brief
  Class definition for the storage engine
*/
class ha_toydb : public handler {
  THR_LOCK_DATA lock;        ///< MySQL lock
  Toydb_share *share{};      ///< Shared lock info
  Toydb_share *get_share();  ///< Get the share

  /// Scan state for rnd_next
  std::vector<std::pair<int64_t, std::string>> scan_rows;
  size_t scan_index = 0;
  int64_t current_key = 0;

 public:
  ha_toydb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_toydb() override = default;

  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const override { return "TOYDB"; }

  /**
    Replace key algorithm with one supported by SE, return the default key
    algorithm for SE if explicit key algorithm was not provided.

    @sa handler::adjust_index_algorithm().
  */
  enum ha_key_alg get_default_index_algorithm() const override {
    return HA_KEY_ALG_HASH;
  }
  bool is_index_algorithm_supported(enum ha_key_alg key_alg) const override {
    return key_alg == HA_KEY_ALG_HASH;
  }

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const override { return HA_BINLOG_STMT_CAPABLE; }

  /** @brief
    This is a bitmap of flags that indicates how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero here.

      @details
    part is the key part to check. First key part is 0.
    If all_parts is set, MySQL wants to know the flags for the combined
    index, up to and including 'part'.
  */
  ulong index_flags(uint inx [[maybe_unused]], uint part [[maybe_unused]],
                    bool all_parts [[maybe_unused]]) const override {
    return 0;
  }

  uint max_supported_record_length() const override {
    return HA_MAX_REC_LENGTH;
  }

  uint max_supported_keys() const override { return 0; }

  uint max_supported_key_parts() const override { return 0; }

  uint max_supported_key_length() const override { return 0; }

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

  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
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
  int delete_table(const char *from, const dd::Table *table_def) override;
  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table_def,
                   dd::Table *to_table_def) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
             dd::Table *table_def) override;  ///< required

  THR_LOCK_DATA **store_lock(
      THD *thd, THR_LOCK_DATA **to,
      enum thr_lock_type lock_type) override;  ///< required
};

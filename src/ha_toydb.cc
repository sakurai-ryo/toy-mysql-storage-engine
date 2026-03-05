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
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

// #include "my_dbug.h"
// #include "mysql/plugin.h"
// #include "sql/table.h"
// #include "sql/field.h"
// #include "nulls.h"
// #include "sql/sql_class.h"
// #include "sql/sql_plugin.h"
// #include "typelib.h"

#include <fcntl.h>
#include <mysql/plugin.h>
#include <sys/types.h>
#include <mutex>

#include "my_base.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_psi_config.h"
#include "mysql/plugin.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql/service_thd_alloc.h"
#include "mysql/status_var.h"
#include "nulls.h"
#include "sql/derror.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/mysqld_cs.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_plugin.h"
#include "sql/table.h"
#include "template_utils.h"
#include "thr_lock.h"
#include "typelib.h"

static handler *toydb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                     bool partitioned, MEM_ROOT *mem_root);

static handlerton *toydb_hton;

Toydb_share::Toydb_share() { thr_lock_init(&lock); }

/**
 * Storage Engineの初期化を行う
 */
static int toydb_init_func(void *p) {
  DBUG_TRACE;

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
 * Storage Engineのdeconstructor
 *
 * 今回は特に処理はなし
 */
static int toydb_deinit_func(void *p [[maybe_unused]]) {
  DBUG_TRACE;

  assert(p);

  return 0;
}

Toydb_share *ha_toydb::get_share() {
  Toydb_share *tmp_share = nullptr;

  DBUG_TRACE;

  this->lock_shared_ha_data();
  if ((tmp_share = dynamic_cast<Toydb_share *>(this->get_ha_share_ptr())) ==
      nullptr) {
    tmp_share = new Toydb_share;
    if (tmp_share == nullptr) goto err;

    this->set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  this->unlock_shared_ha_data();
  return tmp_share;
}

static handler *toydb_create_handler(handlerton *hton, TABLE_SHARE *table, bool,
                                     MEM_ROOT *mem_root) {
  return new (mem_root) ha_toydb(hton, table);
}

ha_toydb::ha_toydb(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {
  this->ref_length = sizeof(int64_t);
}

int ha_toydb::open(const char *, int, uint, const dd::Table *) {
  DBUG_TRACE;

  if ((this->share = this->get_share()) == nullptr) return 1;
  thr_lock_data_init(&this->share->lock, &this->lock, nullptr);

  return 0;
}

int ha_toydb::close(void) {
  DBUG_TRACE;
  return 0;
}

int ha_toydb::write_row(uchar *) {
  DBUG_TRACE;

  int64_t key = this->table->field[0]->val_int();
  String val_buf;
  String *val = this->table->field[1]->val_str(&val_buf);

  std::lock_guard<std::mutex> guard(this->share->data_mutex);
  if (static_cast<unsigned int>(this->share->data.contains(key)) != 0U)
    return HA_ERR_FOUND_DUPP_KEY;
  this->share->data.emplace(key, std::string(val->ptr(), val->length()));
  return 0;
}

int ha_toydb::update_row(const uchar *, uchar *) {
  DBUG_TRACE;

  int64_t new_key = this->table->field[0]->val_int();
  String val_buf;
  String *val = this->table->field[1]->val_str(&val_buf);

  std::lock_guard<std::mutex> guard(this->share->data_mutex);
  if (new_key != this->current_key) {
    if (static_cast<unsigned int>(this->share->data.contains(new_key)) != 0U)
      return HA_ERR_FOUND_DUPP_KEY;
    this->share->data.erase(this->current_key);
  }
  this->share->data[new_key] = std::string(val->ptr(), val->length());
  this->current_key = new_key;
  return 0;
}

int ha_toydb::delete_row(const uchar *) {
  DBUG_TRACE;

  std::lock_guard<std::mutex> guard(this->share->data_mutex);
  this->share->data.erase(this->current_key);
  return 0;
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

int ha_toydb::rnd_init(bool) {
  DBUG_TRACE;

  std::lock_guard<std::mutex> guard(this->share->data_mutex);
  this->scan_rows.assign(this->share->data.begin(), this->share->data.end());
  this->scan_index = 0;
  return 0;
}

int ha_toydb::rnd_end() {
  DBUG_TRACE;
  this->scan_rows.clear();
  return 0;
}

int ha_toydb::rnd_next(uchar *buf) {
  DBUG_TRACE;

  if (this->scan_index >= this->scan_rows.size()) return HA_ERR_END_OF_FILE;

  auto &[key, val] = this->scan_rows[this->scan_index++];
  this->current_key = key;

  memset(buf, 0, this->table->s->null_bytes);
  this->table->field[0]->store(key, false);
  this->table->field[1]->store(val.c_str(), val.length(), system_charset_info);
  return 0;
}

void ha_toydb::position(const uchar *) {
  DBUG_TRACE;
  memcpy(this->ref, &this->current_key, sizeof(this->current_key));
}

int ha_toydb::rnd_pos(uchar *buf, uchar *pos) {
  DBUG_TRACE;

  int64_t key = 0;
  memcpy(&key, pos, sizeof(key));

  std::lock_guard<std::mutex> guard(this->share->data_mutex);
  auto it = this->share->data.find(key);
  if (it == this->share->data.end()) return HA_ERR_KEY_NOT_FOUND;

  this->current_key = key;
  memset(buf, 0, this->table->s->null_bytes);
  this->table->field[0]->store(key, false);
  this->table->field[1]->store(it->second.c_str(), it->second.length(),
                               system_charset_info);
  return 0;
}

int ha_toydb::info(uint) {
  DBUG_TRACE;
  if (this->share != nullptr) {
    std::lock_guard<std::mutex> guard(this->share->data_mutex);
    this->stats.records = this->share->data.size();
  }
  return 0;
}

int ha_toydb::extra(enum ha_extra_function) {
  DBUG_TRACE;
  return 0;
}

int ha_toydb::delete_all_rows() {
  DBUG_TRACE;
  std::lock_guard<std::mutex> guard(this->share->data_mutex);
  this->share->data.clear();
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

int ha_toydb::rename_table(const char *, const char *, const dd::Table *,
                           dd::Table *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

ha_rows ha_toydb::records_in_range(uint, key_range *, key_range *) {
  DBUG_TRACE;
  return 10;  // low number to force index usage
}

static MYSQL_THDVAR_STR(last_create_thdvar, PLUGIN_VAR_MEMALLOC, nullptr,
                        nullptr, nullptr, nullptr);

static MYSQL_THDVAR_UINT(create_count_thdvar, 0, nullptr, nullptr, nullptr, 0,
                         0, 1000, 0);

int ha_toydb::create(const char *name, TABLE *, HA_CREATE_INFO *, dd::Table *) {
  DBUG_TRACE;

  THD *thd = this->ha_thd();
  char *buf = static_cast<char *>(
      my_malloc(PSI_NOT_INSTRUMENTED, SHOW_VAR_FUNC_BUFF_SIZE, MYF(MY_FAE)));
  snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE, "Last creation '%s'", name);
  THDVAR_SET(thd, last_create_thdvar, buf);
  my_free(buf);

  uint count = THDVAR(thd, create_count_thdvar) + 1;
  THDVAR_SET(thd, create_count_thdvar, &count);

  return 0;
}

static struct st_mysql_storage_engine toydb_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

static ulong srv_enum_var = 0;
static ulong srv_ulong_var = 0;
static double srv_double_var = 0;
static int srv_signed_int_var = 0;
static long srv_signed_long_var = 0;
static longlong srv_signed_longlong_var = 0;

static const char *enum_var_names[] = {"e1", "e2", NullS};

static TYPELIB enum_var_typelib = {array_elements(enum_var_names) - 1,
                                   "enum_var_typelib", enum_var_names, nullptr};

static MYSQL_SYSVAR_ENUM(enum_var,                        // name
                         srv_enum_var,                    // varname
                         PLUGIN_VAR_RQCMDARG,             // opt
                         "Sample ENUM system variable.",  // comment
                         nullptr,                         // check
                         nullptr,                         // update
                         0,                               // def
                         &enum_var_typelib);              // typelib

static MYSQL_SYSVAR_ULONG(ulong_var, srv_ulong_var, PLUGIN_VAR_RQCMDARG,
                          "0..1000", nullptr, nullptr, 8, 0, 1000, 0);

static MYSQL_SYSVAR_DOUBLE(double_var, srv_double_var, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", nullptr, nullptr, 8.5, 0.5,
                           1000.5,
                           0);  // reserved always 0

static MYSQL_THDVAR_DOUBLE(double_thdvar, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", nullptr, nullptr, 8.5, 0.5,
                           1000.5, 0);

static MYSQL_SYSVAR_INT(signed_int_var, srv_signed_int_var, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", nullptr, nullptr, -10, INT_MIN,
                        INT_MAX, 0);

static MYSQL_THDVAR_INT(signed_int_thdvar, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", nullptr, nullptr, -10, INT_MIN,
                        INT_MAX, 0);

static MYSQL_SYSVAR_LONG(signed_long_var, srv_signed_long_var,
                         PLUGIN_VAR_RQCMDARG, "LONG_MIN..LONG_MAX", nullptr,
                         nullptr, -10, LONG_MIN, LONG_MAX, 0);

static MYSQL_THDVAR_LONG(signed_long_thdvar, PLUGIN_VAR_RQCMDARG,
                         "LONG_MIN..LONG_MAX", nullptr, nullptr, -10, LONG_MIN,
                         LONG_MAX, 0);

static MYSQL_SYSVAR_LONGLONG(signed_longlong_var, srv_signed_longlong_var,
                             PLUGIN_VAR_RQCMDARG, "LLONG_MIN..LLONG_MAX",
                             nullptr, nullptr, -10, LLONG_MIN, LLONG_MAX, 0);

static MYSQL_THDVAR_LONGLONG(signed_longlong_thdvar, PLUGIN_VAR_RQCMDARG,
                             "LLONG_MIN..LLONG_MAX", nullptr, nullptr, -10,
                             LLONG_MIN, LLONG_MAX, 0);

static SYS_VAR *toydb_system_variables[] = {
    MYSQL_SYSVAR(enum_var),
    MYSQL_SYSVAR(ulong_var),
    MYSQL_SYSVAR(double_var),
    MYSQL_SYSVAR(double_thdvar),
    MYSQL_SYSVAR(last_create_thdvar),
    MYSQL_SYSVAR(create_count_thdvar),
    MYSQL_SYSVAR(signed_int_var),
    MYSQL_SYSVAR(signed_int_thdvar),
    MYSQL_SYSVAR(signed_long_var),
    MYSQL_SYSVAR(signed_long_thdvar),
    MYSQL_SYSVAR(signed_longlong_var),
    MYSQL_SYSVAR(signed_longlong_thdvar),
    nullptr};

// this is an example of SHOW_FUNC
static int show_func_toydb(MYSQL_THD, SHOW_VAR *var, char *buf) {
  var->type = SHOW_CHAR;
  var->value = buf;  // it's of SHOW_VAR_FUNC_BUFF_SIZE bytes
  snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE,
           "enum_var is %lu, ulong_var is %lu, "
           "double_var is %f, signed_int_var is %d, "
           "signed_long_var is %ld, signed_longlong_var is %lld",
           srv_enum_var, srv_ulong_var, srv_double_var, srv_signed_int_var,
           srv_signed_long_var, srv_signed_longlong_var);
  return 0;
}

struct toydb_vars_t {
  ulong var1;
  double var2;
  char var3[64];
  bool var4;
  bool var5;
  ulong var6;
};

static toydb_vars_t toydb_vars = {100,  20.01, "three hundred",
                                  true, false, 8250};

static SHOW_VAR show_status_toydb[] = {
    {"var1", reinterpret_cast<char *>(&toydb_vars.var1), SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"var2", reinterpret_cast<char *>(&toydb_vars.var2), SHOW_DOUBLE,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF,
     SHOW_SCOPE_UNDEF}  // null terminator required
};

static SHOW_VAR show_array_toydb[] = {
    {"array", reinterpret_cast<char *>(show_status_toydb), SHOW_ARRAY,
     SHOW_SCOPE_GLOBAL},
    {"var3", reinterpret_cast<char *>(&toydb_vars.var3), SHOW_CHAR,
     SHOW_SCOPE_GLOBAL},
    {"var4", reinterpret_cast<char *>(&toydb_vars.var4), SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

static SHOW_VAR func_status[] = {
    {"toydb_func_toydb", reinterpret_cast<char *>(show_func_toydb), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"toydb_status_var5", reinterpret_cast<char *>(&toydb_vars.var5), SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {"toydb_status_var6", reinterpret_cast<char *>(&toydb_vars.var6), SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"toydb_status", reinterpret_cast<char *>(show_array_toydb), SHOW_ARRAY,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

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
    func_status,            /* status variables */
    toydb_system_variables, /* system variables */
    nullptr,                /* config options */
    0,                      /* flags */
} mysql_declare_plugin_end;

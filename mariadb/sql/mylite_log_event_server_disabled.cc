/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"

#include "log_event.h"
#include "mysqld.h"
#include "sql_string.h"
#include "table.h"

#define MYLITE_LOG_EVENT_WRITE_DISABLED true
#define MYLITE_LOG_EVENT_IO_DISABLED 1

int append_query_string(CHARSET_INFO *csinfo, String *to,
                        const char *str, size_t len, bool no_backslash)
{
  char *beg, *ptr;
  my_bool overflow;
  uint32 const orig_len= to->length();
  if (to->reserve(orig_len + len * 2 + 4))
    return 1;

  beg= (char*) to->ptr() + to->length();
  ptr= beg;
  if (csinfo->escape_with_backslash_is_dangerous)
    ptr= str_to_hex(ptr, (uchar*) str, len);
  else
  {
    *ptr++= '\'';
    if (!no_backslash)
      ptr+= escape_string_for_mysql(csinfo, ptr, 0, str, len, &overflow);
    else
    {
      const char *frm_str= str;
      for (; frm_str < (str + len); frm_str++)
      {
        if (*frm_str == '\'')
          *ptr++= *frm_str;
        *ptr++= *frm_str;
      }
    }
    *ptr++= '\'';
  }

  to->length(orig_len + (ptr - beg));
  return 0;
}

Log_event::Log_event(THD *thd_arg, uint16 flags_arg, bool using_trans)
  : log_pos(0), temp_buf(0), exec_time(0),
    slave_exec_mode(SLAVE_EXEC_MODE_STRICT), thd(thd_arg)
{
  server_id= thd_arg ? thd_arg->variables.server_id
                     : global_system_variables.server_id;
  when= thd_arg ? thd_arg->start_time : 0;
  when_sec_part= thd_arg ? thd_arg->start_time_sec_part : 0;
  cache_type= using_trans ? EVENT_TRANSACTIONAL_CACHE : EVENT_STMT_CACHE;
  flags= flags_arg;
  if (thd_arg && (thd_arg->variables.option_bits & OPTION_SKIP_REPLICATION))
    flags|= LOG_EVENT_SKIP_REPLICATION_F;
}

Log_event::Log_event()
  : log_pos(0), temp_buf(0), when(0), when_sec_part(0), exec_time(0),
    server_id(global_system_variables.server_id), flags(0),
    event_owns_temp_buf(false), cache_type(EVENT_INVALID_CACHE),
    slave_exec_mode(SLAVE_EXEC_MODE_STRICT), thd(0)
{
}

void Log_event::init_show_field_list(THD *, List<Item> *)
{
}

bool Log_event::write_header(Log_event_writer *, size_t)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

int Log_event_writer::write_header(uchar *, size_t)
{
  return MYLITE_LOG_EVENT_IO_DISABLED;
}

int Log_event_writer::write_data(const uchar *, size_t)
{
  return MYLITE_LOG_EVENT_IO_DISABLED;
}

int Log_event_writer::write_footer()
{
  return MYLITE_LOG_EVENT_IO_DISABLED;
}

int Log_event_writer::write_internal(const uchar *, size_t)
{
  return MYLITE_LOG_EVENT_IO_DISABLED;
}

int Log_event_writer::encrypt_and_write(const uchar *, size_t)
{
  return MYLITE_LOG_EVENT_IO_DISABLED;
}

int Log_event_writer::maybe_write_event_len(uchar *, size_t)
{
  return MYLITE_LOG_EVENT_IO_DISABLED;
}

Query_log_event::Query_log_event()
  : Log_event(), data_buf(0), query(0), catalog(0), db(0), q_len(0),
    db_len(0), error_code(0), thread_id(0), slave_proxy_id(0),
    catalog_len(0), status_vars_len(0), flags2_inited(0),
    sql_mode_inited(false), charset_inited(false),
    character_set_collations({0, 0}), flags2(0), sql_mode(0),
    auto_increment_increment(0), auto_increment_offset(0), time_zone_len(0),
    time_zone_str(0), lc_time_names_number(0), charset_database_number(0),
    table_map_for_update(0), xid(0), gtid_flags_extra(0), sa_seq_no(0)
{
  memset(&user, 0, sizeof(user));
  memset(&host, 0, sizeof(host));
  memset(charset, 0, sizeof(charset));
}

Query_log_event::Query_log_event(THD *thd_arg, const char *query_arg,
                                 size_t query_length, bool using_trans,
                                 bool direct, bool suppress_use, int errcode)
  : Log_event(thd_arg, suppress_use ? LOG_EVENT_SUPPRESS_USE_F : 0,
              using_trans),
    data_buf(0), query(query_arg), catalog(thd_arg ? thd_arg->catalog : 0),
    db(thd_arg && thd_arg->db.str ? thd_arg->db.str : ""),
    q_len((uint32) query_length),
    db_len((uint32) strlen(db)), error_code(errcode),
    thread_id(thd_arg ? thd_arg->thread_id : 0),
    slave_proxy_id(thd_arg ? (ulong) thd_arg->variables.pseudo_thread_id : 0),
    catalog_len(catalog ? (uint32) strlen(catalog) : 0), status_vars_len(0),
    flags2_inited(1), sql_mode_inited(true), charset_inited(true),
    character_set_collations({0, 0}), flags2(0),
    sql_mode(thd_arg ? thd_arg->variables.sql_mode : 0),
    auto_increment_increment(thd_arg ?
                             thd_arg->variables.auto_increment_increment : 0),
    auto_increment_offset(thd_arg ?
                          thd_arg->variables.auto_increment_offset : 0),
    time_zone_len(0), time_zone_str(0), lc_time_names_number(0),
    charset_database_number(0),
    table_map_for_update(thd_arg ? (ulonglong) thd_arg->table_map_for_update : 0),
    xid(0), gtid_flags_extra(0), sa_seq_no(0)
{
  (void) direct;
  memset(&user, 0, sizeof(user));
  memset(&host, 0, sizeof(host));
  memset(charset, 0, sizeof(charset));
  cache_type= EVENT_INVALID_CACHE;
}

bool Query_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Query_compressed_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Format_description_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Intvar_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Rand_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Xid_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool XA_prepare_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool User_var_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Rotate_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

Binlog_checkpoint_log_event::Binlog_checkpoint_log_event(
    const char *binlog_file_name_arg, uint binlog_file_len_arg)
  : Log_event(), binlog_file_name(0), binlog_file_len(binlog_file_len_arg)
{
  cache_type= EVENT_NO_CACHE;
  if (binlog_file_name_arg)
    binlog_file_name= my_strndup(PSI_INSTRUMENT_ME, binlog_file_name_arg,
                                 binlog_file_len_arg, MYF(MY_WME));
}

bool Binlog_checkpoint_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

Gtid_log_event::Gtid_log_event(THD *thd_arg, uint64 seq_no_arg,
                               uint32 domain_id_arg, bool standalone,
                               uint16 flags_arg, bool is_transactional,
                               uint64 commit_id_arg, bool has_xid,
                               bool is_ro_1pc)
  : Log_event(thd_arg, flags_arg, is_transactional),
    seq_no(seq_no_arg), commit_id(commit_id_arg), domain_id(domain_id_arg),
    sa_seq_no(0), xid(), pad_to_size(0),
    flags2(standalone ? FL_STANDALONE : 0), flags_extra(0), extra_engines(0),
    thread_id(thd_arg ? thd_arg->thread_id : 0)
{
  if (is_transactional)
    flags2|= FL_TRANSACTIONAL;
  if (has_xid || is_ro_1pc)
    flags2|= FL_COMPLETED_XA;
}

bool Gtid_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

int Gtid_log_event::make_compatible_event(String *, bool *, ulong,
                                          enum_binlog_checksum_alg)
{
  return 1;
}

bool Gtid_log_event::peek(const uchar *, size_t,
                          enum_binlog_checksum_alg, uint32 *,
                          uint32 *, uint64 *, uchar *,
                          const Format_description_log_event *)
{
  return true;
}

Append_block_log_event::Append_block_log_event(THD *thd_arg,
                                               const char *db_arg,
                                               uchar *block_arg,
                                               uint block_len_arg,
                                               bool using_trans)
  : Log_event(thd_arg, 0, using_trans), block(block_arg),
    block_len(block_len_arg), file_id(thd_arg ? thd_arg->file_id : 0),
    db(db_arg)
{
}

bool Append_block_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

Delete_file_log_event::Delete_file_log_event(THD *thd_arg, const char *db_arg,
                                             bool using_trans)
  : Log_event(thd_arg, 0, using_trans), file_id(thd_arg ? thd_arg->file_id : 0),
    db(db_arg)
{
}

bool Delete_file_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

Begin_load_query_log_event::Begin_load_query_log_event(
    THD *thd_arg, const char *db_arg, uchar *block_arg, uint block_len_arg,
    bool using_trans)
  : Append_block_log_event(thd_arg, db_arg, block_arg, block_len_arg,
                           using_trans)
{
}

Execute_load_query_log_event::Execute_load_query_log_event(
    THD *thd_arg, const char *query_arg, ulong query_length,
    uint fn_pos_start_arg, uint fn_pos_end_arg,
    enum_load_dup_handling dup_handling_arg, bool using_trans, bool direct,
    bool suppress_use, int errcode)
  : Query_log_event(thd_arg, query_arg, query_length, using_trans, direct,
                    suppress_use, errcode),
    file_id(thd_arg ? thd_arg->file_id : 0),
    fn_pos_start(fn_pos_start_arg), fn_pos_end(fn_pos_end_arg),
    dup_handling(dup_handling_arg)
{
}

bool Execute_load_query_log_event::write_post_header_for_derived(
    Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

Annotate_rows_log_event::Annotate_rows_log_event(THD *thd_arg,
                                                 bool using_trans,
                                                 bool direct)
  : Log_event(thd_arg, 0, using_trans), m_query_txt(0)
{
  if (direct)
    cache_type= EVENT_NO_CACHE;
}

bool Annotate_rows_log_event::write_data_header(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Annotate_rows_log_event::write_data_body(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

Table_map_log_event::Table_map_log_event(THD *thd, TABLE *tbl, ulonglong tid,
                                         bool is_transactional)
  : Log_event(thd, 0, is_transactional), m_table(tbl),
    binlog_type_info_array(0),
    m_dbnam(tbl && tbl->s ? tbl->s->db.str : 0),
    m_dblen(tbl && tbl->s ? tbl->s->db.length : 0),
    m_tblnam(tbl && tbl->s ? tbl->s->table_name.str : 0),
    m_tbllen(tbl && tbl->s ? tbl->s->table_name.length : 0),
    m_colcnt(tbl && tbl->s ? tbl->s->fields : 0), m_coltype(0),
    m_memory(0), m_table_id(tid), m_flags(TM_BIT_LEN_EXACT_F),
    m_data_size(0), m_field_metadata(0), m_field_metadata_size(0),
    m_null_bits(0), m_meta_memory(0), m_optional_metadata_len(0),
    m_optional_metadata(0)
{
}

int Table_map_log_event::save_field_metadata()
{
  return 0;
}

bool Table_map_log_event::write_data_header(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Table_map_log_event::write_data_body(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Table_map_log_event::init_signedness_field()
{
  return true;
}

bool Table_map_log_event::init_charset_field(
    bool (*)(Binlog_type_info *, Field *), Optional_metadata_field_type,
    Optional_metadata_field_type)
{
  return true;
}

bool Table_map_log_event::init_column_name_field()
{
  return true;
}

bool Table_map_log_event::init_set_str_value_field()
{
  return true;
}

bool Table_map_log_event::init_enum_str_value_field()
{
  return true;
}

bool Table_map_log_event::init_geometry_type_field()
{
  return true;
}

bool Table_map_log_event::init_primary_key_field()
{
  return true;
}

Rows_log_event::Rows_log_event(THD *thd_arg, TABLE *tbl_arg,
                               ulonglong table_id, MY_BITMAP const *cols,
                               bool is_transactional,
                               Log_event_type event_type)
  : Log_event(thd_arg, 0, is_transactional), m_row_count(0),
    m_table(tbl_arg), m_table_id(table_id),
    m_width(tbl_arg && tbl_arg->s ? tbl_arg->s->fields : 1),
    m_rows_buf(0), m_rows_cur(0), m_rows_end(0), m_rows_before_size(0),
    m_flags_pos(0), m_flags(0), m_type(event_type), m_extra_row_data(0),
    m_vers_from_plain(false)
{
  if (my_bitmap_init(&m_cols,
                     m_width <= sizeof(m_bitbuf) * 8 ? m_bitbuf : NULL,
                     m_width))
    m_cols.bitmap= 0;
  else if (cols)
    bitmap_copy(&m_cols, cols);
  memset(&m_cols_ai, 0, sizeof(m_cols_ai));
}

int Rows_log_event::do_add_row_data(uchar *, size_t)
{
  return HA_ERR_UNSUPPORTED;
}

bool Rows_log_event::write_data_header(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Rows_log_event::write_data_body(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Rows_log_event::write_compressed(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

Write_rows_log_event::Write_rows_log_event(THD *thd_arg, TABLE *table_arg,
                                           ulonglong table_id,
                                           bool is_transactional)
  : Rows_log_event(thd_arg, table_arg, table_id,
                   table_arg ? table_arg->rpl_write_set : 0,
                   is_transactional, WRITE_ROWS_EVENT_V1)
{
}

Write_rows_compressed_log_event::Write_rows_compressed_log_event(
    THD *thd_arg, TABLE *table_arg, ulonglong table_id, bool is_transactional)
  : Write_rows_log_event(thd_arg, table_arg, table_id, is_transactional)
{
  m_type= WRITE_ROWS_COMPRESSED_EVENT_V1;
}

bool Write_rows_compressed_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

Update_rows_log_event::Update_rows_log_event(THD *thd_arg, TABLE *table_arg,
                                             ulonglong table_id,
                                             bool is_transactional)
  : Rows_log_event(thd_arg, table_arg, table_id,
                   table_arg ? table_arg->read_set : 0,
                   is_transactional, UPDATE_ROWS_EVENT_V1)
{
  init(table_arg ? table_arg->rpl_write_set : 0);
}

void Update_rows_log_event::init(MY_BITMAP const *cols)
{
  if (my_bitmap_init(&m_cols_ai,
                     m_width <= sizeof(m_bitbuf_ai) * 8 ? m_bitbuf_ai : NULL,
                     m_width))
    m_cols_ai.bitmap= 0;
  else if (cols)
    bitmap_copy(&m_cols_ai, cols);
}

Update_rows_compressed_log_event::Update_rows_compressed_log_event(
    THD *thd_arg, TABLE *table_arg, ulonglong table_id, bool is_transactional)
  : Update_rows_log_event(thd_arg, table_arg, table_id, is_transactional)
{
  m_type= UPDATE_ROWS_COMPRESSED_EVENT_V1;
}

bool Update_rows_compressed_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

Delete_rows_log_event::Delete_rows_log_event(THD *thd_arg, TABLE *table_arg,
                                             ulonglong table_id,
                                             bool is_transactional)
  : Rows_log_event(thd_arg, table_arg, table_id,
                   table_arg ? table_arg->read_set : 0,
                   is_transactional, DELETE_ROWS_EVENT_V1)
{
}

Delete_rows_compressed_log_event::Delete_rows_compressed_log_event(
    THD *thd_arg, TABLE *table_arg, ulonglong table_id, bool is_transactional)
  : Delete_rows_log_event(thd_arg, table_arg, table_id, is_transactional)
{
  m_type= DELETE_ROWS_COMPRESSED_EVENT_V1;
}

bool Delete_rows_compressed_log_event::write(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Incident_log_event::write_data_header(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

bool Incident_log_event::write_data_body(Log_event_writer *)
{
  return MYLITE_LOG_EVENT_WRITE_DISABLED;
}

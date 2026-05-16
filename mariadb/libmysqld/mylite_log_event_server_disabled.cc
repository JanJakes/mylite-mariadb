/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"

#include "log_event.h"
#include "sql_class.h"

#include <mysql/psi/psi_memory.h>

PSI_memory_key key_memory_log_event;

Log_event::Log_event(THD *thd_arg, uint16 flags_arg, bool using_trans)
  : log_pos(0), temp_buf(0), when(0), when_sec_part(0), exec_time(0),
    data_written(0), server_id(global_system_variables.server_id),
    flags(flags_arg), event_owns_temp_buf(false),
    cache_type(using_trans ? Log_event::EVENT_TRANSACTIONAL_CACHE :
                             Log_event::EVENT_STMT_CACHE),
    slave_exec_mode(SLAVE_EXEC_MODE_STRICT), thd(thd_arg)
{
  if (thd)
  {
    server_id= thd->variables.server_id;
    when= thd->start_time;
    when_sec_part= thd->start_time_sec_part;
    if (thd->variables.option_bits & OPTION_SKIP_REPLICATION)
      flags|= LOG_EVENT_SKIP_REPLICATION_F;
  }
}

Log_event::Log_event()
  : log_pos(0), temp_buf(0), when(0), when_sec_part(0), exec_time(0),
    data_written(0), server_id(global_system_variables.server_id), flags(0),
    event_owns_temp_buf(false), cache_type(EVENT_INVALID_CACHE),
    slave_exec_mode(SLAVE_EXEC_MODE_STRICT), thd(0)
{
}

Log_event *Log_event::read_log_event(const uchar *, uint, const char **error,
                                     const Format_description_log_event *,
                                     my_bool, my_bool)
{
  if (error)
    *error= "binlog is disabled in MyLite embedded profile";
  return 0;
}

const char *Log_event::get_type_str(Log_event_type type)
{
  switch (type) {
  case START_EVENT_V3: return "Start_v3";
  case STOP_EVENT: return "Stop";
  case QUERY_EVENT: return "Query";
  case ROTATE_EVENT: return "Rotate";
  case INTVAR_EVENT: return "Intvar";
  case LOAD_EVENT: return "Load";
  case NEW_LOAD_EVENT: return "New_load";
  case SLAVE_EVENT: return "Slave";
  case CREATE_FILE_EVENT: return "Create_file";
  case APPEND_BLOCK_EVENT: return "Append_block";
  case DELETE_FILE_EVENT: return "Delete_file";
  case EXEC_LOAD_EVENT: return "Exec_load";
  case RAND_EVENT: return "RAND";
  case XID_EVENT: return "Xid";
  case USER_VAR_EVENT: return "User var";
  case FORMAT_DESCRIPTION_EVENT: return "Format_desc";
  case TABLE_MAP_EVENT: return "Table_map";
  case PRE_GA_WRITE_ROWS_EVENT: return "Write_rows_event_old";
  case PRE_GA_UPDATE_ROWS_EVENT: return "Update_rows_event_old";
  case PRE_GA_DELETE_ROWS_EVENT: return "Delete_rows_event_old";
  case WRITE_ROWS_EVENT_V1: return "Write_rows_v1";
  case UPDATE_ROWS_EVENT_V1: return "Update_rows_v1";
  case DELETE_ROWS_EVENT_V1: return "Delete_rows_v1";
  case WRITE_ROWS_EVENT: return "Write_rows";
  case UPDATE_ROWS_EVENT: return "Update_rows";
  case DELETE_ROWS_EVENT: return "Delete_rows";
  case BEGIN_LOAD_QUERY_EVENT: return "Begin_load_query";
  case EXECUTE_LOAD_QUERY_EVENT: return "Execute_load_query";
  case INCIDENT_EVENT: return "Incident";
  case ANNOTATE_ROWS_EVENT: return "Annotate_rows";
  case BINLOG_CHECKPOINT_EVENT: return "Binlog_checkpoint";
  case GTID_EVENT: return "Gtid";
  case GTID_LIST_EVENT: return "Gtid_list";
  case START_ENCRYPTION_EVENT: return "Start_encryption";
  case IGNORABLE_LOG_EVENT: return "Ignorable log event";
  case ROWS_QUERY_LOG_EVENT: return "MySQL Rows_query";
  case GTID_LOG_EVENT: return "MySQL Gtid";
  case ANONYMOUS_GTID_LOG_EVENT: return "MySQL Anonymous_Gtid";
  case PREVIOUS_GTIDS_LOG_EVENT: return "MySQL Previous_gtids";
  case HEARTBEAT_LOG_EVENT: return "Heartbeat";
  case TRANSACTION_CONTEXT_EVENT: return "Transaction_context";
  case VIEW_CHANGE_EVENT: return "View_change";
  case XA_PREPARE_LOG_EVENT: return "XA_prepare";
  case PARTIAL_UPDATE_ROWS_EVENT: return "MySQL Update_rows_partial";
  case TRANSACTION_PAYLOAD_EVENT: return "MySQL Transaction_payload";
  case HEARTBEAT_LOG_EVENT_V2: return "MySQL Heartbeat";
  case QUERY_COMPRESSED_EVENT: return "Query_compressed";
  case WRITE_ROWS_COMPRESSED_EVENT: return "Write_rows_compressed";
  case UPDATE_ROWS_COMPRESSED_EVENT: return "Update_rows_compressed";
  case DELETE_ROWS_COMPRESSED_EVENT: return "Delete_rows_compressed";
  case WRITE_ROWS_COMPRESSED_EVENT_V1: return "Write_rows_compressed_v1";
  case UPDATE_ROWS_COMPRESSED_EVENT_V1: return "Update_rows_compressed_v1";
  case DELETE_ROWS_COMPRESSED_EVENT_V1: return "Delete_rows_compressed_v1";
  default: return "Unknown";
  }
}

const char *Log_event::get_type_str()
{
  return get_type_str(get_type_code());
}

Format_description_log_event::Format_description_log_event(
    uint8 binlog_ver, const char *server_ver,
    enum_binlog_checksum_alg checksum_alg)
  : Log_event(), created(0), binlog_version(binlog_ver),
    dont_set_created(false), common_header_len(LOG_EVENT_HEADER_LEN),
    number_of_event_types(0), post_header_len(0), event_type_permutation(0),
    options_written_to_bin_log(0), used_checksum_alg(checksum_alg)
{
  server_version[0]= '\0';
  if (server_ver)
    strmake(server_version, server_ver, sizeof(server_version) - 1);
  reset_crypto();
}

bool Format_description_log_event::write(Log_event_writer *)
{
  return true;
}

bool Log_event::write_header(Log_event_writer *, size_t)
{
  return true;
}

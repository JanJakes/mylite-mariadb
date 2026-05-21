/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"

#include "rpl_gtid.h"
#include "sql_string.h"

#define MYLITE_GTID_STATE_UNSUPPORTED 1

const LEX_CSTRING rpl_gtid_slave_state_table_name=
  { STRING_WITH_LEN("gtid_slave_pos") };

int rpl_binlog_state_base::element::update_element(const rpl_gtid *)
{
  return MYLITE_GTID_STATE_UNSUPPORTED;
}

rpl_binlog_state_base::~rpl_binlog_state_base()
{
  free();
}

void rpl_binlog_state_base::init()
{
  initialized= 1;
}

void rpl_binlog_state_base::reset_nolock()
{
}

void rpl_binlog_state_base::free()
{
  initialized= 0;
}

bool rpl_binlog_state_base::load_nolock(rpl_gtid *, uint32)
{
  reset_nolock();
  return false;
}

bool rpl_binlog_state_base::load_nolock(rpl_binlog_state_base *)
{
  reset_nolock();
  return false;
}

int rpl_binlog_state_base::update_nolock(const rpl_gtid *)
{
  return MYLITE_GTID_STATE_UNSUPPORTED;
}

int rpl_binlog_state_base::alloc_element_nolock(const rpl_gtid *)
{
  return MYLITE_GTID_STATE_UNSUPPORTED;
}

uint32 rpl_binlog_state_base::count_nolock()
{
  return 0;
}

int rpl_binlog_state_base::get_gtid_list_nolock(rpl_gtid *, uint32)
{
  return 0;
}

rpl_gtid *rpl_binlog_state_base::find_nolock(uint32, uint32)
{
  return NULL;
}

bool rpl_binlog_state_base::is_before_pos(slave_connection_state *)
{
  return true;
}

rpl_binlog_state::~rpl_binlog_state()
{
  free();
}

void rpl_binlog_state::init()
{
  rpl_binlog_state_base::init();
}

void rpl_binlog_state::reset()
{
}

void rpl_binlog_state::free()
{
  rpl_binlog_state_base::free();
}

bool rpl_binlog_state::load(rpl_gtid *, uint32)
{
  reset();
  return false;
}

bool rpl_binlog_state::load(rpl_slave_state *)
{
  reset();
  return false;
}

int rpl_binlog_state::update(const rpl_gtid *, bool)
{
  return MYLITE_GTID_STATE_UNSUPPORTED;
}

int rpl_binlog_state::update_with_next_gtid(uint32 domain_id,
                                            uint32 server_id,
                                            rpl_gtid *gtid)
{
  if (gtid)
  {
    gtid->domain_id= domain_id;
    gtid->server_id= server_id;
    gtid->seq_no= 0;
  }
  return MYLITE_GTID_STATE_UNSUPPORTED;
}

bool rpl_binlog_state::check_strict_sequence(uint32, uint32, uint64, bool)
{
  return true;
}

int rpl_binlog_state::bump_seq_no_if_needed(uint32, uint64)
{
  return MYLITE_GTID_STATE_UNSUPPORTED;
}

int rpl_binlog_state::write_to_iocache(IO_CACHE *)
{
  return MYLITE_GTID_STATE_UNSUPPORTED;
}

int rpl_binlog_state::read_from_iocache(IO_CACHE *)
{
  return MYLITE_GTID_STATE_UNSUPPORTED;
}

uint32 rpl_binlog_state::count()
{
  return 0;
}

int rpl_binlog_state::get_gtid_list(rpl_gtid *, uint32)
{
  return 0;
}

int rpl_binlog_state::get_most_recent_gtid_list(rpl_gtid **list,
                                                uint32 *size)
{
  if (list)
    *list= NULL;
  if (size)
    *size= 0;
  return 0;
}

bool rpl_binlog_state::append_pos(String *)
{
  return false;
}

bool rpl_binlog_state::append_state(String *)
{
  return false;
}

rpl_gtid *rpl_binlog_state::find(uint32, uint32)
{
  return NULL;
}

rpl_gtid *rpl_binlog_state::find_most_recent(uint32)
{
  return NULL;
}

const char *rpl_binlog_state::drop_domain(DYNAMIC_ARRAY *,
                                          Gtid_list_log_event *,
                                          char *)
{
  return "";
}

bool rpl_slave_state_tostring_helper(String *, const rpl_gtid *, bool *)
{
  return false;
}

int gtid_check_rpl_slave_state_table(TABLE *)
{
  return MYLITE_GTID_STATE_UNSUPPORTED;
}

rpl_gtid *gtid_parse_string_to_list(const char *, size_t, uint32 *out_len)
{
  if (out_len)
    *out_len= 0;
  return NULL;
}

rpl_gtid *gtid_unpack_string_to_list(const char *, size_t, uint32 *out_len)
{
  if (out_len)
    *out_len= 0;
  return NULL;
}

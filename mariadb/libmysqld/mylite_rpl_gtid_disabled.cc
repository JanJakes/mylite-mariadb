/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"

#include "rpl_gtid.h"

rpl_binlog_state_base::~rpl_binlog_state_base()
{
}

void rpl_binlog_state_base::init()
{
  initialized= 0;
}

void rpl_binlog_state_base::reset_nolock()
{
}

void rpl_binlog_state_base::free()
{
  initialized= 0;
}

rpl_binlog_state::~rpl_binlog_state()
{
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
  return false;
}

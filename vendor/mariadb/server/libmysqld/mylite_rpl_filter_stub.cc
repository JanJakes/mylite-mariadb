/* Copyright (c) 2026 MyLite contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include "sql_priv.h"
#include "rpl_filter.h"

Rpl_filter::Rpl_filter() :
  parallel_mode(SLAVE_PARALLEL_OPTIMISTIC),
  table_rules_on(0),
  do_table_inited(0), ignore_table_inited(0),
  wild_do_table_inited(0), wild_ignore_table_inited(0)
{
  do_db.empty();
  ignore_db.empty();
  rewrite_db.empty();
}


Rpl_filter::~Rpl_filter()
{
}


bool
Rpl_filter::db_ok(const char *db)
{
  (void) db;
  return true;
}

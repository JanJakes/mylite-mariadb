/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
#include "mysqld.h"
#include "rpl_filter.h"
#include "sql_string.h"

static int mylite_disabled_filter_set(const char *spec);

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

#ifndef MYSQL_CLIENT
bool Rpl_filter::tables_ok(const char *, TABLE_LIST *)
{
  return true;
}
#endif

bool Rpl_filter::db_ok(const char *)
{
  return true;
}

bool Rpl_filter::db_ok_with_wild_table(const char *)
{
  return true;
}

bool Rpl_filter::is_on()
{
  return false;
}

bool Rpl_filter::is_db_empty()
{
  return true;
}

int Rpl_filter::add_do_table(const char *table_spec)
{
  return mylite_disabled_filter_set(table_spec);
}

int Rpl_filter::add_ignore_table(const char *table_spec)
{
  return mylite_disabled_filter_set(table_spec);
}

int Rpl_filter::set_do_table(const char *table_spec)
{
  return mylite_disabled_filter_set(table_spec);
}

int Rpl_filter::set_ignore_table(const char *table_spec)
{
  return mylite_disabled_filter_set(table_spec);
}

int Rpl_filter::add_wild_do_table(const char *table_spec)
{
  return mylite_disabled_filter_set(table_spec);
}

int Rpl_filter::add_wild_ignore_table(const char *table_spec)
{
  return mylite_disabled_filter_set(table_spec);
}

int Rpl_filter::set_wild_do_table(const char *table_spec)
{
  return mylite_disabled_filter_set(table_spec);
}

int Rpl_filter::set_wild_ignore_table(const char *table_spec)
{
  return mylite_disabled_filter_set(table_spec);
}

int Rpl_filter::add_rewrite_db(const char *db_spec)
{
  return mylite_disabled_filter_set(db_spec);
}

int Rpl_filter::add_do_db(const char *db_spec)
{
  return mylite_disabled_filter_set(db_spec);
}

int Rpl_filter::add_ignore_db(const char *db_spec)
{
  return mylite_disabled_filter_set(db_spec);
}

int Rpl_filter::set_rewrite_db(const char *db_spec)
{
  return mylite_disabled_filter_set(db_spec);
}

int Rpl_filter::set_do_db(const char *db_spec)
{
  return mylite_disabled_filter_set(db_spec);
}

int Rpl_filter::set_ignore_db(const char *db_spec)
{
  return mylite_disabled_filter_set(db_spec);
}

void Rpl_filter::get_do_table(String *str)
{
  str->length(0);
}

void Rpl_filter::get_ignore_table(String *str)
{
  str->length(0);
}

void Rpl_filter::get_wild_do_table(String *str)
{
  str->length(0);
}

void Rpl_filter::get_wild_ignore_table(String *str)
{
  str->length(0);
}

bool Rpl_filter::rewrite_db_is_empty()
{
  return true;
}

I_List<i_string_pair> *Rpl_filter::get_rewrite_db()
{
  return &rewrite_db;
}

void Rpl_filter::get_rewrite_db(String *str)
{
  str->length(0);
}

const char *Rpl_filter::get_rewrite_db(const char *db, size_t *)
{
  return db;
}

I_List<i_string> *Rpl_filter::get_do_db()
{
  return &do_db;
}

I_List<i_string> *Rpl_filter::get_ignore_db()
{
  return &ignore_db;
}

void Rpl_filter::get_do_db(String *str)
{
  str->length(0);
}

void Rpl_filter::get_ignore_db(String *str)
{
  str->length(0);
}

static int mylite_disabled_filter_set(const char *spec)
{
  return spec && spec[0] ? 1 : 0;
}

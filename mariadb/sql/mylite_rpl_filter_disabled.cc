/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"

#include "rpl_filter.h"
#include "sql_string.h"

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

int Rpl_filter::add_do_table(const char *)
{
  return 0;
}

int Rpl_filter::add_ignore_table(const char *)
{
  return 0;
}

int Rpl_filter::set_do_table(const char *)
{
  return 0;
}

int Rpl_filter::set_ignore_table(const char *)
{
  return 0;
}

int Rpl_filter::add_wild_do_table(const char *)
{
  return 0;
}

int Rpl_filter::add_wild_ignore_table(const char *)
{
  return 0;
}

int Rpl_filter::set_wild_do_table(const char *)
{
  return 0;
}

int Rpl_filter::set_wild_ignore_table(const char *)
{
  return 0;
}

int Rpl_filter::add_rewrite_db(const char *)
{
  return 0;
}

int Rpl_filter::add_do_db(const char *)
{
  return 0;
}

int Rpl_filter::add_ignore_db(const char *)
{
  return 0;
}

int Rpl_filter::set_rewrite_db(const char *)
{
  return 0;
}

int Rpl_filter::set_do_db(const char *)
{
  return 0;
}

int Rpl_filter::set_ignore_db(const char *)
{
  return 0;
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

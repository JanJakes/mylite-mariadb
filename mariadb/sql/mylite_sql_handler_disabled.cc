/*
  MyLite embedded profile stub for SQL HANDLER commands.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_handler.h"
#include "mysqld_error.h"

static void mylite_sql_handler_not_supported();

void SQL_HANDLER::reset()
{
  fields.empty();
  arena.free_items();
  free_root(&mem_root, MYF(0));
  my_free(lock);
  init();
}

SQL_HANDLER::~SQL_HANDLER()
{
  reset();
  my_free(base_data);
}

bool mysql_ha_open(THD *, TABLE_LIST *, SQL_HANDLER *)
{
  mylite_sql_handler_not_supported();
  return true;
}

bool mysql_ha_close(THD *, TABLE_LIST *)
{
  mylite_sql_handler_not_supported();
  return true;
}

bool mysql_ha_read(THD *, TABLE_LIST *, enum enum_ha_read_modes,
                   const char *, List<Item> *, enum ha_rkey_function,
                   Item *, ha_rows, ha_rows)
{
  mylite_sql_handler_not_supported();
  return true;
}

SQL_HANDLER *mysql_ha_read_prepare(THD *, TABLE_LIST *,
                                   enum enum_ha_read_modes,
                                   const char *, List<Item> *,
                                   enum ha_rkey_function, Item *)
{
  mylite_sql_handler_not_supported();
  return NULL;
}

void mysql_ha_flush(THD *)
{
}

void mysql_ha_flush_tables(THD *, TABLE_LIST *)
{
}

void mysql_ha_rm_tables(THD *, TABLE_LIST *)
{
}

void mysql_ha_cleanup_no_free(THD *)
{
}

void mysql_ha_cleanup(THD *thd)
{
  my_hash_free(&thd->handler_tables_hash);
}

void mysql_ha_set_explicit_lock_duration(THD *)
{
}

void mysql_ha_rm_temporary_tables(THD *)
{
}

static void mylite_sql_handler_not_supported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "SQL HANDLER in the MyLite embedded profile");
}

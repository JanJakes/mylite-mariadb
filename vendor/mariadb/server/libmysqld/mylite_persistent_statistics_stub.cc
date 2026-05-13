/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "table.h"
#include "sql_statistics.h"
#include "sql_alter.h"

int read_statistics_for_tables_if_needed(THD *, TABLE_LIST *)
{
  return 0;
}

int read_statistics_for_tables(THD *, TABLE_LIST *, bool)
{
  return 0;
}

void set_statistics_for_table(THD *, TABLE *table)
{
  if (!table)
    return;

  table->used_stat_records= table->file ? table->file->stats.records : 0;
  for (KEY *key_info= table->key_info;
       key_info < table->key_info + table->s->keys; ++key_info)
  {
    key_info->all_nulls_key_parts= 0;
    key_info->is_statistics_from_stat_tables= false;
  }
}

int delete_statistics_for_table(THD *, const LEX_CSTRING *,
                                const LEX_CSTRING *)
{
  return 0;
}

int delete_statistics_for_column(THD *, TABLE *, Field *)
{
  return 0;
}

int delete_statistics_for_index(THD *, TABLE *, KEY *, bool)
{
  return 0;
}

int rename_table_in_stat_tables(THD *, const LEX_CSTRING *,
                                const LEX_CSTRING *, const LEX_CSTRING *,
                                const LEX_CSTRING *)
{
  return 0;
}

int rename_columns_in_stat_table(
    THD *, TABLE *, List<Alter_info::RENAME_COLUMN_STAT_PARAMS> *)
{
  return 0;
}

int rename_indexes_in_stat_table(
    THD *, TABLE *, List<Alter_info::RENAME_INDEX_STAT_PARAMS> *)
{
  return 0;
}

bool is_eits_usable(Field *)
{
  return false;
}

double get_column_avg_frequency(Field *field)
{
  return field && field->table ?
         static_cast<double>(field->table->stat_records()) : 0.0;
}

double get_column_range_cardinality(Field *field, key_range *, key_range *,
                                    uint)
{
  return field && field->table ?
         static_cast<double>(field->table->stat_records()) : 0.0;
}

TABLE_STATISTICS_CB::~TABLE_STATISTICS_CB()
{
}

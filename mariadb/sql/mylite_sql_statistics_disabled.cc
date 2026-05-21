/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "field.h"
#include "key.h"
#include "opt_range.h"
#include "sql_base.h"
#include "sql_alter.h"
#include "sql_class.h"
#include "sql_const.h"
#include "sql_partition.h"
#include "sql_show.h"
#include "sql_statistics.h"
#include "table.h"
#include "uniques.h"

static const uint STATISTICS_TABLES= 3;

static const Lex_ident_table stat_table_name[STATISTICS_TABLES]=
{
  "table_stats"_Lex_ident_table,
  "column_stats"_Lex_ident_table,
  "index_stats"_Lex_ident_table,
};

static void clear_statistics_from_table(TABLE *table);

TABLE_STATISTICS_CB::TABLE_STATISTICS_CB():
  usage_count(0), table_stats(0),
  stats_available(TABLE_STAT_NO_STATS), histograms_exists_on_disk(0)
{
  init_sql_alloc(PSI_INSTRUMENT_ME, &mem_root, TABLE_ALLOC_BLOCK_SIZE, 0,
                 MYF(0));
}

TABLE_STATISTICS_CB::~TABLE_STATISTICS_CB()
{
  DBUG_ASSERT(usage_count == 0);
  free_root(&mem_root, MYF(0));
}

void TABLE_STATISTICS_CB::update_stats_in_table(TABLE *table)
{
  clear_statistics_from_table(table);
}

int read_statistics_for_tables_if_needed(THD *, TABLE_LIST *)
{
  return 0;
}

int read_statistics_for_tables(THD *, TABLE_LIST *, bool)
{
  return 0;
}

int collect_statistics_for_table(THD *, TABLE *)
{
  return 1;
}

int alloc_statistics_for_table(THD *, TABLE *, MY_BITMAP *)
{
  return 1;
}

void free_statistics_for_table(TABLE *)
{
}

int update_statistics_for_table(THD *, TABLE *)
{
  return 1;
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

int rename_table_in_stat_tables(THD *, const LEX_CSTRING *, const LEX_CSTRING *,
                                const LEX_CSTRING *, const LEX_CSTRING *)
{
  return 0;
}

int rename_columns_in_stat_table(THD *, TABLE *,
                                 List<Alter_info::RENAME_COLUMN_STAT_PARAMS> *)
{
  return 0;
}

int rename_indexes_in_stat_table(THD *, TABLE *,
                                 List<Alter_info::RENAME_INDEX_STAT_PARAMS> *)
{
  return 0;
}

void set_statistics_for_table(THD *, TABLE *table)
{
  if (!table || !table->file)
    return;

  table->used_stat_records= table->file->stats.records;
  clear_statistics_from_table(table);
}

double get_column_avg_frequency(Field *field)
{
  return field && field->table ? (double) field->table->stat_records() : 0.0;
}

double get_column_range_cardinality(Field *field, key_range *, key_range *,
                                    uint)
{
  return field && field->table ? (double) field->table->stat_records() : 0.0;
}

bool is_stat_table(const Lex_ident_db &db, const Lex_ident_table &table)
{
  DBUG_ASSERT(db.str && table.str);

  if (db.streq(MYSQL_SCHEMA_NAME))
  {
    for (uint i= 0; i < STATISTICS_TABLES; i++)
    {
      if (table.streq(stat_table_name[i]))
        return true;
    }
  }
  return false;
}

bool is_eits_usable(Field *)
{
  return false;
}

static void clear_statistics_from_table(TABLE *table)
{
  if (!table)
    return;

  table->stats_is_read= false;
  if (table->field)
  {
    for (Field **field_ptr= table->field; *field_ptr; field_ptr++)
      (*field_ptr)->read_stats= NULL;
  }

  if (!table->s || !table->key_info)
    return;

  KEY *key_info= table->key_info;
  KEY *key_info_end= key_info + table->s->keys;
  for (; key_info < key_info_end; key_info++)
  {
    key_info->read_stats= NULL;
    key_info->is_statistics_from_stat_tables= false;
    key_info->all_nulls_key_parts= 0;
  }
}

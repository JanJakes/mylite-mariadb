/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_cache.h"
#include "mysqld.h"

extern "C"
{
const uchar *query_cache_query_get_key(const void *, size_t *length, my_bool)
{
  *length= 0;
  return nullptr;
}

const uchar *query_cache_table_get_key(const void *, size_t *length, my_bool)
{
  *length= 0;
  return nullptr;
}

void query_cache_invalidate_by_MyISAM_filename(const char *)
{
}

void mysql_query_cache_invalidate4(THD *, const char *, unsigned, int)
{
}
}

void query_cache_insert(void *, const char *, size_t, unsigned)
{
}

Query_cache::Query_cache(size_t query_cache_limit_arg,
                         size_t min_allocation_unit_arg,
                         size_t min_result_data_size_arg,
                         uint def_query_hash_size_arg,
                         uint def_table_hash_size_arg)
  :query_cache_size(0),
   query_cache_limit(query_cache_limit_arg),
   free_memory(0),
   queries_in_cache(0), hits(0), inserts(0), refused(0),
   free_memory_blocks(0), total_blocks(0), lowmem_prunes(0),
#ifndef DBUG_OFF
   m_cache_lock_thread_id(0),
#endif
   m_requests_in_progress(0),
   m_cache_lock_status(UNLOCKED),
   m_cache_status(DISABLED),
   additional_data_size(0),
   cache(nullptr),
   first_block(nullptr),
   queries_blocks(nullptr),
   tables_blocks(nullptr),
   bins(nullptr),
   steps(nullptr),
   min_allocation_unit(ALIGN_SIZE(min_allocation_unit_arg)),
   min_result_data_size(ALIGN_SIZE(min_result_data_size_arg)),
   def_query_hash_size(def_query_hash_size_arg),
   def_table_hash_size(def_table_hash_size_arg),
   mem_bin_num(0),
   mem_bin_steps(0),
   initialized(false)
{
}

void Query_cache::init()
{
  m_cache_status= DISABLED;
  initialized= true;
}

size_t Query_cache::resize(size_t)
{
  query_cache_size= 0;
  ::query_cache_size= 0;
  free_memory= 0;
  queries_in_cache= 0;
  hits= 0;
  inserts= 0;
  refused= 0;
  free_memory_blocks= 0;
  total_blocks= 0;
  lowmem_prunes= 0;
  m_cache_status= DISABLED;
  return 0;
}

size_t Query_cache::set_min_res_unit(size_t size)
{
  if (size == 0)
    size= QUERY_CACHE_MIN_RESULT_DATA_SIZE;
  min_result_data_size= ALIGN_SIZE(size);
  return min_result_data_size;
}

void Query_cache::store_query(THD *, TABLE_LIST *)
{
}

int Query_cache::send_result_to_client(THD *, char *, uint)
{
  return 0;
}

void Query_cache::invalidate(THD *, TABLE_LIST *, my_bool)
{
}

void Query_cache::invalidate(THD *, CHANGED_TABLE_LIST *)
{
}

void Query_cache::invalidate_locked_for_write(THD *, TABLE_LIST *)
{
}

void Query_cache::invalidate(THD *, TABLE *, my_bool)
{
}

void Query_cache::invalidate(THD *, const char *, size_t, my_bool)
{
}

void Query_cache::invalidate(THD *, const LEX_CSTRING &)
{
}

void Query_cache::invalidate_by_MyISAM_filename(const char *)
{
}

void Query_cache::flush()
{
}

void Query_cache::pack(THD *, size_t, uint)
{
}

void Query_cache::destroy()
{
  initialized= false;
}

void Query_cache::insert(THD *, Query_cache_tls *query_cache_tls,
                         const char *, size_t, unsigned)
{
  if (query_cache_tls)
    query_cache_tls->first_query_block= nullptr;
}

my_bool Query_cache::insert_table(THD *, size_t, const char *,
                                  Query_cache_block_table *, size_t, uint8,
                                  uint8, qc_engine_callback, ulonglong,
                                  my_bool)
{
  return TRUE;
}

void Query_cache::end_of_result(THD *thd)
{
  if (thd)
    thd->query_cache_tls.first_query_block= nullptr;
}

void Query_cache::abort(THD *, Query_cache_tls *query_cache_tls)
{
  if (query_cache_tls)
    query_cache_tls->first_query_block= nullptr;
}

void Query_cache::wreck(uint, const char *)
{
}

void Query_cache::bins_dump()
{
}

void Query_cache::cache_dump()
{
}

void Query_cache::queries_dump()
{
}

void Query_cache::tables_dump()
{
}

my_bool Query_cache::check_integrity(bool)
{
  return FALSE;
}

my_bool Query_cache::in_list(Query_cache_block *, Query_cache_block *,
                             const char *)
{
  return FALSE;
}

my_bool Query_cache::in_table_list(Query_cache_block_table *,
                                   Query_cache_block_table *, const char *)
{
  return FALSE;
}

my_bool Query_cache::in_blocks(Query_cache_block *)
{
  return FALSE;
}

uint Query_cache::filename_2_table_key(char *, const char *, uint32 *db_length)
{
  if (db_length)
    *db_length= 0;
  return 0;
}

bool Query_cache::try_lock(THD *, Cache_try_lock_mode)
{
  return true;
}

void Query_cache::lock(THD *)
{
}

void Query_cache::lock_and_suspend()
{
}

void Query_cache::unlock()
{
}

void Query_cache::disable_query_cache(THD *)
{
  m_cache_status= DISABLED;
}

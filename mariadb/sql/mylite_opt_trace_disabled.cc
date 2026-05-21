/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "field.h"
#include "item.h"
#include "opt_trace.h"
#include "sql_class.h"
#include "sql_show.h"

namespace Show {

ST_FIELD_INFO optimizer_trace_info[]=
{
  Column("QUERY",                             Longtext(65535), NOT_NULL),
  Column("TRACE",                             Longtext(65535), NOT_NULL),
  Column("MISSING_BYTES_BEYOND_MAX_MEM_SIZE", SLong(20),       NOT_NULL),
  Column("INSUFFICIENT_PRIVILEGES",           STiny(1),        NOT_NULL),
  CEnd()
};

} // namespace Show

const char *Opt_trace_context::flag_names[]= {"enabled", "default", NullS};

void opt_trace_print_expanded_query(THD *, SELECT_LEX *, Json_writer_object *)
{
}

void opt_trace_disable_if_no_security_context_access(THD *)
{
}

void opt_trace_disable_if_no_stored_proc_func_access(THD *, sp_head *)
{
}

void opt_trace_disable_if_no_tables_access(THD *, TABLE_LIST *)
{
}

void opt_trace_disable_if_no_view_access(THD *, TABLE_LIST *, TABLE_LIST *)
{
}

Opt_trace_stmt::Opt_trace_stmt(Opt_trace_context *ctx_arg) : ctx(ctx_arg)
{
  current_json= NULL;
  missing_priv= false;
  I_S_disabled= 0;
}

Opt_trace_stmt::~Opt_trace_stmt()
{
}

void Opt_trace_stmt::set_query(const char *, size_t, const CHARSET_INFO *)
{
}

void Opt_trace_stmt::fill_info(Opt_trace_info *info)
{
  info->trace_ptr= "";
  info->trace_length= 0;
  info->query_ptr= "";
  info->query_length= 0;
  info->query_charset= system_charset_info;
  info->missing_bytes= 0;
  info->missing_priv= false;
}

void Opt_trace_stmt::missing_privilege()
{
}

void Opt_trace_stmt::disable_tracing_for_children()
{
}

void Opt_trace_stmt::enable_tracing_for_children()
{
}

void Opt_trace_stmt::set_allowed_mem_size(size_t)
{
}

size_t Opt_trace_stmt::get_length()
{
  return 0;
}

size_t Opt_trace_stmt::get_truncated_bytes()
{
  return 0;
}

void Opt_trace_context::missing_privilege()
{
}

void Opt_trace_context::set_allowed_mem_size(size_t)
{
}

size_t Opt_trace_context::remaining_mem_size()
{
  return 0;
}

bool Opt_trace_context::disable_tracing_if_required()
{
  return false;
}

bool Opt_trace_context::enable_tracing_if_required()
{
  return false;
}

bool Opt_trace_context::is_enabled()
{
  return false;
}

Opt_trace_context::Opt_trace_context() : traces(PSI_INSTRUMENT_MEM)
{
  current_trace= NULL;
  max_mem_size= 0;
}

Opt_trace_context::~Opt_trace_context()
{
  delete_traces();
}

void Opt_trace_context::set_query(const char *, size_t, const CHARSET_INFO *)
{
}

void Opt_trace_context::start(THD *, TABLE_LIST *, enum enum_sql_command,
                              const char *, size_t, const CHARSET_INFO *, ulong)
{
}

void Opt_trace_context::end()
{
  current_trace= NULL;
}

void Opt_trace_start::init(THD *, TABLE_LIST *, enum enum_sql_command,
                           List<set_var_base> *, const char *, size_t,
                           const CHARSET_INFO *)
{
  traceable= false;
}

Opt_trace_start::~Opt_trace_start()
{
}

void Json_writer::add_table_name(const JOIN_TAB *)
{
  add_null();
}

void Json_writer::add_table_name(const TABLE *table)
{
  if (table && table->pos_in_table_list)
    add_str(table->pos_in_table_list->alias.str);
  else
    add_null();
}

void trace_condition(THD *, const char *, const char *, Item *, const char *)
{
}

void add_table_scan_values_to_trace(THD *, JOIN_TAB *)
{
}

void trace_plan_prefix(Json_writer_object *, JOIN *, uint, table_map)
{
}

void print_final_join_order(JOIN *)
{
}

void print_best_access_for_table(THD *, POSITION *)
{
}

void Json_writer::add_str(Item *item)
{
  if (item)
  {
    THD *thd= current_thd;
    StringBuffer<256> str(system_charset_info);

    ulonglong save_option_bits= thd->variables.option_bits;
    thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;
    item->print(&str,
                enum_query_type(QT_TO_SYSTEM_CHARSET | QT_SHOW_SELECT_NUMBER |
                                QT_ITEM_IDENT_SKIP_DB_NAMES));
    thd->variables.option_bits= save_option_bits;
    add_str(str.c_ptr_safe());
  }
  else
    add_null();
}

void Opt_trace_context::delete_traces()
{
  while (traces.elements())
  {
    Opt_trace_stmt *prev= traces.at(0);
    delete prev;
    traces.del(0);
  }
  current_trace= NULL;
}

int fill_optimizer_trace_info(THD *, TABLE_LIST *, Item *)
{
  return 0;
}

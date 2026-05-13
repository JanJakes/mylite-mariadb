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
#include "sql_class.h"
#include "opt_trace.h"
#include "field.h"
#include "item.h"
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

void
opt_trace_print_expanded_query(THD *thd, SELECT_LEX *select_lex,
                               Json_writer_object *trace_object)
{
  (void) thd;
  (void) select_lex;
  (void) trace_object;
}


void
opt_trace_disable_if_no_security_context_access(THD *thd)
{
  (void) thd;
}


void
opt_trace_disable_if_no_stored_proc_func_access(THD *thd, sp_head *sp)
{
  (void) thd;
  (void) sp;
}


void
opt_trace_disable_if_no_tables_access(THD *thd, TABLE_LIST *tbl)
{
  (void) thd;
  (void) tbl;
}


void
opt_trace_disable_if_no_view_access(THD *thd, TABLE_LIST *view,
                                    TABLE_LIST *underlying_tables)
{
  (void) thd;
  (void) view;
  (void) underlying_tables;
}


Opt_trace_stmt::Opt_trace_stmt(Opt_trace_context *ctx_arg) :
  ctx(ctx_arg),
  current_json(nullptr),
  missing_priv(false),
  I_S_disabled(0)
{
}


Opt_trace_stmt::~Opt_trace_stmt()
{
}


size_t
Opt_trace_stmt::get_length()
{
  return 0;
}


size_t
Opt_trace_stmt::get_truncated_bytes()
{
  return 0;
}


void
Opt_trace_stmt::set_query(const char *query_ptr, size_t length,
                          const CHARSET_INFO *charset)
{
  (void) query_ptr;
  (void) length;
  (void) charset;
}


void
Opt_trace_context::missing_privilege()
{
}


void
Opt_trace_context::set_allowed_mem_size(size_t mem_size)
{
  (void) mem_size;
}


size_t
Opt_trace_context::remaining_mem_size()
{
  return 0;
}


bool
Opt_trace_context::disable_tracing_if_required()
{
  return false;
}


bool
Opt_trace_context::enable_tracing_if_required()
{
  return false;
}


bool
Opt_trace_context::is_enabled()
{
  return false;
}


Opt_trace_context::Opt_trace_context() : traces(PSI_INSTRUMENT_MEM)
{
  current_trace= nullptr;
  max_mem_size= 0;
}


Opt_trace_context::~Opt_trace_context()
{
  delete_traces();
}


void
Opt_trace_context::set_query(const char *query, size_t length,
                             const CHARSET_INFO *charset)
{
  (void) query;
  (void) length;
  (void) charset;
}


void
Opt_trace_context::start(THD *thd, TABLE_LIST *tbl,
                         enum enum_sql_command sql_command,
                         const char *query,
                         size_t query_length,
                         const CHARSET_INFO *query_charset,
                         ulong max_mem_size_arg)
{
  (void) thd;
  (void) tbl;
  (void) sql_command;
  (void) query;
  (void) query_length;
  (void) query_charset;
  (void) max_mem_size_arg;
}


void
Opt_trace_context::end()
{
}


void
Opt_trace_start::init(THD *thd,
                      TABLE_LIST *tbl,
                      enum enum_sql_command sql_command,
                      List<set_var_base> *set_vars,
                      const char *query,
                      size_t query_length,
                      const CHARSET_INFO *query_charset)
{
  (void) thd;
  (void) tbl;
  (void) sql_command;
  (void) set_vars;
  (void) query;
  (void) query_length;
  (void) query_charset;
  traceable= false;
}


Opt_trace_start::~Opt_trace_start()
{
}


void
Opt_trace_stmt::fill_info(Opt_trace_info* info)
{
  info->trace_ptr= "";
  info->trace_length= 0;
  info->query_ptr= "";
  info->query_length= 0;
  info->query_charset= &my_charset_bin;
  info->missing_bytes= 0;
  info->missing_priv= false;
}


void
Opt_trace_stmt::missing_privilege()
{
}


void
Opt_trace_stmt::disable_tracing_for_children()
{
}


void
Opt_trace_stmt::enable_tracing_for_children()
{
}


void
Opt_trace_stmt::set_allowed_mem_size(size_t mem_size)
{
  (void) mem_size;
}


void
Json_writer::add_table_name(const JOIN_TAB *tab)
{
  char table_name_buffer[64];
  String str;

  if (tab->table && tab->table->derived_select_number)
  {
    size_t len= my_snprintf(table_name_buffer, sizeof(table_name_buffer) - 1,
                            "<derived%u>",
                            tab->table->derived_select_number);
    str.copy(table_name_buffer, len, &my_charset_bin);
  }
  else if (tab->bush_children)
  {
    JOIN_TAB *ctab= tab->bush_children->start;
    size_t len= my_snprintf(table_name_buffer,
                            sizeof(table_name_buffer) - 1,
                            "<subquery%d>",
                            ctab->emb_sj_nest->sj_subq_pred->get_identifier());
    str.copy(table_name_buffer, len, &my_charset_bin);
  }
  else
  {
    TABLE_LIST *real_table= tab->table->pos_in_table_list;
    str.set(real_table->alias.str, real_table->alias.length, &my_charset_bin);
  }
  add_str(str.ptr(), str.length());
}


void
Json_writer::add_table_name(const TABLE *table)
{
  add_str(table->pos_in_table_list->alias.str);
}


void
trace_condition(THD *thd, const char *name, const char *transform_type,
                Item *item, const char *table_name)
{
  (void) thd;
  (void) name;
  (void) transform_type;
  (void) item;
  (void) table_name;
}


void
add_table_scan_values_to_trace(THD *thd, JOIN_TAB *tab)
{
  (void) thd;
  (void) tab;
}


void
trace_plan_prefix(Json_writer_object *jsobj, JOIN *join, uint idx,
                  table_map join_tables)
{
  (void) jsobj;
  (void) join;
  (void) idx;
  (void) join_tables;
}


void
print_final_join_order(JOIN *join)
{
  (void) join;
}


void
print_best_access_for_table(THD *thd, POSITION *pos)
{
  (void) thd;
  (void) pos;
}


void
Json_writer::add_str(Item *item)
{
  if (!item)
  {
    add_null();
    return;
  }

  THD *thd= current_thd;
  StringBuffer<256> str(system_charset_info);
  ulonglong save_option_bits= thd->variables.option_bits;
  thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;
  item->print(&str, enum_query_type(QT_TO_SYSTEM_CHARSET |
                                    QT_SHOW_SELECT_NUMBER |
                                    QT_ITEM_IDENT_SKIP_DB_NAMES));
  thd->variables.option_bits= save_option_bits;
  add_str(str.c_ptr_safe());
}


void
Opt_trace_context::delete_traces()
{
  while (traces.elements())
  {
    Opt_trace_stmt *prev= traces.at(0);
    delete prev;
    traces.del(0);
  }
  current_trace= nullptr;
}


int
fill_optimizer_trace_info(THD *thd, TABLE_LIST *tables, Item *cond)
{
  (void) thd;
  (void) tables;
  (void) cond;
  return 0;
}

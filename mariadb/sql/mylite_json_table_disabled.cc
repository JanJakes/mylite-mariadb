/*
  MyLite embedded profile stub for JSON_TABLE table-function execution.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "table.h"
#include "sql_type_json.h"
#include "item_jsonfunc.h"
#include "json_table.h"
#include "mysqld_error.h"

static void mylite_json_table_not_supported();

bool Table_function_json_table::setup(THD *, TABLE_LIST *, SELECT_LEX *)
{
  mylite_json_table_not_supported();
  return TRUE;
}

int Table_function_json_table::walk_items(Item_processor processor,
                                          bool walk_subquery, void *argument)
{
  return m_json ? m_json->walk(processor, walk_subquery, argument) : 0;
}

void Table_function_json_table::fix_after_pullout(TABLE_LIST *sql_table,
       st_select_lex *, bool)
{
  sql_table->dep_tables= used_tables();
}

void Table_function_json_table::get_estimates(ha_rows *out_rows,
                                              double *scan_time,
                                              double *startup_cost)
{
  *out_rows= 0;
  *scan_time= 0.0;
  *startup_cost= 0.0;
}

int Table_function_json_table::print(THD *, TABLE_LIST *, String *,
                                     enum_query_type)
{
  mylite_json_table_not_supported();
  return TRUE;
}

void Table_function_json_table::start_nested_path(Json_table_nested_path *np)
{
  np->m_parent= cur_parent;
  *last_sibling_hook= np;
  cur_parent= np;
  last_sibling_hook= &np->m_nested;
}

void Table_function_json_table::end_nested_path()
{
  last_sibling_hook= &cur_parent->m_next_nested;
  cur_parent= cur_parent->m_parent;
}

bool push_table_function_arg_context(LEX *lex, MEM_ROOT *alloc)
{
  List_iterator<Name_resolution_context> it(lex->context_stack);
  Name_resolution_context *ctx;
  while ((ctx= it++))
  {
    if (ctx->select_lex && ctx == &ctx->select_lex->context)
      break;
  }

  if (!ctx)
  {
    mylite_json_table_not_supported();
    return true;
  }

  Name_resolution_context *new_ctx= new (alloc) Name_resolution_context;
  if (!new_ctx)
    return true;

  *new_ctx= *ctx;
  return lex->push_context(new_ctx);
}

TABLE *create_table_for_function(THD *, TABLE_LIST *)
{
  mylite_json_table_not_supported();
  return nullptr;
}

table_map add_table_function_dependencies(List<TABLE_LIST> *, table_map,
                                          bool *error)
{
  if (error)
    *error= false;
  return 0;
}

int Json_table_column::set(THD *, enum_type ctype, const LEX_CSTRING &path,
                           CHARSET_INFO *cs)
{
  set(ctype);
  m_explicit_cs= cs;
  m_path.s.c_str= (const uchar *) path.str;
  m_path.s.str_end= (const uchar *) (path.str + path.length);
  if (ctype == PATH)
    m_format_json= false;
  return 0;
}

int Json_table_column::set(THD *thd, enum_type ctype, const LEX_CSTRING &path,
                           const Lex_column_charset_collation_attrs_st &cl)
{
  if (cl.is_empty() || cl.is_contextually_typed_collate_default())
    return set(thd, ctype, path, nullptr);

  CHARSET_INFO *tmp;
  if (!(tmp= cl.resolved_to_character_set(
                  thd,
                  thd->variables.character_set_collations,
                  &my_charset_utf8mb4_general_ci)))
    return 1;
  return set(thd, ctype, path, tmp);
}

int Json_table_column::print(THD *, Field **, String *)
{
  mylite_json_table_not_supported();
  return 1;
}

int Json_table_column::On_response::respond(Json_table_column *, Field *,
                                            uint)
{
  mylite_json_table_not_supported();
  return 1;
}

int Json_table_column::On_response::print(const char *, String *) const
{
  if (m_response == Json_table_column::RESPONSE_NOT_SPECIFIED)
    return 0;

  mylite_json_table_not_supported();
  return 1;
}

int Json_table_nested_path::set_path(THD *, const LEX_CSTRING &path)
{
  m_path.s.c_str= (const uchar *) path.str;
  m_path.s.str_end= (const uchar *) (path.str + path.length);
  return 0;
}

void Json_table_nested_path::scan_start(CHARSET_INFO *, const uchar *,
                                        const uchar *)
{
  m_null= TRUE;
  m_ordinality_counter= 0;
}

int Json_table_nested_path::scan_next()
{
  return 1;
}

bool Json_table_nested_path::check_error(const char *)
{
  return false;
}

int Json_table_nested_path::print(THD *, Field ***, String *,
                                  List_iterator_fast<Json_table_column> &,
                                  Json_table_column **)
{
  mylite_json_table_not_supported();
  return 1;
}

static void mylite_json_table_not_supported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "JSON_TABLE in the MyLite embedded profile");
}

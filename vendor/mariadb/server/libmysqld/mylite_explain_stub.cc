/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "sql_explain.h"
#include "mysqld_error.h"

const char *unit_operation_text[4]=
{
  "UNIT RESULT", "UNION RESULT", "INTERSECT RESULT", "EXCEPT RESULT"
};

const char *pushed_unit_operation_text[4]=
{
  "PUSHED UNIT", "PUSHED UNION", "PUSHED INTERSECT", "PUSHED EXCEPT"
};

const char *pushed_derived_text= "PUSHED DERIVED";
const char *pushed_select_text= "PUSHED SELECT";

static bool mylite_explain_runtime_unsupported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "EXPLAIN runtime in the MyLite minsize profile");
  return true;
}

void delete_explain_query(LEX *lex)
{
  delete lex->explain;
  lex->explain= nullptr;
}

void create_explain_query(LEX *lex, MEM_ROOT *mem_root)
{
  if (!lex->explain)
    lex->explain= new (mem_root) Explain_query(lex->thd, mem_root);
}

void create_explain_query_if_not_exists(LEX *lex, MEM_ROOT *mem_root)
{
  create_explain_query(lex, mem_root);
}

bool print_explain_for_slow_log(LEX *, THD *, String *)
{
  return true;
}

Explain_query::Explain_query(THD *thd, MEM_ROOT *root)
  : mem_root(root),
    upd_del_plan(nullptr),
    insert_plan(nullptr),
    unions(root),
    selects(root),
    stmt_thd(thd),
    apc_enabled(false),
    operations(0)
{
}

Explain_query::~Explain_query()
{
  if (apc_enabled)
    stmt_thd->apc_target.disable();

  delete upd_del_plan;
  delete insert_plan;
  uint i;
  for (i= 0 ; i < unions.elements(); i++)
    delete unions.at(i);
  for (i= 0 ; i < selects.elements(); i++)
    delete selects.at(i);
}

void Explain_query::add_node(Explain_node *node)
{
  uint select_id;
  operations++;
  if (node->get_type() == Explain_node::EXPLAIN_UNION)
  {
    Explain_union *u= (Explain_union*)node;
    select_id= u->get_select_id();
    if (unions.elements() <= select_id)
      unions.resize(MY_MAX(select_id + 1, unions.elements() * 2), NULL);

    Explain_union *old_node;
    if ((old_node= get_union(select_id)))
      delete old_node;

    unions.at(select_id)= u;
  }
  else
  {
    Explain_select *sel= (Explain_select*)node;
    if (sel->select_id == FAKE_SELECT_LEX_ID)
      DBUG_ASSERT(0);
    else
    {
      select_id= sel->select_id;
      Explain_select *old_node;

      if (selects.elements() <= select_id)
        selects.resize(MY_MAX(select_id + 1, selects.elements() * 2), NULL);

      if ((old_node= get_select(select_id)))
        delete old_node;

      selects.at(select_id)= sel;
    }
  }
}

void Explain_query::add_insert_plan(Explain_insert *insert_plan_arg)
{
  insert_plan= insert_plan_arg;
  query_plan_ready();
}

void Explain_query::add_upd_del_plan(Explain_update *upd_del_plan_arg)
{
  upd_del_plan= upd_del_plan_arg;
  query_plan_ready();
}

Explain_node *Explain_query::get_node(uint select_id)
{
  Explain_union *u;
  if ((u= get_union(select_id)))
    return u;
  return get_select(select_id);
}

Explain_select *Explain_query::get_select(uint select_id)
{
  return (selects.elements() > select_id) ? selects.at(select_id) : NULL;
}

Explain_union *Explain_query::get_union(uint select_id)
{
  return (unions.elements() > select_id) ? unions.at(select_id) : NULL;
}

int Explain_query::print_explain(select_result_sink *, uint8, bool)
{
  return mylite_explain_runtime_unsupported();
}

int Explain_query::send_explain(THD *, bool)
{
  return mylite_explain_runtime_unsupported();
}

bool Explain_query::print_explain_str(THD *, String *, bool)
{
  return true;
}

int Explain_query::print_explain_json(select_result_sink *, bool, ulonglong)
{
  return mylite_explain_runtime_unsupported();
}

void Explain_query::query_plan_ready()
{
}

void Explain_query::notify_tables_are_closed()
{
}

Explain_basic_join::~Explain_basic_join()
{
}

bool Explain_basic_join::add_table(Explain_table_access *, Explain_query *)
{
  return false;
}

int Explain_basic_join::print_explain(Explain_query *, select_result_sink *,
                                      uint8, bool)
{
  return mylite_explain_runtime_unsupported();
}

void Explain_basic_join::print_explain_json(Explain_query *, Json_writer *,
                                            bool)
{
}

void Explain_basic_join::print_explain_json_interns(Explain_query *,
                                                    Json_writer *, bool)
{
}

void Explain_select::add_linkage(Json_writer *)
{
}

int Explain_select::print_explain(Explain_query *, select_result_sink *,
                                  uint8, bool)
{
  return mylite_explain_runtime_unsupported();
}

void Explain_select::print_explain_json(Explain_query *, Json_writer *, bool)
{
}

Explain_aggr_filesort::Explain_aggr_filesort(MEM_ROOT *, bool is_analyze,
                                             Filesort *filesort)
  : tracker(is_analyze)
{
  child= nullptr;
  filesort->tracker= &tracker;
}

void Explain_aggr_filesort::print_json_members(Json_writer *, bool)
{
}

void Explain_aggr_window_funcs::print_json_members(Json_writer *, bool)
{
}

int Explain_union::print_explain(Explain_query *, select_result_sink *,
                                 uint8, bool)
{
  return mylite_explain_runtime_unsupported();
}

void Explain_union::print_explain_json(Explain_query *, Json_writer *, bool)
{
}

void Explain_union::print_explain_json_regular(Explain_query *, Json_writer *,
                                               bool)
{
}

void Explain_union::print_explain_json_pushed_down(Explain_query *,
                                                   Json_writer *, bool)
{
}

uint Explain_union::make_union_table_name(char *)
{
  return 0;
}

int Explain_union::print_explain_regular(Explain_query *, select_result_sink *,
                                         uint8, bool)
{
  return mylite_explain_runtime_unsupported();
}

int Explain_union::print_explain_pushed_down(select_result_sink *, uint8,
                                             bool)
{
  return mylite_explain_runtime_unsupported();
}

int Explain_update::print_explain(Explain_query *, select_result_sink *,
                                  uint8, bool)
{
  return mylite_explain_runtime_unsupported();
}

void Explain_update::print_explain_json(Explain_query *, Json_writer *, bool)
{
}

int Explain_insert::print_explain(Explain_query *, select_result_sink *,
                                  uint8, bool)
{
  return mylite_explain_runtime_unsupported();
}

void Explain_insert::print_explain_json(Explain_query *, Json_writer *, bool)
{
}

int Explain_delete::print_explain(Explain_query *, select_result_sink *,
                                  uint8, bool)
{
  return mylite_explain_runtime_unsupported();
}

void Explain_delete::print_explain_json(Explain_query *, Json_writer *, bool)
{
}

bool Explain_index_use::set(MEM_ROOT *root, KEY *key, uint key_len_arg)
{
  if (set_pseudo_key(root, key ? key->name.str : nullptr))
    return true;
  key_len= key_len_arg;
  return false;
}

bool Explain_index_use::set_pseudo_key(MEM_ROOT *root,
                                       const char *key_name_arg)
{
  if (key_name_arg)
  {
    if (!(key_name= strdup_root(root, key_name_arg)))
      return true;
  }
  else
    key_name= nullptr;
  key_len= ~(uint) 0;
  return false;
}

int Explain_range_checked_fer::append_possible_keys_stat(MEM_ROOT *, TABLE *,
                                                         key_map)
{
  return 0;
}

void Explain_range_checked_fer::collect_data(QUICK_SELECT_I *)
{
}

void Explain_range_checked_fer::print_json(Json_writer *, bool)
{
}

void Explain_rowid_filter::print_explain_json(Explain_query *, Json_writer *,
                                              bool)
{
}

void Explain_table_access::push_extra(enum explain_extra_tag)
{
}

int Explain_table_access::print_explain(select_result_sink *, uint8, bool,
                                        uint, const char *, bool, bool)
{
  return mylite_explain_runtime_unsupported();
}

void Explain_table_access::print_explain_json(Explain_query *, Json_writer *,
                                              bool)
{
}

void Explain_table_access::append_tag_name(String *, enum explain_extra_tag)
{
}

void Explain_table_access::fill_key_str(String *, bool) const
{
}

void Explain_table_access::fill_key_len_str(String *, bool) const
{
}

double Explain_table_access::get_r_filtered()
{
  return 0.0;
}

void Explain_table_access::tag_to_json(Json_writer *, enum explain_extra_tag)
{
}

void Explain_subq_materialization::print_explain_json(Json_writer *, bool)
{
}

const char *String_list::append_str(MEM_ROOT *mem_root, const char *str)
{
  size_t len= strlen(str);
  char *copy= static_cast<char *>(alloc_root(mem_root, len + 1));
  if (!copy)
    return nullptr;
  memcpy(copy, str, len + 1);
  push_back(copy, mem_root);
  return copy;
}

void Explain_quick_select::print_extra(String *)
{
}

void Explain_quick_select::print_key(String *)
{
}

void Explain_quick_select::print_key_len(String *)
{
}

void Explain_quick_select::print_json(Json_writer *)
{
}

void Explain_quick_select::print_extra_recursive(String *)
{
}

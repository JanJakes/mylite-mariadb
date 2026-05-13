/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_class.h"
#include "sql_trigger.h"
#include "mysqld_error.h"

const char * const TRG_EXT= ".TRG";
const char * const TRN_EXT= ".TRN";

static bool mylite_trigger_not_supported();

void TRIGGER_RENAME_PARAM::reset()
{
  delete table.triggers;
  table.triggers= 0;
  free_root(&table.mem_root, MYF(0));
}

Trigger::~Trigger()
{
}

void Trigger::get_trigger_info(LEX_CSTRING *stmt, LEX_CSTRING *body,
                               LEX_STRING *definer)
{
  if (stmt)
  {
    stmt->str= nullptr;
    stmt->length= 0;
  }
  if (body)
  {
    body->str= nullptr;
    body->length= 0;
  }
  if (definer)
  {
    definer->str= nullptr;
    definer->length= 0;
  }
}

bool Trigger::change_on_table_name(void *)
{
  return false;
}

bool Trigger::change_table_name(void *)
{
  return false;
}

bool Trigger::add_to_file_list(void *)
{
  return false;
}

bool Trigger::match_updatable_columns(List<Item> &)
{
  return false;
}

Table_triggers_list::~Table_triggers_list()
{
}

bool Table_triggers_list::create_trigger(THD *, TABLE_LIST *, String *,
                                         DDL_LOG_STATE *, DDL_LOG_STATE *)
{
  return mylite_trigger_not_supported();
}

bool Table_triggers_list::drop_trigger(THD *, TABLE_LIST *, LEX_CSTRING *,
                                       String *, DDL_LOG_STATE *)
{
  return mylite_trigger_not_supported();
}

bool Table_triggers_list::process_triggers(THD *, trg_event_type,
                                           trg_action_time_type, bool,
                                           bool *skip_row_indicator,
                                           List<Item> *)
{
  if (skip_row_indicator)
    *skip_row_indicator= false;
  return false;
}

void Table_triggers_list::empty_lists()
{
  definitions_list.empty();
  definition_modes_list.empty();
  definers_list.empty();
  client_cs_names.empty();
  connection_cl_names.empty();
  db_cl_names.empty();
  hr_create_times.empty();
}

bool Table_triggers_list::create_lists_needed_for_files(MEM_ROOT *)
{
  empty_lists();
  return false;
}

bool Table_triggers_list::save_trigger_file(THD *, const LEX_CSTRING *,
                                            const LEX_CSTRING *)
{
  return mylite_trigger_not_supported();
}

bool Table_triggers_list::check_n_load(THD *, const LEX_CSTRING *,
                                       const LEX_CSTRING *, TABLE *table,
                                       bool)
{
  if (table)
    table->triggers= nullptr;
  return false;
}

bool Table_triggers_list::drop_all_triggers(THD *, const LEX_CSTRING *,
                                            const LEX_CSTRING *, myf)
{
  return false;
}

bool Table_triggers_list::prepare_for_rename(THD *,
                                             TRIGGER_RENAME_PARAM *param,
                                             const Lex_ident_db &,
                                             const Lex_ident_table &,
                                             const Lex_ident_table &,
                                             const Lex_ident_db &,
                                             const Lex_ident_table &)
{
  if (param)
    param->got_error= false;
  return false;
}

bool Table_triggers_list::change_table_name(THD *,
                                            TRIGGER_RENAME_PARAM *param,
                                            const LEX_CSTRING *,
                                            const LEX_CSTRING *,
                                            const LEX_CSTRING *,
                                            const LEX_CSTRING *,
                                            const LEX_CSTRING *)
{
  if (param)
    param->got_error= false;
  return false;
}

void Table_triggers_list::add_trigger(trg_event_type, trg_action_time_type,
                                      trigger_order_type,
                                      const Lex_ident_trigger &, Trigger *trigger)
{
  delete trigger;
}

bool Table_triggers_list::match_updatable_columns(List<Item> *)
{
  return false;
}

void Table_triggers_list::mark_fields_used(trg_event_type)
{
}

void Table_triggers_list::set_parse_error_message(char *)
{
}

bool Table_triggers_list::add_tables_and_routines_for_triggers(
    THD *, Query_tables_list *, TABLE_LIST *)
{
  return false;
}

Trigger *Table_triggers_list::find_trigger(const LEX_CSTRING *, bool)
{
  return nullptr;
}

Trigger *Table_triggers_list::for_all_triggers(Triggers_processor, void *)
{
  return nullptr;
}

bool Table_triggers_list::prepare_record_accessors(TABLE *)
{
  return false;
}

Trigger *Table_triggers_list::change_table_name_in_trignames(
    const LEX_CSTRING *, const LEX_CSTRING *, const LEX_CSTRING *,
    Trigger *trigger)
{
  return trigger;
}

bool Table_triggers_list::change_table_name_in_triggers(
    THD *, const LEX_CSTRING *, const LEX_CSTRING *, const LEX_CSTRING *,
    const LEX_CSTRING *)
{
  return false;
}

bool add_table_for_trigger(THD *, const sp_name *, bool continue_if_not_exist,
                           TABLE_LIST **table)
{
  if (table)
    *table= nullptr;
  if (continue_if_not_exist)
    return false;
  return mylite_trigger_not_supported();
}

void build_trn_path(THD *, const sp_name *, LEX_STRING *trn_path)
{
  if (!trn_path)
    return;
  trn_path->length= 0;
  if (trn_path->str)
    trn_path->str[0]= '\0';
}

bool check_trn_exists(const LEX_CSTRING *)
{
  return true;
}

bool load_table_name_for_trigger(THD *, const sp_name *,
                                 const LEX_CSTRING *, LEX_CSTRING *)
{
  return mylite_trigger_not_supported();
}

bool mysql_create_or_drop_trigger(THD *, TABLE_LIST *, bool)
{
  return mylite_trigger_not_supported();
}

bool rm_trigname_file(char *, const LEX_CSTRING *, const LEX_CSTRING *, myf)
{
  return false;
}

static bool mylite_trigger_not_supported()
{
  my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "embedded");
  return true;
}

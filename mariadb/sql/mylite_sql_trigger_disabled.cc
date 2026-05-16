/*
  MyLite embedded profile stub for MariaDB trigger runtime.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#define MYSQL_LEX 1
#include "mariadb.h"
#include "sql_priv.h"
#include "sp_head.h"
#include "sql_trigger.h"
#include "sql_table.h"
#include "mysqld_error.h"

static void mylite_trigger_runtime_not_supported();

const char * const TRG_EXT= ".TRG";
const char * const TRN_EXT= ".TRN";

void TRIGGER_RENAME_PARAM::reset()
{
  delete table.triggers;
  table.triggers= 0;
  free_root(&table.mem_root, MYF(0));
}

Trigger::~Trigger()
{
  sp_head::destroy(body);
}

Trigger *Table_triggers_list::for_all_triggers(Triggers_processor func,
                                               void *arg)
{
  for (uint event= 0; event < (uint) TRG_EVENT_MAX; event++)
  {
    for (uint action= 0; action < (uint) TRG_ACTION_MAX; action++)
    {
      for (Trigger *trigger= get_trigger(event, action);
           trigger;
           trigger= trigger->next)
      {
        if ((trigger->*func)(arg))
          return trigger;
      }
    }
  }
  return 0;
}

bool mysql_create_or_drop_trigger(THD *, TABLE_LIST *, bool)
{
  mylite_trigger_runtime_not_supported();
  return true;
}

bool Table_triggers_list::create_trigger(THD *, TABLE_LIST *, String *,
                                         DDL_LOG_STATE *, DDL_LOG_STATE *)
{
  mylite_trigger_runtime_not_supported();
  return true;
}

bool Table_triggers_list::drop_trigger(THD *, TABLE_LIST *, LEX_CSTRING *,
                                       String *, DDL_LOG_STATE *)
{
  mylite_trigger_runtime_not_supported();
  return true;
}

bool Table_triggers_list::process_triggers(THD *, trg_event_type,
                                           trg_action_time_type, bool,
                                           bool *, List<Item> *)
{
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
  mylite_trigger_runtime_not_supported();
  return true;
}

bool Table_triggers_list::check_n_load(THD *, const LEX_CSTRING *,
                                       const LEX_CSTRING *, TABLE *table,
                                       bool)
{
  if (table)
    table->triggers= 0;
  return false;
}

bool Table_triggers_list::drop_all_triggers(THD *, const LEX_CSTRING *,
                                            const LEX_CSTRING *, myf)
{
  return false;
}

bool Table_triggers_list::prepare_for_rename(THD *, TRIGGER_RENAME_PARAM *param,
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

bool Table_triggers_list::change_table_name(THD *, TRIGGER_RENAME_PARAM *,
                                            const LEX_CSTRING *,
                                            const LEX_CSTRING *,
                                            const LEX_CSTRING *,
                                            const LEX_CSTRING *,
                                            const LEX_CSTRING *)
{
  return false;
}

void Table_triggers_list::add_trigger(trg_event_type event_type,
                                      trg_action_time_type action_time,
                                      trigger_order_type,
                                      const Lex_ident_trigger &,
                                      Trigger *trigger)
{
  if (!trigger)
    return;

  trigger->next= triggers[event_type][action_time];
  triggers[event_type][action_time]= trigger;
  trigger->event= event_type;
  trigger->action_time= action_time;
  trigger->action_order= ++count;
}

bool Table_triggers_list::match_updatable_columns(List<Item> *)
{
  return false;
}

void Table_triggers_list::mark_fields_used(trg_event_type)
{
}

void Table_triggers_list::set_parse_error_message(char *error_message)
{
  m_has_unparseable_trigger= true;
  strmake_buf(m_parse_error_message, error_message ? error_message : "");
}

bool Table_triggers_list::add_tables_and_routines_for_triggers(
    THD *, Query_tables_list *, TABLE_LIST *)
{
  return false;
}

Trigger *Table_triggers_list::find_trigger(const LEX_CSTRING *name,
                                           bool remove_from_list)
{
  for (uint event= 0; event < (uint) TRG_EVENT_MAX; event++)
  {
    for (uint action= 0; action < (uint) TRG_ACTION_MAX; action++)
    {
      Trigger **parent= &triggers[event][action];
      for (Trigger *trigger= *parent; trigger; trigger= trigger->next)
      {
        if (name && trigger->name.streq(*name))
        {
          if (remove_from_list)
          {
            *parent= trigger->next;
            count--;
          }
          return trigger;
        }
        parent= &trigger->next;
      }
    }
  }
  return 0;
}

bool Table_triggers_list::prepare_record_accessors(TABLE *)
{
  return false;
}

Trigger *Table_triggers_list::change_table_name_in_trignames(
    const LEX_CSTRING *, const LEX_CSTRING *, const LEX_CSTRING *,
    Trigger *)
{
  return 0;
}

bool Table_triggers_list::change_table_name_in_triggers(
    THD *, const LEX_CSTRING *, const LEX_CSTRING *,
    const LEX_CSTRING *, const LEX_CSTRING *)
{
  return false;
}

Table_triggers_list::~Table_triggers_list()
{
  for (uint event= 0; event < (uint) TRG_EVENT_MAX; event++)
  {
    for (uint action= 0; action < (uint) TRG_ACTION_MAX; action++)
    {
      Trigger *trigger= get_trigger(event, action);
      while (trigger)
      {
        Trigger *next= trigger->next;
        delete trigger;
        trigger= next;
      }
    }
  }
}

bool Trigger::add_to_file_list(void *)
{
  return false;
}

bool Trigger::change_on_table_name(void *)
{
  return false;
}

bool Trigger::change_table_name(void *)
{
  return false;
}

void Trigger::get_trigger_info(LEX_CSTRING *trigger_stmt,
                               LEX_CSTRING *trigger_body,
                               LEX_STRING *definer)
{
  if (trigger_stmt)
    *trigger_stmt= definition;
  if (trigger_body)
  {
    trigger_body->str= "";
    trigger_body->length= 0;
  }
  if (definer)
  {
    definer->str= const_cast<char *>("");
    definer->length= 0;
  }
}

bool Trigger::match_updatable_columns(List<Item> &)
{
  return false;
}

bool add_table_for_trigger(THD *thd, const sp_name *, bool if_exists,
                           TABLE_LIST **table)
{
  if (table)
    *table= NULL;
  if (if_exists)
  {
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                 ER_TRG_DOES_NOT_EXIST, "Trigger does not exist");
    return false;
  }
  my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
  return true;
}

void build_trn_path(THD *, const sp_name *, LEX_STRING *trn_path)
{
  if (!trn_path || !trn_path->str)
    return;

  trn_path->str[0]= '\0';
  trn_path->length= 0;
}

bool check_trn_exists(const LEX_CSTRING *)
{
  return true;
}

bool load_table_name_for_trigger(THD *, const sp_name *,
                                 const LEX_CSTRING *, LEX_CSTRING *)
{
  my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
  return true;
}

bool rm_trigname_file(char *, const LEX_CSTRING *,
                      const LEX_CSTRING *, myf)
{
  return false;
}

static void mylite_trigger_runtime_not_supported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "trigger runtime in the MyLite embedded profile");
}

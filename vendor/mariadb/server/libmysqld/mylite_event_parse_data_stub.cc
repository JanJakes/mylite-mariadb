/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_class.h"
#include "event_parse_data.h"

Event_parse_data *
Event_parse_data::new_instance(THD *thd)
{
  return new (thd->mem_root) Event_parse_data;
}

Event_parse_data::Event_parse_data()
  : on_completion(Event_parse_data::ON_COMPLETION_DEFAULT),
    status(Event_parse_data::ENABLED), status_changed(false),
    originator(0), do_not_create(false), body_changed(false),
    item_starts(nullptr), item_ends(nullptr), item_execute_at(nullptr),
    starts(0), ends(0), execute_at(0), starts_null(true), ends_null(true),
    execute_at_null(true), identifier(nullptr), item_expression(nullptr),
    expression(0), interval(static_cast<interval_type>(0))
{
  dbname.str= nullptr;
  dbname.length= 0;
  name.str= nullptr;
  name.length= 0;
  definer.str= nullptr;
  definer.length= 0;
  comment.str= nullptr;
  comment.length= 0;
}

/*
  MyLite embedded profile stub for event parse-data validation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#include <new>

#include "mariadb.h"
#include "my_time.h"
#include "mysqld_error.h"
#include "sql_priv.h"
#include "event_parse_data.h"
#include <mysql/service_thd_alloc.h>

Event_parse_data *Event_parse_data::new_instance(THD *thd)
{
  void *storage= thd_alloc(thd, sizeof(Event_parse_data));
  return storage ? ::new (storage) Event_parse_data : NULL;
}

Event_parse_data::Event_parse_data()
    : on_completion(Event_parse_data::ON_COMPLETION_DEFAULT),
      status(Event_parse_data::ENABLED),
      status_changed(false),
      originator(0),
      do_not_create(FALSE),
      body_changed(FALSE),
      dbname{NULL, 0},
      name{NULL, 0},
      definer{NULL, 0},
      comment{NULL, 0},
      item_starts(NULL),
      item_ends(NULL),
      item_execute_at(NULL),
      starts(0),
      ends(0),
      execute_at(0),
      starts_null(TRUE),
      ends_null(TRUE),
      execute_at_null(TRUE),
      identifier(NULL),
      item_expression(NULL),
      expression(0),
      interval(INTERVAL_LAST)
{
}

bool Event_parse_data::check_parse_data(THD *)
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), "event scheduler");
  return true;
}

bool Event_parse_data::check_dates(THD *, int)
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), "event scheduler");
  return true;
}

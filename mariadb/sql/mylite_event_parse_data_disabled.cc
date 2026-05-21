/* Copyright (c) 2026, MyLite contributors

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
#include "sql_priv.h"
#include "unireg.h"
#include "sql_class.h"
#include "event_parse_data.h"
#include "sql_time.h"

Event_parse_data *
Event_parse_data::new_instance(THD *thd)
{
  return new (thd->mem_root) Event_parse_data;
}

Event_parse_data::Event_parse_data()
  : on_completion(Event_parse_data::ON_COMPLETION_DEFAULT),
    status(Event_parse_data::DISABLED), status_changed(false), originator(0),
    do_not_create(TRUE), body_changed(FALSE), item_starts(NULL), item_ends(NULL),
    item_execute_at(NULL), starts(0), ends(0), execute_at(0), starts_null(TRUE),
    ends_null(TRUE), execute_at_null(TRUE), identifier(NULL), item_expression(NULL),
    expression(0), interval(INTERVAL_LAST)
{
  dbname.str= NULL;
  dbname.length= 0;
  name.str= NULL;
  name.length= 0;
  definer.str= NULL;
  definer.length= 0;
  comment.str= NULL;
  comment.length= 0;
}

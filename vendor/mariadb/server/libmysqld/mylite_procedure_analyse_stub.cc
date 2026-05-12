/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "procedure.h"
#include "mysqld_error.h"

Procedure *proc_analyse_init(THD *thd, ORDER *param, select_result *result,
                             List<Item> &field_list)
{
  (void) thd;
  (void) param;
  (void) result;
  (void) field_list;
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "PROCEDURE ANALYSE in MyLite minsize profile");
  return nullptr;
}

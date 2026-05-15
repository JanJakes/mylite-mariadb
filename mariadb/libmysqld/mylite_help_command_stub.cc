/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_help.h"
#include "mysqld_error.h"

bool mysqld_help(THD *thd, const char *text)
{
  (void) thd;
  (void) text;
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "HELP command in MyLite embedded profile");
  return true;
}

bool mysqld_help_prepare(THD *thd, const char *text, List<Item> *fields)
{
  (void) thd;
  (void) text;
  (void) fields;
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "HELP command in MyLite embedded profile");
  return true;
}

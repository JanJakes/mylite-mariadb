/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "mysqld_error.h"

class Item;
class Procedure;
class THD;
class select_result;
struct st_order;
typedef struct st_order ORDER;
template <class T> class List;

Procedure *setup_procedure(THD *, ORDER *param, select_result *,
                           List<Item> &, int *error)
{
  if (error)
    *error= 0;
  if (!param)
    return nullptr;

  if (error)
    *error= 1;
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "SELECT PROCEDURE clause in MyLite embedded profile");
  return nullptr;
}

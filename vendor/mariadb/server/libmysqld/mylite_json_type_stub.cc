/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "item.h"

bool is_json_type(const Item *)
{
  return false;
}

int Arg_comparator::compare_json_str_basic(Item *j, Item *s)
{
  String *js= j->val_str(&value1);
  String *str= s->val_str(&value2);
  if (js && str)
  {
    if (set_null)
      owner->null_value= 0;
    return sortcmp(js, str, compare_collation());
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}

int Arg_comparator::compare_e_json_str_basic(Item *j, Item *s)
{
  String *js= j->val_str(&value1);
  String *str= s->val_str(&value2);
  if (!js || !str)
    return MY_TEST(js == str);
  return MY_TEST(sortcmp(js, str, compare_collation()) == 0);
}

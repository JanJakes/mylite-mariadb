/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#define MYSQL_LEX 1
#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_view.h"
#include "mysqld_error.h"

const LEX_CSTRING view_type= { STRING_WITH_LEN("VIEW") };

static bool mylite_view_not_supported();
static void make_unique_view_field_name(THD *thd, Item *target,
                                        List<Item> &item_list,
                                        Item *last_element);

bool create_view_precheck(THD *, TABLE_LIST *, TABLE_LIST *,
                          enum_view_create_mode)
{
  return false;
}

bool mysql_create_view(THD *, TABLE_LIST *, enum_view_create_mode)
{
  return mylite_view_not_supported();
}

bool mysql_make_view(THD *, TABLE_SHARE *, TABLE_LIST *, bool)
{
  return mylite_view_not_supported();
}

bool mysql_drop_view(THD *, TABLE_LIST *, enum_drop_mode)
{
  return mylite_view_not_supported();
}

bool check_key_in_view(THD *, TABLE_LIST *)
{
  return false;
}

bool insert_view_fields(THD *, List<Item> *, TABLE_LIST *)
{
  return false;
}

int view_checksum(THD *, TABLE_LIST *)
{
  return HA_ADMIN_NOT_IMPLEMENTED;
}

int view_check(THD *, TABLE_LIST *, HA_CHECK_OPT *)
{
  return HA_ADMIN_NOT_IMPLEMENTED;
}

int view_repair(THD *, TABLE_LIST *, HA_CHECK_OPT *)
{
  return HA_ADMIN_NOT_IMPLEMENTED;
}

bool check_duplicate_names(THD *thd, List<Item> &item_list,
                           bool gen_unique_view_names)
{
  Item *item;
  List_iterator_fast<Item> it(item_list);
  List_iterator_fast<Item> itc(item_list);
  DBUG_ENTER("check_duplicate_names");

  while ((item= it++))
  {
    Item *check;
    if (item->real_item()->type() == Item::FIELD_ITEM)
      item->base_flags|= item_base_t::IS_EXPLICIT_NAME;
    itc.rewind();
    while ((check= itc++) && check != item)
    {
      if (item->name.streq(check->name))
      {
        if (!gen_unique_view_names)
          goto err;
        if (!item->is_explicit_name())
          make_unique_view_field_name(thd, item, item_list, item);
        else if (!check->is_explicit_name())
          make_unique_view_field_name(thd, check, item_list, item);
        else
          goto err;
      }
    }
  }
  DBUG_RETURN(false);

err:
  my_error(ER_DUP_FIELDNAME, MYF(0), item->name.str);
  DBUG_RETURN(true);
}

void make_valid_column_names(THD *thd, List<Item> &item_list)
{
  Item *item;
  size_t name_len;
  List_iterator_fast<Item> it(item_list);
  char buff[NAME_LEN];
  DBUG_ENTER("make_valid_column_names");

  for (uint column_no= 1; (item= it++); column_no++)
  {
    if (item->is_explicit_name() || !check_column_name(item->name))
      continue;
    name_len= my_snprintf(buff, NAME_LEN, "Name_exp_%u", column_no);
    item->orig_name= item->name;
    item->set_name(thd, buff, name_len, system_charset_info);
  }

  DBUG_VOID_RETURN;
}

bool mysql_rename_view(THD *, const LEX_CSTRING *, const LEX_CSTRING *,
                       const LEX_CSTRING *, const LEX_CSTRING *)
{
  return mylite_view_not_supported();
}

bool mariadb_view_version_get(TABLE_SHARE *)
{
  return mylite_view_not_supported();
}

static void make_unique_view_field_name(THD *thd, Item *target,
                                        List<Item> &item_list,
                                        Item *last_element)
{
  const char *name= (target->orig_name.str ?
                     target->orig_name.str :
                     target->name.str);
  size_t name_len;
  uint attempt;
  char buff[NAME_LEN + 1];
  List_iterator_fast<Item> itc(item_list);

  for (attempt= 0;; attempt++)
  {
    Item *check;
    bool ok= true;

    if (attempt)
      name_len= my_snprintf(buff, NAME_LEN, "My_exp_%d_%s", attempt, name);
    else
      name_len= my_snprintf(buff, NAME_LEN, "My_exp_%s", name);

    do
    {
      check= itc++;
      if (check != target &&
          check->name.streq(Lex_cstring(buff, name_len)))
      {
        ok= false;
        break;
      }
    } while (check != last_element);
    if (ok)
      break;
    itc.rewind();
  }

  if (!target->orig_name.str)
    target->orig_name= target->name;
  target->set_name(thd, buff, name_len, system_charset_info);
}

static bool mylite_view_not_supported()
{
  my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "embedded");
  return true;
}

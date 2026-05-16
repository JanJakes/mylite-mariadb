/* Copyright (c) 2004, 2013, Oracle and/or its affiliates.
   Copyright (c) 2011, 2021, MariaDB Corporation.
   Copyright (c) 2026, MyLite contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#define MYSQL_LEX 1
#include "mariadb.h"
#include "sql_priv.h"
#include "sql_view.h"
#include "sql_table.h"

static void mylite_view_runtime_not_supported();
static void make_unique_view_field_name(THD *thd, Item *target,
                                        List<Item> &item_list,
                                        Item *last_element);

const LEX_CSTRING view_type= { STRING_WITH_LEN("VIEW") };

bool check_duplicate_names(THD *thd, List<Item> &item_list,
                           bool gen_unique_view_name)
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
        if (!gen_unique_view_name)
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
  DBUG_RETURN(FALSE);

err:
  my_error(ER_DUP_FIELDNAME, MYF(0), item->name.str);
  DBUG_RETURN(TRUE);
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

bool create_view_precheck(THD *, TABLE_LIST *, TABLE_LIST *,
                          enum_view_create_mode)
{
  mylite_view_runtime_not_supported();
  return true;
}

bool mysql_create_view(THD *, TABLE_LIST *, enum_view_create_mode)
{
  mylite_view_runtime_not_supported();
  return true;
}

bool mariadb_view_version_get(TABLE_SHARE *)
{
  mylite_view_runtime_not_supported();
  return true;
}

bool mysql_make_view(THD *, TABLE_SHARE *, TABLE_LIST *, bool)
{
  mylite_view_runtime_not_supported();
  return true;
}

bool mysql_drop_view(THD *, TABLE_LIST *, enum_drop_mode)
{
  mylite_view_runtime_not_supported();
  return true;
}

bool check_key_in_view(THD *, TABLE_LIST *view)
{
  if (!view || (!view->view && !view->belong_to_view))
    return false;

  mylite_view_runtime_not_supported();
  return true;
}

bool insert_view_fields(THD *, List<Item> *, TABLE_LIST *view)
{
  if (!view || !view->field_translation)
    return false;

  mylite_view_runtime_not_supported();
  return true;
}

int view_checksum(THD *, TABLE_LIST *view)
{
  if (!view || !view->view || view->md5.length != VIEW_MD5_LEN)
    return HA_ADMIN_NOT_IMPLEMENTED;

  mylite_view_runtime_not_supported();
  return HA_ADMIN_NOT_IMPLEMENTED;
}

int view_check(THD *thd, TABLE_LIST *view, HA_CHECK_OPT *)
{
  return view_checksum(thd, view);
}

int view_repair(THD *thd, TABLE_LIST *view, HA_CHECK_OPT *)
{
  return view_checksum(thd, view);
}

bool mysql_rename_view(THD *, const LEX_CSTRING *, const LEX_CSTRING *,
                       const LEX_CSTRING *, const LEX_CSTRING *)
{
  mylite_view_runtime_not_supported();
  return true;
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
    bool ok= TRUE;

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
        ok= FALSE;
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

static void mylite_view_runtime_not_supported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "view runtime in the MyLite embedded profile");
}

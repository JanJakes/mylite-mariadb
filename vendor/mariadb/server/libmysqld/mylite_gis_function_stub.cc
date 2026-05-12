/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "set_var.h"
#include "item_geofunc.h"
#include "item_create.h"
#include "mysqld_error.h"

bool Item_geometry_func::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_bin);
  decimals= 0;
  max_length= (uint32) UINT_MAX32;
  set_maybe_null();
  return FALSE;
}

String *Item_func_point::val_str(String *)
{
  my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "FUNCTION", func_name());
  null_value= 1;
  return nullptr;
}

String *Item_func_spatial_collection::val_str(String *)
{
  my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "FUNCTION", func_name());
  null_value= 1;
  return nullptr;
}

int Gcalc_result_receiver::get_result_typeid()
{
  return Geometry::wkb_geometrycollection;
}

Native_func_registry_array native_func_registry_array_geom;

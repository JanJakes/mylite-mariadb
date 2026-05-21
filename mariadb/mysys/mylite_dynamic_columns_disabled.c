/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mysys_priv.h"
#include <ma_dyncol.h>

static enum enum_dyncol_func_result mylite_dynamic_columns_not_supported(void);

enum enum_dyncol_func_result
dynamic_column_create(DYNAMIC_COLUMN *str, uint column_nr,
                      DYNAMIC_COLUMN_VALUE *value)
{
  (void) str;
  (void) column_nr;
  (void) value;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
dynamic_column_create_many(DYNAMIC_COLUMN *str, uint column_count,
                           uint *column_numbers,
                           DYNAMIC_COLUMN_VALUE *values)
{
  (void) str;
  (void) column_count;
  (void) column_numbers;
  (void) values;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
dynamic_column_update(DYNAMIC_COLUMN *str, uint column_nr,
                      DYNAMIC_COLUMN_VALUE *value)
{
  (void) str;
  (void) column_nr;
  (void) value;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
dynamic_column_update_many(DYNAMIC_COLUMN *str, uint add_column_count,
                           uint *column_numbers,
                           DYNAMIC_COLUMN_VALUE *values)
{
  (void) str;
  (void) add_column_count;
  (void) column_numbers;
  (void) values;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
dynamic_column_exists(DYNAMIC_COLUMN *str, uint column_nr)
{
  (void) str;
  (void) column_nr;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
dynamic_column_list(DYNAMIC_COLUMN *str, DYNAMIC_ARRAY *array_of_uint)
{
  (void) str;
  (void) array_of_uint;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
dynamic_column_get(DYNAMIC_COLUMN *str, uint column_nr,
                   DYNAMIC_COLUMN_VALUE *store_it_here)
{
  (void) str;
  (void) column_nr;
  (void) store_it_here;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_create_many_num(DYNAMIC_COLUMN *str, uint column_count,
                               uint *column_numbers,
                               DYNAMIC_COLUMN_VALUE *values,
                               my_bool new_string)
{
  (void) str;
  (void) column_count;
  (void) column_numbers;
  (void) values;
  (void) new_string;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_create_many_named(DYNAMIC_COLUMN *str, uint column_count,
                                 MYSQL_LEX_STRING *column_keys,
                                 DYNAMIC_COLUMN_VALUE *values,
                                 my_bool new_string)
{
  (void) str;
  (void) column_count;
  (void) column_keys;
  (void) values;
  (void) new_string;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_update_many_num(DYNAMIC_COLUMN *str, uint add_column_count,
                               uint *column_keys,
                               DYNAMIC_COLUMN_VALUE *values)
{
  (void) str;
  (void) add_column_count;
  (void) column_keys;
  (void) values;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_update_many_named(DYNAMIC_COLUMN *str, uint add_column_count,
                                 MYSQL_LEX_STRING *column_keys,
                                 DYNAMIC_COLUMN_VALUE *values)
{
  (void) str;
  (void) add_column_count;
  (void) column_keys;
  (void) values;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_exists_num(DYNAMIC_COLUMN *str, uint column_nr)
{
  (void) str;
  (void) column_nr;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_exists_named(DYNAMIC_COLUMN *str, MYSQL_LEX_STRING *name)
{
  (void) str;
  (void) name;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_list_num(DYNAMIC_COLUMN *str, uint *count, uint **nums)
{
  (void) str;
  if (count)
    *count= 0;
  if (nums)
    *nums= NULL;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_list_named(DYNAMIC_COLUMN *str, uint *count,
                          MYSQL_LEX_STRING **names)
{
  (void) str;
  if (count)
    *count= 0;
  if (names)
    *names= NULL;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_get_num(DYNAMIC_COLUMN *str, uint column_nr,
                       DYNAMIC_COLUMN_VALUE *store_it_here)
{
  (void) str;
  (void) column_nr;
  if (store_it_here)
    store_it_here->type= DYN_COL_NULL;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_get_named(DYNAMIC_COLUMN *str, MYSQL_LEX_STRING *name,
                         DYNAMIC_COLUMN_VALUE *store_it_here)
{
  (void) str;
  (void) name;
  if (store_it_here)
    store_it_here->type= DYN_COL_NULL;
  return mylite_dynamic_columns_not_supported();
}

my_bool mariadb_dyncol_has_names(DYNAMIC_COLUMN *str)
{
  (void) str;
  return FALSE;
}

enum enum_dyncol_func_result
mariadb_dyncol_check(DYNAMIC_COLUMN *str)
{
  (void) str;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_json(DYNAMIC_COLUMN *str, DYNAMIC_STRING *json)
{
  (void) str;
  if (json)
    memset(json, 0, sizeof(*json));
  return mylite_dynamic_columns_not_supported();
}

void mariadb_dyncol_free(DYNAMIC_COLUMN *str)
{
  if (str)
    dynstr_free(str);
}

enum enum_dyncol_func_result
mariadb_dyncol_val_str(DYNAMIC_STRING *str, DYNAMIC_COLUMN_VALUE *val,
                       CHARSET_INFO *cs, my_bool quote)
{
  (void) str;
  (void) val;
  (void) cs;
  (void) quote;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_val_long(longlong *ll, DYNAMIC_COLUMN_VALUE *val)
{
  (void) val;
  if (ll)
    *ll= 0;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_val_double(double *dbl, DYNAMIC_COLUMN_VALUE *val)
{
  (void) val;
  if (dbl)
    *dbl= 0.0;
  return mylite_dynamic_columns_not_supported();
}

enum enum_dyncol_func_result
mariadb_dyncol_unpack(DYNAMIC_COLUMN *str, uint *count,
                      MYSQL_LEX_STRING **names,
                      DYNAMIC_COLUMN_VALUE **vals)
{
  (void) str;
  if (count)
    *count= 0;
  if (names)
    *names= NULL;
  if (vals)
    *vals= NULL;
  return mylite_dynamic_columns_not_supported();
}

void mariadb_dyncol_unpack_free(MYSQL_LEX_STRING *names,
                                DYNAMIC_COLUMN_VALUE *vals)
{
  my_free(names);
  my_free(vals);
}

int mariadb_dyncol_column_cmp_named(const MYSQL_LEX_STRING *s1,
                                    const MYSQL_LEX_STRING *s2)
{
  int rc= (s1->length > s2->length ? 1 :
           (s1->length < s2->length ? -1 : 0));
  if (rc == 0)
    rc= memcmp((void *) s1->str, (void *) s2->str, (size_t) s1->length);
  return rc;
}

enum enum_dyncol_func_result
mariadb_dyncol_column_count(DYNAMIC_COLUMN *str, uint *column_count)
{
  (void) str;
  if (column_count)
    *column_count= 0;
  return mylite_dynamic_columns_not_supported();
}

void mariadb_dyncol_prepare_decimal(DYNAMIC_COLUMN_VALUE *value)
{
  value->x.decimal.value.buf= value->x.decimal.buffer;
  value->x.decimal.value.len= DECIMAL_BUFF_LENGTH;
  value->type= DYN_COL_DECIMAL;
  decimal_make_zero(&value->x.decimal.value);
}

void dynamic_column_prepare_decimal(DYNAMIC_COLUMN_VALUE *value)
{
  mariadb_dyncol_prepare_decimal(value);
}

static enum enum_dyncol_func_result mylite_dynamic_columns_not_supported(void)
{
  return ER_DYNCOL_DATA;
}

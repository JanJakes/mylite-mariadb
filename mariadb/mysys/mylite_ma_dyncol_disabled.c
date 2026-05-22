/*
  MyLite embedded profile stub for MariaDB dynamic columns.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#include "mysys_priv.h"
#include <ma_dyncol.h>
#include <m_string.h>

static enum enum_dyncol_func_result mylite_dyncol_disabled(void)
{
  return ER_DYNCOL_FORMAT;
}

static void mylite_dyncol_init_if_new(DYNAMIC_COLUMN *str, my_bool new_string)
{
  if (new_string && str)
    mariadb_dyncol_init(str);
}

my_bool mariadb_dyncol_has_names(DYNAMIC_COLUMN *str)
{
  (void) str;
  return FALSE;
}

int mariadb_dyncol_column_cmp_named(const LEX_STRING *s1,
                                    const LEX_STRING *s2)
{
  int rc= (s1->length > s2->length ? 1 :
           (s1->length < s2->length ? -1 : 0));
  if (rc == 0)
    rc= memcmp((void *) s1->str, (void *) s2->str, (size_t) s1->length);
  return rc;
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

enum enum_dyncol_func_result
dynamic_column_create(DYNAMIC_COLUMN *str, uint column_nr,
                      DYNAMIC_COLUMN_VALUE *value)
{
  mylite_dyncol_init_if_new(str, TRUE);
  (void) column_nr;
  (void) value;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_create_many(DYNAMIC_COLUMN *str, uint column_count,
                           uint *column_numbers,
                           DYNAMIC_COLUMN_VALUE *values)
{
  mylite_dyncol_init_if_new(str, TRUE);
  (void) column_count;
  (void) column_numbers;
  (void) values;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_create_many_num(DYNAMIC_COLUMN *str, uint column_count,
                               uint *column_numbers,
                               DYNAMIC_COLUMN_VALUE *values,
                               my_bool new_string)
{
  mylite_dyncol_init_if_new(str, new_string);
  (void) column_count;
  (void) column_numbers;
  (void) values;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_create_many_named(DYNAMIC_COLUMN *str, uint column_count,
                                 MYSQL_LEX_STRING *column_keys,
                                 DYNAMIC_COLUMN_VALUE *values,
                                 my_bool new_string)
{
  mylite_dyncol_init_if_new(str, new_string);
  (void) column_count;
  (void) column_keys;
  (void) values;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_update(DYNAMIC_COLUMN *str, uint column_nr,
                      DYNAMIC_COLUMN_VALUE *value)
{
  (void) str;
  (void) column_nr;
  (void) value;
  return mylite_dyncol_disabled();
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
  return mylite_dyncol_disabled();
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
  return mylite_dyncol_disabled();
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
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_exists(DYNAMIC_COLUMN *str, uint column_nr)
{
  (void) str;
  (void) column_nr;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_exists_num(DYNAMIC_COLUMN *str, uint column_nr)
{
  (void) str;
  (void) column_nr;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_exists_named(DYNAMIC_COLUMN *str, MYSQL_LEX_STRING *name)
{
  (void) str;
  (void) name;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_list(DYNAMIC_COLUMN *str, DYNAMIC_ARRAY *array_of_uint)
{
  (void) str;
  (void) array_of_uint;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_list_num(DYNAMIC_COLUMN *str, uint *count, uint **nums)
{
  if (count)
    *count= 0;
  if (nums)
    *nums= NULL;
  (void) str;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_list_named(DYNAMIC_COLUMN *str, uint *count,
                          MYSQL_LEX_STRING **names)
{
  if (count)
    *count= 0;
  if (names)
    *names= NULL;
  (void) str;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_get(DYNAMIC_COLUMN *str, uint column_nr,
                   DYNAMIC_COLUMN_VALUE *store_it_here)
{
  (void) str;
  (void) column_nr;
  (void) store_it_here;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_get_num(DYNAMIC_COLUMN *str, uint column_nr,
                       DYNAMIC_COLUMN_VALUE *store_it_here)
{
  (void) str;
  (void) column_nr;
  (void) store_it_here;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_get_named(DYNAMIC_COLUMN *str, MYSQL_LEX_STRING *name,
                         DYNAMIC_COLUMN_VALUE *store_it_here)
{
  (void) str;
  (void) name;
  (void) store_it_here;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_check(DYNAMIC_COLUMN *str)
{
  (void) str;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_json(DYNAMIC_COLUMN *str, DYNAMIC_STRING *json)
{
  mylite_dyncol_init_if_new(json, TRUE);
  (void) str;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_val_str(DYNAMIC_STRING *str, DYNAMIC_COLUMN_VALUE *val,
                       CHARSET_INFO *cs, my_bool quote)
{
  (void) str;
  (void) val;
  (void) cs;
  (void) quote;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_val_long(longlong *ll, DYNAMIC_COLUMN_VALUE *val)
{
  if (ll)
    *ll= 0;
  (void) val;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_val_double(double *dbl, DYNAMIC_COLUMN_VALUE *val)
{
  if (dbl)
    *dbl= 0;
  (void) val;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_unpack(DYNAMIC_COLUMN *str, uint *count,
                      MYSQL_LEX_STRING **names,
                      DYNAMIC_COLUMN_VALUE **vals)
{
  if (count)
    *count= 0;
  if (names)
    *names= NULL;
  if (vals)
    *vals= NULL;
  (void) str;
  return mylite_dyncol_disabled();
}

void mariadb_dyncol_unpack_free(MYSQL_LEX_STRING *names,
                                DYNAMIC_COLUMN_VALUE *vals)
{
  my_free(names);
  my_free(vals);
}

enum enum_dyncol_func_result
mariadb_dyncol_column_count(DYNAMIC_COLUMN *str, uint *column_count)
{
  if (column_count)
    *column_count= 0;
  (void) str;
  return mylite_dyncol_disabled();
}

void mariadb_dyncol_free(DYNAMIC_COLUMN *str)
{
  if (str)
    dynstr_free(str);
}

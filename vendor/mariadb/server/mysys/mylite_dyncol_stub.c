/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mysys_priv.h"
#include <ma_dyncol.h>
#include <m_string.h>

static enum enum_dyncol_func_result mylite_dyncol_disabled(void);

enum enum_dyncol_func_result
dynamic_column_create(DYNAMIC_COLUMN *str __attribute__((unused)),
                      uint column_nr __attribute__((unused)),
                      DYNAMIC_COLUMN_VALUE *value __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_create_many(DYNAMIC_COLUMN *str __attribute__((unused)),
                           uint column_count __attribute__((unused)),
                           uint *column_numbers __attribute__((unused)),
                           DYNAMIC_COLUMN_VALUE *values
                             __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_update(DYNAMIC_COLUMN *str __attribute__((unused)),
                      uint column_nr __attribute__((unused)),
                      DYNAMIC_COLUMN_VALUE *value __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_update_many(DYNAMIC_COLUMN *str __attribute__((unused)),
                           uint add_column_count __attribute__((unused)),
                           uint *column_numbers __attribute__((unused)),
                           DYNAMIC_COLUMN_VALUE *values
                             __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_exists(DYNAMIC_COLUMN *str __attribute__((unused)),
                      uint column_nr __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_list(DYNAMIC_COLUMN *str __attribute__((unused)),
                    DYNAMIC_ARRAY *array_of_uint __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
dynamic_column_get(DYNAMIC_COLUMN *str __attribute__((unused)),
                   uint column_nr __attribute__((unused)),
                   DYNAMIC_COLUMN_VALUE *store_it_here
                     __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_create_many_num(DYNAMIC_COLUMN *str __attribute__((unused)),
                               uint column_count __attribute__((unused)),
                               uint *column_numbers __attribute__((unused)),
                               DYNAMIC_COLUMN_VALUE *values
                                 __attribute__((unused)),
                               my_bool new_string __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_create_many_named(DYNAMIC_COLUMN *str __attribute__((unused)),
                                 uint column_count __attribute__((unused)),
                                 MYSQL_LEX_STRING *column_keys
                                   __attribute__((unused)),
                                 DYNAMIC_COLUMN_VALUE *values
                                   __attribute__((unused)),
                                 my_bool new_string __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_update_many_num(DYNAMIC_COLUMN *str __attribute__((unused)),
                               uint add_column_count __attribute__((unused)),
                               uint *column_keys __attribute__((unused)),
                               DYNAMIC_COLUMN_VALUE *values
                                 __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_update_many_named(DYNAMIC_COLUMN *str __attribute__((unused)),
                                 uint add_column_count __attribute__((unused)),
                                 MYSQL_LEX_STRING *column_keys
                                   __attribute__((unused)),
                                 DYNAMIC_COLUMN_VALUE *values
                                   __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_exists_num(DYNAMIC_COLUMN *str __attribute__((unused)),
                          uint column_nr __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_exists_named(DYNAMIC_COLUMN *str __attribute__((unused)),
                            MYSQL_LEX_STRING *name __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_list_num(DYNAMIC_COLUMN *str __attribute__((unused)),
                        uint *count, uint **nums)
{
  if (count)
    *count= 0;
  if (nums)
    *nums= NULL;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_list_named(DYNAMIC_COLUMN *str __attribute__((unused)),
                          uint *count, MYSQL_LEX_STRING **names)
{
  if (count)
    *count= 0;
  if (names)
    *names= NULL;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_get_num(DYNAMIC_COLUMN *str __attribute__((unused)),
                       uint column_nr __attribute__((unused)),
                       DYNAMIC_COLUMN_VALUE *store_it_here)
{
  if (store_it_here)
    mariadb_dyncol_value_init(store_it_here);
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_get_named(DYNAMIC_COLUMN *str __attribute__((unused)),
                         MYSQL_LEX_STRING *name __attribute__((unused)),
                         DYNAMIC_COLUMN_VALUE *store_it_here)
{
  if (store_it_here)
    mariadb_dyncol_value_init(store_it_here);
  return mylite_dyncol_disabled();
}

my_bool mariadb_dyncol_has_names(DYNAMIC_COLUMN *str __attribute__((unused)))
{
  return FALSE;
}

enum enum_dyncol_func_result
mariadb_dyncol_check(DYNAMIC_COLUMN *str __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_json(DYNAMIC_COLUMN *str __attribute__((unused)),
                    DYNAMIC_STRING *json __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

void mariadb_dyncol_free(DYNAMIC_COLUMN *str)
{
  if (str)
    dynstr_free(str);
}

enum enum_dyncol_func_result
mariadb_dyncol_val_str(DYNAMIC_STRING *str __attribute__((unused)),
                       DYNAMIC_COLUMN_VALUE *val __attribute__((unused)),
                       CHARSET_INFO *cs __attribute__((unused)),
                       my_bool quote __attribute__((unused)))
{
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_val_long(longlong *ll, DYNAMIC_COLUMN_VALUE *val
                          __attribute__((unused)))
{
  if (ll)
    *ll= 0;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_val_double(double *dbl,
                          DYNAMIC_COLUMN_VALUE *val __attribute__((unused)))
{
  if (dbl)
    *dbl= 0.0;
  return mylite_dyncol_disabled();
}

enum enum_dyncol_func_result
mariadb_dyncol_unpack(DYNAMIC_COLUMN *str __attribute__((unused)),
                      uint *count, MYSQL_LEX_STRING **names,
                      DYNAMIC_COLUMN_VALUE **vals)
{
  if (count)
    *count= 0;
  if (names)
    *names= NULL;
  if (vals)
    *vals= NULL;
  return mylite_dyncol_disabled();
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
  size_t min_length;
  int cmp;

  if (!s1 || !s2)
    return s1 == s2 ? 0 : s1 ? 1 : -1;
  min_length= MY_MIN(s1->length, s2->length);
  cmp= memcmp(s1->str, s2->str, min_length);
  if (cmp)
    return cmp;
  return s1->length == s2->length ? 0 : s1->length < s2->length ? -1 : 1;
}

enum enum_dyncol_func_result
mariadb_dyncol_column_count(DYNAMIC_COLUMN *str __attribute__((unused)),
                            uint *column_count)
{
  if (column_count)
    *column_count= 0;
  return mylite_dyncol_disabled();
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

static enum enum_dyncol_func_result mylite_dyncol_disabled(void)
{
  return ER_DYNCOL_RESOURCE;
}

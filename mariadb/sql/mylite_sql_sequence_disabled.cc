/*
  MyLite embedded profile stub for SQL sequence runtime.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#include "mariadb.h"
#include "sql_class.h"
#include "sql_list.h"
#include "sql_sequence.h"
#include "ha_sequence.h"
#include "sql_alter.h"
#include "mysqld.h"
#include "mysqld_error.h"

static void mylite_sequence_not_supported();

handlerton *sql_sequence_hton;

bool sequence_definition::is_allowed_value_type(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONGLONG:
    return true;
  default:
    return false;
  }
}

Type_handler const *sequence_definition::value_type_handler()
{
  const Type_handler *handler=
    Type_handler::get_handler_by_field_type(value_type);
  return is_unsigned ? handler->type_handler_unsigned() : handler;
}

longlong sequence_definition::value_type_max()
{
  return is_unsigned && value_type != MYSQL_TYPE_LONGLONG ?
    ~(~0ULL << 8 * value_type_handler()->calc_pack_length(0)) :
    ~value_type_min();
}

longlong sequence_definition::value_type_min()
{
  return is_unsigned ? 0 :
    ~0ULL << (8 * value_type_handler()->calc_pack_length(0) - 1);
}

longlong sequence_definition::truncate_value(const Longlong_hybrid& original)
{
  if (is_unsigned)
    return original.to_ulonglong(value_type_max());
  if (original.is_unsigned_outside_of_signed_range())
    return value_type_max();
  const longlong value= original.value();
  return (value > value_type_max() ? value_type_max() :
          value < value_type_min() ? value_type_min() : value);
}

bool sequence_definition::check_and_adjust(THD *, bool)
{
  mylite_sequence_not_supported();
  return true;
}

void sequence_definition::store_fields(TABLE *)
{
  mylite_sequence_not_supported();
}

void sequence_definition::read_fields(TABLE *)
{
  mylite_sequence_not_supported();
}

int sequence_definition::write_initial_sequence(TABLE *)
{
  mylite_sequence_not_supported();
  return HA_ERR_UNSUPPORTED;
}

int sequence_definition::write(TABLE *, bool)
{
  mylite_sequence_not_supported();
  return HA_ERR_UNSUPPORTED;
}

void sequence_definition::adjust_values(longlong next_value)
{
  next_free_value= next_value;
  real_increment= increment;
}

bool sequence_definition::prepare_sequence_fields(List<Create_field> *, bool)
{
  mylite_sequence_not_supported();
  return true;
}

SEQUENCE::SEQUENCE() :all_values_used(0), initialized(SEQ_UNINTIALIZED)
{
  mysql_rwlock_init(key_LOCK_SEQUENCE, &mutex);
}

SEQUENCE::~SEQUENCE()
{
  mysql_rwlock_destroy(&mutex);
}

int SEQUENCE::read_initial_values(TABLE *)
{
  mylite_sequence_not_supported();
  return HA_ERR_UNSUPPORTED;
}

int SEQUENCE::read_stored_values(TABLE *)
{
  mylite_sequence_not_supported();
  return HA_ERR_UNSUPPORTED;
}

void SEQUENCE::write_lock(TABLE *)
{
  mylite_sequence_not_supported();
}

void SEQUENCE::write_unlock(TABLE *)
{
}

void SEQUENCE::read_lock(TABLE *)
{
  mylite_sequence_not_supported();
}

void SEQUENCE::read_unlock(TABLE *)
{
}

longlong SEQUENCE::next_value(TABLE *, bool, int *error)
{
  mylite_sequence_not_supported();
  if (error)
    *error= ER_NOT_SUPPORTED_YET;
  return 0;
}

int SEQUENCE::set_value(TABLE *, longlong, ulonglong, bool)
{
  mylite_sequence_not_supported();
  return HA_ERR_UNSUPPORTED;
}

bool SEQUENCE_LAST_VALUE::check_version(TABLE *)
{
  return true;
}

void SEQUENCE_LAST_VALUE::set_version(TABLE *)
{
}

bool check_sequence_fields(LEX *, List<Create_field> *,
                           const LEX_CSTRING, const LEX_CSTRING)
{
  mylite_sequence_not_supported();
  return true;
}

bool sequence_insert(THD *, LEX *, TABLE_LIST *)
{
  mylite_sequence_not_supported();
  return true;
}

bool Sql_cmd_alter_sequence::execute(THD *)
{
  mylite_sequence_not_supported();
  return true;
}

static void mylite_sequence_not_supported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "SQL sequences in the MyLite embedded profile");
}

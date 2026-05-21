/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"

#include "field.h"
#include "log_event.h"
#include "rpl_utility.h"

uint32 Type_handler_newdecimal::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_typelib::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_string::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_time2::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_timestamp2::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_datetime2::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_bit::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_var_string::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_varchar::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_varchar_compressed::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_tiny_blob::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_medium_blob::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_blob::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_blob_compressed::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_long_blob::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

uint32 Type_handler_olddecimal::max_display_length_for_field(
    const Conv_source &) const
{
  return 0;
}

void Type_handler::show_binlog_type(const Conv_source &, const Field &,
                                    String *str) const
{
  if (str)
    str->length(0);
}

void Type_handler_var_string::show_binlog_type(const Conv_source &,
                                               const Field &, String *str) const
{
  if (str)
    str->length(0);
}

void Type_handler_varchar::show_binlog_type(const Conv_source &, const Field &,
                                            String *str) const
{
  if (str)
    str->length(0);
}

void Type_handler_varchar_compressed::show_binlog_type(const Conv_source &,
                                                       const Field &,
                                                       String *str) const
{
  if (str)
    str->length(0);
}

void Type_handler_bit::show_binlog_type(const Conv_source &, const Field &,
                                        String *str) const
{
  if (str)
    str->length(0);
}

void Type_handler_olddecimal::show_binlog_type(const Conv_source &,
                                               const Field &, String *str) const
{
  if (str)
    str->length(0);
}

void Type_handler_newdecimal::show_binlog_type(const Conv_source &,
                                               const Field &, String *str) const
{
  if (str)
    str->length(0);
}

void Type_handler_blob_compressed::show_binlog_type(const Conv_source &,
                                                    const Field &,
                                                    String *str) const
{
  if (str)
    str->length(0);
}

void Type_handler_string::show_binlog_type(const Conv_source &, const Field &,
                                           String *str) const
{
  if (str)
    str->length(0);
}

enum_conv_type Field::rpl_conv_type_from_same_data_type(
    uint16, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_new_decimal::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_real::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_int::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_enum::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_longstr::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_newdate::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_time::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_timef::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_timestamp::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_timestampf::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_datetime::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_datetimef::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_date::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_bit::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_year::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

enum_conv_type Field_null::rpl_conv_type_from(
    const Conv_source &, const Relay_log_info *, const Conv_param &) const
{
  return CONV_TYPE_IMPOSSIBLE;
}

#if defined(HAVE_REPLICATION)

const Type_handler *table_def::field_type_handler(uint) const
{
  return NULL;
}

bool table_def::compatible_with(THD *, rpl_group_info *, TABLE *,
                                TABLE **conv_table_var) const
{
  if (conv_table_var)
    *conv_table_var= NULL;
  return false;
}

TABLE *table_def::create_conversion_table(THD *, rpl_group_info *,
                                          TABLE *) const
{
  return NULL;
}

Deferred_log_events::Deferred_log_events(Relay_log_info *) : last_added(NULL)
{
  my_init_dynamic_array(PSI_INSTRUMENT_ME, &array, sizeof(Log_event *), 0, 0,
                        MYF(0));
}

Deferred_log_events::~Deferred_log_events()
{
  delete_dynamic(&array);
}

int Deferred_log_events::add(Log_event *)
{
  return 1;
}

bool Deferred_log_events::is_empty()
{
  return true;
}

bool Deferred_log_events::execute(rpl_group_info *)
{
  return true;
}

void Deferred_log_events::rewind()
{
}

#endif

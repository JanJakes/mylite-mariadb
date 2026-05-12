/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_class.h"
#include "sql_type_geom.h"
#include "mysqld_error.h"

Named_type_handler<Type_handler_geometry> type_handler_geometry("geometry");
Type_collection_geometry type_collection_geometry;

static bool mylite_geometry_type_unsupported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), "GEOMETRY type");
  return true;
}

LEX_CSTRING Type_handler_geometry::extended_metadata_data_type_name() const
{
  return null_clex_str;
}

const Type_handler_geometry *
Type_handler_geometry::type_handler_geom_by_type(uint)
{
  return &type_handler_geometry;
}

const Type_handler *
Type_collection_geometry_handler_by_name(const LEX_CSTRING &)
{
  return nullptr;
}

const Type_collection *Type_handler_geometry::type_collection() const
{
  return &type_collection_geometry;
}

const Type_handler *
Type_handler_geometry::type_handler_frm_unpack(const uchar *) const
{
  return &type_handler_geometry;
}

const Type_handler *
Type_collection_geometry::aggregate_for_comparison(const Type_handler *,
                                                   const Type_handler *) const
{
  return nullptr;
}

const Type_handler *
Type_collection_geometry::aggregate_for_result(const Type_handler *,
                                               const Type_handler *) const
{
  return nullptr;
}

const Type_handler *
Type_collection_geometry::aggregate_for_min_max(const Type_handler *,
                                                const Type_handler *) const
{
  return nullptr;
}

const Type_handler *
Type_collection_geometry::aggregate_if_string(const Type_handler *,
                                              const Type_handler *) const
{
  return nullptr;
}

#ifndef DBUG_OFF
bool Type_collection_geometry::init_aggregators(Type_handler_data *,
                                                const Type_handler *) const
{
  return false;
}
#endif

bool Type_collection_geometry::init(Type_handler_data *)
{
  return false;
}

bool Type_handler_geometry::check_type_geom_or_binary(const LEX_CSTRING &,
                                                      const Item *)
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::check_types_geom_or_binary(const LEX_CSTRING &,
                                                       Item* const *,
                                                       uint, uint)
{
  return mylite_geometry_type_unsupported();
}

const Type_handler *Type_handler_geometry::type_handler_for_comparison() const
{
  return &type_handler_geometry;
}

Field *Type_handler_geometry::make_conversion_table_field(MEM_ROOT *,
                                                          TABLE *,
                                                          uint,
                                                          const Field *) const
{
  mylite_geometry_type_unsupported();
  return nullptr;
}

bool Type_handler_geometry::
       Column_definition_fix_attributes(Column_definition *) const
{
  return mylite_geometry_type_unsupported();
}

void Type_handler_geometry::
       Column_definition_reuse_fix_attributes(THD *, Column_definition *,
                                              const Field *) const
{
}

bool Type_handler_geometry::
       Column_definition_prepare_stage1(THD *, MEM_ROOT *, Column_definition *,
                                        column_definition_type_t,
                                        const Column_derived_attributes *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Column_definition_prepare_stage2(Column_definition *, handler *,
                                        ulonglong) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::Key_part_spec_init_primary(Key_part_spec *,
                                                       const Column_definition &,
                                                       const handler *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::Key_part_spec_init_unique(Key_part_spec *,
                                                      const Column_definition &,
                                                      const handler *,
                                                      bool *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::Key_part_spec_init_multiple(Key_part_spec *,
                                                        const Column_definition &,
                                                        const handler *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::Key_part_spec_init_foreign(Key_part_spec *,
                                                       const Column_definition &,
                                                       const handler *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::Key_part_spec_init_spatial(Key_part_spec *,
                                                       const Column_definition &)
                                                       const
{
  return mylite_geometry_type_unsupported();
}

Item *Type_handler_geometry::create_typecast_item(THD *, Item *,
                                                  const Type_cast_attributes &)
                                                  const
{
  mylite_geometry_type_unsupported();
  return nullptr;
}

uint32 Type_handler_geometry::calc_pack_length(uint32) const
{
  return 4 + portable_sizeof_char_ptr;
}

Field *Type_handler_geometry::make_table_field(MEM_ROOT *,
                                               const LEX_CSTRING *,
                                               const Record_addr &,
                                               const Type_all_attributes &,
                                               TABLE_SHARE *) const
{
  mylite_geometry_type_unsupported();
  return nullptr;
}

bool Type_handler_geometry::
       Item_hybrid_func_fix_attributes(THD *, const LEX_CSTRING &,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *, Item **,
                                       uint) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_func_round_fix_length_and_dec(Item_func_round *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::Item_func_abs_fix_length_and_dec(Item_func_abs *)
  const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::Item_func_neg_fix_length_and_dec(Item_func_neg *)
  const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_func_signed_fix_length_and_dec(Item_func_signed *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_double_typecast_fix_length_and_dec(Item_double_typecast *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_float_typecast_fix_length_and_dec(Item_float_typecast *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_time_typecast_fix_length_and_dec(Item_time_typecast *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_date_typecast_fix_length_and_dec(Item_date_typecast *) const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
       Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *)
                                                 const
{
  return mylite_geometry_type_unsupported();
}

bool Type_handler_geometry::
  Item_param_set_from_value(THD *, Item_param *param,
                            const Type_all_attributes *, const st_value *) const
{
  param->set_null();
  return mylite_geometry_type_unsupported();
}

void Type_handler_geometry::Item_param_set_param_func(Item_param *param,
                                                      uchar **, ulong) const
{
  param->set_null();
}

Field *Type_handler_geometry::
  make_table_field_from_def(TABLE_SHARE *, MEM_ROOT *, const LEX_CSTRING *,
                            const Record_addr &, const Bit_addr &,
                            const Column_definition_attributes *,
                            uint32) const
{
  mylite_geometry_type_unsupported();
  return nullptr;
}

void Type_handler_geometry::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *,
                                        uchar *) const
{
}

uint Type_handler_geometry::
  Column_definition_gis_options_image(uchar *, const Column_definition &) const
{
  return 0;
}

bool Type_handler_geometry::
  Column_definition_attributes_frm_unpack(Column_definition_attributes *,
                                          TABLE_SHARE *, const uchar *,
                                          LEX_CUSTRING *) const
{
  return mylite_geometry_type_unsupported();
}

uint32 Type_handler_geometry::max_display_length_for_field(const Conv_source &)
                                                    const
{
  return 0;
}

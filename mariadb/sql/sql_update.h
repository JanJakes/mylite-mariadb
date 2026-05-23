/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_UPDATE_INCLUDED
#define SQL_UPDATE_INCLUDED

#include "sql_class.h"                          /* enum_duplicates */
#include "sql_cmd.h"                            // Sql_cmd_dml
#include "sql_base.h"
#include "opt_range.h"

class Item;
struct TABLE_LIST;
class THD;

typedef class st_select_lex SELECT_LEX;
typedef class st_select_lex_unit SELECT_LEX_UNIT;

bool check_unique_table(THD *thd, TABLE_LIST *table_list);
bool records_are_comparable(const TABLE *table);
bool compare_record(const TABLE *table);

/**
   @class Sql_cmd_update - class used for any UPDATE statements

   This class is derived from Sql_cmd_dml and contains implementations
   for abstract virtual function of the latter such as precheck() and
   prepare_inner(). It also overrides the implementation of execute_inner()
   providing a special handling for single-table update statements that
   are not converted to multi-table updates.
   The class provides an object of the Multiupdate_prelocking_strategy class
   for the virtual function get_dml_prelocking_strategy().
*/
class Sql_cmd_update final : public Sql_cmd_dml
{
public:
  ha_rows found{0}, updated{0};
  Sql_cmd_update(bool multitable_arg)
    : orig_multitable(multitable_arg), multitable(multitable_arg)
  {}

  enum_sql_command sql_command_code() const override
  {
    return orig_multitable ? SQLCOM_UPDATE_MULTI : SQLCOM_UPDATE;
  }

  DML_prelocking_strategy *get_dml_prelocking_strategy() override
  {
    return &multiupdate_prelocking_strategy;
  }

  bool processing_as_multitable_update_prohibited(THD *thd);

  bool is_multitable() const { return multitable; }

  void set_as_multitable() { multitable= true; }

  void get_dml_stat (ha_rows &found, ha_rows &changed) override
  {

     found= this->found;
     changed= this->updated;
  }

protected:
  /**
    @brief Perform precheck of table privileges for update statements
  */
  bool precheck(THD *thd) override;

  /**
    @brief Perform context analysis for update statements
  */
  bool prepare_inner(THD *thd) override;

  /**
    @brief Perform optimization and execution actions needed for updates
  */
  bool execute_inner(THD *thd) override;

private:

  /**
    @brief Special handling of single-table updates after prepare phase
  */
  bool update_single_table(THD *thd);

  bool mylite_rebind_prepared_direct_update_shape(THD *thd,
                                                  TABLE_LIST *table_list,
                                                  SELECT_LEX *select_lex,
                                                  bool elide_result);
  bool mylite_prepared_direct_update_shape_matches(TABLE *table,
                                                   Item *condition);
  void store_mylite_prepared_direct_update_shape(
      TABLE *table, uint key_number, Item *key_value,
      bool condition_guaranteed_by_key, bool values_need_setup,
      bool row_only_update);
  void clear_mylite_prepared_direct_update_shape();

  bool mylite_update_values_have_subquery(List<Item> &values);
  bool mylite_update_values_need_setup(List<Item> &values);

  /* Original value of the 'multitable' flag set by constructor */
  const bool orig_multitable;

  /*
    True if the statement is a multi-table update or converted to such.
    For a single-table update this flag is set to true if the statement
    is supposed to be converted to multi-table update.
  */
  bool multitable;

  /* The prelocking strategy used when opening the used tables */
  Multiupdate_prelocking_strategy multiupdate_prelocking_strategy;

  Mylite_update_exact_key_proof_cache mylite_update_exact_key_proof_cache;

  bool mylite_update_values_have_subquery_known{false};
  bool mylite_update_values_have_subquery_cached{false};
  bool mylite_update_values_need_setup_known{false};
  bool mylite_update_values_need_setup_cached{false};
  bool mylite_prepared_direct_update_shape_valid{false};
  uint mylite_prepared_direct_update_shape_table_ref_type{0};
  ulonglong mylite_prepared_direct_update_shape_table_ref_version{0};
  uint mylite_prepared_direct_update_shape_key_number{0};
  uint mylite_prepared_direct_update_shape_key_field_index{0};
  uint mylite_prepared_direct_update_shape_key_field_name_length{0};
  char mylite_prepared_direct_update_shape_key_field_name[NAME_LEN + 1]{0};
  Item *mylite_prepared_direct_update_shape_key_value{NULL};
  bool mylite_prepared_direct_update_shape_condition_guaranteed_by_key{false};
  bool mylite_prepared_direct_update_shape_values_need_setup{false};
  bool mylite_prepared_direct_update_shape_row_only_update{false};

public:
  /* The list of the updating expressions used in the set clause */
  List<Item> *update_value_list;
};

#endif /* SQL_UPDATE_INCLUDED */

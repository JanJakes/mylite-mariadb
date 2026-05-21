/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_i_s.h"
#include "sql_profile.h"
#include "mysqld_error.h"

namespace Show {

ST_FIELD_INFO query_profile_statistics_info[]=
{
  Column("UNSUPPORTED", STiny(1), NOT_NULL, "Unsupported"),
  CEnd()
};

} // namespace Show

int fill_query_profile_statistics_info(THD *, TABLE_LIST *, Item *)
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "statement profiling in the MyLite embedded profile");
  return 1;
}

int make_profile_table_for_show(THD *, ST_SCHEMA_TABLE *)
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "statement profiling in the MyLite embedded profile");
  return 1;
}

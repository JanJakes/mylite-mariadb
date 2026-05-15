/*
  MyLite embedded profile stub for Oracle SQL mode parsing.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"

int ORAparse(THD *)
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "Oracle SQL mode in the MyLite embedded profile");
  return true;
}

#ifndef DBUG_OFF
void turn_parser_debug_on_ORAparse()
{
}
#endif

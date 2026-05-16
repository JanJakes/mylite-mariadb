/*
  MyLite embedded profile stub for mysql.servers foreign-server metadata.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#include "mariadb.h"
#include "sql_servers.h"
#include "mysqld_error.h"

static void mylite_foreign_server_metadata_not_supported();

bool servers_init(bool)
{
  return false;
}

bool servers_reload(THD *)
{
  return false;
}

void servers_free(bool)
{
}

int create_server(THD *, LEX_SERVER_OPTIONS *)
{
  mylite_foreign_server_metadata_not_supported();
  return true;
}

int drop_server(THD *, LEX_SERVER_OPTIONS *)
{
  return ER_NOT_SUPPORTED_YET;
}

int alter_server(THD *, LEX_SERVER_OPTIONS *)
{
  return ER_NOT_SUPPORTED_YET;
}

FOREIGN_SERVER *get_server_by_name(MEM_ROOT *, const char *, FOREIGN_SERVER *)
{
  return NULL;
}

static void mylite_foreign_server_metadata_not_supported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "foreign-server metadata in the MyLite embedded profile");
}

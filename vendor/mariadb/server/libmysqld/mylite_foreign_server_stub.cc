/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_servers.h"
#include "mysqld_error.h"

static int mylite_foreign_server_unsupported();

bool
servers_init(bool)
{
  return false;
}

bool
servers_reload(THD *)
{
  return false;
}

void
servers_free(bool)
{
}

int
create_server(THD *, LEX_SERVER_OPTIONS *)
{
  return mylite_foreign_server_unsupported();
}

int
drop_server(THD *, LEX_SERVER_OPTIONS *)
{
  return mylite_foreign_server_unsupported();
}

int
alter_server(THD *, LEX_SERVER_OPTIONS *)
{
  return mylite_foreign_server_unsupported();
}

FOREIGN_SERVER *
get_server_by_name(MEM_ROOT *, const char *, FOREIGN_SERVER *)
{
  return nullptr;
}

static int
mylite_foreign_server_unsupported()
{
  my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "embedded");
  return ER_OPTION_PREVENTS_STATEMENT;
}

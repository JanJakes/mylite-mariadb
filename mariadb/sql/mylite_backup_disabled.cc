/*
  MyLite embedded profile stub for MariaDB external backup SQL runtime.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#include "mariadb.h"
#include "sql_class.h"
#include "backup.h"
#include "mysqld_error.h"

static void mylite_backup_runtime_not_supported();

static const char *stage_names[]=
{"START", "FLUSH", "BLOCK_DDL", "BLOCK_COMMIT", "END", 0};

TYPELIB backup_stage_names= CREATE_TYPELIB_FOR(stage_names);

void backup_init()
{
}

bool run_backup_stage(THD *thd, backup_stages)
{
  thd->current_backup_stage= BACKUP_FINISHED;
  mylite_backup_runtime_not_supported();
  return true;
}

bool backup_end(THD *thd)
{
  thd->current_backup_stage= BACKUP_FINISHED;
  return false;
}

void backup_set_alter_copy_lock(THD *, TABLE *)
{
}

bool backup_reset_alter_copy_lock(THD *)
{
  return false;
}

bool backup_lock(THD *, TABLE_LIST *)
{
  mylite_backup_runtime_not_supported();
  return true;
}

void backup_unlock(THD *thd)
{
  if (thd->mdl_backup_lock)
    thd->mdl_context.release_lock(thd->mdl_backup_lock);
  thd->mdl_backup_lock= 0;
}

void backup_log_ddl(const backup_log_info *)
{
}

static void mylite_backup_runtime_not_supported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "external backup runtime in the MyLite embedded profile");
}

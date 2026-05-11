/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include <my_global.h>
#include <mysql/plugin.h>
#include "ha_mylite.h"

static handler *mylite_create_handler(handlerton *hton, TABLE_SHARE *table,
                                      MEM_ROOT *mem_root);

static handlerton *mylite_hton;

static const char *ha_mylite_exts[]= {
  NullS
};

static int mylite_init_func(void *p)
{
  DBUG_ENTER("mylite_init_func");

  mylite_hton= static_cast<handlerton *>(p);
  mylite_hton->create= mylite_create_handler;
  mylite_hton->flags= HTON_NO_PARTITION | HTON_TEMPORARY_NOT_SUPPORTED;
  mylite_hton->tablefile_extensions= ha_mylite_exts;

  DBUG_RETURN(0);
}

static handler *mylite_create_handler(handlerton *hton, TABLE_SHARE *table,
                                      MEM_ROOT *mem_root)
{
  return new (mem_root) ha_mylite(hton, table);
}

ha_mylite::ha_mylite(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg)
{}

int ha_mylite::open(const char *, int, uint)
{
  DBUG_ENTER("ha_mylite::open");

  if (!(share= get_share()))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  thr_lock_data_init(&share->lock, &lock, nullptr);

  DBUG_RETURN(0);
}

int ha_mylite::close()
{
  DBUG_ENTER("ha_mylite::close");
  share= nullptr;
  DBUG_RETURN(0);
}

int ha_mylite::create(const char *, TABLE *, HA_CREATE_INFO *)
{
  DBUG_ENTER("ha_mylite::create");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_mylite::rnd_init(bool)
{
  DBUG_ENTER("ha_mylite::rnd_init");
  DBUG_RETURN(0);
}

int ha_mylite::rnd_next(uchar *)
{
  DBUG_ENTER("ha_mylite::rnd_next");
  DBUG_RETURN(HA_ERR_END_OF_FILE);
}

int ha_mylite::rnd_pos(uchar *, uchar *)
{
  DBUG_ENTER("ha_mylite::rnd_pos");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

void ha_mylite::position(const uchar *)
{
  DBUG_ENTER("ha_mylite::position");
  DBUG_VOID_RETURN;
}

int ha_mylite::info(uint)
{
  DBUG_ENTER("ha_mylite::info");
  stats.records= 0;
  stats.deleted= 0;
  stats.data_file_length= 0;
  stats.index_file_length= 0;
  DBUG_RETURN(0);
}

int ha_mylite::external_lock(THD *, int)
{
  DBUG_ENTER("ha_mylite::external_lock");
  DBUG_RETURN(0);
}

THR_LOCK_DATA **ha_mylite::store_lock(THD *, THR_LOCK_DATA **to,
                                      enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type= lock_type;
  *to++= &lock;
  return to;
}

Mylite_share *ha_mylite::get_share()
{
  Mylite_share *tmp_share;

  DBUG_ENTER("ha_mylite::get_share");

  lock_shared_ha_data();
  if (!(tmp_share= static_cast<Mylite_share *>(get_ha_share_ptr())))
  {
    tmp_share= new Mylite_share;
    if (!tmp_share)
      goto err;
    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }

err:
  unlock_shared_ha_data();
  DBUG_RETURN(tmp_share);
}

struct st_mysql_storage_engine mylite_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(mylite)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &mylite_storage_engine,
  "MYLITE",
  "MyLite contributors",
  "MyLite storage engine skeleton",
  PLUGIN_LICENSE_GPL,
  mylite_init_func,
  nullptr,
  0x0001,
  nullptr,
  nullptr,
  "0.1",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;

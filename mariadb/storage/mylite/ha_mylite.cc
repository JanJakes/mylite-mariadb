/* Copyright (c) 2026, MyLite contributors.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include <my_global.h>
#include <mysql/plugin.h>

#include "ha_mylite.h"
#include "sql_class.h"

static handler *mylite_create_handler(handlerton *hton,
                                      TABLE_SHARE *table,
                                      MEM_ROOT *mem_root);

static const char *ha_mylite_exts[]= {
  NullS
};

handlerton *mylite_hton;

Mylite_share::Mylite_share()
{
  thr_lock_init(&lock);
}

static int mylite_init_func(void *p)
{
  DBUG_ENTER("mylite_init_func");

  mylite_hton= static_cast<handlerton *>(p);
  mylite_hton->create= mylite_create_handler;
  mylite_hton->tablefile_extensions= ha_mylite_exts;

  DBUG_RETURN(0);
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

static handler *mylite_create_handler(handlerton *hton,
                                      TABLE_SHARE *table,
                                      MEM_ROOT *mem_root)
{
  return new (mem_root) ha_mylite(hton, table);
}

ha_mylite::ha_mylite(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg), share(NULL)
{}

int ha_mylite::open(const char *, int, uint)
{
  DBUG_ENTER("ha_mylite::open");

  if (!(share= get_share()))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  thr_lock_data_init(&share->lock, &lock, NULL);

  DBUG_RETURN(0);
}

int ha_mylite::close(void)
{
  DBUG_ENTER("ha_mylite::close");
  DBUG_RETURN(0);
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

int ha_mylite::create(const char *, TABLE *, HA_CREATE_INFO *)
{
  DBUG_ENTER("ha_mylite::create");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

THR_LOCK_DATA **ha_mylite::store_lock(THD *, THR_LOCK_DATA **to,
                                      enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type= lock_type;

  *to++= &lock;
  return to;
}

struct st_mysql_storage_engine mylite_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(mylite_se)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &mylite_storage_engine,
  "MYLITE",
  "MyLite contributors",
  "MyLite storage engine skeleton",
  PLUGIN_LICENSE_GPL,
  mylite_init_func,                            /* Plugin Init */
  NULL,                                        /* Plugin Deinit */
  0x0001,                                      /* version number (0.1) */
  NULL,                                        /* status variables */
  NULL,                                        /* system variables */
  "0.1",                                       /* string version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL         /* maturity */
}
maria_declare_plugin_end;

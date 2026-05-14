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

#define MYSQL_SERVER 1

#include <my_global.h>
#include <m_ctype.h>
#include <mylite/storage.h>
#include <mysql/plugin.h>

#include "ha_mylite.h"
#include "handler.h"
#include "sql_class.h"
#include "sql_cmd.h"

static handler *mylite_create_handler(handlerton *hton,
                                      TABLE_SHARE *table,
                                      MEM_ROOT *mem_root);
static int mylite_discover_table(handlerton *hton, THD *thd,
                                 TABLE_SHARE *share);
static int mylite_discover_table_names(handlerton *hton, const LEX_CSTRING *db,
                                       MY_DIR *dir,
                                       handlerton::discovered_list *result);
static int mylite_discover_table_existence(handlerton *hton, const char *db,
                                           const char *table_name);
static int mylite_done_func(void *p);
static int mylite_add_discovered_table(void *ctx, const char *schema_name,
                                       const char *table_name);
static const char *mylite_primary_file_path();
static int mylite_requested_engine_name(HA_CREATE_INFO *create_info,
                                        char *out_name, size_t out_name_size);
static int mylite_copy_engine_name(const LEX_CSTRING *engine_name,
                                   char *out_name, size_t out_name_size);
static bool mylite_supported_engine_request(const char *engine_name);
static bool mylite_engine_name_equals(const char *engine_name,
                                      const char *expected_engine_name);
static int mylite_table_name_from_path(const char *path, char *out_schema_name,
                                       size_t out_schema_name_size,
                                       char *out_table_name,
                                       size_t out_table_name_size);
static int mylite_storage_to_handler_error(mylite_storage_result result);

static const char *ha_mylite_exts[]= {
  NullS
};

handlerton *mylite_hton;
static char *mylite_primary_file;

static MYSQL_SYSVAR_STR(primary_file, mylite_primary_file,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Primary .mylite file path", NULL, NULL, NULL);

static struct st_mysql_sys_var *mylite_system_variables[]=
{
  MYSQL_SYSVAR(primary_file),
  NULL
};

Mylite_share::Mylite_share()
{
  thr_lock_init(&lock);
}

static int mylite_init_func(void *p)
{
  DBUG_ENTER("mylite_init_func");

  mylite_hton= static_cast<handlerton *>(p);
  mylite_hton->create= mylite_create_handler;
  mylite_hton->discover_table= mylite_discover_table;
  mylite_hton->discover_table_names= mylite_discover_table_names;
  mylite_hton->discover_table_existence= mylite_discover_table_existence;
  mylite_hton->tablefile_extensions= ha_mylite_exts;

  DBUG_RETURN(0);
}

static int mylite_done_func(void *)
{
  mylite_primary_file= NULL;
  return 0;
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

struct Mylite_discover_context
{
  handlerton::discovered_list *result;
};

static int mylite_discover_table(handlerton *, THD *thd, TABLE_SHARE *share)
{
  DBUG_ENTER("mylite_discover_table");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);

  unsigned char *frm= NULL;
  size_t frm_len= 0;
  mylite_storage_result storage_result=
    mylite_storage_read_table_definition(primary_file, share->db.str,
                                         share->table_name.str, &frm,
                                         &frm_len);
  if (storage_result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(storage_result));

  int error= share->init_from_binary_frm_image(thd, false, frm, frm_len);
  mylite_storage_free(frm);
  DBUG_RETURN((my_errno= error));
}

static int mylite_discover_table_names(handlerton *, const LEX_CSTRING *db,
                                       MY_DIR *,
                                       handlerton::discovered_list *result)
{
  DBUG_ENTER("mylite_discover_table_names");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(0);

  Mylite_discover_context ctx= {result};
  mylite_storage_result storage_result=
    mylite_storage_list_tables(primary_file, db->str, mylite_add_discovered_table,
                               &ctx);
  DBUG_RETURN(storage_result == MYLITE_STORAGE_OK ? 0 : 1);
}

static int mylite_discover_table_existence(handlerton *, const char *db,
                                           const char *table_name)
{
  DBUG_ENTER("mylite_discover_table_existence");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(0);

  mylite_storage_result storage_result=
    mylite_storage_table_exists(primary_file, db, table_name);
  DBUG_RETURN(storage_result == MYLITE_STORAGE_OK ? 1 : 0);
}

static int mylite_add_discovered_table(void *ctx, const char *,
                                       const char *table_name)
{
  Mylite_discover_context *discover_ctx=
    static_cast<Mylite_discover_context *>(ctx);
  return discover_ctx->result->add_table(table_name, strlen(table_name)) ? 1 : 0;
}

static const char *mylite_primary_file_path()
{
  return mylite_primary_file && mylite_primary_file[0] ? mylite_primary_file :
                                                         NULL;
}

static int mylite_storage_to_handler_error(mylite_storage_result result)
{
  switch (result) {
  case MYLITE_STORAGE_OK:
    return 0;
  case MYLITE_STORAGE_NOMEM:
    return HA_ERR_OUT_OF_MEM;
  case MYLITE_STORAGE_READONLY:
    return HA_ERR_TABLE_READONLY;
  case MYLITE_STORAGE_IOERR:
    return HA_ERR_CRASHED;
  case MYLITE_STORAGE_CORRUPT:
    return HA_ERR_CRASHED_ON_USAGE;
  case MYLITE_STORAGE_NOTFOUND:
    return HA_ERR_NO_SUCH_TABLE;
  case MYLITE_STORAGE_FULL:
    return HA_ERR_RECORD_FILE_FULL;
  case MYLITE_STORAGE_MISUSE:
    return HA_ERR_INTERNAL_ERROR;
  case MYLITE_STORAGE_UNSUPPORTED:
    return HA_ERR_OLD_FILE;
  case MYLITE_STORAGE_ERROR:
    return HA_ERR_GENERIC;
  }

  return HA_ERR_GENERIC;
}

ha_mylite::ha_mylite(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg), share(NULL), scan_rows(NULL), scan_row_size(0),
   scan_row_count(0), scan_row_index(0)
{}

ha_mylite::~ha_mylite()
{
  clear_scan_rows();
}

void ha_mylite::clear_scan_rows()
{
  mylite_storage_free(scan_rows);
  scan_rows= NULL;
  scan_row_size= 0;
  scan_row_count= 0;
  scan_row_index= 0;
}

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
  clear_scan_rows();
  DBUG_RETURN(0);
}

int ha_mylite::rnd_init(bool scan)
{
  DBUG_ENTER("ha_mylite::rnd_init");

  clear_scan_rows();
  if (!scan)
    DBUG_RETURN(0);

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  mylite_storage_rowset rowset= {sizeof(rowset), NULL, 0, 0};
  const mylite_storage_result result=
    mylite_storage_read_rows(primary_file, table->s->db.str,
                             table->s->table_name.str, &rowset);
  if (result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(result));
  if (rowset.row_count > 0 && rowset.row_size != table->s->reclength)
  {
    mylite_storage_free(rowset.rows);
    DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
  }

  scan_rows= rowset.rows;
  scan_row_size= rowset.row_size;
  scan_row_count= rowset.row_count;
  scan_row_index= 0;
  DBUG_RETURN(0);
}

int ha_mylite::rnd_end(void)
{
  DBUG_ENTER("ha_mylite::rnd_end");
  clear_scan_rows();
  DBUG_RETURN(0);
}

int ha_mylite::rnd_next(uchar *buf)
{
  DBUG_ENTER("ha_mylite::rnd_next");
  if (scan_row_index >= scan_row_count)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  memcpy(buf, scan_rows + (scan_row_index * scan_row_size), scan_row_size);
  ++scan_row_index;
  DBUG_RETURN(0);
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

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(0);

  unsigned long long row_count= 0;
  const mylite_storage_result result=
    mylite_storage_count_rows(primary_file, table->s->db.str,
                              table->s->table_name.str, &row_count);
  if (result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(result));

  stats.records= (ha_rows) row_count;
  DBUG_RETURN(0);
}

int ha_mylite::external_lock(THD *, int)
{
  DBUG_ENTER("ha_mylite::external_lock");
  DBUG_RETURN(0);
}

int ha_mylite::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_mylite::write_row");

  if (table->s->keys != 0 || table->next_number_field)
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  const mylite_storage_result result=
    mylite_storage_append_row(primary_file, table->s->db.str,
                              table->s->table_name.str, buf,
                              table->s->reclength);
  DBUG_RETURN(mylite_storage_to_handler_error(result));
}

int ha_mylite::create(const char *, TABLE *form, HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_mylite::create");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  char requested_engine_name[NAME_LEN + 1];
  int engine_name_error=
    mylite_requested_engine_name(create_info, requested_engine_name,
                                 sizeof(requested_engine_name));
  if (engine_name_error)
    DBUG_RETURN(engine_name_error);
  if (!mylite_supported_engine_request(requested_engine_name))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  const uchar *frm= NULL;
  size_t frm_len= 0;
  if (form->s->read_frm_image(&frm, &frm_len))
    DBUG_RETURN(HA_ERR_GENERIC);

  const mylite_storage_table_definition definition= {
    sizeof(definition),
    form->s->db.str,
    form->s->table_name.str,
    requested_engine_name,
    MYLITE_STORAGE_ENGINE_NAME,
    frm,
    frm_len
  };
  const mylite_storage_result result=
    mylite_storage_store_table_definition(primary_file, &definition);
  form->s->free_frm_image(frm);

  DBUG_RETURN(mylite_storage_to_handler_error(result));
}

int ha_mylite::delete_table(const char *name)
{
  DBUG_ENTER("ha_mylite::delete_table");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  char schema_name[NAME_LEN + 1];
  char table_name[NAME_LEN + 1];
  int path_error= mylite_table_name_from_path(name, schema_name,
                                              sizeof(schema_name), table_name,
                                              sizeof(table_name));
  if (path_error)
    DBUG_RETURN(path_error);

  const mylite_storage_result result=
    mylite_storage_drop_table(primary_file, schema_name, table_name);
  DBUG_RETURN(mylite_storage_to_handler_error(result));
}

int ha_mylite::rename_table(const char *from, const char *to)
{
  DBUG_ENTER("ha_mylite::rename_table");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  char old_schema_name[NAME_LEN + 1];
  char old_table_name[NAME_LEN + 1];
  int path_error= mylite_table_name_from_path(from, old_schema_name,
                                              sizeof(old_schema_name),
                                              old_table_name,
                                              sizeof(old_table_name));
  if (path_error)
    DBUG_RETURN(path_error);

  char new_schema_name[NAME_LEN + 1];
  char new_table_name[NAME_LEN + 1];
  path_error= mylite_table_name_from_path(to, new_schema_name,
                                          sizeof(new_schema_name),
                                          new_table_name,
                                          sizeof(new_table_name));
  if (path_error)
    DBUG_RETURN(path_error);

  const mylite_storage_result result=
    mylite_storage_rename_table(primary_file, old_schema_name, old_table_name,
                                new_schema_name, new_table_name);
  DBUG_RETURN(mylite_storage_to_handler_error(result));
}

static int mylite_requested_engine_name(HA_CREATE_INFO *create_info,
                                        char *out_name, size_t out_name_size)
{
  if (!(create_info->used_fields & HA_CREATE_USED_ENGINE))
  {
    static const LEX_CSTRING default_engine= {STRING_WITH_LEN("DEFAULT")};
    return mylite_copy_engine_name(&default_engine, out_name, out_name_size);
  }

  THD *thd= current_thd;
  if (!thd || !thd->lex || !thd->lex->m_sql_cmd)
    return HA_ERR_UNSUPPORTED;

  Storage_engine_name *storage_engine_name=
    thd->lex->m_sql_cmd->option_storage_engine_name();
  if (!storage_engine_name || !storage_engine_name->name()->str)
    return HA_ERR_UNSUPPORTED;

  return mylite_copy_engine_name(storage_engine_name->name(), out_name,
                                 out_name_size);
}

static int mylite_copy_engine_name(const LEX_CSTRING *engine_name,
                                   char *out_name, size_t out_name_size)
{
  if (!engine_name || !engine_name->str || engine_name->length == 0 ||
      engine_name->length >= out_name_size)
    return HA_ERR_UNSUPPORTED;

  memcpy(out_name, engine_name->str, engine_name->length);
  out_name[engine_name->length]= '\0';
  return 0;
}

static bool mylite_supported_engine_request(const char *engine_name)
{
  return mylite_engine_name_equals(engine_name, "DEFAULT") ||
         mylite_engine_name_equals(engine_name, MYLITE_STORAGE_ENGINE_NAME) ||
         mylite_engine_name_equals(engine_name, "InnoDB") ||
         mylite_engine_name_equals(engine_name, "MyISAM") ||
         mylite_engine_name_equals(engine_name, "Aria");
}

static bool mylite_engine_name_equals(const char *engine_name,
                                      const char *expected_engine_name)
{
  return my_strcasecmp_latin1(engine_name, expected_engine_name) == 0;
}

static int mylite_table_name_from_path(const char *path, char *out_schema_name,
                                       size_t out_schema_name_size,
                                       char *out_table_name,
                                       size_t out_table_name_size)
{
  if (!path || !path[0])
    return HA_ERR_UNSUPPORTED;

  const char *table_start= path + strlen(path);
  while (table_start > path && table_start[-1] != '/' &&
         table_start[-1] != '\\')
    --table_start;

  const char *schema_end= table_start;
  if (schema_end > path &&
      (schema_end[-1] == '/' || schema_end[-1] == '\\'))
    --schema_end;

  const char *schema_start= schema_end;
  while (schema_start > path && schema_start[-1] != '/' &&
         schema_start[-1] != '\\')
    --schema_start;

  const size_t schema_name_length= (size_t)(schema_end - schema_start);
  const size_t table_name_length= strlen(table_start);
  if (schema_name_length == 0 || table_name_length == 0 ||
      schema_name_length >= out_schema_name_size ||
      table_name_length >= out_table_name_size)
    return HA_ERR_UNSUPPORTED;

  memcpy(out_schema_name, schema_start, schema_name_length);
  out_schema_name[schema_name_length]= '\0';
  memcpy(out_table_name, table_start, table_name_length);
  out_table_name[table_name_length]= '\0';
  return 0;
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
  mylite_done_func,                            /* Plugin Deinit */
  0x0001,                                      /* version number (0.1) */
  NULL,                                        /* status variables */
  mylite_system_variables,                     /* system variables */
  "0.1",                                       /* string version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL         /* maturity */
}
maria_declare_plugin_end;

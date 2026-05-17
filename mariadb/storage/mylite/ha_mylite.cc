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
#include <stdint.h>
#include <stdlib.h>

#include "ha_mylite.h"
#include "field.h"
#include "handler.h"
#include "mylite_schema_hook.h"
#include "mylite_volatile_rows.h"
#include "key.h"
#include "sql_class.h"
#include "sql_cmd.h"
#include "sql_show.h"
#include "sql_string.h"
#include "table.h"

struct Mylite_catalog_table_share
{
  TABLE_SHARE share;
  char path[FN_REFLEN + 1];
  bool initialized;
};

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
static int mylite_close_connection(THD *thd);
static int mylite_commit(THD *thd, bool all);
static int mylite_rollback(THD *thd, bool all);
static int mylite_done_func(void *p);
static int mylite_add_discovered_table(void *ctx, const char *schema_name,
                                       const char *table_name);
static const char *mylite_primary_file_path();
static bool mylite_schema_hook_active();
static int mylite_schema_hook_exists(const char *schema_name);
static int mylite_schema_hook_store(const char *schema_name,
                                    const char *default_character_set_name,
                                    const char *default_collation_name,
                                    const char *schema_comment,
                                    size_t schema_comment_size);
static int mylite_schema_hook_drop(const char *schema_name);
static int mylite_schema_hook_read(const char *schema_name,
                                   Mylite_schema_options *options);
static void mylite_schema_hook_free(Mylite_schema_options *options);
static int mylite_schema_hook_list_schemas(
  mylite_schema_name_callback callback, void *ctx);
static int mylite_schema_hook_list_tables(const char *schema_name,
                                          mylite_schema_name_callback callback,
                                          void *ctx);
static int mylite_schema_hook_add_table(void *ctx, const char *,
                                        const char *table_name);
static bool mylite_is_user_temporary_table_share(const TABLE_SHARE *share);
static bool mylite_current_command_creates_user_temporary_table();
static bool mylite_foreign_key_checks_disabled(THD *thd);
static int mylite_requested_engine_name(const char *primary_file,
                                        HA_CREATE_INFO *create_info,
                                        char *out_name, size_t out_name_size);
static int mylite_display_engine_name(const char *primary_file,
                                      const char *schema_name,
                                      const char *table_name, char *out_name,
                                      size_t out_name_size);
static bool mylite_preserves_requested_engine_name(const THD *thd);
static int mylite_preserve_source_requested_engine_name(
  const char *primary_file, char *out_name, size_t out_name_size);
static TABLE_LIST *mylite_requested_engine_source_table(const THD *thd);
static int mylite_copy_engine_name(const LEX_CSTRING *engine_name,
                                   char *out_name, size_t out_name_size);
static int mylite_copy_lex_string(const LEX_CSTRING *value, char *out_value,
                                  size_t out_value_size);
static int mylite_copy_string(const char *value, char *out_value,
                              size_t out_value_size);
static bool mylite_supported_engine_request(const char *engine_name);
static bool mylite_discards_rows_engine_request(const char *engine_name);
static bool mylite_uses_volatile_rows_engine_request(const char *engine_name);
static bool mylite_engine_name_equals(const char *engine_name,
                                      const char *expected_engine_name);
static int mylite_begin_transaction_checkpoint(THD *thd,
                                               const char *primary_file);
static int mylite_begin_statement_checkpoint(THD *thd,
                                             const char *primary_file);
static int mylite_finish_statement_checkpoint(THD *thd, bool commit);
static int mylite_finish_transaction_checkpoint(THD *thd, bool commit);
static struct Mylite_trx_context *mylite_trx_context(THD *thd, bool create);
static int mylite_table_name_from_path(const char *path, char *out_schema_name,
                                       size_t out_schema_name_size,
                                       char *out_table_name,
                                       size_t out_table_name_size);
static bool mylite_is_alter_backup_table_name(const char *table_name);
static int mylite_drop_foreign_keys_for_alter(const char *primary_file,
                                              const char *schema_name,
                                              const char *table_name);
static int mylite_drop_foreign_key_for_alter(const char *primary_file,
                                             const char *schema_name,
                                             const char *table_name,
                                             const Lex_ident_column &name);
static bool mylite_table_supports_row_write(TABLE *table);
static bool mylite_table_supports_row_lifecycle(TABLE *table);
static bool mylite_table_supports_auto_increment(TABLE *table);
static bool mylite_table_supports_indexes(TABLE *table);
static bool mylite_key_is_supported(const KEY *key);
static Field *mylite_auto_increment_field(TABLE *table);
static int mylite_prepare_row_payload(TABLE *table, const uchar *buf,
                                      const uchar **out_payload,
                                      size_t *out_payload_size,
                                      uchar **out_owned_payload);
static int mylite_prepare_index_entries(
  TABLE *table, const uchar *buf, mylite_storage_index_entry **out_entries,
  size_t *out_entry_count, uchar **out_key_storage);
static void mylite_free_index_entries(mylite_storage_index_entry *entries,
                                      uchar *key_storage);
static int mylite_prepare_scan_rows(TABLE *table,
                                    const mylite_storage_rowset *rowset,
                                    uchar **out_rows,
                                    size_t *out_row_size,
                                    size_t *out_row_count,
                                    ulonglong **out_row_ids,
                                    uchar **out_blob_payloads,
                                    size_t *out_blob_payloads_size);
static ulonglong mylite_first_auto_increment_value(ulonglong next_value,
                                                   ulonglong offset,
                                                   ulonglong increment);
static int mylite_check_duplicate_keys(
  const char *primary_file, const char *schema_name, const char *table_name,
  TABLE *table, const mylite_storage_index_entry *index_entries,
  size_t index_entry_count, const uchar *buf, ulonglong skip_row_id,
  uint *out_duplicate_key);
static int mylite_check_volatile_duplicate_keys(
  const char *primary_file, const char *schema_name, const char *table_name,
  TABLE *table, const mylite_storage_index_entry *index_entries,
  size_t index_entry_count, const uchar *buf, ulonglong skip_row_id,
  uint *out_duplicate_key);
static int mylite_check_child_foreign_keys(const char *primary_file,
                                           const char *schema_name,
                                           const char *table_name,
                                           TABLE *table, const uchar *buf);
static int mylite_check_parent_foreign_keys(const char *primary_file,
                                            const char *schema_name,
                                            const char *table_name,
                                            TABLE *table,
                                            const uchar *old_data,
                                            const uchar *new_data);
static int mylite_validate_foreign_key_definitions(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, HA_CREATE_INFO *create_info,
  bool volatile_or_discarded_rows);
static int mylite_validate_retained_foreign_keys(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, HA_CREATE_INFO *create_info);
static int mylite_validate_retained_child_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata);
static int mylite_validate_retained_parent_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata);
static const char *mylite_foreign_key_referenced_key_name_for_alter(
  HA_CREATE_INFO *create_info, const char *referenced_key_name);
static const char *mylite_foreign_key_referenced_key_name_in_alter_info(
  Alter_info *alter_info, const char *referenced_key_name);
static bool mylite_alter_renames_keys(HA_CREATE_INFO *create_info);
static Alter_info *mylite_current_alter_info_for_key_rename();
static bool mylite_alter_drops_foreign_key(
  HA_CREATE_INFO *create_info,
  const mylite_storage_foreign_key_metadata *metadata);
static bool mylite_foreign_key_is_self_referencing(
  const mylite_storage_foreign_key_metadata *metadata);
static int mylite_reject_dropped_foreign_key_supporting_key(
  const mylite_storage_foreign_key_metadata *metadata,
  const char *fallback_key_name);
static int mylite_update_renamed_parent_foreign_keys(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, HA_CREATE_INFO *create_info);
static int mylite_update_renamed_parent_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata);
static int mylite_store_foreign_key_definitions(
  const char *primary_file, const char *storage_schema_name,
  const char *storage_table_name, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, HA_CREATE_INFO *create_info);
static int mylite_validate_foreign_key_definition(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, const Foreign_key *fk,
  uint foreign_key_number, bool volatile_or_discarded_rows);
static int mylite_store_foreign_key_definition(
  const char *primary_file, const char *storage_schema_name,
  const char *storage_table_name, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, const Foreign_key *fk,
  uint foreign_key_number);
static int mylite_prepare_foreign_key_definition(
  THD *thd, const char *storage_schema_name, const char *storage_table_name,
  const char *logical_schema_name, const char *logical_table_name, TABLE *form,
  const Foreign_key *fk, uint foreign_key_number,
  mylite_storage_foreign_key_definition *out_definition);
static int mylite_validate_foreign_key_shape(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, THD *thd,
  mylite_storage_foreign_key_definition *definition);
static int mylite_validate_foreign_key_parent_engine(
  const char *primary_file, const char *schema_name, const char *table_name);
static int mylite_validate_foreign_key_parent_share(
  mylite_storage_foreign_key_definition *definition, TABLE *child_table,
  const TABLE_SHARE *parent_share);
static int mylite_validate_foreign_key_key_parts(
  const KEY *child_key, const KEY *parent_key, size_t column_count);
static int mylite_init_catalog_table_share(
  THD *thd, const char *primary_file, const char *schema_name,
  const char *table_name, Mylite_catalog_table_share *out_share);
static void mylite_free_catalog_table_share(
  Mylite_catalog_table_share *catalog_share);
static int mylite_foreign_key_columns_from_list(
  THD *thd, List<Key_part_spec> *columns, const char ***out_column_names,
  size_t *out_column_count);
static const char *mylite_foreign_key_constraint_name(
  THD *thd, const char *logical_table_name, const Foreign_key *fk,
  uint foreign_key_number);
static unsigned mylite_foreign_key_storage_action(enum_fk_option action);
static unsigned mylite_foreign_key_storage_match(
  Foreign_key::fk_match_opt match_option);
static bool mylite_foreign_key_action_supported(enum_fk_option action);
static unsigned long long mylite_key_nullable_bitmap(const KEY *key,
                                                     size_t column_count);
static int mylite_check_child_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata);
static int mylite_check_parent_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata);
static const KEY *mylite_find_foreign_key_prefix(
  TABLE *table, const char *const *column_names, size_t column_count,
  const char *key_name);
static const KEY *mylite_find_foreign_key_prefix_in_share(
  const TABLE_SHARE *share, const char *const *column_names,
  size_t column_count, const char *key_name, bool require_unique_exact_key);
static const KEY *mylite_find_foreign_key_prefix_in_keys(
  const KEY *keys, uint key_count, const char *const *column_names,
  size_t column_count, const char *key_name, bool require_unique_exact_key);
static bool mylite_key_prefix_contains_null(TABLE *table, const KEY *key,
                                            const uchar *buf,
                                            size_t column_count);
static int mylite_make_key_prefix(TABLE *table, const KEY *key,
                                  const uchar *buf, size_t column_count,
                                  unsigned long long target_nullable_bitmap,
                                  uchar **out_key, size_t *out_key_size);
static int mylite_check_same_row_self_reference(
  TABLE *table, const mylite_storage_foreign_key_metadata *metadata,
  const uchar *new_data, const uchar *child_key_prefix,
  size_t child_key_prefix_size, bool *out_matches);
static bool mylite_key_prefix_equals(const uchar *left, size_t left_size,
                                     const uchar *right, size_t right_size);
static bool mylite_unique_key_allows_duplicate_null(TABLE *table,
                                                    const KEY *key,
                                                    const uchar *buf);
static int mylite_advance_auto_increment_from_row(const char *primary_file,
                                                  const char *schema_name,
                                                  const char *table_name,
                                                  TABLE *table);
static int mylite_find_index_entry(TABLE *table,
                                   const Mylite_index_cursor_entry *entries,
                                   size_t entry_count, const uchar *keys,
                                   uint index_number, const uchar *key,
                                   uint key_length,
                                   enum ha_rkey_function find_flag,
                                   size_t *out_entry_index);
static int mylite_compare_key_tuple(TABLE *table, uint index_number,
                                    const uchar *left_key,
                                    size_t left_key_size,
                                    const uchar *right_key,
                                    size_t right_key_size,
                                    uint key_length, int *out_cmp);
static int mylite_sort_index_entries(TABLE *table, uint index_number,
                                     const uchar *keys,
                                     Mylite_index_cursor_entry *entries,
                                     size_t entry_count);
static int mylite_compare_index_entries(TABLE *table, uint index_number,
                                        const uchar *keys,
                                        const Mylite_index_cursor_entry *left,
                                        const Mylite_index_cursor_entry *right,
                                        int *out_cmp);
static int mylite_append_foreign_key_create_info(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata);
static int mylite_append_foreign_key_clause(
  THD *thd, String *text,
  const mylite_storage_foreign_key_metadata *metadata);
static int mylite_append_foreign_key_column_names(THD *thd, String *text,
                                                  char **column_names,
                                                  size_t column_count);
static int mylite_append_foreign_key_referenced_table(
  THD *thd, String *text,
  const mylite_storage_foreign_key_metadata *metadata);
static const char *mylite_foreign_key_action_name(unsigned action);
static int mylite_add_foreign_key_info(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata);
static int mylite_detect_foreign_key_presence(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata);
static int mylite_match_foreign_key_to_drop(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata);
static FOREIGN_KEY_INFO *mylite_foreign_key_info_from_metadata(
  THD *thd, const mylite_storage_foreign_key_metadata *metadata,
  bool map_referenced_key_name);
static LEX_CSTRING *mylite_make_foreign_key_string(THD *thd,
                                                   const char *value);
static int mylite_append_foreign_key_column_names(THD *thd,
                                                  List<LEX_CSTRING> *list,
                                                  char **column_names,
                                                  size_t column_count);
static void mylite_set_foreign_key_nullable_bits(
  THD *thd, FOREIGN_KEY_INFO *info,
  const mylite_storage_foreign_key_metadata *metadata);
static enum_fk_option mylite_foreign_key_option(unsigned action);
static int mylite_storage_to_handler_error(mylite_storage_result result);
static int mylite_storage_to_schema_hook_result(mylite_storage_result result);
static bool mylite_table_has_blob_fields(TABLE *table);
static int mylite_serialize_blob_row(TABLE *table, const uchar *buf,
                                     uchar **out_payload,
                                     size_t *out_payload_size);
static int mylite_scan_stored_row(TABLE *table, const uchar *payload,
                                  size_t payload_size,
                                  size_t *out_blob_payload_size);
static int mylite_copy_stored_row_to_scan(TABLE *table, const uchar *payload,
                                          size_t payload_size,
                                          uchar *out_row,
                                          uchar *blob_payloads,
                                          size_t *inout_blob_payloads_used);
static int mylite_field_record_offset(TABLE *table, Field *field,
                                      size_t *out_offset);
static bool mylite_is_blob_row_payload(const uchar *payload,
                                       size_t payload_size);
static unsigned mylite_get_u32(const uchar *ptr);
static void mylite_put_u32(uchar *ptr, unsigned value);
static ulonglong mylite_get_u64(const uchar *ptr);
static void mylite_put_u64(uchar *ptr, ulonglong value);

static const char *ha_mylite_exts[]= {
  NullS
};

handlerton *mylite_hton;
static char *mylite_primary_file;

static const uchar mylite_blob_row_magic[8]=
  {'M', 'Y', 'L', 'R', 'B', '1', '\0', '\0'};
static const unsigned MYLITE_BLOB_ROW_VERSION= 1U;
static const size_t MYLITE_BLOB_ROW_MAGIC_OFFSET= 0U;
static const size_t MYLITE_BLOB_ROW_VERSION_OFFSET= 8U;
static const size_t MYLITE_BLOB_ROW_RECORD_SIZE_OFFSET= 12U;
static const size_t MYLITE_BLOB_ROW_BLOB_COUNT_OFFSET= 16U;
static const size_t MYLITE_BLOB_ROW_BLOB_BYTES_OFFSET= 20U;
static const size_t MYLITE_BLOB_ROW_HEADER_SIZE= 24U;
static const size_t MYLITE_BLOB_ROW_DESCRIPTOR_SIZE= 8U;

static MYSQL_SYSVAR_STR(primary_file, mylite_primary_file,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Primary .mylite file path", NULL, NULL, NULL);

static struct st_mysql_sys_var *mylite_system_variables[]=
{
  MYSQL_SYSVAR(primary_file),
  NULL
};

static const Mylite_schema_hooks mylite_schema_hooks=
{
  mylite_schema_hook_active,
  mylite_schema_hook_exists,
  mylite_schema_hook_store,
  mylite_schema_hook_drop,
  mylite_schema_hook_read,
  mylite_schema_hook_free,
  mylite_schema_hook_list_schemas,
  mylite_schema_hook_list_tables
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
  mylite_hton->close_connection= mylite_close_connection;
  mylite_hton->commit= mylite_commit;
  mylite_hton->rollback= mylite_rollback;
  mylite_hton->flags|= HTON_SUPPORTS_FOREIGN_KEYS;
  mylite_hton->tablefile_extensions= ha_mylite_exts;
  mylite_register_schema_hooks(&mylite_schema_hooks);

  DBUG_RETURN(0);
}

static int mylite_done_func(void *)
{
  mylite_volatile_clear_tables();
  mylite_register_schema_hooks(NULL);
  mylite_primary_file= NULL;
  return 0;
}

static int mylite_close_connection(THD *thd)
{
  DBUG_ENTER("mylite_close_connection");

  int error= mylite_finish_statement_checkpoint(thd, false);
  int transaction_error= mylite_finish_transaction_checkpoint(thd, false);
  if (!error)
    error= transaction_error;
  Mylite_trx_context *ctx= mylite_trx_context(thd, false);
  if (ctx)
  {
    free(ctx);
    thd->ha_data[mylite_hton->slot].ha_ptr= NULL;
  }

  DBUG_RETURN(error);
}

static int mylite_commit(THD *thd, bool all)
{
  DBUG_ENTER("mylite_commit");

  int error= mylite_finish_statement_checkpoint(thd, true);
  if (all)
  {
    int transaction_error= mylite_finish_transaction_checkpoint(thd, true);
    if (!error)
      error= transaction_error;
  }
  DBUG_RETURN(error);
}

static int mylite_rollback(THD *thd, bool all)
{
  DBUG_ENTER("mylite_rollback");

  int error= mylite_finish_statement_checkpoint(thd, false);
  if (all)
  {
    int transaction_error= mylite_finish_transaction_checkpoint(thd, false);
    if (!error)
      error= transaction_error;
  }
  DBUG_RETURN(error);
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

struct Mylite_trx_context
{
  mylite_storage_statement *transaction;
  mylite_storage_statement *statement;
};

struct Mylite_foreign_key_list_context
{
  THD *thd;
  List<FOREIGN_KEY_INFO> *list;
  bool map_referenced_key_name;
  int error;
};

struct Mylite_foreign_key_presence_context
{
  bool found;
};

struct Mylite_foreign_key_drop_context
{
  THD *thd;
  Lex_ident_column name;
  const char *stored_name;
  int error;
};

struct Mylite_foreign_key_create_info_context
{
  THD *thd;
  String *text;
  int error;
};

struct Mylite_foreign_key_row_check_context
{
  const char *primary_file;
  TABLE *table;
  const uchar *old_data;
  const uchar *new_data;
  int error;
};

struct Mylite_retained_foreign_key_validation_context
{
  TABLE *table;
  HA_CREATE_INFO *create_info;
  int error;
};

struct Mylite_parent_foreign_key_rename_context
{
  const char *primary_file;
  HA_CREATE_INFO *create_info;
  int error;
};

static int mylite_discover_table(handlerton *, THD *thd, TABLE_SHARE *share)
{
  DBUG_ENTER("mylite_discover_table");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);

  /*
    Copy ALTER temporary shares keep the final SQL table name while the
    handler path carries the temporary storage identity.
  */
  char schema_name[NAME_LEN + 1];
  char table_name[NAME_LEN + 1];
  int path_error= mylite_table_name_from_path(share->path.str, schema_name,
                                              sizeof(schema_name), table_name,
                                              sizeof(table_name));
  if (path_error)
    DBUG_RETURN(path_error);

  unsigned char *frm= NULL;
  size_t frm_len= 0;
  mylite_storage_result storage_result=
    mylite_storage_read_table_definition(primary_file, schema_name, table_name,
                                         &frm, &frm_len);
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

static bool mylite_schema_hook_active()
{
  return mylite_primary_file_path() != NULL;
}

static int mylite_schema_hook_exists(const char *schema_name)
{
  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;

  mylite_storage_result storage_result=
    mylite_storage_schema_exists(primary_file, schema_name);
  return mylite_storage_to_schema_hook_result(storage_result);
}

static int mylite_schema_hook_store(const char *schema_name,
                                    const char *default_character_set_name,
                                    const char *default_collation_name,
                                    const char *schema_comment,
                                    size_t schema_comment_size)
{
  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;

  char *schema_comment_copy= NULL;
  if (schema_comment)
  {
    schema_comment_copy= static_cast<char *>(malloc(schema_comment_size + 1));
    if (!schema_comment_copy)
      return MYLITE_SCHEMA_HOOK_ERROR;
    memcpy(schema_comment_copy, schema_comment, schema_comment_size);
    schema_comment_copy[schema_comment_size]= '\0';
  }

  mylite_storage_schema_definition definition=
  {
    sizeof(definition),
    schema_name,
    default_character_set_name,
    default_collation_name,
    schema_comment_copy
  };
  mylite_storage_result storage_result=
    mylite_storage_store_schema_definition(primary_file, &definition);
  free(schema_comment_copy);
  return mylite_storage_to_schema_hook_result(storage_result);
}

static int mylite_schema_hook_drop(const char *schema_name)
{
  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;

  mylite_storage_result storage_result=
    mylite_storage_drop_schema(primary_file, schema_name);
  return mylite_storage_to_schema_hook_result(storage_result);
}

static int mylite_schema_hook_read(const char *schema_name,
                                   Mylite_schema_options *options)
{
  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;

  mylite_storage_schema_metadata metadata= {sizeof(metadata), NULL, NULL, NULL};
  mylite_storage_result storage_result=
    mylite_storage_read_schema_definition(primary_file, schema_name, &metadata);
  if (storage_result != MYLITE_STORAGE_OK)
    return mylite_storage_to_schema_hook_result(storage_result);

  options->default_character_set_name= metadata.default_character_set_name;
  options->default_collation_name= metadata.default_collation_name;
  options->schema_comment= metadata.schema_comment;
  return MYLITE_SCHEMA_HOOK_OK;
}

static void mylite_schema_hook_free(Mylite_schema_options *options)
{
  if (!options)
    return;

  mylite_storage_free(options->default_character_set_name);
  mylite_storage_free(options->default_collation_name);
  mylite_storage_free(options->schema_comment);
  options->default_character_set_name= NULL;
  options->default_collation_name= NULL;
  options->schema_comment= NULL;
}

static int mylite_schema_hook_list_schemas(
  mylite_schema_name_callback callback, void *ctx)
{
  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;

  mylite_storage_result storage_result=
    mylite_storage_list_schemas(primary_file, callback, ctx);
  return mylite_storage_to_schema_hook_result(storage_result);
}

struct Mylite_schema_hook_table_list
{
  mylite_schema_name_callback callback;
  void *ctx;
};

static int mylite_schema_hook_list_tables(const char *schema_name,
                                          mylite_schema_name_callback callback,
                                          void *ctx)
{
  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;

  Mylite_schema_hook_table_list table_list= {callback, ctx};
  mylite_storage_result storage_result=
    mylite_storage_list_tables(primary_file, schema_name,
                               mylite_schema_hook_add_table, &table_list);
  return mylite_storage_to_schema_hook_result(storage_result);
}

static int mylite_schema_hook_add_table(void *ctx, const char *,
                                        const char *table_name)
{
  Mylite_schema_hook_table_list *table_list=
    static_cast<Mylite_schema_hook_table_list *>(ctx);
  return table_list->callback(table_list->ctx, table_name);
}

static bool mylite_is_user_temporary_table_share(const TABLE_SHARE *share)
{
  return share && (share->tmp_table == TRANSACTIONAL_TMP_TABLE ||
                   share->tmp_table == NON_TRANSACTIONAL_TMP_TABLE);
}

static bool mylite_current_command_creates_user_temporary_table()
{
  THD *thd= current_thd;
  return thd && thd->lex && thd->lex->sql_command == SQLCOM_CREATE_TABLE &&
         thd->lex->create_info.tmp_table();
}

static bool mylite_foreign_key_checks_disabled(THD *thd)
{
  return thd && thd_test_options(thd, OPTION_NO_FOREIGN_KEY_CHECKS);
}

static int mylite_storage_to_handler_error(mylite_storage_result result)
{
  switch (result) {
  case MYLITE_STORAGE_OK:
    return 0;
  case MYLITE_STORAGE_NOMEM:
    return HA_ERR_OUT_OF_MEM;
  case MYLITE_STORAGE_BUSY:
    return HA_ERR_LOCK_WAIT_TIMEOUT;
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

static int mylite_storage_to_schema_hook_result(mylite_storage_result result)
{
  switch (result) {
  case MYLITE_STORAGE_OK:
    return MYLITE_SCHEMA_HOOK_OK;
  case MYLITE_STORAGE_NOTFOUND:
    return MYLITE_SCHEMA_HOOK_NOTFOUND;
  case MYLITE_STORAGE_NOMEM:
  case MYLITE_STORAGE_BUSY:
  case MYLITE_STORAGE_READONLY:
  case MYLITE_STORAGE_IOERR:
  case MYLITE_STORAGE_CORRUPT:
  case MYLITE_STORAGE_FULL:
  case MYLITE_STORAGE_MISUSE:
  case MYLITE_STORAGE_UNSUPPORTED:
  case MYLITE_STORAGE_ERROR:
    return MYLITE_SCHEMA_HOOK_ERROR;
  }

  return MYLITE_SCHEMA_HOOK_ERROR;
}

ha_mylite::ha_mylite(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg), share(NULL), scan_rows(NULL),
   scan_blob_payloads(NULL), scan_row_ids(NULL),
   record_blob_payloads{NULL, NULL}, index_rows(NULL),
   index_blob_payloads(NULL), index_keys(NULL), index_entries(NULL),
   scan_row_size(0), scan_row_count(0), scan_row_index(0),
   scan_blob_payloads_size(0), record_blob_payloads_size{0, 0},
   index_row_size(0), index_row_count(0),
   index_row_index(0), index_blob_payloads_size(0),
   index_cursor_number(MAX_KEY), current_row_id(0),
   duplicate_key_index((uint) -1), discard_rows(false), volatile_rows(false)
{
  storage_schema_name[0]= '\0';
  storage_table_name[0]= '\0';
  strcpy(display_engine_name, MYLITE_STORAGE_ENGINE_NAME);
  display_engine_name_lex.str= display_engine_name;
  display_engine_name_lex.length= strlen(display_engine_name);
  ref_length= sizeof(ulonglong);
}

ha_mylite::~ha_mylite()
{
  clear_scan_rows();
  clear_index_cursor();
  clear_record_blob_payloads();
}

void ha_mylite::clear_scan_rows()
{
  mylite_storage_free(scan_rows);
  mylite_storage_free(scan_blob_payloads);
  mylite_storage_free(scan_row_ids);
  scan_rows= NULL;
  scan_blob_payloads= NULL;
  scan_row_ids= NULL;
  scan_row_size= 0;
  scan_row_count= 0;
  scan_row_index= 0;
  scan_blob_payloads_size= 0;
  current_row_id= 0;
}

void ha_mylite::clear_index_cursor()
{
  mylite_storage_free(index_rows);
  mylite_storage_free(index_blob_payloads);
  mylite_storage_free(index_keys);
  mylite_storage_free(index_entries);
  index_rows= NULL;
  index_blob_payloads= NULL;
  index_keys= NULL;
  index_entries= NULL;
  index_row_size= 0;
  index_row_count= 0;
  index_row_index= 0;
  index_blob_payloads_size= 0;
  index_cursor_number= MAX_KEY;
}

void ha_mylite::clear_record_blob_payloads()
{
  for (size_t i= 0; i < 2; ++i)
  {
    mylite_storage_free(record_blob_payloads[i]);
    record_blob_payloads[i]= NULL;
    record_blob_payloads_size[i]= 0;
  }
}

const char *ha_mylite::storage_schema() const
{
  return storage_schema_name[0] ? storage_schema_name : table->s->db.str;
}

const char *ha_mylite::storage_table() const
{
  return storage_table_name[0] ? storage_table_name : table->s->table_name.str;
}

ulong ha_mylite::index_flags(uint index_number, uint, bool) const
{
  TABLE_SHARE *share= table ? table->s : table_share;
  if (!share || index_number >= share->keys ||
      !mylite_key_is_supported(share->key_info + index_number))
    return 0;

  return HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE;
}

int ha_mylite::build_index_cursor(uint index_number)
{
  DBUG_ENTER("ha_mylite::build_index_cursor");

  clear_index_cursor();
  if (index_number >= table->s->keys ||
      !mylite_key_is_supported(table->key_info + index_number))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  if (discard_rows)
  {
    index_cursor_number= index_number;
    DBUG_RETURN(0);
  }

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  mylite_storage_index_entryset entryset= {sizeof(entryset), NULL, 0, 0};
  mylite_storage_result storage_result= volatile_rows ?
    mylite_volatile_read_index_entries(primary_file, storage_schema(),
                                       storage_table(), index_number,
                                       &entryset) :
    mylite_storage_read_index_entries(primary_file, storage_schema(),
                                      storage_table(), index_number,
                                      &entryset);
  if (storage_result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(storage_result));

  if (entryset.entry_count == 0)
  {
    mylite_storage_free_index_entryset(&entryset);
    index_cursor_number= index_number;
    DBUG_RETURN(0);
  }
  if (!entryset.key_offsets || !entryset.key_sizes || !entryset.row_ids ||
      !entryset.keys || table->s->reclength == 0 ||
      entryset.entry_count > SIZE_MAX / table->s->reclength ||
      entryset.entry_count > SIZE_MAX / sizeof(Mylite_index_cursor_entry))
  {
    mylite_storage_free_index_entryset(&entryset);
    DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
  }

  KEY *key_info= table->key_info + index_number;
  size_t blob_payloads_size= 0;
  for (size_t i= 0; i < entryset.entry_count; ++i)
  {
    if (entryset.key_sizes[i] != key_info->key_length ||
        entryset.key_offsets[i] > entryset.key_bytes ||
        entryset.key_sizes[i] > entryset.key_bytes - entryset.key_offsets[i])
    {
      mylite_storage_free_index_entryset(&entryset);
      DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
    }

    uchar *row_payload= NULL;
    size_t row_payload_size= 0;
    storage_result= volatile_rows ?
      mylite_volatile_read_row(primary_file, storage_schema(), storage_table(),
                               entryset.row_ids[i], &row_payload,
                               &row_payload_size) :
      mylite_storage_read_row(primary_file, storage_schema(), storage_table(),
                              entryset.row_ids[i], &row_payload,
                              &row_payload_size);
    if (storage_result != MYLITE_STORAGE_OK)
    {
      mylite_storage_free_index_entryset(&entryset);
      DBUG_RETURN(mylite_storage_to_handler_error(storage_result));
    }

    size_t row_blob_payloads_size= 0;
    int error= mylite_scan_stored_row(table, row_payload, row_payload_size,
                                      &row_blob_payloads_size);
    mylite_storage_free(row_payload);
    if (error)
    {
      mylite_storage_free_index_entryset(&entryset);
      DBUG_RETURN(error);
    }
    if (row_blob_payloads_size > SIZE_MAX - blob_payloads_size)
    {
      mylite_storage_free_index_entryset(&entryset);
      DBUG_RETURN(HA_ERR_RECORD_FILE_FULL);
    }
    blob_payloads_size+= row_blob_payloads_size;
  }

  const size_t rows_size= entryset.entry_count * table->s->reclength;
  uchar *rows= static_cast<uchar *>(malloc(rows_size));
  Mylite_index_cursor_entry *entries= static_cast<Mylite_index_cursor_entry *>(
    malloc(entryset.entry_count * sizeof(Mylite_index_cursor_entry)));
  uchar *blob_payloads= NULL;
  if (blob_payloads_size > 0)
    blob_payloads= static_cast<uchar *>(malloc(blob_payloads_size));
  if (!rows || !entries || (blob_payloads_size > 0 && !blob_payloads))
  {
    free(rows);
    free(entries);
    free(blob_payloads);
    mylite_storage_free_index_entryset(&entryset);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  size_t blob_payloads_used= 0;
  for (size_t i= 0; i < entryset.entry_count; ++i)
  {
    uchar *row_payload= NULL;
    size_t row_payload_size= 0;
    storage_result= volatile_rows ?
      mylite_volatile_read_row(primary_file, storage_schema(), storage_table(),
                               entryset.row_ids[i], &row_payload,
                               &row_payload_size) :
      mylite_storage_read_row(primary_file, storage_schema(), storage_table(),
                              entryset.row_ids[i], &row_payload,
                              &row_payload_size);
    if (storage_result != MYLITE_STORAGE_OK)
    {
      free(rows);
      free(entries);
      free(blob_payloads);
      mylite_storage_free_index_entryset(&entryset);
      DBUG_RETURN(mylite_storage_to_handler_error(storage_result));
    }

    uchar *out_row= rows + (i * table->s->reclength);
    int error= mylite_copy_stored_row_to_scan(table, row_payload,
                                              row_payload_size, out_row,
                                              blob_payloads,
                                              &blob_payloads_used);
    mylite_storage_free(row_payload);
    if (error)
    {
      free(rows);
      free(entries);
      free(blob_payloads);
      mylite_storage_free_index_entryset(&entryset);
      DBUG_RETURN(error);
    }
    entries[i].key_offset= entryset.key_offsets[i];
    entries[i].key_size= entryset.key_sizes[i];
    entries[i].row_offset= i * table->s->reclength;
    entries[i].row_id= entryset.row_ids[i];
  }
  if (blob_payloads_used != blob_payloads_size)
  {
    free(rows);
    free(entries);
    free(blob_payloads);
    mylite_storage_free_index_entryset(&entryset);
    DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
  }

  int error= mylite_sort_index_entries(table, index_number, entryset.keys,
                                       entries, entryset.entry_count);
  if (error)
  {
    free(rows);
    free(entries);
    free(blob_payloads);
    mylite_storage_free_index_entryset(&entryset);
    DBUG_RETURN(error);
  }

  index_rows= rows;
  index_blob_payloads= blob_payloads;
  index_keys= entryset.keys;
  index_entries= entries;
  index_row_size= table->s->reclength;
  index_row_count= entryset.entry_count;
  index_blob_payloads_size= blob_payloads_size;
  index_cursor_number= index_number;
  entryset.keys= NULL;
  mylite_storage_free_index_entryset(&entryset);
  DBUG_RETURN(0);
}

int ha_mylite::read_index_cursor_row(uchar *buf, size_t row_index)
{
  DBUG_ENTER("ha_mylite::read_index_cursor_row");
  if (row_index >= index_row_count || !index_entries || !index_rows)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  const Mylite_index_cursor_entry *entry= index_entries + row_index;
  if (index_row_size != 0 && index_row_count > SIZE_MAX / index_row_size)
    DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
  const size_t rows_size= index_row_count * index_row_size;
  if (entry->row_offset > rows_size ||
      index_row_size > rows_size - entry->row_offset)
    DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);

  memcpy(buf, index_rows + entry->row_offset, index_row_size);
  const int error= preserve_record_blob_payloads(buf);
  if (error)
    DBUG_RETURN(error);
  index_row_index= row_index;
  current_row_id= entry->row_id;
  DBUG_RETURN(0);
}

int ha_mylite::record_blob_payload_slot(const uchar *buf, size_t *out_slot) const
{
  if (buf == table->record[0])
  {
    *out_slot= 0;
    return 0;
  }
  if (table->record[1] && buf == table->record[1])
  {
    *out_slot= 1;
    return 0;
  }

  return HA_ERR_CRASHED_ON_USAGE;
}

int ha_mylite::preserve_record_blob_payloads(uchar *buf)
{
  DBUG_ENTER("ha_mylite::preserve_record_blob_payloads");

  size_t slot= 0;
  int error= record_blob_payload_slot(buf, &slot);
  if (error)
    DBUG_RETURN(error);

  /* Joined result evaluation can outlive scan or index cursor buffers. */
  if (!mylite_table_has_blob_fields(table))
  {
    mylite_storage_free(record_blob_payloads[slot]);
    record_blob_payloads[slot]= NULL;
    record_blob_payloads_size[slot]= 0;
    DBUG_RETURN(0);
  }

  const my_ptrdiff_t row_offset= buf - table->record[0];
  size_t blob_payloads_size= 0;
  for (Field **field= table->field; *field; ++field)
  {
    if (!((*field)->flags & BLOB_FLAG))
      continue;

    Field_blob *blob_field= static_cast<Field_blob *>(*field);
    if (blob_field->is_null(row_offset))
      continue;

    const uint32 length= blob_field->get_length(row_offset);
    if (length == 0)
      continue;
    if (!blob_field->get_ptr(blob_field->ptr + row_offset))
      DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
    if (blob_payloads_size > SIZE_MAX - length)
      DBUG_RETURN(HA_ERR_RECORD_FILE_FULL);
    blob_payloads_size+= length;
  }

  if (blob_payloads_size == 0)
  {
    mylite_storage_free(record_blob_payloads[slot]);
    record_blob_payloads[slot]= NULL;
    record_blob_payloads_size[slot]= 0;
    DBUG_RETURN(0);
  }

  uchar *blob_payloads= static_cast<uchar *>(malloc(blob_payloads_size));
  if (!blob_payloads)
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  size_t blob_payloads_used= 0;
  for (Field **field= table->field; *field; ++field)
  {
    if (!((*field)->flags & BLOB_FLAG))
      continue;

    Field_blob *blob_field= static_cast<Field_blob *>(*field);
    if (blob_field->is_null(row_offset))
      continue;

    const uint32 length= blob_field->get_length(row_offset);
    if (length == 0)
      continue;

    uchar *data= blob_field->get_ptr(blob_field->ptr + row_offset);
    if (!data || length > blob_payloads_size - blob_payloads_used)
    {
      mylite_storage_free(blob_payloads);
      DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
    }
    memcpy(blob_payloads + blob_payloads_used, data, length);
    blob_field->set_ptr_offset(row_offset, length,
                               blob_payloads + blob_payloads_used);
    blob_payloads_used+= length;
  }

  if (blob_payloads_used != blob_payloads_size)
  {
    mylite_storage_free(blob_payloads);
    DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
  }

  mylite_storage_free(record_blob_payloads[slot]);
  record_blob_payloads[slot]= blob_payloads;
  record_blob_payloads_size[slot]= blob_payloads_size;
  DBUG_RETURN(0);
}

int ha_mylite::open(const char *name, int, uint)
{
  DBUG_ENTER("ha_mylite::open");

  int path_error= mylite_table_name_from_path(name, storage_schema_name,
                                              sizeof(storage_schema_name),
                                              storage_table_name,
                                              sizeof(storage_table_name));
  if (path_error)
    DBUG_RETURN(path_error);

  const bool user_temporary_table=
    mylite_is_user_temporary_table_share(table_share) ||
    (table_share && table_share->tmp_table == INTERNAL_TMP_TABLE &&
     mylite_current_command_creates_user_temporary_table());
  if (user_temporary_table &&
      (mylite_copy_string(table_share->db.str, storage_schema_name,
                          sizeof(storage_schema_name)) ||
       mylite_copy_string(table_share->table_name.str, storage_table_name,
                          sizeof(storage_table_name))))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  const char *primary_file= mylite_primary_file_path();
  if (primary_file)
  {
    if (user_temporary_table)
    {
      volatile_rows= true;
      display_engine_name_lex.length= strlen(display_engine_name);
    }
    else
    {
      const int engine_name_error=
        mylite_display_engine_name(primary_file, storage_schema_name,
                                   storage_table_name, display_engine_name,
                                   sizeof(display_engine_name));
      if (engine_name_error)
        DBUG_RETURN(engine_name_error);
      display_engine_name_lex.length= strlen(display_engine_name);
      discard_rows= mylite_discards_rows_engine_request(display_engine_name);
      volatile_rows= mylite_uses_volatile_rows_engine_request(display_engine_name);
    }
  }

  if (!(share= get_share()))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  thr_lock_data_init(&share->lock, &lock, NULL);

  DBUG_RETURN(0);
}

LEX_CSTRING *ha_mylite::engine_name()
{
  return &display_engine_name_lex;
}

int ha_mylite::close(void)
{
  DBUG_ENTER("ha_mylite::close");
  clear_scan_rows();
  clear_index_cursor();
  clear_record_blob_payloads();
  discard_rows= false;
  volatile_rows= false;
  DBUG_RETURN(0);
}

int ha_mylite::index_init(uint idx, bool)
{
  DBUG_ENTER("ha_mylite::index_init");
  DBUG_RETURN(build_index_cursor(idx));
}

int ha_mylite::index_end()
{
  DBUG_ENTER("ha_mylite::index_end");
  clear_index_cursor();
  DBUG_RETURN(0);
}

int ha_mylite::index_read_map(uchar *buf, const uchar *key,
                              key_part_map keypart_map,
                              enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_mylite::index_read_map");
  if (index_cursor_number != active_index)
  {
    const int error= build_index_cursor(active_index);
    if (error)
      DBUG_RETURN(error);
  }
  if (!key)
    DBUG_RETURN(index_first(buf));

  const uint key_length= calculate_key_len(table, active_index, key,
                                           keypart_map);
  size_t entry_index= 0;
  const int error=
    mylite_find_index_entry(table, index_entries, index_row_count, index_keys,
                            active_index, key, key_length, find_flag,
                            &entry_index);
  if (error)
    DBUG_RETURN(error);

  DBUG_RETURN(read_index_cursor_row(buf, entry_index));
}

int ha_mylite::index_read_idx_map(uchar *buf, uint index, const uchar *key,
                                  key_part_map keypart_map,
                                  enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_mylite::index_read_idx_map");

  int error= build_index_cursor(index);
  if (error)
    DBUG_RETURN(error);
  if (!key)
    DBUG_RETURN(index_first(buf));

  const uint key_length= calculate_key_len(table, index, key, keypart_map);
  size_t entry_index= 0;
  error=
    mylite_find_index_entry(table, index_entries, index_row_count, index_keys,
                            index, key, key_length, find_flag, &entry_index);
  if (error)
    DBUG_RETURN(error);

  DBUG_RETURN(read_index_cursor_row(buf, entry_index));
}

int ha_mylite::index_next(uchar *buf)
{
  DBUG_ENTER("ha_mylite::index_next");
  if (index_row_count == 0 || index_row_index + 1 >= index_row_count)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  DBUG_RETURN(read_index_cursor_row(buf, index_row_index + 1));
}

int ha_mylite::index_prev(uchar *buf)
{
  DBUG_ENTER("ha_mylite::index_prev");
  if (index_row_count == 0 || index_row_index == 0)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  DBUG_RETURN(read_index_cursor_row(buf, index_row_index - 1));
}

int ha_mylite::index_first(uchar *buf)
{
  DBUG_ENTER("ha_mylite::index_first");
  if (index_row_count == 0)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  DBUG_RETURN(read_index_cursor_row(buf, 0));
}

int ha_mylite::index_last(uchar *buf)
{
  DBUG_ENTER("ha_mylite::index_last");
  if (index_row_count == 0)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  DBUG_RETURN(read_index_cursor_row(buf, index_row_count - 1));
}

int ha_mylite::rnd_init(bool scan)
{
  DBUG_ENTER("ha_mylite::rnd_init");

  clear_scan_rows();
  if (!scan)
    DBUG_RETURN(0);
  if (discard_rows)
    DBUG_RETURN(0);

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  mylite_storage_rowset rowset= {sizeof(rowset), NULL, 0, 0};
  const mylite_storage_result result= volatile_rows ?
    mylite_volatile_read_rows(primary_file, storage_schema(), storage_table(),
                              &rowset) :
    mylite_storage_read_rows(primary_file, storage_schema(), storage_table(),
                             &rowset);
  if (result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(result));

  uchar *rows= NULL;
  uchar *blob_payloads= NULL;
  ulonglong *row_ids= NULL;
  size_t row_size= 0;
  size_t row_count= 0;
  size_t blob_payloads_size= 0;
  int error= mylite_prepare_scan_rows(table, &rowset, &rows, &row_size,
                                      &row_count, &row_ids, &blob_payloads,
                                      &blob_payloads_size);
  mylite_storage_free_rowset(&rowset);
  if (error)
  {
    mylite_storage_free(rows);
    mylite_storage_free(row_ids);
    mylite_storage_free(blob_payloads);
    DBUG_RETURN(error);
  }

  scan_rows= rows;
  scan_blob_payloads= blob_payloads;
  scan_row_ids= row_ids;
  scan_row_size= row_size;
  scan_row_count= row_count;
  scan_blob_payloads_size= blob_payloads_size;
  scan_row_index= 0;
  current_row_id= 0;
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

  current_row_id= scan_row_ids[scan_row_index];
  memcpy(buf, scan_rows + (scan_row_index * scan_row_size), scan_row_size);
  const int error= preserve_record_blob_payloads(buf);
  if (error)
    DBUG_RETURN(error);
  ++scan_row_index;
  DBUG_RETURN(0);
}

int ha_mylite::rnd_pos(uchar *buf, uchar *pos)
{
  DBUG_ENTER("ha_mylite::rnd_pos");
  if (discard_rows)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  const ulonglong row_id= mylite_get_u64(pos);
  uchar *row_payload= NULL;
  size_t row_payload_size= 0;
  mylite_storage_result result= volatile_rows ?
    mylite_volatile_read_row(primary_file, storage_schema(), storage_table(),
                             row_id, &row_payload, &row_payload_size) :
    mylite_storage_read_row(primary_file, storage_schema(), storage_table(),
                            row_id, &row_payload, &row_payload_size);
  if (result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(result));

  size_t blob_payloads_size= 0;
  int error= mylite_scan_stored_row(table, row_payload, row_payload_size,
                                    &blob_payloads_size);
  if (error)
  {
    mylite_storage_free(row_payload);
    DBUG_RETURN(error);
  }

  uchar *blob_payloads= NULL;
  if (blob_payloads_size > 0)
  {
    blob_payloads= static_cast<uchar *>(malloc(blob_payloads_size));
    if (!blob_payloads)
    {
      mylite_storage_free(row_payload);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  size_t blob_payloads_used= 0;
  error= mylite_copy_stored_row_to_scan(table, row_payload, row_payload_size,
                                        buf, blob_payloads,
                                        &blob_payloads_used);
  mylite_storage_free(row_payload);
  if (error || blob_payloads_used != blob_payloads_size)
  {
    mylite_storage_free(blob_payloads);
    DBUG_RETURN(error ? error : HA_ERR_CRASHED_ON_USAGE);
  }

  size_t slot= 0;
  error= record_blob_payload_slot(buf, &slot);
  if (error)
  {
    mylite_storage_free(blob_payloads);
    DBUG_RETURN(error);
  }

  mylite_storage_free(record_blob_payloads[slot]);
  record_blob_payloads[slot]= blob_payloads;
  record_blob_payloads_size[slot]= blob_payloads_size;
  current_row_id= row_id;
  DBUG_RETURN(0);
}

void ha_mylite::position(const uchar *)
{
  DBUG_ENTER("ha_mylite::position");
  mylite_put_u64(ref, current_row_id);
  DBUG_VOID_RETURN;
}

int ha_mylite::info(uint flag)
{
  DBUG_ENTER("ha_mylite::info");

  if ((flag & HA_STATUS_ERRKEY) && duplicate_key_index != (uint) -1)
    errkey= duplicate_key_index;

  stats.records= 0;
  stats.deleted= 0;
  stats.data_file_length= 0;
  stats.index_file_length= 0;
  stats.auto_increment_value= 0;
  if (discard_rows)
  {
    stats.block_size= 8192;
    if (flag & HA_STATUS_AUTO)
      stats.auto_increment_value= 1;
    DBUG_RETURN(0);
  }

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(0);

  unsigned long long row_count= 0;
  const mylite_storage_result result= volatile_rows ?
    mylite_volatile_count_rows(primary_file, storage_schema(), storage_table(),
                               &row_count) :
    mylite_storage_count_rows(primary_file, storage_schema(), storage_table(),
                              &row_count);
  if (result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(result));

  stats.records= (ha_rows) row_count;

  if ((flag & HA_STATUS_AUTO) && mylite_auto_increment_field(table))
  {
    unsigned long long next_value= 0ULL;
    const mylite_storage_result auto_result= volatile_rows ?
      mylite_volatile_read_auto_increment(primary_file, storage_schema(),
                                          storage_table(), &next_value) :
      mylite_storage_read_auto_increment(primary_file, storage_schema(),
                                         storage_table(), &next_value);
    if (auto_result != MYLITE_STORAGE_OK)
      DBUG_RETURN(mylite_storage_to_handler_error(auto_result));
    stats.auto_increment_value= next_value;
  }

  DBUG_RETURN(0);
}

void ha_mylite::update_create_info(HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_mylite::update_create_info");

  if (!(create_info->used_fields & HA_CREATE_USED_AUTO))
  {
    ha_mylite::info(HA_STATUS_AUTO);
    create_info->auto_increment_value= stats.auto_increment_value;
  }

  DBUG_VOID_RETURN;
}

int ha_mylite::external_lock(THD *thd, int lock_type)
{
  DBUG_ENTER("ha_mylite::external_lock");

  if (lock_type != F_WRLCK)
    DBUG_RETURN(0);

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN) &&
      !mylite_storage_statement_active(primary_file))
  {
    int transaction_error= mylite_begin_transaction_checkpoint(thd,
                                                              primary_file);
    if (transaction_error)
      DBUG_RETURN(transaction_error);
  }

  int error= mylite_begin_statement_checkpoint(thd, primary_file);
  if (error)
    DBUG_RETURN(error);

  trans_register_ha(thd, false, mylite_hton, 0);
  if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
    trans_register_ha(thd, true, mylite_hton, 0);

  DBUG_RETURN(0);
}

void ha_mylite::get_auto_increment(ulonglong offset, ulonglong increment,
                                   ulonglong, ulonglong *first_value,
                                   ulonglong *nb_reserved_values)
{
  DBUG_ENTER("ha_mylite::get_auto_increment");

  *first_value= ULONGLONG_MAX;
  *nb_reserved_values= 0ULL;

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_VOID_RETURN;

  unsigned long long next_value= 0ULL;
  const mylite_storage_result result= volatile_rows ?
    mylite_volatile_read_auto_increment(primary_file, storage_schema(),
                                        storage_table(), &next_value) :
    mylite_storage_read_auto_increment(primary_file, storage_schema(),
                                       storage_table(), &next_value);
  if (result != MYLITE_STORAGE_OK)
    DBUG_VOID_RETURN;

  *first_value= mylite_first_auto_increment_value(next_value, offset,
                                                  increment);
  if (*first_value != ULONGLONG_MAX)
    *nb_reserved_values= ULONGLONG_MAX;

  DBUG_VOID_RETURN;
}

int ha_mylite::reset_auto_increment(ulonglong value)
{
  DBUG_ENTER("ha_mylite::reset_auto_increment");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  const ulonglong next_value= value == 0ULL ? 1ULL : value;
  const mylite_storage_result result= volatile_rows ?
    mylite_volatile_set_auto_increment(primary_file, storage_schema(),
                                       storage_table(), next_value) :
    mylite_storage_set_auto_increment(primary_file, storage_schema(),
                                      storage_table(), next_value);
  DBUG_RETURN(mylite_storage_to_handler_error(result));
}

int ha_mylite::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_mylite::write_row");

  duplicate_key_index= (uint) -1;

  if (!mylite_table_supports_row_write(table))
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  if (discard_rows)
    DBUG_RETURN(table->next_number_field ? update_auto_increment() : 0);

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  if (table->next_number_field && buf == table->record[0])
  {
    const int error= update_auto_increment();
    if (error)
      DBUG_RETURN(error);
  }

  mylite_storage_index_entry *index_entries= NULL;
  uchar *index_key_storage= NULL;
  size_t index_entry_count= 0;
  int error= mylite_prepare_index_entries(table, buf, &index_entries,
                                          &index_entry_count,
                                          &index_key_storage);
  if (error)
    DBUG_RETURN(error);

  uint duplicate_key= (uint) -1;
  error= volatile_rows ?
    mylite_check_volatile_duplicate_keys(primary_file, storage_schema(),
                                         storage_table(), table, index_entries,
                                         index_entry_count, buf, 0ULL,
                                         &duplicate_key) :
    mylite_check_duplicate_keys(primary_file, storage_schema(),
                                storage_table(), table, index_entries,
                                index_entry_count, buf, 0ULL,
                                &duplicate_key);
  if (error)
  {
    mylite_free_index_entries(index_entries, index_key_storage);
    duplicate_key_index= duplicate_key;
    DBUG_RETURN(error);
  }

  if (!volatile_rows && !mylite_foreign_key_checks_disabled(ha_thd()))
  {
    error= mylite_check_child_foreign_keys(primary_file, storage_schema(),
                                           storage_table(), table, buf);
    if (error)
    {
      mylite_free_index_entries(index_entries, index_key_storage);
      DBUG_RETURN(error);
    }
  }

  if (volatile_rows)
  {
    Field *auto_field= mylite_auto_increment_field(table);
    if (auto_field)
    {
      const longlong signed_value= auto_field->val_int();
      if (signed_value >= 0 || (auto_field->flags & UNSIGNED_FLAG))
      {
        const ulonglong value= (ulonglong) signed_value;
        error= value == ULONGLONG_MAX ? HA_ERR_AUTOINC_ERANGE :
          mylite_storage_to_handler_error(
            mylite_volatile_advance_auto_increment(primary_file,
                                                   storage_schema(),
                                                   storage_table(),
                                                   value + 1ULL));
      }
    }
  }
  else
    error= mylite_advance_auto_increment_from_row(primary_file,
                                                  storage_schema(),
                                                  storage_table(), table);
  if (error)
  {
    mylite_free_index_entries(index_entries, index_key_storage);
    DBUG_RETURN(error);
  }

  const uchar *row_payload= NULL;
  size_t row_payload_size= 0;
  uchar *owned_row_payload= NULL;
  error= mylite_prepare_row_payload(table, buf, &row_payload,
                                    &row_payload_size, &owned_row_payload);
  if (error)
  {
    mylite_free_index_entries(index_entries, index_key_storage);
    DBUG_RETURN(error);
  }

  unsigned long long row_id= 0ULL;
  const mylite_storage_result result= volatile_rows ?
    mylite_volatile_append_row_with_index_entries(
      primary_file, storage_schema(), storage_table(), row_payload,
      row_payload_size, index_entries, index_entry_count, &row_id) :
    mylite_storage_append_row_with_index_entries(
      primary_file, storage_schema(), storage_table(), row_payload,
      row_payload_size, index_entries, index_entry_count, &row_id);
  mylite_storage_free(owned_row_payload);
  mylite_free_index_entries(index_entries, index_key_storage);
  if (result == MYLITE_STORAGE_OK)
    current_row_id= row_id;
  DBUG_RETURN(mylite_storage_to_handler_error(result));
}

int ha_mylite::update_row(const uchar *old_data, const uchar *new_data)
{
  DBUG_ENTER("ha_mylite::update_row");

  duplicate_key_index= (uint) -1;

  if (discard_rows)
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  if (!mylite_table_supports_row_lifecycle(table))
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  if (current_row_id == 0)
    DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  mylite_storage_index_entry *index_entries= NULL;
  uchar *index_key_storage= NULL;
  size_t index_entry_count= 0;
  int error= mylite_prepare_index_entries(table, new_data, &index_entries,
                                          &index_entry_count,
                                          &index_key_storage);
  if (error)
    DBUG_RETURN(error);

  uint duplicate_key= (uint) -1;
  error= volatile_rows ?
    mylite_check_volatile_duplicate_keys(primary_file, storage_schema(),
                                         storage_table(), table, index_entries,
                                         index_entry_count, new_data,
                                         current_row_id,
                                         &duplicate_key) :
    mylite_check_duplicate_keys(primary_file, storage_schema(),
                                storage_table(), table, index_entries,
                                index_entry_count, new_data,
                                current_row_id,
                                &duplicate_key);
  if (error)
  {
    mylite_free_index_entries(index_entries, index_key_storage);
    duplicate_key_index= duplicate_key;
    DBUG_RETURN(error);
  }

  if (!volatile_rows && !mylite_foreign_key_checks_disabled(ha_thd()))
  {
    error= mylite_check_child_foreign_keys(primary_file, storage_schema(),
                                           storage_table(), table, new_data);
    if (!error)
      error= mylite_check_parent_foreign_keys(primary_file, storage_schema(),
                                              storage_table(), table,
                                              old_data, new_data);
    if (error)
    {
      mylite_free_index_entries(index_entries, index_key_storage);
      DBUG_RETURN(error);
    }
  }

  if (volatile_rows)
  {
    Field *auto_field= mylite_auto_increment_field(table);
    if (auto_field)
    {
      const longlong signed_value= auto_field->val_int();
      if (signed_value >= 0 || (auto_field->flags & UNSIGNED_FLAG))
      {
        const ulonglong value= (ulonglong) signed_value;
        error= value == ULONGLONG_MAX ? HA_ERR_AUTOINC_ERANGE :
          mylite_storage_to_handler_error(
            mylite_volatile_advance_auto_increment(primary_file,
                                                   storage_schema(),
                                                   storage_table(),
                                                   value + 1ULL));
      }
    }
  }
  else
    error= mylite_advance_auto_increment_from_row(primary_file,
                                                  storage_schema(),
                                                  storage_table(), table);
  if (error)
  {
    mylite_free_index_entries(index_entries, index_key_storage);
    DBUG_RETURN(error);
  }

  const uchar *row_payload= NULL;
  size_t row_payload_size= 0;
  uchar *owned_row_payload= NULL;
  error= mylite_prepare_row_payload(table, new_data, &row_payload,
                                    &row_payload_size, &owned_row_payload);
  if (error)
  {
    mylite_free_index_entries(index_entries, index_key_storage);
    DBUG_RETURN(error);
  }

  unsigned long long new_row_id= 0ULL;
  const mylite_storage_result result= volatile_rows ?
    mylite_volatile_update_row_with_index_entries(
      primary_file, storage_schema(), storage_table(), current_row_id,
      row_payload, row_payload_size, index_entries, index_entry_count,
      &new_row_id) :
    mylite_storage_update_row_with_index_entries(
      primary_file, storage_schema(), storage_table(), current_row_id,
      row_payload, row_payload_size, index_entries, index_entry_count,
      &new_row_id);
  mylite_storage_free(owned_row_payload);
  mylite_free_index_entries(index_entries, index_key_storage);
  if (result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(result));

  current_row_id= new_row_id;
  DBUG_RETURN(0);
}

int ha_mylite::delete_row(const uchar *buf)
{
  DBUG_ENTER("ha_mylite::delete_row");

  if (discard_rows)
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  if (!mylite_table_supports_row_lifecycle(table))
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  if (current_row_id == 0)
    DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  if (!volatile_rows && !mylite_foreign_key_checks_disabled(ha_thd()))
  {
    const int error=
      mylite_check_parent_foreign_keys(primary_file, storage_schema(),
                                       storage_table(), table, buf, NULL);
    if (error)
      DBUG_RETURN(error);
  }

  const mylite_storage_result result= volatile_rows ?
    mylite_volatile_delete_row(primary_file, storage_schema(), storage_table(),
                               current_row_id) :
    mylite_storage_delete_row(primary_file, storage_schema(), storage_table(),
                              current_row_id);
  if (result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(result));

  current_row_id= 0;
  DBUG_RETURN(0);
}

int ha_mylite::truncate()
{
  DBUG_ENTER("ha_mylite::truncate");

  if (discard_rows)
  {
    clear_scan_rows();
    clear_index_cursor();
    clear_record_blob_payloads();
    current_row_id= 0;
    duplicate_key_index= (uint) -1;
    DBUG_RETURN(0);
  }
  if (!mylite_table_supports_row_lifecycle(table))
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file)
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  const mylite_storage_result result= volatile_rows ?
    mylite_volatile_truncate_table(primary_file, storage_schema(),
                                   storage_table()) :
    mylite_storage_truncate_table(primary_file, storage_schema(),
                                  storage_table());
  if (result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(result));

  clear_scan_rows();
  clear_index_cursor();
  clear_record_blob_payloads();
  current_row_id= 0;
  duplicate_key_index= (uint) -1;
  DBUG_RETURN(0);
}

char *ha_mylite::get_foreign_key_create_info()
{
  DBUG_ENTER("ha_mylite::get_foreign_key_create_info");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file || volatile_rows)
    DBUG_RETURN(NULL);

  THD *thd= ha_thd();
  StringBuffer<1024> text(system_charset_info);
  Mylite_foreign_key_create_info_context ctx= {thd, &text, 0};
  const mylite_storage_result result=
    mylite_storage_list_foreign_keys(primary_file, storage_schema(),
                                     storage_table(),
                                     mylite_append_foreign_key_create_info,
                                     &ctx);
  if (ctx.error || result != MYLITE_STORAGE_OK || text.length() == 0)
    DBUG_RETURN(NULL);

  DBUG_RETURN(my_strndup(PSI_INSTRUMENT_ME, text.ptr(), text.length(),
                         MYF(0)));
}

void ha_mylite::free_foreign_key_create_info(char *str)
{
  my_free(str);
}

int ha_mylite::get_foreign_key_list(THD *thd,
                                    List<FOREIGN_KEY_INFO> *f_key_list)
{
  DBUG_ENTER("ha_mylite::get_foreign_key_list");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file || volatile_rows)
    DBUG_RETURN(0);

  Mylite_foreign_key_list_context ctx= {thd, f_key_list, false, 0};
  const mylite_storage_result result=
    mylite_storage_list_foreign_keys(primary_file, storage_schema(),
                                     storage_table(),
                                     mylite_add_foreign_key_info, &ctx);
  if (ctx.error)
    DBUG_RETURN(ctx.error);
  DBUG_RETURN(mylite_storage_to_handler_error(result));
}

int ha_mylite::get_parent_foreign_key_list(THD *thd,
                                           List<FOREIGN_KEY_INFO> *f_key_list)
{
  DBUG_ENTER("ha_mylite::get_parent_foreign_key_list");

  const char *primary_file= mylite_primary_file_path();
  if (!primary_file || volatile_rows)
    DBUG_RETURN(0);

  Mylite_foreign_key_list_context ctx= {thd, f_key_list, true, 0};
  const mylite_storage_result result=
    mylite_storage_list_parent_foreign_keys(primary_file, storage_schema(),
                                            storage_table(),
                                            mylite_add_foreign_key_info, &ctx);
  if (ctx.error)
    DBUG_RETURN(ctx.error);
  DBUG_RETURN(mylite_storage_to_handler_error(result));
}

bool ha_mylite::referenced_by_foreign_key() const noexcept
{
  const char *primary_file= mylite_primary_file_path();
  if (!primary_file || volatile_rows)
    return false;

  Mylite_foreign_key_presence_context ctx= {false};
  const mylite_storage_result result=
    mylite_storage_list_parent_foreign_keys(primary_file, storage_schema(),
                                            storage_table(),
                                            mylite_detect_foreign_key_presence,
                                            &ctx);
  return ctx.found || result != MYLITE_STORAGE_OK;
}

enum_alter_inplace_result ha_mylite::check_if_supported_inplace_alter(
  TABLE *, Alter_inplace_info *)
{
  return HA_ALTER_INPLACE_NOT_SUPPORTED;
}

int ha_mylite::create(const char *name, TABLE *form, HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_mylite::create");

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
  char alias_schema_name[NAME_LEN + 1];
  char alias_table_name[NAME_LEN + 1];
  strcpy(alias_schema_name, schema_name);
  strcpy(alias_table_name, table_name);

  THD *thd= current_thd;
  if (thd && thd->lex && thd->lex->sql_command == SQLCOM_ALTER_TABLE)
  {
    if (thd->lex->alter_info.requested_lock ==
        Alter_info::ALTER_TABLE_LOCK_NONE)
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }
  if (!mylite_table_supports_row_write(form))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  char requested_engine_name[NAME_LEN + 1];
  int engine_name_error=
    mylite_requested_engine_name(primary_file, create_info,
                                 requested_engine_name,
                                 sizeof(requested_engine_name));
  if (engine_name_error)
    DBUG_RETURN(engine_name_error);
  if (!mylite_supported_engine_request(requested_engine_name))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  const bool requested_engine_discards_rows=
    mylite_discards_rows_engine_request(requested_engine_name);
  const bool requested_engine_uses_volatile_rows=
    mylite_uses_volatile_rows_engine_request(requested_engine_name);
  const bool temporary_table= create_info && create_info->tmp_table();
  if (temporary_table &&
      (mylite_copy_string(form->s->db.str, schema_name, sizeof(schema_name)) ||
       mylite_copy_string(form->s->table_name.str, table_name, sizeof(table_name))))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  const bool use_volatile_rows=
    temporary_table || requested_engine_uses_volatile_rows;
  if (requested_engine_uses_volatile_rows && mylite_table_has_blob_fields(form))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  if (!temporary_table && requested_engine_discards_rows &&
      mylite_copy_string(requested_engine_name, display_engine_name,
                         sizeof(display_engine_name)))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  if (!temporary_table && requested_engine_uses_volatile_rows &&
      mylite_copy_string(requested_engine_name, display_engine_name,
                         sizeof(display_engine_name)))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  const bool rebuild_existing_table_command=
    thd && thd->lex &&
    (thd->lex->sql_command == SQLCOM_ALTER_TABLE ||
     thd->lex->sql_command == SQLCOM_CREATE_INDEX ||
     thd->lex->sql_command == SQLCOM_DROP_INDEX);
  TABLE *alter_original_table= rebuild_existing_table_command ?
    thd->lex->alter_info.original_table : NULL;
  const char *logical_schema_name=
    alter_original_table && alter_original_table->s &&
    alter_original_table->s->db.str ? alter_original_table->s->db.str :
    form && form->s && form->s->db.str ? form->s->db.str : schema_name;
  const char *logical_table_name=
    alter_original_table && alter_original_table->s &&
    alter_original_table->s->table_name.str ?
    alter_original_table->s->table_name.str :
    form && form->s && form->s->table_name.str ?
    form->s->table_name.str : table_name;
  const bool foreign_key_unsupported_rows=
    temporary_table || requested_engine_discards_rows || use_volatile_rows;
  const int foreign_key_validation_error=
    mylite_validate_foreign_key_definitions(
      primary_file, logical_schema_name, logical_table_name, form,
      create_info, foreign_key_unsupported_rows);
  if (foreign_key_validation_error)
    DBUG_RETURN(foreign_key_validation_error);
  const int retained_foreign_key_validation_error=
    mylite_validate_retained_foreign_keys(
      primary_file, logical_schema_name, logical_table_name, form,
      create_info);
  if (retained_foreign_key_validation_error)
    DBUG_RETURN(retained_foreign_key_validation_error);

  if (!temporary_table)
  {
    const uchar *frm= NULL;
    size_t frm_len= 0;
    if (form->s->read_frm_image(&frm, &frm_len))
      DBUG_RETURN(HA_ERR_GENERIC);

    const mylite_storage_table_definition definition= {
      sizeof(definition),
      schema_name,
      table_name,
      requested_engine_name,
      MYLITE_STORAGE_ENGINE_NAME,
      frm,
      frm_len
    };
    const mylite_storage_result result=
      mylite_storage_store_table_definition(primary_file, &definition);
    form->s->free_frm_image(frm);
    if (result != MYLITE_STORAGE_OK)
      DBUG_RETURN(mylite_storage_to_handler_error(result));

    const int parent_foreign_key_rename_error=
      mylite_update_renamed_parent_foreign_keys(
        primary_file, logical_schema_name, logical_table_name, create_info);
    if (parent_foreign_key_rename_error)
      DBUG_RETURN(parent_foreign_key_rename_error);

    if (!requested_engine_discards_rows && !use_volatile_rows)
    {
      const int foreign_key_store_error=
        mylite_store_foreign_key_definitions(
          primary_file, schema_name, table_name, logical_schema_name,
          logical_table_name, form, create_info);
      if (foreign_key_store_error)
        DBUG_RETURN(foreign_key_store_error);
    }
  }

  if (!temporary_table && requested_engine_discards_rows)
  {
    discard_rows= true;
    display_engine_name_lex.length= strlen(display_engine_name);
  }
  if (use_volatile_rows)
  {
    const mylite_storage_result volatile_result=
      mylite_volatile_create_table(primary_file, schema_name, table_name);
    if (volatile_result != MYLITE_STORAGE_OK)
      DBUG_RETURN(mylite_storage_to_handler_error(volatile_result));
    if (temporary_table)
    {
      const mylite_storage_result alias_result=
        mylite_volatile_register_table_alias(primary_file, alias_schema_name,
                                             alias_table_name, schema_name,
                                             table_name);
      if (alias_result != MYLITE_STORAGE_OK)
        DBUG_RETURN(mylite_storage_to_handler_error(alias_result));
    }
    volatile_rows= true;
    display_engine_name_lex.length= strlen(display_engine_name);
  }

  if (mylite_auto_increment_field(form) &&
      create_info->auto_increment_value != 0ULL)
  {
    const mylite_storage_result auto_result= use_volatile_rows ?
      mylite_volatile_set_auto_increment(primary_file, schema_name, table_name,
                                         create_info->auto_increment_value) :
      mylite_storage_set_auto_increment(primary_file, schema_name, table_name,
                                        create_info->auto_increment_value);
    if (auto_result != MYLITE_STORAGE_OK)
      DBUG_RETURN(mylite_storage_to_handler_error(auto_result));
  }

  DBUG_RETURN(0);
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

  const bool user_temporary_table=
    mylite_is_user_temporary_table_share(table_share);
  if (user_temporary_table &&
      (mylite_copy_string(table_share->db.str, schema_name, sizeof(schema_name)) ||
       mylite_copy_string(table_share->table_name.str, table_name, sizeof(table_name))))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  THD *thd= current_thd;
  if (user_temporary_table && thd && thd->lex &&
      (thd->lex->sql_command == SQLCOM_ROLLBACK ||
       thd->lex->sql_command == SQLCOM_ROLLBACK_TO_SAVEPOINT))
    DBUG_RETURN(0);

  if (!user_temporary_table)
  {
    const mylite_storage_result alias_result=
      mylite_volatile_drop_table_alias(primary_file, schema_name, table_name);
    if (alias_result == MYLITE_STORAGE_OK)
      DBUG_RETURN(0);
    if (alias_result != MYLITE_STORAGE_NOTFOUND)
      DBUG_RETURN(mylite_storage_to_handler_error(alias_result));

    const mylite_storage_result result=
      mylite_storage_drop_table(primary_file, schema_name, table_name);
    if (result != MYLITE_STORAGE_OK)
      DBUG_RETURN(mylite_storage_to_handler_error(result));
  }

  const mylite_storage_result volatile_result=
    mylite_volatile_drop_table(primary_file, schema_name, table_name);
  DBUG_RETURN(mylite_storage_to_handler_error(volatile_result));
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

  THD *thd= current_thd;
  const bool rebuild_backup_rename=
    thd && thd->lex && thd->lex->sql_command == SQLCOM_ALTER_TABLE &&
    mylite_is_alter_backup_table_name(new_table_name);
  if (rebuild_backup_rename)
  {
    const int drop_error= mylite_drop_foreign_keys_for_alter(
      primary_file, old_schema_name, old_table_name);
    if (drop_error)
      DBUG_RETURN(drop_error);
  }
  const mylite_storage_result result= rebuild_backup_rename ?
    mylite_storage_rename_table_for_rebuild_backup(
      primary_file, old_schema_name, old_table_name, new_schema_name,
      new_table_name) :
    mylite_storage_rename_table(primary_file, old_schema_name, old_table_name,
                                new_schema_name, new_table_name);
  if (result != MYLITE_STORAGE_OK)
    DBUG_RETURN(mylite_storage_to_handler_error(result));
  const mylite_storage_result volatile_result=
    mylite_volatile_rename_table(primary_file, old_schema_name, old_table_name,
                                 new_schema_name, new_table_name);
  DBUG_RETURN(mylite_storage_to_handler_error(volatile_result));
}

static int mylite_drop_foreign_keys_for_alter(const char *primary_file,
                                              const char *schema_name,
                                              const char *table_name)
{
  THD *thd= current_thd;
  if (!thd || !thd->lex || thd->lex->sql_command != SQLCOM_ALTER_TABLE)
    return 0;

  List_iterator_fast<Alter_drop> drop_it(thd->lex->alter_info.drop_list);
  while (Alter_drop *drop= drop_it++)
  {
    if (drop->type != Alter_drop::FOREIGN_KEY)
      continue;

    const int error= mylite_drop_foreign_key_for_alter(
      primary_file, schema_name, table_name, drop->name);
    if (error)
      return error;
  }
  return 0;
}

static int mylite_drop_foreign_key_for_alter(const char *primary_file,
                                             const char *schema_name,
                                             const char *table_name,
                                             const Lex_ident_column &name)
{
  THD *thd= current_thd;
  if (!thd)
    return HA_ERR_INTERNAL_ERROR;

  Mylite_foreign_key_drop_context ctx= {thd, name, NULL, 0};
  const mylite_storage_result list_result=
    mylite_storage_list_foreign_keys(primary_file, schema_name, table_name,
                                     mylite_match_foreign_key_to_drop, &ctx);
  if (ctx.error)
    return ctx.error;
  if (list_result != MYLITE_STORAGE_OK)
    return mylite_storage_to_handler_error(list_result);
  if (!ctx.stored_name)
    return HA_ERR_UNSUPPORTED;

  const mylite_storage_result drop_result=
    mylite_storage_drop_foreign_key_definition(primary_file, schema_name,
                                               table_name, ctx.stored_name);
  return mylite_storage_to_handler_error(drop_result);
}

static int mylite_requested_engine_name(const char *primary_file,
                                        HA_CREATE_INFO *create_info,
                                        char *out_name, size_t out_name_size)
{
  THD *thd= current_thd;
  if (!(create_info->used_fields & HA_CREATE_USED_ENGINE))
  {
    if (mylite_preserves_requested_engine_name(thd))
      return mylite_preserve_source_requested_engine_name(
        primary_file, out_name, out_name_size);

    static const LEX_CSTRING default_engine= {STRING_WITH_LEN("DEFAULT")};
    return mylite_copy_engine_name(&default_engine, out_name, out_name_size);
  }

  if (!thd || !thd->lex || !thd->lex->m_sql_cmd)
    return HA_ERR_UNSUPPORTED;

  Storage_engine_name *storage_engine_name=
    thd->lex->m_sql_cmd->option_storage_engine_name();
  if (!storage_engine_name || !storage_engine_name->name()->str)
    return HA_ERR_UNSUPPORTED;

  return mylite_copy_engine_name(storage_engine_name->name(), out_name,
                                 out_name_size);
}

static int mylite_display_engine_name(const char *primary_file,
                                      const char *schema_name,
                                      const char *table_name, char *out_name,
                                      size_t out_name_size)
{
  mylite_storage_table_metadata metadata= {sizeof(metadata), NULL, NULL};
  const mylite_storage_result result=
    mylite_storage_read_table_metadata(primary_file, schema_name, table_name,
                                       &metadata);
  if (result == MYLITE_STORAGE_NOTFOUND)
    return mylite_copy_string(MYLITE_STORAGE_ENGINE_NAME, out_name,
                              out_name_size);
  if (result != MYLITE_STORAGE_OK)
    return mylite_storage_to_handler_error(result);

  const char *engine_name= metadata.requested_engine_name;
  if (!engine_name)
  {
    mylite_storage_free(metadata.requested_engine_name);
    mylite_storage_free(metadata.effective_engine_name);
    return HA_ERR_UNSUPPORTED;
  }
  if (mylite_engine_name_equals(engine_name, "DEFAULT"))
    engine_name= MYLITE_STORAGE_ENGINE_NAME;
  const int error= mylite_copy_string(engine_name, out_name, out_name_size);
  mylite_storage_free(metadata.requested_engine_name);
  mylite_storage_free(metadata.effective_engine_name);
  return error;
}

static bool mylite_preserves_requested_engine_name(const THD *thd)
{
  if (!thd || !thd->lex)
    return false;

  return thd->lex->sql_command == SQLCOM_ALTER_TABLE ||
         (thd->lex->sql_command == SQLCOM_CREATE_TABLE &&
          thd->lex->create_like()) ||
         thd->lex->sql_command == SQLCOM_CREATE_INDEX ||
         thd->lex->sql_command == SQLCOM_DROP_INDEX;
}

static int mylite_preserve_source_requested_engine_name(
  const char *primary_file, char *out_name, size_t out_name_size)
{
  THD *thd= current_thd;
  TABLE_LIST *source_table= mylite_requested_engine_source_table(thd);
  if (!source_table)
    return HA_ERR_UNSUPPORTED;

  char schema_name[NAME_LEN + 1];
  char table_name[NAME_LEN + 1];
  int error= mylite_copy_lex_string(&source_table->db, schema_name,
                                    sizeof(schema_name));
  if (error)
    return error;
  error= mylite_copy_lex_string(&source_table->table_name, table_name,
                                sizeof(table_name));
  if (error)
    return error;

  mylite_storage_table_metadata metadata= {sizeof(metadata), NULL, NULL};
  const mylite_storage_result result=
    mylite_storage_read_table_metadata(primary_file, schema_name, table_name,
                                       &metadata);
  if (result != MYLITE_STORAGE_OK)
    return mylite_storage_to_handler_error(result);

  error= mylite_copy_string(metadata.requested_engine_name, out_name,
                            out_name_size);
  mylite_storage_free(metadata.requested_engine_name);
  mylite_storage_free(metadata.effective_engine_name);
  return error;
}

static TABLE_LIST *mylite_requested_engine_source_table(const THD *thd)
{
  if (!thd || !thd->lex || !thd->lex->query_tables)
    return NULL;

  if (thd->lex->sql_command == SQLCOM_CREATE_TABLE &&
      thd->lex->create_like())
  {
    TABLE_LIST *create_table= thd->lex->create_last_non_select_table;
    if (create_table && create_table->next_global)
      return create_table->next_global;
    return thd->lex->query_tables->next_global;
  }

  return thd->lex->query_tables;
}

static int mylite_copy_engine_name(const LEX_CSTRING *engine_name,
                                   char *out_name, size_t out_name_size)
{
  return mylite_copy_lex_string(engine_name, out_name, out_name_size);
}

static int mylite_copy_lex_string(const LEX_CSTRING *value, char *out_value,
                                  size_t out_value_size)
{
  if (!value || !value->str || value->length == 0 ||
      value->length >= out_value_size)
    return HA_ERR_UNSUPPORTED;

  memcpy(out_value, value->str, value->length);
  out_value[value->length]= '\0';
  return 0;
}

static int mylite_copy_string(const char *value, char *out_value,
                              size_t out_value_size)
{
  if (!value || !value[0])
    return HA_ERR_UNSUPPORTED;

  const size_t value_length= strlen(value);
  if (value_length >= out_value_size)
    return HA_ERR_UNSUPPORTED;

  memcpy(out_value, value, value_length + 1);
  return 0;
}

static bool mylite_supported_engine_request(const char *engine_name)
{
  return mylite_engine_name_equals(engine_name, "DEFAULT") ||
         mylite_engine_name_equals(engine_name, MYLITE_STORAGE_ENGINE_NAME) ||
         mylite_engine_name_equals(engine_name, "InnoDB") ||
         mylite_engine_name_equals(engine_name, "MyISAM") ||
         mylite_engine_name_equals(engine_name, "Aria") ||
         mylite_discards_rows_engine_request(engine_name) ||
         mylite_uses_volatile_rows_engine_request(engine_name);
}

static bool mylite_discards_rows_engine_request(const char *engine_name)
{
  return mylite_engine_name_equals(engine_name, "BLACKHOLE");
}

static bool mylite_uses_volatile_rows_engine_request(const char *engine_name)
{
  return mylite_engine_name_equals(engine_name, "MEMORY") ||
         mylite_engine_name_equals(engine_name, "HEAP");
}

static bool mylite_engine_name_equals(const char *engine_name,
                                      const char *expected_engine_name)
{
  return my_strcasecmp_latin1(engine_name, expected_engine_name) == 0;
}

static int mylite_begin_transaction_checkpoint(THD *thd,
                                               const char *primary_file)
{
  Mylite_trx_context *ctx= mylite_trx_context(thd, true);
  if (!ctx)
    return HA_ERR_OUT_OF_MEM;
  if (ctx->transaction)
    return 0;

  mylite_storage_result result=
    mylite_storage_begin_transaction(primary_file, &ctx->transaction);
  return mylite_storage_to_handler_error(result);
}

static int mylite_begin_statement_checkpoint(THD *thd,
                                             const char *primary_file)
{
  if (mylite_storage_statement_active(primary_file))
    return 0;

  Mylite_trx_context *ctx= mylite_trx_context(thd, true);
  if (!ctx)
    return HA_ERR_OUT_OF_MEM;
  if (ctx->statement)
    return 0;

  mylite_storage_result result=
    mylite_storage_begin_statement(primary_file, &ctx->statement);
  return mylite_storage_to_handler_error(result);
}

static int mylite_finish_statement_checkpoint(THD *thd, bool commit)
{
  Mylite_trx_context *ctx= mylite_trx_context(thd, false);
  if (!ctx || !ctx->statement)
    return 0;

  mylite_storage_statement *statement= ctx->statement;
  ctx->statement= NULL;
  mylite_storage_result result;
  if (commit)
    result= mylite_storage_commit_statement(statement);
  else
    result= mylite_storage_rollback_statement(statement);
  return mylite_storage_to_handler_error(result);
}

static int mylite_finish_transaction_checkpoint(THD *thd, bool commit)
{
  Mylite_trx_context *ctx= mylite_trx_context(thd, false);
  if (!ctx || !ctx->transaction)
    return 0;

  mylite_storage_statement *transaction= ctx->transaction;
  ctx->transaction= NULL;
  mylite_storage_result result;
  if (commit)
    result= mylite_storage_commit_statement(transaction);
  else
    result= mylite_storage_rollback_statement(transaction);
  return mylite_storage_to_handler_error(result);
}

static Mylite_trx_context *mylite_trx_context(THD *thd, bool create)
{
  if (!thd)
    return NULL;

  Mylite_trx_context *ctx= static_cast<Mylite_trx_context *>(
    thd->ha_data[mylite_hton->slot].ha_ptr);
  if (ctx || !create)
    return ctx;

  ctx= static_cast<Mylite_trx_context *>(calloc(1, sizeof(*ctx)));
  if (!ctx)
    return NULL;

  thd->ha_data[mylite_hton->slot].ha_ptr= ctx;
  return ctx;
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

static bool mylite_is_alter_backup_table_name(const char *table_name)
{
  return table_name &&
         strncmp(table_name, "#sql-backup-", strlen("#sql-backup-")) == 0;
}

static bool mylite_table_supports_row_write(TABLE *table)
{
  return mylite_table_supports_auto_increment(table) &&
         (table->s->keys == 0 || mylite_table_supports_indexes(table));
}

static bool mylite_table_supports_row_lifecycle(TABLE *table)
{
  return mylite_table_supports_row_write(table);
}

static bool mylite_table_supports_auto_increment(TABLE *table)
{
  Field *auto_field= mylite_auto_increment_field(table);
  if (!auto_field)
    return true;

  for (uint i= 0; i < table->s->keys; ++i)
  {
    const KEY *key= table->key_info + i;
    if (key->user_defined_key_parts > 0 && key->key_part &&
        key->key_part[0].field == auto_field)
      return true;
  }

  return false;
}

static bool mylite_table_supports_indexes(TABLE *table)
{
  for (uint i= 0; i < table->s->keys; ++i)
  {
    if (!mylite_key_is_supported(table->key_info + i))
      return false;
  }

  return true;
}

static bool mylite_key_is_supported(const KEY *key)
{
  if (!key || key->key_length == 0 ||
      key->key_length > MYLITE_STORAGE_MAX_INDEX_KEY_SIZE ||
      (key->algorithm != HA_KEY_ALG_UNDEF &&
       key->algorithm != HA_KEY_ALG_BTREE) ||
      (key->flags & (HA_FULLTEXT_legacy | HA_SPATIAL_legacy |
                     HA_UNIQUE_HASH)))
    return false;

  for (uint i= 0; i < key->user_defined_key_parts; ++i)
  {
    const KEY_PART_INFO *key_part= key->key_part + i;
    if (!key_part->field)
      return false;
    if ((key_part->key_part_flag & HA_BLOB_PART) && key_part->length == 0)
      return false;
  }

  return true;
}

static Field *mylite_auto_increment_field(TABLE *table)
{
  if (table->found_next_number_field)
    return table->found_next_number_field;
  if (table->next_number_field)
    return table->next_number_field;

  if (table->field)
  {
    for (Field **field= table->field; *field; ++field)
    {
      if ((*field)->unireg_check == Field::NEXT_NUMBER)
        return *field;
    }
  }

  return NULL;
}

static int mylite_prepare_row_payload(TABLE *table, const uchar *buf,
                                      const uchar **out_payload,
                                      size_t *out_payload_size,
                                      uchar **out_owned_payload)
{
  *out_payload= buf;
  *out_payload_size= table->s->reclength;
  *out_owned_payload= NULL;

  if (!mylite_table_has_blob_fields(table))
    return 0;

  int error= mylite_serialize_blob_row(table, buf, out_owned_payload,
                                       out_payload_size);
  if (error)
    return error;

  *out_payload= *out_owned_payload;
  return 0;
}

static int mylite_prepare_index_entries(
  TABLE *table, const uchar *buf, mylite_storage_index_entry **out_entries,
  size_t *out_entry_count, uchar **out_key_storage)
{
  *out_entries= NULL;
  *out_entry_count= 0;
  *out_key_storage= NULL;

  if (table->s->keys == 0)
    return 0;
  if (!mylite_table_supports_indexes(table))
    return HA_ERR_UNSUPPORTED;

  size_t key_storage_size= 0;
  for (uint i= 0; i < table->s->keys; ++i)
  {
    if (table->key_info[i].key_length > SIZE_MAX - key_storage_size)
      return HA_ERR_RECORD_FILE_FULL;
    key_storage_size+= table->key_info[i].key_length;
  }
  if (key_storage_size == 0)
    return HA_ERR_UNSUPPORTED;
  const size_t key_count= table->s->keys;
  if (key_count > SIZE_MAX / sizeof(mylite_storage_index_entry))
    return HA_ERR_RECORD_FILE_FULL;

  mylite_storage_index_entry *entries=
    static_cast<mylite_storage_index_entry *>(
      malloc(key_count * sizeof(mylite_storage_index_entry)));
  uchar *key_storage= static_cast<uchar *>(malloc(key_storage_size));
  if (!entries || !key_storage)
  {
    free(entries);
    free(key_storage);
    return HA_ERR_OUT_OF_MEM;
  }

  size_t key_offset= 0;
  for (uint i= 0; i < table->s->keys; ++i)
  {
    KEY *key_info= table->key_info + i;
    key_copy(key_storage + key_offset, buf, key_info, 0);
    entries[i].size= sizeof(entries[i]);
    entries[i].index_number= i;
    entries[i].key= key_storage + key_offset;
    entries[i].key_size= key_info->key_length;
    key_offset+= key_info->key_length;
  }

  *out_entries= entries;
  *out_entry_count= table->s->keys;
  *out_key_storage= key_storage;
  return 0;
}

static void mylite_free_index_entries(mylite_storage_index_entry *entries,
                                      uchar *key_storage)
{
  free(entries);
  free(key_storage);
}

static int mylite_prepare_scan_rows(TABLE *table,
                                    const mylite_storage_rowset *rowset,
                                    uchar **out_rows,
                                    size_t *out_row_size,
                                    size_t *out_row_count,
                                    ulonglong **out_row_ids,
                                    uchar **out_blob_payloads,
                                    size_t *out_blob_payloads_size)
{
  *out_rows= NULL;
  *out_row_size= table->s->reclength;
  *out_row_count= 0;
  *out_row_ids= NULL;
  *out_blob_payloads= NULL;
  *out_blob_payloads_size= 0;

  if (rowset->row_count == 0)
    return 0;
  if (rowset->row_offsets == NULL || rowset->row_sizes == NULL ||
      rowset->row_ids == NULL || table->s->reclength == 0 ||
      rowset->row_count > SIZE_MAX / table->s->reclength)
    return HA_ERR_CRASHED_ON_USAGE;

  size_t blob_payloads_size= 0;
  for (size_t i= 0; i < rowset->row_count; ++i)
  {
    const size_t payload_offset= rowset->row_offsets[i];
    const size_t payload_size= rowset->row_sizes[i];
    if (payload_offset > rowset->row_bytes ||
        payload_size > rowset->row_bytes - payload_offset)
      return HA_ERR_CRASHED_ON_USAGE;

    size_t row_blob_payloads_size= 0;
    int error= mylite_scan_stored_row(table, rowset->rows + payload_offset,
                                      payload_size, &row_blob_payloads_size);
    if (error)
      return error;
    if (row_blob_payloads_size > SIZE_MAX - blob_payloads_size)
      return HA_ERR_RECORD_FILE_FULL;
    blob_payloads_size+= row_blob_payloads_size;
  }

  const size_t rows_size= rowset->row_count * table->s->reclength;
  uchar *rows= static_cast<uchar *>(malloc(rows_size));
  if (!rows)
    return HA_ERR_OUT_OF_MEM;

  ulonglong *row_ids= static_cast<ulonglong *>(
    malloc(rowset->row_count * sizeof(ulonglong)));
  if (!row_ids)
  {
    free(rows);
    return HA_ERR_OUT_OF_MEM;
  }

  uchar *blob_payloads= NULL;
  if (blob_payloads_size > 0)
  {
    blob_payloads= static_cast<uchar *>(malloc(blob_payloads_size));
    if (!blob_payloads)
    {
      free(rows);
      free(row_ids);
      return HA_ERR_OUT_OF_MEM;
    }
  }

  size_t blob_payloads_used= 0;
  for (size_t i= 0; i < rowset->row_count; ++i)
  {
    const size_t payload_offset= rowset->row_offsets[i];
    const size_t payload_size= rowset->row_sizes[i];
    uchar *out_row= rows + (i * table->s->reclength);
    int error= mylite_copy_stored_row_to_scan(table,
                                              rowset->rows + payload_offset,
                                              payload_size, out_row,
                                              blob_payloads,
                                              &blob_payloads_used);
    if (error)
    {
      free(rows);
      free(row_ids);
      free(blob_payloads);
      return error;
    }
    row_ids[i]= (ulonglong) rowset->row_ids[i];
  }
  if (blob_payloads_used != blob_payloads_size)
  {
    free(rows);
    free(row_ids);
    free(blob_payloads);
    return HA_ERR_CRASHED_ON_USAGE;
  }

  *out_rows= rows;
  *out_row_count= rowset->row_count;
  *out_row_ids= row_ids;
  *out_blob_payloads= blob_payloads;
  *out_blob_payloads_size= blob_payloads_size;
  return 0;
}

static ulonglong mylite_first_auto_increment_value(ulonglong next_value,
                                                   ulonglong offset,
                                                   ulonglong increment)
{
  if (next_value == 0ULL || offset == 0ULL || increment == 0ULL)
    return ULONGLONG_MAX;
  if (next_value <= offset)
    return offset;

  const ulonglong distance= next_value - offset;
  ulonglong steps= distance / increment;
  if (distance % increment != 0ULL)
  {
    if (steps == ULONGLONG_MAX)
      return ULONGLONG_MAX;
    ++steps;
  }
  if (steps > (ULONGLONG_MAX - offset) / increment)
    return ULONGLONG_MAX;

  return offset + (steps * increment);
}

static int mylite_check_duplicate_keys(
  const char *primary_file, const char *schema_name, const char *table_name,
  TABLE *table, const mylite_storage_index_entry *index_entries,
  size_t index_entry_count, const uchar *buf, ulonglong skip_row_id,
  uint *out_duplicate_key)
{
  *out_duplicate_key= (uint) -1;
  if (table->s->keys == 0)
    return 0;
  if (index_entry_count != table->s->keys)
    return HA_ERR_CRASHED_ON_USAGE;

  for (uint i= 0; i < table->s->keys; ++i)
  {
    KEY *key_info= table->key_info + i;
    if (!(key_info->flags & HA_NOSAME) ||
        mylite_unique_key_allows_duplicate_null(table, key_info, buf))
      continue;

    mylite_storage_index_entryset entryset= {sizeof(entryset), NULL, 0, 0};
    const mylite_storage_result storage_result=
      mylite_storage_read_index_entries(primary_file, schema_name, table_name,
                                        i, &entryset);
    if (storage_result != MYLITE_STORAGE_OK)
      return mylite_storage_to_handler_error(storage_result);

    for (size_t j= 0; j < entryset.entry_count; ++j)
    {
      if (entryset.row_ids[j] == skip_row_id)
        continue;
      if (entryset.key_sizes[j] != index_entries[i].key_size)
      {
        mylite_storage_free_index_entryset(&entryset);
        return HA_ERR_CRASHED_ON_USAGE;
      }

      const uchar *entry_key= entryset.keys + entryset.key_offsets[j];
      if (!key_buf_cmp(key_info, key_info->user_defined_key_parts, entry_key,
                       index_entries[i].key))
      {
        mylite_storage_free_index_entryset(&entryset);
        *out_duplicate_key= i;
        return HA_ERR_FOUND_DUPP_KEY;
      }
    }

    mylite_storage_free_index_entryset(&entryset);
  }

  return 0;
}

static int mylite_check_volatile_duplicate_keys(
  const char *primary_file, const char *schema_name, const char *table_name,
  TABLE *table, const mylite_storage_index_entry *index_entries,
  size_t index_entry_count, const uchar *buf, ulonglong skip_row_id,
  uint *out_duplicate_key)
{
  *out_duplicate_key= (uint) -1;
  if (table->s->keys == 0)
    return 0;
  if (index_entry_count != table->s->keys)
    return HA_ERR_CRASHED_ON_USAGE;

  for (uint i= 0; i < table->s->keys; ++i)
  {
    KEY *key_info= table->key_info + i;
    if (!(key_info->flags & HA_NOSAME) ||
        mylite_unique_key_allows_duplicate_null(table, key_info, buf))
      continue;

    mylite_storage_index_entryset entryset= {sizeof(entryset), NULL, 0, 0};
    const mylite_storage_result storage_result=
      mylite_volatile_read_index_entries(primary_file, schema_name, table_name,
                                         i, &entryset);
    if (storage_result != MYLITE_STORAGE_OK)
      return mylite_storage_to_handler_error(storage_result);

    for (size_t j= 0; j < entryset.entry_count; ++j)
    {
      if (entryset.row_ids[j] == skip_row_id)
        continue;
      if (entryset.key_sizes[j] != index_entries[i].key_size)
      {
        mylite_storage_free_index_entryset(&entryset);
        return HA_ERR_CRASHED_ON_USAGE;
      }

      const uchar *entry_key= entryset.keys + entryset.key_offsets[j];
      if (!key_buf_cmp(key_info, key_info->user_defined_key_parts,
                       entry_key, index_entries[i].key))
      {
        mylite_storage_free_index_entryset(&entryset);
        *out_duplicate_key= i;
        return HA_ERR_FOUND_DUPP_KEY;
      }
    }

    mylite_storage_free_index_entryset(&entryset);
  }

  return 0;
}

static int mylite_check_child_foreign_keys(const char *primary_file,
                                           const char *schema_name,
                                           const char *table_name,
                                           TABLE *table, const uchar *buf)
{
  Mylite_foreign_key_row_check_context ctx= {primary_file, table, NULL, buf, 0};
  const mylite_storage_result result=
    mylite_storage_list_foreign_keys(primary_file, schema_name, table_name,
                                     mylite_check_child_foreign_key, &ctx);
  if (ctx.error)
    return ctx.error;
  return mylite_storage_to_handler_error(result);
}

static int mylite_check_parent_foreign_keys(const char *primary_file,
                                            const char *schema_name,
                                            const char *table_name,
                                            TABLE *table,
                                            const uchar *old_data,
                                            const uchar *new_data)
{
  Mylite_foreign_key_row_check_context ctx=
    {primary_file, table, old_data, new_data, 0};
  const mylite_storage_result result=
    mylite_storage_list_parent_foreign_keys(primary_file, schema_name,
                                            table_name,
                                            mylite_check_parent_foreign_key,
                                            &ctx);
  if (ctx.error)
    return ctx.error;
  return mylite_storage_to_handler_error(result);
}

static int mylite_validate_foreign_key_definitions(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, HA_CREATE_INFO *create_info,
  bool volatile_or_discarded_rows)
{
  if (!create_info || !create_info->alter_info)
    return 0;

  uint foreign_key_number= 0;
  List_iterator_fast<Key> key_it(create_info->alter_info->key_list);
  while (Key *key= key_it++)
  {
    if (key->type != Key::FOREIGN_KEY || key->old)
      continue;

    ++foreign_key_number;
    int error= mylite_validate_foreign_key_definition(
      primary_file, logical_schema_name, logical_table_name, form,
      static_cast<Foreign_key *>(key), foreign_key_number,
      volatile_or_discarded_rows);
    if (error)
      return error;
  }
  return 0;
}

static int mylite_validate_retained_foreign_keys(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, HA_CREATE_INFO *create_info)
{
  THD *thd= current_thd;
  if (!thd || !thd->lex ||
      (thd->lex->sql_command != SQLCOM_ALTER_TABLE &&
       thd->lex->sql_command != SQLCOM_CREATE_INDEX &&
       thd->lex->sql_command != SQLCOM_DROP_INDEX) ||
      !create_info || !create_info->alter_info)
    return 0;

  Mylite_retained_foreign_key_validation_context ctx=
    {form, create_info, 0};
  mylite_storage_result result=
    mylite_storage_list_foreign_keys(
      primary_file, logical_schema_name, logical_table_name,
      mylite_validate_retained_child_foreign_key, &ctx);
  if (ctx.error)
    return ctx.error;
  if (result != MYLITE_STORAGE_OK)
    return mylite_storage_to_handler_error(result);

  result= mylite_storage_list_parent_foreign_keys(
    primary_file, logical_schema_name, logical_table_name,
    mylite_validate_retained_parent_foreign_key, &ctx);
  if (ctx.error)
    return ctx.error;
  return mylite_storage_to_handler_error(result);
}

static int mylite_validate_retained_child_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata)
{
  Mylite_retained_foreign_key_validation_context *validation_ctx=
    static_cast<Mylite_retained_foreign_key_validation_context *>(ctx);
  if (mylite_alter_drops_foreign_key(validation_ctx->create_info, metadata))
    return 0;

  const KEY *key= mylite_find_foreign_key_prefix(
    validation_ctx->table, metadata->foreign_column_names,
    metadata->column_count, NULL);
  if (key)
    return 0;

  validation_ctx->error=
    mylite_reject_dropped_foreign_key_supporting_key(
      metadata, metadata->constraint_name);
  return 1;
}

static int mylite_validate_retained_parent_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata)
{
  Mylite_retained_foreign_key_validation_context *validation_ctx=
    static_cast<Mylite_retained_foreign_key_validation_context *>(ctx);
  if (mylite_foreign_key_is_self_referencing(metadata) &&
      mylite_alter_drops_foreign_key(validation_ctx->create_info, metadata))
    return 0;

  const char *referenced_key_name=
    mylite_foreign_key_referenced_key_name_for_alter(
      validation_ctx->create_info, metadata->referenced_key_name);
  const KEY *key= mylite_find_foreign_key_prefix(
    validation_ctx->table, metadata->referenced_column_names,
    metadata->column_count, referenced_key_name);
  if (key && (key->flags & HA_NOSAME) &&
      key->user_defined_key_parts == metadata->column_count)
    return 0;

  validation_ctx->error=
    mylite_reject_dropped_foreign_key_supporting_key(
      metadata, referenced_key_name);
  return 1;
}

static const char *mylite_foreign_key_referenced_key_name_for_alter(
  HA_CREATE_INFO *create_info, const char *referenced_key_name)
{
  if (!referenced_key_name || !referenced_key_name[0])
    return referenced_key_name;

  const char *renamed_key_name=
    mylite_foreign_key_referenced_key_name_in_alter_info(
      create_info ? create_info->alter_info : NULL, referenced_key_name);
  if (renamed_key_name != referenced_key_name)
    return renamed_key_name;

  return mylite_foreign_key_referenced_key_name_in_alter_info(
    mylite_current_alter_info_for_key_rename(), referenced_key_name);
}

static const char *mylite_foreign_key_referenced_key_name_in_alter_info(
  Alter_info *alter_info, const char *referenced_key_name)
{
  if (!alter_info || !referenced_key_name || !referenced_key_name[0])
    return referenced_key_name;

  LEX_CSTRING stored_name=
    {referenced_key_name, strlen(referenced_key_name)};
  List_iterator_fast<Alter_rename_key> rename_key_it(
    alter_info->alter_rename_key_list);
  while (Alter_rename_key *rename_key= rename_key_it++)
  {
    if (Lex_ident_column(stored_name).streq(rename_key->old_name))
      return rename_key->new_name.str;
  }
  return referenced_key_name;
}

static bool mylite_alter_renames_keys(HA_CREATE_INFO *create_info)
{
  if (create_info && create_info->alter_info &&
      create_info->alter_info->alter_rename_key_list.elements)
    return true;

  Alter_info *alter_info= mylite_current_alter_info_for_key_rename();
  return alter_info && alter_info->alter_rename_key_list.elements;
}

static Alter_info *mylite_current_alter_info_for_key_rename()
{
  THD *thd= current_thd;
  if (!thd || !thd->lex)
    return NULL;

  switch (thd->lex->sql_command) {
  case SQLCOM_ALTER_TABLE:
  case SQLCOM_CREATE_INDEX:
  case SQLCOM_DROP_INDEX:
    return &thd->lex->alter_info;
  default:
    return NULL;
  }
}

static bool mylite_alter_drops_foreign_key(
  HA_CREATE_INFO *create_info,
  const mylite_storage_foreign_key_metadata *metadata)
{
  if (!create_info || !create_info->alter_info || !metadata ||
      !metadata->constraint_name)
    return false;

  LEX_CSTRING constraint_name=
    {metadata->constraint_name, strlen(metadata->constraint_name)};
  List_iterator_fast<Alter_drop> drop_it(create_info->alter_info->drop_list);
  while (Alter_drop *drop= drop_it++)
  {
    if (drop->type == Alter_drop::FOREIGN_KEY &&
        Lex_ident_column(constraint_name).streq(drop->name))
      return true;
  }
  return false;
}

static bool mylite_foreign_key_is_self_referencing(
  const mylite_storage_foreign_key_metadata *metadata)
{
  return metadata && metadata->schema_name && metadata->table_name &&
         metadata->referenced_schema_name && metadata->referenced_table_name &&
         strcmp(metadata->schema_name, metadata->referenced_schema_name) == 0 &&
         strcmp(metadata->table_name, metadata->referenced_table_name) == 0;
}

static int mylite_reject_dropped_foreign_key_supporting_key(
  const mylite_storage_foreign_key_metadata *metadata,
  const char *fallback_key_name)
{
  const char *key_name= fallback_key_name && fallback_key_name[0] ?
    fallback_key_name :
    metadata && metadata->constraint_name ? metadata->constraint_name : "";
  my_error(ER_DROP_INDEX_FK, MYF(0), key_name);
  return HA_ERR_UNSUPPORTED;
}

static int mylite_update_renamed_parent_foreign_keys(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, HA_CREATE_INFO *create_info)
{
  if (!mylite_alter_renames_keys(create_info))
    return 0;

  Mylite_parent_foreign_key_rename_context ctx=
    {primary_file, create_info, 0};
  const mylite_storage_result result=
    mylite_storage_list_parent_foreign_keys(
      primary_file, logical_schema_name, logical_table_name,
      mylite_update_renamed_parent_foreign_key, &ctx);
  if (ctx.error)
    return ctx.error;
  return mylite_storage_to_handler_error(result);
}

static int mylite_update_renamed_parent_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata)
{
  Mylite_parent_foreign_key_rename_context *rename_ctx=
    static_cast<Mylite_parent_foreign_key_rename_context *>(ctx);
  if (!metadata || !metadata->referenced_key_name ||
      !metadata->referenced_key_name[0])
    return 0;
  if (mylite_foreign_key_is_self_referencing(metadata) &&
      mylite_alter_drops_foreign_key(rename_ctx->create_info, metadata))
    return 0;

  const char *referenced_key_name=
    mylite_foreign_key_referenced_key_name_for_alter(
      rename_ctx->create_info, metadata->referenced_key_name);
  if (!referenced_key_name || !referenced_key_name[0])
    return 0;

  LEX_CSTRING stored_name=
    {metadata->referenced_key_name, strlen(metadata->referenced_key_name)};
  LEX_CSTRING renamed_name=
    {referenced_key_name, strlen(referenced_key_name)};
  if (Lex_ident_column(stored_name).streq(renamed_name))
    return 0;

  const mylite_storage_result result=
    mylite_storage_update_foreign_key_referenced_key_name(
      rename_ctx->primary_file, metadata->schema_name, metadata->table_name,
      metadata->constraint_name, referenced_key_name);
  if (result == MYLITE_STORAGE_OK)
    return 0;

  rename_ctx->error= mylite_storage_to_handler_error(result);
  return 1;
}

static int mylite_store_foreign_key_definitions(
  const char *primary_file, const char *storage_schema_name,
  const char *storage_table_name, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, HA_CREATE_INFO *create_info)
{
  if (!create_info || !create_info->alter_info)
    return 0;

  uint foreign_key_number= 0;
  List_iterator_fast<Key> key_it(create_info->alter_info->key_list);
  while (Key *key= key_it++)
  {
    if (key->type != Key::FOREIGN_KEY || key->old)
      continue;

    ++foreign_key_number;
    int error= mylite_store_foreign_key_definition(
      primary_file, storage_schema_name, storage_table_name,
      logical_schema_name, logical_table_name, form,
      static_cast<Foreign_key *>(key), foreign_key_number);
    if (error)
      return error;
  }
  return 0;
}

static int mylite_validate_foreign_key_definition(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, const Foreign_key *fk,
  uint foreign_key_number, bool volatile_or_discarded_rows)
{
  THD *thd= current_thd;
  mylite_storage_foreign_key_definition definition= {0};
  int error= mylite_prepare_foreign_key_definition(
    thd, logical_schema_name, logical_table_name, logical_schema_name,
    logical_table_name, form, fk, foreign_key_number, &definition);
  if (error)
    return error;

  if (volatile_or_discarded_rows ||
      !mylite_foreign_key_action_supported(fk->delete_opt) ||
      !mylite_foreign_key_action_supported(fk->update_opt) ||
      (fk->match_opt != Foreign_key::FK_MATCH_UNDEF &&
       fk->match_opt != Foreign_key::FK_MATCH_SIMPLE))
  {
    my_error(ER_FK_INCORRECT_OPTION, MYF(0), logical_table_name,
             definition.constraint_name);
    return HA_ERR_UNSUPPORTED;
  }

  mylite_storage_foreign_key_metadata existing= {sizeof(existing)};
  mylite_storage_result storage_result=
    mylite_storage_read_foreign_key_definition(
      primary_file, logical_schema_name, logical_table_name,
      definition.constraint_name, &existing);
  mylite_storage_free_foreign_key_metadata(&existing);
  if (storage_result == MYLITE_STORAGE_OK)
  {
    my_error(ER_DUP_CONSTRAINT_NAME, MYF(0), "FOREIGN KEY",
             definition.constraint_name);
    return HA_ERR_UNSUPPORTED;
  }
  if (storage_result != MYLITE_STORAGE_NOTFOUND)
    return mylite_storage_to_handler_error(storage_result);

  return mylite_validate_foreign_key_shape(
    primary_file, logical_schema_name, logical_table_name, form, thd,
    &definition);
}

static int mylite_store_foreign_key_definition(
  const char *primary_file, const char *storage_schema_name,
  const char *storage_table_name, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, const Foreign_key *fk,
  uint foreign_key_number)
{
  THD *thd= current_thd;
  mylite_storage_foreign_key_definition definition= {0};
  int error= mylite_prepare_foreign_key_definition(
    thd, storage_schema_name, storage_table_name, logical_schema_name,
    logical_table_name, form, fk, foreign_key_number, &definition);
  if (error)
    return error;

  error= mylite_validate_foreign_key_shape(
    primary_file, logical_schema_name, logical_table_name, form, thd,
    &definition);
  if (error)
    return error;

  const mylite_storage_result result=
    mylite_storage_store_foreign_key_definition(primary_file, &definition);
  return mylite_storage_to_handler_error(result);
}

static int mylite_prepare_foreign_key_definition(
  THD *thd, const char *storage_schema_name, const char *storage_table_name,
  const char *logical_schema_name, const char *logical_table_name, TABLE *form,
  const Foreign_key *fk, uint foreign_key_number,
  mylite_storage_foreign_key_definition *out_definition)
{
  if (!thd || !storage_schema_name || !storage_table_name ||
      !logical_schema_name || !form || !fk || !out_definition)
    return HA_ERR_INTERNAL_ERROR;

  const char *constraint_name=
    mylite_foreign_key_constraint_name(thd, logical_table_name, fk,
                                       foreign_key_number);
  if (!constraint_name)
    return HA_ERR_OUT_OF_MEM;

  const char **foreign_column_names= NULL;
  const char **referenced_column_names= NULL;
  size_t foreign_column_count= 0;
  size_t referenced_column_count= 0;
  int error= mylite_foreign_key_columns_from_list(
    thd, const_cast<List<Key_part_spec> *>(&fk->columns),
    &foreign_column_names, &foreign_column_count);
  if (error)
    return error;
  error= mylite_foreign_key_columns_from_list(
    thd, const_cast<List<Key_part_spec> *>(&fk->ref_columns),
    &referenced_column_names, &referenced_column_count);
  if (error)
    return error;
  if (foreign_column_count != referenced_column_count)
  {
    my_error(ER_WRONG_FK_DEF, MYF(0), constraint_name,
             ER_THD(thd, ER_KEY_REF_DO_NOT_MATCH_TABLE_REF));
    return HA_ERR_UNSUPPORTED;
  }

  const char *referenced_schema_name= logical_schema_name;
  if (fk->ref_db.str && fk->ref_db.length)
  {
    referenced_schema_name= thd->strmake(fk->ref_db.str, fk->ref_db.length);
    if (!referenced_schema_name)
      return HA_ERR_OUT_OF_MEM;
  }
  const char *referenced_table_name=
    thd->strmake(fk->ref_table.str, fk->ref_table.length);
  if (!referenced_table_name)
    return HA_ERR_OUT_OF_MEM;

  *out_definition= {
    sizeof(*out_definition),
    storage_schema_name,
    storage_table_name,
    constraint_name,
    referenced_schema_name,
    referenced_table_name,
    NULL,
    foreign_column_names,
    referenced_column_names,
    foreign_column_count,
    mylite_foreign_key_storage_action(fk->update_opt),
    mylite_foreign_key_storage_action(fk->delete_opt),
    mylite_foreign_key_storage_match(fk->match_opt),
    0ULL,
    0ULL
  };
  return 0;
}

static int mylite_validate_foreign_key_shape(
  const char *primary_file, const char *logical_schema_name,
  const char *logical_table_name, TABLE *form, THD *thd,
  mylite_storage_foreign_key_definition *definition)
{
  const KEY *child_key= mylite_find_foreign_key_prefix(
    form, definition->foreign_column_names, definition->column_count, NULL);
  if (!child_key)
  {
    my_error(ER_CANNOT_ADD_FOREIGN, MYF(0), logical_table_name);
    return HA_ERR_UNSUPPORTED;
  }

  const bool self_reference=
    strcmp(definition->referenced_schema_name, logical_schema_name) == 0 &&
    strcmp(definition->referenced_table_name, logical_table_name) == 0;
  if (self_reference)
  {
    const KEY *parent_key= mylite_find_foreign_key_prefix_in_share(
      form->s, definition->referenced_column_names, definition->column_count,
      NULL, true);
    if (!parent_key ||
        mylite_validate_foreign_key_key_parts(child_key, parent_key,
                                              definition->column_count))
    {
      my_error(ER_CANNOT_ADD_FOREIGN, MYF(0), logical_table_name);
      return HA_ERR_UNSUPPORTED;
    }
    if (!parent_key->name.str || !parent_key->name.length)
      return HA_ERR_UNSUPPORTED;
    definition->referenced_key_name=
      thd->strmake(parent_key->name.str, parent_key->name.length);
    if (!definition->referenced_key_name)
      return HA_ERR_OUT_OF_MEM;
    definition->nullable_column_bitmap=
      mylite_key_nullable_bitmap(child_key, definition->column_count);
    definition->referenced_nullable_column_bitmap=
      mylite_key_nullable_bitmap(parent_key, definition->column_count);
    return 0;
  }

  int error= mylite_validate_foreign_key_parent_engine(
    primary_file, definition->referenced_schema_name,
    definition->referenced_table_name);
  if (error)
  {
    my_error(ER_CANNOT_ADD_FOREIGN, MYF(0), logical_table_name);
    return error;
  }

  Mylite_catalog_table_share parent_share= {};
  error= mylite_init_catalog_table_share(
    thd, primary_file, definition->referenced_schema_name,
    definition->referenced_table_name, &parent_share);
  if (error)
  {
    my_error(ER_CANNOT_ADD_FOREIGN, MYF(0), logical_table_name);
    return error;
  }

  error= mylite_validate_foreign_key_parent_share(definition, form,
                                                  &parent_share.share);
  if (!error)
  {
    const KEY *parent_key= mylite_find_foreign_key_prefix_in_share(
      &parent_share.share, definition->referenced_column_names,
      definition->column_count, NULL, true);
    DBUG_ASSERT(parent_key);
    if (!parent_key->name.str || !parent_key->name.length)
      error= HA_ERR_UNSUPPORTED;
    definition->referenced_key_name=
      error ? NULL : thd->strmake(parent_key->name.str,
                                  parent_key->name.length);
    if (!error && !definition->referenced_key_name)
      error= HA_ERR_OUT_OF_MEM;
  }
  mylite_free_catalog_table_share(&parent_share);
  if (error)
  {
    if (error == HA_ERR_UNSUPPORTED)
      my_error(ER_CANNOT_ADD_FOREIGN, MYF(0), logical_table_name);
    return error;
  }

  definition->nullable_column_bitmap=
    mylite_key_nullable_bitmap(child_key, definition->column_count);
  return 0;
}

static int mylite_validate_foreign_key_parent_engine(
  const char *primary_file, const char *schema_name, const char *table_name)
{
  mylite_storage_table_metadata metadata= {sizeof(metadata), NULL, NULL};
  const mylite_storage_result storage_result=
    mylite_storage_read_table_metadata(primary_file, schema_name, table_name,
                                       &metadata);
  if (storage_result != MYLITE_STORAGE_OK)
    return mylite_storage_to_handler_error(storage_result);

  const char *requested_engine_name= metadata.requested_engine_name ?
    metadata.requested_engine_name : "";
  const bool unsupported_parent_engine=
    mylite_discards_rows_engine_request(requested_engine_name) ||
    mylite_uses_volatile_rows_engine_request(requested_engine_name);
  mylite_storage_free(metadata.requested_engine_name);
  mylite_storage_free(metadata.effective_engine_name);
  return unsupported_parent_engine ? HA_ERR_UNSUPPORTED : 0;
}

static int mylite_validate_foreign_key_parent_share(
  mylite_storage_foreign_key_definition *definition, TABLE *child_table,
  const TABLE_SHARE *parent_share)
{
  const KEY *child_key= mylite_find_foreign_key_prefix(
    child_table, definition->foreign_column_names, definition->column_count,
    NULL);
  const KEY *parent_key= mylite_find_foreign_key_prefix_in_share(
    parent_share, definition->referenced_column_names,
    definition->column_count, NULL, true);
  if (!child_key || !parent_key ||
      mylite_validate_foreign_key_key_parts(child_key, parent_key,
                                            definition->column_count))
    return HA_ERR_UNSUPPORTED;

  definition->referenced_nullable_column_bitmap=
    mylite_key_nullable_bitmap(parent_key, definition->column_count);
  return 0;
}

static int mylite_validate_foreign_key_key_parts(
  const KEY *child_key, const KEY *parent_key, size_t column_count)
{
  if (!child_key || !parent_key || column_count == 0 ||
      child_key->user_defined_key_parts < column_count ||
      parent_key->user_defined_key_parts != column_count)
    return HA_ERR_UNSUPPORTED;

  for (size_t i= 0; i < column_count; ++i)
  {
    const KEY_PART_INFO *child_part= child_key->key_part + i;
    const KEY_PART_INFO *parent_part= parent_key->key_part + i;
    if (!child_part->field || !parent_part->field ||
        !child_part->field->eq_def(parent_part->field) ||
        child_part->length != child_part->field->key_length() ||
        parent_part->length != parent_part->field->key_length())
      return HA_ERR_UNSUPPORTED;

    const size_t child_value_size= child_part->store_length -
      (child_part->null_bit ? HA_KEY_NULL_LENGTH : 0U);
    const size_t parent_value_size= parent_part->store_length -
      (parent_part->null_bit ? HA_KEY_NULL_LENGTH : 0U);
    if (child_value_size != parent_value_size)
      return HA_ERR_UNSUPPORTED;
  }
  return 0;
}

static int mylite_init_catalog_table_share(
  THD *thd, const char *primary_file, const char *schema_name,
  const char *table_name, Mylite_catalog_table_share *out_share)
{
  if (!thd || !primary_file || !schema_name || !table_name || !out_share)
    return HA_ERR_INTERNAL_ERROR;

  *out_share= {};
  unsigned char *frm= NULL;
  size_t frm_len= 0;
  mylite_storage_result storage_result=
    mylite_storage_read_table_definition(primary_file, schema_name,
                                         table_name, &frm, &frm_len);
  if (storage_result != MYLITE_STORAGE_OK)
    return mylite_storage_to_handler_error(storage_result);

  my_snprintf(out_share->path, sizeof(out_share->path), "%s/%s",
              schema_name, table_name);
  init_tmp_table_share(thd, &out_share->share, schema_name, 0, table_name,
                       out_share->path, true);
  out_share->initialized= true;
  const int error=
    out_share->share.init_from_binary_frm_image(thd, false, frm, frm_len);
  mylite_storage_free(frm);
  if (error)
  {
    mylite_free_catalog_table_share(out_share);
    return HA_ERR_CRASHED_ON_USAGE;
  }
  return 0;
}

static void mylite_free_catalog_table_share(
  Mylite_catalog_table_share *catalog_share)
{
  if (!catalog_share || !catalog_share->initialized)
    return;

  free_table_share(&catalog_share->share);
  catalog_share->initialized= false;
}

static int mylite_foreign_key_columns_from_list(
  THD *thd, List<Key_part_spec> *columns, const char ***out_column_names,
  size_t *out_column_count)
{
  if (!thd || !columns || !out_column_names || !out_column_count ||
      columns->elements == 0 ||
      columns->elements > sizeof(unsigned long long) * CHAR_BIT)
    return HA_ERR_UNSUPPORTED;

  const char **column_names= static_cast<const char **>(
    thd_alloc(thd, columns->elements * sizeof(char *)));
  if (!column_names)
    return HA_ERR_OUT_OF_MEM;

  size_t index= 0;
  List_iterator_fast<Key_part_spec> column_it(*columns);
  while (Key_part_spec *column= column_it++)
  {
    if (!column->field_name.str || !column->field_name.length)
      return HA_ERR_UNSUPPORTED;
    column_names[index]= thd->strmake(column->field_name.str,
                                      column->field_name.length);
    if (!column_names[index])
      return HA_ERR_OUT_OF_MEM;
    ++index;
  }

  *out_column_names= column_names;
  *out_column_count= index;
  return 0;
}

static const char *mylite_foreign_key_constraint_name(
  THD *thd, const char *logical_table_name, const Foreign_key *fk,
  uint foreign_key_number)
{
  if (fk->constraint_name.str && fk->constraint_name.length)
    return thd->strmake(fk->constraint_name.str, fk->constraint_name.length);
  if (fk->name.str && fk->name.length)
    return thd->strmake(fk->name.str, fk->name.length);

  char generated_name[NAME_LEN + 32];
  my_snprintf(generated_name, sizeof(generated_name), "%s_ibfk_%u",
              logical_table_name, foreign_key_number);
  return thd->strmake(generated_name, strlen(generated_name));
}

static unsigned mylite_foreign_key_storage_action(enum_fk_option action)
{
  switch (action) {
  case FK_OPTION_RESTRICT:
    return MYLITE_STORAGE_FOREIGN_KEY_ACTION_RESTRICT;
  case FK_OPTION_NO_ACTION:
    return MYLITE_STORAGE_FOREIGN_KEY_ACTION_NO_ACTION;
  case FK_OPTION_CASCADE:
    return MYLITE_STORAGE_FOREIGN_KEY_ACTION_CASCADE;
  case FK_OPTION_SET_NULL:
    return MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_NULL;
  case FK_OPTION_SET_DEFAULT:
    return MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_DEFAULT;
  case FK_OPTION_UNDEF:
  default:
    return MYLITE_STORAGE_FOREIGN_KEY_ACTION_UNSPECIFIED;
  }
}

static unsigned mylite_foreign_key_storage_match(
  Foreign_key::fk_match_opt match_option)
{
  switch (match_option) {
  case Foreign_key::FK_MATCH_SIMPLE:
    return MYLITE_STORAGE_FOREIGN_KEY_MATCH_SIMPLE;
  case Foreign_key::FK_MATCH_FULL:
    return MYLITE_STORAGE_FOREIGN_KEY_MATCH_FULL;
  case Foreign_key::FK_MATCH_PARTIAL:
    return MYLITE_STORAGE_FOREIGN_KEY_MATCH_PARTIAL;
  case Foreign_key::FK_MATCH_UNDEF:
  default:
    return MYLITE_STORAGE_FOREIGN_KEY_MATCH_UNSPECIFIED;
  }
}

static bool mylite_foreign_key_action_supported(enum_fk_option action)
{
  return action == FK_OPTION_UNDEF || action == FK_OPTION_RESTRICT ||
         action == FK_OPTION_NO_ACTION;
}

static unsigned long long mylite_key_nullable_bitmap(const KEY *key,
                                                     size_t column_count)
{
  unsigned long long bitmap= 0ULL;
  if (!key)
    return bitmap;

  for (size_t i= 0; i < column_count; ++i)
  {
    if (key->key_part[i].null_bit)
      bitmap|= 1ULL << i;
  }
  return bitmap;
}

static int mylite_check_child_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata)
{
  Mylite_foreign_key_row_check_context *check_ctx=
    static_cast<Mylite_foreign_key_row_check_context *>(ctx);
  const KEY *key= mylite_find_foreign_key_prefix(
    check_ctx->table, metadata->foreign_column_names, metadata->column_count,
    NULL);
  if (!key)
  {
    check_ctx->error= HA_ERR_NO_REFERENCED_ROW;
    return 1;
  }
  if (mylite_key_prefix_contains_null(check_ctx->table, key,
                                      check_ctx->new_data,
                                      metadata->column_count))
    return 0;

  uchar *key_prefix= NULL;
  size_t key_prefix_size= 0;
  int error= mylite_make_key_prefix(check_ctx->table, key, check_ctx->new_data,
                                    metadata->column_count,
                                    metadata->referenced_nullable_column_bitmap,
                                    &key_prefix, &key_prefix_size);
  if (error)
  {
    check_ctx->error= error;
    return 1;
  }

  bool same_row_reference= false;
  error= mylite_check_same_row_self_reference(
    check_ctx->table, metadata, check_ctx->new_data, key_prefix,
    key_prefix_size, &same_row_reference);
  if (error)
  {
    free(key_prefix);
    check_ctx->error= error;
    return 1;
  }
  if (same_row_reference)
  {
    free(key_prefix);
    return 0;
  }

  int exists= 0;
  const mylite_storage_result result=
    mylite_storage_index_prefix_exists(check_ctx->primary_file,
                                       metadata->referenced_schema_name,
                                       metadata->referenced_table_name,
                                       key_prefix, key_prefix_size, &exists);
  free(key_prefix);
  if (result != MYLITE_STORAGE_OK)
  {
    check_ctx->error= mylite_storage_to_handler_error(result);
    return 1;
  }
  if (!exists)
  {
    check_ctx->error= HA_ERR_NO_REFERENCED_ROW;
    return 1;
  }
  return 0;
}

static int mylite_check_same_row_self_reference(
  TABLE *table, const mylite_storage_foreign_key_metadata *metadata,
  const uchar *new_data, const uchar *child_key_prefix,
  size_t child_key_prefix_size, bool *out_matches)
{
  *out_matches= false;
  if (!mylite_foreign_key_is_self_referencing(metadata))
    return 0;

  const KEY *parent_key= mylite_find_foreign_key_prefix(
    table, metadata->referenced_column_names, metadata->column_count,
    metadata->referenced_key_name);
  if (!parent_key ||
      mylite_key_prefix_contains_null(table, parent_key, new_data,
                                      metadata->column_count))
    return 0;

  uchar *parent_key_prefix= NULL;
  size_t parent_key_prefix_size= 0;
  const int error= mylite_make_key_prefix(
    table, parent_key, new_data, metadata->column_count,
    metadata->referenced_nullable_column_bitmap, &parent_key_prefix,
    &parent_key_prefix_size);
  if (error)
    return error;

  *out_matches= mylite_key_prefix_equals(
    child_key_prefix, child_key_prefix_size, parent_key_prefix,
    parent_key_prefix_size);
  free(parent_key_prefix);
  return 0;
}

static int mylite_check_parent_foreign_key(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata)
{
  Mylite_foreign_key_row_check_context *check_ctx=
    static_cast<Mylite_foreign_key_row_check_context *>(ctx);
  const KEY *key= mylite_find_foreign_key_prefix(
    check_ctx->table, metadata->referenced_column_names,
    metadata->column_count, metadata->referenced_key_name);
  if (!key)
  {
    check_ctx->error= HA_ERR_ROW_IS_REFERENCED;
    return 1;
  }
  if (mylite_key_prefix_contains_null(check_ctx->table, key,
                                      check_ctx->old_data,
                                      metadata->column_count))
    return 0;

  uchar *old_key_prefix= NULL;
  size_t old_key_prefix_size= 0;
  int error= mylite_make_key_prefix(check_ctx->table, key, check_ctx->old_data,
                                    metadata->column_count,
                                    metadata->nullable_column_bitmap,
                                    &old_key_prefix, &old_key_prefix_size);
  if (error)
  {
    check_ctx->error= error;
    return 1;
  }

  if (check_ctx->new_data)
  {
    uchar *new_key_prefix= NULL;
    size_t new_key_prefix_size= 0;
    error= mylite_make_key_prefix(check_ctx->table, key, check_ctx->new_data,
                                  metadata->column_count,
                                  metadata->nullable_column_bitmap,
                                  &new_key_prefix, &new_key_prefix_size);
    if (error)
    {
      free(old_key_prefix);
      check_ctx->error= error;
      return 1;
    }
    const bool unchanged= mylite_key_prefix_equals(
      old_key_prefix, old_key_prefix_size, new_key_prefix,
      new_key_prefix_size);
    free(new_key_prefix);
    if (unchanged)
    {
      free(old_key_prefix);
      return 0;
    }
  }

  int exists= 0;
  const mylite_storage_result result=
    mylite_storage_index_prefix_exists(check_ctx->primary_file,
                                       metadata->schema_name,
                                       metadata->table_name,
                                       old_key_prefix, old_key_prefix_size,
                                       &exists);
  free(old_key_prefix);
  if (result != MYLITE_STORAGE_OK)
  {
    check_ctx->error= mylite_storage_to_handler_error(result);
    return 1;
  }
  if (exists)
  {
    check_ctx->error= HA_ERR_ROW_IS_REFERENCED;
    return 1;
  }
  return 0;
}

static const KEY *mylite_find_foreign_key_prefix(
  TABLE *table, const char *const *column_names, size_t column_count,
  const char *key_name)
{
  if (!table || !column_names || column_count == 0 ||
      column_count > UINT_MAX)
    return NULL;

  return mylite_find_foreign_key_prefix_in_keys(
    table->key_info, table->s->keys, column_names, column_count, key_name,
    false);
}

static const KEY *mylite_find_foreign_key_prefix_in_share(
  const TABLE_SHARE *share, const char *const *column_names,
  size_t column_count, const char *key_name, bool require_unique_exact_key)
{
  if (!share)
    return NULL;

  return mylite_find_foreign_key_prefix_in_keys(
    share->key_info, share->keys, column_names, column_count, key_name,
    require_unique_exact_key);
}

static const KEY *mylite_find_foreign_key_prefix_in_keys(
  const KEY *keys, uint key_count, const char *const *column_names,
  size_t column_count, const char *key_name, bool require_unique_exact_key)
{
  if (!keys || !column_names || column_count == 0 ||
      column_count > UINT_MAX)
    return NULL;

  for (uint i= 0; i < key_count; ++i)
  {
    const KEY *key= keys + i;
    if (!mylite_key_is_supported(key) ||
        key->user_defined_key_parts < column_count)
      continue;
    if (require_unique_exact_key &&
        (!(key->flags & HA_NOSAME) ||
         key->user_defined_key_parts != column_count))
      continue;
    if (key_name && key_name[0])
    {
      LEX_CSTRING key_name_lex= {key_name, strlen(key_name)};
      if (!key->name.streq(key_name_lex))
        continue;
    }

    bool matches= true;
    for (size_t j= 0; matches && j < column_count; ++j)
    {
      if (!column_names[j] || !column_names[j][0] ||
          !key->key_part[j].field)
      {
        matches= false;
        break;
      }
      LEX_CSTRING column_name= {column_names[j], strlen(column_names[j])};
      matches= key->key_part[j].field->field_name.streq(column_name);
    }
    if (matches)
      return key;
  }

  return NULL;
}

static bool mylite_key_prefix_contains_null(TABLE *table, const KEY *key,
                                            const uchar *buf,
                                            size_t column_count)
{
  if (!table || !key || !buf)
    return true;

  const my_ptrdiff_t row_offset= buf - table->record[0];
  for (size_t i= 0; i < column_count; ++i)
  {
    Field *field= key->key_part[i].field;
    if (field && field->is_null(row_offset))
      return true;
  }
  return false;
}

static int mylite_make_key_prefix(TABLE *, const KEY *key, const uchar *buf,
                                  size_t column_count,
                                  unsigned long long target_nullable_bitmap,
                                  uchar **out_key, size_t *out_key_size)
{
  *out_key= NULL;
  *out_key_size= 0;
  if (!key || !buf || column_count == 0 ||
      column_count > key->user_defined_key_parts ||
      column_count > sizeof(target_nullable_bitmap) * CHAR_BIT)
    return HA_ERR_CRASHED_ON_USAGE;

  size_t source_key_prefix_size= 0;
  size_t target_key_prefix_size= 0;
  for (size_t i= 0; i < column_count; ++i)
  {
    const KEY_PART_INFO *key_part= key->key_part + i;
    const bool source_nullable= key_part->null_bit != 0;
    const bool target_nullable=
      target_nullable_bitmap & (1ULL << i);
    if (source_nullable && key_part->store_length < HA_KEY_NULL_LENGTH)
      return HA_ERR_CRASHED_ON_USAGE;

    const size_t value_size= key_part->store_length -
      (source_nullable ? HA_KEY_NULL_LENGTH : 0);
    const size_t target_part_size= value_size +
      (target_nullable ? HA_KEY_NULL_LENGTH : 0);
    if (key_part->store_length > SIZE_MAX - source_key_prefix_size ||
        target_part_size > SIZE_MAX - target_key_prefix_size)
      return HA_ERR_RECORD_FILE_FULL;
    source_key_prefix_size+= key_part->store_length;
    target_key_prefix_size+= target_part_size;
  }
  if (source_key_prefix_size == 0 || target_key_prefix_size == 0 ||
      key->key_length == 0)
    return HA_ERR_CRASHED_ON_USAGE;

  uchar *key_buffer= static_cast<uchar *>(malloc(key->key_length));
  if (!key_buffer)
    return HA_ERR_OUT_OF_MEM;

  uchar *target_key= static_cast<uchar *>(malloc(target_key_prefix_size));
  if (!target_key)
  {
    free(key_buffer);
    return HA_ERR_OUT_OF_MEM;
  }

  key_copy(key_buffer, buf, key, 0);
  size_t source_offset= 0;
  size_t target_offset= 0;
  for (size_t i= 0; i < column_count; ++i)
  {
    const KEY_PART_INFO *key_part= key->key_part + i;
    const bool source_nullable= key_part->null_bit != 0;
    const bool target_nullable=
      target_nullable_bitmap & (1ULL << i);
    const uchar *source_part= key_buffer + source_offset;
    const size_t value_size= key_part->store_length -
      (source_nullable ? HA_KEY_NULL_LENGTH : 0);
    if (target_nullable)
      target_key[target_offset++]= source_nullable ? source_part[0] : 0;
    if (source_nullable)
      source_part+= HA_KEY_NULL_LENGTH;
    memcpy(target_key + target_offset, source_part, value_size);
    source_offset+= key_part->store_length;
    target_offset+= value_size;
  }
  free(key_buffer);
  if (target_offset != target_key_prefix_size ||
      source_offset != source_key_prefix_size)
  {
    free(target_key);
    return HA_ERR_CRASHED_ON_USAGE;
  }

  *out_key= target_key;
  *out_key_size= target_key_prefix_size;
  return 0;
}

static bool mylite_key_prefix_equals(const uchar *left, size_t left_size,
                                     const uchar *right, size_t right_size)
{
  return left_size == right_size &&
         (left_size == 0 || memcmp(left, right, left_size) == 0);
}

static bool mylite_unique_key_allows_duplicate_null(TABLE *table,
                                                    const KEY *key,
                                                    const uchar *buf)
{
  if (!(key->flags & HA_NULL_PART_KEY) || (key->flags & HA_NULL_ARE_EQUAL))
    return false;

  const my_ptrdiff_t row_offset= buf - table->record[0];
  for (uint i= 0; i < key->user_defined_key_parts; ++i)
  {
    const KEY_PART_INFO *key_part= key->key_part + i;
    if (key_part->null_bit && key_part->field->is_real_null(row_offset))
      return true;
  }

  return false;
}

static int mylite_advance_auto_increment_from_row(const char *primary_file,
                                                  const char *schema_name,
                                                  const char *table_name,
                                                  TABLE *table)
{
  Field *auto_field= mylite_auto_increment_field(table);
  if (!auto_field)
    return 0;

  const longlong signed_value= auto_field->val_int();
  if (signed_value < 0 && !(auto_field->flags & UNSIGNED_FLAG))
    return 0;

  const ulonglong value= (ulonglong) signed_value;
  if (value == ULONGLONG_MAX)
    return HA_ERR_AUTOINC_ERANGE;

  const mylite_storage_result result=
    mylite_storage_advance_auto_increment(primary_file, schema_name, table_name,
                                          value + 1ULL);
  return mylite_storage_to_handler_error(result);
}

static int mylite_find_index_entry(TABLE *table,
                                   const Mylite_index_cursor_entry *entries,
                                   size_t entry_count, const uchar *keys,
                                   uint index_number, const uchar *key,
                                   uint key_length,
                                   enum ha_rkey_function find_flag,
                                   size_t *out_entry_index)
{
  if (!entries || !keys)
    return HA_ERR_KEY_NOT_FOUND;

  bool found= false;
  size_t found_index= 0;
  for (size_t i= 0; i < entry_count; ++i)
  {
    int cmp= 0;
    int error=
      mylite_compare_key_tuple(table, index_number, keys + entries[i].key_offset,
                               entries[i].key_size, key, key_length,
                               key_length, &cmp);
    if (error)
      return error;

    switch (find_flag) {
    case HA_READ_KEY_EXACT:
    case HA_READ_PREFIX:
      if (cmp == 0)
      {
        *out_entry_index= i;
        return 0;
      }
      break;
    case HA_READ_KEY_OR_NEXT:
      if (cmp >= 0)
      {
        *out_entry_index= i;
        return 0;
      }
      break;
    case HA_READ_AFTER_KEY:
      if (cmp > 0)
      {
        *out_entry_index= i;
        return 0;
      }
      break;
    case HA_READ_KEY_OR_PREV:
      if (cmp <= 0)
      {
        found= true;
        found_index= i;
      }
      else if (found)
        i= entry_count;
      break;
    case HA_READ_BEFORE_KEY:
      if (cmp < 0)
      {
        found= true;
        found_index= i;
      }
      else if (found)
        i= entry_count;
      break;
    case HA_READ_PREFIX_LAST:
      if (cmp == 0)
      {
        found= true;
        found_index= i;
      }
      else if (found)
        i= entry_count;
      break;
    case HA_READ_PREFIX_LAST_OR_PREV:
      if (cmp <= 0)
      {
        found= true;
        found_index= i;
      }
      else if (found)
        i= entry_count;
      break;
    default:
      return HA_ERR_UNSUPPORTED;
    }
  }

  if (found)
  {
    *out_entry_index= found_index;
    return 0;
  }

  return HA_ERR_KEY_NOT_FOUND;
}

static int mylite_compare_key_tuple(TABLE *table, uint index_number,
                                    const uchar *left_key,
                                    size_t left_key_size,
                                    const uchar *right_key,
                                    size_t right_key_size,
                                    uint key_length, int *out_cmp)
{
  if (index_number >= table->s->keys || !left_key || !right_key ||
      key_length > left_key_size || key_length > right_key_size)
    return HA_ERR_CRASHED_ON_USAGE;

  *out_cmp= key_tuple_cmp(table->key_info[index_number].key_part, left_key,
                          right_key, key_length);
  return 0;
}

static int mylite_sort_index_entries(TABLE *table, uint index_number,
                                     const uchar *keys,
                                     Mylite_index_cursor_entry *entries,
                                     size_t entry_count)
{
  for (size_t i= 1; i < entry_count; ++i)
  {
    Mylite_index_cursor_entry value= entries[i];
    size_t j= i;
    while (j > 0)
    {
      int cmp= 0;
      const int error=
        mylite_compare_index_entries(table, index_number, keys, entries + j - 1,
                                     &value, &cmp);
      if (error)
        return error;
      if (cmp <= 0)
        break;

      entries[j]= entries[j - 1];
      --j;
    }
    entries[j]= value;
  }

  return 0;
}

static int mylite_compare_index_entries(TABLE *table, uint index_number,
                                        const uchar *keys,
                                        const Mylite_index_cursor_entry *left,
                                        const Mylite_index_cursor_entry *right,
                                        int *out_cmp)
{
  KEY *key_info= table->key_info + index_number;
  int cmp= 0;
  int error= mylite_compare_key_tuple(table, index_number,
                                      keys + left->key_offset, left->key_size,
                                      keys + right->key_offset,
                                      right->key_size, key_info->key_length,
                                      &cmp);
  if (error)
    return error;
  if (cmp == 0)
  {
    if (left->row_id < right->row_id)
      cmp= -1;
    else if (left->row_id > right->row_id)
      cmp= 1;
  }

  *out_cmp= cmp;
  return 0;
}

static int mylite_append_foreign_key_create_info(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata)
{
  Mylite_foreign_key_create_info_context *create_ctx=
    static_cast<Mylite_foreign_key_create_info_context *>(ctx);
  const int error= mylite_append_foreign_key_clause(
    create_ctx->thd, create_ctx->text, metadata);
  if (error)
  {
    create_ctx->error= error;
    return 1;
  }
  return 0;
}

static int mylite_append_foreign_key_clause(
  THD *thd, String *text,
  const mylite_storage_foreign_key_metadata *metadata)
{
  if (text->append(STRING_WITH_LEN(",\n  CONSTRAINT ")) ||
      append_identifier(thd, text, metadata->constraint_name,
                        strlen(metadata->constraint_name)) ||
      text->append(STRING_WITH_LEN(" FOREIGN KEY (")) ||
      mylite_append_foreign_key_column_names(
        thd, text, metadata->foreign_column_names, metadata->column_count) ||
      text->append(STRING_WITH_LEN(") REFERENCES ")) ||
      mylite_append_foreign_key_referenced_table(thd, text, metadata) ||
      text->append(STRING_WITH_LEN(" (")) ||
      mylite_append_foreign_key_column_names(
        thd, text, metadata->referenced_column_names, metadata->column_count) ||
      text->append(')'))
    return HA_ERR_OUT_OF_MEM;

  const char *delete_action=
    mylite_foreign_key_action_name(metadata->delete_action);
  if (delete_action && (text->append(STRING_WITH_LEN(" ON DELETE ")) ||
                        text->append(delete_action, strlen(delete_action))))
    return HA_ERR_OUT_OF_MEM;

  const char *update_action=
    mylite_foreign_key_action_name(metadata->update_action);
  if (update_action && (text->append(STRING_WITH_LEN(" ON UPDATE ")) ||
                        text->append(update_action, strlen(update_action))))
    return HA_ERR_OUT_OF_MEM;

  return 0;
}

static int mylite_append_foreign_key_column_names(THD *thd, String *text,
                                                  char **column_names,
                                                  size_t column_count)
{
  for (size_t i= 0; i < column_count; ++i)
  {
    if (i > 0 && text->append(STRING_WITH_LEN(", ")))
      return HA_ERR_OUT_OF_MEM;
    if (append_identifier(thd, text, column_names[i], strlen(column_names[i])))
      return HA_ERR_OUT_OF_MEM;
  }
  return 0;
}

static int mylite_append_foreign_key_referenced_table(
  THD *thd, String *text,
  const mylite_storage_foreign_key_metadata *metadata)
{
  if (strcmp(metadata->schema_name, metadata->referenced_schema_name) != 0)
  {
    if (append_identifier(thd, text, metadata->referenced_schema_name,
                          strlen(metadata->referenced_schema_name)) ||
        text->append('.') ||
        append_identifier(thd, text, metadata->referenced_table_name,
                          strlen(metadata->referenced_table_name)))
      return HA_ERR_OUT_OF_MEM;
    return 0;
  }

  if (append_identifier(thd, text, metadata->referenced_table_name,
                        strlen(metadata->referenced_table_name)))
    return HA_ERR_OUT_OF_MEM;
  return 0;
}

static const char *mylite_foreign_key_action_name(unsigned action)
{
  switch (action) {
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_CASCADE:
    return "CASCADE";
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_NULL:
    return "SET NULL";
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_NO_ACTION:
    return "NO ACTION";
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_DEFAULT:
    return "SET DEFAULT";
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_UNSPECIFIED:
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_RESTRICT:
  default:
    return NULL;
  }
}

static int mylite_add_foreign_key_info(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata)
{
  Mylite_foreign_key_list_context *list_ctx=
    static_cast<Mylite_foreign_key_list_context *>(ctx);
  FOREIGN_KEY_INFO *info=
    mylite_foreign_key_info_from_metadata(
      list_ctx->thd, metadata, list_ctx->map_referenced_key_name);
  if (!info)
  {
    list_ctx->error= HA_ERR_OUT_OF_MEM;
    return 1;
  }
  if (list_ctx->list->push_back(info))
  {
    list_ctx->error= HA_ERR_OUT_OF_MEM;
    return 1;
  }
  return 0;
}

static int mylite_detect_foreign_key_presence(
  void *ctx, const mylite_storage_foreign_key_metadata *)
{
  Mylite_foreign_key_presence_context *presence_ctx=
    static_cast<Mylite_foreign_key_presence_context *>(ctx);
  presence_ctx->found= true;
  return 1;
}

static int mylite_match_foreign_key_to_drop(
  void *ctx, const mylite_storage_foreign_key_metadata *metadata)
{
  Mylite_foreign_key_drop_context *drop_ctx=
    static_cast<Mylite_foreign_key_drop_context *>(ctx);
  if (drop_ctx->stored_name || !metadata || !metadata->constraint_name)
    return 0;

  LEX_CSTRING stored_name=
    {metadata->constraint_name, strlen(metadata->constraint_name)};
  if (!Lex_ident_column(stored_name).streq(drop_ctx->name))
    return 0;

  drop_ctx->stored_name= drop_ctx->thd->strmake(stored_name.str,
                                                stored_name.length);
  if (!drop_ctx->stored_name)
  {
    drop_ctx->error= HA_ERR_OUT_OF_MEM;
    return 1;
  }
  return 0;
}

static FOREIGN_KEY_INFO *mylite_foreign_key_info_from_metadata(
  THD *thd, const mylite_storage_foreign_key_metadata *metadata,
  bool map_referenced_key_name)
{
  if (!thd || !metadata)
    return NULL;

  FOREIGN_KEY_INFO info;
  info.foreign_id= mylite_make_foreign_key_string(thd,
                                                  metadata->constraint_name);
  info.foreign_db= mylite_make_foreign_key_string(thd, metadata->schema_name);
  info.foreign_table= mylite_make_foreign_key_string(thd, metadata->table_name);
  info.referenced_db= mylite_make_foreign_key_string(
    thd, metadata->referenced_schema_name);
  info.referenced_table= mylite_make_foreign_key_string(
    thd, metadata->referenced_table_name);
  if (!info.foreign_id || !info.foreign_db || !info.foreign_table ||
      !info.referenced_db || !info.referenced_table)
    return NULL;

  info.update_method= mylite_foreign_key_option(metadata->update_action);
  info.delete_method= mylite_foreign_key_option(metadata->delete_action);
  const char *referenced_key_name= map_referenced_key_name ?
    mylite_foreign_key_referenced_key_name_for_alter(
      NULL, metadata->referenced_key_name) : metadata->referenced_key_name;
  info.referenced_key_name= referenced_key_name && referenced_key_name[0] ?
    mylite_make_foreign_key_string(thd, referenced_key_name) : NULL;
  if (referenced_key_name && referenced_key_name[0] &&
      !info.referenced_key_name)
    return NULL;

  if (mylite_append_foreign_key_column_names(
        thd, &info.foreign_fields, metadata->foreign_column_names,
        metadata->column_count) ||
      mylite_append_foreign_key_column_names(
        thd, &info.referenced_fields, metadata->referenced_column_names,
        metadata->column_count))
    return NULL;

  mylite_set_foreign_key_nullable_bits(thd, &info, metadata);

  return static_cast<FOREIGN_KEY_INFO *>(
    thd_memdup(thd, &info, sizeof(FOREIGN_KEY_INFO)));
}

static LEX_CSTRING *mylite_make_foreign_key_string(THD *thd,
                                                   const char *value)
{
  if (!value)
    return NULL;
  return thd_make_lex_string(thd, NULL, value, strlen(value), 1);
}

static int mylite_append_foreign_key_column_names(THD *thd,
                                                  List<LEX_CSTRING> *list,
                                                  char **column_names,
                                                  size_t column_count)
{
  for (size_t i= 0; i < column_count; ++i)
  {
    LEX_CSTRING *name= mylite_make_foreign_key_string(thd, column_names[i]);
    if (!name || list->push_back(name))
      return HA_ERR_OUT_OF_MEM;
  }
  return 0;
}

static void mylite_set_foreign_key_nullable_bits(
  THD *thd, FOREIGN_KEY_INFO *info,
  const mylite_storage_foreign_key_metadata *metadata)
{
  const unsigned column_count= static_cast<unsigned>(metadata->column_count);
  for (unsigned i= 0; i < column_count; ++i)
  {
    const unsigned long long bit= 1ULL << i;
    if (metadata->nullable_column_bitmap & bit)
      info->set_nullable(thd, false, i, column_count);
    if (metadata->referenced_nullable_column_bitmap & bit)
      info->set_nullable(thd, true, i, column_count);
  }
}

static enum_fk_option mylite_foreign_key_option(unsigned action)
{
  switch (action) {
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_CASCADE:
    return FK_OPTION_CASCADE;
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_NULL:
    return FK_OPTION_SET_NULL;
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_NO_ACTION:
    return FK_OPTION_NO_ACTION;
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_DEFAULT:
    return FK_OPTION_SET_DEFAULT;
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_UNSPECIFIED:
  case MYLITE_STORAGE_FOREIGN_KEY_ACTION_RESTRICT:
  default:
    return FK_OPTION_RESTRICT;
  }
}

static bool mylite_table_has_blob_fields(TABLE *table)
{
  for (Field **field= table->field; *field; ++field)
  {
    if ((*field)->flags & BLOB_FLAG)
      return true;
  }

  return false;
}

static int mylite_serialize_blob_row(TABLE *table, const uchar *buf,
                                     uchar **out_payload,
                                     size_t *out_payload_size)
{
  const my_ptrdiff_t row_offset= buf - table->record[0];
  size_t blob_count= 0;
  size_t blob_payloads_size= 0;

  for (Field **field= table->field; *field; ++field)
  {
    if (!((*field)->flags & BLOB_FLAG))
      continue;

    Field_blob *blob_field= static_cast<Field_blob *>(*field);
    const uint32 length= blob_field->is_null(row_offset) ? 0 :
                         blob_field->get_length(row_offset);
    if (length == 0)
      continue;
    if (!blob_field->get_ptr(blob_field->ptr + row_offset))
      return HA_ERR_CRASHED_ON_USAGE;
    if (blob_payloads_size > SIZE_MAX - length)
      return HA_ERR_RECORD_FILE_FULL;
    blob_payloads_size+= length;
    ++blob_count;
  }

  if (blob_count > SIZE_MAX / MYLITE_BLOB_ROW_DESCRIPTOR_SIZE)
    return HA_ERR_RECORD_FILE_FULL;
  const size_t descriptor_bytes= blob_count * MYLITE_BLOB_ROW_DESCRIPTOR_SIZE;
  size_t total_size= MYLITE_BLOB_ROW_HEADER_SIZE;
  if (table->s->reclength > SIZE_MAX - total_size)
    return HA_ERR_RECORD_FILE_FULL;
  total_size+= table->s->reclength;
  if (descriptor_bytes > SIZE_MAX - total_size)
    return HA_ERR_RECORD_FILE_FULL;
  total_size+= descriptor_bytes;
  if (blob_payloads_size > SIZE_MAX - total_size)
    return HA_ERR_RECORD_FILE_FULL;
  total_size+= blob_payloads_size;
  if (total_size > UINT32_MAX)
    return HA_ERR_RECORD_FILE_FULL;

  uchar *payload= static_cast<uchar *>(malloc(total_size));
  if (!payload)
    return HA_ERR_OUT_OF_MEM;

  memcpy(payload + MYLITE_BLOB_ROW_MAGIC_OFFSET, mylite_blob_row_magic,
         sizeof(mylite_blob_row_magic));
  mylite_put_u32(payload + MYLITE_BLOB_ROW_VERSION_OFFSET,
                 MYLITE_BLOB_ROW_VERSION);
  mylite_put_u32(payload + MYLITE_BLOB_ROW_RECORD_SIZE_OFFSET,
                 table->s->reclength);
  mylite_put_u32(payload + MYLITE_BLOB_ROW_BLOB_COUNT_OFFSET, blob_count);
  mylite_put_u32(payload + MYLITE_BLOB_ROW_BLOB_BYTES_OFFSET,
                 blob_payloads_size);

  uchar *record= payload + MYLITE_BLOB_ROW_HEADER_SIZE;
  memcpy(record, buf, table->s->reclength);

  size_t descriptor_offset= MYLITE_BLOB_ROW_HEADER_SIZE + table->s->reclength;
  size_t blob_payload_offset= descriptor_offset + descriptor_bytes;
  for (Field **field= table->field; *field; ++field)
  {
    if (!((*field)->flags & BLOB_FLAG))
      continue;

    Field_blob *blob_field= static_cast<Field_blob *>(*field);
    size_t field_offset= 0;
    int error= mylite_field_record_offset(table, *field, &field_offset);
    if (error)
    {
      free(payload);
      return error;
    }
    if (blob_field->pack_length() > table->s->reclength - field_offset)
    {
      free(payload);
      return HA_ERR_CRASHED_ON_USAGE;
    }

    memset(record + field_offset + blob_field->pack_length_no_ptr(), 0,
           sizeof(uchar *));
    if (blob_field->is_null(row_offset))
      continue;

    const uint32 length= blob_field->get_length(row_offset);
    if (length == 0)
      continue;

    uchar *data= blob_field->get_ptr(blob_field->ptr + row_offset);
    mylite_put_u32(payload + descriptor_offset, (*field)->field_index);
    mylite_put_u32(payload + descriptor_offset + sizeof(uint32_t), length);
    descriptor_offset+= MYLITE_BLOB_ROW_DESCRIPTOR_SIZE;
    memcpy(payload + blob_payload_offset, data, length);
    blob_payload_offset+= length;
  }

  if (descriptor_offset !=
        MYLITE_BLOB_ROW_HEADER_SIZE + table->s->reclength + descriptor_bytes ||
      blob_payload_offset != total_size)
  {
    free(payload);
    return HA_ERR_CRASHED_ON_USAGE;
  }

  *out_payload= payload;
  *out_payload_size= total_size;
  return 0;
}

static int mylite_scan_stored_row(TABLE *table, const uchar *payload,
                                  size_t payload_size,
                                  size_t *out_blob_payload_size)
{
  *out_blob_payload_size= 0;
  if (!mylite_table_has_blob_fields(table))
  {
    return payload_size == table->s->reclength ? 0 :
                                                 HA_ERR_CRASHED_ON_USAGE;
  }
  if (!mylite_is_blob_row_payload(payload, payload_size))
    return HA_ERR_CRASHED_ON_USAGE;

  const unsigned version= mylite_get_u32(payload + MYLITE_BLOB_ROW_VERSION_OFFSET);
  const size_t record_size=
    mylite_get_u32(payload + MYLITE_BLOB_ROW_RECORD_SIZE_OFFSET);
  const size_t blob_count=
    mylite_get_u32(payload + MYLITE_BLOB_ROW_BLOB_COUNT_OFFSET);
  const size_t blob_payloads_size=
    mylite_get_u32(payload + MYLITE_BLOB_ROW_BLOB_BYTES_OFFSET);
  if (version != MYLITE_BLOB_ROW_VERSION || record_size != table->s->reclength ||
      blob_count > table->s->fields ||
      blob_count > SIZE_MAX / MYLITE_BLOB_ROW_DESCRIPTOR_SIZE)
    return HA_ERR_CRASHED_ON_USAGE;

  const size_t descriptor_bytes= blob_count * MYLITE_BLOB_ROW_DESCRIPTOR_SIZE;
  size_t descriptor_offset= MYLITE_BLOB_ROW_HEADER_SIZE + record_size;
  if (record_size > SIZE_MAX - MYLITE_BLOB_ROW_HEADER_SIZE ||
      descriptor_bytes > SIZE_MAX - descriptor_offset)
    return HA_ERR_CRASHED_ON_USAGE;
  const size_t blob_payload_offset= descriptor_offset + descriptor_bytes;
  if (blob_payloads_size > SIZE_MAX - blob_payload_offset ||
      blob_payload_offset + blob_payloads_size != payload_size)
    return HA_ERR_CRASHED_ON_USAGE;

  const uchar *record= payload + MYLITE_BLOB_ROW_HEADER_SIZE;
  size_t blob_payloads_seen= 0;
  for (size_t i= 0; i < blob_count; ++i)
  {
    const size_t descriptor= descriptor_offset +
                             (i * MYLITE_BLOB_ROW_DESCRIPTOR_SIZE);
    const unsigned field_index= mylite_get_u32(payload + descriptor);
    const size_t blob_size=
      mylite_get_u32(payload + descriptor + sizeof(uint32_t));
    if (field_index >= table->s->fields ||
        !((table->field[field_index])->flags & BLOB_FLAG) ||
        blob_size == 0 || blob_size > blob_payloads_size - blob_payloads_seen)
      return HA_ERR_CRASHED_ON_USAGE;
    for (size_t j= 0; j < i; ++j)
    {
      const size_t earlier_descriptor= descriptor_offset +
                                       (j * MYLITE_BLOB_ROW_DESCRIPTOR_SIZE);
      if (mylite_get_u32(payload + earlier_descriptor) == field_index)
        return HA_ERR_CRASHED_ON_USAGE;
    }

    Field_blob *blob_field=
      static_cast<Field_blob *>(table->field[field_index]);
    size_t field_offset= 0;
    int error= mylite_field_record_offset(table, blob_field, &field_offset);
    if (error)
      return error;
    if (blob_field->pack_length() > record_size - field_offset ||
        blob_field->get_length(record + field_offset) != blob_size)
      return HA_ERR_CRASHED_ON_USAGE;

    const size_t pointer_offset= field_offset + blob_field->pack_length_no_ptr();
    for (size_t pointer_byte= 0; pointer_byte < sizeof(uchar *);
         ++pointer_byte)
    {
      if (record[pointer_offset + pointer_byte] != 0)
        return HA_ERR_CRASHED_ON_USAGE;
    }

    const my_ptrdiff_t row_offset= record - table->record[0];
    if (blob_field->is_null(row_offset))
      return HA_ERR_CRASHED_ON_USAGE;
    blob_payloads_seen+= blob_size;
  }

  for (Field **field= table->field; *field; ++field)
  {
    if (!((*field)->flags & BLOB_FLAG))
      continue;

    Field_blob *blob_field= static_cast<Field_blob *>(*field);
    size_t field_offset= 0;
    int error= mylite_field_record_offset(table, *field, &field_offset);
    if (error)
      return error;
    if (blob_field->pack_length() > record_size - field_offset)
      return HA_ERR_CRASHED_ON_USAGE;

    const size_t pointer_offset= field_offset + blob_field->pack_length_no_ptr();
    for (size_t pointer_byte= 0; pointer_byte < sizeof(uchar *);
         ++pointer_byte)
    {
      if (record[pointer_offset + pointer_byte] != 0)
        return HA_ERR_CRASHED_ON_USAGE;
    }

    const uint32 length= blob_field->get_length(record + field_offset);
    bool has_descriptor= false;
    for (size_t i= 0; i < blob_count; ++i)
    {
      const size_t descriptor= descriptor_offset +
                               (i * MYLITE_BLOB_ROW_DESCRIPTOR_SIZE);
      if (mylite_get_u32(payload + descriptor) == (*field)->field_index)
      {
        has_descriptor= true;
        break;
      }
    }
    if ((length == 0 && has_descriptor) || (length != 0 && !has_descriptor))
      return HA_ERR_CRASHED_ON_USAGE;
  }

  *out_blob_payload_size= blob_payloads_size;
  return 0;
}

static int mylite_copy_stored_row_to_scan(TABLE *table, const uchar *payload,
                                          size_t payload_size,
                                          uchar *out_row,
                                          uchar *blob_payloads,
                                          size_t *inout_blob_payloads_used)
{
  if (!mylite_table_has_blob_fields(table))
  {
    if (payload_size != table->s->reclength)
      return HA_ERR_CRASHED_ON_USAGE;
    memcpy(out_row, payload, payload_size);
    return 0;
  }

  size_t row_blob_payloads_size= 0;
  int error= mylite_scan_stored_row(table, payload, payload_size,
                                    &row_blob_payloads_size);
  if (error)
    return error;

  const size_t record_size=
    mylite_get_u32(payload + MYLITE_BLOB_ROW_RECORD_SIZE_OFFSET);
  const size_t blob_count=
    mylite_get_u32(payload + MYLITE_BLOB_ROW_BLOB_COUNT_OFFSET);
  const size_t descriptor_offset= MYLITE_BLOB_ROW_HEADER_SIZE + record_size;
  size_t blob_payload_offset= descriptor_offset +
                              (blob_count * MYLITE_BLOB_ROW_DESCRIPTOR_SIZE);
  memcpy(out_row, payload + MYLITE_BLOB_ROW_HEADER_SIZE, record_size);

  const my_ptrdiff_t row_offset= out_row - table->record[0];
  for (size_t i= 0; i < blob_count; ++i)
  {
    const size_t descriptor= descriptor_offset +
                             (i * MYLITE_BLOB_ROW_DESCRIPTOR_SIZE);
    const unsigned field_index= mylite_get_u32(payload + descriptor);
    const uint32 blob_size=
      mylite_get_u32(payload + descriptor + sizeof(uint32_t));
    if (blob_size > 0 && blob_payloads == NULL)
      return HA_ERR_CRASHED_ON_USAGE;

    uchar *blob_payload= blob_payloads + *inout_blob_payloads_used;
    memcpy(blob_payload, payload + blob_payload_offset, blob_size);

    Field_blob *blob_field=
      static_cast<Field_blob *>(table->field[field_index]);
    blob_field->set_ptr_offset(row_offset, blob_size, blob_payload);

    blob_payload_offset+= blob_size;
    *inout_blob_payloads_used+= blob_size;
  }

  return 0;
}

static int mylite_field_record_offset(TABLE *table, Field *field,
                                      size_t *out_offset)
{
  if (field->ptr < table->record[0])
    return HA_ERR_CRASHED_ON_USAGE;

  const size_t offset= (size_t)(field->ptr - table->record[0]);
  if (offset >= table->s->reclength)
    return HA_ERR_CRASHED_ON_USAGE;

  *out_offset= offset;
  return 0;
}

static bool mylite_is_blob_row_payload(const uchar *payload,
                                       size_t payload_size)
{
  return payload_size >= MYLITE_BLOB_ROW_HEADER_SIZE &&
         memcmp(payload + MYLITE_BLOB_ROW_MAGIC_OFFSET,
                mylite_blob_row_magic, sizeof(mylite_blob_row_magic)) == 0;
}

static unsigned mylite_get_u32(const uchar *ptr)
{
  unsigned value= 0;
  for (size_t i= 0; i < sizeof(uint32_t); ++i)
    value|= (unsigned) ptr[i] << (i * 8U);
  return value;
}

static void mylite_put_u32(uchar *ptr, unsigned value)
{
  for (size_t i= 0; i < sizeof(uint32_t); ++i)
    ptr[i]= (uchar)((value >> (i * 8U)) & UINT8_MAX);
}

static ulonglong mylite_get_u64(const uchar *ptr)
{
  ulonglong value= 0;
  for (size_t i= 0; i < sizeof(uint64_t); ++i)
    value|= (ulonglong) ptr[i] << (i * 8U);
  return value;
}

static void mylite_put_u64(uchar *ptr, ulonglong value)
{
  for (size_t i= 0; i < sizeof(uint64_t); ++i)
    ptr[i]= (uchar)((value >> (i * 8U)) & UINT8_MAX);
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

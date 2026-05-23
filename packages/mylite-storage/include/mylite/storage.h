#ifndef MYLITE_STORAGE_H
#define MYLITE_STORAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_STORAGE_ENGINE_NAME "MYLITE"
#define MYLITE_STORAGE_FORMAT_VERSION 1U
#define MYLITE_STORAGE_MAX_INDEX_KEY_SIZE 4032U
#define MYLITE_STORAGE_CAPABILITY_FILE_HEADER 0x00000001U
#define MYLITE_STORAGE_CAPABILITY_EMPTY_CATALOG 0x00000002U
#define MYLITE_STORAGE_CAPABILITY_TABLE_DEFINITIONS 0x00000004U
#define MYLITE_STORAGE_CAPABILITY_TABLE_ROWS 0x00000008U
#define MYLITE_STORAGE_CAPABILITY_AUTOINCREMENT 0x00000010U
#define MYLITE_STORAGE_CAPABILITY_BLOB_TEXT_ROWS 0x00000020U
#define MYLITE_STORAGE_CAPABILITY_ROW_LIFECYCLE 0x00000040U
#define MYLITE_STORAGE_CAPABILITY_INDEX_ENTRIES 0x00000080U
#define MYLITE_STORAGE_CAPABILITY_RECOVERY_JOURNAL 0x00000100U
#define MYLITE_STORAGE_CAPABILITY_FILE_LOCKS 0x00000200U
#define MYLITE_STORAGE_CAPABILITY_TRUNCATE 0x00000400U
#define MYLITE_STORAGE_CAPABILITY_SCHEMAS 0x00000800U
#define MYLITE_STORAGE_CAPABILITY_STATEMENT_CHECKPOINTS 0x00001000U
#define MYLITE_STORAGE_CAPABILITY_BUSY_TIMEOUT 0x00002000U
#define MYLITE_STORAGE_CAPABILITY_TRANSACTION_JOURNAL 0x00004000U
#define MYLITE_STORAGE_CAPABILITY_FOREIGN_KEY_METADATA 0x00008000U
#define MYLITE_STORAGE_CAPABILITY_INDEX_ROOTS 0x00010000U
#define MYLITE_STORAGE_CAPABILITY_INDEX_LEAF_PAGES 0x00020000U

#define MYLITE_STORAGE_FOREIGN_KEY_ACTION_UNSPECIFIED 0U
#define MYLITE_STORAGE_FOREIGN_KEY_ACTION_RESTRICT 1U
#define MYLITE_STORAGE_FOREIGN_KEY_ACTION_CASCADE 2U
#define MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_NULL 3U
#define MYLITE_STORAGE_FOREIGN_KEY_ACTION_NO_ACTION 4U
#define MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_DEFAULT 5U
#define MYLITE_STORAGE_FOREIGN_KEY_MATCH_UNSPECIFIED 0U
#define MYLITE_STORAGE_FOREIGN_KEY_MATCH_SIMPLE 1U
#define MYLITE_STORAGE_FOREIGN_KEY_MATCH_FULL 2U
#define MYLITE_STORAGE_FOREIGN_KEY_MATCH_PARTIAL 3U

typedef enum mylite_storage_result { /* NOLINT(performance-enum-size): C ABI enum. */
                                     MYLITE_STORAGE_OK = 0,
                                     MYLITE_STORAGE_ERROR = 1,
                                     MYLITE_STORAGE_BUSY = 5,
                                     MYLITE_STORAGE_NOMEM = 7,
                                     MYLITE_STORAGE_READONLY = 8,
                                     MYLITE_STORAGE_IOERR = 10,
                                     MYLITE_STORAGE_CORRUPT = 11,
                                     MYLITE_STORAGE_NOTFOUND = 12,
                                     MYLITE_STORAGE_FULL = 13,
                                     MYLITE_STORAGE_MISUSE = 21,
                                     MYLITE_STORAGE_UNSUPPORTED = 22
} mylite_storage_result;

typedef struct mylite_storage_capabilities {
    size_t size;
    unsigned format_version;
    unsigned flags;
} mylite_storage_capabilities;

typedef struct mylite_storage_header {
    size_t size;
    unsigned format_version;
    unsigned header_version;
    unsigned page_size;
    unsigned checksum_algorithm;
    unsigned flags;
    unsigned long long catalog_root_page;
    unsigned long long catalog_generation;
    unsigned long long free_list_root_page;
    unsigned long long page_count;
} mylite_storage_header;

typedef struct mylite_storage_table_definition {
    size_t size;
    const char *schema_name;
    const char *table_name;
    const char *requested_engine_name;
    const char *effective_engine_name;
    const unsigned char *definition;
    size_t definition_size;
} mylite_storage_table_definition;

typedef struct mylite_storage_table_metadata {
    size_t size;
    char *requested_engine_name;
    char *effective_engine_name;
} mylite_storage_table_metadata;

typedef struct mylite_storage_schema_definition {
    size_t size;
    const char *schema_name;
    const char *default_character_set_name;
    const char *default_collation_name;
    const char *schema_comment;
} mylite_storage_schema_definition;

typedef struct mylite_storage_schema_metadata {
    size_t size;
    char *default_character_set_name;
    char *default_collation_name;
    char *schema_comment;
} mylite_storage_schema_metadata;

typedef struct mylite_storage_index_root_definition {
    size_t size;
    const char *schema_name;
    const char *table_name;
    unsigned index_number;
    unsigned long long root_page;
    unsigned long long entry_count;
} mylite_storage_index_root_definition;

typedef struct mylite_storage_index_root_metadata {
    size_t size;
    unsigned long long root_page;
    unsigned long long entry_count;
} mylite_storage_index_root_metadata;

typedef struct mylite_storage_foreign_key_definition {
    size_t size;
    const char *schema_name;
    const char *table_name;
    const char *constraint_name;
    const char *referenced_schema_name;
    const char *referenced_table_name;
    const char *referenced_key_name;
    const char *const *foreign_column_names;
    const char *const *referenced_column_names;
    size_t column_count;
    unsigned update_action;
    unsigned delete_action;
    unsigned match_option;
    unsigned long long nullable_column_bitmap;
    unsigned long long referenced_nullable_column_bitmap;
} mylite_storage_foreign_key_definition;

typedef struct mylite_storage_foreign_key_metadata {
    size_t size;
    char *schema_name;
    char *table_name;
    char *constraint_name;
    char *referenced_schema_name;
    char *referenced_table_name;
    char *referenced_key_name;
    char **foreign_column_names;
    char **referenced_column_names;
    size_t column_count;
    unsigned update_action;
    unsigned delete_action;
    unsigned match_option;
    unsigned long long nullable_column_bitmap;
    unsigned long long referenced_nullable_column_bitmap;
} mylite_storage_foreign_key_metadata;

typedef struct mylite_storage_rowset {
    size_t size;
    unsigned char *rows;
    size_t row_size;
    size_t row_count;
    size_t *row_offsets;
    size_t *row_sizes;
    size_t row_bytes;
    unsigned long long *row_ids;
} mylite_storage_rowset;

typedef struct mylite_storage_index_entry {
    size_t size;
    unsigned index_number;
    const unsigned char *key;
    size_t key_size;
} mylite_storage_index_entry;

typedef struct mylite_storage_index_entryset {
    size_t size;
    unsigned char *keys;
    size_t key_bytes;
    size_t entry_count;
    size_t *key_offsets;
    size_t *key_sizes;
    unsigned long long *row_ids;
} mylite_storage_index_entryset;

typedef struct mylite_storage_statement mylite_storage_statement;

/* Callers must keep the pointed-to filename bytes stable for active statements. */
typedef struct mylite_storage_filename_identity_scope {
    const char *previous_filename;
    int previous_active;
} mylite_storage_filename_identity_scope;

/* Callers must keep the pointed-to table-name bytes immutable until scope end. */
typedef struct mylite_storage_table_name_identity_scope {
    const char *previous_schema_name;
    const char *previous_table_name;
    int previous_active;
} mylite_storage_table_name_identity_scope;

typedef struct mylite_storage_identity_scope {
    const char *previous_filename;
    const char *previous_schema_name;
    const char *previous_table_name;
    int previous_filename_active;
    int previous_table_name_active;
} mylite_storage_identity_scope;

typedef int (*mylite_storage_table_callback)(
    void *ctx,
    const char *schema_name,
    const char *table_name
);
typedef int (*mylite_storage_schema_callback)(void *ctx, const char *schema_name);
typedef int (*mylite_storage_foreign_key_callback)(
    void *ctx,
    const mylite_storage_foreign_key_metadata *metadata
);

const char *mylite_storage_engine_name(void);
mylite_storage_capabilities mylite_storage_get_capabilities(void);
mylite_storage_result mylite_storage_create_empty(const char *filename);
mylite_storage_result mylite_storage_open_header(
    const char *filename,
    mylite_storage_header *out_header
);
mylite_storage_result mylite_storage_store_table_definition(
    const char *filename,
    const mylite_storage_table_definition *definition
);
mylite_storage_result mylite_storage_store_schema(const char *filename, const char *schema_name);
mylite_storage_result mylite_storage_store_schema_definition(
    const char *filename,
    const mylite_storage_schema_definition *definition
);
mylite_storage_result mylite_storage_drop_schema(const char *filename, const char *schema_name);
mylite_storage_result mylite_storage_schema_exists(const char *filename, const char *schema_name);
mylite_storage_result mylite_storage_read_schema_definition(
    const char *filename,
    const char *schema_name,
    mylite_storage_schema_metadata *out_metadata
);
mylite_storage_result mylite_storage_store_foreign_key_definition(
    const char *filename,
    const mylite_storage_foreign_key_definition *definition
);
mylite_storage_result mylite_storage_read_foreign_key_definition(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name,
    mylite_storage_foreign_key_metadata *out_metadata
);
mylite_storage_result mylite_storage_update_foreign_key_referenced_key_name(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name,
    const char *referenced_key_name
);
mylite_storage_result mylite_storage_drop_foreign_key_definition(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name
);
mylite_storage_result mylite_storage_list_foreign_keys(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    mylite_storage_foreign_key_callback callback,
    void *ctx
);
mylite_storage_result mylite_storage_list_parent_foreign_keys(
    const char *filename,
    const char *referenced_schema_name,
    const char *referenced_table_name,
    mylite_storage_foreign_key_callback callback,
    void *ctx
);
mylite_storage_result mylite_storage_read_table_definition(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned char **out_definition,
    size_t *out_definition_size
);
mylite_storage_result mylite_storage_read_table_metadata(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    mylite_storage_table_metadata *out_metadata
);
mylite_storage_result mylite_storage_table_exists(
    const char *filename,
    const char *schema_name,
    const char *table_name
);
mylite_storage_result mylite_storage_store_index_root(
    const char *filename,
    const mylite_storage_index_root_definition *definition
);
mylite_storage_result mylite_storage_read_index_root(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    mylite_storage_index_root_metadata *out_metadata
);
mylite_storage_result mylite_storage_drop_index_root(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number
);
mylite_storage_result mylite_storage_rebuild_index_leaf(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number
);
mylite_storage_result mylite_storage_rebuild_index_leaves(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned *index_numbers,
    size_t index_number_count
);
mylite_storage_result mylite_storage_drop_table(
    const char *filename,
    const char *schema_name,
    const char *table_name
);
mylite_storage_result mylite_storage_rename_table(
    const char *filename,
    const char *old_schema_name,
    const char *old_table_name,
    const char *new_schema_name,
    const char *new_table_name
);
mylite_storage_result mylite_storage_rename_table_for_rebuild_backup(
    const char *filename,
    const char *old_schema_name,
    const char *old_table_name,
    const char *new_schema_name,
    const char *new_table_name
);
mylite_storage_result mylite_storage_append_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned char *row,
    size_t row_size
);
mylite_storage_result mylite_storage_append_row_with_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    unsigned long long *out_row_id
);
mylite_storage_result mylite_storage_read_rows(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    mylite_storage_rowset *out_rows
);
mylite_storage_result mylite_storage_count_rows(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_row_count
);
mylite_storage_result mylite_storage_read_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    unsigned char **out_row,
    size_t *out_row_size
);
mylite_storage_result mylite_storage_read_indexed_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    unsigned char **out_row,
    size_t *out_row_size
);
mylite_storage_result mylite_storage_read_indexed_rows(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned long long *row_ids,
    size_t row_id_count,
    mylite_storage_rowset *out_rows
);
mylite_storage_result mylite_storage_update_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    unsigned long long *out_new_row_id
);
mylite_storage_result mylite_storage_update_row_preserving_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    unsigned long long *out_new_row_id
);
mylite_storage_result mylite_storage_update_row_preserving_index_entries_in_statement(
    mylite_storage_statement *statement,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    unsigned long long *out_new_row_id
);
mylite_storage_result mylite_storage_update_row_with_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    unsigned long long *out_new_row_id
);
mylite_storage_result mylite_storage_update_row_with_index_entry_changes(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed,
    unsigned long long *out_new_row_id
);
mylite_storage_result mylite_storage_update_row_with_index_entry_changes_in_statement(
    mylite_storage_statement *statement,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed,
    unsigned long long *out_new_row_id
);
mylite_storage_result mylite_storage_delete_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id
);
mylite_storage_result mylite_storage_truncate_table(
    const char *filename,
    const char *schema_name,
    const char *table_name
);
mylite_storage_result mylite_storage_read_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    mylite_storage_index_entryset *out_entries
);
mylite_storage_result mylite_storage_read_index_prefix_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    mylite_storage_index_entryset *out_entries
);
mylite_storage_result mylite_storage_find_index_entry(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id
);
mylite_storage_result mylite_storage_find_indexed_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    unsigned char **out_row,
    size_t *out_row_size
);
mylite_storage_result mylite_storage_find_indexed_row_reuse(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    unsigned char **inout_row,
    size_t *inout_row_capacity,
    size_t *out_row_size
);
mylite_storage_result mylite_storage_find_indexed_row_into(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    unsigned char *out_row,
    size_t out_row_capacity,
    size_t *out_row_size
);
mylite_storage_result mylite_storage_find_indexed_row_in_statement_into(
    mylite_storage_statement *statement,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    unsigned char *out_row,
    size_t out_row_capacity,
    size_t *out_row_size
);
mylite_storage_result mylite_storage_read_exact_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries
);
mylite_storage_result mylite_storage_index_prefix_exists(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    int *out_exists
);
mylite_storage_result mylite_storage_index_prefix_exists_for_index(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    unsigned long long skip_row_id,
    int *out_exists
);
mylite_storage_result mylite_storage_read_auto_increment(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_next_value
);
mylite_storage_result mylite_storage_set_auto_increment(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long next_value
);
mylite_storage_result mylite_storage_advance_auto_increment(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long next_value
);
mylite_storage_result mylite_storage_list_tables(
    const char *filename,
    const char *schema_name,
    mylite_storage_table_callback callback,
    void *ctx
);
mylite_storage_result mylite_storage_list_schemas(
    const char *filename,
    mylite_storage_schema_callback callback,
    void *ctx
);
mylite_storage_result mylite_storage_begin_statement(
    const char *filename,
    mylite_storage_statement **out_statement
);
mylite_storage_result mylite_storage_begin_nested_statement(
    mylite_storage_statement *parent,
    mylite_storage_statement **out_statement
);
mylite_storage_result mylite_storage_begin_transaction(
    const char *filename,
    mylite_storage_statement **out_statement
);
mylite_storage_result mylite_storage_begin_read_statement(
    const char *filename,
    mylite_storage_statement **out_statement
);
int mylite_storage_statement_active(const char *filename);
int mylite_storage_context_has_active_statement(void);
int mylite_storage_context_has_active_read_statement(const char *filename);
mylite_storage_statement *mylite_storage_borrow_active_statement(const char *filename);
int mylite_storage_statement_matches(mylite_storage_statement *statement, const char *filename);
mylite_storage_result mylite_storage_preserve_auto_increment_on_rollback(
    const char *filename
);
const void *mylite_storage_context_owner(void);
void mylite_storage_set_context_owner(const void *owner);
void mylite_storage_clear_thread_caches(void);
void mylite_storage_begin_filename_identity_scope(
    const char *filename,
    mylite_storage_filename_identity_scope *scope
);
void mylite_storage_end_filename_identity_scope(
    const mylite_storage_filename_identity_scope *scope
);
void mylite_storage_begin_table_name_identity_scope(
    const char *schema_name,
    const char *table_name,
    mylite_storage_table_name_identity_scope *scope
);
void mylite_storage_end_table_name_identity_scope(
    const mylite_storage_table_name_identity_scope *scope
);
void mylite_storage_begin_identity_scope(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    mylite_storage_identity_scope *scope
);
void mylite_storage_end_identity_scope(const mylite_storage_identity_scope *scope);
void mylite_storage_set_busy_timeout(unsigned milliseconds);
unsigned mylite_storage_busy_timeout(void);
mylite_storage_result mylite_storage_commit_statement(mylite_storage_statement *statement);
mylite_storage_result mylite_storage_rollback_statement(mylite_storage_statement *statement);
mylite_storage_result mylite_storage_end_read_statement(mylite_storage_statement *statement);
void mylite_storage_free(void *ptr);
void mylite_storage_free_rowset(mylite_storage_rowset *rowset);
void mylite_storage_free_index_entryset(mylite_storage_index_entryset *entryset);
void mylite_storage_free_foreign_key_metadata(mylite_storage_foreign_key_metadata *metadata);

#ifdef __cplusplus
}
#endif

#endif

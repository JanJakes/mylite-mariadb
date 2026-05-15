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

typedef int (*mylite_storage_table_callback)(
    void *ctx,
    const char *schema_name,
    const char *table_name
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
mylite_storage_result mylite_storage_update_row(
    const char *filename,
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
mylite_storage_result mylite_storage_read_auto_increment(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_next_value
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
void mylite_storage_free(void *ptr);
void mylite_storage_free_rowset(mylite_storage_rowset *rowset);
void mylite_storage_free_index_entryset(mylite_storage_index_entryset *entryset);

#ifdef __cplusplus
}
#endif

#endif

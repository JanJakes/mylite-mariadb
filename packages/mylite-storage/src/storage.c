#include "storage_format.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct mylite_storage_catalog_entry {
    const unsigned char *record;
    unsigned long long table_id;
    unsigned long long definition_root_page;
    unsigned long long definition_size;
} mylite_storage_catalog_entry;

typedef struct mylite_storage_row_page {
    unsigned long long row_id;
    unsigned long long table_id;
    size_t row_size;
    size_t row_count;
    const unsigned char *payload;
    unsigned char *owned_payload;
} mylite_storage_row_page;

typedef struct mylite_storage_row_page_metadata {
    unsigned long long row_id;
    unsigned long long table_id;
    unsigned long long overflow_root_page;
    size_t row_size;
    size_t row_count;
} mylite_storage_row_page_metadata;

typedef struct mylite_storage_autoincrement_page {
    unsigned long long table_id;
    unsigned long long next_value;
} mylite_storage_autoincrement_page;

typedef struct mylite_storage_row_state_page {
    unsigned long long table_id;
    unsigned long long source_row_id;
    unsigned long long replacement_row_id;
    unsigned state_kind;
} mylite_storage_row_state_page;

typedef struct mylite_storage_row_state_entry {
    unsigned long long source_row_id;
    unsigned long long replacement_row_id;
    unsigned state_kind;
} mylite_storage_row_state_entry;

typedef struct mylite_storage_row_state_map_bucket {
    unsigned long long row_id;
    size_t entry_index;
    int occupied;
} mylite_storage_row_state_map_bucket;

typedef struct mylite_storage_row_state_map {
    mylite_storage_row_state_entry *entries;
    size_t count;
    size_t capacity;
    mylite_storage_row_state_map_bucket *buckets;
    size_t bucket_capacity;
} mylite_storage_row_state_map;

typedef struct mylite_storage_live_row_cache {
    unsigned long long table_id;
    unsigned long long catalog_root_page;
    unsigned long long catalog_generation;
    unsigned long long *row_ids;
    size_t count;
    size_t capacity;
    unsigned long long *validated_row_ids;
    size_t validated_count;
    size_t validated_capacity;
} mylite_storage_live_row_cache;

typedef struct mylite_storage_live_row_cache_set {
    mylite_storage_live_row_cache *entries;
    size_t count;
    size_t capacity;
} mylite_storage_live_row_cache_set;

typedef struct mylite_storage_row_id_list {
    unsigned long long *row_ids;
    size_t count;
} mylite_storage_row_id_list;

typedef struct mylite_storage_index_prefix_match {
    unsigned long long row_id;
    unsigned index_number;
} mylite_storage_index_prefix_match;

typedef struct mylite_storage_index_prefix_match_list {
    mylite_storage_index_prefix_match *matches;
    size_t count;
} mylite_storage_index_prefix_match_list;

typedef struct mylite_storage_live_row_id_cache {
    char *filename;
    unsigned long long catalog_root_page;
    unsigned long long catalog_generation;
    unsigned long long page_count;
    unsigned long long table_id;
    unsigned long long *row_ids;
    size_t count;
} mylite_storage_live_row_id_cache;

typedef struct mylite_storage_live_row_id_cache_set {
    mylite_storage_live_row_id_cache *entries;
    size_t count;
    size_t capacity;
} mylite_storage_live_row_id_cache_set;

typedef struct mylite_storage_exact_index_cache {
    char *filename;
    unsigned long long catalog_root_page;
    unsigned long long catalog_generation;
    unsigned long long page_count;
    unsigned long long table_id;
    unsigned index_number;
    size_t key_size;
    unsigned char *keys;
    unsigned long long *row_ids;
    unsigned char *entry_live;
    size_t count;
    size_t capacity;
    size_t live_count;
    size_t dead_count;
    size_t *bucket_heads;
    size_t *bucket_next;
    size_t bucket_count;
    size_t bucket_next_capacity;
    int buckets_valid;
    size_t *row_id_bucket_heads;
    size_t *row_id_bucket_next;
    size_t row_id_bucket_count;
    size_t row_id_bucket_next_capacity;
    int row_id_buckets_valid;
} mylite_storage_exact_index_cache;

typedef struct mylite_storage_exact_index_cache_set {
    mylite_storage_exact_index_cache *entries;
    size_t count;
    size_t capacity;
} mylite_storage_exact_index_cache_set;

typedef struct mylite_storage_row_payload_cache_entry {
    unsigned long long row_id;
    unsigned char *row;
    size_t row_size;
} mylite_storage_row_payload_cache_entry;

typedef struct mylite_storage_row_payload_cache_bucket {
    unsigned long long row_id;
    size_t entry_index;
    int state;
} mylite_storage_row_payload_cache_bucket;

typedef struct mylite_storage_row_payload_cache {
    char *filename;
    unsigned long long catalog_root_page;
    unsigned long long catalog_generation;
    unsigned long long page_count;
    unsigned long long table_id;
    mylite_storage_row_payload_cache_entry *entries;
    mylite_storage_row_payload_cache_bucket *buckets;
    size_t count;
    size_t capacity;
    size_t bucket_capacity;
    size_t tombstone_count;
} mylite_storage_row_payload_cache;

typedef struct mylite_storage_row_payload_cache_set {
    mylite_storage_row_payload_cache *entries;
    size_t count;
    size_t capacity;
} mylite_storage_row_payload_cache_set;

typedef struct mylite_storage_index_leaf_page_cache_entry {
    unsigned long long page_id;
    unsigned long long table_id;
    unsigned index_number;
    size_t key_size;
    size_t entry_count;
    size_t used_bytes;
    unsigned char *page;
} mylite_storage_index_leaf_page_cache_entry;

typedef struct mylite_storage_index_leaf_page_cache {
    char *filename;
    unsigned long long catalog_root_page;
    unsigned long long catalog_generation;
    unsigned long long page_count;
    mylite_storage_index_leaf_page_cache_entry *entries;
    size_t count;
    size_t capacity;
} mylite_storage_index_leaf_page_cache;

typedef struct mylite_storage_index_leaf_page_cache_set {
    mylite_storage_index_leaf_page_cache *entries;
    size_t count;
    size_t capacity;
} mylite_storage_index_leaf_page_cache_set;

typedef struct mylite_storage_index_entry_page {
    unsigned long long table_id;
    unsigned long long row_id;
    unsigned index_number;
    size_t key_size;
    const unsigned char *key;
} mylite_storage_index_entry_page;

typedef struct mylite_storage_index_leaf_page {
    unsigned long long table_id;
    unsigned index_number;
    size_t key_size;
    size_t entry_count;
    size_t used_bytes;
    const unsigned char *payload;
} mylite_storage_index_leaf_page;

typedef struct mylite_storage_index_leaf_run {
    unsigned long long first_page_id;
    unsigned long long page_count;
    unsigned long long tail_page_id;
    unsigned long long entry_count;
    size_t key_size;
    size_t entry_capacity;
} mylite_storage_index_leaf_run;

typedef struct mylite_storage_blob_write {
    const unsigned char *payload;
    size_t payload_size;
    unsigned page_type;
} mylite_storage_blob_write;

typedef struct mylite_storage_blob_chain {
    unsigned long long first_page_id;
    size_t payload_size;
    unsigned page_type;
} mylite_storage_blob_chain;

typedef struct mylite_storage_row_write {
    const unsigned char *row;
    size_t row_size;
    unsigned long long overflow_root_page;
} mylite_storage_row_write;

typedef struct mylite_storage_row_write_position {
    unsigned long long row_page_id;
    unsigned long long next_page_id;
} mylite_storage_row_write_position;

typedef struct mylite_storage_index_entry_write {
    unsigned long long first_page_id;
    unsigned long long table_id;
    unsigned long long row_id;
    const mylite_storage_index_entry *index_entries;
    size_t index_entry_count;
    const unsigned char *index_entry_changed;
} mylite_storage_index_entry_write;

typedef struct mylite_storage_definition_lengths {
    size_t schema_name_size;
    size_t table_name_size;
    size_t requested_engine_name_size;
    size_t effective_engine_name_size;
} mylite_storage_definition_lengths;

typedef struct mylite_storage_schema_definition_lengths {
    size_t schema_name_size;
    size_t default_character_set_name_size;
    size_t default_collation_name_size;
    size_t schema_comment_size;
} mylite_storage_schema_definition_lengths;

typedef struct mylite_storage_foreign_key_definition_lengths {
    size_t schema_name_size;
    size_t table_name_size;
    size_t constraint_name_size;
    size_t referenced_schema_name_size;
    size_t referenced_table_name_size;
    size_t referenced_key_name_size;
    size_t foreign_column_names_size;
    size_t referenced_column_names_size;
} mylite_storage_foreign_key_definition_lengths;

typedef struct mylite_storage_foreign_key_record_fields {
    const char *schema_name;
    size_t schema_name_size;
    const char *table_name;
    size_t table_name_size;
    const char *constraint_name;
    size_t constraint_name_size;
    const char *referenced_schema_name;
    size_t referenced_schema_name_size;
    const char *referenced_table_name;
    size_t referenced_table_name_size;
    unsigned long long metadata_root_page;
    unsigned long long metadata_size;
    unsigned long long table_id;
} mylite_storage_foreign_key_record_fields;

typedef struct mylite_storage_index_root_definition_lengths {
    size_t schema_name_size;
    size_t table_name_size;
} mylite_storage_index_root_definition_lengths;

typedef struct mylite_storage_index_root_record_fields {
    const char *schema_name;
    size_t schema_name_size;
    const char *table_name;
    size_t table_name_size;
    unsigned index_number;
    unsigned long long table_id;
    unsigned long long root_page;
    unsigned long long entry_count;
} mylite_storage_index_root_record_fields;

typedef struct mylite_storage_schema_list {
    char **names;
    size_t count;
} mylite_storage_schema_list;

typedef struct mylite_storage_autoincrement_rollback_value {
    unsigned long long table_id;
    unsigned long long next_value;
} mylite_storage_autoincrement_rollback_value;

typedef struct mylite_storage_autoincrement_rollback_values {
    mylite_storage_autoincrement_rollback_value *entries;
    size_t count;
    size_t capacity;
} mylite_storage_autoincrement_rollback_values;

typedef struct mylite_storage_table_identity {
    const char *schema_name;
    const char *table_name;
} mylite_storage_table_identity;

typedef struct mylite_storage_table_entry_cache {
    char *schema_name;
    char *table_name;
    unsigned long long catalog_root_page;
    unsigned long long catalog_generation;
    mylite_storage_catalog_entry entry;
    int has_entry;
} mylite_storage_table_entry_cache;

typedef struct mylite_storage_rowset_builder {
    mylite_storage_rowset *rowset;
    size_t row_capacity;
    size_t metadata_capacity;
} mylite_storage_rowset_builder;

typedef struct mylite_storage_recovery_journal {
    unsigned long long page_ids[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES];
    size_t page_count;
} mylite_storage_recovery_journal;

typedef struct mylite_storage_append_page_buffer {
    unsigned long long first_page_id;
    size_t page_count;
    size_t capacity_pages;
    unsigned char *pages;
    unsigned char *checksum_dirty;
} mylite_storage_append_page_buffer;

typedef struct mylite_storage_buffered_page_undo {
    unsigned long long page_id;
    size_t used_size;
    int checksum_dirty;
    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
} mylite_storage_buffered_page_undo;

typedef struct mylite_storage_buffered_page_undo_list {
    mylite_storage_buffered_page_undo *entries;
    size_t count;
    size_t capacity;
} mylite_storage_buffered_page_undo_list;

enum { MYLITE_STORAGE_BUFFERED_UPDATE_REWRITE_INLINE_INDEXES = 4U };

typedef struct mylite_storage_buffered_update_rewrite_bucket {
    unsigned long long row_id;
    unsigned long long table_id;
    size_t changed_index_count;
    unsigned index_numbers[MYLITE_STORAGE_BUFFERED_UPDATE_REWRITE_INLINE_INDEXES];
    size_t key_sizes[MYLITE_STORAGE_BUFFERED_UPDATE_REWRITE_INLINE_INDEXES];
    int has_shape;
    int occupied;
} mylite_storage_buffered_update_rewrite_bucket;

typedef struct mylite_storage_buffered_update_rewrite_cache {
    mylite_storage_buffered_update_rewrite_bucket *buckets;
    size_t count;
    size_t bucket_capacity;
} mylite_storage_buffered_update_rewrite_cache;

struct mylite_storage_statement {
    FILE *file;
    char *filename;
    dev_t device;
    ino_t inode;
    struct mylite_storage_statement *parent;
    const void *owner;
    mylite_storage_header header;
    mylite_storage_header current_header;
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char current_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned long long current_catalog_root_page;
    unsigned long long current_catalog_generation;
    int owns_file;
    int has_current_header;
    int current_header_dirty;
    int has_current_catalog_page;
    int owns_recovery_journal;
    int owns_transaction_journal;
    int preserve_auto_increment_rollback;
    int cache_file_on_close;
    int has_identity;
    mylite_storage_exact_index_cache_set exact_index_caches;
    mylite_storage_live_row_cache_set live_row_caches;
    mylite_storage_live_row_id_cache_set live_row_id_caches;
    mylite_storage_row_payload_cache_set row_payload_caches;
    mylite_storage_table_entry_cache table_entry_cache;
    mylite_storage_append_page_buffer append_pages;
    mylite_storage_buffered_page_undo_list buffered_page_undos;
    mylite_storage_buffered_update_rewrite_cache buffered_update_rewrites;
};

typedef struct mylite_storage_transaction_journal_snapshot {
    FILE *file;
    mylite_storage_header header;
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
} mylite_storage_transaction_journal_snapshot;

typedef struct mylite_storage_read_checkpoint_cache {
    char *filename;
    const void *owner;
    dev_t device;
    ino_t inode;
    mylite_storage_header header;
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char current_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned long long current_catalog_root_page;
    unsigned long long current_catalog_generation;
    int has_snapshot;
    int has_current_catalog_page;
    int has_identity;
} mylite_storage_read_checkpoint_cache;

typedef struct mylite_storage_read_file_cache {
    FILE *file;
    char *filename;
    dev_t device;
    ino_t inode;
    int has_identity;
} mylite_storage_read_file_cache;

typedef struct mylite_storage_journal_path_cache {
    char *filename;
    char *recovery_journal_filename;
    char *transaction_journal_filename;
} mylite_storage_journal_path_cache;

typedef mylite_storage_result (*mylite_storage_row_page_callback)(
    void *ctx,
    const mylite_storage_row_page *row_page
);

static _Thread_local mylite_storage_statement *active_statement;
static _Thread_local const void *active_context_owner;
static _Thread_local mylite_storage_statement *active_read_statement;
static _Thread_local mylite_storage_statement *active_read_snapshot;
static _Thread_local mylite_storage_transaction_journal_snapshot active_transaction_journal_snapshot;
static _Thread_local mylite_storage_read_checkpoint_cache active_read_checkpoint_cache;
static _Thread_local mylite_storage_read_file_cache active_read_file_cache;
static _Thread_local mylite_storage_journal_path_cache active_journal_path_cache;
static _Thread_local mylite_storage_live_row_id_cache_set durable_live_row_id_caches;
static _Thread_local mylite_storage_exact_index_cache_set durable_exact_index_caches;
static _Thread_local mylite_storage_row_payload_cache_set durable_row_payload_caches;
static _Thread_local unsigned long long durable_row_payload_caches_generation;
static _Thread_local mylite_storage_index_leaf_page_cache_set durable_index_leaf_page_caches;

#define MYLITE_STORAGE_INDEX_ROOT_CATALOG_RESERVE_BYTES 1024U
#define MYLITE_STORAGE_ACTIVE_ROW_PAYLOAD_CACHE_LIMIT 16U
#define MYLITE_STORAGE_ACTIVE_ROW_PAYLOAD_ENTRY_LIMIT 4096U
#define MYLITE_STORAGE_DURABLE_LIVE_ROW_ID_CACHE_LIMIT 16U
#define MYLITE_STORAGE_DURABLE_LIVE_ROW_ID_ENTRY_LIMIT 4096U
#define MYLITE_STORAGE_DURABLE_EXACT_INDEX_CACHE_LIMIT 16U
#define MYLITE_STORAGE_DURABLE_ROW_PAYLOAD_CACHE_LIMIT 16U
#define MYLITE_STORAGE_DURABLE_ROW_PAYLOAD_ENTRY_LIMIT 4096U
#define MYLITE_STORAGE_DURABLE_INDEX_LEAF_PAGE_CACHE_LIMIT 16U
#define MYLITE_STORAGE_DURABLE_INDEX_LEAF_PAGE_ENTRY_LIMIT 256U
#define MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES 4096U
#define MYLITE_STORAGE_CACHE_BUCKET_EMPTY SIZE_MAX
#define MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_EMPTY 0
#define MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_OCCUPIED 1
#define MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_DELETED 2

static mylite_storage_result path_exists(const char *filename, int *exists);
static mylite_storage_result write_empty_database(FILE *file);
static void initialize_header_page(unsigned char *page);
static void encode_header_page(unsigned char *page, const mylite_storage_header *header);
static void initialize_empty_catalog_page(unsigned char *page);
static void update_catalog_checksum(unsigned char *page);
static char *recovery_journal_path(const char *filename);
static char *transaction_journal_path(const char *filename);
static char *journal_path_with_suffix(const char *filename, const char *suffix, size_t suffix_size);
static mylite_storage_result cached_journal_paths(
    const char *filename,
    const char **out_recovery_journal_filename,
    const char **out_transaction_journal_filename
);
static void clear_journal_path_cache(const char *filename);
static mylite_storage_result recover_pending_journals(const char *filename);
static mylite_storage_result recover_pending_journals_locked(FILE *file, const char *filename);
static mylite_storage_result recover_pending_journal_locked(
    FILE *file,
    const char *journal_filename
);
static mylite_storage_result read_recovery_journal(
    FILE *journal_file,
    mylite_storage_recovery_journal *out_journal,
    unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                       [MYLITE_STORAGE_FORMAT_PAGE_SIZE],
    mylite_storage_header *out_header
);
static mylite_storage_result restore_recovery_journal(
    FILE *file,
    const mylite_storage_recovery_journal *journal,
    const unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE],
    const mylite_storage_header *saved_header
);
static mylite_storage_result begin_recovery_journal(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    int include_catalog
);
static mylite_storage_result begin_write_journal(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    int include_catalog
);
static mylite_storage_result begin_transaction_journal(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header
);
static mylite_storage_result begin_journal_at_path(
    FILE *file,
    char *journal_filename,
    const mylite_storage_header *header,
    int include_catalog
);
static mylite_storage_result finish_recovery_journal(FILE *file, const char *filename);
static mylite_storage_result finish_write_journal(FILE *file, const char *filename);
static mylite_storage_result finish_transaction_journal(FILE *file, const char *filename);
static mylite_storage_result finish_journal_at_path(FILE *file, char *journal_filename);
static int statement_chain_has_write_journal(const mylite_storage_statement *statement);
static void encode_recovery_journal_header(
    unsigned char *page,
    const mylite_storage_recovery_journal *journal
);
static mylite_storage_result decode_recovery_journal_header(
    const unsigned char *page,
    mylite_storage_recovery_journal *out_journal
);
static mylite_storage_result validate_recovery_journal_pages(
    const mylite_storage_recovery_journal *journal,
    const unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE],
    mylite_storage_header *out_header
);
static mylite_storage_result write_page(FILE *file, const unsigned char *page, size_t size);
static mylite_storage_result read_file_at(
    FILE *file,
    off_t offset,
    unsigned char *out,
    size_t size
);
static mylite_storage_result write_file_at(
    FILE *file,
    off_t offset,
    const unsigned char *data,
    size_t size
);
static mylite_storage_result page_offset_for_io(
    unsigned long long page_id,
    unsigned page_size,
    off_t *out_offset
);
static mylite_storage_result lock_file(FILE *file, int operation);
static int is_lock_conflict(int error_number);
static mylite_storage_result flush_file(FILE *file);
static mylite_storage_result truncate_file_to_header_page_count(
    FILE *file,
    const mylite_storage_header *header
);
static mylite_storage_result io_error_result_from_errno(int error_number);
static int is_storage_full_error(int error_number);
static mylite_storage_result flush_parent_directory(const char *filename);
static mylite_storage_result close_created_file(FILE *file, const char *filename);
static mylite_storage_result open_existing_file(const char *filename, FILE **out_file);
static mylite_storage_result open_transaction_journal_snapshot(
    const char *filename,
    FILE **out_file
);
static mylite_storage_result read_transaction_journal_snapshot(
    const char *filename,
    mylite_storage_transaction_journal_snapshot *out_snapshot
);
static mylite_storage_result open_existing_file_for_update(const char *filename, FILE **out_file);
static mylite_storage_result close_existing_file(FILE *file);
static mylite_storage_statement *active_statement_for(const char *filename);
static mylite_storage_statement *active_statement_for_any_owner(const char *filename);
static mylite_storage_statement *active_read_statement_for(const char *filename);
static mylite_storage_statement *active_read_statement_for_any_owner(const char *filename);
static mylite_storage_statement *active_read_snapshot_for(const char *filename);
static mylite_storage_statement *active_table_entry_cache_statement_for(const char *filename);
static mylite_storage_statement *active_exact_index_cache_statement_for(const char *filename);
static mylite_storage_statement *active_live_row_id_cache_statement_for(const char *filename);
static mylite_storage_statement *active_row_payload_cache_statement_for(const char *filename);
static mylite_storage_statement *active_statement_for_file(FILE *file);
static mylite_storage_statement *active_read_statement_for_file(FILE *file);
static mylite_storage_statement *append_page_buffer_statement_for_file(FILE *file);
static int active_statement_has_file(FILE *file);
static int active_read_statement_has_file(FILE *file);
static int active_read_snapshot_has_file(FILE *file);
static int active_transaction_journal_snapshot_has_file(FILE *file);
static mylite_storage_result begin_checkpoint(
    const char *filename,
    mylite_storage_statement **out_statement,
    int durable_transaction
);
static mylite_storage_result initialize_checkpoint_statement(
    mylite_storage_statement *statement,
    const char *filename,
    mylite_storage_statement *parent
);
static void clone_parent_checkpoint_snapshot(
    mylite_storage_statement *statement,
    const mylite_storage_statement *parent
);
static mylite_storage_result initialize_read_statement(
    mylite_storage_statement *statement,
    const char *filename,
    mylite_storage_statement *same_file_parent
);
static mylite_storage_result read_checkpoint_snapshot(mylite_storage_statement *statement);
static mylite_storage_result read_cached_checkpoint_snapshot(mylite_storage_statement *statement);
static mylite_storage_result read_checkpoint_snapshot_from_header_page(
    mylite_storage_statement *statement,
    const unsigned char *header_page
);
static void copy_read_checkpoint_cache_to_statement(
    const mylite_storage_read_checkpoint_cache *cache,
    mylite_storage_statement *statement
);
static mylite_storage_read_checkpoint_cache *read_checkpoint_cache_for(
    const mylite_storage_statement *statement
);
static void store_read_checkpoint_cache(const mylite_storage_statement *statement);
static void clear_read_checkpoint_cache(void);
static mylite_storage_result close_statement(mylite_storage_statement *statement);
static mylite_storage_result flush_statement_append_page_buffer(
    mylite_storage_statement *statement
);
static mylite_storage_result refresh_statement_append_page_buffer_checksums(
    mylite_storage_statement *statement
);
static mylite_storage_result flush_statement_append_page_buffer_before_truncate(
    mylite_storage_statement *statement,
    unsigned long long page_count
);
static void trim_statement_append_page_buffer(
    mylite_storage_statement *statement,
    unsigned long long page_count
);
static mylite_storage_result restore_buffered_page_undos(mylite_storage_statement *statement);
static void clear_append_page_buffer(mylite_storage_statement *statement);
static void clear_buffered_page_undos(mylite_storage_statement *statement);
static void clear_buffered_update_rewrites(mylite_storage_statement *statement);
static void clear_statement_chain_buffered_update_rewrites(mylite_storage_statement *statement);
static int take_cached_read_file(
    const char *filename,
    FILE **out_file,
    dev_t *out_device,
    ino_t *out_inode
);
static mylite_storage_result cache_read_file(
    const char *filename,
    FILE *file,
    dev_t device,
    ino_t inode,
    int has_identity
);
static void clear_cached_read_file(const char *filename);
static int cached_read_file_matches_path(const char *filename);
static mylite_storage_result write_statement_current_header(mylite_storage_statement *statement);
static void clear_statement_chain_catalog_root_caches(mylite_storage_statement *statement);
static void clear_catalog_root_cache(mylite_storage_statement *statement);
static void free_statement(mylite_storage_statement *statement);
static int find_active_table_entry_cache(
    const char *filename,
    const mylite_storage_header *header,
    const char *schema_name,
    const char *table_name,
    mylite_storage_catalog_entry *out_entry
);
static void store_active_table_entry_cache(
    const char *filename,
    const mylite_storage_header *header,
    const char *schema_name,
    const char *table_name,
    const mylite_storage_catalog_entry *entry
);
static void clear_table_entry_cache(mylite_storage_table_entry_cache *cache);
static void clear_statement_chain_exact_index_caches(mylite_storage_statement *statement);
static void clear_exact_index_caches(mylite_storage_statement *statement);
static void free_exact_index_cache(mylite_storage_exact_index_cache *cache);
static void clear_statement_chain_live_row_caches(mylite_storage_statement *statement);
static void clear_live_row_caches(mylite_storage_statement *statement);
static void free_live_row_cache(mylite_storage_live_row_cache *cache);
static void clear_statement_chain_live_row_id_caches(mylite_storage_statement *statement);
static void clear_live_row_id_caches(mylite_storage_statement *statement);
static void clear_statement_chain_row_payload_caches(mylite_storage_statement *statement);
static void clear_row_payload_caches(mylite_storage_statement *statement);
static int checkpoint_preserves_auto_increment_rollback(
    const mylite_storage_statement *statement
);
static mylite_storage_result collect_rollback_auto_increment_values(
    const mylite_storage_statement *statement,
    mylite_storage_autoincrement_rollback_values *out_values
);
static mylite_storage_result append_rollback_auto_increment_value(
    mylite_storage_autoincrement_rollback_values *values,
    unsigned long long table_id,
    unsigned long long next_value
);
static mylite_storage_result publish_rollback_auto_increment_values(
    mylite_storage_statement *statement,
    const mylite_storage_autoincrement_rollback_values *values
);
static int catalog_contains_table_id(
    const unsigned char *catalog_page,
    unsigned long long table_id
);
static void free_rollback_auto_increment_values(
    mylite_storage_autoincrement_rollback_values *values
);
static char *copy_string(const char *value);
static char *copy_filename(const char *filename);
static mylite_storage_result read_header(FILE *file, mylite_storage_header *out_header);
static mylite_storage_result read_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    unsigned char *out_page
);
static mylite_storage_result publish_header(FILE *file, const mylite_storage_header *header);
static mylite_storage_result write_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page
);
static mylite_storage_result write_page_at_raw(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page
);
static mylite_storage_result write_pages_at_raw(
    FILE *file,
    unsigned long long first_page_id,
    unsigned page_size,
    const unsigned char *pages,
    size_t page_count
);
static mylite_storage_result buffer_append_pages_at_raw(
    FILE *file,
    unsigned long long first_page_id,
    unsigned page_size,
    const unsigned char *pages,
    size_t page_count,
    int *out_buffered
);
static mylite_storage_result copy_buffered_append_page(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    unsigned char *out_page,
    int *out_copied
);
static int replace_buffered_append_page(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page,
    int checksum_dirty
);
static unsigned char *buffered_append_page(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size
);
static unsigned char *buffered_append_page_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id,
    unsigned page_size
);
static int buffered_append_page_checksum_dirty(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size
);
static int buffered_append_page_checksum_dirty_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id,
    unsigned page_size
);
static void set_buffered_append_page_checksum_dirty(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    int checksum_dirty
);
static void set_buffered_append_page_checksum_dirty_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id,
    unsigned page_size,
    int checksum_dirty
);
static unsigned char *buffered_append_page_checksum_dirty_slot_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id,
    unsigned page_size
);
static mylite_storage_result refresh_dirty_buffered_page_checksum(unsigned char *page);
static int buffered_append_page_range_contains_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long first_page_id,
    unsigned long long page_count
);
static mylite_storage_result capture_buffered_page_undo(
    mylite_storage_statement *statement,
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id
);
static size_t buffered_page_undo_used_size(const unsigned char *page);
static int buffered_update_rewrite_row_state_known(
    mylite_storage_statement *statement,
    unsigned long long row_id
);
static int buffered_update_rewrite_shape_known(
    mylite_storage_statement *statement,
    unsigned long long row_id,
    unsigned long long table_id,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed
);
static mylite_storage_result mark_buffered_update_rewrite_row_state(
    mylite_storage_statement *statement,
    unsigned long long row_id
);
static void mark_buffered_update_rewrite_shape(
    mylite_storage_statement *statement,
    unsigned long long row_id,
    unsigned long long table_id,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed
);
static mylite_storage_result ensure_buffered_update_rewrite_buckets(
    mylite_storage_buffered_update_rewrite_cache *cache,
    size_t next_count
);
static size_t buffered_update_rewrite_bucket_capacity(size_t entry_count);
static mylite_storage_result rebuild_buffered_update_rewrite_buckets(
    mylite_storage_buffered_update_rewrite_cache *cache,
    size_t bucket_capacity
);
static mylite_storage_buffered_update_rewrite_bucket *find_buffered_update_rewrite_bucket(
    mylite_storage_buffered_update_rewrite_cache *cache,
    unsigned long long row_id
);
static int place_buffered_update_rewrite_bucket(
    mylite_storage_buffered_update_rewrite_bucket *buckets,
    size_t bucket_capacity,
    const mylite_storage_buffered_update_rewrite_bucket *entry
);
static mylite_storage_result ensure_append_page_buffer_capacity(
    mylite_storage_statement *statement,
    size_t needed_pages
);
static mylite_storage_result decode_header_page(
    const unsigned char *page,
    mylite_storage_header *out_header
);
static mylite_storage_result validate_catalog_root_page(
    FILE *file,
    const mylite_storage_header *header
);
static mylite_storage_result validate_catalog_root_bytes(
    const unsigned char *page,
    const mylite_storage_header *header
);
static mylite_storage_result validate_catalog_records(
    const unsigned char *page,
    const mylite_storage_header *header
);
static mylite_storage_result validate_catalog_record(
    const unsigned char *record,
    size_t available_bytes,
    const mylite_storage_header *header,
    size_t *out_record_size
);
static mylite_storage_result validate_table_definition(
    const mylite_storage_table_definition *definition,
    mylite_storage_definition_lengths *out_lengths
);
static mylite_storage_result validate_schema_definition(
    const mylite_storage_schema_definition *definition,
    mylite_storage_schema_definition_lengths *out_lengths
);
static mylite_storage_result validate_foreign_key_definition(
    const mylite_storage_foreign_key_definition *definition,
    mylite_storage_foreign_key_definition_lengths *out_lengths
);
static mylite_storage_result validate_index_root_definition(
    const mylite_storage_index_root_definition *definition,
    mylite_storage_index_root_definition_lengths *out_lengths
);
static mylite_storage_result validate_schema_name(const char *schema_name, size_t *out_length);
static mylite_storage_result validate_string_field(const char *value, size_t *out_length);
static mylite_storage_result validate_foreign_key_action(unsigned action);
static mylite_storage_result validate_foreign_key_match_option(unsigned match_option);
static mylite_storage_result validate_foreign_key_columns(
    const char *const *column_names,
    size_t column_count,
    size_t *out_column_names_size
);
static mylite_storage_result validate_index_entries(
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count
);
static mylite_storage_result update_row_with_index_entries(
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
static size_t changed_index_entry_count(
    const unsigned char *index_entry_changed,
    size_t index_entry_count
);
static int is_index_entry_changed(const unsigned char *index_entry_changed, size_t entry_index);
static mylite_storage_result rename_table(
    const char *filename,
    const char *old_schema_name,
    const char *old_table_name,
    const char *new_schema_name,
    const char *new_table_name,
    int preserve_foreign_keys
);
static mylite_storage_result read_catalog_root(
    FILE *file,
    const mylite_storage_header *header,
    unsigned char *out_page
);
static size_t catalog_used_bytes(const unsigned char *page);
static unsigned long long catalog_record_count(const unsigned char *page);
static mylite_storage_result find_table_record(
    const unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    mylite_storage_catalog_entry *out_entry
);
static mylite_storage_result find_schema_record(
    const unsigned char *catalog_page,
    const char *schema_name
);
static mylite_storage_result find_foreign_key_record(
    const unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name,
    mylite_storage_catalog_entry *out_entry
);
static mylite_storage_result find_parent_foreign_key_record(
    const unsigned char *catalog_page,
    const char *referenced_schema_name,
    const char *referenced_table_name
);
static mylite_storage_result find_index_root_record(
    const unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    unsigned long long table_id,
    unsigned index_number,
    mylite_storage_catalog_entry *out_entry
);
static int catalog_has_schema(const unsigned char *catalog_page, const char *schema_name);
static mylite_storage_result read_table_metadata_from_record(
    const unsigned char *record,
    mylite_storage_table_metadata *out_metadata
);
static mylite_storage_result read_schema_metadata_from_record(
    const unsigned char *record,
    mylite_storage_schema_metadata *out_metadata
);
static mylite_storage_result remove_table_record(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name
);
static mylite_storage_result remove_foreign_key_record(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name
);
static mylite_storage_result remove_child_foreign_key_records(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name
);
static mylite_storage_result remove_index_root_record(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    unsigned long long table_id,
    unsigned index_number
);
static mylite_storage_result remove_table_index_root_records(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    unsigned long long table_id
);
static mylite_storage_result remove_explicit_schema_records(
    unsigned char *catalog_page,
    const char *schema_name
);
static mylite_storage_result remove_schema_records(
    unsigned char *catalog_page,
    const char *schema_name
);
static mylite_storage_result rename_table_record(
    unsigned char *catalog_page,
    mylite_storage_table_identity old_identity,
    mylite_storage_table_identity new_identity,
    int preserve_foreign_keys
);
static mylite_storage_result append_renamed_foreign_key_record(
    unsigned char *catalog_page,
    const unsigned char *record,
    mylite_storage_table_identity old_identity,
    mylite_storage_table_identity new_identity
);
static mylite_storage_result append_renamed_index_root_record(
    unsigned char *catalog_page,
    const unsigned char *record,
    mylite_storage_table_identity new_identity
);
static int record_matches_table(
    const unsigned char *record,
    const char *schema_name,
    const char *table_name
);
static int record_matches_foreign_key(
    const unsigned char *record,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name
);
static int record_matches_foreign_key_parent(
    const unsigned char *record,
    const char *referenced_schema_name,
    const char *referenced_table_name
);
static int record_matches_index_root(
    const unsigned char *record,
    const char *schema_name,
    const char *table_name,
    unsigned long long table_id,
    unsigned index_number
);
static int record_is_table(const unsigned char *record);
static int record_is_schema(const unsigned char *record);
static int record_is_foreign_key(const unsigned char *record);
static int record_is_index_root(const unsigned char *record);
static int record_matches_schema(const unsigned char *record, const char *schema_name);
static size_t record_header_size(const unsigned char *record);
static size_t record_field_offset(const unsigned char *record, unsigned field_index);
static size_t record_field_size(const unsigned char *record, unsigned field_index);
static mylite_storage_result append_schema_record(
    unsigned char *catalog_page,
    const mylite_storage_schema_definition *definition,
    const mylite_storage_schema_definition_lengths *lengths
);
static mylite_storage_result append_table_record(
    unsigned char *catalog_page,
    const mylite_storage_table_definition *definition,
    const mylite_storage_definition_lengths *lengths,
    unsigned long long definition_root_page,
    unsigned long long table_id
);
static mylite_storage_result append_foreign_key_record(
    unsigned char *catalog_page,
    const mylite_storage_foreign_key_definition *definition,
    const mylite_storage_foreign_key_definition_lengths *lengths,
    unsigned long long metadata_root_page,
    unsigned long long metadata_size,
    unsigned long long table_id
);
static mylite_storage_result append_foreign_key_record_fields(
    unsigned char *catalog_page,
    const mylite_storage_foreign_key_record_fields *fields
);
static mylite_storage_result append_index_root_record(
    unsigned char *catalog_page,
    const mylite_storage_index_root_definition *definition,
    const mylite_storage_index_root_definition_lengths *lengths,
    unsigned long long table_id
);
static mylite_storage_result append_index_root_record_fields(
    unsigned char *catalog_page,
    const mylite_storage_index_root_record_fields *fields
);
static mylite_storage_result next_table_id(
    FILE *file,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long *out_table_id
);
static unsigned long long catalog_max_table_id(const unsigned char *catalog_page);
static mylite_storage_result read_max_row_table_id(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long *inout_table_id
);
static mylite_storage_result write_definition_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    const unsigned char *definition,
    size_t definition_size
);
static mylite_storage_result write_foreign_key_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    const unsigned char *metadata,
    size_t metadata_size
);
static mylite_storage_result encode_foreign_key_metadata(
    const mylite_storage_foreign_key_definition *definition,
    const mylite_storage_foreign_key_definition_lengths *lengths,
    unsigned char **out_metadata,
    size_t *out_metadata_size
);
static void encode_foreign_key_column_names(
    unsigned char *metadata,
    const char *const *column_names,
    size_t column_count
);
static mylite_storage_result write_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    mylite_storage_blob_write blob
);
static void encode_blob_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long next_page_id,
    const unsigned char *payload,
    size_t payload_size,
    unsigned page_type
);
static mylite_storage_result find_table_id(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_table_id
);
static void encode_row_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    mylite_storage_row_write row
);
static mylite_storage_result write_row_payload_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const unsigned char *row,
    size_t row_size,
    mylite_storage_row_write_position *out_position
);
static mylite_storage_result write_inline_update_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long source_row_id,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed,
    mylite_storage_row_write_position *out_position,
    unsigned long long *out_next_page_id,
    int *out_used_fast_path
);
static mylite_storage_result rewrite_active_update_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed,
    int *out_rewritten
);
static void rewrite_buffered_row_page(
    unsigned char *page,
    const unsigned char *row,
    size_t row_size
);
static void rewrite_buffered_index_entry_page(
    unsigned char *page,
    const mylite_storage_index_entry *index_entry
);
static mylite_storage_result write_index_entry_pages(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_index_entry_write write,
    unsigned long long *out_next_page_id
);
static void encode_index_entry_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned long long row_id,
    const mylite_storage_index_entry *index_entry
);
static mylite_storage_result decode_index_entry_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_index_entry_page *out_index_entry_page
);
static mylite_storage_result decode_buffered_index_entry_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_index_entry_page *out_index_entry_page
);
static int is_index_entry_page(const unsigned char *page);
static mylite_storage_result prepare_index_leaf_pages(
    unsigned char **out_pages,
    size_t *out_page_count,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned index_number,
    const mylite_storage_index_entryset *entryset
);
static void encode_index_leaf_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned index_number,
    const mylite_storage_index_entryset *entryset,
    const size_t *order,
    size_t first_entry,
    size_t entry_count,
    size_t key_size,
    size_t used_bytes
);
static size_t index_leaf_entry_capacity(size_t key_size);
static mylite_storage_result read_index_leaf_page(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page
);
static mylite_storage_result decode_index_leaf_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page
);
static int is_index_leaf_page(const unsigned char *page);
static mylite_storage_result read_index_leaf_run_root(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_catalog_entry *root_entry,
    unsigned long long table_id,
    unsigned index_number,
    unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page,
    mylite_storage_index_leaf_run *out_leaf_run
);
static mylite_storage_result read_index_leaf_run_page(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long table_id,
    unsigned index_number,
    unsigned long long page_offset,
    unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page
);
static size_t index_leaf_run_expected_entry_count(
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long page_offset
);
static mylite_storage_result read_live_index_entries(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    mylite_storage_index_entryset *out_entries
);
static mylite_storage_result read_index_leaf_exact_entries(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long table_id,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries,
    int *out_used_leaf
);
static mylite_storage_result read_index_leaf_exact_row_ids(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long table_id,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_row_id_list *out_row_ids,
    int *out_used_leaf
);
static mylite_storage_result append_index_leaf_matches_to_row_id_list(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_row_id_list *out_row_ids
);
static mylite_storage_result append_index_leaf_matches_to_entryset(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries
);
static mylite_storage_result append_index_leaf_run_matches_to_row_id_list(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_row_id_list *out_row_ids
);
static mylite_storage_result append_index_leaf_run_matches_to_entryset(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries
);
static mylite_storage_result find_index_leaf_run_match_page(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_page_offset,
    int *out_found
);
static int compare_index_leaf_page_key_range(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size
);
static int index_leaf_page_first_key_matches(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size
);
static int index_leaf_page_last_key_matches(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size
);
static mylite_storage_result scan_exact_index_row_ids(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_row_id_list *out_row_ids
);
static mylite_storage_result scan_exact_index_row_ids_from(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long first_page_id,
    mylite_storage_row_id_list *out_row_ids
);
static mylite_storage_result scan_exact_index_entries(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries
);
static mylite_storage_result scan_exact_index_entries_from(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long first_page_id,
    mylite_storage_index_entryset *out_entries
);
static int is_exact_index_scan_skip_page(const unsigned char *page);
static mylite_storage_result index_entryset_fixed_key_size(
    const mylite_storage_index_entryset *entryset,
    size_t *out_key_size
);
static mylite_storage_result build_raw_index_entry_order(
    const mylite_storage_index_entryset *entryset,
    size_t **out_order
);
static int compare_raw_index_entry(
    const mylite_storage_index_entryset *entryset,
    size_t left_index,
    size_t right_index
);
static int compare_leaf_key(
    const mylite_storage_index_leaf_page *leaf_page,
    size_t entry_index,
    const unsigned char *key,
    size_t key_size
);
static const unsigned char *index_leaf_entry_key(
    const mylite_storage_index_leaf_page *leaf_page,
    size_t entry_index
);
static unsigned long long index_leaf_entry_row_id(
    const mylite_storage_index_leaf_page *leaf_page,
    size_t entry_index
);
static mylite_storage_result scan_table_row_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_page_callback callback,
    void *ctx
);
static mylite_storage_result collect_live_table_row_ids(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_id_list *out_row_ids
);
static mylite_storage_result copy_cached_durable_live_row_ids(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_id_list *out_row_ids,
    int *out_used_cache
);
static void store_durable_live_row_ids(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const mylite_storage_row_id_list *row_ids
);
static void clear_durable_live_row_id_caches(const char *filename);
static void retarget_durable_live_row_id_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static void promote_statement_live_row_id_caches(const mylite_storage_statement *statement);
static void seed_active_live_row_id_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static void append_active_live_row_id(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
);
static void replace_active_live_row_id(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long old_row_id,
    unsigned long long new_row_id
);
static void remove_active_live_row_id(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
);
static mylite_storage_live_row_id_cache *active_live_row_id_cache_for(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_live_row_id_cache *find_active_live_row_id_cache(
    mylite_storage_live_row_id_cache_set *caches,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_result append_active_live_row_id_cache(
    mylite_storage_live_row_id_cache_set *caches,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_live_row_id_cache **out_cache
);
static int append_row_id_to_cache(
    mylite_storage_live_row_id_cache *cache,
    unsigned long long row_id
);
static int replace_row_id_in_cache(
    mylite_storage_live_row_id_cache *cache,
    unsigned long long old_row_id,
    unsigned long long new_row_id
);
static int remove_row_id_from_cache(
    mylite_storage_live_row_id_cache *cache,
    unsigned long long row_id
);
static int durable_live_row_id_cache_available(const char *filename);
static mylite_storage_live_row_id_cache *find_durable_live_row_id_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_live_row_id_cache *ensure_durable_live_row_id_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_result append_durable_live_row_id_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_live_row_id_cache **out_cache
);
static mylite_storage_result assign_live_row_id_cache(
    mylite_storage_live_row_id_cache *cache,
    const mylite_storage_row_id_list *row_ids
);
static mylite_storage_result copy_live_row_ids(
    const unsigned long long *row_ids,
    size_t row_id_count,
    mylite_storage_row_id_list *out_row_ids
);
static void free_live_row_id_cache(mylite_storage_live_row_id_cache *cache);
static mylite_storage_result compact_live_table_row_ids(
    mylite_storage_row_id_list *row_ids,
    const mylite_storage_row_state_map *row_state_map
);
static mylite_storage_result read_row_ids_into_rowset(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    const mylite_storage_row_id_list *row_ids,
    mylite_storage_rowset *out_rows
);
static mylite_storage_result validate_row_id_payloads(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const mylite_storage_row_id_list *row_ids
);
static mylite_storage_result read_row_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_row_page *out_row_page
);
static mylite_storage_result decode_row_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_page *out_row_page
);
static mylite_storage_result decode_row_page_metadata(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_page_metadata *out_metadata
);
static mylite_storage_result decode_buffered_row_page_metadata(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_page_metadata *out_metadata
);
static int is_row_page(const unsigned char *page);
static mylite_storage_result validate_direct_live_row(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    unsigned char *page,
    mylite_storage_row_page *out_row_page
);
static mylite_storage_result row_is_hidden_after(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    unsigned long long first_page_id,
    int *out_hidden
);
static int active_live_row_known(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
);
static int active_validated_live_row_known(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
);
static mylite_storage_result mark_active_live_row(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
);
static mylite_storage_result mark_active_validated_live_row(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
);
static void replace_active_live_row(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long old_row_id,
    unsigned long long new_row_id
);
static mylite_storage_live_row_cache *live_row_cache_for(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_live_row_cache *find_live_row_cache(
    mylite_storage_live_row_cache_set *caches,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_result append_live_row_cache(
    mylite_storage_live_row_cache_set *caches,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_live_row_cache **out_cache
);
static mylite_storage_result add_live_row_id(
    mylite_storage_live_row_cache *cache,
    unsigned long long row_id
);
static mylite_storage_result add_validated_live_row_id(
    mylite_storage_live_row_cache *cache,
    unsigned long long row_id
);
static void remove_live_row_id(mylite_storage_live_row_cache *cache, unsigned long long row_id);
static void remove_validated_live_row_id(
    mylite_storage_live_row_cache *cache,
    unsigned long long row_id
);
static void encode_row_state_page(
    unsigned char *page,
    unsigned long long page_id,
    const mylite_storage_row_state_page *row_state_page
);
static mylite_storage_result build_row_state_map(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_state_map *out_row_state_map
);
static mylite_storage_result read_row_state_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_row_state_page *out_row_state_page
);
static mylite_storage_result decode_row_state_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_state_page *out_row_state_page
);
static mylite_storage_result decode_buffered_row_state_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_state_page *out_row_state_page
);
static int is_row_state_page(const unsigned char *page);
static mylite_storage_row_state_entry *find_row_state_entry(
    const mylite_storage_row_state_map *row_state_map,
    unsigned long long row_id
);
static mylite_storage_result set_row_state_entry(
    mylite_storage_row_state_map *row_state_map,
    const mylite_storage_row_state_page *row_state_page
);
static mylite_storage_result ensure_row_state_map_entry_capacity(
    mylite_storage_row_state_map *row_state_map,
    size_t minimum_capacity
);
static mylite_storage_result ensure_row_state_map_buckets(
    mylite_storage_row_state_map *row_state_map,
    size_t minimum_entry_count
);
static size_t row_state_map_bucket_capacity(size_t entry_count);
static mylite_storage_result rebuild_row_state_map_buckets(
    mylite_storage_row_state_map *row_state_map,
    size_t bucket_capacity
);
static mylite_storage_result insert_row_state_map_bucket(
    mylite_storage_row_state_map *row_state_map,
    unsigned long long row_id,
    size_t entry_index
);
static int place_row_state_map_bucket(
    mylite_storage_row_state_map_bucket *buckets,
    size_t bucket_capacity,
    unsigned long long row_id,
    size_t entry_index
);
static void free_row_state_map(mylite_storage_row_state_map *row_state_map);
static mylite_storage_result append_live_row_id(void *ctx, const mylite_storage_row_page *row_page);
static int truncate_needs_publication(
    const mylite_storage_row_id_list *live_rows,
    int reset_auto_increment
);
static mylite_storage_result write_truncate_publication(
    FILE *file,
    const char *filename,
    mylite_storage_header *header,
    unsigned long long table_id,
    const mylite_storage_row_id_list *live_rows,
    int reset_auto_increment
);
static mylite_storage_result validate_truncate_page_capacity(
    const mylite_storage_header *header,
    size_t live_row_count,
    int reset_auto_increment
);
static mylite_storage_result write_truncate_row_state_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const mylite_storage_row_id_list *live_rows,
    unsigned long long first_page_id,
    unsigned long long *out_next_page_id
);
static mylite_storage_result write_truncate_auto_increment_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long page_id,
    unsigned long long *out_next_page_id
);
static mylite_storage_result publish_header_page_count(
    FILE *file,
    mylite_storage_header *header,
    unsigned long long page_count
);
static mylite_storage_result initialize_rowset_builder(
    mylite_storage_rowset_builder *builder,
    mylite_storage_rowset *rowset,
    size_t metadata_capacity
);
static mylite_storage_result append_row_to_rowset_builder(
    mylite_storage_rowset_builder *builder,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
);
static mylite_storage_result ensure_rowset_builder_row_capacity(
    mylite_storage_rowset_builder *builder,
    size_t required_bytes,
    size_t first_row_size
);
static mylite_storage_result read_row_payload(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    int validate_visibility,
    unsigned char **out_row,
    size_t *out_row_size
);
static mylite_storage_result read_indexed_row_payload_from_open_file(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    unsigned char **out_row,
    size_t *inout_row_capacity,
    size_t *out_row_size
);
static mylite_storage_result find_indexed_row_payload(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    unsigned char **out_row,
    size_t *inout_row_capacity,
    size_t *out_row_size
);
static mylite_storage_result append_row_id_to_list(
    mylite_storage_row_id_list *list,
    unsigned long long row_id
);
static void remove_row_id_from_list(mylite_storage_row_id_list *list, unsigned long long row_id);
static void replace_row_id_in_list(
    mylite_storage_row_id_list *list,
    unsigned long long old_row_id,
    unsigned long long new_row_id
);
static mylite_storage_result append_index_prefix_match(
    mylite_storage_index_prefix_match_list *list,
    unsigned long long row_id,
    unsigned index_number
);
static void remove_index_prefix_matches(
    mylite_storage_index_prefix_match_list *list,
    unsigned long long row_id,
    unsigned index_number
);
static void remove_index_prefix_matches_by_row_id(
    mylite_storage_index_prefix_match_list *list,
    unsigned long long row_id
);
static void replace_index_prefix_match_row_id(
    mylite_storage_index_prefix_match_list *list,
    unsigned long long old_row_id,
    unsigned long long new_row_id
);
static mylite_storage_result find_cached_exact_index_entry(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    int *out_used_cache
);
static mylite_storage_result find_exact_index_row_id(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    const mylite_storage_catalog_entry *table_entry,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id
);
static mylite_storage_result find_cached_durable_exact_index_entry(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    int *out_used_cache
);
static mylite_storage_result append_cached_durable_exact_index_entries(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries,
    int *out_used_cache
);
static mylite_storage_result find_exact_index_cache_entry_row_id(
    mylite_storage_exact_index_cache *cache,
    const unsigned char *key,
    unsigned long long *out_row_id
);
static mylite_storage_result append_exact_index_cache_matches_to_entryset(
    mylite_storage_exact_index_cache *cache,
    const unsigned char *key,
    mylite_storage_index_entryset *out_entries
);
static mylite_storage_result durable_exact_index_cache_for(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size,
    mylite_storage_exact_index_cache **out_cache
);
static void promote_statement_exact_index_caches(const mylite_storage_statement *statement);
static mylite_storage_result seed_active_exact_index_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size,
    mylite_storage_exact_index_cache *cache,
    int *out_seeded_cache
);
static mylite_storage_result append_active_exact_index_cache_entries(
    const char *filename,
    unsigned long long table_id,
    unsigned long long row_id,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count
);
static void replace_active_exact_index_cache_entries(
    const char *filename,
    unsigned long long table_id,
    unsigned long long old_row_id,
    unsigned long long new_row_id,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count
);
static void invalidate_exact_index_caches(const char *filename);
static void clear_durable_exact_index_caches(const char *filename);
static void retarget_durable_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static void retarget_durable_exact_index_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_exact_index_cache *ensure_durable_exact_index_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size
);
static mylite_storage_result copy_exact_index_cache_entries(
    mylite_storage_exact_index_cache *destination,
    const mylite_storage_exact_index_cache *source
);
static mylite_storage_result append_cached_row_payload_to_builder(
    const mylite_storage_row_payload_cache *cache,
    unsigned long long row_id,
    mylite_storage_rowset_builder *builder,
    int *out_used_cache
);
static const mylite_storage_row_payload_cache_entry *active_row_payload_cache_entry_for(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
);
static void store_active_row_payload(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
);
static void replace_active_row_payload(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long old_row_id,
    unsigned long long new_row_id,
    const unsigned char *new_row,
    size_t new_row_size
);
static void remove_active_row_payload(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
);
static mylite_storage_result copy_cached_row_payload(
    const mylite_storage_row_payload_cache_entry *entry,
    unsigned char **out_row,
    size_t *inout_row_capacity,
    size_t *out_row_size
);
static mylite_storage_result copy_row_payload_to_output(
    const unsigned char *row,
    size_t row_size,
    unsigned char **out_row,
    size_t *inout_row_capacity,
    size_t *out_row_size
);
static mylite_storage_row_payload_cache *active_row_payload_cache_for(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_row_payload_cache *ensure_active_row_payload_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_row_payload_cache *find_active_row_payload_cache(
    mylite_storage_row_payload_cache_set *caches,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_result append_active_row_payload_cache(
    mylite_storage_row_payload_cache_set *caches,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **out_cache
);
static mylite_storage_result append_cached_durable_row_payload_to_builder(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **cache,
    unsigned long long *cache_generation,
    unsigned long long row_id,
    mylite_storage_rowset_builder *builder,
    int *out_used_cache
);
static void store_durable_row_payload(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **cache,
    unsigned long long *cache_generation,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
);
static void clear_durable_row_payload_caches(const char *filename);
static void retarget_durable_row_payload_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_row_payload_cache *durable_row_payload_cache_for(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static mylite_storage_row_payload_cache *durable_row_payload_cache_for_batch(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **cache,
    unsigned long long *cache_generation
);
static mylite_storage_row_payload_cache *find_durable_row_payload_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
);
static int durable_row_payload_cache_available(const char *filename);
static mylite_storage_row_payload_cache *ensure_durable_row_payload_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long *cache_generation
);
static mylite_storage_result append_durable_row_payload_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **out_cache
);
static mylite_storage_result append_row_payload_cache_entry(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
);
static mylite_storage_result put_row_payload_cache_entry(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
);
static mylite_storage_result replace_row_payload_cache_entry(
    mylite_storage_row_payload_cache *cache,
    unsigned long long old_row_id,
    unsigned long long new_row_id,
    const unsigned char *new_row,
    size_t new_row_size
);
static void remove_row_payload_cache_entry(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id
);
static const mylite_storage_row_payload_cache_entry *find_row_payload_cache_entry(
    const mylite_storage_row_payload_cache *cache,
    unsigned long long row_id
);
static mylite_storage_row_payload_cache_bucket *find_mutable_row_payload_cache_bucket(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id
);
static mylite_storage_result ensure_row_payload_cache_buckets(
    mylite_storage_row_payload_cache *cache,
    size_t next_count
);
static mylite_storage_result rebuild_row_payload_cache_buckets(
    mylite_storage_row_payload_cache *cache,
    size_t bucket_capacity
);
static mylite_storage_result insert_row_payload_cache_bucket(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id,
    size_t entry_index
);
static void remove_row_payload_cache_bucket(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id
);
static void maybe_rebuild_row_payload_cache_buckets_after_tombstone(
    mylite_storage_row_payload_cache *cache
);
static size_t hash_row_id(unsigned long long row_id);
static void free_row_payload_cache(mylite_storage_row_payload_cache *cache);
static mylite_storage_result read_cached_durable_index_leaf_page(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page,
    int *out_used_cache
);
static void store_durable_index_leaf_page(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    const mylite_storage_index_leaf_page *leaf_page
);
static void clear_durable_index_leaf_page_caches(const char *filename);
static void retarget_durable_index_leaf_page_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header
);
static mylite_storage_index_leaf_page_cache *durable_index_leaf_page_cache_for(
    const char *filename,
    const mylite_storage_header *header
);
static mylite_storage_index_leaf_page_cache *find_durable_index_leaf_page_cache(
    const char *filename,
    const mylite_storage_header *header
);
static mylite_storage_result append_durable_index_leaf_page_cache(
    const char *filename,
    const mylite_storage_header *header,
    mylite_storage_index_leaf_page_cache **out_cache
);
static mylite_storage_result append_index_leaf_page_cache_entry(
    mylite_storage_index_leaf_page_cache *cache,
    unsigned long long page_id,
    const unsigned char *page,
    const mylite_storage_index_leaf_page *leaf_page
);
static const mylite_storage_index_leaf_page_cache_entry *find_index_leaf_page_cache_entry(
    const mylite_storage_index_leaf_page_cache *cache,
    unsigned long long page_id
);
static void free_index_leaf_page_cache(mylite_storage_index_leaf_page_cache *cache);
static mylite_storage_exact_index_cache *find_durable_exact_index_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size
);
static mylite_storage_result append_durable_exact_index_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size,
    mylite_storage_exact_index_cache **out_cache
);
static mylite_storage_exact_index_cache *find_exact_index_cache(
    mylite_storage_exact_index_cache_set *caches,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size
);
static mylite_storage_result append_exact_index_cache(
    mylite_storage_exact_index_cache_set *caches,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size,
    mylite_storage_exact_index_cache **out_cache
);
static mylite_storage_result load_exact_index_cache(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_exact_index_cache *cache
);
static mylite_storage_result append_exact_index_cache_entry(
    mylite_storage_exact_index_cache *cache,
    const unsigned char *key,
    unsigned long long row_id
);
static void remove_exact_index_cache_entries_by_row_id(
    mylite_storage_exact_index_cache *cache,
    unsigned long long row_id
);
static mylite_storage_result grow_exact_index_cache_entry_capacity(
    mylite_storage_exact_index_cache *cache,
    size_t minimum_capacity
);
static void maybe_compact_exact_index_cache_entries(mylite_storage_exact_index_cache *cache);
static void compact_exact_index_cache_entries(mylite_storage_exact_index_cache *cache);
static void unlink_exact_index_cache_bucket_entry(
    mylite_storage_exact_index_cache *cache,
    size_t entry_index
);
static void link_exact_index_cache_bucket_entry(
    mylite_storage_exact_index_cache *cache,
    size_t entry_index
);
static mylite_storage_result ensure_exact_index_cache_buckets(
    mylite_storage_exact_index_cache *cache
);
static size_t exact_index_cache_bucket_for_key(
    const mylite_storage_exact_index_cache *cache,
    const unsigned char *key
);
static size_t exact_index_cache_bucket_count(size_t entry_count);
static size_t hash_key_bytes(const unsigned char *key, size_t key_size);
static void clear_exact_index_cache_buckets(mylite_storage_exact_index_cache *cache);
static void unlink_exact_index_cache_row_id_bucket_entry(
    mylite_storage_exact_index_cache *cache,
    size_t entry_index
);
static void link_exact_index_cache_row_id_bucket_entry(
    mylite_storage_exact_index_cache *cache,
    size_t entry_index
);
static mylite_storage_result ensure_exact_index_cache_row_id_buckets(
    mylite_storage_exact_index_cache *cache
);
static size_t exact_index_cache_bucket_for_row_id(
    const mylite_storage_exact_index_cache *cache,
    unsigned long long row_id
);
static void clear_exact_index_cache_row_id_buckets(mylite_storage_exact_index_cache *cache);
static mylite_storage_result append_index_entry_to_entryset(
    mylite_storage_index_entryset *entryset,
    const mylite_storage_index_entry_page *entry_page
);
static mylite_storage_result grow_index_entryset_for_append(
    mylite_storage_index_entryset *entryset,
    size_t additional_entry_count,
    size_t additional_key_bytes,
    size_t *out_first_entry,
    size_t *out_first_key_offset
);
static void remove_index_entries_by_row_id(
    mylite_storage_index_entryset *entryset,
    unsigned long long row_id
);
static void replace_index_entries_row_id(
    mylite_storage_index_entryset *entryset,
    unsigned long long old_row_id,
    unsigned long long new_row_id
);
static void encode_autoincrement_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned long long next_value
);
static mylite_storage_result read_autoincrement_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_autoincrement_page *out_autoincrement_page
);
static int is_autoincrement_page(const unsigned char *page);
static mylite_storage_result latest_auto_increment_value(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long *out_next_value
);
static mylite_storage_result publish_auto_increment_value(
    FILE *file,
    const char *filename,
    mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long next_value
);
static mylite_storage_result read_definition_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_catalog_entry *entry,
    unsigned char **out_definition,
    size_t *out_definition_size
);
static mylite_storage_result read_foreign_key_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_catalog_entry *entry,
    unsigned char **out_metadata,
    size_t *out_metadata_size
);
static mylite_storage_result read_row_payload_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_blob_chain chain,
    unsigned char **out_payload
);
static mylite_storage_result read_blob_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned long long expected_remaining,
    unsigned char *out_payload,
    size_t *inout_written,
    unsigned long long *out_next_page_id,
    unsigned expected_page_type
);
static char *copy_record_field(const unsigned char *record, unsigned field_index);
static mylite_storage_result decode_foreign_key_metadata(
    const unsigned char *record,
    const unsigned char *metadata,
    size_t metadata_size,
    mylite_storage_foreign_key_metadata *out_metadata
);
static mylite_storage_result copy_foreign_key_metadata_identity(
    const unsigned char *record,
    mylite_storage_foreign_key_metadata *out_metadata
);
static mylite_storage_result copy_foreign_key_payload_strings(
    const unsigned char *metadata,
    size_t metadata_size,
    mylite_storage_foreign_key_metadata *out_metadata
);
static mylite_storage_result copy_serialized_column_names(
    const unsigned char *serialized_names,
    size_t serialized_names_size,
    size_t column_count,
    char ***out_column_names
);
static void free_column_names(char **column_names, size_t column_count);
static mylite_storage_result list_catalog_schemas(
    const unsigned char *catalog_page,
    mylite_storage_schema_callback callback,
    void *ctx
);
static mylite_storage_result collect_schema_name(
    mylite_storage_schema_list *list,
    const unsigned char *record
);
static int schema_list_contains(
    const mylite_storage_schema_list *list,
    const char *schema_name,
    size_t schema_name_size
);
static void free_schema_list(mylite_storage_schema_list *list);
static mylite_storage_result list_catalog_tables(
    const unsigned char *catalog_page,
    const char *schema_name,
    mylite_storage_table_callback callback,
    void *ctx
);
static unsigned get_u32_le(const unsigned char *page, size_t offset);
static unsigned long long get_u64_le(const unsigned char *page, size_t offset);
static void put_u32_le(unsigned char *page, size_t offset, unsigned value);
static void put_u64_le(unsigned char *page, size_t offset, unsigned long long value);
static uint64_t checksum_page(const unsigned char *page, size_t checksum_offset);
static uint64_t checksum_page_zero_tail(
    const unsigned char *page,
    size_t checksum_offset,
    size_t used_size
);
static uint64_t advance_checksum_zero_bytes(uint64_t checksum, size_t byte_count);

static const unsigned char k_header_magic[8] = {'M', 'Y', 'L', 'I', 'T', 'E', '1', '\0'};
static const unsigned char k_catalog_magic[8] = {'M', 'Y', 'L', 'C', 'A', 'T', '1', '\0'};
static const unsigned char k_blob_magic[8] = {'M', 'Y', 'L', 'B', 'L', 'B', '1', '\0'};
static const unsigned char k_row_magic[8] = {'M', 'Y', 'L', 'R', 'O', 'W', '1', '\0'};
static const unsigned char k_autoincrement_magic[8] = {'M', 'Y', 'L', 'A', 'U', 'T', '1', '\0'};
static const unsigned char k_row_state_magic[8] = {'M', 'Y', 'L', 'R', 'S', 'T', '1', '\0'};
static const unsigned char k_index_magic[8] = {'M', 'Y', 'L', 'I', 'D', 'X', '1', '\0'};
static const unsigned char k_journal_magic[8] = {'M', 'Y', 'L', 'J', 'N', 'L', '1', '\0'};
static const unsigned k_lock_retry_sleep_ms = 5U;
static const useconds_t k_microseconds_per_millisecond = 1000U;
static const uint64_t k_fnv1a64_offset_basis = UINT64_C(1469598103934665603);
static const uint64_t k_fnv1a64_prime = UINT64_C(1099511628211);
static _Thread_local unsigned active_busy_timeout_ms = 0U;

const char *mylite_storage_engine_name(void) {
    return MYLITE_STORAGE_ENGINE_NAME;
}

mylite_storage_capabilities mylite_storage_get_capabilities(void) {
    mylite_storage_capabilities capabilities = {
        .size = sizeof(capabilities),
        .format_version = MYLITE_STORAGE_FORMAT_VERSION,
        .flags =
            MYLITE_STORAGE_CAPABILITY_FILE_HEADER | MYLITE_STORAGE_CAPABILITY_EMPTY_CATALOG |
            MYLITE_STORAGE_CAPABILITY_TABLE_DEFINITIONS | MYLITE_STORAGE_CAPABILITY_TABLE_ROWS |
            MYLITE_STORAGE_CAPABILITY_AUTOINCREMENT | MYLITE_STORAGE_CAPABILITY_BLOB_TEXT_ROWS |
            MYLITE_STORAGE_CAPABILITY_ROW_LIFECYCLE | MYLITE_STORAGE_CAPABILITY_INDEX_ENTRIES |
            MYLITE_STORAGE_CAPABILITY_RECOVERY_JOURNAL | MYLITE_STORAGE_CAPABILITY_FILE_LOCKS |
            MYLITE_STORAGE_CAPABILITY_TRUNCATE | MYLITE_STORAGE_CAPABILITY_SCHEMAS |
            MYLITE_STORAGE_CAPABILITY_STATEMENT_CHECKPOINTS |
            MYLITE_STORAGE_CAPABILITY_BUSY_TIMEOUT | MYLITE_STORAGE_CAPABILITY_TRANSACTION_JOURNAL |
            MYLITE_STORAGE_CAPABILITY_FOREIGN_KEY_METADATA | MYLITE_STORAGE_CAPABILITY_INDEX_ROOTS |
            MYLITE_STORAGE_CAPABILITY_INDEX_LEAF_PAGES,
    };

    return capabilities;
}

mylite_storage_result mylite_storage_create_empty(const char *filename) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }
    clear_cached_read_file(filename);

    int exists = 0;
    mylite_storage_result result = path_exists(filename, &exists);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (exists) {
        return MYLITE_STORAGE_ERROR;
    }

    errno = 0;
    const int file_descriptor = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file_descriptor < 0) {
        return errno == EEXIST ? MYLITE_STORAGE_ERROR : MYLITE_STORAGE_IOERR;
    }

    FILE *file = fdopen(file_descriptor, "wb");
    if (file == NULL) {
        close(file_descriptor);
        remove(filename);
        return MYLITE_STORAGE_IOERR;
    }

    result = lock_file(file, LOCK_EX);
    if (result == MYLITE_STORAGE_OK) {
        result = write_empty_database(file);
    }
    if (result != MYLITE_STORAGE_OK) {
        close_existing_file(file);
        remove(filename);
        return result;
    }

    return close_created_file(file, filename);
}

mylite_storage_result mylite_storage_open_header(
    const char *filename,
    mylite_storage_header *out_header
) {
    if (filename == NULL || filename[0] == '\0' || out_header == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_header = (mylite_storage_header){0};

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    result = read_header(file, out_header);
    if (result == MYLITE_STORAGE_OK) {
        result = validate_catalog_root_page(file, out_header);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }

    if (result != MYLITE_STORAGE_OK) {
        *out_header = (mylite_storage_header){0};
    }
    return result;
}

mylite_storage_result mylite_storage_store_table_definition(
    const char *filename,
    const mylite_storage_table_definition *definition
) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    mylite_storage_definition_lengths lengths = {0};
    mylite_storage_result result = validate_table_definition(definition, &lengths);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result =
            find_table_record(catalog_page, definition->schema_name, definition->table_name, NULL);
        if (result == MYLITE_STORAGE_OK) {
            result = MYLITE_STORAGE_ERROR;
        } else if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
        }
    }

    const size_t blob_payload_size =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    const unsigned long long blob_page_count =
        ((definition->definition_size - 1U) / blob_payload_size) + 1U;
    const unsigned long long first_blob_page = header.page_count;
    unsigned long long table_id = 0ULL;
    if (result == MYLITE_STORAGE_OK && blob_page_count > ULLONG_MAX - header.page_count) {
        result = MYLITE_STORAGE_FULL;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = next_table_id(file, &header, catalog_page, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = append_table_record(catalog_page, definition, &lengths, first_blob_page, table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = write_definition_blob_pages(
            file,
            first_blob_page,
            definition->definition,
            definition->definition_size
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        header.page_count += blob_page_count;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
mylite_storage_result mylite_storage_store_schema(const char *filename, const char *schema_name) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    size_t schema_name_size = 0U;
    mylite_storage_result result = validate_schema_name(schema_name, &schema_name_size);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_schema_record(catalog_page, schema_name);
        if (result == MYLITE_STORAGE_OK) {
            if (close_existing_file(file) != MYLITE_STORAGE_OK) {
                return MYLITE_STORAGE_IOERR;
            }
            return MYLITE_STORAGE_OK;
        }
        if (result == MYLITE_STORAGE_NOTFOUND) {
            const mylite_storage_schema_definition definition = {
                .size = sizeof(definition),
                .schema_name = schema_name,
            };
            const mylite_storage_schema_definition_lengths lengths = {
                .schema_name_size = schema_name_size,
            };
            result = append_schema_record(catalog_page, &definition, &lengths);
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_store_schema_definition(
    const char *filename,
    const mylite_storage_schema_definition *definition
) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    mylite_storage_schema_definition_lengths lengths = {0};
    mylite_storage_result result = validate_schema_definition(definition, &lengths);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_explicit_schema_records(catalog_page, definition->schema_name);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = append_schema_record(catalog_page, definition, &lengths);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_drop_schema(const char *filename, const char *schema_name) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    size_t schema_name_size = 0U;
    mylite_storage_result result = validate_schema_name(schema_name, &schema_name_size);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    (void)schema_name_size;

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_schema_records(catalog_page, schema_name);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_schema_exists(const char *filename, const char *schema_name) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    size_t schema_name_size = 0U;
    mylite_storage_result result = validate_schema_name(schema_name, &schema_name_size);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    (void)schema_name_size;

    FILE *file = NULL;
    result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK && !catalog_has_schema(catalog_page, schema_name)) {
        result = MYLITE_STORAGE_NOTFOUND;
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_read_schema_definition(
    const char *filename,
    const char *schema_name,
    mylite_storage_schema_metadata *out_metadata
) {
    if (filename == NULL || filename[0] == '\0' || out_metadata == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    size_t schema_name_size = 0U;
    mylite_storage_result result = validate_schema_name(schema_name, &schema_name_size);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    (void)schema_name_size;

    *out_metadata = (mylite_storage_schema_metadata){
        .size = sizeof(*out_metadata),
    };

    FILE *file = NULL;
    result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_schema_record(catalog_page, schema_name);
        if (result == MYLITE_STORAGE_OK) {
            size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
            const unsigned long long record_count = catalog_record_count(catalog_page);
            result = MYLITE_STORAGE_NOTFOUND;
            for (unsigned long long i = 0ULL; i < record_count; ++i) {
                const unsigned char *record = catalog_page + offset;
                const size_t record_size =
                    get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
                if (record_is_schema(record) && record_matches_schema(record, schema_name)) {
                    result = read_schema_metadata_from_record(record, out_metadata);
                    break;
                }
                offset += record_size;
            }
        }
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        free(out_metadata->default_character_set_name);
        free(out_metadata->default_collation_name);
        free(out_metadata->schema_comment);
        *out_metadata = (mylite_storage_schema_metadata){0};
    }
    return result;
}

mylite_storage_result mylite_storage_store_foreign_key_definition(
    const char *filename,
    const mylite_storage_foreign_key_definition *definition
) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    mylite_storage_foreign_key_definition_lengths lengths = {0};
    mylite_storage_result result = validate_foreign_key_definition(definition, &lengths);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    unsigned char *metadata = NULL;
    size_t metadata_size = 0U;
    result = encode_foreign_key_metadata(definition, &lengths, &metadata, &metadata_size);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        free(metadata);
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry child_entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(
            catalog_page,
            definition->schema_name,
            definition->table_name,
            &child_entry
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(
            catalog_page,
            definition->referenced_schema_name,
            definition->referenced_table_name,
            NULL
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_foreign_key_record(
            catalog_page,
            definition->schema_name,
            definition->table_name,
            definition->constraint_name,
            NULL
        );
        if (result == MYLITE_STORAGE_OK) {
            result = MYLITE_STORAGE_ERROR;
        } else if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
        }
    }

    const size_t blob_payload_size =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    const unsigned long long blob_page_count = ((metadata_size - 1U) / blob_payload_size) + 1U;
    const unsigned long long first_blob_page = header.page_count;
    if (result == MYLITE_STORAGE_OK && blob_page_count > ULLONG_MAX - header.page_count) {
        result = MYLITE_STORAGE_FULL;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = append_foreign_key_record(
            catalog_page,
            definition,
            &lengths,
            first_blob_page,
            metadata_size,
            child_entry.table_id
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = write_foreign_key_blob_pages(file, first_blob_page, metadata, metadata_size);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        header.page_count += blob_page_count;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }

    free(metadata);
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_read_foreign_key_definition(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name,
    mylite_storage_foreign_key_metadata *out_metadata
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || constraint_name == NULL ||
        constraint_name[0] == '\0' || out_metadata == NULL ||
        out_metadata->size < sizeof(*out_metadata)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_metadata = (mylite_storage_foreign_key_metadata){
        .size = sizeof(*out_metadata),
    };

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry entry = {0};
    unsigned char *metadata = NULL;
    size_t metadata_size = 0U;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result =
            find_foreign_key_record(catalog_page, schema_name, table_name, constraint_name, &entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_foreign_key_blob_pages(file, &header, &entry, &metadata, &metadata_size);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = decode_foreign_key_metadata(entry.record, metadata, metadata_size, out_metadata);
    }

    free(metadata);
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        mylite_storage_free_foreign_key_metadata(out_metadata);
    }
    return result;
}

mylite_storage_result mylite_storage_update_foreign_key_referenced_key_name(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name,
    const char *referenced_key_name
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || constraint_name == NULL ||
        constraint_name[0] == '\0' || referenced_key_name == NULL ||
        referenced_key_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry entry = {0};
    unsigned char *old_metadata = NULL;
    size_t old_metadata_size = 0U;
    mylite_storage_foreign_key_metadata decoded = {
        .size = sizeof(decoded),
    };
    unsigned char *new_metadata = NULL;
    size_t new_metadata_size = 0U;
    mylite_storage_foreign_key_definition_lengths lengths = {0};

    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_foreign_key_record(
            catalog_page,
            schema_name,
            table_name,
            constraint_name,
            &entry
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_foreign_key_blob_pages(
            file,
            &header,
            &entry,
            &old_metadata,
            &old_metadata_size
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = decode_foreign_key_metadata(
            entry.record,
            old_metadata,
            old_metadata_size,
            &decoded
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        const mylite_storage_foreign_key_definition definition = {
            .size = sizeof(definition),
            .schema_name = decoded.schema_name,
            .table_name = decoded.table_name,
            .constraint_name = decoded.constraint_name,
            .referenced_schema_name = decoded.referenced_schema_name,
            .referenced_table_name = decoded.referenced_table_name,
            .referenced_key_name = referenced_key_name,
            .foreign_column_names = (const char *const *)decoded.foreign_column_names,
            .referenced_column_names = (const char *const *)decoded.referenced_column_names,
            .column_count = decoded.column_count,
            .update_action = decoded.update_action,
            .delete_action = decoded.delete_action,
            .match_option = decoded.match_option,
            .nullable_column_bitmap = decoded.nullable_column_bitmap,
            .referenced_nullable_column_bitmap = decoded.referenced_nullable_column_bitmap,
        };
        result = validate_foreign_key_definition(&definition, &lengths);
        if (result == MYLITE_STORAGE_OK) {
            result = encode_foreign_key_metadata(
                &definition,
                &lengths,
                &new_metadata,
                &new_metadata_size
            );
        }
        const size_t blob_payload_size =
            MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
        const unsigned long long blob_page_count =
            result == MYLITE_STORAGE_OK ?
                ((new_metadata_size - 1U) / blob_payload_size) + 1U :
                0ULL;
        const unsigned long long first_blob_page = header.page_count;
        if (result == MYLITE_STORAGE_OK && blob_page_count > ULLONG_MAX - header.page_count) {
            result = MYLITE_STORAGE_FULL;
        }
        if (result == MYLITE_STORAGE_OK) {
            result = remove_foreign_key_record(
                catalog_page,
                schema_name,
                table_name,
                constraint_name
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = append_foreign_key_record(
                catalog_page,
                &definition,
                &lengths,
                first_blob_page,
                new_metadata_size,
                entry.table_id
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = begin_write_journal(file, filename, &header, 1);
        }
        if (result == MYLITE_STORAGE_OK) {
            result = write_foreign_key_blob_pages(
                file,
                first_blob_page,
                new_metadata,
                new_metadata_size
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            ++header.catalog_generation;
            header.page_count += blob_page_count;
            put_u64_le(
                catalog_page,
                MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
                header.catalog_generation
            );
            update_catalog_checksum(catalog_page);

            unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
            encode_header_page(header_page, &header);

            result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
            if (result == MYLITE_STORAGE_OK) {
                result = write_page_at(
                    file,
                    MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                    header.page_size,
                    header_page
                );
            }
            if (result == MYLITE_STORAGE_OK) {
                result = finish_write_journal(file, filename);
            }
        }
    }

    free(old_metadata);
    free(new_metadata);
    mylite_storage_free_foreign_key_metadata(&decoded);
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_drop_foreign_key_definition(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || constraint_name == NULL ||
        constraint_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_foreign_key_record(catalog_page, schema_name, table_name, constraint_name);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_list_foreign_keys(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    mylite_storage_foreign_key_callback callback,
    void *ctx
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || callback == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }

    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count =
        result == MYLITE_STORAGE_OK ? catalog_record_count(catalog_page) : 0ULL;
    for (unsigned long long i = 0ULL; result == MYLITE_STORAGE_OK && i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_is_foreign_key(record) &&
            record_matches_table(record, schema_name, table_name)) {
            mylite_storage_catalog_entry entry = {
                .record = record,
                .table_id = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET),
                .definition_root_page =
                    get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET),
                .definition_size =
                    get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
            };
            unsigned char *metadata = NULL;
            size_t metadata_size = 0U;
            mylite_storage_foreign_key_metadata decoded = {
                .size = sizeof(decoded),
            };
            result = read_foreign_key_blob_pages(file, &header, &entry, &metadata, &metadata_size);
            if (result == MYLITE_STORAGE_OK) {
                result = decode_foreign_key_metadata(record, metadata, metadata_size, &decoded);
            }
            free(metadata);
            if (result == MYLITE_STORAGE_OK && callback(ctx, &decoded) != 0) {
                result = MYLITE_STORAGE_ERROR;
            }
            mylite_storage_free_foreign_key_metadata(&decoded);
        }
        offset += record_size;
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_list_parent_foreign_keys(
    const char *filename,
    const char *referenced_schema_name,
    const char *referenced_table_name,
    mylite_storage_foreign_key_callback callback,
    void *ctx
) {
    if (filename == NULL || filename[0] == '\0' || referenced_schema_name == NULL ||
        referenced_schema_name[0] == '\0' || referenced_table_name == NULL ||
        referenced_table_name[0] == '\0' || callback == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }

    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count =
        result == MYLITE_STORAGE_OK ? catalog_record_count(catalog_page) : 0ULL;
    for (unsigned long long i = 0ULL; result == MYLITE_STORAGE_OK && i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_is_foreign_key(record) && record_matches_foreign_key_parent(
                                                 record,
                                                 referenced_schema_name,
                                                 referenced_table_name
                                             )) {
            mylite_storage_catalog_entry entry = {
                .record = record,
                .table_id = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET),
                .definition_root_page =
                    get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET),
                .definition_size =
                    get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
            };
            unsigned char *metadata = NULL;
            size_t metadata_size = 0U;
            mylite_storage_foreign_key_metadata decoded = {
                .size = sizeof(decoded),
            };
            result = read_foreign_key_blob_pages(file, &header, &entry, &metadata, &metadata_size);
            if (result == MYLITE_STORAGE_OK) {
                result = decode_foreign_key_metadata(record, metadata, metadata_size, &decoded);
            }
            free(metadata);
            if (result == MYLITE_STORAGE_OK && callback(ctx, &decoded) != 0) {
                result = MYLITE_STORAGE_ERROR;
            }
            mylite_storage_free_foreign_key_metadata(&decoded);
        }
        offset += record_size;
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

mylite_storage_result mylite_storage_read_table_definition(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned char **out_definition,
    size_t *out_definition_size
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_definition == NULL ||
        out_definition_size == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_definition = NULL;
    *out_definition_size = 0U;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result =
            read_definition_blob_pages(file, &header, &entry, out_definition, out_definition_size);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        free(*out_definition);
        *out_definition = NULL;
        *out_definition_size = 0U;
    }
    return result;
}

mylite_storage_result mylite_storage_read_table_metadata(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    mylite_storage_table_metadata *out_metadata
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_metadata == NULL ||
        out_metadata->size < sizeof(*out_metadata)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_metadata = (mylite_storage_table_metadata){
        .size = sizeof(*out_metadata),
    };

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_table_metadata_from_record(entry.record, out_metadata);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        free(out_metadata->requested_engine_name);
        free(out_metadata->effective_engine_name);
        *out_metadata = (mylite_storage_table_metadata){
            .size = sizeof(*out_metadata),
        };
    }
    return result;
}

mylite_storage_result mylite_storage_table_exists(
    const char *filename,
    const char *schema_name,
    const char *table_name
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, NULL);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_store_index_root(
    const char *filename,
    const mylite_storage_index_root_definition *definition
) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    mylite_storage_index_root_definition_lengths lengths = {0};
    mylite_storage_result result = validate_index_root_definition(definition, &lengths);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry table_entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(
            catalog_page,
            definition->schema_name,
            definition->table_name,
            &table_entry
        );
    }
    if (result == MYLITE_STORAGE_OK && (definition->root_page <= header.catalog_root_page ||
                                        definition->root_page >= header.page_count)) {
        result = MYLITE_STORAGE_MISUSE;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_index_root_record(
            catalog_page,
            definition->schema_name,
            definition->table_name,
            table_entry.table_id,
            definition->index_number
        );
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = append_index_root_record(catalog_page, definition, &lengths, table_entry.table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_read_index_root(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    mylite_storage_index_root_metadata *out_metadata
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_metadata == NULL ||
        out_metadata->size < sizeof(*out_metadata)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_metadata = (mylite_storage_index_root_metadata){
        .size = sizeof(*out_metadata),
    };

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry table_entry = {0};
    mylite_storage_catalog_entry entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &table_entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_index_root_record(
            catalog_page,
            schema_name,
            table_name,
            table_entry.table_id,
            index_number,
            &entry
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        *out_metadata = (mylite_storage_index_root_metadata){
            .size = sizeof(*out_metadata),
            .root_page = entry.definition_root_page,
            .entry_count = entry.definition_size,
        };
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_metadata = (mylite_storage_index_root_metadata){
            .size = sizeof(*out_metadata),
        };
    }
    return result;
}

mylite_storage_result mylite_storage_drop_index_root(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry table_entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &table_entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_index_root_record(
            catalog_page,
            schema_name,
            table_name,
            table_entry.table_id,
            index_number
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_rebuild_index_leaf(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry table_entry = {0};
    mylite_storage_index_entryset entryset = {
        .size = sizeof(entryset),
    };
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &table_entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result =
            read_live_index_entries(file, &header, table_entry.table_id, index_number, &entryset);
    }
    const unsigned long long root_page = header.page_count;
    unsigned char *leaf_pages = NULL;
    size_t leaf_page_count = 0U;
    if (result == MYLITE_STORAGE_OK) {
        result = prepare_index_leaf_pages(
            &leaf_pages,
            &leaf_page_count,
            root_page,
            table_entry.table_id,
            index_number,
            &entryset
        );
    }
    if (result == MYLITE_STORAGE_OK &&
        (unsigned long long)leaf_page_count > ULLONG_MAX - header.page_count) {
        result = MYLITE_STORAGE_FULL;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_index_root_record(
            catalog_page,
            schema_name,
            table_name,
            table_entry.table_id,
            index_number
        );
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        const mylite_storage_index_root_definition definition = {
            .size = sizeof(definition),
            .schema_name = schema_name,
            .table_name = table_name,
            .index_number = index_number,
            .root_page = root_page,
            .entry_count = (unsigned long long)entryset.entry_count,
        };
        const mylite_storage_index_root_definition_lengths lengths = {
            .schema_name_size = strlen(schema_name),
            .table_name_size = strlen(table_name),
        };
        const size_t record_size = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE +
                                   lengths.schema_name_size + lengths.table_name_size;
        const size_t used_bytes = catalog_used_bytes(catalog_page);
        if (record_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - used_bytes ||
            MYLITE_STORAGE_FORMAT_PAGE_SIZE - used_bytes - record_size <
                MYLITE_STORAGE_INDEX_ROOT_CATALOG_RESERVE_BYTES) {
            result = MYLITE_STORAGE_FULL;
        } else {
            result =
                append_index_root_record(catalog_page, &definition, &lengths, table_entry.table_id);
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < leaf_page_count; ++i) {
        result = write_page_at(
            file,
            root_page + (unsigned long long)i,
            header.page_size,
            leaf_pages + (i * MYLITE_STORAGE_FORMAT_PAGE_SIZE)
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        header.page_count = root_page + (unsigned long long)leaf_page_count;
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }

    mylite_storage_free_index_entryset(&entryset);
    free(leaf_pages);
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_drop_table(
    const char *filename,
    const char *schema_name,
    const char *table_name
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry table_entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &table_entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_child_foreign_key_records(catalog_page, schema_name, table_name);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_parent_foreign_key_record(catalog_page, schema_name, table_name);
        if (result == MYLITE_STORAGE_OK) {
            result = MYLITE_STORAGE_ERROR;
        } else if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_table_index_root_records(
            catalog_page,
            schema_name,
            table_name,
            table_entry.table_id
        );
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_table_record(catalog_page, schema_name, table_name);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_rename_table(
    const char *filename,
    const char *old_schema_name,
    const char *old_table_name,
    const char *new_schema_name,
    const char *new_table_name
) {
    return rename_table(
        filename,
        old_schema_name,
        old_table_name,
        new_schema_name,
        new_table_name,
        0
    );
}

mylite_storage_result mylite_storage_rename_table_for_rebuild_backup(
    const char *filename,
    const char *old_schema_name,
    const char *old_table_name,
    const char *new_schema_name,
    const char *new_table_name
) {
    return rename_table(
        filename,
        old_schema_name,
        old_table_name,
        new_schema_name,
        new_table_name,
        1
    );
}

static mylite_storage_result rename_table(
    const char *filename,
    const char *old_schema_name,
    const char *old_table_name,
    const char *new_schema_name,
    const char *new_table_name,
    int preserve_foreign_keys
) {
    if (filename == NULL || filename[0] == '\0' || old_schema_name == NULL ||
        old_schema_name[0] == '\0' || old_table_name == NULL || old_table_name[0] == '\0' ||
        new_schema_name == NULL || new_schema_name[0] == '\0' || new_table_name == NULL ||
        new_table_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = rename_table_record(
            catalog_page,
            (mylite_storage_table_identity){
                .schema_name = old_schema_name,
                .table_name = old_table_name,
            },
            (mylite_storage_table_identity){
                .schema_name = new_schema_name,
                .table_name = new_table_name,
            },
            preserve_foreign_keys
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_append_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned char *row,
    size_t row_size
) {
    return mylite_storage_append_row_with_index_entries(
        filename,
        schema_name,
        table_name,
        row,
        row_size,
        NULL,
        0U,
        NULL
    );
}

mylite_storage_result mylite_storage_append_row_with_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    unsigned long long *out_row_id
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || row == NULL || row_size == 0U) {
        return MYLITE_STORAGE_MISUSE;
    }
    if (row_size > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    if (out_row_id != NULL) {
        *out_row_id = 0ULL;
    }
    mylite_storage_result result = validate_index_entries(index_entries, index_entry_count);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    mylite_storage_row_write_position position = {0};
    if (result == MYLITE_STORAGE_OK) {
        seed_active_live_row_id_cache(filename, &header, table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 0);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = write_row_payload_pages(file, &header, table_id, row, row_size, &position);
    }
    unsigned long long next_page_id = position.next_page_id;
    if (result == MYLITE_STORAGE_OK) {
        result = write_index_entry_pages(
            file,
            &header,
            (mylite_storage_index_entry_write){
                .first_page_id = next_page_id,
                .table_id = table_id,
                .row_id = position.row_page_id,
                .index_entries = index_entries,
                .index_entry_count = index_entry_count,
            },
            &next_page_id
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        header.page_count = next_page_id;
        result = publish_header(file, &header);
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = append_active_exact_index_cache_entries(
            filename,
            table_id,
            position.row_page_id,
            index_entries,
            index_entry_count
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        (void)mark_active_validated_live_row(file, &header, table_id, position.row_page_id);
        append_active_live_row_id(filename, &header, table_id, position.row_page_id);
        retarget_durable_caches_after_table_mutation(filename, &header, table_id);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result == MYLITE_STORAGE_OK && out_row_id != NULL) {
        *out_row_id = position.row_page_id;
    }
    return result;
}

mylite_storage_result mylite_storage_read_rows(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    mylite_storage_rowset *out_rows
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_rows == NULL ||
        out_rows->size < sizeof(*out_rows)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_rows = (mylite_storage_rowset){
        .size = sizeof(*out_rows),
    };

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    mylite_storage_row_id_list live_rows = {0};
    int used_live_row_cache = 0;
    if (result == MYLITE_STORAGE_OK) {
        result = copy_cached_durable_live_row_ids(
            filename,
            &header,
            table_id,
            &live_rows,
            &used_live_row_cache
        );
    }
    if (result == MYLITE_STORAGE_OK && !used_live_row_cache) {
        result = collect_live_table_row_ids(file, &header, table_id, &live_rows);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_row_ids_into_rowset(file, &header, filename, table_id, &live_rows, out_rows);
    }
    if (result == MYLITE_STORAGE_OK && !used_live_row_cache) {
        store_durable_live_row_ids(filename, &header, table_id, &live_rows);
    }
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < out_rows->row_count; ++i) {
        (void)mark_active_validated_live_row(file, &header, table_id, out_rows->row_ids[i]);
    }
    free(live_rows.row_ids);

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        mylite_storage_free_rowset(out_rows);
    }
    return result;
}

mylite_storage_result mylite_storage_count_rows(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_row_count
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_row_count == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_row_count = 0ULL;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    mylite_storage_row_id_list live_rows = {0};
    int used_live_row_cache = 0;
    if (result == MYLITE_STORAGE_OK) {
        result = copy_cached_durable_live_row_ids(
            filename,
            &header,
            table_id,
            &live_rows,
            &used_live_row_cache
        );
    }
    if (result == MYLITE_STORAGE_OK && !used_live_row_cache) {
        result = collect_live_table_row_ids(file, &header, table_id, &live_rows);
    }
    if (result == MYLITE_STORAGE_OK && !used_live_row_cache) {
        result = validate_row_id_payloads(file, &header, table_id, &live_rows);
    }
    if (result == MYLITE_STORAGE_OK) {
        *out_row_count = (unsigned long long)live_rows.count;
        if (!used_live_row_cache) {
            store_durable_live_row_ids(filename, &header, table_id, &live_rows);
        }
    }
    free(live_rows.row_ids);

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_row_count = 0ULL;
    }
    return result;
}

mylite_storage_result mylite_storage_read_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    unsigned char **out_row,
    size_t *out_row_size
) {
    return read_row_payload(filename, schema_name, table_name, row_id, 1, out_row, out_row_size);
}

mylite_storage_result mylite_storage_read_indexed_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    unsigned char **out_row,
    size_t *out_row_size
) {
    return read_row_payload(filename, schema_name, table_name, row_id, 0, out_row, out_row_size);
}

mylite_storage_result mylite_storage_read_indexed_rows(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned long long *row_ids,
    size_t row_id_count,
    mylite_storage_rowset *out_rows
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_rows == NULL ||
        (row_id_count > 0U && row_ids == NULL)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_rows = (mylite_storage_rowset){
        .size = sizeof(*out_rows),
    };
    if (row_id_count == 0U) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_rowset_builder builder = {0};
    mylite_storage_result result = initialize_rowset_builder(&builder, out_rows, row_id_count);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    mylite_storage_row_payload_cache *row_payload_cache = NULL;
    unsigned long long row_payload_cache_generation = 0ULL;
    mylite_storage_statement *active_row_payload_statement = NULL;
    mylite_storage_row_payload_cache *active_row_payload_cache = NULL;
    if (result == MYLITE_STORAGE_OK) {
        row_payload_cache = durable_row_payload_cache_for(filename, &header, table_id);
        row_payload_cache_generation = durable_row_payload_caches_generation;
        active_row_payload_statement = active_row_payload_cache_statement_for(filename);
        if (active_row_payload_statement != NULL) {
            active_row_payload_cache = find_active_row_payload_cache(
                &active_row_payload_statement->row_payload_caches,
                filename,
                &header,
                table_id
            );
        }
    }
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < row_id_count; ++i) {
        const unsigned long long row_id = row_ids[i];
        int used_cache = 0;
        if (active_row_payload_cache != NULL) {
            result = append_cached_row_payload_to_builder(
                active_row_payload_cache,
                row_id,
                &builder,
                &used_cache
            );
            if (result != MYLITE_STORAGE_OK || used_cache) {
                continue;
            }
        }

        result = append_cached_durable_row_payload_to_builder(
            filename,
            &header,
            table_id,
            &row_payload_cache,
            &row_payload_cache_generation,
            row_id,
            &builder,
            &used_cache
        );
        if (result != MYLITE_STORAGE_OK || used_cache) {
            continue;
        }

        mylite_storage_row_page row_page = {0};
        unsigned char row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        if (row_id <= header.catalog_root_page || row_id >= header.page_count) {
            result = MYLITE_STORAGE_NOTFOUND;
        }
        if (result == MYLITE_STORAGE_OK) {
            result = read_row_page(file, &header, row_id, row_buffer, &row_page);
        }
        if (result == MYLITE_STORAGE_OK && row_page.table_id != table_id) {
            result = MYLITE_STORAGE_NOTFOUND;
        }
        if (result == MYLITE_STORAGE_OK) {
            result =
                append_row_to_rowset_builder(&builder, row_id, row_page.payload, row_page.row_size);
            if (result == MYLITE_STORAGE_OK) {
                store_active_row_payload(
                    filename,
                    &header,
                    table_id,
                    row_id,
                    row_page.payload,
                    row_page.row_size
                );
                if (active_row_payload_statement != NULL) {
                    active_row_payload_cache = find_active_row_payload_cache(
                        &active_row_payload_statement->row_payload_caches,
                        filename,
                        &header,
                        table_id
                    );
                }
                store_durable_row_payload(
                    filename,
                    &header,
                    table_id,
                    &row_payload_cache,
                    &row_payload_cache_generation,
                    row_id,
                    row_page.payload,
                    row_page.row_size
                );
            }
        }
        free(row_page.owned_payload);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        mylite_storage_free_rowset(out_rows);
    }
    return result;
}

static mylite_storage_result read_row_payload(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    int validate_visibility,
    unsigned char **out_row,
    size_t *out_row_size
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || row_id == 0ULL || out_row == NULL ||
        out_row_size == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_row = NULL;
    *out_row_size = 0U;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    mylite_storage_row_page row_page = {0};
    unsigned char row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    int row_hidden = 0;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK &&
        (row_id <= header.catalog_root_page || row_id >= header.page_count)) {
        result = MYLITE_STORAGE_NOTFOUND;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_row_page(file, &header, row_id, row_buffer, &row_page);
    }
    if (result == MYLITE_STORAGE_OK && row_page.table_id != table_id) {
        result = MYLITE_STORAGE_NOTFOUND;
    }
    if (result == MYLITE_STORAGE_OK && validate_visibility) {
        const unsigned long long first_state_page_id =
            row_id == ULLONG_MAX ? header.page_count : row_id + 1ULL;
        result =
            row_is_hidden_after(file, &header, table_id, row_id, first_state_page_id, &row_hidden);
    }
    if (result == MYLITE_STORAGE_OK && row_hidden) {
        result = MYLITE_STORAGE_NOTFOUND;
    }
    if (result == MYLITE_STORAGE_OK && validate_visibility) {
        (void)mark_active_validated_live_row(file, &header, table_id, row_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        unsigned char *row = (unsigned char *)malloc(row_page.row_size);
        if (row == NULL) {
            result = MYLITE_STORAGE_NOMEM;
        } else {
            memcpy(row, row_page.payload, row_page.row_size);
            *out_row = row;
            *out_row_size = row_page.row_size;
        }
    }

    free(row_page.owned_payload);
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        free(*out_row);
        *out_row = NULL;
        *out_row_size = 0U;
    }
    return result;
}

static mylite_storage_result read_indexed_row_payload_from_open_file(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    unsigned char **out_row,
    size_t *inout_row_capacity,
    size_t *out_row_size
) {
    if (inout_row_capacity == NULL) {
        *out_row = NULL;
    }
    *out_row_size = 0U;

    const mylite_storage_row_payload_cache_entry *active_entry =
        active_row_payload_cache_entry_for(filename, header, table_id, row_id);
    if (active_entry != NULL) {
        return copy_cached_row_payload(active_entry, out_row, inout_row_capacity, out_row_size);
    }

    mylite_storage_row_payload_cache *row_payload_cache =
        durable_row_payload_cache_for(filename, header, table_id);
    unsigned long long row_payload_cache_generation = durable_row_payload_caches_generation;
    if (row_payload_cache != NULL) {
        const mylite_storage_row_payload_cache_entry *entry =
            find_row_payload_cache_entry(row_payload_cache, row_id);
        if (entry != NULL) {
            return copy_cached_row_payload(entry, out_row, inout_row_capacity, out_row_size);
        }
    }

    mylite_storage_result result = MYLITE_STORAGE_OK;
    mylite_storage_row_page row_page = {0};
    unsigned char row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    if (row_id <= header->catalog_root_page || row_id >= header->page_count) {
        result = MYLITE_STORAGE_NOTFOUND;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_row_page(file, header, row_id, row_buffer, &row_page);
    }
    if (result == MYLITE_STORAGE_OK && row_page.table_id != table_id) {
        result = MYLITE_STORAGE_NOTFOUND;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = copy_row_payload_to_output(
            row_page.payload,
            row_page.row_size,
            out_row,
            inout_row_capacity,
            out_row_size
        );
        if (result == MYLITE_STORAGE_OK) {
            store_active_row_payload(
                filename,
                header,
                table_id,
                row_id,
                row_page.payload,
                row_page.row_size
            );
            store_durable_row_payload(
                filename,
                header,
                table_id,
                &row_payload_cache,
                &row_payload_cache_generation,
                row_id,
                row_page.payload,
                row_page.row_size
            );
        }
    }

    free(row_page.owned_payload);
    return result;
}

mylite_storage_result mylite_storage_update_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    unsigned long long *out_new_row_id
) {
    return mylite_storage_update_row_with_index_entries(
        filename,
        schema_name,
        table_name,
        row_id,
        row,
        row_size,
        NULL,
        0U,
        out_new_row_id
    );
}

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
) {
    return update_row_with_index_entries(
        filename,
        schema_name,
        table_name,
        row_id,
        row,
        row_size,
        index_entries,
        index_entry_count,
        NULL,
        out_new_row_id
    );
}

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
) {
    return update_row_with_index_entries(
        filename,
        schema_name,
        table_name,
        row_id,
        row,
        row_size,
        index_entries,
        index_entry_count,
        index_entry_changed,
        out_new_row_id
    );
}

static mylite_storage_result update_row_with_index_entries(
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
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || row_id == 0ULL || row == NULL ||
        row_size == 0U || out_new_row_id == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }
    if (row_size > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    *out_new_row_id = 0ULL;
    mylite_storage_result result = validate_index_entries(index_entries, index_entry_count);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    mylite_storage_row_page old_row_page = {0};
    unsigned char old_row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_row_write_position position = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        seed_active_live_row_id_cache(filename, &header, table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_direct_live_row(
            file,
            &header,
            table_id,
            row_id,
            old_row_buffer,
            &old_row_page
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 0);
    }
    unsigned long long next_page_id = 0ULL;
    int used_inline_update_pages = 0;
    int used_active_update_rewrite = 0;
    if (result == MYLITE_STORAGE_OK) {
        result = rewrite_active_update_pages(
            file,
            &header,
            table_id,
            row_id,
            row,
            row_size,
            index_entries,
            index_entry_count,
            index_entry_changed,
            &used_active_update_rewrite
        );
        if (result == MYLITE_STORAGE_OK && used_active_update_rewrite) {
            position = (mylite_storage_row_write_position){
                .row_page_id = row_id,
                .next_page_id = header.page_count,
            };
            next_page_id = header.page_count;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        if (!used_active_update_rewrite) {
            result = write_inline_update_pages(
                file,
                &header,
                table_id,
                row_id,
                row,
                row_size,
                index_entries,
                index_entry_count,
                index_entry_changed,
                &position,
                &next_page_id,
                &used_inline_update_pages
            );
        }
    }
    if (result == MYLITE_STORAGE_OK && !used_active_update_rewrite && !used_inline_update_pages) {
        result = write_row_payload_pages(file, &header, table_id, row, row_size, &position);
        if (result == MYLITE_STORAGE_OK && position.next_page_id == ULLONG_MAX) {
            result = MYLITE_STORAGE_FULL;
        }
        next_page_id = position.next_page_id;
        if (result == MYLITE_STORAGE_OK) {
            const mylite_storage_row_state_page row_state = {
                .table_id = table_id,
                .source_row_id = row_id,
                .replacement_row_id = position.row_page_id,
                .state_kind = MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE,
            };
            unsigned char state_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
            encode_row_state_page(state_page, next_page_id, &row_state);
            result = write_page_at(file, next_page_id, header.page_size, state_page);
        }
        if (result == MYLITE_STORAGE_OK) {
            ++next_page_id;
            result = write_index_entry_pages(
                file,
                &header,
                (mylite_storage_index_entry_write){
                    .first_page_id = next_page_id,
                    .table_id = table_id,
                    .row_id = position.row_page_id,
                    .index_entries = index_entries,
                    .index_entry_count = index_entry_count,
                    .index_entry_changed = index_entry_changed,
                },
                &next_page_id
            );
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        header.page_count = next_page_id;
        result = publish_header(file, &header);
        if (result == MYLITE_STORAGE_OK) {
            result = finish_write_journal(file, filename);
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        *out_new_row_id = position.row_page_id;
        replace_active_exact_index_cache_entries(
            filename,
            table_id,
            row_id,
            position.row_page_id,
            index_entries,
            index_entry_count
        );
        replace_active_live_row(file, &header, table_id, row_id, position.row_page_id);
        replace_active_live_row_id(filename, &header, table_id, row_id, position.row_page_id);
        replace_active_row_payload(
            filename,
            &header,
            table_id,
            row_id,
            position.row_page_id,
            row,
            row_size
        );
        retarget_durable_caches_after_table_mutation(filename, &header, table_id);
    }

    free(old_row_page.owned_payload);
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_new_row_id = 0ULL;
    }
    return result;
}

mylite_storage_result mylite_storage_delete_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || row_id == 0ULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    mylite_storage_row_page row_page = {0};
    unsigned char row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        seed_active_live_row_id_cache(filename, &header, table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_direct_live_row(file, &header, table_id, row_id, row_buffer, &row_page);
    }
    if (result == MYLITE_STORAGE_OK && header.page_count == ULLONG_MAX) {
        result = MYLITE_STORAGE_FULL;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_write_journal(file, filename, &header, 0);
    }
    if (result == MYLITE_STORAGE_OK) {
        const unsigned long long state_page_id = header.page_count;
        const mylite_storage_row_state_page row_state = {
            .table_id = table_id,
            .source_row_id = row_id,
            .replacement_row_id = 0ULL,
            .state_kind = MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_DELETE,
        };
        unsigned char state_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_row_state_page(state_page, state_page_id, &row_state);
        result = write_page_at(file, state_page_id, header.page_size, state_page);
        if (result == MYLITE_STORAGE_OK) {
            ++header.page_count;
            result = publish_header(file, &header);
            if (result == MYLITE_STORAGE_OK) {
                result = finish_write_journal(file, filename);
            }
        }
    }

    free(row_page.owned_payload);
    if (result == MYLITE_STORAGE_OK) {
        replace_active_exact_index_cache_entries(filename, table_id, row_id, 0ULL, NULL, 0U);
        replace_active_live_row(file, &header, table_id, row_id, 0ULL);
        remove_active_live_row_id(filename, &header, table_id, row_id);
        remove_active_row_payload(filename, &header, table_id, row_id);
        retarget_durable_caches_after_table_mutation(filename, &header, table_id);
    }
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_truncate_table(
    const char *filename,
    const char *schema_name,
    const char *table_name
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    mylite_storage_row_id_list live_rows = {0};
    unsigned long long current_next_value = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = scan_table_row_pages(file, &header, table_id, append_live_row_id, &live_rows);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = latest_auto_increment_value(file, &header, table_id, &current_next_value);
    }

    const int reset_auto_increment = current_next_value != 1ULL;
    if (result == MYLITE_STORAGE_OK &&
        truncate_needs_publication(&live_rows, reset_auto_increment)) {
        result = write_truncate_publication(
            file,
            filename,
            &header,
            table_id,
            &live_rows,
            reset_auto_increment
        );
    }

    free(live_rows.row_ids);
    if (result == MYLITE_STORAGE_OK) {
        invalidate_exact_index_caches(filename);
    }
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_read_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    mylite_storage_index_entryset *out_entries
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_entries == NULL ||
        out_entries->size < sizeof(*out_entries)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_entries = (mylite_storage_index_entryset){
        .size = sizeof(*out_entries),
    };

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_live_index_entries(file, &header, table_id, index_number, out_entries);
    }
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < out_entries->entry_count; ++i) {
        (void)mark_active_live_row(file, &header, table_id, out_entries->row_ids[i]);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        mylite_storage_free_index_entryset(out_entries);
    }
    return result;
}

mylite_storage_result mylite_storage_find_index_entry(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || key == NULL || key_size == 0U ||
        out_row_id == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_row_id = 0ULL;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry table_entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        const int used_cached_table_entry =
            find_active_table_entry_cache(filename, &header, schema_name, table_name, &table_entry);
        if (!used_cached_table_entry) {
            result = read_catalog_root(file, &header, catalog_page);
            if (result == MYLITE_STORAGE_OK) {
                result = find_table_record(catalog_page, schema_name, table_name, &table_entry);
            }
            if (result == MYLITE_STORAGE_OK) {
                store_active_table_entry_cache(
                    filename,
                    &header,
                    schema_name,
                    table_name,
                    &table_entry
                );
            }
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_exact_index_row_id(
            file,
            filename,
            &header,
            table_entry.record != NULL ? catalog_page : NULL,
            &table_entry,
            schema_name,
            table_name,
            index_number,
            key,
            key_size,
            out_row_id
        );
    }
    if (result == MYLITE_STORAGE_OK && *out_row_id != 0ULL) {
        (void)mark_active_live_row(file, &header, table_entry.table_id, *out_row_id);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_row_id = 0ULL;
        return result;
    }
    return *out_row_id != 0ULL ? MYLITE_STORAGE_OK : MYLITE_STORAGE_NOTFOUND;
}

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
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || key == NULL || key_size == 0U ||
        out_row_id == NULL || out_row == NULL || out_row_size == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    return find_indexed_row_payload(
        filename,
        schema_name,
        table_name,
        index_number,
        key,
        key_size,
        out_row_id,
        out_row,
        NULL,
        out_row_size
    );
}

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
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || key == NULL || key_size == 0U ||
        out_row_id == NULL || inout_row == NULL || inout_row_capacity == NULL ||
        out_row_size == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    return find_indexed_row_payload(
        filename,
        schema_name,
        table_name,
        index_number,
        key,
        key_size,
        out_row_id,
        inout_row,
        inout_row_capacity,
        out_row_size
    );
}

static mylite_storage_result find_indexed_row_payload(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    unsigned char **out_row,
    size_t *inout_row_capacity,
    size_t *out_row_size
) {
    *out_row_id = 0ULL;
    if (inout_row_capacity == NULL) {
        *out_row = NULL;
    }
    *out_row_size = 0U;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry table_entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        const int used_cached_table_entry =
            find_active_table_entry_cache(filename, &header, schema_name, table_name, &table_entry);
        if (!used_cached_table_entry) {
            result = read_catalog_root(file, &header, catalog_page);
            if (result == MYLITE_STORAGE_OK) {
                result = find_table_record(catalog_page, schema_name, table_name, &table_entry);
            }
            if (result == MYLITE_STORAGE_OK) {
                store_active_table_entry_cache(
                    filename,
                    &header,
                    schema_name,
                    table_name,
                    &table_entry
                );
            }
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_exact_index_row_id(
            file,
            filename,
            &header,
            table_entry.record != NULL ? catalog_page : NULL,
            &table_entry,
            schema_name,
            table_name,
            index_number,
            key,
            key_size,
            out_row_id
        );
    }
    if (result == MYLITE_STORAGE_OK && *out_row_id != 0ULL) {
        (void)mark_active_live_row(file, &header, table_entry.table_id, *out_row_id);
        result = read_indexed_row_payload_from_open_file(
            file,
            filename,
            &header,
            table_entry.table_id,
            *out_row_id,
            out_row,
            inout_row_capacity,
            out_row_size
        );
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_CORRUPT;
        }
        if (result == MYLITE_STORAGE_OK) {
            (void)mark_active_validated_live_row(file, &header, table_entry.table_id, *out_row_id);
        }
    }
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        if (inout_row_capacity == NULL) {
            free(*out_row);
            *out_row = NULL;
        }
        *out_row_id = 0ULL;
        *out_row_size = 0U;
        return result;
    }
    return *out_row_id != 0ULL ? MYLITE_STORAGE_OK : MYLITE_STORAGE_NOTFOUND;
}

mylite_storage_result mylite_storage_read_exact_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || key == NULL || key_size == 0U ||
        out_entries == NULL || out_entries->size < sizeof(*out_entries)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_entries = (mylite_storage_index_entryset){
        .size = sizeof(*out_entries),
    };

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry table_entry = {0};
    int used_leaf = 0;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &table_entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_index_leaf_exact_entries(
            file,
            filename,
            &header,
            catalog_page,
            table_entry.table_id,
            schema_name,
            table_name,
            index_number,
            key,
            key_size,
            out_entries,
            &used_leaf
        );
    }
    if (result == MYLITE_STORAGE_OK && !used_leaf) {
        int used_cache = 0;
        result = append_cached_durable_exact_index_entries(
            file,
            &header,
            filename,
            table_entry.table_id,
            index_number,
            key,
            key_size,
            out_entries,
            &used_cache
        );
        if (result == MYLITE_STORAGE_OK && !used_cache) {
            result = scan_exact_index_entries(
                file,
                &header,
                table_entry.table_id,
                index_number,
                key,
                key_size,
                out_entries
            );
        }
    }

    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < out_entries->entry_count; ++i) {
        (void)mark_active_live_row(file, &header, table_entry.table_id, out_entries->row_ids[i]);
    }
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        mylite_storage_free_index_entryset(out_entries);
    }
    return result;
}

mylite_storage_result mylite_storage_index_prefix_exists(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    int *out_exists
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || key_prefix == NULL ||
        key_prefix_size == 0U || out_exists == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_exists = 0;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_index_prefix_match_list matches = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         result == MYLITE_STORAGE_OK && page_id < header.page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        result = read_page_at(file, page_id, header.page_size, page);
        if (result != MYLITE_STORAGE_OK) {
            break;
        }

        if (is_index_entry_page(page)) {
            mylite_storage_index_entry_page entry_page = {0};
            result = decode_index_entry_page(&header, page_id, page, &entry_page);
            if (result == MYLITE_STORAGE_OK && entry_page.table_id == table_id &&
                find_row_state_entry(&row_state_map, entry_page.row_id) == NULL) {
                remove_index_prefix_matches(&matches, entry_page.row_id, entry_page.index_number);
                if (entry_page.key_size >= key_prefix_size &&
                    memcmp(entry_page.key, key_prefix, key_prefix_size) == 0) {
                    result = append_index_prefix_match(
                        &matches,
                        entry_page.row_id,
                        entry_page.index_number
                    );
                }
            }
            continue;
        }

        if (is_row_state_page(page)) {
            mylite_storage_row_state_page row_state_page = {0};
            result = decode_row_state_page(&header, page_id, page, &row_state_page);
            if (result == MYLITE_STORAGE_OK && row_state_page.table_id == table_id) {
                result = set_row_state_entry(&row_state_map, &row_state_page);
                if (result == MYLITE_STORAGE_OK) {
                    if (row_state_page.state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE) {
                        replace_index_prefix_match_row_id(
                            &matches,
                            row_state_page.source_row_id,
                            row_state_page.replacement_row_id
                        );
                    } else {
                        remove_index_prefix_matches_by_row_id(
                            &matches,
                            row_state_page.source_row_id
                        );
                    }
                }
            }
            continue;
        }

        if (!is_exact_index_scan_skip_page(page)) {
            result = MYLITE_STORAGE_CORRUPT;
        }
    }
    if (result == MYLITE_STORAGE_OK && matches.count != 0U) {
        *out_exists = 1;
    }

    free(matches.matches);
    free_row_state_map(&row_state_map);
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_exists = 0;
    }
    return result;
}

mylite_storage_result mylite_storage_read_auto_increment(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_next_value
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_next_value == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_next_value = 0ULL;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = latest_auto_increment_value(file, &header, table_id, out_next_value);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_next_value = 0ULL;
    }
    return result;
}

mylite_storage_result mylite_storage_set_auto_increment(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long next_value
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || next_value == 0ULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    unsigned long long current_next_value = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = latest_auto_increment_value(file, &header, table_id, &current_next_value);
    }
    if (result == MYLITE_STORAGE_OK && next_value != current_next_value) {
        result = publish_auto_increment_value(file, filename, &header, table_id, next_value);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_advance_auto_increment(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long next_value
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || next_value == 0ULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    unsigned long long current_next_value = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, filename, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = latest_auto_increment_value(file, &header, table_id, &current_next_value);
    }
    if (result == MYLITE_STORAGE_OK && next_value <= current_next_value) {
        if (close_existing_file(file) != MYLITE_STORAGE_OK) {
            return MYLITE_STORAGE_IOERR;
        }
        return MYLITE_STORAGE_OK;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = publish_auto_increment_value(file, filename, &header, table_id, next_value);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_list_tables(
    const char *filename,
    const char *schema_name,
    mylite_storage_table_callback callback,
    void *ctx
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        callback == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = list_catalog_tables(catalog_page, schema_name, callback, ctx);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_list_schemas(
    const char *filename,
    mylite_storage_schema_callback callback,
    void *ctx
) {
    if (filename == NULL || filename[0] == '\0' || callback == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = list_catalog_schemas(catalog_page, callback, ctx);
    }

    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_begin_statement(
    const char *filename,
    mylite_storage_statement **out_statement
) {
    return begin_checkpoint(filename, out_statement, 0);
}

mylite_storage_result mylite_storage_begin_transaction(
    const char *filename,
    mylite_storage_statement **out_statement
) {
    return begin_checkpoint(filename, out_statement, 1);
}

mylite_storage_result mylite_storage_begin_read_statement(
    const char *filename,
    mylite_storage_statement **out_statement
) {
    if (filename == NULL || filename[0] == '\0' || out_statement == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }
    *out_statement = NULL;

    if (active_statement_for(filename) != NULL) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_statement *same_file_parent = active_read_statement_for(filename);
    mylite_storage_statement *statement =
        (mylite_storage_statement *)calloc(1U, sizeof(*statement));
    if (statement == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    statement->filename = copy_filename(filename);
    if (statement->filename == NULL) {
        free(statement);
        return MYLITE_STORAGE_NOMEM;
    }
    statement->owner = active_context_owner;
    statement->parent = active_read_statement;

    mylite_storage_result result = initialize_read_statement(statement, filename, same_file_parent);
    if (result != MYLITE_STORAGE_OK) {
        free_statement(statement);
        return result;
    }

    active_read_statement = statement;
    *out_statement = statement;
    return MYLITE_STORAGE_OK;
}

int mylite_storage_statement_active(const char *filename) {
    return active_statement_for(filename) != NULL ? 1 : 0;
}

mylite_storage_result mylite_storage_preserve_auto_increment_on_rollback(
    const char *filename
) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    mylite_storage_statement *statement = active_statement_for(filename);
    if (statement == NULL) {
        return MYLITE_STORAGE_OK;
    }

    statement->preserve_auto_increment_rollback = 1;
    return MYLITE_STORAGE_OK;
}

const void *mylite_storage_context_owner(void) {
    return active_context_owner;
}

void mylite_storage_set_context_owner(const void *owner) {
    active_context_owner = owner;
}

void mylite_storage_set_busy_timeout(unsigned milliseconds) {
    active_busy_timeout_ms = milliseconds;
}

unsigned mylite_storage_busy_timeout(void) {
    return active_busy_timeout_ms;
}

mylite_storage_result mylite_storage_commit_statement(mylite_storage_statement *statement) {
    if (statement == NULL || active_statement != statement) {
        return MYLITE_STORAGE_MISUSE;
    }

    if (statement->parent == NULL) {
        const mylite_storage_result buffer_result =
            flush_statement_append_page_buffer(statement);
        if (buffer_result != MYLITE_STORAGE_OK) {
            return buffer_result;
        }
    }
    if (statement->current_header_dirty) {
        if (statement->parent != NULL) {
            statement->parent->current_header = statement->current_header;
            statement->parent->has_current_header = 1;
            statement->parent->current_header_dirty = 1;
        } else {
            const mylite_storage_result header_result = write_statement_current_header(statement);
            if (header_result != MYLITE_STORAGE_OK) {
                return header_result;
            }
        }
    }
    if (statement->owns_recovery_journal) {
        const mylite_storage_result journal_result =
            finish_recovery_journal(statement->file, statement->filename);
        if (journal_result != MYLITE_STORAGE_OK) {
            return journal_result;
        }
    }
    if (statement->owns_transaction_journal) {
        const mylite_storage_result journal_result =
            finish_transaction_journal(statement->file, statement->filename);
        if (journal_result != MYLITE_STORAGE_OK) {
            return journal_result;
        }
    }

    const int promote_active_caches = statement->parent == NULL;
    active_statement = statement->parent;
    mylite_storage_result result = close_statement(statement);
    if (result == MYLITE_STORAGE_OK && promote_active_caches) {
        promote_statement_exact_index_caches(statement);
        promote_statement_live_row_id_caches(statement);
    }
    free_statement(statement);
    return result;
}

mylite_storage_result mylite_storage_rollback_statement(mylite_storage_statement *statement) {
    if (statement == NULL || active_statement != statement) {
        return MYLITE_STORAGE_MISUSE;
    }

    mylite_storage_autoincrement_rollback_values auto_increment_values = {0};
    mylite_storage_result result =
        collect_rollback_auto_increment_values(statement, &auto_increment_values);
    if (result != MYLITE_STORAGE_OK) {
        free_rollback_auto_increment_values(&auto_increment_values);
        return result;
    }

    result = write_page_at(
        statement->file,
        statement->header.catalog_root_page,
        statement->header.page_size,
        statement->catalog_page
    );
    if (result == MYLITE_STORAGE_OK) {
        result = write_page_at_raw(
            statement->file,
            MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
            statement->header.page_size,
            statement->header_page
        );
        if (result == MYLITE_STORAGE_OK) {
            statement->current_header = statement->header;
            statement->has_current_header = 1;
            statement->current_header_dirty = 0;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = restore_buffered_page_undos(statement);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = flush_statement_append_page_buffer_before_truncate(
            statement,
            statement->current_header.page_count
        );
        if (result == MYLITE_STORAGE_OK) {
            trim_statement_append_page_buffer(statement, statement->current_header.page_count);
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = publish_rollback_auto_increment_values(statement, &auto_increment_values);
    }
    free_rollback_auto_increment_values(&auto_increment_values);
    if (result == MYLITE_STORAGE_OK && statement->current_header_dirty) {
        result = write_statement_current_header(statement);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = truncate_file_to_header_page_count(statement->file, &statement->current_header);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = flush_file(statement->file);
    }
    if (result == MYLITE_STORAGE_OK && statement->owns_recovery_journal) {
        result = finish_recovery_journal(statement->file, statement->filename);
    }
    if (result == MYLITE_STORAGE_OK && statement->owns_transaction_journal) {
        result = finish_transaction_journal(statement->file, statement->filename);
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    if (statement->parent != NULL) {
        clear_statement_chain_exact_index_caches(statement->parent);
        clear_statement_chain_live_row_caches(statement->parent);
        clear_statement_chain_live_row_id_caches(statement->parent);
        clear_statement_chain_row_payload_caches(statement->parent);
        clear_statement_chain_buffered_update_rewrites(statement->parent);
        statement->parent->current_header = statement->current_header;
        statement->parent->has_current_header = 1;
        statement->parent->current_header_dirty = 1;
    }
    active_statement = statement->parent;
    mylite_storage_result close_result = close_statement(statement);
    free_statement(statement);
    return close_result;
}

mylite_storage_result mylite_storage_end_read_statement(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return MYLITE_STORAGE_OK;
    }
    if (active_read_statement != statement) {
        return MYLITE_STORAGE_MISUSE;
    }

    active_read_statement = statement->parent;
    mylite_storage_result result = close_statement(statement);
    free_statement(statement);
    return result;
}

static int checkpoint_preserves_auto_increment_rollback(
    const mylite_storage_statement *statement
) {
    return statement->owns_transaction_journal || statement->parent != NULL ||
           statement->preserve_auto_increment_rollback;
}

static mylite_storage_result collect_rollback_auto_increment_values(
    const mylite_storage_statement *statement,
    mylite_storage_autoincrement_rollback_values *out_values
) {
    if (!checkpoint_preserves_auto_increment_rollback(statement)) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_header current_header = {0};
    mylite_storage_result result = read_header(statement->file, &current_header);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (current_header.page_count <= statement->header.page_count) {
        return MYLITE_STORAGE_OK;
    }

    for (unsigned long long page_id = statement->header.page_count;
         result == MYLITE_STORAGE_OK && page_id < current_header.page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_autoincrement_page autoincrement_page = {0};
        result = read_autoincrement_page(
            statement->file,
            &current_header,
            page_id,
            page,
            &autoincrement_page
        );
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
            continue;
        }
        if (result == MYLITE_STORAGE_OK &&
            catalog_contains_table_id(statement->catalog_page, autoincrement_page.table_id)) {
            result = append_rollback_auto_increment_value(
                out_values,
                autoincrement_page.table_id,
                autoincrement_page.next_value
            );
        }
    }
    return result;
}

static mylite_storage_result append_rollback_auto_increment_value(
    mylite_storage_autoincrement_rollback_values *values,
    unsigned long long table_id,
    unsigned long long next_value
) {
    for (size_t i = 0U; i < values->count; ++i) {
        if (values->entries[i].table_id == table_id) {
            values->entries[i].next_value = next_value;
            return MYLITE_STORAGE_OK;
        }
    }

    if (values->count == values->capacity) {
        const size_t next_capacity = values->capacity == 0U ? 4U : values->capacity * 2U;
        if (next_capacity <= values->capacity ||
            next_capacity > SIZE_MAX / sizeof(*values->entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_autoincrement_rollback_value *entries =
            (mylite_storage_autoincrement_rollback_value *)realloc(
                values->entries,
                next_capacity * sizeof(*values->entries)
            );
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        values->entries = entries;
        values->capacity = next_capacity;
    }

    values->entries[values->count++] = (mylite_storage_autoincrement_rollback_value){
        .table_id = table_id,
        .next_value = next_value,
    };
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result publish_rollback_auto_increment_values(
    mylite_storage_statement *statement,
    const mylite_storage_autoincrement_rollback_values *values
) {
    mylite_storage_header header = statement->header;
    for (size_t i = 0U; i < values->count; ++i) {
        unsigned long long checkpoint_next_value = 0ULL;
        mylite_storage_result result = latest_auto_increment_value(
            statement->file,
            &statement->header,
            values->entries[i].table_id,
            &checkpoint_next_value
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (values->entries[i].next_value <= checkpoint_next_value) {
            continue;
        }

        result = publish_auto_increment_value(
            statement->file,
            statement->filename,
            &header,
            values->entries[i].table_id,
            values->entries[i].next_value
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }
    return MYLITE_STORAGE_OK;
}

static int catalog_contains_table_id(
    const unsigned char *catalog_page,
    unsigned long long table_id
) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        if (record_is_table(record) &&
            get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET) == table_id) {
            return 1;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return 0;
}

static void free_rollback_auto_increment_values(
    mylite_storage_autoincrement_rollback_values *values
) {
    free(values->entries);
    values->entries = NULL;
    values->count = 0U;
    values->capacity = 0U;
}

static mylite_storage_result begin_checkpoint(
    const char *filename,
    mylite_storage_statement **out_statement,
    int durable_transaction
) {
    if (filename == NULL || filename[0] == '\0' || out_statement == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }
    *out_statement = NULL;
    mylite_storage_statement *parent = active_statement_for(filename);
    if (active_statement != NULL && parent == NULL) {
        return active_statement_for_any_owner(filename) != NULL ? MYLITE_STORAGE_BUSY
                                                               : MYLITE_STORAGE_MISUSE;
    }
    if (durable_transaction && parent != NULL) {
        return MYLITE_STORAGE_MISUSE;
    }
    mylite_storage_statement *statement =
        (mylite_storage_statement *)calloc(1U, sizeof(*statement));
    if (statement == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    statement->filename = copy_filename(filename);
    if (statement->filename == NULL) {
        free(statement);
        return MYLITE_STORAGE_NOMEM;
    }
    statement->owner = active_context_owner;

    mylite_storage_result result = initialize_checkpoint_statement(statement, filename, parent);
    if (result == MYLITE_STORAGE_OK && parent == NULL) {
        result = read_checkpoint_snapshot(statement);
    }
    if (result == MYLITE_STORAGE_OK && durable_transaction) {
        result = begin_transaction_journal(statement->file, filename, &statement->header);
        if (result == MYLITE_STORAGE_OK) {
            statement->owns_transaction_journal = 1;
        }
    }
    if (result != MYLITE_STORAGE_OK) {
        free_statement(statement);
        return result;
    }

    active_statement = statement;
    *out_statement = statement;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result initialize_checkpoint_statement(
    mylite_storage_statement *statement,
    const char *filename,
    mylite_storage_statement *parent
) {
    statement->parent = parent;
    if (parent != NULL) {
        statement->file = parent->file;
        statement->owns_file = 0;
        clone_parent_checkpoint_snapshot(statement, parent);
        return MYLITE_STORAGE_OK;
    }
    mylite_storage_statement *read_statement = active_read_statement_for(filename);
    if (read_statement != NULL) {
        statement->file = read_statement->file;
        statement->owns_file = 0;
        return lock_file(statement->file, LOCK_EX);
    }

    errno = 0;
    statement->file = fopen(filename, "r+b");
    if (statement->file == NULL) {
        return errno == ENOENT ? MYLITE_STORAGE_NOTFOUND : MYLITE_STORAGE_IOERR;
    }
    statement->owns_file = 1;

    mylite_storage_result result = lock_file(statement->file, LOCK_EX);
    if (result == MYLITE_STORAGE_OK) {
        result = recover_pending_journals_locked(statement->file, filename);
    }
    return result;
}

static void clone_parent_checkpoint_snapshot(
    mylite_storage_statement *statement,
    const mylite_storage_statement *parent
) {
    statement->header = parent->has_current_header ? parent->current_header : parent->header;
    statement->current_header = statement->header;
    statement->has_current_header = 1;
    statement->current_header_dirty = 0;
    encode_header_page(statement->header_page, &statement->header);

    const unsigned char *catalog_page =
        parent->has_current_catalog_page ? parent->current_catalog_page : parent->catalog_page;
    memcpy(statement->catalog_page, catalog_page, sizeof(statement->catalog_page));
    memcpy(statement->current_catalog_page, catalog_page, sizeof(statement->current_catalog_page));
    statement->current_catalog_root_page = statement->header.catalog_root_page;
    statement->current_catalog_generation = statement->header.catalog_generation;
    statement->has_current_catalog_page = 1;
}

static mylite_storage_result initialize_read_statement(
    mylite_storage_statement *statement,
    const char *filename,
    mylite_storage_statement *same_file_parent
) {
    if (same_file_parent != NULL) {
        statement->file = same_file_parent->file;
        statement->device = same_file_parent->device;
        statement->inode = same_file_parent->inode;
        statement->owns_file = 0;
        statement->header = same_file_parent->header;
        statement->current_header = same_file_parent->current_header;
        memcpy(
            statement->header_page,
            same_file_parent->header_page,
            sizeof(statement->header_page)
        );
        memcpy(
            statement->catalog_page,
            same_file_parent->catalog_page,
            sizeof(statement->catalog_page)
        );
        memcpy(
            statement->current_catalog_page,
            same_file_parent->current_catalog_page,
            sizeof(statement->current_catalog_page)
        );
        statement->current_catalog_root_page = same_file_parent->current_catalog_root_page;
        statement->current_catalog_generation = same_file_parent->current_catalog_generation;
        statement->has_current_header = same_file_parent->has_current_header;
        statement->has_current_catalog_page = same_file_parent->has_current_catalog_page;
        statement->has_identity = same_file_parent->has_identity;
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_result result = recover_pending_journals(filename);
    if (result == MYLITE_STORAGE_BUSY) {
        errno = 0;
        statement->file = fopen(filename, "rb");
        if (statement->file == NULL) {
            return errno == ENOENT ? MYLITE_STORAGE_NOTFOUND : MYLITE_STORAGE_IOERR;
        }
        statement->owns_file = 1;

        if (flock(fileno(statement->file), LOCK_SH | LOCK_NB) == 0) {
            flock(fileno(statement->file), LOCK_UN);
            return MYLITE_STORAGE_BUSY;
        }
        if (!is_lock_conflict(errno)) {
            return MYLITE_STORAGE_IOERR;
        }

        mylite_storage_transaction_journal_snapshot snapshot = {0};
        result = read_transaction_journal_snapshot(filename, &snapshot);
        if (result != MYLITE_STORAGE_OK) {
            return result == MYLITE_STORAGE_CORRUPT ? MYLITE_STORAGE_BUSY : result;
        }
        statement->header = snapshot.header;
        statement->current_header = snapshot.header;
        memcpy(statement->header_page, snapshot.header_page, sizeof(statement->header_page));
        memcpy(statement->catalog_page, snapshot.catalog_page, sizeof(statement->catalog_page));
        memcpy(
            statement->current_catalog_page,
            snapshot.catalog_page,
            sizeof(statement->current_catalog_page)
        );
        statement->current_catalog_root_page = snapshot.header.catalog_root_page;
        statement->current_catalog_generation = snapshot.header.catalog_generation;
        statement->has_current_header = 1;
        statement->has_current_catalog_page = 1;
        return MYLITE_STORAGE_OK;
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *cached_file = NULL;
    dev_t cached_device = 0;
    ino_t cached_inode = 0;
    if (take_cached_read_file(filename, &cached_file, &cached_device, &cached_inode)) {
        statement->file = cached_file;
        statement->device = cached_device;
        statement->inode = cached_inode;
        statement->has_identity = 1;
    } else {
        errno = 0;
        statement->file = fopen(filename, "r+b");
        if (statement->file == NULL) {
            return errno == ENOENT ? MYLITE_STORAGE_NOTFOUND : MYLITE_STORAGE_IOERR;
        }
    }
    statement->owns_file = 1;
    statement->cache_file_on_close = 1;
    if (!statement->has_identity) {
        struct stat file_stat;
        if (fstat(fileno(statement->file), &file_stat) != 0) {
            return MYLITE_STORAGE_IOERR;
        }
        statement->device = file_stat.st_dev;
        statement->inode = file_stat.st_ino;
        statement->has_identity = 1;
    }

    result = lock_file(statement->file, LOCK_SH);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return read_cached_checkpoint_snapshot(statement);
}

static mylite_storage_result read_checkpoint_snapshot(mylite_storage_statement *statement) {
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_result result = read_page_at(
        statement->file,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        header_page
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return read_checkpoint_snapshot_from_header_page(statement, header_page);
}

static mylite_storage_result read_cached_checkpoint_snapshot(mylite_storage_statement *statement) {
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_result result = read_page_at(
        statement->file,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        header_page
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_read_checkpoint_cache *cache = read_checkpoint_cache_for(statement);
    if (cache != NULL && memcmp(header_page, cache->header_page, sizeof(cache->header_page)) == 0) {
        if (cache->header.page_size > sizeof(statement->catalog_page)) {
            return MYLITE_STORAGE_CORRUPT;
        }
        copy_read_checkpoint_cache_to_statement(cache, statement);
        return MYLITE_STORAGE_OK;
    }

    result = read_checkpoint_snapshot_from_header_page(statement, header_page);
    if (result == MYLITE_STORAGE_OK) {
        store_read_checkpoint_cache(statement);
    }
    return result;
}

static mylite_storage_result read_checkpoint_snapshot_from_header_page(
    mylite_storage_statement *statement,
    const unsigned char *header_page
) {
    memcpy(statement->header_page, header_page, sizeof(statement->header_page));
    mylite_storage_result result = decode_header_page(statement->header_page, &statement->header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_page_at(
            statement->file,
            statement->header.catalog_root_page,
            statement->header.page_size,
            statement->catalog_page
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_catalog_root_bytes(statement->catalog_page, &statement->header);
    }
    if (result == MYLITE_STORAGE_OK) {
        statement->current_header = statement->header;
        statement->has_current_header = 1;
        statement->current_header_dirty = 0;
        memcpy(
            statement->current_catalog_page,
            statement->catalog_page,
            sizeof(statement->current_catalog_page)
        );
        statement->current_catalog_root_page = statement->header.catalog_root_page;
        statement->current_catalog_generation = statement->header.catalog_generation;
        statement->has_current_catalog_page = 1;
    }
    return result;
}

static void copy_read_checkpoint_cache_to_statement(
    const mylite_storage_read_checkpoint_cache *cache,
    mylite_storage_statement *statement
) {
    statement->header = cache->header;
    statement->current_header = cache->header;
    memcpy(statement->header_page, cache->header_page, sizeof(statement->header_page));
    memcpy(statement->catalog_page, cache->catalog_page, sizeof(statement->catalog_page));
    memcpy(
        statement->current_catalog_page,
        cache->current_catalog_page,
        sizeof(statement->current_catalog_page)
    );
    statement->current_catalog_root_page = cache->current_catalog_root_page;
    statement->current_catalog_generation = cache->current_catalog_generation;
    statement->has_current_header = 1;
    statement->current_header_dirty = 0;
    statement->has_current_catalog_page = cache->has_current_catalog_page;
}

static mylite_storage_read_checkpoint_cache *read_checkpoint_cache_for(
    const mylite_storage_statement *statement
) {
    if (!active_read_checkpoint_cache.has_snapshot ||
        active_read_checkpoint_cache.owner != active_context_owner ||
        active_read_checkpoint_cache.filename == NULL ||
        strcmp(active_read_checkpoint_cache.filename, statement->filename) != 0 ||
        !active_read_checkpoint_cache.has_identity || !statement->has_identity ||
        active_read_checkpoint_cache.device != statement->device ||
        active_read_checkpoint_cache.inode != statement->inode) {
        return NULL;
    }
    return &active_read_checkpoint_cache;
}

static void store_read_checkpoint_cache(const mylite_storage_statement *statement) {
    if (active_read_checkpoint_cache.filename == NULL ||
        strcmp(active_read_checkpoint_cache.filename, statement->filename) != 0 ||
        active_read_checkpoint_cache.owner != active_context_owner) {
        clear_read_checkpoint_cache();
        active_read_checkpoint_cache.filename = copy_filename(statement->filename);
        if (active_read_checkpoint_cache.filename == NULL) {
            return;
        }
    }

    active_read_checkpoint_cache.owner = active_context_owner;
    active_read_checkpoint_cache.device = statement->device;
    active_read_checkpoint_cache.inode = statement->inode;
    active_read_checkpoint_cache.header = statement->header;
    memcpy(
        active_read_checkpoint_cache.header_page,
        statement->header_page,
        sizeof(active_read_checkpoint_cache.header_page)
    );
    memcpy(
        active_read_checkpoint_cache.catalog_page,
        statement->catalog_page,
        sizeof(active_read_checkpoint_cache.catalog_page)
    );
    memcpy(
        active_read_checkpoint_cache.current_catalog_page,
        statement->current_catalog_page,
        sizeof(active_read_checkpoint_cache.current_catalog_page)
    );
    active_read_checkpoint_cache.current_catalog_root_page = statement->current_catalog_root_page;
    active_read_checkpoint_cache.current_catalog_generation = statement->current_catalog_generation;
    active_read_checkpoint_cache.has_current_catalog_page = statement->has_current_catalog_page;
    active_read_checkpoint_cache.has_snapshot = 1;
    active_read_checkpoint_cache.has_identity = statement->has_identity;
}

static void clear_read_checkpoint_cache(void) {
    free(active_read_checkpoint_cache.filename);
    active_read_checkpoint_cache = (mylite_storage_read_checkpoint_cache){0};
}

void mylite_storage_free(void *ptr) {
    free(ptr);
}

void mylite_storage_free_rowset(mylite_storage_rowset *rowset) {
    if (rowset == NULL) {
        return;
    }

    free(rowset->rows);
    free(rowset->row_offsets);
    free(rowset->row_sizes);
    free(rowset->row_ids);
    *rowset = (mylite_storage_rowset){
        .size = sizeof(*rowset),
    };
}

void mylite_storage_free_index_entryset(mylite_storage_index_entryset *entryset) {
    if (entryset == NULL) {
        return;
    }

    free(entryset->keys);
    free(entryset->key_offsets);
    free(entryset->key_sizes);
    free(entryset->row_ids);
    *entryset = (mylite_storage_index_entryset){
        .size = sizeof(*entryset),
    };
}

void mylite_storage_free_foreign_key_metadata(mylite_storage_foreign_key_metadata *metadata) {
    if (metadata == NULL) {
        return;
    }

    free(metadata->schema_name);
    free(metadata->table_name);
    free(metadata->constraint_name);
    free(metadata->referenced_schema_name);
    free(metadata->referenced_table_name);
    free(metadata->referenced_key_name);
    free_column_names(metadata->foreign_column_names, metadata->column_count);
    free_column_names(metadata->referenced_column_names, metadata->column_count);
    *metadata = (mylite_storage_foreign_key_metadata){
        .size = sizeof(*metadata),
    };
}

static mylite_storage_result path_exists(const char *filename, int *exists) {
    errno = 0;
    if (access(filename, F_OK) == 0) {
        *exists = 1;
        return MYLITE_STORAGE_OK;
    }

    if (errno == ENOENT || errno == ENOTDIR) {
        *exists = 0;
        return MYLITE_STORAGE_OK;
    }

    return MYLITE_STORAGE_IOERR;
}

static mylite_storage_result write_empty_database(FILE *file) {
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    initialize_header_page(header_page);
    initialize_empty_catalog_page(catalog_page);

    mylite_storage_result result = write_page(file, header_page, sizeof(header_page));
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return write_page(file, catalog_page, sizeof(catalog_page));
}

static void initialize_header_page(unsigned char *page) {
    const mylite_storage_header header = {
        .size = sizeof(header),
        .format_version = MYLITE_STORAGE_FORMAT_VERSION,
        .header_version = MYLITE_STORAGE_FORMAT_HEADER_VERSION,
        .page_size = MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        .checksum_algorithm = MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64,
        .flags = 0U,
        .catalog_root_page = MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID,
        .catalog_generation = MYLITE_STORAGE_FORMAT_EMPTY_CATALOG_GENERATION,
        .free_list_root_page = 0ULL,
        .page_count = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT,
    };
    encode_header_page(page, &header);
}

static void encode_header_page(unsigned char *page, const mylite_storage_header *header) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_HEADER_MAGIC_OFFSET,
        k_header_magic,
        sizeof(k_header_magic)
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_VERSION_OFFSET, header->header_version);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_FORMAT_VERSION_OFFSET, header->format_version);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_PAGE_SIZE_OFFSET, header->page_size);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_BYTE_ORDER_OFFSET,
        MYLITE_STORAGE_FORMAT_BYTE_ORDER_MARKER
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_ALGORITHM_OFFSET,
        header->checksum_algorithm
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_FLAGS_OFFSET, header->flags);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CATALOG_ROOT_PAGE_OFFSET,
        header->catalog_root_page
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CATALOG_GENERATION_OFFSET,
        header->catalog_generation
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_FREE_LIST_ROOT_PAGE_OFFSET,
        header->free_list_root_page
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_PAGE_COUNT_OFFSET, header->page_count);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET,
        checksum_page_zero_tail(
            page,
            MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET,
            MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET
        )
    );
}

static void initialize_empty_catalog_page(unsigned char *page) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_CATALOG_MAGIC_OFFSET,
        k_catalog_magic,
        sizeof(k_catalog_magic)
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_PAGE_TYPE_ROOT
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_PAGE_ID_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
        MYLITE_STORAGE_FORMAT_EMPTY_CATALOG_GENERATION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    update_catalog_checksum(page);
}

static void update_catalog_checksum(unsigned char *page) {
    put_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET,
        checksum_page_zero_tail(
            page,
            MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET,
            catalog_used_bytes(page)
        )
    );
}

static char *recovery_journal_path(const char *filename) {
    static const char suffix[] = "-journal";
    return journal_path_with_suffix(filename, suffix, sizeof(suffix));
}

static char *transaction_journal_path(const char *filename) {
    static const char suffix[] = "-transaction-journal";
    return journal_path_with_suffix(filename, suffix, sizeof(suffix));
}

static char *journal_path_with_suffix(
    const char *filename,
    const char *suffix,
    size_t suffix_size
) {
    const size_t filename_size = strlen(filename);
    if (filename_size > SIZE_MAX - suffix_size) {
        return NULL;
    }

    char *journal_filename = (char *)malloc(filename_size + suffix_size);
    if (journal_filename == NULL) {
        return NULL;
    }

    memcpy(journal_filename, filename, filename_size);
    memcpy(journal_filename + filename_size, suffix, suffix_size);
    return journal_filename;
}

static mylite_storage_result cached_journal_paths(
    const char *filename,
    const char **out_recovery_journal_filename,
    const char **out_transaction_journal_filename
) {
    if (active_journal_path_cache.filename == NULL ||
        strcmp(active_journal_path_cache.filename, filename) != 0) {
        char *filename_copy = copy_filename(filename);
        if (filename_copy == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        char *recovery_filename = recovery_journal_path(filename);
        if (recovery_filename == NULL) {
            free(filename_copy);
            return MYLITE_STORAGE_NOMEM;
        }
        char *transaction_filename = transaction_journal_path(filename);
        if (transaction_filename == NULL) {
            free(recovery_filename);
            free(filename_copy);
            return MYLITE_STORAGE_NOMEM;
        }

        clear_journal_path_cache(NULL);
        active_journal_path_cache = (mylite_storage_journal_path_cache){
            .filename = filename_copy,
            .recovery_journal_filename = recovery_filename,
            .transaction_journal_filename = transaction_filename,
        };
    }

    *out_recovery_journal_filename = active_journal_path_cache.recovery_journal_filename;
    *out_transaction_journal_filename = active_journal_path_cache.transaction_journal_filename;
    return MYLITE_STORAGE_OK;
}

static void clear_journal_path_cache(const char *filename) {
    if (filename != NULL && (active_journal_path_cache.filename == NULL ||
                             strcmp(active_journal_path_cache.filename, filename) != 0)) {
        return;
    }

    free(active_journal_path_cache.filename);
    free(active_journal_path_cache.recovery_journal_filename);
    free(active_journal_path_cache.transaction_journal_filename);
    active_journal_path_cache = (mylite_storage_journal_path_cache){0};
}

static mylite_storage_result recover_pending_journals(const char *filename) {
    const char *journal_filename = NULL;
    const char *transaction_filename = NULL;
    mylite_storage_result result =
        cached_journal_paths(filename, &journal_filename, &transaction_filename);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    int journal_exists = 0;
    int transaction_exists = 0;
    result = path_exists(journal_filename, &journal_exists);
    if (result == MYLITE_STORAGE_OK) {
        result = path_exists(transaction_filename, &transaction_exists);
    }
    if (result != MYLITE_STORAGE_OK || (!journal_exists && !transaction_exists)) {
        return result;
    }

    errno = 0;
    FILE *file = fopen(filename, "r+b");
    if (file == NULL) {
        return errno == ENOENT ? MYLITE_STORAGE_CORRUPT : MYLITE_STORAGE_IOERR;
    }

    result = lock_file(file, LOCK_EX);
    if (result == MYLITE_STORAGE_OK) {
        result = recover_pending_journals_locked(file, filename);
    }
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

static mylite_storage_result recover_pending_journals_locked(FILE *file, const char *filename) {
    const char *journal_filename = NULL;
    const char *transaction_filename = NULL;
    mylite_storage_result result =
        cached_journal_paths(filename, &journal_filename, &transaction_filename);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    result = recover_pending_journal_locked(file, journal_filename);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    return recover_pending_journal_locked(file, transaction_filename);
}

static mylite_storage_result recover_pending_journal_locked(
    FILE *file,
    const char *journal_filename
) {
    errno = 0;
    FILE *journal_file = fopen(journal_filename, "rb");
    if (journal_file == NULL) {
        return errno == ENOENT ? MYLITE_STORAGE_OK : MYLITE_STORAGE_IOERR;
    }
    mylite_storage_result result = MYLITE_STORAGE_OK;
    mylite_storage_recovery_journal journal = {0};
    unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                       [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    mylite_storage_header saved_header = {0};
    result = read_recovery_journal(journal_file, &journal, pages, &saved_header);
    if (result == MYLITE_STORAGE_OK) {
        result = restore_recovery_journal(file, &journal, pages, &saved_header);
    }
    if (fclose(journal_file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result == MYLITE_STORAGE_OK && remove(journal_filename) != 0) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = flush_parent_directory(journal_filename);
    }
    return result;
}

static mylite_storage_result read_recovery_journal(
    FILE *journal_file,
    mylite_storage_recovery_journal *out_journal,
    unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                       [MYLITE_STORAGE_FORMAT_PAGE_SIZE],
    mylite_storage_header *out_header
) {
    unsigned char journal_header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_result result = read_page_at(
        journal_file,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        journal_header_page
    );
    if (result == MYLITE_STORAGE_OK) {
        result = decode_recovery_journal_header(journal_header_page, out_journal);
    }
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < out_journal->page_count; ++i) {
        result = read_page_at(
            journal_file,
            (unsigned long long)i + 1ULL,
            MYLITE_STORAGE_FORMAT_PAGE_SIZE,
            pages[i]
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_recovery_journal_pages(out_journal, pages, out_header);
    }
    return result;
}

static mylite_storage_result restore_recovery_journal(
    FILE *file,
    const mylite_storage_recovery_journal *journal,
    const unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE],
    const mylite_storage_header *saved_header
) {
    mylite_storage_result result = MYLITE_STORAGE_OK;
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < journal->page_count; ++i) {
        result = write_page_at(file, journal->page_ids[i], saved_header->page_size, pages[i]);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = truncate_file_to_header_page_count(file, saved_header);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = flush_file(file);
    }
    return result;
}

static mylite_storage_result begin_write_journal(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    int include_catalog
) {
    mylite_storage_statement *statement = active_statement_for(filename);
    if (statement == NULL) {
        return begin_recovery_journal(file, filename, header, include_catalog);
    }
    if (include_catalog) {
        clear_statement_chain_exact_index_caches(statement);
        clear_statement_chain_live_row_id_caches(statement);
        clear_statement_chain_row_payload_caches(statement);
        clear_durable_exact_index_caches(filename);
    }
    if (statement_chain_has_write_journal(statement)) {
        return MYLITE_STORAGE_OK;
    }

    const mylite_storage_result result = begin_recovery_journal(file, filename, header, 1);
    if (result == MYLITE_STORAGE_OK) {
        statement->owns_recovery_journal = 1;
    }
    return result;
}

static mylite_storage_result begin_recovery_journal(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    int include_catalog
) {
    char *journal_filename = recovery_journal_path(filename);
    if (journal_filename == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    return begin_journal_at_path(file, journal_filename, header, include_catalog);
}

static mylite_storage_result begin_transaction_journal(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header
) {
    char *journal_filename = transaction_journal_path(filename);
    if (journal_filename == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    return begin_journal_at_path(file, journal_filename, header, 1);
}

static mylite_storage_result begin_journal_at_path(
    FILE *file,
    char *journal_filename,
    const mylite_storage_header *header,
    int include_catalog
) {
    if (header->page_size != MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        free(journal_filename);
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    mylite_storage_result result = MYLITE_STORAGE_OK;
    mylite_storage_recovery_journal journal = {
        .page_ids = {MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, header->catalog_root_page},
        .page_count = include_catalog ? 2U : 1U,
    };
    unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                       [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < journal.page_count; ++i) {
        result = read_page_at(file, journal.page_ids[i], header->page_size, pages[i]);
    }
    mylite_storage_header saved_header = {0};
    if (result == MYLITE_STORAGE_OK) {
        result = validate_recovery_journal_pages(&journal, pages, &saved_header);
    }

    FILE *journal_file = NULL;
    int journal_created = 0;
    if (result == MYLITE_STORAGE_OK) {
        errno = 0;
        const int journal_fd = open(journal_filename, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (journal_fd < 0) {
            result = errno == EEXIST ? MYLITE_STORAGE_CORRUPT : MYLITE_STORAGE_IOERR;
        } else {
            journal_created = 1;
            journal_file = fdopen(journal_fd, "wb");
            if (journal_file == NULL) {
                close(journal_fd);
                result = MYLITE_STORAGE_IOERR;
            }
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        unsigned char journal_header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_recovery_journal_header(journal_header_page, &journal);
        result = write_page(journal_file, journal_header_page, sizeof(journal_header_page));
    }
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < journal.page_count; ++i) {
        result = write_page(journal_file, pages[i], header->page_size);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = flush_file(journal_file);
    }

    if (journal_file != NULL && fclose(journal_file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = flush_parent_directory(journal_filename);
    }
    if (result != MYLITE_STORAGE_OK && journal_created) {
        remove(journal_filename);
    }

    free(journal_filename);
    return result;
}

static mylite_storage_result finish_write_journal(FILE *file, const char *filename) {
    if (active_statement_for(filename) != NULL) {
        return MYLITE_STORAGE_OK;
    }
    return finish_recovery_journal(file, filename);
}

static mylite_storage_result finish_recovery_journal(FILE *file, const char *filename) {
    char *journal_filename = recovery_journal_path(filename);
    if (journal_filename == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    return finish_journal_at_path(file, journal_filename);
}

static mylite_storage_result finish_transaction_journal(FILE *file, const char *filename) {
    char *journal_filename = transaction_journal_path(filename);
    if (journal_filename == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    return finish_journal_at_path(file, journal_filename);
}

static mylite_storage_result finish_journal_at_path(FILE *file, char *journal_filename) {
    mylite_storage_result result = flush_file(file);
    if (result == MYLITE_STORAGE_OK && remove(journal_filename) != 0) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = flush_parent_directory(journal_filename);
    }

    free(journal_filename);
    return result;
}

static int statement_chain_has_write_journal(const mylite_storage_statement *statement) {
    for (const mylite_storage_statement *current = statement; current != NULL;
         current = current->parent) {
        if (current->owns_recovery_journal || current->owns_transaction_journal) {
            return 1;
        }
    }
    return 0;
}

static void encode_recovery_journal_header(
    unsigned char *page,
    const mylite_storage_recovery_journal *journal
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_JOURNAL_MAGIC_OFFSET,
        k_journal_magic,
        sizeof(k_journal_magic)
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_TYPE_ROLLBACK
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_JOURNAL_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_PRIMARY_PAGE_SIZE_OFFSET,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_PROTECTED_PAGE_COUNT_OFFSET,
        (unsigned)journal->page_count
    );
    for (size_t i = 0U; i < journal->page_count; ++i) {
        put_u64_le(
            page,
            MYLITE_STORAGE_FORMAT_JOURNAL_PROTECTED_PAGE_IDS_OFFSET + (i * sizeof(uint64_t)),
            journal->page_ids[i]
        );
    }
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_OFFSET,
        checksum_page(page, MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result decode_recovery_journal_header(
    const unsigned char *page,
    mylite_storage_recovery_journal *out_journal
) {
    if (memcmp(
            page + MYLITE_STORAGE_FORMAT_JOURNAL_MAGIC_OFFSET,
            k_journal_magic,
            sizeof(k_journal_magic)
        ) != 0) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_TYPE_OFFSET);
    const unsigned page_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_JOURNAL_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned primary_page_size =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_JOURNAL_PRIMARY_PAGE_SIZE_OFFSET);
    if (page_type != MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_TYPE_ROLLBACK) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (page_version != MYLITE_STORAGE_FORMAT_JOURNAL_VERSION ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 ||
        primary_page_size != MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_OFFSET);
    if (expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_count =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_JOURNAL_PROTECTED_PAGE_COUNT_OFFSET);
    if (page_count == 0U || page_count > MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_recovery_journal journal = {
        .page_count = page_count,
    };
    for (size_t i = 0U; i < journal.page_count; ++i) {
        journal.page_ids[i] = get_u64_le(
            page,
            MYLITE_STORAGE_FORMAT_JOURNAL_PROTECTED_PAGE_IDS_OFFSET + (i * sizeof(uint64_t))
        );
    }

    *out_journal = journal;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_recovery_journal_pages(
    const mylite_storage_recovery_journal *journal,
    const unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE],
    mylite_storage_header *out_header
) {
    if (journal->page_count == 0U || journal->page_ids[0] != MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = decode_header_page(pages[0], out_header);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    for (size_t i = 1U; i < journal->page_count; ++i) {
        if (journal->page_ids[i] == MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID) {
            return MYLITE_STORAGE_CORRUPT;
        }
        for (size_t j = 1U; j < i; ++j) {
            if (journal->page_ids[i] == journal->page_ids[j]) {
                return MYLITE_STORAGE_CORRUPT;
            }
        }
        if (journal->page_ids[i] != out_header->catalog_root_page) {
            return MYLITE_STORAGE_CORRUPT;
        }
        result = validate_catalog_root_bytes(pages[i], out_header);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_page(FILE *file, const unsigned char *page, size_t size) {
    errno = 0;
    if (fwrite(page, 1U, size, file) != size) {
        return io_error_result_from_errno(errno);
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result lock_file(FILE *file, int operation) {
    unsigned waited_ms = 0U;
    for (;;) {
        if (flock(fileno(file), operation | LOCK_NB) == 0) {
            return MYLITE_STORAGE_OK;
        }
        if (!is_lock_conflict(errno)) {
            return MYLITE_STORAGE_IOERR;
        }
        if (waited_ms >= active_busy_timeout_ms) {
            return MYLITE_STORAGE_BUSY;
        }

        const unsigned remaining_ms = active_busy_timeout_ms - waited_ms;
        const unsigned sleep_ms =
            remaining_ms < k_lock_retry_sleep_ms ? remaining_ms : k_lock_retry_sleep_ms;
        usleep((useconds_t)sleep_ms * k_microseconds_per_millisecond);
        waited_ms += sleep_ms;
    }
}

static int is_lock_conflict(int error_number) {
    if (error_number == EWOULDBLOCK) {
        return 1;
    }
#if EAGAIN != EWOULDBLOCK
    if (error_number == EAGAIN) {
        return 1;
    }
#endif
    return 0;
}

static mylite_storage_result flush_file(FILE *file) {
    errno = 0;
    if (fflush(file) != 0) {
        return io_error_result_from_errno(errno);
    }
    errno = 0;
    if (fsync(fileno(file)) != 0) {
        return io_error_result_from_errno(errno);
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result truncate_file_to_header_page_count(
    FILE *file,
    const mylite_storage_header *header
) {
    if (header->page_size == 0U ||
        header->page_count > ULLONG_MAX / (unsigned long long)header->page_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long file_size =
        header->page_count * (unsigned long long)header->page_size;
    if (file_size > (unsigned long long)LONG_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    const int file_descriptor = fileno(file);
    if (file_descriptor < 0) {
        return MYLITE_STORAGE_IOERR;
    }
    errno = 0;
    if (ftruncate(file_descriptor, (off_t)file_size) != 0) {
        return io_error_result_from_errno(errno);
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result flush_parent_directory(const char *filename) {
    const char *last_slash = strrchr(filename, '/');
    const char *parent_path = ".";
    char *owned_parent_path = NULL;
    if (last_slash == filename) {
        parent_path = "/";
    } else if (last_slash != NULL) {
        const size_t parent_path_size = (size_t)(last_slash - filename);
        owned_parent_path = (char *)malloc(parent_path_size + 1U);
        if (owned_parent_path == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        memcpy(owned_parent_path, filename, parent_path_size);
        owned_parent_path[parent_path_size] = '\0';
        parent_path = owned_parent_path;
    }

    const int directory_fd = open(parent_path, O_RDONLY);
    free(owned_parent_path);
    if (directory_fd < 0) {
        return MYLITE_STORAGE_IOERR;
    }

    mylite_storage_result result = MYLITE_STORAGE_OK;
    if (fsync(directory_fd) != 0) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (close(directory_fd) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

static mylite_storage_result close_created_file(FILE *file, const char *filename) {
    mylite_storage_result result = flush_file(file);
    const mylite_storage_result close_result = close_existing_file(file);
    if (close_result != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = close_result;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = flush_parent_directory(filename);
    }
    if (result == MYLITE_STORAGE_OK) {
        return MYLITE_STORAGE_OK;
    }

    remove(filename);
    return result;
}

static mylite_storage_result open_existing_file(const char *filename, FILE **out_file) {
    mylite_storage_statement *statement = active_statement_for(filename);
    if (statement != NULL) {
        *out_file = statement->file;
        return MYLITE_STORAGE_OK;
    }
    statement = active_read_statement_for(filename);
    if (statement != NULL) {
        *out_file = statement->file;
        return MYLITE_STORAGE_OK;
    }
    statement = active_read_snapshot_for(filename);
    if (statement != NULL) {
        active_read_snapshot = statement;
        *out_file = statement->file;
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_result result = recover_pending_journals(filename);
    if (result != MYLITE_STORAGE_OK) {
        if (result == MYLITE_STORAGE_BUSY) {
            const mylite_storage_result snapshot_result =
                open_transaction_journal_snapshot(filename, out_file);
            if (snapshot_result == MYLITE_STORAGE_OK) {
                return MYLITE_STORAGE_OK;
            }
            if (snapshot_result != MYLITE_STORAGE_BUSY) {
                return snapshot_result;
            }
        }
        return result;
    }

    errno = 0;
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        return errno == ENOENT ? MYLITE_STORAGE_NOTFOUND : MYLITE_STORAGE_IOERR;
    }

    result = lock_file(file, LOCK_SH);
    if (result != MYLITE_STORAGE_OK) {
        close_existing_file(file);
        return result;
    }

    *out_file = file;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result open_transaction_journal_snapshot(
    const char *filename,
    FILE **out_file
) {
    errno = 0;
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        return errno == ENOENT ? MYLITE_STORAGE_NOTFOUND : MYLITE_STORAGE_IOERR;
    }

    if (flock(fileno(file), LOCK_SH | LOCK_NB) == 0) {
        flock(fileno(file), LOCK_UN);
        fclose(file);
        return MYLITE_STORAGE_BUSY;
    }
    if (!is_lock_conflict(errno)) {
        fclose(file);
        return MYLITE_STORAGE_IOERR;
    }

    mylite_storage_transaction_journal_snapshot snapshot = {0};
    mylite_storage_result result = read_transaction_journal_snapshot(filename, &snapshot);
    if (result != MYLITE_STORAGE_OK) {
        fclose(file);
        return result == MYLITE_STORAGE_CORRUPT ? MYLITE_STORAGE_BUSY : result;
    }

    snapshot.file = file;
    active_transaction_journal_snapshot = snapshot;
    *out_file = file;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_transaction_journal_snapshot(
    const char *filename,
    mylite_storage_transaction_journal_snapshot *out_snapshot
) {
    char *journal_filename = transaction_journal_path(filename);
    if (journal_filename == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    errno = 0;
    FILE *journal_file = fopen(journal_filename, "rb");
    free(journal_filename);
    if (journal_file == NULL) {
        return errno == ENOENT ? MYLITE_STORAGE_BUSY : MYLITE_STORAGE_IOERR;
    }

    mylite_storage_recovery_journal journal = {0};
    unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                       [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    mylite_storage_header saved_header = {0};
    mylite_storage_result result =
        read_recovery_journal(journal_file, &journal, pages, &saved_header);
    if (fclose(journal_file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (journal.page_count != 2U) {
        return MYLITE_STORAGE_CORRUPT;
    }

    out_snapshot->header = saved_header;
    memcpy(out_snapshot->header_page, pages[0], sizeof(out_snapshot->header_page));
    memcpy(out_snapshot->catalog_page, pages[1], sizeof(out_snapshot->catalog_page));
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result open_existing_file_for_update(const char *filename, FILE **out_file) {
    mylite_storage_statement *statement = active_statement_for(filename);
    if (statement != NULL) {
        *out_file = statement->file;
        return MYLITE_STORAGE_OK;
    }
    statement = active_read_statement_for(filename);
    if (statement != NULL) {
        mylite_storage_result result = lock_file(statement->file, LOCK_EX);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        *out_file = statement->file;
        return MYLITE_STORAGE_OK;
    }
    if (active_read_statement_for_any_owner(filename) != NULL &&
        active_read_statement_for(filename) == NULL) {
        return MYLITE_STORAGE_BUSY;
    }
    if (active_statement_for_any_owner(filename) != NULL) {
        return MYLITE_STORAGE_BUSY;
    }

    errno = 0;
    FILE *file = fopen(filename, "r+b");
    if (file == NULL) {
        return errno == ENOENT ? MYLITE_STORAGE_NOTFOUND : MYLITE_STORAGE_IOERR;
    }

    mylite_storage_result result = lock_file(file, LOCK_EX);
    if (result == MYLITE_STORAGE_OK) {
        result = recover_pending_journals_locked(file, filename);
    }
    if (result != MYLITE_STORAGE_OK) {
        close_existing_file(file);
        return result;
    }

    *out_file = file;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result close_existing_file(FILE *file) {
#ifndef __clang_analyzer__
    if (active_read_statement_has_file(file)) {
        clearerr(file);
        return MYLITE_STORAGE_OK;
    }
    if (active_read_snapshot_has_file(file)) {
        active_read_snapshot = NULL;
        clearerr(file);
        return MYLITE_STORAGE_OK;
    }
    if (active_transaction_journal_snapshot_has_file(file)) {
        active_transaction_journal_snapshot = (mylite_storage_transaction_journal_snapshot){0};
        clearerr(file);
        return fclose(file) == 0 ? MYLITE_STORAGE_OK : MYLITE_STORAGE_IOERR;
    }
    /* Statement-owned handles stay open until checkpoint commit or rollback. */
    if (active_statement_has_file(file)) {
        clearerr(file);
        return MYLITE_STORAGE_OK;
    }
#endif
    return fclose(file) == 0 ? MYLITE_STORAGE_OK : MYLITE_STORAGE_IOERR;
}

static mylite_storage_statement *active_statement_for(const char *filename) {
    if (filename == NULL) {
        return NULL;
    }

    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (strcmp(statement->filename, filename) == 0 &&
            statement->owner == active_context_owner) {
            return statement;
        }
    }
    return NULL;
}

static mylite_storage_statement *active_statement_for_any_owner(const char *filename) {
    if (filename == NULL) {
        return NULL;
    }

    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (strcmp(statement->filename, filename) == 0) {
            return statement;
        }
    }
    return NULL;
}

static mylite_storage_statement *active_read_statement_for(const char *filename) {
    if (filename == NULL) {
        return NULL;
    }

    for (mylite_storage_statement *statement = active_read_statement; statement != NULL;
         statement = statement->parent) {
        if (strcmp(statement->filename, filename) == 0 &&
            statement->owner == active_context_owner) {
            return statement;
        }
    }
    return NULL;
}

static mylite_storage_statement *active_read_statement_for_any_owner(const char *filename) {
    if (filename == NULL) {
        return NULL;
    }

    for (mylite_storage_statement *statement = active_read_statement; statement != NULL;
         statement = statement->parent) {
        if (strcmp(statement->filename, filename) == 0) {
            return statement;
        }
    }
    return NULL;
}

static mylite_storage_statement *active_read_snapshot_for(const char *filename) {
    if (filename == NULL) {
        return NULL;
    }

    mylite_storage_statement *snapshot = NULL;
    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (strcmp(statement->filename, filename) != 0) {
            continue;
        }
        if (statement->owner == active_context_owner) {
            return NULL;
        }
        snapshot = statement;
    }
    return snapshot;
}

static mylite_storage_statement *active_table_entry_cache_statement_for(const char *filename) {
    if (filename == NULL) {
        return NULL;
    }

    mylite_storage_statement *cache_statement = NULL;
    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (strcmp(statement->filename, filename) == 0 &&
            statement->owner == active_context_owner) {
            cache_statement = statement;
        }
    }
    return cache_statement;
}

static mylite_storage_statement *active_exact_index_cache_statement_for(const char *filename) {
    if (filename == NULL) {
        return NULL;
    }

    mylite_storage_statement *cache_statement = NULL;
    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (strcmp(statement->filename, filename) == 0 &&
            statement->owner == active_context_owner) {
            cache_statement = statement;
        }
    }
    return cache_statement;
}

static mylite_storage_statement *active_live_row_id_cache_statement_for(const char *filename) {
    if (filename == NULL) {
        return NULL;
    }

    mylite_storage_statement *cache_statement = NULL;
    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (strcmp(statement->filename, filename) == 0 &&
            statement->owner == active_context_owner) {
            cache_statement = statement;
        }
    }
    return cache_statement;
}

static mylite_storage_statement *active_row_payload_cache_statement_for(const char *filename) {
    if (filename == NULL) {
        return NULL;
    }

    mylite_storage_statement *cache_statement = NULL;
    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (strcmp(statement->filename, filename) == 0 &&
            statement->owner == active_context_owner) {
            cache_statement = statement;
        }
    }
    return cache_statement;
}

static mylite_storage_statement *active_statement_for_file(FILE *file) {
    if (file == NULL || active_read_snapshot_has_file(file)) {
        return NULL;
    }

    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (statement->file == file && statement->owner == active_context_owner) {
            return statement;
        }
    }
    return NULL;
}

static mylite_storage_statement *active_read_statement_for_file(FILE *file) {
    if (file == NULL) {
        return NULL;
    }

    for (mylite_storage_statement *statement = active_read_statement; statement != NULL;
         statement = statement->parent) {
        if (statement->file == file && statement->owner == active_context_owner) {
            return statement;
        }
    }
    return NULL;
}

static mylite_storage_statement *append_page_buffer_statement_for_file(FILE *file) {
    if (file == NULL || active_read_snapshot_has_file(file)) {
        return NULL;
    }

    mylite_storage_statement *buffer_statement = NULL;
    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (statement->file == file && statement->owner == active_context_owner) {
            buffer_statement = statement;
        }
    }
    return buffer_statement;
}

static int active_statement_has_file(FILE *file) {
    if (file == NULL) {
        return 0;
    }
    for (mylite_storage_statement *statement = active_statement; statement != NULL;
         statement = statement->parent) {
        if (statement->file == file) {
            return 1;
        }
    }
    return 0;
}

static int active_read_statement_has_file(FILE *file) {
    if (file == NULL) {
        return 0;
    }
    for (mylite_storage_statement *statement = active_read_statement; statement != NULL;
         statement = statement->parent) {
        if (statement->file == file) {
            return 1;
        }
    }
    return 0;
}

static int active_read_snapshot_has_file(FILE *file) {
    return active_read_snapshot != NULL && active_read_snapshot->file == file;
}

static int active_transaction_journal_snapshot_has_file(FILE *file) {
    return active_transaction_journal_snapshot.file != NULL &&
           active_transaction_journal_snapshot.file == file;
}

static mylite_storage_result close_statement(mylite_storage_statement *statement) {
    if (statement->file == NULL || !statement->owns_file) {
        return MYLITE_STORAGE_OK;
    }

    FILE *file = statement->file;
    statement->file = NULL;
    if (statement->cache_file_on_close) {
        return cache_read_file(
            statement->filename,
            file,
            statement->device,
            statement->inode,
            statement->has_identity
        );
    }
    return fclose(file) == 0 ? MYLITE_STORAGE_OK : MYLITE_STORAGE_IOERR;
}

static mylite_storage_result flush_statement_append_page_buffer(
    mylite_storage_statement *statement
) {
    if (statement == NULL || statement->append_pages.page_count == 0U) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_result result = refresh_statement_append_page_buffer_checksums(statement);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_append_page_buffer *buffer = &statement->append_pages;
    off_t offset = 0;
    result = page_offset_for_io(buffer->first_page_id, MYLITE_STORAGE_FORMAT_PAGE_SIZE, &offset);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    const size_t write_size = buffer->page_count * MYLITE_STORAGE_FORMAT_PAGE_SIZE;
    if ((unsigned long long)write_size >
        (unsigned long long)LONG_MAX - (unsigned long long)offset) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    result = write_file_at(statement->file, offset, buffer->pages, write_size);
    if (result == MYLITE_STORAGE_OK) {
        buffer->first_page_id = 0ULL;
        buffer->page_count = 0U;
    }
    return result;
}

static mylite_storage_result refresh_statement_append_page_buffer_checksums(
    mylite_storage_statement *statement
) {
    mylite_storage_append_page_buffer *buffer = &statement->append_pages;
    if (buffer->checksum_dirty == NULL) {
        return MYLITE_STORAGE_CORRUPT;
    }
    for (size_t i = 0U; i < buffer->page_count; ++i) {
        if (buffer->checksum_dirty[i] == 0U) {
            continue;
        }
        mylite_storage_result result = refresh_dirty_buffered_page_checksum(
            buffer->pages + (i * MYLITE_STORAGE_FORMAT_PAGE_SIZE)
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        buffer->checksum_dirty[i] = 0U;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result flush_statement_append_page_buffer_before_truncate(
    mylite_storage_statement *statement,
    unsigned long long page_count
) {
    mylite_storage_statement *buffer_statement =
        append_page_buffer_statement_for_file(statement->file);
    if (buffer_statement == NULL || buffer_statement->append_pages.page_count == 0U) {
        return MYLITE_STORAGE_OK;
    }

    if (buffer_statement->append_pages.first_page_id >= page_count) {
        return MYLITE_STORAGE_OK;
    }
    return flush_statement_append_page_buffer(buffer_statement);
}

static void trim_statement_append_page_buffer(
    mylite_storage_statement *statement,
    unsigned long long page_count
) {
    mylite_storage_statement *buffer_statement =
        append_page_buffer_statement_for_file(statement->file);
    if (buffer_statement == NULL || buffer_statement->append_pages.page_count == 0U) {
        return;
    }

    mylite_storage_append_page_buffer *buffer = &buffer_statement->append_pages;
    if (buffer->first_page_id >= page_count) {
        buffer->first_page_id = 0ULL;
        buffer->page_count = 0U;
        return;
    }

    const unsigned long long buffered_end =
        buffer->first_page_id + (unsigned long long)buffer->page_count;
    if (buffered_end > page_count) {
        buffer->page_count = (size_t)(page_count - buffer->first_page_id);
    }
}

static mylite_storage_result restore_buffered_page_undos(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return MYLITE_STORAGE_OK;
    }

    for (size_t i = 0U; i < statement->buffered_page_undos.count; ++i) {
        const mylite_storage_buffered_page_undo *undo = statement->buffered_page_undos.entries + i;
        if (undo->used_size == 0U || undo->used_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
            return MYLITE_STORAGE_CORRUPT;
        }
        if (undo->used_size < MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
            unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
            memset(page, 0, sizeof(page));
            memcpy(page, undo->page, undo->used_size);
            if (!replace_buffered_append_page(
                    statement->file,
                    undo->page_id,
                    MYLITE_STORAGE_FORMAT_PAGE_SIZE,
                    page,
                    undo->checksum_dirty
                )) {
                return MYLITE_STORAGE_CORRUPT;
            }
            continue;
        }
        if (!replace_buffered_append_page(
                statement->file,
                undo->page_id,
                MYLITE_STORAGE_FORMAT_PAGE_SIZE,
                undo->page,
                undo->checksum_dirty
            )) {
            return MYLITE_STORAGE_CORRUPT;
        }
    }
    clear_buffered_page_undos(statement);
    return MYLITE_STORAGE_OK;
}

static void clear_append_page_buffer(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return;
    }

    free(statement->append_pages.pages);
    free(statement->append_pages.checksum_dirty);
    statement->append_pages = (mylite_storage_append_page_buffer){0};
}

static void clear_buffered_page_undos(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return;
    }

    free(statement->buffered_page_undos.entries);
    statement->buffered_page_undos = (mylite_storage_buffered_page_undo_list){0};
}

static void clear_buffered_update_rewrites(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return;
    }

    free(statement->buffered_update_rewrites.buckets);
    statement->buffered_update_rewrites = (mylite_storage_buffered_update_rewrite_cache){0};
}

static void clear_statement_chain_buffered_update_rewrites(mylite_storage_statement *statement) {
    for (; statement != NULL; statement = statement->parent) {
        clear_buffered_update_rewrites(statement);
    }
}

static int take_cached_read_file(
    const char *filename,
    FILE **out_file,
    dev_t *out_device,
    ino_t *out_inode
) {
    *out_file = NULL;
    *out_device = 0;
    *out_inode = 0;
    if (active_read_file_cache.file == NULL || active_read_file_cache.filename == NULL ||
        strcmp(active_read_file_cache.filename, filename) != 0) {
        return 0;
    }
    if (!cached_read_file_matches_path(filename)) {
        clear_cached_read_file(filename);
        return 0;
    }

    *out_file = active_read_file_cache.file;
    *out_device = active_read_file_cache.device;
    *out_inode = active_read_file_cache.inode;
    active_read_file_cache.file = NULL;
    clearerr(*out_file);
    return 1;
}

static mylite_storage_result cache_read_file(
    const char *filename,
    FILE *file,
    dev_t device,
    ino_t inode,
    int has_identity
) {
    if (file == NULL) {
        return MYLITE_STORAGE_OK;
    }
    if (flock(fileno(file), LOCK_UN) != 0) {
        fclose(file);
        return MYLITE_STORAGE_IOERR;
    }
    clearerr(file);

    if (!has_identity) {
        struct stat file_stat;
        if (fstat(fileno(file), &file_stat) != 0) {
            fclose(file);
            return MYLITE_STORAGE_IOERR;
        }
        device = file_stat.st_dev;
        inode = file_stat.st_ino;
        has_identity = 1;
    }

    if (active_read_file_cache.filename == NULL ||
        strcmp(active_read_file_cache.filename, filename) != 0) {
        char *filename_copy = copy_filename(filename);
        if (filename_copy == NULL) {
            return fclose(file) == 0 ? MYLITE_STORAGE_OK : MYLITE_STORAGE_IOERR;
        }
        clear_cached_read_file(NULL);
        active_read_file_cache.filename = filename_copy;
    } else if (active_read_file_cache.file != NULL && active_read_file_cache.file != file) {
        fclose(active_read_file_cache.file);
        active_read_file_cache.file = NULL;
    }

    active_read_file_cache.file = file;
    active_read_file_cache.device = device;
    active_read_file_cache.inode = inode;
    active_read_file_cache.has_identity = has_identity;
    return MYLITE_STORAGE_OK;
}

static void clear_cached_read_file(const char *filename) {
    if (filename != NULL && (active_read_file_cache.filename == NULL ||
                             strcmp(active_read_file_cache.filename, filename) != 0)) {
        return;
    }

    FILE *file = active_read_file_cache.file;
    active_read_file_cache.file = NULL;
    free(active_read_file_cache.filename);
    active_read_file_cache.filename = NULL;
    active_read_file_cache.device = 0;
    active_read_file_cache.inode = 0;
    active_read_file_cache.has_identity = 0;
    if (file != NULL) {
        fclose(file);
    }
}

static int cached_read_file_matches_path(const char *filename) {
    if (!active_read_file_cache.has_identity) {
        return 0;
    }

    struct stat path_stat;
    if (stat(filename, &path_stat) != 0) {
        return 0;
    }
    return path_stat.st_dev == active_read_file_cache.device &&
           path_stat.st_ino == active_read_file_cache.inode;
}

static mylite_storage_result write_statement_current_header(mylite_storage_statement *statement) {
    if (!statement->has_current_header) {
        return MYLITE_STORAGE_OK;
    }

    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    encode_header_page(header_page, &statement->current_header);
    const mylite_storage_result result = write_page_at_raw(
        statement->file,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        statement->current_header.page_size,
        header_page
    );
    if (result == MYLITE_STORAGE_OK) {
        statement->current_header_dirty = 0;
    }
    return result;
}

static void clear_statement_chain_catalog_root_caches(mylite_storage_statement *statement) {
    for (mylite_storage_statement *current = statement; current != NULL;
         current = current->parent) {
        clear_catalog_root_cache(current);
    }
}

static void clear_catalog_root_cache(mylite_storage_statement *statement) {
    statement->has_current_catalog_page = 0;
    statement->current_catalog_root_page = 0ULL;
    statement->current_catalog_generation = 0ULL;
    clear_table_entry_cache(&statement->table_entry_cache);
}

static void free_statement(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return;
    }

    if (statement->file != NULL && statement->owns_file) {
        fclose(statement->file);
    }
    clear_exact_index_caches(statement);
    clear_live_row_caches(statement);
    clear_live_row_id_caches(statement);
    clear_row_payload_caches(statement);
    clear_table_entry_cache(&statement->table_entry_cache);
    clear_append_page_buffer(statement);
    clear_buffered_page_undos(statement);
    clear_buffered_update_rewrites(statement);
    free(statement->filename);
    free(statement);
}

static int find_active_table_entry_cache(
    const char *filename,
    const mylite_storage_header *header,
    const char *schema_name,
    const char *table_name,
    mylite_storage_catalog_entry *out_entry
) {
    mylite_storage_statement *statement = active_table_entry_cache_statement_for(filename);
    if (statement == NULL) {
        return 0;
    }

    const mylite_storage_table_entry_cache *cache = &statement->table_entry_cache;
    if (!cache->has_entry || cache->schema_name == NULL || cache->table_name == NULL ||
        cache->catalog_root_page != header->catalog_root_page ||
        cache->catalog_generation != header->catalog_generation ||
        strcmp(cache->schema_name, schema_name) != 0 ||
        strcmp(cache->table_name, table_name) != 0) {
        return 0;
    }

    *out_entry = cache->entry;
    out_entry->record = NULL;
    return 1;
}

static void store_active_table_entry_cache(
    const char *filename,
    const mylite_storage_header *header,
    const char *schema_name,
    const char *table_name,
    const mylite_storage_catalog_entry *entry
) {
    mylite_storage_statement *statement = active_table_entry_cache_statement_for(filename);
    if (statement == NULL) {
        return;
    }

    mylite_storage_table_entry_cache *cache = &statement->table_entry_cache;
    if (cache->has_entry && cache->schema_name != NULL && cache->table_name != NULL &&
        strcmp(cache->schema_name, schema_name) == 0 &&
        strcmp(cache->table_name, table_name) == 0) {
        cache->catalog_root_page = header->catalog_root_page;
        cache->catalog_generation = header->catalog_generation;
        cache->entry = *entry;
        cache->entry.record = NULL;
        return;
    }

    char *schema_name_copy = copy_string(schema_name);
    if (schema_name_copy == NULL) {
        return;
    }
    char *table_name_copy = copy_string(table_name);
    if (table_name_copy == NULL) {
        free(schema_name_copy);
        return;
    }

    clear_table_entry_cache(cache);
    cache->schema_name = schema_name_copy;
    cache->table_name = table_name_copy;
    cache->catalog_root_page = header->catalog_root_page;
    cache->catalog_generation = header->catalog_generation;
    cache->entry = *entry;
    cache->entry.record = NULL;
    cache->has_entry = 1;
}

static void clear_table_entry_cache(mylite_storage_table_entry_cache *cache) {
    free(cache->schema_name);
    free(cache->table_name);
    *cache = (mylite_storage_table_entry_cache){0};
}

static void clear_statement_chain_exact_index_caches(mylite_storage_statement *statement) {
    for (mylite_storage_statement *current = statement; current != NULL;
         current = current->parent) {
        clear_exact_index_caches(current);
    }
}

static void clear_exact_index_caches(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return;
    }

    for (size_t i = 0U; i < statement->exact_index_caches.count; ++i) {
        free_exact_index_cache(statement->exact_index_caches.entries + i);
    }
    free(statement->exact_index_caches.entries);
    statement->exact_index_caches = (mylite_storage_exact_index_cache_set){0};
}

static void free_exact_index_cache(mylite_storage_exact_index_cache *cache) {
    free(cache->filename);
    free(cache->keys);
    free(cache->row_ids);
    free(cache->entry_live);
    free(cache->bucket_heads);
    free(cache->bucket_next);
    free(cache->row_id_bucket_heads);
    free(cache->row_id_bucket_next);
    *cache = (mylite_storage_exact_index_cache){0};
}

static void clear_statement_chain_live_row_caches(mylite_storage_statement *statement) {
    for (mylite_storage_statement *current = statement; current != NULL;
         current = current->parent) {
        clear_live_row_caches(current);
    }
}

static void clear_live_row_caches(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return;
    }

    for (size_t i = 0U; i < statement->live_row_caches.count; ++i) {
        free_live_row_cache(statement->live_row_caches.entries + i);
    }
    free(statement->live_row_caches.entries);
    statement->live_row_caches = (mylite_storage_live_row_cache_set){0};
}

static void free_live_row_cache(mylite_storage_live_row_cache *cache) {
    free(cache->row_ids);
    free(cache->validated_row_ids);
    *cache = (mylite_storage_live_row_cache){0};
}

static void clear_statement_chain_live_row_id_caches(mylite_storage_statement *statement) {
    for (mylite_storage_statement *current = statement; current != NULL;
         current = current->parent) {
        clear_live_row_id_caches(current);
    }
}

static void clear_live_row_id_caches(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return;
    }

    for (size_t i = 0U; i < statement->live_row_id_caches.count; ++i) {
        free_live_row_id_cache(statement->live_row_id_caches.entries + i);
    }
    free(statement->live_row_id_caches.entries);
    statement->live_row_id_caches = (mylite_storage_live_row_id_cache_set){0};
}

static void clear_statement_chain_row_payload_caches(mylite_storage_statement *statement) {
    for (mylite_storage_statement *current = statement; current != NULL;
         current = current->parent) {
        clear_row_payload_caches(current);
    }
}

static void clear_row_payload_caches(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return;
    }

    for (size_t i = 0U; i < statement->row_payload_caches.count; ++i) {
        free_row_payload_cache(statement->row_payload_caches.entries + i);
    }
    free(statement->row_payload_caches.entries);
    statement->row_payload_caches = (mylite_storage_row_payload_cache_set){0};
}

static char *copy_string(const char *value) {
    const size_t value_size = strlen(value) + 1U;
    char *copy = (char *)malloc(value_size);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, value_size);
    return copy;
}

static char *copy_filename(const char *filename) {
    return copy_string(filename);
}

static mylite_storage_result read_header(FILE *file, mylite_storage_header *out_header) {
    mylite_storage_statement *statement = active_statement_for_file(file);
    if (statement != NULL && statement->has_current_header) {
        *out_header = statement->current_header;
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_statement *read_statement = active_read_statement_for_file(file);
    if (read_statement != NULL) {
        *out_header = read_statement->header;
        return MYLITE_STORAGE_OK;
    }

    if (active_read_snapshot_has_file(file)) {
        *out_header = active_read_snapshot->header;
        return MYLITE_STORAGE_OK;
    }

    if (active_transaction_journal_snapshot_has_file(file)) {
        *out_header = active_transaction_journal_snapshot.header;
        return MYLITE_STORAGE_OK;
    }

    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_result result = read_page_at(
        file,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        header_page
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    return decode_header_page(header_page, out_header);
}

static mylite_storage_result read_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    unsigned char *out_page
) {
    mylite_storage_statement *statement = active_statement_for_file(file);
    if (statement != NULL && statement->has_current_header &&
        page_id == MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID &&
        page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        encode_header_page(out_page, &statement->current_header);
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_statement *read_statement = active_read_statement_for_file(file);
    if (read_statement != NULL) {
        if (page_id == MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID &&
            page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
            memcpy(out_page, read_statement->header_page, page_size);
            return MYLITE_STORAGE_OK;
        }
        if (read_statement->has_current_catalog_page &&
            page_id == read_statement->current_catalog_root_page &&
            page_size == read_statement->header.page_size) {
            memcpy(out_page, read_statement->current_catalog_page, page_size);
            return MYLITE_STORAGE_OK;
        }
    }

    if (active_read_snapshot_has_file(file)) {
        if (page_id == MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID &&
            page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
            memcpy(out_page, active_read_snapshot->header_page, page_size);
            return MYLITE_STORAGE_OK;
        }
        if (page_id == active_read_snapshot->header.catalog_root_page &&
            page_size == active_read_snapshot->header.page_size) {
            memcpy(out_page, active_read_snapshot->catalog_page, page_size);
            return MYLITE_STORAGE_OK;
        }
    }

    if (active_transaction_journal_snapshot_has_file(file)) {
        if (page_id == MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID &&
            page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
            memcpy(out_page, active_transaction_journal_snapshot.header_page, page_size);
            return MYLITE_STORAGE_OK;
        }
        if (page_id == active_transaction_journal_snapshot.header.catalog_root_page &&
            page_size == active_transaction_journal_snapshot.header.page_size) {
            memcpy(out_page, active_transaction_journal_snapshot.catalog_page, page_size);
            return MYLITE_STORAGE_OK;
        }
    }

    int copied_buffered_page = 0;
    mylite_storage_result result =
        copy_buffered_append_page(file, page_id, page_size, out_page, &copied_buffered_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (copied_buffered_page) {
        return MYLITE_STORAGE_OK;
    }

    off_t offset = 0;
    result = page_offset_for_io(page_id, page_size, &offset);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    return read_file_at(file, offset, out_page, page_size);
}

static mylite_storage_result publish_header(FILE *file, const mylite_storage_header *header) {
    mylite_storage_statement *statement = active_statement_for_file(file);
    if (statement != NULL) {
        if (!statement->has_current_header ||
            header->catalog_root_page != statement->current_header.catalog_root_page ||
            header->catalog_generation != statement->current_header.catalog_generation) {
            clear_statement_chain_catalog_root_caches(statement);
        }
        statement->current_header = *header;
        statement->has_current_header = 1;
        statement->current_header_dirty = 1;
        return MYLITE_STORAGE_OK;
    }

    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    encode_header_page(header_page, header);
    const mylite_storage_result result = write_page_at_raw(
        file,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        header->page_size,
        header_page
    );
    mylite_storage_statement *read_statement = active_read_statement_for_file(file);
    if (result == MYLITE_STORAGE_OK && read_statement != NULL) {
        if (!read_statement->has_current_header ||
            header->catalog_root_page != read_statement->current_header.catalog_root_page ||
            header->catalog_generation != read_statement->current_header.catalog_generation) {
            clear_catalog_root_cache(read_statement);
        }
        read_statement->header = *header;
        read_statement->current_header = *header;
        memcpy(read_statement->header_page, header_page, sizeof(read_statement->header_page));
        read_statement->has_current_header = 1;
    }
    return result;
}

static mylite_storage_result write_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page
) {
    mylite_storage_statement *statement = active_statement_for_file(file);
    if (statement != NULL && page_id == MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID &&
        page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        mylite_storage_header header = {0};
        mylite_storage_result result = decode_header_page(page, &header);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (!statement->has_current_header ||
            header.catalog_root_page != statement->current_header.catalog_root_page ||
            header.catalog_generation != statement->current_header.catalog_generation) {
            clear_statement_chain_catalog_root_caches(statement);
        }
        statement->current_header = header;
        statement->has_current_header = 1;
        statement->current_header_dirty = 1;
        return MYLITE_STORAGE_OK;
    }
    mylite_storage_statement *read_statement = active_read_statement_for_file(file);
    if (read_statement != NULL && page_id == MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID &&
        page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        mylite_storage_header header = {0};
        mylite_storage_result result = decode_header_page(page, &header);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        result = write_page_at_raw(file, page_id, page_size, page);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (!read_statement->has_current_header ||
            header.catalog_root_page != read_statement->current_header.catalog_root_page ||
            header.catalog_generation != read_statement->current_header.catalog_generation) {
            clear_catalog_root_cache(read_statement);
        }
        read_statement->header = header;
        read_statement->current_header = header;
        memcpy(read_statement->header_page, page, sizeof(read_statement->header_page));
        read_statement->has_current_header = 1;
        return MYLITE_STORAGE_OK;
    }
    if (statement != NULL && statement->has_current_header &&
        page_id == statement->current_header.catalog_root_page &&
        page_size == statement->current_header.page_size) {
        clear_statement_chain_catalog_root_caches(statement);
    }
    if (replace_buffered_append_page(file, page_id, page_size, page, 0)) {
        return MYLITE_STORAGE_OK;
    }

    const mylite_storage_result result = write_page_at_raw(file, page_id, page_size, page);
    if (result == MYLITE_STORAGE_OK && read_statement != NULL &&
        read_statement->has_current_header &&
        page_id == read_statement->current_header.catalog_root_page &&
        page_size == read_statement->current_header.page_size &&
        page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        memcpy(read_statement->catalog_page, page, sizeof(read_statement->catalog_page));
        memcpy(
            read_statement->current_catalog_page,
            page,
            sizeof(read_statement->current_catalog_page)
        );
        read_statement->current_catalog_root_page =
            read_statement->current_header.catalog_root_page;
        read_statement->current_catalog_generation =
            read_statement->current_header.catalog_generation;
        read_statement->has_current_catalog_page = 1;
    }
    return result;
}

static mylite_storage_result write_page_at_raw(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page
) {
    off_t offset = 0;
    mylite_storage_result result = page_offset_for_io(page_id, page_size, &offset);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return write_file_at(file, offset, page, page_size);
}

static mylite_storage_result write_pages_at_raw(
    FILE *file,
    unsigned long long first_page_id,
    unsigned page_size,
    const unsigned char *pages,
    size_t page_count
) {
    if (page_count == 0U) {
        return MYLITE_STORAGE_OK;
    }
    if (page_size == 0U) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (page_count > SIZE_MAX / page_size) {
        return MYLITE_STORAGE_FULL;
    }

    int buffered = 0;
    mylite_storage_result result = buffer_append_pages_at_raw(
        file,
        first_page_id,
        page_size,
        pages,
        page_count,
        &buffered
    );
    if (result != MYLITE_STORAGE_OK || buffered) {
        return result;
    }

    off_t offset = 0;
    result = page_offset_for_io(first_page_id, page_size, &offset);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    const size_t write_size = page_count * page_size;
    if ((unsigned long long)write_size >
        (unsigned long long)LONG_MAX - (unsigned long long)offset) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    return write_file_at(file, offset, pages, write_size);
}

static mylite_storage_result buffer_append_pages_at_raw(
    FILE *file,
    unsigned long long first_page_id,
    unsigned page_size,
    const unsigned char *pages,
    size_t page_count,
    int *out_buffered
) {
    *out_buffered = 0;
    if (page_size != MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_statement *active = active_statement_for_file(file);
    mylite_storage_statement *buffer_statement = append_page_buffer_statement_for_file(file);
    if (active == NULL || buffer_statement == NULL) {
        return MYLITE_STORAGE_OK;
    }
    if (page_count > MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES) {
        return flush_statement_append_page_buffer(buffer_statement);
    }
    if (!active->has_current_header || first_page_id != active->current_header.page_count) {
        return flush_statement_append_page_buffer(buffer_statement);
    }

    mylite_storage_append_page_buffer *buffer = &buffer_statement->append_pages;
    if (buffer->page_count > 0U) {
        if ((unsigned long long)buffer->page_count > ULLONG_MAX - buffer->first_page_id) {
            return MYLITE_STORAGE_FULL;
        }
        const unsigned long long expected_first_page_id =
            buffer->first_page_id + (unsigned long long)buffer->page_count;
        if (first_page_id != expected_first_page_id) {
            mylite_storage_result result =
                flush_statement_append_page_buffer(buffer_statement);
            if (result != MYLITE_STORAGE_OK) {
                return result;
            }
        }
    }

    if (buffer->page_count > 0U &&
        page_count > MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES - buffer->page_count) {
        mylite_storage_result result = flush_statement_append_page_buffer(buffer_statement);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }

    if (buffer->page_count == 0U) {
        buffer->first_page_id = first_page_id;
    }

    const size_t needed_pages = buffer->page_count + page_count;
    mylite_storage_result result =
        ensure_append_page_buffer_capacity(buffer_statement, needed_pages);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    memcpy(
        buffer->pages + (buffer->page_count * MYLITE_STORAGE_FORMAT_PAGE_SIZE),
        pages,
        page_count * MYLITE_STORAGE_FORMAT_PAGE_SIZE
    );
    memset(buffer->checksum_dirty + buffer->page_count, 0, page_count);
    buffer->page_count = needed_pages;
    *out_buffered = 1;

    if (buffer->page_count == MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES) {
        result = flush_statement_append_page_buffer(buffer_statement);
    }
    return result;
}

static mylite_storage_result copy_buffered_append_page(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    unsigned char *out_page,
    int *out_copied
) {
    *out_copied = 0;

    unsigned char *page = buffered_append_page(file, page_id, page_size);
    if (page == NULL) {
        return MYLITE_STORAGE_OK;
    }

    if (buffered_append_page_checksum_dirty(file, page_id, page_size)) {
        mylite_storage_result result = refresh_dirty_buffered_page_checksum(page);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        set_buffered_append_page_checksum_dirty(file, page_id, page_size, 0);
    }

    memcpy(out_page, page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    *out_copied = 1;
    return MYLITE_STORAGE_OK;
}

static int replace_buffered_append_page(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page,
    int checksum_dirty
) {
    mylite_storage_statement *buffer_statement = append_page_buffer_statement_for_file(file);
    unsigned char *buffered_page =
        buffered_append_page_in_statement(buffer_statement, page_id, page_size);
    if (buffered_page == NULL) {
        return 0;
    }

    memcpy(buffered_page, page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    set_buffered_append_page_checksum_dirty_in_statement(
        buffer_statement,
        page_id,
        page_size,
        checksum_dirty
    );
    return 1;
}

static int buffered_append_page_range_contains_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long first_page_id,
    unsigned long long page_count
) {
    if (buffer_statement == NULL || buffer_statement->append_pages.page_count == 0U) {
        return 0;
    }
    if (page_count == 0ULL) {
        return 1;
    }
    if (page_count > ULLONG_MAX - first_page_id) {
        return 0;
    }

    const mylite_storage_append_page_buffer *buffer = &buffer_statement->append_pages;
    if (first_page_id < buffer->first_page_id) {
        return 0;
    }
    if ((unsigned long long)buffer->page_count > ULLONG_MAX - buffer->first_page_id) {
        return 0;
    }
    const unsigned long long buffered_end =
        buffer->first_page_id + (unsigned long long)buffer->page_count;
    return first_page_id + page_count <= buffered_end ? 1 : 0;
}

static mylite_storage_result capture_buffered_page_undo(
    mylite_storage_statement *statement,
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id
) {
    if (statement == NULL || page_id >= statement->header.page_count) {
        return MYLITE_STORAGE_OK;
    }

    for (size_t i = 0U; i < statement->buffered_page_undos.count; ++i) {
        if (statement->buffered_page_undos.entries[i].page_id == page_id) {
            return MYLITE_STORAGE_OK;
        }
    }

    if (statement->buffered_page_undos.count == statement->buffered_page_undos.capacity) {
        const size_t next_capacity = statement->buffered_page_undos.capacity == 0U
                                         ? 4U
                                         : statement->buffered_page_undos.capacity * 2U;
        if (next_capacity <= statement->buffered_page_undos.capacity ||
            next_capacity > SIZE_MAX / sizeof(*statement->buffered_page_undos.entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_buffered_page_undo *entries = (mylite_storage_buffered_page_undo *)realloc(
            statement->buffered_page_undos.entries,
            next_capacity * sizeof(*statement->buffered_page_undos.entries)
        );
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        statement->buffered_page_undos.entries = entries;
        statement->buffered_page_undos.capacity = next_capacity;
    }

    mylite_storage_buffered_page_undo *undo =
        statement->buffered_page_undos.entries + statement->buffered_page_undos.count;
    undo->page_id = page_id;
    const unsigned char *page = buffered_append_page_in_statement(
        buffer_statement,
        page_id,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE
    );
    if (page == NULL) {
        return MYLITE_STORAGE_CORRUPT;
    }
    undo->checksum_dirty = buffered_append_page_checksum_dirty_in_statement(
        buffer_statement,
        page_id,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE
    );
    undo->used_size = buffered_page_undo_used_size(page);
    memcpy(undo->page, page, undo->used_size);
    ++statement->buffered_page_undos.count;
    return MYLITE_STORAGE_OK;
}

static size_t buffered_page_undo_used_size(const unsigned char *page) {
    if (is_row_page(page)) {
        const size_t row_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_SIZE_OFFSET);
        if (row_size <=
            MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET) {
            return MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET + row_size;
        }
    }
    if (is_index_entry_page(page)) {
        const size_t key_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_KEY_SIZE_OFFSET);
        if (key_size <= MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET) {
            return MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET + key_size;
        }
    }
    return MYLITE_STORAGE_FORMAT_PAGE_SIZE;
}

static unsigned char *buffered_append_page(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size
) {
    return buffered_append_page_in_statement(
        append_page_buffer_statement_for_file(file),
        page_id,
        page_size
    );
}

static unsigned char *buffered_append_page_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id,
    unsigned page_size
) {
    if (page_size != MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return NULL;
    }

    if (buffer_statement == NULL || buffer_statement->append_pages.page_count == 0U) {
        return NULL;
    }

    mylite_storage_append_page_buffer *buffer = &buffer_statement->append_pages;
    if (buffer->checksum_dirty == NULL) {
        return NULL;
    }
    if (page_id < buffer->first_page_id) {
        return NULL;
    }
    const unsigned long long offset_pages = page_id - buffer->first_page_id;
    if (offset_pages >= (unsigned long long)buffer->page_count) {
        return NULL;
    }
    return buffer->pages + ((size_t)offset_pages * MYLITE_STORAGE_FORMAT_PAGE_SIZE);
}

static int buffered_append_page_checksum_dirty(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size
) {
    return buffered_append_page_checksum_dirty_in_statement(
        append_page_buffer_statement_for_file(file),
        page_id,
        page_size
    );
}

static int buffered_append_page_checksum_dirty_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id,
    unsigned page_size
) {
    const unsigned char *slot =
        buffered_append_page_checksum_dirty_slot_in_statement(buffer_statement, page_id, page_size);
    return slot != NULL && *slot != 0U;
}

static void set_buffered_append_page_checksum_dirty(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    int checksum_dirty
) {
    set_buffered_append_page_checksum_dirty_in_statement(
        append_page_buffer_statement_for_file(file),
        page_id,
        page_size,
        checksum_dirty
    );
}

static void set_buffered_append_page_checksum_dirty_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id,
    unsigned page_size,
    int checksum_dirty
) {
    unsigned char *slot =
        buffered_append_page_checksum_dirty_slot_in_statement(buffer_statement, page_id, page_size);
    if (slot != NULL) {
        *slot = checksum_dirty ? 1U : 0U;
    }
}

static unsigned char *buffered_append_page_checksum_dirty_slot_in_statement(
    mylite_storage_statement *buffer_statement,
    unsigned long long page_id,
    unsigned page_size
) {
    if (page_size != MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return NULL;
    }

    if (buffer_statement == NULL || buffer_statement->append_pages.page_count == 0U) {
        return NULL;
    }

    mylite_storage_append_page_buffer *buffer = &buffer_statement->append_pages;
    if (page_id < buffer->first_page_id) {
        return NULL;
    }
    const unsigned long long offset_pages = page_id - buffer->first_page_id;
    if (offset_pages >= (unsigned long long)buffer->page_count) {
        return NULL;
    }
    return buffer->checksum_dirty + (size_t)offset_pages;
}

static mylite_storage_result refresh_dirty_buffered_page_checksum(unsigned char *page) {
    if (is_row_page(page)) {
        const size_t row_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_SIZE_OFFSET);
        if (row_size == 0U ||
            row_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET) {
            return MYLITE_STORAGE_CORRUPT;
        }
        put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET, 0ULL);
        put_u64_le(
            page,
            MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET,
            checksum_page_zero_tail(
                page,
                MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET,
                MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET + row_size
            )
        );
        return MYLITE_STORAGE_OK;
    }
    if (is_index_entry_page(page)) {
        const size_t key_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_KEY_SIZE_OFFSET);
        if (key_size == 0U ||
            key_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET) {
            return MYLITE_STORAGE_CORRUPT;
        }
        put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET, 0ULL);
        put_u64_le(
            page,
            MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET,
            checksum_page_zero_tail(
                page,
                MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET,
                MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET + key_size
            )
        );
        return MYLITE_STORAGE_OK;
    }
    return MYLITE_STORAGE_CORRUPT;
}

static int buffered_update_rewrite_row_state_known(
    mylite_storage_statement *statement,
    unsigned long long row_id
) {
    if (statement == NULL) {
        return 0;
    }

    return find_buffered_update_rewrite_bucket(&statement->buffered_update_rewrites, row_id) !=
           NULL;
}

static int buffered_update_rewrite_shape_known(
    mylite_storage_statement *statement,
    unsigned long long row_id,
    unsigned long long table_id,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed
) {
    if (statement == NULL) {
        return 0;
    }

    const mylite_storage_buffered_update_rewrite_bucket *bucket =
        find_buffered_update_rewrite_bucket(&statement->buffered_update_rewrites, row_id);
    if (bucket == NULL || !bucket->has_shape || bucket->table_id != table_id) {
        return 0;
    }

    size_t changed_index = 0U;
    for (size_t i = 0U; i < index_entry_count; ++i) {
        if (!is_index_entry_changed(index_entry_changed, i)) {
            continue;
        }
        if (changed_index >= bucket->changed_index_count ||
            changed_index >= MYLITE_STORAGE_BUFFERED_UPDATE_REWRITE_INLINE_INDEXES) {
            return 0;
        }
        if (bucket->index_numbers[changed_index] != index_entries[i].index_number ||
            bucket->key_sizes[changed_index] != index_entries[i].key_size) {
            return 0;
        }
        ++changed_index;
    }
    return changed_index == bucket->changed_index_count ? 1 : 0;
}

static mylite_storage_result mark_buffered_update_rewrite_row_state(
    mylite_storage_statement *statement,
    unsigned long long row_id
) {
    if (statement == NULL || buffered_update_rewrite_row_state_known(statement, row_id)) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_buffered_update_rewrite_cache *cache = &statement->buffered_update_rewrites;
    mylite_storage_result result = ensure_buffered_update_rewrite_buckets(cache, cache->count + 1U);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    const mylite_storage_buffered_update_rewrite_bucket entry = {
        .row_id = row_id,
        .occupied = 1,
    };
    if (!place_buffered_update_rewrite_bucket(cache->buckets, cache->bucket_capacity, &entry)) {
        return MYLITE_STORAGE_FULL;
    }
    ++cache->count;
    return MYLITE_STORAGE_OK;
}

static void mark_buffered_update_rewrite_shape(
    mylite_storage_statement *statement,
    unsigned long long row_id,
    unsigned long long table_id,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed
) {
    if (statement == NULL) {
        return;
    }

    size_t changed_index_count = 0U;
    for (size_t i = 0U; i < index_entry_count; ++i) {
        if (is_index_entry_changed(index_entry_changed, i)) {
            ++changed_index_count;
        }
    }
    if (changed_index_count > MYLITE_STORAGE_BUFFERED_UPDATE_REWRITE_INLINE_INDEXES) {
        (void)mark_buffered_update_rewrite_row_state(statement, row_id);
        mylite_storage_buffered_update_rewrite_bucket *bucket =
            find_buffered_update_rewrite_bucket(&statement->buffered_update_rewrites, row_id);
        if (bucket != NULL) {
            bucket->has_shape = 0;
        }
        return;
    }

    mylite_storage_buffered_update_rewrite_cache *cache = &statement->buffered_update_rewrites;
    mylite_storage_buffered_update_rewrite_bucket *bucket =
        find_buffered_update_rewrite_bucket(cache, row_id);
    if (bucket == NULL) {
        mylite_storage_result result =
            ensure_buffered_update_rewrite_buckets(cache, cache->count + 1U);
        if (result != MYLITE_STORAGE_OK) {
            return;
        }
        const mylite_storage_buffered_update_rewrite_bucket entry = {
            .row_id = row_id,
            .occupied = 1,
        };
        if (!place_buffered_update_rewrite_bucket(cache->buckets, cache->bucket_capacity, &entry)) {
            return;
        }
        ++cache->count;
        bucket = find_buffered_update_rewrite_bucket(cache, row_id);
        if (bucket == NULL) {
            return;
        }
    }

    bucket->table_id = table_id;
    bucket->changed_index_count = changed_index_count;
    bucket->has_shape = 1;
    size_t changed_index = 0U;
    for (size_t i = 0U; i < index_entry_count; ++i) {
        if (!is_index_entry_changed(index_entry_changed, i)) {
            continue;
        }
        bucket->index_numbers[changed_index] = index_entries[i].index_number;
        bucket->key_sizes[changed_index] = index_entries[i].key_size;
        ++changed_index;
    }
}

static mylite_storage_result ensure_buffered_update_rewrite_buckets(
    mylite_storage_buffered_update_rewrite_cache *cache,
    size_t next_count
) {
    if (next_count <= cache->count) {
        return MYLITE_STORAGE_OK;
    }
    if (next_count > SIZE_MAX / 2U) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t next_capacity = buffered_update_rewrite_bucket_capacity(next_count);
    if (next_capacity == 0U) {
        return MYLITE_STORAGE_FULL;
    }
    if (cache->bucket_capacity >= next_capacity) {
        return MYLITE_STORAGE_OK;
    }
    return rebuild_buffered_update_rewrite_buckets(cache, next_capacity);
}

static size_t buffered_update_rewrite_bucket_capacity(size_t entry_count) {
    const size_t target_count = entry_count * 2U;
    size_t bucket_capacity = 16U;
    while (bucket_capacity < target_count) {
        if (bucket_capacity > SIZE_MAX / 2U) {
            return 0U;
        }
        bucket_capacity *= 2U;
    }
    return bucket_capacity;
}

static mylite_storage_result rebuild_buffered_update_rewrite_buckets(
    mylite_storage_buffered_update_rewrite_cache *cache,
    size_t bucket_capacity
) {
    if (bucket_capacity > SIZE_MAX / sizeof(*cache->buckets)) {
        return MYLITE_STORAGE_FULL;
    }

    mylite_storage_buffered_update_rewrite_bucket *buckets =
        (mylite_storage_buffered_update_rewrite_bucket *)calloc(bucket_capacity, sizeof(*buckets));
    if (buckets == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    for (size_t i = 0U; i < cache->bucket_capacity; ++i) {
        const mylite_storage_buffered_update_rewrite_bucket *bucket = cache->buckets + i;
        if (!bucket->occupied) {
            continue;
        }
        if (!place_buffered_update_rewrite_bucket(buckets, bucket_capacity, bucket)) {
            free(buckets);
            return MYLITE_STORAGE_FULL;
        }
    }

    free(cache->buckets);
    cache->buckets = buckets;
    cache->bucket_capacity = bucket_capacity;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_buffered_update_rewrite_bucket *find_buffered_update_rewrite_bucket(
    mylite_storage_buffered_update_rewrite_cache *cache,
    unsigned long long row_id
) {
    if (cache == NULL || cache->bucket_capacity == 0U) {
        return NULL;
    }

    const size_t mask = cache->bucket_capacity - 1U;
    size_t bucket_index = hash_row_id(row_id) & mask;
    for (size_t probes = 0U; probes < cache->bucket_capacity; ++probes) {
        mylite_storage_buffered_update_rewrite_bucket *bucket = cache->buckets + bucket_index;
        if (!bucket->occupied) {
            return NULL;
        }
        if (bucket->row_id == row_id) {
            return bucket;
        }
        bucket_index = (bucket_index + 1U) & mask;
    }
    return NULL;
}

static int place_buffered_update_rewrite_bucket(
    mylite_storage_buffered_update_rewrite_bucket *buckets,
    size_t bucket_capacity,
    const mylite_storage_buffered_update_rewrite_bucket *entry
) {
    if (bucket_capacity == 0U || entry == NULL) {
        return 0;
    }

    const size_t mask = bucket_capacity - 1U;
    size_t bucket_index = hash_row_id(entry->row_id) & mask;
    for (size_t probes = 0U; probes < bucket_capacity; ++probes) {
        mylite_storage_buffered_update_rewrite_bucket *bucket = buckets + bucket_index;
        if (!bucket->occupied) {
            *bucket = *entry;
            bucket->occupied = 1;
            return 1;
        }
        if (bucket->row_id == entry->row_id) {
            *bucket = *entry;
            bucket->occupied = 1;
            return 1;
        }
        bucket_index = (bucket_index + 1U) & mask;
    }
    return 0;
}

static mylite_storage_result ensure_append_page_buffer_capacity(
    mylite_storage_statement *statement,
    size_t needed_pages
) {
    if (needed_pages <= statement->append_pages.capacity_pages) {
        return MYLITE_STORAGE_OK;
    }
    if (needed_pages > MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES ||
        needed_pages > SIZE_MAX / MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return MYLITE_STORAGE_FULL;
    }

    size_t capacity_pages = statement->append_pages.capacity_pages;
    if (capacity_pages == 0U) {
        capacity_pages = 16U;
    }
    while (capacity_pages < needed_pages) {
        if (capacity_pages >= MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES / 2U) {
            capacity_pages = MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES;
        } else {
            capacity_pages *= 2U;
        }
    }

    const size_t old_capacity_pages = statement->append_pages.capacity_pages;
    unsigned char *pages = (unsigned char *)realloc(
        statement->append_pages.pages,
        capacity_pages * MYLITE_STORAGE_FORMAT_PAGE_SIZE
    );
    if (pages == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    unsigned char *checksum_dirty =
        (unsigned char *)realloc(statement->append_pages.checksum_dirty, capacity_pages);
    if (checksum_dirty == NULL) {
        statement->append_pages.pages = pages;
        return MYLITE_STORAGE_NOMEM;
    }
    memset(checksum_dirty + old_capacity_pages, 0, capacity_pages - old_capacity_pages);

    statement->append_pages.pages = pages;
    statement->append_pages.checksum_dirty = checksum_dirty;
    statement->append_pages.capacity_pages = capacity_pages;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_file_at(
    FILE *file,
    off_t offset,
    unsigned char *out,
    size_t size
) {
    const int file_descriptor = fileno(file);
    if (file_descriptor < 0) {
        return MYLITE_STORAGE_IOERR;
    }

    size_t bytes_read = 0U;
    while (bytes_read < size) {
        const ssize_t count =
            pread(file_descriptor, out + bytes_read, size - bytes_read, offset + (off_t)bytes_read);
        if (count > 0) {
            bytes_read += (size_t)count;
            continue;
        }
        if (count == 0) {
            return MYLITE_STORAGE_CORRUPT;
        }
        if (errno == EINTR) {
            continue;
        }
        return MYLITE_STORAGE_IOERR;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_file_at(
    FILE *file,
    off_t offset,
    const unsigned char *data,
    size_t size
) {
    const int file_descriptor = fileno(file);
    if (file_descriptor < 0) {
        return MYLITE_STORAGE_IOERR;
    }

    size_t bytes_written = 0U;
    while (bytes_written < size) {
        const ssize_t count = pwrite(
            file_descriptor,
            data + bytes_written,
            size - bytes_written,
            offset + (off_t)bytes_written
        );
        if (count > 0) {
            bytes_written += (size_t)count;
            continue;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return io_error_result_from_errno(errno);
        }
        return MYLITE_STORAGE_IOERR;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result io_error_result_from_errno(int error_number) {
    return is_storage_full_error(error_number) ? MYLITE_STORAGE_FULL : MYLITE_STORAGE_IOERR;
}

static int is_storage_full_error(int error_number) {
    if (error_number == ENOSPC || error_number == EFBIG) {
        return 1;
    }
#ifdef EDQUOT
    if (error_number == EDQUOT) {
        return 1;
    }
#endif
    return 0;
}

static mylite_storage_result page_offset_for_io(
    unsigned long long page_id,
    unsigned page_size,
    off_t *out_offset
) {
    if (page_size == 0U || page_id > ULLONG_MAX / page_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long offset = page_id * (unsigned long long)page_size;
    if (offset > (unsigned long long)LONG_MAX ||
        (unsigned long long)page_size > (unsigned long long)LONG_MAX - offset) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    *out_offset = (off_t)offset;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result decode_header_page(
    const unsigned char *page,
    mylite_storage_header *out_header
) {
    if (memcmp(
            page + MYLITE_STORAGE_FORMAT_HEADER_MAGIC_OFFSET,
            k_header_magic,
            sizeof(k_header_magic)
        ) != 0) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned header_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_FORMAT_VERSION_OFFSET);
    const unsigned page_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_PAGE_SIZE_OFFSET);
    const unsigned byte_order = get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_BYTE_ORDER_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_ALGORITHM_OFFSET);

    if (header_version != MYLITE_STORAGE_FORMAT_HEADER_VERSION ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        page_size != MYLITE_STORAGE_FORMAT_PAGE_SIZE ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    if (byte_order != MYLITE_STORAGE_FORMAT_BYTE_ORDER_MARKER) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET);
    if (expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long catalog_root_page =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_CATALOG_ROOT_PAGE_OFFSET);
    const unsigned long long catalog_generation =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_CATALOG_GENERATION_OFFSET);
    const unsigned long long page_count =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_PAGE_COUNT_OFFSET);
    if (catalog_root_page == 0ULL || catalog_root_page >= page_count ||
        catalog_generation == 0ULL) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_header = (mylite_storage_header){
        .size = sizeof(*out_header),
        .format_version = format_version,
        .header_version = header_version,
        .page_size = page_size,
        .checksum_algorithm = checksum_algorithm,
        .flags = get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_FLAGS_OFFSET),
        .catalog_root_page = catalog_root_page,
        .catalog_generation = catalog_generation,
        .free_list_root_page =
            get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_FREE_LIST_ROOT_PAGE_OFFSET),
        .page_count = page_count,
    };
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_catalog_root_page(
    FILE *file,
    const mylite_storage_header *header
) {
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_result result =
        read_page_at(file, header->catalog_root_page, header->page_size, catalog_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return validate_catalog_root_bytes(catalog_page, header);
}

static mylite_storage_result validate_catalog_root_bytes(
    const unsigned char *page,
    const mylite_storage_header *header
) {
    if (memcmp(
            page + MYLITE_STORAGE_FORMAT_CATALOG_MAGIC_OFFSET,
            k_catalog_magic,
            sizeof(k_catalog_magic)
        ) != 0) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_PAGE_TYPE_OFFSET);
    const unsigned page_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_ALGORITHM_OFFSET);
    if (page_type != MYLITE_STORAGE_FORMAT_CATALOG_PAGE_TYPE_ROOT || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    const unsigned long long page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_PAGE_ID_OFFSET);
    const unsigned long long generation =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET);
    const unsigned long long next_page =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_NEXT_PAGE_OFFSET);
    const size_t used_bytes = catalog_used_bytes(page);
    if (page_id != header->catalog_root_page || generation != header->catalog_generation ||
        next_page != 0ULL || used_bytes < MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE ||
        used_bytes > MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET);
    if (expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }

    return validate_catalog_records(page, header);
}

static mylite_storage_result validate_catalog_records(
    const unsigned char *page,
    const mylite_storage_header *header
) {
    const size_t used_bytes = catalog_used_bytes(page);
    const unsigned long long record_count = catalog_record_count(page);
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        size_t record_size = 0U;
        mylite_storage_result result =
            validate_catalog_record(page + offset, used_bytes - offset, header, &record_size);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        offset += record_size;
    }

    return offset == used_bytes ? MYLITE_STORAGE_OK : MYLITE_STORAGE_CORRUPT;
}

static mylite_storage_result validate_catalog_record(
    const unsigned char *record,
    size_t available_bytes,
    const mylite_storage_header *header,
    size_t *out_record_size
) {
    if (available_bytes < MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned record_type = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET);
    const size_t header_size = record_header_size(record);
    if (available_bytes < header_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    const size_t schema_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET);
    const size_t table_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET);
    const size_t requested_engine_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_REQUESTED_ENGINE_LENGTH_OFFSET);
    const size_t effective_engine_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_EFFECTIVE_ENGINE_LENGTH_OFFSET);
    if (record_size < header_size || record_size > available_bytes || schema_name_size == 0U) {
        return MYLITE_STORAGE_CORRUPT;
    }

    size_t expected_size = header_size;
    if (schema_name_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += schema_name_size;
    if (table_name_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += table_name_size;
    if (requested_engine_name_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += requested_engine_name_size;
    if (effective_engine_name_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += effective_engine_name_size;
    if (record_type == MYLITE_STORAGE_FORMAT_RECORD_TYPE_FOREIGN_KEY) {
        const size_t referenced_table_name_size = get_u32_le(
            record,
            MYLITE_STORAGE_FORMAT_FOREIGN_KEY_RECORD_REFERENCED_TABLE_LENGTH_OFFSET
        );
        if (referenced_table_name_size > SIZE_MAX - expected_size) {
            return MYLITE_STORAGE_CORRUPT;
        }
        expected_size += referenced_table_name_size;
    }

    const unsigned long long definition_root_page =
        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET);
    const unsigned long long definition_size =
        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET);
    const unsigned long long table_id =
        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET);
    if (record_size != expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    if (record_type == MYLITE_STORAGE_FORMAT_RECORD_TYPE_SCHEMA) {
        if (definition_size != 0ULL || table_id != 0ULL || definition_root_page != 0ULL) {
            return MYLITE_STORAGE_CORRUPT;
        }
        *out_record_size = record_size;
        return MYLITE_STORAGE_OK;
    }

    if (record_type == MYLITE_STORAGE_FORMAT_RECORD_TYPE_FOREIGN_KEY) {
        const size_t referenced_table_name_size = get_u32_le(
            record,
            MYLITE_STORAGE_FORMAT_FOREIGN_KEY_RECORD_REFERENCED_TABLE_LENGTH_OFFSET
        );
        if (table_name_size == 0U || requested_engine_name_size == 0U ||
            effective_engine_name_size == 0U || referenced_table_name_size == 0U ||
            definition_size == 0ULL || table_id == 0ULL ||
            definition_root_page <= header->catalog_root_page ||
            definition_root_page >= header->page_count) {
            return MYLITE_STORAGE_CORRUPT;
        }
        *out_record_size = record_size;
        return MYLITE_STORAGE_OK;
    }

    if (record_type == MYLITE_STORAGE_FORMAT_RECORD_TYPE_INDEX_ROOT) {
        if (table_name_size == 0U || requested_engine_name_size != 0U ||
            effective_engine_name_size != 0U || table_id == 0ULL ||
            definition_root_page <= header->catalog_root_page ||
            definition_root_page >= header->page_count) {
            return MYLITE_STORAGE_CORRUPT;
        }
        *out_record_size = record_size;
        return MYLITE_STORAGE_OK;
    }

    if (record_type != MYLITE_STORAGE_FORMAT_RECORD_TYPE_TABLE_DEFINITION ||
        table_name_size == 0U || requested_engine_name_size == 0U ||
        effective_engine_name_size == 0U || definition_size == 0ULL || table_id == 0ULL ||
        definition_root_page <= header->catalog_root_page ||
        definition_root_page >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }
    *out_record_size = record_size;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_table_definition(
    const mylite_storage_table_definition *definition,
    mylite_storage_definition_lengths *out_lengths
) {
    if (definition == NULL || definition->size < sizeof(*definition) ||
        definition->schema_name == NULL || definition->schema_name[0] == '\0' ||
        definition->table_name == NULL || definition->table_name[0] == '\0' ||
        definition->requested_engine_name == NULL || definition->requested_engine_name[0] == '\0' ||
        definition->effective_engine_name == NULL || definition->effective_engine_name[0] == '\0' ||
        definition->definition == NULL || definition->definition_size == 0U) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_lengths = (mylite_storage_definition_lengths){
        .schema_name_size = strlen(definition->schema_name),
        .table_name_size = strlen(definition->table_name),
        .requested_engine_name_size = strlen(definition->requested_engine_name),
        .effective_engine_name_size = strlen(definition->effective_engine_name),
    };
    if (out_lengths->schema_name_size > UINT32_MAX || out_lengths->table_name_size > UINT32_MAX ||
        out_lengths->requested_engine_name_size > UINT32_MAX ||
        out_lengths->effective_engine_name_size > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_schema_definition(
    const mylite_storage_schema_definition *definition,
    mylite_storage_schema_definition_lengths *out_lengths
) {
    if (definition == NULL || definition->size < sizeof(*definition) || out_lengths == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    size_t schema_name_size = 0U;
    mylite_storage_result result = validate_schema_name(definition->schema_name, &schema_name_size);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    *out_lengths = (mylite_storage_schema_definition_lengths){
        .schema_name_size = schema_name_size,
        .default_character_set_name_size = definition->default_character_set_name != NULL
                                               ? strlen(definition->default_character_set_name)
                                               : 0U,
        .default_collation_name_size = definition->default_collation_name != NULL
                                           ? strlen(definition->default_collation_name)
                                           : 0U,
        .schema_comment_size =
            definition->schema_comment != NULL ? strlen(definition->schema_comment) : 0U,
    };

    if (out_lengths->default_character_set_name_size > UINT32_MAX ||
        out_lengths->default_collation_name_size > UINT32_MAX ||
        out_lengths->schema_comment_size > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_foreign_key_definition(
    const mylite_storage_foreign_key_definition *definition,
    mylite_storage_foreign_key_definition_lengths *out_lengths
) {
    if (definition == NULL || definition->size < sizeof(*definition) || out_lengths == NULL ||
        definition->column_count == 0U ||
        definition->column_count > sizeof(unsigned long long) * CHAR_BIT) {
        return MYLITE_STORAGE_MISUSE;
    }

    mylite_storage_result result =
        validate_string_field(definition->schema_name, &out_lengths->schema_name_size);
    if (result == MYLITE_STORAGE_OK) {
        result = validate_string_field(definition->table_name, &out_lengths->table_name_size);
    }
    if (result == MYLITE_STORAGE_OK) {
        result =
            validate_string_field(definition->constraint_name, &out_lengths->constraint_name_size);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_string_field(
            definition->referenced_schema_name,
            &out_lengths->referenced_schema_name_size
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_string_field(
            definition->referenced_table_name,
            &out_lengths->referenced_table_name_size
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        out_lengths->referenced_key_name_size =
            definition->referenced_key_name != NULL ? strlen(definition->referenced_key_name) : 0U;
        if (out_lengths->referenced_key_name_size > UINT32_MAX) {
            result = MYLITE_STORAGE_UNSUPPORTED;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_foreign_key_action(definition->update_action);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_foreign_key_action(definition->delete_action);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_foreign_key_match_option(definition->match_option);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_foreign_key_columns(
            definition->foreign_column_names,
            definition->column_count,
            &out_lengths->foreign_column_names_size
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_foreign_key_columns(
            definition->referenced_column_names,
            definition->column_count,
            &out_lengths->referenced_column_names_size
        );
    }
    return result;
}

static mylite_storage_result validate_index_root_definition(
    const mylite_storage_index_root_definition *definition,
    mylite_storage_index_root_definition_lengths *out_lengths
) {
    if (definition == NULL || definition->size < sizeof(*definition) || out_lengths == NULL ||
        definition->root_page == 0ULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    mylite_storage_result result =
        validate_string_field(definition->schema_name, &out_lengths->schema_name_size);
    if (result == MYLITE_STORAGE_OK) {
        result = validate_string_field(definition->table_name, &out_lengths->table_name_size);
    }
    return result;
}

static mylite_storage_result validate_schema_name(const char *schema_name, size_t *out_length) {
    if (schema_name == NULL || schema_name[0] == '\0' || out_length == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_length = strlen(schema_name);
    if (*out_length > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_string_field(const char *value, size_t *out_length) {
    if (value == NULL || value[0] == '\0' || out_length == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_length = strlen(value);
    if (*out_length > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_foreign_key_action(unsigned action) {
    if (action == MYLITE_STORAGE_FOREIGN_KEY_ACTION_UNSPECIFIED ||
        action == MYLITE_STORAGE_FOREIGN_KEY_ACTION_RESTRICT ||
        action == MYLITE_STORAGE_FOREIGN_KEY_ACTION_CASCADE ||
        action == MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_NULL ||
        action == MYLITE_STORAGE_FOREIGN_KEY_ACTION_NO_ACTION ||
        action == MYLITE_STORAGE_FOREIGN_KEY_ACTION_SET_DEFAULT) {
        return MYLITE_STORAGE_OK;
    }
    return MYLITE_STORAGE_MISUSE;
}

static mylite_storage_result validate_foreign_key_match_option(unsigned match_option) {
    if (match_option == MYLITE_STORAGE_FOREIGN_KEY_MATCH_UNSPECIFIED ||
        match_option == MYLITE_STORAGE_FOREIGN_KEY_MATCH_SIMPLE ||
        match_option == MYLITE_STORAGE_FOREIGN_KEY_MATCH_FULL ||
        match_option == MYLITE_STORAGE_FOREIGN_KEY_MATCH_PARTIAL) {
        return MYLITE_STORAGE_OK;
    }
    return MYLITE_STORAGE_MISUSE;
}

static mylite_storage_result validate_foreign_key_columns(
    const char *const *column_names,
    size_t column_count,
    size_t *out_column_names_size
) {
    if (column_names == NULL || out_column_names_size == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_column_names_size = 0U;
    for (size_t i = 0U; i < column_count; ++i) {
        size_t column_name_size = 0U;
        const mylite_storage_result result =
            validate_string_field(column_names[i], &column_name_size);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (column_name_size == SIZE_MAX ||
            column_name_size + 1U > SIZE_MAX - *out_column_names_size) {
            return MYLITE_STORAGE_FULL;
        }
        *out_column_names_size += column_name_size + 1U;
    }
    if (*out_column_names_size > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_index_entries(
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count
) {
    const size_t key_capacity = MYLITE_STORAGE_MAX_INDEX_KEY_SIZE;
    if (index_entry_count == 0U) {
        return MYLITE_STORAGE_OK;
    }
    if (index_entries == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    for (size_t i = 0U; i < index_entry_count; ++i) {
        if (index_entries[i].size < sizeof(index_entries[i]) || index_entries[i].key == NULL ||
            index_entries[i].key_size == 0U) {
            return MYLITE_STORAGE_MISUSE;
        }
        if (index_entries[i].key_size > key_capacity || index_entries[i].key_size > UINT32_MAX) {
            return MYLITE_STORAGE_UNSUPPORTED;
        }
    }

    return MYLITE_STORAGE_OK;
}

static size_t changed_index_entry_count(
    const unsigned char *index_entry_changed,
    size_t index_entry_count
) {
    if (index_entry_changed == NULL) {
        return index_entry_count;
    }

    size_t changed_count = 0U;
    for (size_t i = 0U; i < index_entry_count; ++i) {
        if (index_entry_changed[i] != 0U) {
            ++changed_count;
        }
    }
    return changed_count;
}

static int is_index_entry_changed(const unsigned char *index_entry_changed, size_t entry_index) {
    return index_entry_changed == NULL || index_entry_changed[entry_index] != 0U;
}

static mylite_storage_result read_catalog_root(
    FILE *file,
    const mylite_storage_header *header,
    unsigned char *out_page
) {
    mylite_storage_statement *statement = active_statement_for_file(file);
    if (statement != NULL && statement->has_current_catalog_page &&
        statement->current_catalog_root_page == header->catalog_root_page &&
        statement->current_catalog_generation == header->catalog_generation &&
        header->page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        memcpy(out_page, statement->current_catalog_page, header->page_size);
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_statement *read_statement = active_read_statement_for_file(file);
    if (read_statement != NULL && read_statement->has_current_catalog_page &&
        read_statement->current_catalog_root_page == header->catalog_root_page &&
        read_statement->current_catalog_generation == header->catalog_generation &&
        header->page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        memcpy(out_page, read_statement->current_catalog_page, header->page_size);
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_result result =
        read_page_at(file, header->catalog_root_page, header->page_size, out_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    result = validate_catalog_root_bytes(out_page, header);
    if (result == MYLITE_STORAGE_OK && header->page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        if (statement != NULL) {
            memcpy(
                statement->current_catalog_page,
                out_page,
                sizeof(statement->current_catalog_page)
            );
            statement->current_catalog_root_page = header->catalog_root_page;
            statement->current_catalog_generation = header->catalog_generation;
            statement->has_current_catalog_page = 1;
        }
        if (read_statement != NULL) {
            memcpy(
                read_statement->current_catalog_page,
                out_page,
                sizeof(read_statement->current_catalog_page)
            );
            read_statement->current_catalog_root_page = header->catalog_root_page;
            read_statement->current_catalog_generation = header->catalog_generation;
            read_statement->has_current_catalog_page = 1;
        }
    }
    return result;
}

static size_t catalog_used_bytes(const unsigned char *page) {
    const unsigned used_bytes = get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET);
    if (used_bytes == 0U && catalog_record_count(page) == 0ULL) {
        return MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    }
    return used_bytes;
}

static unsigned long long catalog_record_count(const unsigned char *page) {
    return get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET);
}

static mylite_storage_result find_table_record(
    const unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    mylite_storage_catalog_entry *out_entry
) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        if (record_is_table(record) && record_matches_table(record, schema_name, table_name)) {
            if (out_entry != NULL) {
                *out_entry = (mylite_storage_catalog_entry){
                    .record = record,
                    .table_id = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET),
                    .definition_root_page = get_u64_le(
                        record,
                        MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET
                    ),
                    .definition_size =
                        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
                };
            }
            return MYLITE_STORAGE_OK;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return MYLITE_STORAGE_NOTFOUND;
}

static mylite_storage_result find_schema_record(
    const unsigned char *catalog_page,
    const char *schema_name
) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        if (record_is_schema(record) && record_matches_schema(record, schema_name)) {
            return MYLITE_STORAGE_OK;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return MYLITE_STORAGE_NOTFOUND;
}

static mylite_storage_result find_foreign_key_record(
    const unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name,
    mylite_storage_catalog_entry *out_entry
) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        if (record_is_foreign_key(record) &&
            record_matches_foreign_key(record, schema_name, table_name, constraint_name)) {
            if (out_entry != NULL) {
                *out_entry = (mylite_storage_catalog_entry){
                    .record = record,
                    .table_id = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET),
                    .definition_root_page = get_u64_le(
                        record,
                        MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET
                    ),
                    .definition_size =
                        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
                };
            }
            return MYLITE_STORAGE_OK;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return MYLITE_STORAGE_NOTFOUND;
}

static mylite_storage_result find_parent_foreign_key_record(
    const unsigned char *catalog_page,
    const char *referenced_schema_name,
    const char *referenced_table_name
) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        if (record_is_foreign_key(record) && record_matches_foreign_key_parent(
                                                 record,
                                                 referenced_schema_name,
                                                 referenced_table_name
                                             )) {
            return MYLITE_STORAGE_OK;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return MYLITE_STORAGE_NOTFOUND;
}

static mylite_storage_result find_index_root_record(
    const unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    unsigned long long table_id,
    unsigned index_number,
    mylite_storage_catalog_entry *out_entry
) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        if (record_matches_index_root(record, schema_name, table_name, table_id, index_number)) {
            if (out_entry != NULL) {
                *out_entry = (mylite_storage_catalog_entry){
                    .record = record,
                    .table_id = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET),
                    .definition_root_page = get_u64_le(
                        record,
                        MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET
                    ),
                    .definition_size =
                        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
                };
            }
            return MYLITE_STORAGE_OK;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return MYLITE_STORAGE_NOTFOUND;
}

static int catalog_has_schema(const unsigned char *catalog_page, const char *schema_name) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        if (record_matches_schema(record, schema_name)) {
            return 1;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return 0;
}

static mylite_storage_result read_table_metadata_from_record(
    const unsigned char *record,
    mylite_storage_table_metadata *out_metadata
) {
    char *requested_engine_name = copy_record_field(record, 2U);
    char *effective_engine_name = copy_record_field(record, 3U);
    if (requested_engine_name == NULL || effective_engine_name == NULL) {
        free(requested_engine_name);
        free(effective_engine_name);
        return MYLITE_STORAGE_NOMEM;
    }

    out_metadata->requested_engine_name = requested_engine_name;
    out_metadata->effective_engine_name = effective_engine_name;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_schema_metadata_from_record(
    const unsigned char *record,
    mylite_storage_schema_metadata *out_metadata
) {
    char *default_character_set_name = copy_record_field(record, 1U);
    char *default_collation_name = copy_record_field(record, 2U);
    char *schema_comment = copy_record_field(record, 3U);
    if (default_character_set_name == NULL || default_collation_name == NULL ||
        schema_comment == NULL) {
        free(default_character_set_name);
        free(default_collation_name);
        free(schema_comment);
        return MYLITE_STORAGE_NOMEM;
    }

    out_metadata->default_character_set_name = default_character_set_name;
    out_metadata->default_collation_name = default_collation_name;
    out_metadata->schema_comment = schema_comment;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result remove_table_record(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name
) {
    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    size_t new_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    unsigned new_record_count = 0U;
    int removed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_is_table(record) && record_matches_table(record, schema_name, table_name)) {
            removed = 1;
        } else {
            memcpy(new_catalog_page + new_offset, record, record_size);
            new_offset += record_size;
            ++new_record_count;
        }
        old_offset += record_size;
    }

    if (!removed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        new_record_count
    );
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)new_offset
    );
    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result remove_foreign_key_record(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name
) {
    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    size_t new_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    unsigned new_record_count = 0U;
    int removed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_is_foreign_key(record) &&
            record_matches_foreign_key(record, schema_name, table_name, constraint_name)) {
            removed = 1;
        } else {
            memcpy(new_catalog_page + new_offset, record, record_size);
            new_offset += record_size;
            ++new_record_count;
        }
        old_offset += record_size;
    }

    if (!removed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        new_record_count
    );
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)new_offset
    );
    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result remove_child_foreign_key_records(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name
) {
    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    size_t new_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    unsigned new_record_count = 0U;
    int removed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_is_foreign_key(record) &&
            record_matches_table(record, schema_name, table_name)) {
            removed = 1;
        } else {
            memcpy(new_catalog_page + new_offset, record, record_size);
            new_offset += record_size;
            ++new_record_count;
        }
        old_offset += record_size;
    }

    if (!removed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        new_record_count
    );
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)new_offset
    );
    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result remove_index_root_record(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    unsigned long long table_id,
    unsigned index_number
) {
    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    size_t new_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    unsigned new_record_count = 0U;
    int removed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_matches_index_root(record, schema_name, table_name, table_id, index_number)) {
            removed = 1;
        } else {
            memcpy(new_catalog_page + new_offset, record, record_size);
            new_offset += record_size;
            ++new_record_count;
        }
        old_offset += record_size;
    }

    if (!removed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        new_record_count
    );
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)new_offset
    );
    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result remove_table_index_root_records(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    unsigned long long table_id
) {
    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    size_t new_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    unsigned new_record_count = 0U;
    int removed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_is_index_root(record) && record_matches_table(record, schema_name, table_name) &&
            get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET) == table_id) {
            removed = 1;
        } else {
            memcpy(new_catalog_page + new_offset, record, record_size);
            new_offset += record_size;
            ++new_record_count;
        }
        old_offset += record_size;
    }

    if (!removed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        new_record_count
    );
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)new_offset
    );
    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result remove_explicit_schema_records(
    unsigned char *catalog_page,
    const char *schema_name
) {
    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    size_t new_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    unsigned new_record_count = 0U;
    int removed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_is_schema(record) && record_matches_schema(record, schema_name)) {
            removed = 1;
        } else {
            memcpy(new_catalog_page + new_offset, record, record_size);
            new_offset += record_size;
            ++new_record_count;
        }
        old_offset += record_size;
    }

    if (!removed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        new_record_count
    );
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)new_offset
    );
    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result remove_schema_records(
    unsigned char *catalog_page,
    const char *schema_name
) {
    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    size_t new_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    unsigned new_record_count = 0U;
    int removed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_matches_schema(record, schema_name)) {
            removed = 1;
        } else {
            memcpy(new_catalog_page + new_offset, record, record_size);
            new_offset += record_size;
            ++new_record_count;
        }
        old_offset += record_size;
    }

    if (!removed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        new_record_count
    );
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)new_offset
    );
    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result rename_table_record(
    unsigned char *catalog_page,
    mylite_storage_table_identity old_identity,
    mylite_storage_table_identity new_identity,
    int preserve_foreign_keys
) {
    mylite_storage_result result =
        find_table_record(catalog_page, old_identity.schema_name, old_identity.table_name, NULL);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    result =
        find_table_record(catalog_page, new_identity.schema_name, new_identity.table_name, NULL);
    if (result == MYLITE_STORAGE_OK) {
        return MYLITE_STORAGE_ERROR;
    }
    if (result != MYLITE_STORAGE_NOTFOUND) {
        return result;
    }

    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    int renamed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_is_table(record) &&
            record_matches_table(record, old_identity.schema_name, old_identity.table_name)) {
            char *requested_engine_name = copy_record_field(record, 2U);
            char *effective_engine_name = copy_record_field(record, 3U);
            if (requested_engine_name == NULL || effective_engine_name == NULL) {
                free(requested_engine_name);
                free(effective_engine_name);
                return MYLITE_STORAGE_NOMEM;
            }

            mylite_storage_table_definition definition = {
                .size = sizeof(definition),
                .schema_name = new_identity.schema_name,
                .table_name = new_identity.table_name,
                .requested_engine_name = requested_engine_name,
                .effective_engine_name = effective_engine_name,
                .definition = (const unsigned char *)"",
                .definition_size =
                    get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
            };
            mylite_storage_definition_lengths lengths = {
                .schema_name_size = strlen(new_identity.schema_name),
                .table_name_size = strlen(new_identity.table_name),
                .requested_engine_name_size = strlen(requested_engine_name),
                .effective_engine_name_size = strlen(effective_engine_name),
            };
            result = append_table_record(
                new_catalog_page,
                &definition,
                &lengths,
                get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET),
                get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET)
            );
            free(requested_engine_name);
            free(effective_engine_name);
            if (result != MYLITE_STORAGE_OK) {
                return result;
            }
            renamed = 1;
        } else if (
            record_is_index_root(record) &&
            record_matches_table(record, old_identity.schema_name, old_identity.table_name)
        ) {
            result = append_renamed_index_root_record(new_catalog_page, record, new_identity);
            if (result != MYLITE_STORAGE_OK) {
                return result;
            }
        } else if (
            !preserve_foreign_keys && record_is_foreign_key(record) &&
            (record_matches_table(record, old_identity.schema_name, old_identity.table_name) ||
             record_matches_foreign_key_parent(
                 record,
                 old_identity.schema_name,
                 old_identity.table_name
             ))
        ) {
            result = append_renamed_foreign_key_record(
                new_catalog_page,
                record,
                old_identity,
                new_identity
            );
            if (result != MYLITE_STORAGE_OK) {
                return result;
            }
        } else {
            const size_t new_offset = catalog_used_bytes(new_catalog_page);
            if (record_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - new_offset) {
                return MYLITE_STORAGE_FULL;
            }
            memcpy(new_catalog_page + new_offset, record, record_size);
            put_u32_le(
                new_catalog_page,
                MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
                (unsigned)(catalog_record_count(new_catalog_page) + 1ULL)
            );
            put_u32_le(
                new_catalog_page,
                MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
                (unsigned)(new_offset + record_size)
            );
        }
        old_offset += record_size;
    }

    if (!renamed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_renamed_foreign_key_record(
    unsigned char *catalog_page,
    const unsigned char *record,
    mylite_storage_table_identity old_identity,
    mylite_storage_table_identity new_identity
) {
    char *schema_name = copy_record_field(record, 0U);
    char *table_name = copy_record_field(record, 1U);
    char *constraint_name = copy_record_field(record, 2U);
    char *referenced_schema_name = copy_record_field(record, 3U);
    char *referenced_table_name = copy_record_field(record, 4U);
    if (schema_name == NULL || table_name == NULL || constraint_name == NULL ||
        referenced_schema_name == NULL || referenced_table_name == NULL) {
        free(schema_name);
        free(table_name);
        free(constraint_name);
        free(referenced_schema_name);
        free(referenced_table_name);
        return MYLITE_STORAGE_NOMEM;
    }

    const int child_matches =
        record_matches_table(record, old_identity.schema_name, old_identity.table_name);
    const int parent_matches = record_matches_foreign_key_parent(
        record,
        old_identity.schema_name,
        old_identity.table_name
    );
    const char *new_schema_name = child_matches ? new_identity.schema_name : schema_name;
    const char *new_table_name = child_matches ? new_identity.table_name : table_name;
    const char *new_referenced_schema_name =
        parent_matches ? new_identity.schema_name : referenced_schema_name;
    const char *new_referenced_table_name =
        parent_matches ? new_identity.table_name : referenced_table_name;
    const mylite_storage_foreign_key_record_fields fields = {
        .schema_name = new_schema_name,
        .schema_name_size = strlen(new_schema_name),
        .table_name = new_table_name,
        .table_name_size = strlen(new_table_name),
        .constraint_name = constraint_name,
        .constraint_name_size = strlen(constraint_name),
        .referenced_schema_name = new_referenced_schema_name,
        .referenced_schema_name_size = strlen(new_referenced_schema_name),
        .referenced_table_name = new_referenced_table_name,
        .referenced_table_name_size = strlen(new_referenced_table_name),
        .metadata_root_page =
            get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET),
        .metadata_size = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
        .table_id = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET),
    };
    const mylite_storage_result result = append_foreign_key_record_fields(catalog_page, &fields);
    free(schema_name);
    free(table_name);
    free(constraint_name);
    free(referenced_schema_name);
    free(referenced_table_name);
    return result;
}

static mylite_storage_result append_renamed_index_root_record(
    unsigned char *catalog_page,
    const unsigned char *record,
    mylite_storage_table_identity new_identity
) {
    const mylite_storage_index_root_record_fields fields = {
        .schema_name = new_identity.schema_name,
        .schema_name_size = strlen(new_identity.schema_name),
        .table_name = new_identity.table_name,
        .table_name_size = strlen(new_identity.table_name),
        .index_number = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_FLAGS_OFFSET),
        .table_id = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET),
        .root_page = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET),
        .entry_count = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
    };
    return append_index_root_record_fields(catalog_page, &fields);
}

static int record_matches_table(
    const unsigned char *record,
    const char *schema_name,
    const char *table_name
) {
    const size_t schema_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET);
    const size_t table_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET);
    const unsigned char *record_schema = record + record_field_offset(record, 0U);
    const unsigned char *record_table = record + record_field_offset(record, 1U);
    return strlen(schema_name) == schema_name_size && strlen(table_name) == table_name_size &&
           memcmp(record_schema, schema_name, schema_name_size) == 0 &&
           memcmp(record_table, table_name, table_name_size) == 0;
}

static int record_matches_foreign_key(
    const unsigned char *record,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name
) {
    const size_t constraint_name_size = record_field_size(record, 2U);
    const unsigned char *record_constraint = record + record_field_offset(record, 2U);
    return record_matches_table(record, schema_name, table_name) &&
           strlen(constraint_name) == constraint_name_size &&
           memcmp(record_constraint, constraint_name, constraint_name_size) == 0;
}

static int record_matches_foreign_key_parent(
    const unsigned char *record,
    const char *referenced_schema_name,
    const char *referenced_table_name
) {
    const size_t referenced_schema_name_size = record_field_size(record, 3U);
    const size_t referenced_table_name_size = record_field_size(record, 4U);
    const unsigned char *record_referenced_schema = record + record_field_offset(record, 3U);
    const unsigned char *record_referenced_table = record + record_field_offset(record, 4U);
    return strlen(referenced_schema_name) == referenced_schema_name_size &&
           strlen(referenced_table_name) == referenced_table_name_size &&
           memcmp(record_referenced_schema, referenced_schema_name, referenced_schema_name_size) ==
               0 &&
           memcmp(record_referenced_table, referenced_table_name, referenced_table_name_size) == 0;
}

static int record_matches_index_root(
    const unsigned char *record,
    const char *schema_name,
    const char *table_name,
    unsigned long long table_id,
    unsigned index_number
) {
    return record_is_index_root(record) && record_matches_table(record, schema_name, table_name) &&
           get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET) == table_id &&
           get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_FLAGS_OFFSET) == index_number;
}

static int record_is_table(const unsigned char *record) {
    return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET) ==
           MYLITE_STORAGE_FORMAT_RECORD_TYPE_TABLE_DEFINITION;
}

static int record_is_schema(const unsigned char *record) {
    return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET) ==
           MYLITE_STORAGE_FORMAT_RECORD_TYPE_SCHEMA;
}

static int record_is_foreign_key(const unsigned char *record) {
    return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET) ==
           MYLITE_STORAGE_FORMAT_RECORD_TYPE_FOREIGN_KEY;
}

static int record_is_index_root(const unsigned char *record) {
    return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET) ==
           MYLITE_STORAGE_FORMAT_RECORD_TYPE_INDEX_ROOT;
}

static int record_matches_schema(const unsigned char *record, const char *schema_name) {
    const size_t schema_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET);
    const unsigned char *record_schema = record + record_field_offset(record, 0U);
    return strlen(schema_name) == schema_name_size &&
           memcmp(record_schema, schema_name, schema_name_size) == 0;
}

static size_t record_header_size(const unsigned char *record) {
    return record_is_foreign_key(record) ? MYLITE_STORAGE_FORMAT_FOREIGN_KEY_RECORD_HEADER_SIZE
                                         : MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
}

static size_t record_field_offset(const unsigned char *record, unsigned field_index) {
    size_t offset = record_header_size(record);
    for (unsigned i = 0U; i < field_index; ++i) {
        offset += record_field_size(record, i);
    }
    return offset;
}

static size_t record_field_size(const unsigned char *record, unsigned field_index) {
    switch (field_index) {
    case 0U:
        return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET);
    case 1U:
        return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET);
    case 2U:
        return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_REQUESTED_ENGINE_LENGTH_OFFSET);
    case 3U:
        return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_EFFECTIVE_ENGINE_LENGTH_OFFSET);
    case 4U:
        return record_is_foreign_key(record)
                   ? get_u32_le(
                         record,
                         MYLITE_STORAGE_FORMAT_FOREIGN_KEY_RECORD_REFERENCED_TABLE_LENGTH_OFFSET
                     )
                   : 0U;
    default:
        return 0U;
    }
}

static mylite_storage_result append_schema_record(
    unsigned char *catalog_page,
    const mylite_storage_schema_definition *definition,
    const mylite_storage_schema_definition_lengths *lengths
) {
    size_t record_size = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    if (lengths->schema_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->schema_name_size;
    if (lengths->default_character_set_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->default_character_set_name_size;
    if (lengths->default_collation_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->default_collation_name_size;
    if (lengths->schema_comment_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->schema_comment_size;
    if (record_size > UINT32_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t used_bytes = catalog_used_bytes(catalog_page);
    const unsigned long long record_count = catalog_record_count(catalog_page);
    if (record_count >= UINT32_MAX || used_bytes > MYLITE_STORAGE_FORMAT_PAGE_SIZE ||
        record_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - used_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned char *record = catalog_page + used_bytes;
    memset(record, 0, record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_SCHEMA
    );
    put_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET, (unsigned)record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET,
        (unsigned)lengths->schema_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET,
        (unsigned)lengths->default_character_set_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_REQUESTED_ENGINE_LENGTH_OFFSET,
        (unsigned)lengths->default_collation_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_EFFECTIVE_ENGINE_LENGTH_OFFSET,
        (unsigned)lengths->schema_comment_size
    );

    size_t field_offset = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    memcpy(record + field_offset, definition->schema_name, lengths->schema_name_size);
    field_offset += lengths->schema_name_size;
    if (lengths->default_character_set_name_size > 0U) {
        memcpy(
            record + field_offset,
            definition->default_character_set_name,
            lengths->default_character_set_name_size
        );
        field_offset += lengths->default_character_set_name_size;
    }
    if (lengths->default_collation_name_size > 0U) {
        memcpy(
            record + field_offset,
            definition->default_collation_name,
            lengths->default_collation_name_size
        );
        field_offset += lengths->default_collation_name_size;
    }
    if (lengths->schema_comment_size > 0U) {
        memcpy(record + field_offset, definition->schema_comment, lengths->schema_comment_size);
    }

    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        (unsigned)(record_count + 1ULL)
    );
    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)(used_bytes + record_size)
    );
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_table_record(
    unsigned char *catalog_page,
    const mylite_storage_table_definition *definition,
    const mylite_storage_definition_lengths *lengths,
    unsigned long long definition_root_page,
    unsigned long long table_id
) {
    size_t record_size = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    if (lengths->schema_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->schema_name_size;
    if (lengths->table_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->table_name_size;
    if (lengths->requested_engine_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->requested_engine_name_size;
    if (lengths->effective_engine_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->effective_engine_name_size;
    if (record_size > UINT32_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t used_bytes = catalog_used_bytes(catalog_page);
    const unsigned long long record_count = catalog_record_count(catalog_page);
    if (record_count >= UINT32_MAX || used_bytes > MYLITE_STORAGE_FORMAT_PAGE_SIZE ||
        record_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - used_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned char *record = catalog_page + used_bytes;
    memset(record, 0, record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_TABLE_DEFINITION
    );
    put_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET, (unsigned)record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET,
        (unsigned)lengths->schema_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET,
        (unsigned)lengths->table_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_REQUESTED_ENGINE_LENGTH_OFFSET,
        (unsigned)lengths->requested_engine_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_EFFECTIVE_ENGINE_LENGTH_OFFSET,
        (unsigned)lengths->effective_engine_name_size
    );
    put_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET, table_id);
    put_u64_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET,
        definition_root_page
    );
    put_u64_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET,
        (unsigned long long)definition->definition_size
    );

    size_t field_offset = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    memcpy(record + field_offset, definition->schema_name, lengths->schema_name_size);
    field_offset += lengths->schema_name_size;
    memcpy(record + field_offset, definition->table_name, lengths->table_name_size);
    field_offset += lengths->table_name_size;
    memcpy(
        record + field_offset,
        definition->requested_engine_name,
        lengths->requested_engine_name_size
    );
    field_offset += lengths->requested_engine_name_size;
    memcpy(
        record + field_offset,
        definition->effective_engine_name,
        lengths->effective_engine_name_size
    );

    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        (unsigned)(record_count + 1ULL)
    );
    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)(used_bytes + record_size)
    );
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_foreign_key_record(
    unsigned char *catalog_page,
    const mylite_storage_foreign_key_definition *definition,
    const mylite_storage_foreign_key_definition_lengths *lengths,
    unsigned long long metadata_root_page,
    unsigned long long metadata_size,
    unsigned long long table_id
) {
    const mylite_storage_foreign_key_record_fields fields = {
        .schema_name = definition->schema_name,
        .schema_name_size = lengths->schema_name_size,
        .table_name = definition->table_name,
        .table_name_size = lengths->table_name_size,
        .constraint_name = definition->constraint_name,
        .constraint_name_size = lengths->constraint_name_size,
        .referenced_schema_name = definition->referenced_schema_name,
        .referenced_schema_name_size = lengths->referenced_schema_name_size,
        .referenced_table_name = definition->referenced_table_name,
        .referenced_table_name_size = lengths->referenced_table_name_size,
        .metadata_root_page = metadata_root_page,
        .metadata_size = metadata_size,
        .table_id = table_id,
    };
    return append_foreign_key_record_fields(catalog_page, &fields);
}

static mylite_storage_result append_foreign_key_record_fields(
    unsigned char *catalog_page,
    const mylite_storage_foreign_key_record_fields *fields
) {
    size_t record_size = MYLITE_STORAGE_FORMAT_FOREIGN_KEY_RECORD_HEADER_SIZE;
    if (fields->schema_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += fields->schema_name_size;
    if (fields->table_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += fields->table_name_size;
    if (fields->constraint_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += fields->constraint_name_size;
    if (fields->referenced_schema_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += fields->referenced_schema_name_size;
    if (fields->referenced_table_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += fields->referenced_table_name_size;
    if (record_size > UINT32_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t used_bytes = catalog_used_bytes(catalog_page);
    const unsigned long long record_count = catalog_record_count(catalog_page);
    if (record_count >= UINT32_MAX || used_bytes > MYLITE_STORAGE_FORMAT_PAGE_SIZE ||
        record_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - used_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned char *record = catalog_page + used_bytes;
    memset(record, 0, record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_FOREIGN_KEY
    );
    put_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET, (unsigned)record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET,
        (unsigned)fields->schema_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET,
        (unsigned)fields->table_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_REQUESTED_ENGINE_LENGTH_OFFSET,
        (unsigned)fields->constraint_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_EFFECTIVE_ENGINE_LENGTH_OFFSET,
        (unsigned)fields->referenced_schema_name_size
    );
    put_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET, fields->table_id);
    put_u64_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET,
        fields->metadata_root_page
    );
    put_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET, fields->metadata_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_RECORD_REFERENCED_TABLE_LENGTH_OFFSET,
        (unsigned)fields->referenced_table_name_size
    );

    size_t field_offset = MYLITE_STORAGE_FORMAT_FOREIGN_KEY_RECORD_HEADER_SIZE;
    memcpy(record + field_offset, fields->schema_name, fields->schema_name_size);
    field_offset += fields->schema_name_size;
    memcpy(record + field_offset, fields->table_name, fields->table_name_size);
    field_offset += fields->table_name_size;
    memcpy(record + field_offset, fields->constraint_name, fields->constraint_name_size);
    field_offset += fields->constraint_name_size;
    memcpy(
        record + field_offset,
        fields->referenced_schema_name,
        fields->referenced_schema_name_size
    );
    field_offset += fields->referenced_schema_name_size;
    memcpy(
        record + field_offset,
        fields->referenced_table_name,
        fields->referenced_table_name_size
    );

    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        (unsigned)(record_count + 1ULL)
    );
    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)(used_bytes + record_size)
    );
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_index_root_record(
    unsigned char *catalog_page,
    const mylite_storage_index_root_definition *definition,
    const mylite_storage_index_root_definition_lengths *lengths,
    unsigned long long table_id
) {
    const mylite_storage_index_root_record_fields fields = {
        .schema_name = definition->schema_name,
        .schema_name_size = lengths->schema_name_size,
        .table_name = definition->table_name,
        .table_name_size = lengths->table_name_size,
        .index_number = definition->index_number,
        .table_id = table_id,
        .root_page = definition->root_page,
        .entry_count = definition->entry_count,
    };
    return append_index_root_record_fields(catalog_page, &fields);
}

static mylite_storage_result append_index_root_record_fields(
    unsigned char *catalog_page,
    const mylite_storage_index_root_record_fields *fields
) {
    size_t record_size = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    if (fields->schema_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += fields->schema_name_size;
    if (fields->table_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += fields->table_name_size;
    if (record_size > UINT32_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t used_bytes = catalog_used_bytes(catalog_page);
    const unsigned long long record_count = catalog_record_count(catalog_page);
    if (record_count >= UINT32_MAX || used_bytes > MYLITE_STORAGE_FORMAT_PAGE_SIZE ||
        record_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - used_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned char *record = catalog_page + used_bytes;
    memset(record, 0, record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_INDEX_ROOT
    );
    put_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET, (unsigned)record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET,
        (unsigned)fields->schema_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET,
        (unsigned)fields->table_name_size
    );
    put_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET, fields->table_id);
    put_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET, fields->root_page);
    put_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET, fields->entry_count);
    put_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_FLAGS_OFFSET, fields->index_number);

    size_t field_offset = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    memcpy(record + field_offset, fields->schema_name, fields->schema_name_size);
    field_offset += fields->schema_name_size;
    memcpy(record + field_offset, fields->table_name, fields->table_name_size);

    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        (unsigned)(record_count + 1ULL)
    );
    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)(used_bytes + record_size)
    );
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result next_table_id(
    FILE *file,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long *out_table_id
) {
    unsigned long long max_table_id = catalog_max_table_id(catalog_page);
    mylite_storage_result result = read_max_row_table_id(file, header, &max_table_id);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (max_table_id == ULLONG_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    *out_table_id = max_table_id + 1ULL;
    return MYLITE_STORAGE_OK;
}

static unsigned long long catalog_max_table_id(const unsigned char *catalog_page) {
    unsigned long long max_table_id = 0ULL;
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        if (record_is_table(record)) {
            const unsigned long long table_id =
                get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET);
            if (table_id > max_table_id) {
                max_table_id = table_id;
            }
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }

    return max_table_id;
}

static mylite_storage_result read_max_row_table_id(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long *inout_table_id
) {
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_row_page row_page = {0};
        mylite_storage_result result = read_row_page(file, header, page_id, page, &row_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (row_page.table_id > *inout_table_id) {
            *inout_table_id = row_page.table_id;
        }
        free(row_page.owned_payload);
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_definition_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    const unsigned char *definition,
    size_t definition_size
) {
    return write_blob_pages(
        file,
        first_page_id,
        (mylite_storage_blob_write){
            .payload = definition,
            .payload_size = definition_size,
            .page_type = MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_TABLE_DEFINITION,
        }
    );
}

static mylite_storage_result write_foreign_key_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    const unsigned char *metadata,
    size_t metadata_size
) {
    return write_blob_pages(
        file,
        first_page_id,
        (mylite_storage_blob_write){
            .payload = metadata,
            .payload_size = metadata_size,
            .page_type = MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_FOREIGN_KEY,
        }
    );
}

static mylite_storage_result encode_foreign_key_metadata(
    const mylite_storage_foreign_key_definition *definition,
    const mylite_storage_foreign_key_definition_lengths *lengths,
    unsigned char **out_metadata,
    size_t *out_metadata_size
) {
    size_t metadata_size = MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_HEADER_SIZE;
    if (lengths->referenced_key_name_size > SIZE_MAX - metadata_size) {
        return MYLITE_STORAGE_FULL;
    }
    metadata_size += lengths->referenced_key_name_size;
    if (lengths->foreign_column_names_size > SIZE_MAX - metadata_size) {
        return MYLITE_STORAGE_FULL;
    }
    metadata_size += lengths->foreign_column_names_size;
    if (lengths->referenced_column_names_size > SIZE_MAX - metadata_size) {
        return MYLITE_STORAGE_FULL;
    }
    metadata_size += lengths->referenced_column_names_size;

    unsigned char *metadata = (unsigned char *)malloc(metadata_size);
    if (metadata == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    memset(metadata, 0, metadata_size);
    put_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_VERSION
    );
    put_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_COLUMN_COUNT_OFFSET,
        (unsigned)definition->column_count
    );
    put_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_REFERENCED_KEY_LENGTH_OFFSET,
        (unsigned)lengths->referenced_key_name_size
    );
    put_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_FOREIGN_COLUMNS_SIZE_OFFSET,
        (unsigned)lengths->foreign_column_names_size
    );
    put_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_REFERENCED_COLUMNS_SIZE_OFFSET,
        (unsigned)lengths->referenced_column_names_size
    );
    put_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_UPDATE_ACTION_OFFSET,
        definition->update_action
    );
    put_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_DELETE_ACTION_OFFSET,
        definition->delete_action
    );
    put_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_MATCH_OPTION_OFFSET,
        definition->match_option
    );
    put_u64_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_NULLABLE_BITMAP_OFFSET,
        definition->nullable_column_bitmap
    );
    put_u64_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_REFERENCED_NULLABLE_BITMAP_OFFSET,
        definition->referenced_nullable_column_bitmap
    );

    size_t offset = MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_HEADER_SIZE;
    if (lengths->referenced_key_name_size > 0U) {
        memcpy(
            metadata + offset,
            definition->referenced_key_name,
            lengths->referenced_key_name_size
        );
        offset += lengths->referenced_key_name_size;
    }
    encode_foreign_key_column_names(
        metadata + offset,
        definition->foreign_column_names,
        definition->column_count
    );
    offset += lengths->foreign_column_names_size;
    encode_foreign_key_column_names(
        metadata + offset,
        definition->referenced_column_names,
        definition->column_count
    );

    *out_metadata = metadata;
    *out_metadata_size = metadata_size;
    return MYLITE_STORAGE_OK;
}

static void encode_foreign_key_column_names(
    unsigned char *metadata,
    const char *const *column_names,
    size_t column_count
) {
    size_t offset = 0U;
    for (size_t i = 0U; i < column_count; ++i) {
        const size_t column_name_size = strlen(column_names[i]);
        memcpy(metadata + offset, column_names[i], column_name_size);
        offset += column_name_size + 1U;
    }
}

static mylite_storage_result write_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    mylite_storage_blob_write blob
) {
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    size_t written = 0U;
    unsigned long long page_id = first_page_id;
    while (written < blob.payload_size) {
        const size_t remaining = blob.payload_size - written;
        const size_t chunk_size = remaining < payload_capacity ? remaining : payload_capacity;
        const unsigned long long next_page_id =
            written + chunk_size < blob.payload_size ? page_id + 1ULL : 0ULL;
        unsigned char blob_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_blob_page(
            blob_page,
            page_id,
            next_page_id,
            blob.payload + written,
            chunk_size,
            blob.page_type
        );

        const mylite_storage_result result =
            write_page_at(file, page_id, MYLITE_STORAGE_FORMAT_PAGE_SIZE, blob_page);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        written += chunk_size;
        ++page_id;
    }

    return MYLITE_STORAGE_OK;
}

static void encode_blob_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long next_page_id,
    const unsigned char *payload,
    size_t payload_size,
    unsigned page_type
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET, k_blob_magic, sizeof(k_blob_magic));
    put_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_OFFSET, page_type);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_BLOB_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_NEXT_PAGE_OFFSET, next_page_id);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_SIZE_OFFSET, (unsigned)payload_size);
    memcpy(page + MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET, payload, payload_size);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET,
        checksum_page_zero_tail(
            page,
            MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET,
            MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET + payload_size
        )
    );
}

static mylite_storage_result find_table_id(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_table_id
) {
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry entry = {0};
    if (find_active_table_entry_cache(filename, header, schema_name, table_name, &entry)) {
        *out_table_id = entry.table_id;
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_result result = read_catalog_root(file, header, catalog_page);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        store_active_table_entry_cache(filename, header, schema_name, table_name, &entry);
        *out_table_id = entry.table_id;
    }
    return result;
}

static void encode_row_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    mylite_storage_row_write row
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(page + MYLITE_STORAGE_FORMAT_ROW_MAGIC_OFFSET, k_row_magic, sizeof(k_row_magic));
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_TABLE_ROWS
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_TABLE_ID_OFFSET, table_id);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_SIZE_OFFSET, (unsigned)row.row_size);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_COUNT_OFFSET, 1U);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_OVERFLOW_ROOT_PAGE_OFFSET, row.overflow_root_page);
    if (row.overflow_root_page == 0ULL) {
        memcpy(page + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET, row.row, row.row_size);
    }
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET, 0ULL);
    const size_t used_size = row.overflow_root_page == 0ULL
                                 ? MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET + row.row_size
                                 : MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET,
        checksum_page_zero_tail(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET, used_size)
    );
}

static mylite_storage_result write_row_payload_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const unsigned char *row,
    size_t row_size,
    mylite_storage_row_write_position *out_position
) {
    const size_t row_payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    const size_t blob_payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    unsigned long long first_blob_page = 0ULL;
    unsigned long long row_page_id = header->page_count;
    if (row_size > row_payload_capacity) {
        const unsigned long long blob_page_count = ((row_size - 1U) / blob_payload_capacity) + 1U;
        first_blob_page = header->page_count;
        if (blob_page_count > ULLONG_MAX - header->page_count) {
            return MYLITE_STORAGE_FULL;
        }
        row_page_id = header->page_count + blob_page_count;
    }
    if (row_page_id == ULLONG_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    if (first_blob_page != 0ULL) {
        mylite_storage_result result = write_blob_pages(
            file,
            first_blob_page,
            (mylite_storage_blob_write){
                .payload = row,
                .payload_size = row_size,
                .page_type = MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_ROW_PAYLOAD,
            }
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }

    unsigned char row_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    encode_row_page(
        row_page,
        row_page_id,
        table_id,
        (mylite_storage_row_write){
            .row = row,
            .row_size = row_size,
            .overflow_root_page = first_blob_page,
        }
    );

    mylite_storage_result result = write_page_at(file, row_page_id, header->page_size, row_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    *out_position = (mylite_storage_row_write_position){
        .row_page_id = row_page_id,
        .next_page_id = row_page_id + 1ULL,
    };
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_inline_update_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long source_row_id,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed,
    mylite_storage_row_write_position *out_position,
    unsigned long long *out_next_page_id,
    int *out_used_fast_path
) {
    *out_used_fast_path = 0;
    const size_t row_payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    if (header->page_size != MYLITE_STORAGE_FORMAT_PAGE_SIZE || row_size > row_payload_capacity) {
        return MYLITE_STORAGE_OK;
    }
    const size_t changed_entry_count =
        changed_index_entry_count(index_entry_changed, index_entry_count);
    if (header->page_count > ULLONG_MAX - 2ULL ||
        (unsigned long long)changed_entry_count > ULLONG_MAX - header->page_count - 2ULL) {
        return MYLITE_STORAGE_FULL;
    }
    if (changed_entry_count > SIZE_MAX - 2U) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t page_count = changed_entry_count + 2U;
    if (page_count > SIZE_MAX / MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return MYLITE_STORAGE_FULL;
    }
    enum { stack_page_count = 4 };
    unsigned char stack_pages[stack_page_count * MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char *pages = stack_pages;
    if (page_count > stack_page_count) {
        pages = (unsigned char *)malloc(page_count * MYLITE_STORAGE_FORMAT_PAGE_SIZE);
        if (pages == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
    }

    const unsigned long long row_page_id = header->page_count;
    const unsigned long long state_page_id = row_page_id + 1ULL;
    encode_row_page(
        pages,
        row_page_id,
        table_id,
        (mylite_storage_row_write){
            .row = row,
            .row_size = row_size,
        }
    );

    const mylite_storage_row_state_page row_state = {
        .table_id = table_id,
        .source_row_id = source_row_id,
        .replacement_row_id = row_page_id,
        .state_kind = MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE,
    };
    encode_row_state_page(pages + MYLITE_STORAGE_FORMAT_PAGE_SIZE, state_page_id, &row_state);

    size_t changed_index = 0U;
    for (size_t i = 0U; i < index_entry_count; ++i) {
        if (!is_index_entry_changed(index_entry_changed, i)) {
            continue;
        }
        encode_index_entry_page(
            pages + ((changed_index + 2U) * MYLITE_STORAGE_FORMAT_PAGE_SIZE),
            state_page_id + 1ULL + (unsigned long long)changed_index,
            table_id,
            row_page_id,
            index_entries + i
        );
        ++changed_index;
    }

    mylite_storage_result result =
        write_pages_at_raw(file, row_page_id, header->page_size, pages, page_count);
    if (pages != stack_pages) {
        free(pages);
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    *out_position = (mylite_storage_row_write_position){
        .row_page_id = row_page_id,
        .next_page_id = state_page_id + 1ULL,
    };
    *out_next_page_id = state_page_id + 1ULL + (unsigned long long)changed_entry_count;
    *out_used_fast_path = 1;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result rewrite_active_update_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    const unsigned char *index_entry_changed,
    int *out_rewritten
) {
    *out_rewritten = 0;

    mylite_storage_statement *statement = active_statement_for_file(file);
    mylite_storage_statement *buffer_statement = append_page_buffer_statement_for_file(file);
    const size_t row_payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    if (statement == NULL || header->page_size != MYLITE_STORAGE_FORMAT_PAGE_SIZE ||
        row_size > row_payload_capacity) {
        return MYLITE_STORAGE_OK;
    }
    const size_t changed_entry_count =
        changed_index_entry_count(index_entry_changed, index_entry_count);
    if (row_id > ULLONG_MAX - 2ULL ||
        (unsigned long long)changed_entry_count > ULLONG_MAX - row_id - 2ULL) {
        return MYLITE_STORAGE_FULL;
    }

    const unsigned long long state_page_id = row_id + 1ULL;
    const unsigned long long first_index_page_id = row_id + 2ULL;
    const unsigned long long rewritten_page_count = 2ULL + (unsigned long long)changed_entry_count;
    if (rewritten_page_count > header->page_count - row_id) {
        return MYLITE_STORAGE_OK;
    }
    if (!buffered_append_page_range_contains_in_statement(
            buffer_statement,
            row_id,
            rewritten_page_count
        )) {
        return MYLITE_STORAGE_OK;
    }
    const int use_cached_shape = buffered_update_rewrite_shape_known(
        buffer_statement,
        row_id,
        table_id,
        index_entries,
        index_entry_count,
        index_entry_changed
    );

    unsigned char *current_page =
        buffered_append_page_in_statement(buffer_statement, row_id, header->page_size);
    if (current_page == NULL) {
        return MYLITE_STORAGE_OK;
    }
    mylite_storage_result result = MYLITE_STORAGE_OK;
    if (!use_cached_shape) {
        mylite_storage_row_page_metadata current_metadata = {0};
        result = decode_buffered_row_page_metadata(header, row_id, current_page, &current_metadata);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            return MYLITE_STORAGE_OK;
        }
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (current_metadata.table_id != table_id || current_metadata.overflow_root_page != 0ULL) {
            return MYLITE_STORAGE_OK;
        }
    }

    const unsigned char *state_page =
        buffered_append_page_in_statement(buffer_statement, state_page_id, header->page_size);
    if (state_page == NULL) {
        return MYLITE_STORAGE_OK;
    }
    if (!use_cached_shape) {
        mylite_storage_row_state_page row_state = {0};
        if (buffered_update_rewrite_row_state_known(buffer_statement, row_id)) {
            result = decode_buffered_row_state_page(header, state_page_id, state_page, &row_state);
        } else {
            result = decode_row_state_page(header, state_page_id, state_page, &row_state);
            if (result == MYLITE_STORAGE_OK) {
                (void)mark_buffered_update_rewrite_row_state(buffer_statement, row_id);
            }
        }
        if (result == MYLITE_STORAGE_NOTFOUND) {
            return MYLITE_STORAGE_OK;
        }
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (row_state.table_id != table_id || row_state.replacement_row_id != row_id ||
            row_state.state_kind != MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE) {
            return MYLITE_STORAGE_OK;
        }
    }

    unsigned char *index_pages_stack[4U];
    unsigned char **index_pages = index_pages_stack;
    if (changed_entry_count > sizeof(index_pages_stack) / sizeof(index_pages_stack[0])) {
        if (changed_entry_count > SIZE_MAX / sizeof(*index_pages)) {
            return MYLITE_STORAGE_FULL;
        }
        index_pages = (unsigned char **)malloc(changed_entry_count * sizeof(*index_pages));
        if (index_pages == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
    }

    int can_rewrite = 1;
    size_t changed_index = 0U;
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < index_entry_count; ++i) {
        if (!is_index_entry_changed(index_entry_changed, i)) {
            continue;
        }
        const unsigned long long index_page_id =
            first_index_page_id + (unsigned long long)changed_index;
        unsigned char *page =
            buffered_append_page_in_statement(buffer_statement, index_page_id, header->page_size);
        mylite_storage_index_entry_page index_page = {0};
        if (page == NULL) {
            result = MYLITE_STORAGE_OK;
            can_rewrite = 0;
            goto done;
        }
        if (!use_cached_shape) {
            result = decode_buffered_index_entry_page(header, index_page_id, page, &index_page);
            if (result == MYLITE_STORAGE_NOTFOUND) {
                result = MYLITE_STORAGE_OK;
                can_rewrite = 0;
                goto done;
            }
            if (result != MYLITE_STORAGE_OK) {
                break;
            }
            if (index_page.table_id != table_id || index_page.row_id != row_id ||
                index_page.index_number != index_entries[i].index_number ||
                index_page.key_size != index_entries[i].key_size) {
                can_rewrite = 0;
                goto done;
            }
        }
        index_pages[changed_index] = page;
        ++changed_index;
    }
    if (result != MYLITE_STORAGE_OK) {
        goto done;
    }
    if (!can_rewrite) {
        goto done;
    }
    if (!use_cached_shape) {
        mark_buffered_update_rewrite_shape(
            buffer_statement,
            row_id,
            table_id,
            index_entries,
            index_entry_count,
            index_entry_changed
        );
    }

    result = capture_buffered_page_undo(statement, buffer_statement, row_id);
    if (result != MYLITE_STORAGE_OK) {
        goto done;
    }

    rewrite_buffered_row_page(current_page, row, row_size);
    set_buffered_append_page_checksum_dirty_in_statement(
        buffer_statement,
        row_id,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        1
    );
    changed_index = 0U;
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < index_entry_count; ++i) {
        if (!is_index_entry_changed(index_entry_changed, i)) {
            continue;
        }
        unsigned char *page = index_pages[changed_index];
        const unsigned char *existing_key = page + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET;
        if (memcmp(existing_key, index_entries[i].key, index_entries[i].key_size) == 0) {
            ++changed_index;
            continue;
        }

        result = capture_buffered_page_undo(
            statement,
            buffer_statement,
            first_index_page_id + (unsigned long long)changed_index
        );
        if (result != MYLITE_STORAGE_OK) {
            break;
        }
        rewrite_buffered_index_entry_page(page, index_entries + i);
        set_buffered_append_page_checksum_dirty_in_statement(
            buffer_statement,
            first_index_page_id + (unsigned long long)changed_index,
            MYLITE_STORAGE_FORMAT_PAGE_SIZE,
            1
        );
        ++changed_index;
    }
    if (result == MYLITE_STORAGE_OK) {
        *out_rewritten = 1;
    }

done:
    if (index_pages != index_pages_stack) {
        free(index_pages);
    }
    return result;
}

static void rewrite_buffered_row_page(
    unsigned char *page,
    const unsigned char *row,
    size_t row_size
) {
    const size_t old_row_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_SIZE_OFFSET);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_SIZE_OFFSET, (unsigned)row_size);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_OVERFLOW_ROOT_PAGE_OFFSET, 0ULL);
    memcpy(page + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET, row, row_size);
    if (old_row_size > row_size) {
        memset(
            page + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET + row_size,
            0,
            old_row_size - row_size
        );
    }
}

static void rewrite_buffered_index_entry_page(
    unsigned char *page,
    const mylite_storage_index_entry *index_entry
) {
    const size_t old_key_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_KEY_SIZE_OFFSET);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_NUMBER_OFFSET, index_entry->index_number);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_KEY_SIZE_OFFSET, (unsigned)index_entry->key_size);
    memcpy(page + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET, index_entry->key, index_entry->key_size);
    if (old_key_size > index_entry->key_size) {
        memset(
            page + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET + index_entry->key_size,
            0,
            old_key_size - index_entry->key_size
        );
    }
}

static mylite_storage_result write_index_entry_pages(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_index_entry_write write,
    unsigned long long *out_next_page_id
) {
    const size_t changed_entry_count =
        changed_index_entry_count(write.index_entry_changed, write.index_entry_count);
    if (changed_entry_count > ULLONG_MAX - write.first_page_id) {
        return MYLITE_STORAGE_FULL;
    }

    size_t changed_index = 0U;
    for (size_t i = 0U; i < write.index_entry_count; ++i) {
        if (!is_index_entry_changed(write.index_entry_changed, i)) {
            continue;
        }
        const unsigned long long page_id = write.first_page_id + (unsigned long long)changed_index;
        unsigned char index_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_index_entry_page(
            index_page,
            page_id,
            write.table_id,
            write.row_id,
            write.index_entries + i
        );

        const mylite_storage_result result =
            write_page_at(file, page_id, header->page_size, index_page);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        ++changed_index;
    }

    *out_next_page_id = write.first_page_id + (unsigned long long)changed_entry_count;
    return MYLITE_STORAGE_OK;
}

static void encode_index_entry_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned long long row_id,
    const mylite_storage_index_entry *index_entry
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(page + MYLITE_STORAGE_FORMAT_INDEX_MAGIC_OFFSET, k_index_magic, sizeof(k_index_magic));
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_TABLE_ID_OFFSET, table_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_ROW_ID_OFFSET, row_id);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_NUMBER_OFFSET, index_entry->index_number);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_KEY_SIZE_OFFSET, (unsigned)index_entry->key_size);
    memcpy(page + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET, index_entry->key, index_entry->key_size);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET,
        checksum_page_zero_tail(
            page,
            MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET,
            MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET + index_entry->key_size
        )
    );
}

static mylite_storage_result decode_index_entry_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_index_entry_page *out_index_entry_page
) {
    if (!is_index_entry_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_row_page(page) || is_autoincrement_page(page) || is_row_state_page(page) ||
            is_index_leaf_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET);
    const unsigned page_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_ID_OFFSET);
    const unsigned long long table_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_TABLE_ID_OFFSET);
    const unsigned long long row_id = get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_ROW_ID_OFFSET);
    const size_t key_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_KEY_SIZE_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET);
    const size_t key_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET;
    if (page_type != MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || row_id <= header->catalog_root_page || row_id >= header->page_count ||
        key_size == 0U || key_size > key_capacity || expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_index_entry_page = (mylite_storage_index_entry_page){
        .table_id = table_id,
        .row_id = row_id,
        .index_number = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_NUMBER_OFFSET),
        .key_size = key_size,
        .key = page + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET,
    };
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result decode_buffered_index_entry_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_index_entry_page *out_index_entry_page
) {
    if (!is_index_entry_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_row_page(page) || is_autoincrement_page(page) || is_row_state_page(page) ||
            is_index_leaf_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET);
    const unsigned page_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_ID_OFFSET);
    const unsigned long long table_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_TABLE_ID_OFFSET);
    const unsigned long long row_id = get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_ROW_ID_OFFSET);
    const size_t key_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_KEY_SIZE_OFFSET);
    const size_t key_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET;
    if (page_type != MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || row_id <= header->catalog_root_page || row_id >= header->page_count ||
        key_size == 0U || key_size > key_capacity) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_index_entry_page = (mylite_storage_index_entry_page){
        .table_id = table_id,
        .row_id = row_id,
        .index_number = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_NUMBER_OFFSET),
        .key_size = key_size,
        .key = page + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET,
    };
    return MYLITE_STORAGE_OK;
}

static int is_index_entry_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_INDEX_MAGIC_OFFSET,
               k_index_magic,
               sizeof(k_index_magic)
           ) == 0 &&
           get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET) ==
               MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX;
}

static int is_index_leaf_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_INDEX_MAGIC_OFFSET,
               k_index_magic,
               sizeof(k_index_magic)
           ) == 0 &&
           get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET) ==
               MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_LEAF;
}

static mylite_storage_result prepare_index_leaf_pages(
    unsigned char **out_pages,
    size_t *out_page_count,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned index_number,
    const mylite_storage_index_entryset *entryset
) {
    *out_pages = NULL;
    *out_page_count = 0U;

    size_t key_size = 0U;
    mylite_storage_result result = index_entryset_fixed_key_size(entryset, &key_size);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    const size_t entry_capacity = index_leaf_entry_capacity(key_size);
    if (entryset->entry_count != 0U && entry_capacity == 0U) {
        return MYLITE_STORAGE_FULL;
    }
    const size_t page_count =
        entryset->entry_count == 0U ? 1U : ((entryset->entry_count - 1U) / entry_capacity) + 1U;
    if (page_count > SIZE_MAX / MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return MYLITE_STORAGE_NOMEM;
    }
    unsigned char *pages = (unsigned char *)calloc(page_count, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    if (pages == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    size_t *order = NULL;
    result = build_raw_index_entry_order(entryset, &order);
    if (result != MYLITE_STORAGE_OK) {
        free(pages);
        return result;
    }

    const size_t cell_size = MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size;
    size_t first_entry = 0U;
    for (size_t i = 0U; i < page_count; ++i) {
        const size_t remaining_entries = entryset->entry_count - first_entry;
        const size_t page_entries =
            remaining_entries < entry_capacity ? remaining_entries : entry_capacity;
        const size_t used_bytes =
            MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET + (page_entries * cell_size);
        encode_index_leaf_page(
            pages + (i * MYLITE_STORAGE_FORMAT_PAGE_SIZE),
            page_id + (unsigned long long)i,
            table_id,
            index_number,
            entryset,
            order,
            first_entry,
            page_entries,
            key_size,
            used_bytes
        );
        first_entry += page_entries;
    }

    free(order);
    *out_pages = pages;
    *out_page_count = page_count;
    return MYLITE_STORAGE_OK;
}

static void encode_index_leaf_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned index_number,
    const mylite_storage_index_entryset *entryset,
    const size_t *order,
    size_t first_entry,
    size_t entry_count,
    size_t key_size,
    size_t used_bytes
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(page + MYLITE_STORAGE_FORMAT_INDEX_MAGIC_OFFSET, k_index_magic, sizeof(k_index_magic));
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_LEAF
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_TABLE_ID_OFFSET, table_id);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_INDEX_NUMBER_OFFSET, index_number);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_KEY_SIZE_OFFSET, (unsigned)key_size);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET, (unsigned)entry_count);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_USED_BYTES_OFFSET, (unsigned)used_bytes);

    const size_t cell_size = MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size;
    for (size_t i = 0U; i < entry_count; ++i) {
        const size_t entry_index = order[first_entry + i];
        unsigned char *cell =
            page + MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET + (i * cell_size);
        put_u64_le(
            cell,
            MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_ROW_ID_OFFSET,
            entryset->row_ids[entry_index]
        );
        memcpy(
            cell + MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_KEY_OFFSET,
            entryset->keys + entryset->key_offsets[entry_index],
            key_size
        );
    }

    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET,
        checksum_page_zero_tail(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET, used_bytes)
    );
}

static size_t index_leaf_entry_capacity(size_t key_size) {
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET;
    const size_t cell_size = MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size;
    if (key_size == 0U || cell_size <= key_size || cell_size > payload_capacity) {
        return 0U;
    }
    return payload_capacity / cell_size;
}

static mylite_storage_result read_index_leaf_page(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    int used_cache = 0;
    mylite_storage_result result = read_cached_durable_index_leaf_page(
        filename,
        header,
        page_id,
        page,
        out_index_leaf_page,
        &used_cache
    );
    if (result != MYLITE_STORAGE_OK || used_cache) {
        return result;
    }

    result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    result = decode_index_leaf_page(header, page_id, page, out_index_leaf_page);
    if (result == MYLITE_STORAGE_OK) {
        store_durable_index_leaf_page(filename, header, page_id, page, out_index_leaf_page);
    }
    return result;
}

static mylite_storage_result decode_index_leaf_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page
) {
    if (!is_index_leaf_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_row_page(page) || is_autoincrement_page(page) || is_row_state_page(page) ||
            is_index_entry_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET);
    const unsigned page_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAGE_ID_OFFSET);
    const unsigned long long table_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_TABLE_ID_OFFSET);
    const unsigned index_number =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_INDEX_NUMBER_OFFSET);
    const size_t key_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_KEY_SIZE_OFFSET);
    const size_t entry_count =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET);
    const size_t used_bytes = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_USED_BYTES_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET);
    const size_t cell_size = MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size;
    if (page_type != MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_LEAF || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || key_size > MYLITE_STORAGE_MAX_INDEX_KEY_SIZE ||
        used_bytes < MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET ||
        used_bytes > MYLITE_STORAGE_FORMAT_PAGE_SIZE || expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if ((entry_count == 0U && used_bytes != MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET) ||
        (entry_count != 0U &&
         (key_size == 0U ||
          entry_count >
              (used_bytes - MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET) / cell_size ||
          MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET + (entry_count * cell_size) !=
              used_bytes))) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_index_leaf_page leaf_page = {
        .table_id = table_id,
        .index_number = index_number,
        .key_size = key_size,
        .entry_count = entry_count,
        .used_bytes = used_bytes,
        .payload = page + MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET,
    };
    for (size_t i = 0U; i < entry_count; ++i) {
        const unsigned long long row_id = index_leaf_entry_row_id(&leaf_page, i);
        if (row_id <= header->catalog_root_page || row_id >= header->page_count) {
            return MYLITE_STORAGE_CORRUPT;
        }
        if (i != 0U) {
            const unsigned char *previous_key = index_leaf_entry_key(&leaf_page, i - 1U);
            const unsigned char *current_key = index_leaf_entry_key(&leaf_page, i);
            int cmp = memcmp(previous_key, current_key, key_size);
            if (cmp == 0) {
                const unsigned long long previous_row_id =
                    index_leaf_entry_row_id(&leaf_page, i - 1U);
                const unsigned long long current_row_id = row_id;
                if (previous_row_id > current_row_id) {
                    return MYLITE_STORAGE_CORRUPT;
                }
            } else if (cmp > 0) {
                return MYLITE_STORAGE_CORRUPT;
            }
        }
    }

    *out_index_leaf_page = leaf_page;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_live_index_entries(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    mylite_storage_index_entryset *out_entries
) {
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_result result = MYLITE_STORAGE_OK;
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         result == MYLITE_STORAGE_OK && page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        result = read_page_at(file, page_id, header->page_size, page);
        if (result != MYLITE_STORAGE_OK) {
            break;
        }

        if (is_index_entry_page(page)) {
            mylite_storage_index_entry_page entry_page = {0};
            result = decode_index_entry_page(header, page_id, page, &entry_page);
            if (result == MYLITE_STORAGE_OK && entry_page.table_id == table_id &&
                entry_page.index_number == index_number &&
                find_row_state_entry(&row_state_map, entry_page.row_id) == NULL) {
                remove_index_entries_by_row_id(out_entries, entry_page.row_id);
                result = append_index_entry_to_entryset(out_entries, &entry_page);
            }
            continue;
        }

        if (is_row_state_page(page)) {
            mylite_storage_row_state_page row_state_page = {0};
            result = decode_row_state_page(header, page_id, page, &row_state_page);
            if (result == MYLITE_STORAGE_OK && row_state_page.table_id == table_id) {
                result = set_row_state_entry(&row_state_map, &row_state_page);
                if (result == MYLITE_STORAGE_OK) {
                    if (row_state_page.state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE) {
                        replace_index_entries_row_id(
                            out_entries,
                            row_state_page.source_row_id,
                            row_state_page.replacement_row_id
                        );
                    } else {
                        remove_index_entries_by_row_id(out_entries, row_state_page.source_row_id);
                    }
                }
            }
            continue;
        }

        if (!is_exact_index_scan_skip_page(page)) {
            result = MYLITE_STORAGE_CORRUPT;
        }
    }

    free_row_state_map(&row_state_map);
    return result;
}

static mylite_storage_result read_index_leaf_exact_entries(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long table_id,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries,
    int *out_used_leaf
) {
    *out_used_leaf = 0;

    mylite_storage_catalog_entry root_entry = {0};
    mylite_storage_result result = find_index_root_record(
        catalog_page,
        schema_name,
        table_name,
        table_id,
        index_number,
        &root_entry
    );
    if (result == MYLITE_STORAGE_NOTFOUND) {
        return MYLITE_STORAGE_OK;
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_index_leaf_page leaf_page = {0};
    mylite_storage_index_leaf_run leaf_run = {0};
    result = read_index_leaf_run_root(
        file,
        filename,
        header,
        &root_entry,
        table_id,
        index_number,
        page,
        &leaf_page,
        &leaf_run
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    *out_used_leaf = 1;
    result = append_index_leaf_run_matches_to_entryset(
        file,
        filename,
        header,
        &leaf_run,
        table_id,
        index_number,
        key,
        key_size,
        out_entries
    );
    if (result == MYLITE_STORAGE_OK && leaf_run.tail_page_id < header->page_count) {
        result = scan_exact_index_entries_from(
            file,
            header,
            table_id,
            index_number,
            key,
            key_size,
            leaf_run.tail_page_id,
            out_entries
        );
    }
    return result;
}

static mylite_storage_result read_index_leaf_exact_row_ids(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long table_id,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_row_id_list *out_row_ids,
    int *out_used_leaf
) {
    *out_used_leaf = 0;

    mylite_storage_catalog_entry root_entry = {0};
    mylite_storage_result result = find_index_root_record(
        catalog_page,
        schema_name,
        table_name,
        table_id,
        index_number,
        &root_entry
    );
    if (result == MYLITE_STORAGE_NOTFOUND) {
        return MYLITE_STORAGE_OK;
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_index_leaf_page leaf_page = {0};
    mylite_storage_index_leaf_run leaf_run = {0};
    result = read_index_leaf_run_root(
        file,
        filename,
        header,
        &root_entry,
        table_id,
        index_number,
        page,
        &leaf_page,
        &leaf_run
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    *out_used_leaf = 1;
    result = append_index_leaf_run_matches_to_row_id_list(
        file,
        filename,
        header,
        &leaf_run,
        table_id,
        index_number,
        key,
        key_size,
        out_row_ids
    );
    if (result == MYLITE_STORAGE_OK && leaf_run.tail_page_id < header->page_count) {
        result = scan_exact_index_row_ids_from(
            file,
            header,
            table_id,
            index_number,
            key,
            key_size,
            leaf_run.tail_page_id,
            out_row_ids
        );
    }
    return result;
}

static mylite_storage_result read_index_leaf_run_root(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_catalog_entry *root_entry,
    unsigned long long table_id,
    unsigned index_number,
    unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page,
    mylite_storage_index_leaf_run *out_leaf_run
) {
    mylite_storage_result result = read_index_leaf_page(
        file,
        filename,
        header,
        root_entry->definition_root_page,
        page,
        out_index_leaf_page
    );
    if (result == MYLITE_STORAGE_NOTFOUND) {
        result = MYLITE_STORAGE_CORRUPT;
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (out_index_leaf_page->table_id != table_id ||
        out_index_leaf_page->index_number != index_number) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_index_leaf_run leaf_run = {
        .first_page_id = root_entry->definition_root_page,
        .page_count = 1ULL,
        .entry_count = root_entry->definition_size,
        .key_size = out_index_leaf_page->key_size,
    };
    if (root_entry->definition_size != 0ULL) {
        leaf_run.entry_capacity = index_leaf_entry_capacity(out_index_leaf_page->key_size);
        if (leaf_run.entry_capacity == 0U) {
            return MYLITE_STORAGE_CORRUPT;
        }
        leaf_run.page_count =
            ((root_entry->definition_size - 1ULL) / (unsigned long long)leaf_run.entry_capacity) +
            1ULL;
    }
    if (leaf_run.page_count == 0ULL ||
        leaf_run.page_count > header->page_count - root_entry->definition_root_page) {
        return MYLITE_STORAGE_CORRUPT;
    }
    leaf_run.tail_page_id = leaf_run.first_page_id + leaf_run.page_count;

    if (out_index_leaf_page->entry_count != index_leaf_run_expected_entry_count(&leaf_run, 0ULL)) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_leaf_run = leaf_run;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_index_leaf_run_page(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long table_id,
    unsigned index_number,
    unsigned long long page_offset,
    unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page
) {
    if (page_offset >= leaf_run->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_index_leaf_page(
        file,
        filename,
        header,
        leaf_run->first_page_id + page_offset,
        page,
        out_index_leaf_page
    );
    if (result == MYLITE_STORAGE_NOTFOUND) {
        result = MYLITE_STORAGE_CORRUPT;
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (out_index_leaf_page->table_id != table_id ||
        out_index_leaf_page->index_number != index_number ||
        out_index_leaf_page->key_size != leaf_run->key_size ||
        out_index_leaf_page->entry_count !=
            index_leaf_run_expected_entry_count(leaf_run, page_offset)) {
        return MYLITE_STORAGE_CORRUPT;
    }
    return MYLITE_STORAGE_OK;
}

static size_t index_leaf_run_expected_entry_count(
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long page_offset
) {
    if (leaf_run->entry_count == 0ULL) {
        return 0U;
    }
    if (page_offset + 1ULL < leaf_run->page_count) {
        return leaf_run->entry_capacity;
    }
    return (size_t)(leaf_run->entry_count -
                    (page_offset * (unsigned long long)leaf_run->entry_capacity));
}

static mylite_storage_result append_index_leaf_run_matches_to_row_id_list(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_row_id_list *out_row_ids
) {
    unsigned long long match_page_offset = 0ULL;
    int found = 0;
    mylite_storage_result result = find_index_leaf_run_match_page(
        file,
        filename,
        header,
        leaf_run,
        table_id,
        index_number,
        key,
        key_size,
        &match_page_offset,
        &found
    );
    if (result != MYLITE_STORAGE_OK || !found) {
        return result;
    }

    unsigned long long first_match_page_offset = match_page_offset;
    while (first_match_page_offset > 0ULL) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_index_leaf_page leaf_page = {0};
        result = read_index_leaf_run_page(
            file,
            filename,
            header,
            leaf_run,
            table_id,
            index_number,
            first_match_page_offset - 1ULL,
            page,
            &leaf_page
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (!index_leaf_page_last_key_matches(&leaf_page, key, key_size)) {
            break;
        }
        --first_match_page_offset;
    }

    for (unsigned long long page_offset = first_match_page_offset;
         page_offset < leaf_run->page_count;
         ++page_offset) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_index_leaf_page leaf_page = {0};
        result = read_index_leaf_run_page(
            file,
            filename,
            header,
            leaf_run,
            table_id,
            index_number,
            page_offset,
            page,
            &leaf_page
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }

        const int range_cmp = compare_index_leaf_page_key_range(&leaf_page, key, key_size);
        if (range_cmp < 0) {
            break;
        }
        if (range_cmp > 0 || (page_offset != first_match_page_offset &&
                              !index_leaf_page_first_key_matches(&leaf_page, key, key_size))) {
            return MYLITE_STORAGE_CORRUPT;
        }
        if (range_cmp == 0) {
            result =
                append_index_leaf_matches_to_row_id_list(&leaf_page, key, key_size, out_row_ids);
            if (result != MYLITE_STORAGE_OK) {
                return result;
            }
        }
        if (!index_leaf_page_last_key_matches(&leaf_page, key, key_size)) {
            break;
        }
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_index_leaf_run_matches_to_entryset(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries
) {
    unsigned long long match_page_offset = 0ULL;
    int found = 0;
    mylite_storage_result result = find_index_leaf_run_match_page(
        file,
        filename,
        header,
        leaf_run,
        table_id,
        index_number,
        key,
        key_size,
        &match_page_offset,
        &found
    );
    if (result != MYLITE_STORAGE_OK || !found) {
        return result;
    }

    unsigned long long first_match_page_offset = match_page_offset;
    while (first_match_page_offset > 0ULL) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_index_leaf_page leaf_page = {0};
        result = read_index_leaf_run_page(
            file,
            filename,
            header,
            leaf_run,
            table_id,
            index_number,
            first_match_page_offset - 1ULL,
            page,
            &leaf_page
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (!index_leaf_page_last_key_matches(&leaf_page, key, key_size)) {
            break;
        }
        --first_match_page_offset;
    }

    for (unsigned long long page_offset = first_match_page_offset;
         page_offset < leaf_run->page_count;
         ++page_offset) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_index_leaf_page leaf_page = {0};
        result = read_index_leaf_run_page(
            file,
            filename,
            header,
            leaf_run,
            table_id,
            index_number,
            page_offset,
            page,
            &leaf_page
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }

        const int range_cmp = compare_index_leaf_page_key_range(&leaf_page, key, key_size);
        if (range_cmp < 0) {
            break;
        }
        if (range_cmp > 0 || (page_offset != first_match_page_offset &&
                              !index_leaf_page_first_key_matches(&leaf_page, key, key_size))) {
            return MYLITE_STORAGE_CORRUPT;
        }
        if (range_cmp == 0) {
            result = append_index_leaf_matches_to_entryset(&leaf_page, key, key_size, out_entries);
            if (result != MYLITE_STORAGE_OK) {
                return result;
            }
        }
        if (!index_leaf_page_last_key_matches(&leaf_page, key, key_size)) {
            break;
        }
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result find_index_leaf_run_match_page(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const mylite_storage_index_leaf_run *leaf_run,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_page_offset,
    int *out_found
) {
    *out_page_offset = 0ULL;
    *out_found = 0;
    if (leaf_run->entry_count == 0ULL || key_size != leaf_run->key_size) {
        return MYLITE_STORAGE_OK;
    }

    unsigned long long lower = 0ULL;
    unsigned long long upper = leaf_run->page_count;
    while (lower < upper) {
        const unsigned long long mid = lower + ((upper - lower) / 2ULL);
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_index_leaf_page leaf_page = {0};
        mylite_storage_result result = read_index_leaf_run_page(
            file,
            filename,
            header,
            leaf_run,
            table_id,
            index_number,
            mid,
            page,
            &leaf_page
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }

        const int range_cmp = compare_index_leaf_page_key_range(&leaf_page, key, key_size);
        if (range_cmp < 0) {
            upper = mid;
        } else if (range_cmp > 0) {
            lower = mid + 1ULL;
        } else {
            *out_page_offset = mid;
            *out_found = 1;
            return MYLITE_STORAGE_OK;
        }
    }
    return MYLITE_STORAGE_OK;
}

static int compare_index_leaf_page_key_range(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size
) {
    if (leaf_page->entry_count == 0U || key_size != leaf_page->key_size) {
        return 1;
    }
    if (compare_leaf_key(leaf_page, 0U, key, key_size) > 0) {
        return -1;
    }
    if (compare_leaf_key(leaf_page, leaf_page->entry_count - 1U, key, key_size) < 0) {
        return 1;
    }
    return 0;
}

static int index_leaf_page_first_key_matches(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size
) {
    return leaf_page->entry_count != 0U && key_size == leaf_page->key_size &&
           compare_leaf_key(leaf_page, 0U, key, key_size) == 0;
}

static int index_leaf_page_last_key_matches(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size
) {
    return leaf_page->entry_count != 0U && key_size == leaf_page->key_size &&
           compare_leaf_key(leaf_page, leaf_page->entry_count - 1U, key, key_size) == 0;
}

static mylite_storage_result append_index_leaf_matches_to_row_id_list(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_row_id_list *out_row_ids
) {
    if (key_size != leaf_page->key_size) {
        return MYLITE_STORAGE_OK;
    }

    size_t lower = 0U;
    size_t upper = leaf_page->entry_count;
    while (lower < upper) {
        const size_t mid = lower + ((upper - lower) / 2U);
        if (compare_leaf_key(leaf_page, mid, key, key_size) < 0) {
            lower = mid + 1U;
        } else {
            upper = mid;
        }
    }

    for (size_t i = lower; i < leaf_page->entry_count; ++i) {
        if (compare_leaf_key(leaf_page, i, key, key_size) != 0) {
            break;
        }
        const mylite_storage_result result =
            append_row_id_to_list(out_row_ids, index_leaf_entry_row_id(leaf_page, i));
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_index_leaf_matches_to_entryset(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries
) {
    if (key_size != leaf_page->key_size) {
        return MYLITE_STORAGE_OK;
    }

    size_t lower = 0U;
    size_t upper = leaf_page->entry_count;
    while (lower < upper) {
        const size_t mid = lower + ((upper - lower) / 2U);
        if (compare_leaf_key(leaf_page, mid, key, key_size) < 0) {
            lower = mid + 1U;
        } else {
            upper = mid;
        }
    }

    size_t match_count = 0U;
    for (size_t i = lower; i < leaf_page->entry_count; ++i) {
        if (compare_leaf_key(leaf_page, i, key, key_size) != 0) {
            break;
        }
        ++match_count;
    }
    if (match_count == 0U) {
        return MYLITE_STORAGE_OK;
    }
    if (key_size != 0U && match_count > SIZE_MAX / key_size) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t additional_key_bytes = match_count * key_size;
    size_t first_entry = 0U;
    size_t first_key_offset = 0U;
    mylite_storage_result result = grow_index_entryset_for_append(
        out_entries,
        match_count,
        additional_key_bytes,
        &first_entry,
        &first_key_offset
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    for (size_t i = 0U; i < match_count; ++i) {
        const size_t leaf_entry = lower + i;
        const size_t entry_index = first_entry + i;
        const size_t key_offset = first_key_offset + (i * key_size);
        memcpy(
            out_entries->keys + key_offset,
            index_leaf_entry_key(leaf_page, leaf_entry),
            key_size
        );
        out_entries->key_offsets[entry_index] = key_offset;
        out_entries->key_sizes[entry_index] = key_size;
        out_entries->row_ids[entry_index] = index_leaf_entry_row_id(leaf_page, leaf_entry);
    }
    out_entries->entry_count = first_entry + match_count;
    out_entries->key_bytes = first_key_offset + additional_key_bytes;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result scan_exact_index_row_ids(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_row_id_list *out_row_ids
) {
    *out_row_ids = (mylite_storage_row_id_list){0};

    mylite_storage_result result = scan_exact_index_row_ids_from(
        file,
        header,
        table_id,
        index_number,
        key,
        key_size,
        MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT,
        out_row_ids
    );
    if (result != MYLITE_STORAGE_OK) {
        free(out_row_ids->row_ids);
        *out_row_ids = (mylite_storage_row_id_list){0};
    }
    return result;
}

static mylite_storage_result scan_exact_index_row_ids_from(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long first_page_id,
    mylite_storage_row_id_list *out_row_ids
) {
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_result result = MYLITE_STORAGE_OK;
    for (unsigned long long page_id = first_page_id;
         result == MYLITE_STORAGE_OK && page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        result = read_page_at(file, page_id, header->page_size, page);
        if (result != MYLITE_STORAGE_OK) {
            break;
        }

        if (is_index_entry_page(page)) {
            mylite_storage_index_entry_page entry_page = {0};
            result = decode_index_entry_page(header, page_id, page, &entry_page);
            if (result == MYLITE_STORAGE_OK && entry_page.table_id == table_id &&
                entry_page.index_number == index_number &&
                find_row_state_entry(&row_state_map, entry_page.row_id) == NULL) {
                remove_row_id_from_list(out_row_ids, entry_page.row_id);
                if (entry_page.key_size == key_size && memcmp(entry_page.key, key, key_size) == 0) {
                    result = append_row_id_to_list(out_row_ids, entry_page.row_id);
                }
            }
            continue;
        }

        if (is_row_state_page(page)) {
            mylite_storage_row_state_page row_state_page = {0};
            result = decode_row_state_page(header, page_id, page, &row_state_page);
            if (result == MYLITE_STORAGE_OK && row_state_page.table_id == table_id) {
                result = set_row_state_entry(&row_state_map, &row_state_page);
                if (result == MYLITE_STORAGE_OK) {
                    if (row_state_page.state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE) {
                        replace_row_id_in_list(
                            out_row_ids,
                            row_state_page.source_row_id,
                            row_state_page.replacement_row_id
                        );
                    } else {
                        remove_row_id_from_list(out_row_ids, row_state_page.source_row_id);
                    }
                }
            }
            continue;
        }

        if (!is_exact_index_scan_skip_page(page)) {
            result = MYLITE_STORAGE_CORRUPT;
        }
    }

    free_row_state_map(&row_state_map);
    return result;
}

static mylite_storage_result scan_exact_index_entries(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries
) {
    return scan_exact_index_entries_from(
        file,
        header,
        table_id,
        index_number,
        key,
        key_size,
        MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT,
        out_entries
    );
}

static mylite_storage_result scan_exact_index_entries_from(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long first_page_id,
    mylite_storage_index_entryset *out_entries
) {
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_result result = MYLITE_STORAGE_OK;
    for (unsigned long long page_id = first_page_id;
         result == MYLITE_STORAGE_OK && page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        result = read_page_at(file, page_id, header->page_size, page);
        if (result != MYLITE_STORAGE_OK) {
            break;
        }

        if (is_index_entry_page(page)) {
            mylite_storage_index_entry_page entry_page = {0};
            result = decode_index_entry_page(header, page_id, page, &entry_page);
            if (result == MYLITE_STORAGE_OK && entry_page.table_id == table_id &&
                entry_page.index_number == index_number &&
                find_row_state_entry(&row_state_map, entry_page.row_id) == NULL) {
                remove_index_entries_by_row_id(out_entries, entry_page.row_id);
                if (entry_page.key_size == key_size && memcmp(entry_page.key, key, key_size) == 0) {
                    result = append_index_entry_to_entryset(out_entries, &entry_page);
                }
            }
            continue;
        }

        if (is_row_state_page(page)) {
            mylite_storage_row_state_page row_state_page = {0};
            result = decode_row_state_page(header, page_id, page, &row_state_page);
            if (result == MYLITE_STORAGE_OK && row_state_page.table_id == table_id) {
                result = set_row_state_entry(&row_state_map, &row_state_page);
                if (result == MYLITE_STORAGE_OK) {
                    if (row_state_page.state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE) {
                        replace_index_entries_row_id(
                            out_entries,
                            row_state_page.source_row_id,
                            row_state_page.replacement_row_id
                        );
                    } else {
                        remove_index_entries_by_row_id(out_entries, row_state_page.source_row_id);
                    }
                }
            }
            continue;
        }

        if (!is_exact_index_scan_skip_page(page)) {
            result = MYLITE_STORAGE_CORRUPT;
        }
    }

    free_row_state_map(&row_state_map);
    return result;
}

static int is_exact_index_scan_skip_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
               k_blob_magic,
               sizeof(k_blob_magic)
           ) == 0 ||
           is_row_page(page) || is_autoincrement_page(page) || is_index_leaf_page(page);
}

static mylite_storage_result index_entryset_fixed_key_size(
    const mylite_storage_index_entryset *entryset,
    size_t *out_key_size
) {
    *out_key_size = 0U;
    if (entryset->entry_count == 0U) {
        return MYLITE_STORAGE_OK;
    }
    if (entryset->keys == NULL || entryset->key_offsets == NULL || entryset->key_sizes == NULL ||
        entryset->row_ids == NULL) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const size_t key_size = entryset->key_sizes[0];
    if (key_size == 0U || key_size > MYLITE_STORAGE_MAX_INDEX_KEY_SIZE) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    for (size_t i = 0U; i < entryset->entry_count; ++i) {
        if (entryset->key_sizes[i] != key_size || entryset->key_offsets[i] > entryset->key_bytes ||
            key_size > entryset->key_bytes - entryset->key_offsets[i]) {
            return MYLITE_STORAGE_UNSUPPORTED;
        }
    }

    *out_key_size = key_size;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result build_raw_index_entry_order(
    const mylite_storage_index_entryset *entryset,
    size_t **out_order
) {
    *out_order = NULL;
    if (entryset->entry_count == 0U) {
        return MYLITE_STORAGE_OK;
    }
    if (entryset->entry_count > SIZE_MAX / sizeof(size_t)) {
        return MYLITE_STORAGE_FULL;
    }

    size_t *order = (size_t *)malloc(entryset->entry_count * sizeof(size_t));
    if (order == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    for (size_t i = 0U; i < entryset->entry_count; ++i) {
        order[i] = i;
    }
    for (size_t i = 1U; i < entryset->entry_count; ++i) {
        const size_t value = order[i];
        size_t j = i;
        while (j > 0U && compare_raw_index_entry(entryset, order[j - 1U], value) > 0) {
            order[j] = order[j - 1U];
            --j;
        }
        order[j] = value;
    }

    *out_order = order;
    return MYLITE_STORAGE_OK;
}

static int compare_raw_index_entry(
    const mylite_storage_index_entryset *entryset,
    size_t left_index,
    size_t right_index
) {
    const size_t left_key_size = entryset->key_sizes[left_index];
    const size_t right_key_size = entryset->key_sizes[right_index];
    const size_t shared_key_size = left_key_size < right_key_size ? left_key_size : right_key_size;
    int cmp = memcmp(
        entryset->keys + entryset->key_offsets[left_index],
        entryset->keys + entryset->key_offsets[right_index],
        shared_key_size
    );
    if (cmp == 0) {
        if (left_key_size < right_key_size) {
            cmp = -1;
        } else if (left_key_size > right_key_size) {
            cmp = 1;
        }
    }
    if (cmp == 0) {
        if (entryset->row_ids[left_index] < entryset->row_ids[right_index]) {
            cmp = -1;
        } else if (entryset->row_ids[left_index] > entryset->row_ids[right_index]) {
            cmp = 1;
        }
    }
    return cmp;
}

static int compare_leaf_key(
    const mylite_storage_index_leaf_page *leaf_page,
    size_t entry_index,
    const unsigned char *key,
    size_t key_size
) {
    const size_t shared_key_size = leaf_page->key_size < key_size ? leaf_page->key_size : key_size;
    int cmp = memcmp(index_leaf_entry_key(leaf_page, entry_index), key, shared_key_size);
    if (cmp == 0) {
        if (leaf_page->key_size < key_size) {
            cmp = -1;
        } else if (leaf_page->key_size > key_size) {
            cmp = 1;
        }
    }
    return cmp;
}

static const unsigned char *index_leaf_entry_key(
    const mylite_storage_index_leaf_page *leaf_page,
    size_t entry_index
) {
    const size_t cell_size =
        MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + leaf_page->key_size;
    return leaf_page->payload + (entry_index * cell_size) +
           MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_KEY_OFFSET;
}

static unsigned long long index_leaf_entry_row_id(
    const mylite_storage_index_leaf_page *leaf_page,
    size_t entry_index
) {
    const size_t cell_size =
        MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + leaf_page->key_size;
    return get_u64_le(
        leaf_page->payload + (entry_index * cell_size),
        MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_ROW_ID_OFFSET
    );
}

static mylite_storage_result scan_table_row_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_page_callback callback,
    void *ctx
) {
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_result result = build_row_state_map(file, header, table_id, &row_state_map);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_row_page row_page = {0};
        result = read_row_page(file, header, page_id, page, &row_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            free_row_state_map(&row_state_map);
            return result;
        }
        if (row_page.table_id == table_id &&
            find_row_state_entry(&row_state_map, row_page.row_id) == NULL) {
            result = callback(ctx, &row_page);
            free(row_page.owned_payload);
            if (result != MYLITE_STORAGE_OK) {
                free_row_state_map(&row_state_map);
                return result;
            }
        } else {
            free(row_page.owned_payload);
        }
    }

    free_row_state_map(&row_state_map);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result collect_live_table_row_ids(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_id_list *out_row_ids
) {
    *out_row_ids = (mylite_storage_row_id_list){0};
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_result result = MYLITE_STORAGE_OK;

    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         result == MYLITE_STORAGE_OK && page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        result = read_page_at(file, page_id, header->page_size, page);
        if (result != MYLITE_STORAGE_OK) {
            break;
        }

        if (is_row_page(page)) {
            mylite_storage_row_page_metadata metadata = {0};
            result = decode_row_page_metadata(header, page_id, page, &metadata);
            if (result == MYLITE_STORAGE_OK && metadata.table_id == table_id) {
                result = append_row_id_to_list(out_row_ids, metadata.row_id);
            }
            continue;
        }

        if (is_row_state_page(page)) {
            mylite_storage_row_state_page row_state_page = {0};
            result = decode_row_state_page(header, page_id, page, &row_state_page);
            if (result == MYLITE_STORAGE_OK && row_state_page.table_id == table_id) {
                result = set_row_state_entry(&row_state_map, &row_state_page);
            }
            continue;
        }

        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) != 0 &&
            !is_autoincrement_page(page) && !is_index_entry_page(page) &&
            !is_index_leaf_page(page)) {
            result = MYLITE_STORAGE_CORRUPT;
        }
    }

    if (result == MYLITE_STORAGE_OK) {
        result = compact_live_table_row_ids(out_row_ids, &row_state_map);
    }
    free_row_state_map(&row_state_map);
    if (result != MYLITE_STORAGE_OK) {
        free(out_row_ids->row_ids);
        *out_row_ids = (mylite_storage_row_id_list){0};
    }
    return result;
}

static mylite_storage_result copy_cached_durable_live_row_ids(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_id_list *out_row_ids,
    int *out_used_cache
) {
    *out_row_ids = (mylite_storage_row_id_list){0};
    *out_used_cache = 0;
    if (!durable_live_row_id_cache_available(filename)) {
        return MYLITE_STORAGE_OK;
    }

    const mylite_storage_live_row_id_cache *cache =
        find_durable_live_row_id_cache(filename, header, table_id);
    if (cache == NULL) {
        return MYLITE_STORAGE_OK;
    }

    *out_used_cache = 1;
    return copy_live_row_ids(cache->row_ids, cache->count, out_row_ids);
}

static void store_durable_live_row_ids(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const mylite_storage_row_id_list *row_ids
) {
    if (!durable_live_row_id_cache_available(filename) ||
        row_ids->count > MYLITE_STORAGE_DURABLE_LIVE_ROW_ID_ENTRY_LIMIT) {
        return;
    }

    mylite_storage_live_row_id_cache *cache =
        ensure_durable_live_row_id_cache(filename, header, table_id);
    if (cache == NULL) {
        return;
    }
    (void)assign_live_row_id_cache(cache, row_ids);
}

static void clear_durable_live_row_id_caches(const char *filename) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < durable_live_row_id_caches.count; ++read_index) {
        mylite_storage_live_row_id_cache *cache =
            durable_live_row_id_caches.entries + read_index;
        const int clear_cache =
            filename == NULL || (cache->filename != NULL && strcmp(cache->filename, filename) == 0);
        if (clear_cache) {
            free_live_row_id_cache(cache);
            continue;
        }
        if (write_index != read_index) {
            durable_live_row_id_caches.entries[write_index] = *cache;
        }
        ++write_index;
    }
    durable_live_row_id_caches.count = write_index;
    if (durable_live_row_id_caches.count == 0U) {
        free(durable_live_row_id_caches.entries);
        durable_live_row_id_caches = (mylite_storage_live_row_id_cache_set){0};
    }
}

static void retarget_durable_live_row_id_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < durable_live_row_id_caches.count; ++read_index) {
        mylite_storage_live_row_id_cache *cache =
            durable_live_row_id_caches.entries + read_index;
        const int same_file = cache->filename != NULL && strcmp(cache->filename, filename) == 0;
        if (same_file && cache->table_id == table_id) {
            free_live_row_id_cache(cache);
            continue;
        }
        if (same_file) {
            cache->catalog_root_page = header->catalog_root_page;
            cache->catalog_generation = header->catalog_generation;
            cache->page_count = header->page_count;
        }
        if (write_index != read_index) {
            durable_live_row_id_caches.entries[write_index] = *cache;
        }
        ++write_index;
    }
    durable_live_row_id_caches.count = write_index;
    if (durable_live_row_id_caches.count == 0U) {
        free(durable_live_row_id_caches.entries);
        durable_live_row_id_caches = (mylite_storage_live_row_id_cache_set){0};
    }
}

static void promote_statement_live_row_id_caches(const mylite_storage_statement *statement) {
    if (statement == NULL || statement->live_row_id_caches.count == 0U) {
        return;
    }

    const mylite_storage_header *header =
        statement->has_current_header ? &statement->current_header : &statement->header;
    for (size_t i = 0U; i < statement->live_row_id_caches.count; ++i) {
        const mylite_storage_live_row_id_cache *cache =
            statement->live_row_id_caches.entries + i;
        mylite_storage_row_id_list row_ids = {
            .row_ids = cache->row_ids,
            .count = cache->count,
        };
        store_durable_live_row_ids(statement->filename, header, cache->table_id, &row_ids);
    }
}

static void seed_active_live_row_id_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    mylite_storage_statement *statement = active_live_row_id_cache_statement_for(filename);
    if (statement == NULL ||
        find_active_live_row_id_cache(&statement->live_row_id_caches, filename, header, table_id) !=
            NULL) {
        return;
    }

    const mylite_storage_live_row_id_cache *durable_cache =
        find_durable_live_row_id_cache(filename, header, table_id);
    if (durable_cache == NULL) {
        return;
    }
    if (statement->live_row_id_caches.count >= MYLITE_STORAGE_DURABLE_LIVE_ROW_ID_CACHE_LIMIT) {
        clear_live_row_id_caches(statement);
    }

    mylite_storage_live_row_id_cache *cache = NULL;
    if (append_active_live_row_id_cache(
            &statement->live_row_id_caches,
            filename,
            header,
            table_id,
            &cache
        ) != MYLITE_STORAGE_OK) {
        return;
    }
    if (assign_live_row_id_cache(
            cache,
            &(mylite_storage_row_id_list){
                .row_ids = durable_cache->row_ids,
                .count = durable_cache->count,
            }
        ) != MYLITE_STORAGE_OK) {
        free_live_row_id_cache(cache);
        if (statement->live_row_id_caches.count > 0U) {
            --statement->live_row_id_caches.count;
        }
    }
}

static void append_active_live_row_id(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
) {
    mylite_storage_live_row_id_cache *cache =
        active_live_row_id_cache_for(filename, header, table_id);
    if (cache == NULL) {
        return;
    }
    if (!append_row_id_to_cache(cache, row_id)) {
        clear_live_row_id_caches(active_live_row_id_cache_statement_for(filename));
    }
}

static void replace_active_live_row_id(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long old_row_id,
    unsigned long long new_row_id
) {
    mylite_storage_live_row_id_cache *cache =
        active_live_row_id_cache_for(filename, header, table_id);
    if (cache == NULL) {
        return;
    }
    if (!replace_row_id_in_cache(cache, old_row_id, new_row_id)) {
        clear_live_row_id_caches(active_live_row_id_cache_statement_for(filename));
    }
}

static void remove_active_live_row_id(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
) {
    mylite_storage_live_row_id_cache *cache =
        active_live_row_id_cache_for(filename, header, table_id);
    if (cache == NULL) {
        return;
    }
    if (!remove_row_id_from_cache(cache, row_id)) {
        clear_live_row_id_caches(active_live_row_id_cache_statement_for(filename));
    }
}

static mylite_storage_live_row_id_cache *active_live_row_id_cache_for(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    mylite_storage_statement *statement = active_live_row_id_cache_statement_for(filename);
    if (statement == NULL) {
        return NULL;
    }
    return find_active_live_row_id_cache(
        &statement->live_row_id_caches,
        filename,
        header,
        table_id
    );
}

static mylite_storage_live_row_id_cache *find_active_live_row_id_cache(
    mylite_storage_live_row_id_cache_set *caches,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    for (size_t i = 0U; i < caches->count; ++i) {
        mylite_storage_live_row_id_cache *cache = caches->entries + i;
        if (cache->filename != NULL && strcmp(cache->filename, filename) == 0 &&
            cache->catalog_root_page == header->catalog_root_page &&
            cache->catalog_generation == header->catalog_generation &&
            cache->table_id == table_id) {
            return cache;
        }
    }
    return NULL;
}

static mylite_storage_result append_active_live_row_id_cache(
    mylite_storage_live_row_id_cache_set *caches,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_live_row_id_cache **out_cache
) {
    if (caches->count == caches->capacity) {
        const size_t next_capacity = caches->capacity == 0U ? 4U : caches->capacity * 2U;
        if (next_capacity <= caches->capacity ||
            next_capacity > SIZE_MAX / sizeof(*caches->entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_live_row_id_cache *entries = (mylite_storage_live_row_id_cache *)realloc(
            caches->entries,
            next_capacity * sizeof(*caches->entries)
        );
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        caches->entries = entries;
        caches->capacity = next_capacity;
    }

    mylite_storage_live_row_id_cache *cache = caches->entries + caches->count;
    *cache = (mylite_storage_live_row_id_cache){
        .filename = copy_filename(filename),
        .catalog_root_page = header->catalog_root_page,
        .catalog_generation = header->catalog_generation,
        .page_count = header->page_count,
        .table_id = table_id,
    };
    if (cache->filename == NULL) {
        *cache = (mylite_storage_live_row_id_cache){0};
        return MYLITE_STORAGE_NOMEM;
    }
    ++caches->count;
    *out_cache = cache;
    return MYLITE_STORAGE_OK;
}

static int append_row_id_to_cache(
    mylite_storage_live_row_id_cache *cache,
    unsigned long long row_id
) {
    for (size_t i = 0U; i < cache->count; ++i) {
        if (cache->row_ids[i] == row_id) {
            return 1;
        }
    }
    if (cache->count >= MYLITE_STORAGE_DURABLE_LIVE_ROW_ID_ENTRY_LIMIT ||
        cache->count >= SIZE_MAX / sizeof(*cache->row_ids)) {
        return 0;
    }

    const size_t new_count = cache->count + 1U;
    unsigned long long *row_ids =
        (unsigned long long *)realloc(cache->row_ids, new_count * sizeof(*cache->row_ids));
    if (row_ids == NULL) {
        return 0;
    }
    cache->row_ids = row_ids;
    cache->row_ids[cache->count] = row_id;
    cache->count = new_count;
    return 1;
}

static int replace_row_id_in_cache(
    mylite_storage_live_row_id_cache *cache,
    unsigned long long old_row_id,
    unsigned long long new_row_id
) {
    for (size_t i = 0U; i < cache->count; ++i) {
        if (cache->row_ids[i] == old_row_id) {
            cache->row_ids[i] = new_row_id;
            return 1;
        }
    }
    return 0;
}

static int remove_row_id_from_cache(
    mylite_storage_live_row_id_cache *cache,
    unsigned long long row_id
) {
    size_t write_index = 0U;
    int removed = 0;
    for (size_t read_index = 0U; read_index < cache->count; ++read_index) {
        if (cache->row_ids[read_index] == row_id) {
            removed = 1;
            continue;
        }
        cache->row_ids[write_index++] = cache->row_ids[read_index];
    }
    cache->count = write_index;
    return removed;
}

static int durable_live_row_id_cache_available(const char *filename) {
    return active_statement_for_any_owner(filename) == NULL && !active_read_snapshot_for(filename);
}

static mylite_storage_live_row_id_cache *find_durable_live_row_id_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    for (size_t i = 0U; i < durable_live_row_id_caches.count; ++i) {
        mylite_storage_live_row_id_cache *cache = durable_live_row_id_caches.entries + i;
        if (cache->filename != NULL && strcmp(cache->filename, filename) == 0 &&
            cache->catalog_root_page == header->catalog_root_page &&
            cache->catalog_generation == header->catalog_generation &&
            cache->page_count == header->page_count && cache->table_id == table_id) {
            return cache;
        }
    }
    return NULL;
}

static mylite_storage_live_row_id_cache *ensure_durable_live_row_id_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    mylite_storage_live_row_id_cache *cache =
        find_durable_live_row_id_cache(filename, header, table_id);
    if (cache != NULL) {
        return cache;
    }

    if (durable_live_row_id_caches.count >= MYLITE_STORAGE_DURABLE_LIVE_ROW_ID_CACHE_LIMIT) {
        clear_durable_live_row_id_caches(NULL);
    }
    if (append_durable_live_row_id_cache(filename, header, table_id, &cache) !=
        MYLITE_STORAGE_OK) {
        return NULL;
    }
    return cache;
}

static mylite_storage_result append_durable_live_row_id_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_live_row_id_cache **out_cache
) {
    if (durable_live_row_id_caches.count == durable_live_row_id_caches.capacity) {
        const size_t next_capacity =
            durable_live_row_id_caches.capacity == 0U
                ? 4U
                : durable_live_row_id_caches.capacity * 2U;
        if (next_capacity <= durable_live_row_id_caches.capacity ||
            next_capacity > SIZE_MAX / sizeof(*durable_live_row_id_caches.entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_live_row_id_cache *entries = (mylite_storage_live_row_id_cache *)realloc(
            durable_live_row_id_caches.entries,
            next_capacity * sizeof(*durable_live_row_id_caches.entries)
        );
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        durable_live_row_id_caches.entries = entries;
        durable_live_row_id_caches.capacity = next_capacity;
    }

    mylite_storage_live_row_id_cache *cache =
        durable_live_row_id_caches.entries + durable_live_row_id_caches.count;
    *cache = (mylite_storage_live_row_id_cache){
        .filename = copy_filename(filename),
        .catalog_root_page = header->catalog_root_page,
        .catalog_generation = header->catalog_generation,
        .page_count = header->page_count,
        .table_id = table_id,
    };
    if (cache->filename == NULL) {
        *cache = (mylite_storage_live_row_id_cache){0};
        return MYLITE_STORAGE_NOMEM;
    }
    ++durable_live_row_id_caches.count;
    *out_cache = cache;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result assign_live_row_id_cache(
    mylite_storage_live_row_id_cache *cache,
    const mylite_storage_row_id_list *row_ids
) {
    mylite_storage_row_id_list copy = {0};
    mylite_storage_result result = copy_live_row_ids(row_ids->row_ids, row_ids->count, &copy);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    free(cache->row_ids);
    cache->row_ids = copy.row_ids;
    cache->count = copy.count;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result copy_live_row_ids(
    const unsigned long long *row_ids,
    size_t row_id_count,
    mylite_storage_row_id_list *out_row_ids
) {
    *out_row_ids = (mylite_storage_row_id_list){0};
    if (row_id_count == 0U) {
        return MYLITE_STORAGE_OK;
    }
    if (row_ids == NULL || row_id_count > SIZE_MAX / sizeof(*row_ids)) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned long long *copy =
        (unsigned long long *)malloc(row_id_count * sizeof(*copy));
    if (copy == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    memcpy(copy, row_ids, row_id_count * sizeof(*copy));
    out_row_ids->row_ids = copy;
    out_row_ids->count = row_id_count;
    return MYLITE_STORAGE_OK;
}

static void free_live_row_id_cache(mylite_storage_live_row_id_cache *cache) {
    free(cache->filename);
    free(cache->row_ids);
    *cache = (mylite_storage_live_row_id_cache){0};
}

static mylite_storage_result compact_live_table_row_ids(
    mylite_storage_row_id_list *row_ids,
    const mylite_storage_row_state_map *row_state_map
) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < row_ids->count; ++read_index) {
        const unsigned long long row_id = row_ids->row_ids[read_index];
        if (find_row_state_entry(row_state_map, row_id) == NULL) {
            row_ids->row_ids[write_index++] = row_id;
        }
    }
    row_ids->count = write_index;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_row_ids_into_rowset(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    const mylite_storage_row_id_list *row_ids,
    mylite_storage_rowset *out_rows
) {
    if (row_ids->count == 0U) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_rowset_builder builder = {0};
    mylite_storage_result result = initialize_rowset_builder(&builder, out_rows, row_ids->count);
    mylite_storage_row_payload_cache *row_payload_cache = NULL;
    unsigned long long row_payload_cache_generation = 0ULL;
    if (result == MYLITE_STORAGE_OK) {
        row_payload_cache = durable_row_payload_cache_for(filename, header, table_id);
        row_payload_cache_generation = durable_row_payload_caches_generation;
    }
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < row_ids->count; ++i) {
        const unsigned long long row_id = row_ids->row_ids[i];
        int used_cache = 0;
        result = append_cached_durable_row_payload_to_builder(
            filename,
            header,
            table_id,
            &row_payload_cache,
            &row_payload_cache_generation,
            row_id,
            &builder,
            &used_cache
        );
        if (result != MYLITE_STORAGE_OK || used_cache) {
            continue;
        }

        mylite_storage_row_page row_page = {0};
        unsigned char row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        if (row_id <= header->catalog_root_page || row_id >= header->page_count) {
            result = MYLITE_STORAGE_NOTFOUND;
        }
        if (result == MYLITE_STORAGE_OK) {
            result = read_row_page(file, header, row_id, row_buffer, &row_page);
        }
        if (result == MYLITE_STORAGE_OK && row_page.table_id != table_id) {
            result = MYLITE_STORAGE_NOTFOUND;
        }
        if (result == MYLITE_STORAGE_OK) {
            result =
                append_row_to_rowset_builder(&builder, row_id, row_page.payload, row_page.row_size);
            if (result == MYLITE_STORAGE_OK) {
                store_durable_row_payload(
                    filename,
                    header,
                    table_id,
                    &row_payload_cache,
                    &row_payload_cache_generation,
                    row_id,
                    row_page.payload,
                    row_page.row_size
                );
            }
        }
        free(row_page.owned_payload);
    }
    return result;
}

static mylite_storage_result validate_row_id_payloads(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const mylite_storage_row_id_list *row_ids
) {
    for (size_t i = 0U; i < row_ids->count; ++i) {
        const unsigned long long row_id = row_ids->row_ids[i];
        mylite_storage_row_page row_page = {0};
        unsigned char row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        if (row_id <= header->catalog_root_page || row_id >= header->page_count) {
            return MYLITE_STORAGE_NOTFOUND;
        }

        mylite_storage_result result = read_row_page(file, header, row_id, row_buffer, &row_page);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        free(row_page.owned_payload);
        if (row_page.table_id != table_id) {
            return MYLITE_STORAGE_NOTFOUND;
        }
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_row_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_row_page *out_row_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return decode_row_page(file, header, page_id, page, out_row_page);
}

static mylite_storage_result decode_row_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_page *out_row_page
) {
    mylite_storage_row_page_metadata metadata = {0};
    mylite_storage_result result = decode_row_page_metadata(header, page_id, page, &metadata);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    unsigned char *owned_payload = NULL;
    const unsigned char *payload = page + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    if (metadata.overflow_root_page != 0ULL) {
        result = read_row_payload_blob_pages(
            file,
            header,
            (mylite_storage_blob_chain){
                .first_page_id = metadata.overflow_root_page,
                .payload_size = metadata.row_size,
                .page_type = MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_ROW_PAYLOAD,
            },
            &owned_payload
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        payload = owned_payload;
    }

    *out_row_page = (mylite_storage_row_page){
        .row_id = metadata.row_id,
        .table_id = metadata.table_id,
        .row_size = metadata.row_size,
        .row_count = metadata.row_count,
        .payload = payload,
        .owned_payload = owned_payload,
    };
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result decode_row_page_metadata(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_page_metadata *out_metadata
) {
    if (!is_row_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_autoincrement_page(page) || is_row_state_page(page) || is_index_entry_page(page) ||
            is_index_leaf_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_OFFSET);
    const unsigned page_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_ID_OFFSET);
    const unsigned long long table_id = get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_TABLE_ID_OFFSET);
    const size_t row_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_SIZE_OFFSET);
    const size_t row_count = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_COUNT_OFFSET);
    const unsigned long long overflow_root_page =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_OVERFLOW_ROOT_PAGE_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET);
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    if (page_type != MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_TABLE_ROWS || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || expected_checksum != actual_checksum || row_size == 0U ||
        row_count != 1U) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (overflow_root_page == 0ULL && row_count > payload_capacity / row_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (overflow_root_page != 0ULL &&
        (row_count != 1U || overflow_root_page <= header->catalog_root_page ||
         overflow_root_page >= header->page_count)) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_metadata = (mylite_storage_row_page_metadata){
        .row_id = page_id,
        .table_id = table_id,
        .overflow_root_page = overflow_root_page,
        .row_size = row_size,
        .row_count = row_count,
    };
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result decode_buffered_row_page_metadata(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_page_metadata *out_metadata
) {
    if (!is_row_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_autoincrement_page(page) || is_row_state_page(page) || is_index_entry_page(page) ||
            is_index_leaf_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_OFFSET);
    const unsigned page_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_ID_OFFSET);
    const unsigned long long table_id = get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_TABLE_ID_OFFSET);
    const size_t row_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_SIZE_OFFSET);
    const size_t row_count = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_COUNT_OFFSET);
    const unsigned long long overflow_root_page =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_OVERFLOW_ROOT_PAGE_OFFSET);
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    if (page_type != MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_TABLE_ROWS || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || row_size == 0U || row_count != 1U) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (overflow_root_page == 0ULL && row_count > payload_capacity / row_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (overflow_root_page != 0ULL &&
        (row_count != 1U || overflow_root_page <= header->catalog_root_page ||
         overflow_root_page >= header->page_count)) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_metadata = (mylite_storage_row_page_metadata){
        .row_id = page_id,
        .table_id = table_id,
        .overflow_root_page = overflow_root_page,
        .row_size = row_size,
        .row_count = row_count,
    };
    return MYLITE_STORAGE_OK;
}

static int is_row_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_ROW_MAGIC_OFFSET,
               k_row_magic,
               sizeof(k_row_magic)
           ) == 0;
}

static mylite_storage_result validate_direct_live_row(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    unsigned char *page,
    mylite_storage_row_page *out_row_page
) {
    if (row_id <= header->catalog_root_page || row_id >= header->page_count) {
        return MYLITE_STORAGE_NOTFOUND;
    }
    if (active_validated_live_row_known(file, header, table_id, row_id)) {
        *out_row_page = (mylite_storage_row_page){
            .row_id = row_id,
            .table_id = table_id,
        };
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_row_page row_page = {0};
    mylite_storage_result result = read_row_page(file, header, row_id, page, &row_page);
    if (result == MYLITE_STORAGE_NOTFOUND) {
        return MYLITE_STORAGE_NOTFOUND;
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (row_page.table_id != table_id) {
        free(row_page.owned_payload);
        return MYLITE_STORAGE_NOTFOUND;
    }

    int row_hidden = 0;
    if (active_live_row_known(file, header, table_id, row_id)) {
        (void)mark_active_validated_live_row(file, header, table_id, row_id);
        *out_row_page = row_page;
        return MYLITE_STORAGE_OK;
    }

    result = row_is_hidden_after(file, header, table_id, row_id, row_id + 1ULL, &row_hidden);
    if (result != MYLITE_STORAGE_OK) {
        free(row_page.owned_payload);
        return result;
    }
    if (row_hidden) {
        free(row_page.owned_payload);
        return MYLITE_STORAGE_NOTFOUND;
    }

    (void)mark_active_validated_live_row(file, header, table_id, row_id);
    *out_row_page = row_page;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result row_is_hidden_after(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    unsigned long long first_page_id,
    int *out_hidden
) {
    *out_hidden = 0;
    for (unsigned long long page_id = first_page_id; page_id < header->page_count; ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_row_state_page row_state_page = {0};
        mylite_storage_result result =
            read_row_state_page(file, header, page_id, page, &row_state_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (row_state_page.table_id == table_id && row_state_page.source_row_id == row_id) {
            *out_hidden = 1;
            return MYLITE_STORAGE_OK;
        }
    }
    return MYLITE_STORAGE_OK;
}

static int active_live_row_known(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
) {
    mylite_storage_live_row_cache *cache = live_row_cache_for(file, header, table_id);
    if (cache == NULL) {
        return 0;
    }
    for (size_t i = 0U; i < cache->count; ++i) {
        if (cache->row_ids[i] == row_id) {
            return 1;
        }
    }
    return 0;
}

static int active_validated_live_row_known(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
) {
    mylite_storage_live_row_cache *cache = live_row_cache_for(file, header, table_id);
    if (cache == NULL) {
        return 0;
    }
    for (size_t i = 0U; i < cache->validated_count; ++i) {
        if (cache->validated_row_ids[i] == row_id) {
            return 1;
        }
    }
    return 0;
}

static mylite_storage_result mark_active_live_row(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
) {
    mylite_storage_live_row_cache *cache = live_row_cache_for(file, header, table_id);
    if (cache == NULL) {
        mylite_storage_statement *statement = active_statement_for_file(file);
        if (statement == NULL) {
            return MYLITE_STORAGE_OK;
        }
        mylite_storage_result result =
            append_live_row_cache(&statement->live_row_caches, header, table_id, &cache);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }

    return add_live_row_id(cache, row_id);
}

static mylite_storage_result mark_active_validated_live_row(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
) {
    mylite_storage_result result = mark_active_live_row(file, header, table_id, row_id);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_live_row_cache *cache = live_row_cache_for(file, header, table_id);
    return cache == NULL ? MYLITE_STORAGE_OK : add_validated_live_row_id(cache, row_id);
}

static void replace_active_live_row(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long old_row_id,
    unsigned long long new_row_id
) {
    mylite_storage_live_row_cache *cache = live_row_cache_for(file, header, table_id);
    if (cache == NULL) {
        if (new_row_id != 0ULL) {
            (void)mark_active_validated_live_row(file, header, table_id, new_row_id);
        }
        return;
    }
    remove_live_row_id(cache, old_row_id);
    remove_validated_live_row_id(cache, old_row_id);
    if (new_row_id != 0ULL) {
        (void)add_live_row_id(cache, new_row_id);
        (void)add_validated_live_row_id(cache, new_row_id);
    }
}

static mylite_storage_live_row_cache *live_row_cache_for(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    mylite_storage_statement *statement = active_statement_for_file(file);
    if (statement == NULL) {
        return NULL;
    }
    return find_live_row_cache(&statement->live_row_caches, header, table_id);
}

static mylite_storage_live_row_cache *find_live_row_cache(
    mylite_storage_live_row_cache_set *caches,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    for (size_t i = 0U; i < caches->count; ++i) {
        mylite_storage_live_row_cache *cache = caches->entries + i;
        if (cache->table_id == table_id && cache->catalog_root_page == header->catalog_root_page &&
            cache->catalog_generation == header->catalog_generation) {
            return cache;
        }
    }
    return NULL;
}

static mylite_storage_result append_live_row_cache(
    mylite_storage_live_row_cache_set *caches,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_live_row_cache **out_cache
) {
    if (caches->count == caches->capacity) {
        const size_t next_capacity = caches->capacity == 0U ? 4U : caches->capacity * 2U;
        if (next_capacity <= caches->capacity ||
            next_capacity > SIZE_MAX / sizeof(*caches->entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_live_row_cache *entries = (mylite_storage_live_row_cache *)
            realloc(caches->entries, next_capacity * sizeof(*caches->entries));
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        caches->entries = entries;
        caches->capacity = next_capacity;
    }

    mylite_storage_live_row_cache *cache = caches->entries + caches->count;
    *cache = (mylite_storage_live_row_cache){
        .table_id = table_id,
        .catalog_root_page = header->catalog_root_page,
        .catalog_generation = header->catalog_generation,
    };
    ++caches->count;
    *out_cache = cache;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result add_live_row_id(
    mylite_storage_live_row_cache *cache,
    unsigned long long row_id
) {
    for (size_t i = 0U; i < cache->count; ++i) {
        if (cache->row_ids[i] == row_id) {
            return MYLITE_STORAGE_OK;
        }
    }

    if (cache->count == cache->capacity) {
        const size_t next_capacity = cache->capacity == 0U ? 16U : cache->capacity * 2U;
        if (next_capacity <= cache->capacity ||
            next_capacity > SIZE_MAX / sizeof(*cache->row_ids)) {
            return MYLITE_STORAGE_FULL;
        }
        unsigned long long *row_ids =
            (unsigned long long *)realloc(cache->row_ids, next_capacity * sizeof(*cache->row_ids));
        if (row_ids == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        cache->row_ids = row_ids;
        cache->capacity = next_capacity;
    }

    cache->row_ids[cache->count++] = row_id;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result add_validated_live_row_id(
    mylite_storage_live_row_cache *cache,
    unsigned long long row_id
) {
    for (size_t i = 0U; i < cache->validated_count; ++i) {
        if (cache->validated_row_ids[i] == row_id) {
            return MYLITE_STORAGE_OK;
        }
    }

    if (cache->validated_count == cache->validated_capacity) {
        const size_t next_capacity =
            cache->validated_capacity == 0U ? 16U : cache->validated_capacity * 2U;
        if (next_capacity <= cache->validated_capacity ||
            next_capacity > SIZE_MAX / sizeof(*cache->validated_row_ids)) {
            return MYLITE_STORAGE_FULL;
        }
        unsigned long long *row_ids = (unsigned long long *)realloc(
            cache->validated_row_ids,
            next_capacity * sizeof(*cache->validated_row_ids)
        );
        if (row_ids == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        cache->validated_row_ids = row_ids;
        cache->validated_capacity = next_capacity;
    }

    cache->validated_row_ids[cache->validated_count++] = row_id;
    return MYLITE_STORAGE_OK;
}

static void remove_live_row_id(mylite_storage_live_row_cache *cache, unsigned long long row_id) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < cache->count; ++read_index) {
        if (cache->row_ids[read_index] == row_id) {
            continue;
        }
        cache->row_ids[write_index++] = cache->row_ids[read_index];
    }
    cache->count = write_index;
}

static void remove_validated_live_row_id(
    mylite_storage_live_row_cache *cache,
    unsigned long long row_id
) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < cache->validated_count; ++read_index) {
        if (cache->validated_row_ids[read_index] == row_id) {
            continue;
        }
        cache->validated_row_ids[write_index++] = cache->validated_row_ids[read_index];
    }
    cache->validated_count = write_index;
}

static void encode_row_state_page(
    unsigned char *page,
    unsigned long long page_id,
    const mylite_storage_row_state_page *row_state_page
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_ROW_STATE_MAGIC_OFFSET,
        k_row_state_magic,
        sizeof(k_row_state_magic)
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_TABLE_ROWS
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_TABLE_ID_OFFSET, row_state_page->table_id);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_SOURCE_ROW_ID_OFFSET,
        row_state_page->source_row_id
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_REPLACEMENT_ROW_ID_OFFSET,
        row_state_page->replacement_row_id
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_OFFSET, row_state_page->state_kind);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET,
        checksum_page_zero_tail(
            page,
            MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET,
            MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_OFFSET + sizeof(uint32_t)
        )
    );
}

static mylite_storage_result build_row_state_map(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_state_map *out_row_state_map
) {
    *out_row_state_map = (mylite_storage_row_state_map){0};
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_row_state_page row_state_page = {0};
        mylite_storage_result result =
            read_row_state_page(file, header, page_id, page, &row_state_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            free_row_state_map(out_row_state_map);
            return result;
        }
        if (row_state_page.table_id != table_id) {
            continue;
        }

        result = set_row_state_entry(out_row_state_map, &row_state_page);
        if (result != MYLITE_STORAGE_OK) {
            free_row_state_map(out_row_state_map);
            return result;
        }
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_row_state_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_row_state_page *out_row_state_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return decode_row_state_page(header, page_id, page, out_row_state_page);
}

static mylite_storage_result decode_row_state_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_state_page *out_row_state_page
) {
    if (!is_row_state_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_row_page(page) || is_autoincrement_page(page) || is_index_entry_page(page) ||
            is_index_leaf_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_OFFSET);
    const unsigned page_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_ID_OFFSET);
    const unsigned long long table_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_TABLE_ID_OFFSET);
    const unsigned long long source_row_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_SOURCE_ROW_ID_OFFSET);
    const unsigned long long replacement_row_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_REPLACEMENT_ROW_ID_OFFSET);
    const unsigned state_kind = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET);

    if (page_type != MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_TABLE_ROWS || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || source_row_id <= header->catalog_root_page ||
        source_row_id >= header->page_count || expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_DELETE) {
        if (replacement_row_id != 0ULL) {
            return MYLITE_STORAGE_CORRUPT;
        }
    } else if (state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE) {
        if (replacement_row_id <= header->catalog_root_page ||
            replacement_row_id >= header->page_count || replacement_row_id == source_row_id) {
            return MYLITE_STORAGE_CORRUPT;
        }
    } else {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_row_state_page = (mylite_storage_row_state_page){
        .table_id = table_id,
        .source_row_id = source_row_id,
        .replacement_row_id = replacement_row_id,
        .state_kind = state_kind,
    };
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result decode_buffered_row_state_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_row_state_page *out_row_state_page
) {
    if (!is_row_state_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_row_page(page) || is_autoincrement_page(page) || is_index_entry_page(page) ||
            is_index_leaf_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_OFFSET);
    const unsigned page_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_ID_OFFSET);
    const unsigned long long table_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_TABLE_ID_OFFSET);
    const unsigned long long source_row_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_SOURCE_ROW_ID_OFFSET);
    const unsigned long long replacement_row_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_REPLACEMENT_ROW_ID_OFFSET);
    const unsigned state_kind = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_OFFSET);

    if (page_type != MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_TABLE_ROWS || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || source_row_id <= header->catalog_root_page ||
        source_row_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_DELETE) {
        if (replacement_row_id != 0ULL) {
            return MYLITE_STORAGE_CORRUPT;
        }
    } else if (state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE) {
        if (replacement_row_id <= header->catalog_root_page ||
            replacement_row_id >= header->page_count || replacement_row_id == source_row_id) {
            return MYLITE_STORAGE_CORRUPT;
        }
    } else {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_row_state_page = (mylite_storage_row_state_page){
        .table_id = table_id,
        .source_row_id = source_row_id,
        .replacement_row_id = replacement_row_id,
        .state_kind = state_kind,
    };
    return MYLITE_STORAGE_OK;
}

static int is_row_state_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_ROW_STATE_MAGIC_OFFSET,
               k_row_state_magic,
               sizeof(k_row_state_magic)
           ) == 0;
}

static mylite_storage_row_state_entry *find_row_state_entry(
    const mylite_storage_row_state_map *row_state_map,
    unsigned long long row_id
) {
    if (row_state_map->bucket_capacity > 0U) {
        const size_t mask = row_state_map->bucket_capacity - 1U;
        size_t bucket_index = hash_row_id(row_id) & mask;
        for (size_t probes = 0U; probes < row_state_map->bucket_capacity; ++probes) {
            const mylite_storage_row_state_map_bucket *bucket =
                row_state_map->buckets + bucket_index;
            if (!bucket->occupied) {
                return NULL;
            }
            if (bucket->row_id == row_id) {
                return row_state_map->entries + bucket->entry_index;
            }
            bucket_index = (bucket_index + 1U) & mask;
        }
        return NULL;
    }

    for (size_t i = 0U; i < row_state_map->count; ++i) {
        if (row_state_map->entries[i].source_row_id == row_id) {
            return row_state_map->entries + i;
        }
    }

    return NULL;
}

static mylite_storage_result set_row_state_entry(
    mylite_storage_row_state_map *row_state_map,
    const mylite_storage_row_state_page *row_state_page
) {
    if (row_state_map->count == SIZE_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    mylite_storage_result result =
        ensure_row_state_map_buckets(row_state_map, row_state_map->count + 1U);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_row_state_entry *entry =
        find_row_state_entry(row_state_map, row_state_page->source_row_id);
    if (entry != NULL) {
        *entry = (mylite_storage_row_state_entry){
            .source_row_id = row_state_page->source_row_id,
            .replacement_row_id = row_state_page->replacement_row_id,
            .state_kind = row_state_page->state_kind,
        };
        return MYLITE_STORAGE_OK;
    }

    const size_t new_count = row_state_map->count + 1U;
    result = ensure_row_state_map_entry_capacity(row_state_map, new_count);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    row_state_map->entries[row_state_map->count] = (mylite_storage_row_state_entry){
        .source_row_id = row_state_page->source_row_id,
        .replacement_row_id = row_state_page->replacement_row_id,
        .state_kind = row_state_page->state_kind,
    };
    row_state_map->count = new_count;
    result =
        insert_row_state_map_bucket(row_state_map, row_state_page->source_row_id, new_count - 1U);
    if (result != MYLITE_STORAGE_OK) {
        row_state_map->count = new_count - 1U;
        row_state_map->entries[row_state_map->count] = (mylite_storage_row_state_entry){0};
    }
    return result;
}

static mylite_storage_result ensure_row_state_map_entry_capacity(
    mylite_storage_row_state_map *row_state_map,
    size_t minimum_capacity
) {
    if (row_state_map->capacity >= minimum_capacity) {
        return MYLITE_STORAGE_OK;
    }

    size_t next_capacity = row_state_map->capacity == 0U ? 128U : row_state_map->capacity;
    while (next_capacity < minimum_capacity) {
        if (next_capacity > SIZE_MAX / 2U) {
            return MYLITE_STORAGE_FULL;
        }
        next_capacity *= 2U;
    }
    if (next_capacity > SIZE_MAX / sizeof(*row_state_map->entries)) {
        return MYLITE_STORAGE_FULL;
    }

    mylite_storage_row_state_entry *entries = (mylite_storage_row_state_entry *)
        realloc(row_state_map->entries, next_capacity * sizeof(*row_state_map->entries));
    if (entries == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    row_state_map->entries = entries;
    row_state_map->capacity = next_capacity;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result ensure_row_state_map_buckets(
    mylite_storage_row_state_map *row_state_map,
    size_t minimum_entry_count
) {
    const size_t minimum_bucket_capacity = row_state_map_bucket_capacity(minimum_entry_count);
    if (minimum_bucket_capacity == 0U) {
        return MYLITE_STORAGE_FULL;
    }
    if (row_state_map->bucket_capacity >= minimum_bucket_capacity) {
        return MYLITE_STORAGE_OK;
    }
    return rebuild_row_state_map_buckets(row_state_map, minimum_bucket_capacity);
}

static size_t row_state_map_bucket_capacity(size_t entry_count) {
    if (entry_count > SIZE_MAX / 2U) {
        return 0U;
    }

    const size_t target_count = entry_count * 2U;
    size_t bucket_capacity = 128U;
    while (bucket_capacity < target_count) {
        if (bucket_capacity > SIZE_MAX / 2U) {
            return 0U;
        }
        bucket_capacity *= 2U;
    }
    return bucket_capacity;
}

static mylite_storage_result rebuild_row_state_map_buckets(
    mylite_storage_row_state_map *row_state_map,
    size_t bucket_capacity
) {
    if (bucket_capacity > SIZE_MAX / sizeof(*row_state_map->buckets)) {
        return MYLITE_STORAGE_FULL;
    }

    mylite_storage_row_state_map_bucket *buckets =
        (mylite_storage_row_state_map_bucket *)calloc(bucket_capacity, sizeof(*buckets));
    if (buckets == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    for (size_t i = 0U; i < row_state_map->count; ++i) {
        if (!place_row_state_map_bucket(
                buckets,
                bucket_capacity,
                row_state_map->entries[i].source_row_id,
                i
            )) {
            free(buckets);
            return MYLITE_STORAGE_FULL;
        }
    }

    free(row_state_map->buckets);
    row_state_map->buckets = buckets;
    row_state_map->bucket_capacity = bucket_capacity;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result insert_row_state_map_bucket(
    mylite_storage_row_state_map *row_state_map,
    unsigned long long row_id,
    size_t entry_index
) {
    if (row_state_map->bucket_capacity == 0U) {
        return MYLITE_STORAGE_FULL;
    }
    return place_row_state_map_bucket(
               row_state_map->buckets,
               row_state_map->bucket_capacity,
               row_id,
               entry_index
           )
               ? MYLITE_STORAGE_OK
               : MYLITE_STORAGE_FULL;
}

static int place_row_state_map_bucket(
    mylite_storage_row_state_map_bucket *buckets,
    size_t bucket_capacity,
    unsigned long long row_id,
    size_t entry_index
) {
    const size_t mask = bucket_capacity - 1U;
    size_t bucket_index = hash_row_id(row_id) & mask;
    for (size_t probes = 0U; probes < bucket_capacity; ++probes) {
        mylite_storage_row_state_map_bucket *bucket = buckets + bucket_index;
        if (!bucket->occupied) {
            *bucket = (mylite_storage_row_state_map_bucket){
                .row_id = row_id,
                .entry_index = entry_index,
                .occupied = 1,
            };
            return 1;
        }
        if (bucket->row_id == row_id) {
            bucket->entry_index = entry_index;
            return 1;
        }
        bucket_index = (bucket_index + 1U) & mask;
    }

    return 0;
}

static void free_row_state_map(mylite_storage_row_state_map *row_state_map) {
    free(row_state_map->entries);
    free(row_state_map->buckets);
    *row_state_map = (mylite_storage_row_state_map){0};
}

static mylite_storage_result append_live_row_id(
    void *ctx,
    const mylite_storage_row_page *row_page
) {
    mylite_storage_row_id_list *rows = (mylite_storage_row_id_list *)ctx;
    if (rows->count == SIZE_MAX || rows->count >= SIZE_MAX / sizeof(unsigned long long)) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_count = rows->count + 1U;
    unsigned long long *row_ids =
        (unsigned long long *)realloc(rows->row_ids, new_count * sizeof(unsigned long long));
    if (row_ids == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    rows->row_ids = row_ids;
    rows->row_ids[rows->count] = row_page->row_id;
    rows->count = new_count;
    return MYLITE_STORAGE_OK;
}

static int truncate_needs_publication(
    const mylite_storage_row_id_list *live_rows,
    int reset_auto_increment
) {
    return live_rows->count > 0U || reset_auto_increment;
}

static mylite_storage_result write_truncate_publication(
    FILE *file,
    const char *filename,
    mylite_storage_header *header,
    unsigned long long table_id,
    const mylite_storage_row_id_list *live_rows,
    int reset_auto_increment
) {
    mylite_storage_result result =
        validate_truncate_page_capacity(header, live_rows->count, reset_auto_increment);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    result = begin_write_journal(file, filename, header, 0);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    unsigned long long next_page_id = header->page_count;
    result = write_truncate_row_state_pages(
        file,
        header,
        table_id,
        live_rows,
        next_page_id,
        &next_page_id
    );
    if (result == MYLITE_STORAGE_OK && reset_auto_increment) {
        result =
            write_truncate_auto_increment_page(file, header, table_id, next_page_id, &next_page_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = publish_header_page_count(file, header, next_page_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = finish_write_journal(file, filename);
    }
    return result;
}

static mylite_storage_result validate_truncate_page_capacity(
    const mylite_storage_header *header,
    size_t live_row_count,
    int reset_auto_increment
) {
    unsigned long long required_pages = (unsigned long long)live_row_count;
    if (live_row_count > (size_t)ULLONG_MAX ||
        (reset_auto_increment && required_pages == ULLONG_MAX)) {
        return MYLITE_STORAGE_FULL;
    }
    if (reset_auto_increment) {
        ++required_pages;
    }
    if (header->page_count > ULLONG_MAX - required_pages) {
        return MYLITE_STORAGE_FULL;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_truncate_row_state_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const mylite_storage_row_id_list *live_rows,
    unsigned long long first_page_id,
    unsigned long long *out_next_page_id
) {
    unsigned long long next_page_id = first_page_id;
    for (size_t i = 0U; i < live_rows->count; ++i) {
        const mylite_storage_row_state_page row_state = {
            .table_id = table_id,
            .source_row_id = live_rows->row_ids[i],
            .replacement_row_id = 0ULL,
            .state_kind = MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_DELETE,
        };
        unsigned char state_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_row_state_page(state_page, next_page_id, &row_state);

        const mylite_storage_result result =
            write_page_at(file, next_page_id, header->page_size, state_page);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        ++next_page_id;
    }

    *out_next_page_id = next_page_id;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_truncate_auto_increment_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long page_id,
    unsigned long long *out_next_page_id
) {
    unsigned char autoincrement_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    encode_autoincrement_page(autoincrement_page, page_id, table_id, 1ULL);

    const mylite_storage_result result =
        write_page_at(file, page_id, header->page_size, autoincrement_page);
    if (result == MYLITE_STORAGE_OK) {
        *out_next_page_id = page_id + 1ULL;
    }
    return result;
}

static mylite_storage_result publish_header_page_count(
    FILE *file,
    mylite_storage_header *header,
    unsigned long long page_count
) {
    header->page_count = page_count;
    return publish_header(file, header);
}

static mylite_storage_result initialize_rowset_builder(
    mylite_storage_rowset_builder *builder,
    mylite_storage_rowset *rowset,
    size_t metadata_capacity
) {
    *builder = (mylite_storage_rowset_builder){
        .rowset = rowset,
        .metadata_capacity = metadata_capacity,
    };
    if (metadata_capacity == 0U) {
        return MYLITE_STORAGE_OK;
    }
    if (metadata_capacity > SIZE_MAX / sizeof(size_t) ||
        metadata_capacity > SIZE_MAX / sizeof(unsigned long long)) {
        return MYLITE_STORAGE_FULL;
    }

    rowset->row_offsets = (size_t *)malloc(metadata_capacity * sizeof(size_t));
    rowset->row_sizes = (size_t *)malloc(metadata_capacity * sizeof(size_t));
    rowset->row_ids = (unsigned long long *)malloc(metadata_capacity * sizeof(unsigned long long));
    if (rowset->row_offsets == NULL || rowset->row_sizes == NULL || rowset->row_ids == NULL) {
        mylite_storage_free_rowset(rowset);
        *builder = (mylite_storage_rowset_builder){0};
        return MYLITE_STORAGE_NOMEM;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_row_to_rowset_builder(
    mylite_storage_rowset_builder *builder,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
) {
    mylite_storage_rowset *rowset = builder->rowset;
    if (rowset->row_count >= builder->metadata_capacity ||
        row_size > SIZE_MAX - rowset->row_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_row_bytes = rowset->row_bytes + row_size;
    mylite_storage_result result =
        ensure_rowset_builder_row_capacity(builder, new_row_bytes, row_size);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    memcpy(rowset->rows + rowset->row_bytes, row, row_size);
    rowset->row_offsets[rowset->row_count] = rowset->row_bytes;
    rowset->row_sizes[rowset->row_count] = row_size;
    rowset->row_ids[rowset->row_count] = row_id;
    ++rowset->row_count;
    rowset->row_bytes = new_row_bytes;
    if (rowset->row_count == 1U) {
        rowset->row_size = row_size;
    } else if (rowset->row_size != row_size) {
        rowset->row_size = 0U;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result ensure_rowset_builder_row_capacity(
    mylite_storage_rowset_builder *builder,
    size_t required_bytes,
    size_t first_row_size
) {
    if (required_bytes <= builder->row_capacity) {
        return MYLITE_STORAGE_OK;
    }

    size_t new_capacity = builder->row_capacity;
    if (new_capacity == 0U) {
        if (first_row_size > 0U && builder->metadata_capacity > 0U &&
            first_row_size <= SIZE_MAX / builder->metadata_capacity) {
            new_capacity = first_row_size * builder->metadata_capacity;
        } else {
            new_capacity = required_bytes;
        }
    }
    while (new_capacity < required_bytes) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = required_bytes;
            break;
        }
        new_capacity *= 2U;
    }

    unsigned char *rows = (unsigned char *)realloc(builder->rowset->rows, new_capacity);
    if (rows == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    builder->rowset->rows = rows;
    builder->row_capacity = new_capacity;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_row_id_to_list(
    mylite_storage_row_id_list *list,
    unsigned long long row_id
) {
    if (list->count == SIZE_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_count = list->count + 1U;
    if (new_count > SIZE_MAX / sizeof(unsigned long long)) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned long long *row_ids =
        (unsigned long long *)realloc(list->row_ids, new_count * sizeof(unsigned long long));
    if (row_ids == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    list->row_ids = row_ids;
    list->row_ids[list->count] = row_id;
    list->count = new_count;
    return MYLITE_STORAGE_OK;
}

static void remove_row_id_from_list(mylite_storage_row_id_list *list, unsigned long long row_id) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < list->count; ++read_index) {
        if (list->row_ids[read_index] != row_id) {
            list->row_ids[write_index] = list->row_ids[read_index];
            ++write_index;
        }
    }
    list->count = write_index;
}

static void replace_row_id_in_list(
    mylite_storage_row_id_list *list,
    unsigned long long old_row_id,
    unsigned long long new_row_id
) {
    for (size_t i = 0U; i < list->count; ++i) {
        if (list->row_ids[i] == old_row_id) {
            list->row_ids[i] = new_row_id;
        }
    }
}

static mylite_storage_result append_index_prefix_match(
    mylite_storage_index_prefix_match_list *list,
    unsigned long long row_id,
    unsigned index_number
) {
    if (list->count == SIZE_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_count = list->count + 1U;
    if (new_count > SIZE_MAX / sizeof(*list->matches)) {
        return MYLITE_STORAGE_FULL;
    }

    mylite_storage_index_prefix_match *matches = (mylite_storage_index_prefix_match *)
        realloc(list->matches, new_count * sizeof(*list->matches));
    if (matches == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    list->matches = matches;
    list->matches[list->count] = (mylite_storage_index_prefix_match){
        .row_id = row_id,
        .index_number = index_number,
    };
    list->count = new_count;
    return MYLITE_STORAGE_OK;
}

static void remove_index_prefix_matches(
    mylite_storage_index_prefix_match_list *list,
    unsigned long long row_id,
    unsigned index_number
) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < list->count; ++read_index) {
        const mylite_storage_index_prefix_match match = list->matches[read_index];
        if (match.row_id == row_id && match.index_number == index_number) {
            continue;
        }
        list->matches[write_index] = match;
        ++write_index;
    }
    list->count = write_index;
}

static void remove_index_prefix_matches_by_row_id(
    mylite_storage_index_prefix_match_list *list,
    unsigned long long row_id
) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < list->count; ++read_index) {
        const mylite_storage_index_prefix_match match = list->matches[read_index];
        if (match.row_id == row_id) {
            continue;
        }
        list->matches[write_index] = match;
        ++write_index;
    }
    list->count = write_index;
}

static void replace_index_prefix_match_row_id(
    mylite_storage_index_prefix_match_list *list,
    unsigned long long old_row_id,
    unsigned long long new_row_id
) {
    for (size_t i = 0U; i < list->count; ++i) {
        if (list->matches[i].row_id == old_row_id) {
            list->matches[i].row_id = new_row_id;
        }
    }
}

static mylite_storage_result find_exact_index_row_id(
    FILE *file,
    const char *filename,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    const mylite_storage_catalog_entry *table_entry,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id
) {
    mylite_storage_row_id_list row_ids = {0};
    int used_cache = 0;
    int used_leaf = 0;
    mylite_storage_result result = find_cached_exact_index_entry(
        file,
        header,
        filename,
        table_entry->table_id,
        index_number,
        key,
        key_size,
        out_row_id,
        &used_cache
    );
    if (result == MYLITE_STORAGE_OK && !used_cache && catalog_page != NULL) {
        result = read_index_leaf_exact_row_ids(
            file,
            filename,
            header,
            catalog_page,
            table_entry->table_id,
            schema_name,
            table_name,
            index_number,
            key,
            key_size,
            &row_ids,
            &used_leaf
        );
    }
    if (result == MYLITE_STORAGE_OK && !used_cache && !used_leaf) {
        result = find_cached_durable_exact_index_entry(
            file,
            header,
            filename,
            table_entry->table_id,
            index_number,
            key,
            key_size,
            out_row_id,
            &used_cache
        );
    }
    if (result == MYLITE_STORAGE_OK && !used_cache && !used_leaf) {
        result = scan_exact_index_row_ids(
            file,
            header,
            table_entry->table_id,
            index_number,
            key,
            key_size,
            &row_ids
        );
    }
    if (result == MYLITE_STORAGE_OK && !used_cache && row_ids.count != 0U) {
        *out_row_id = row_ids.row_ids[0];
    }

    free(row_ids.row_ids);
    return result;
}

static mylite_storage_result find_cached_exact_index_entry(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    int *out_used_cache
) {
    *out_used_cache = 0;
    mylite_storage_statement *statement = active_exact_index_cache_statement_for(filename);
    if (statement == NULL) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_exact_index_cache *cache =
        find_exact_index_cache(&statement->exact_index_caches, table_id, index_number, key_size);
    const int created_cache = cache == NULL;
    if (cache == NULL) {
        mylite_storage_result result = append_exact_index_cache(
            &statement->exact_index_caches,
            table_id,
            index_number,
            key_size,
            &cache
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }

        int seeded_cache = 0;
        result = seed_active_exact_index_cache(
            filename,
            header,
            table_id,
            index_number,
            key_size,
            cache,
            &seeded_cache
        );
        if (result == MYLITE_STORAGE_OK && !seeded_cache) {
            result = load_exact_index_cache(file, header, cache);
        }
        if (result != MYLITE_STORAGE_OK) {
            free_exact_index_cache(cache);
            if (created_cache && statement->exact_index_caches.count > 0U) {
                --statement->exact_index_caches.count;
            }
            return result;
        }
    }

    *out_used_cache = 1;
    return find_exact_index_cache_entry_row_id(cache, key, out_row_id);
}

static mylite_storage_result find_cached_durable_exact_index_entry(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    int *out_used_cache
) {
    *out_used_cache = 0;
    mylite_storage_exact_index_cache *cache = NULL;
    mylite_storage_result result = durable_exact_index_cache_for(
        file,
        header,
        filename,
        table_id,
        index_number,
        key_size,
        &cache
    );
    if (result != MYLITE_STORAGE_OK || cache == NULL) {
        return result;
    }

    *out_used_cache = 1;
    return find_exact_index_cache_entry_row_id(cache, key, out_row_id);
}

static mylite_storage_result append_cached_durable_exact_index_entries(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries,
    int *out_used_cache
) {
    *out_used_cache = 0;
    mylite_storage_exact_index_cache *cache = NULL;
    mylite_storage_result result = durable_exact_index_cache_for(
        file,
        header,
        filename,
        table_id,
        index_number,
        key_size,
        &cache
    );
    if (result != MYLITE_STORAGE_OK || cache == NULL) {
        return result;
    }

    *out_used_cache = 1;
    return append_exact_index_cache_matches_to_entryset(cache, key, out_entries);
}

static mylite_storage_result find_exact_index_cache_entry_row_id(
    mylite_storage_exact_index_cache *cache,
    const unsigned char *key,
    unsigned long long *out_row_id
) {
    *out_row_id = 0ULL;
    if (cache->live_count == 0U) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_result result = ensure_exact_index_cache_buckets(cache);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    const size_t bucket_index = exact_index_cache_bucket_for_key(cache, key);
    for (size_t entry_index = cache->bucket_heads[bucket_index];
         entry_index != MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
         entry_index = cache->bucket_next[entry_index]) {
        if (cache->entry_live != NULL && !cache->entry_live[entry_index]) {
            continue;
        }
        const unsigned char *entry_key = cache->keys + (entry_index * cache->key_size);
        if (memcmp(entry_key, key, cache->key_size) == 0) {
            *out_row_id = cache->row_ids[entry_index];
            break;
        }
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_exact_index_cache_matches_to_entryset(
    mylite_storage_exact_index_cache *cache,
    const unsigned char *key,
    mylite_storage_index_entryset *out_entries
) {
    if (cache->key_size == 0U) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (cache->live_count == 0U) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_result result = ensure_exact_index_cache_buckets(cache);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    size_t match_count = 0U;
    const size_t bucket_index = exact_index_cache_bucket_for_key(cache, key);
    for (size_t entry_index = cache->bucket_heads[bucket_index];
         entry_index != MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
         entry_index = cache->bucket_next[entry_index]) {
        if (cache->entry_live != NULL && !cache->entry_live[entry_index]) {
            continue;
        }
        const unsigned char *entry_key = cache->keys + (entry_index * cache->key_size);
        if (memcmp(entry_key, key, cache->key_size) != 0) {
            continue;
        }
        ++match_count;
    }
    if (match_count == 0U) {
        return MYLITE_STORAGE_OK;
    }
    if (match_count > SIZE_MAX / cache->key_size) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t additional_key_bytes = match_count * cache->key_size;
    size_t first_entry = 0U;
    size_t first_key_offset = 0U;
    result = grow_index_entryset_for_append(
        out_entries,
        match_count,
        additional_key_bytes,
        &first_entry,
        &first_key_offset
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    size_t match_index = 0U;
    for (size_t entry_index = cache->bucket_heads[bucket_index];
         entry_index != MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
         entry_index = cache->bucket_next[entry_index]) {
        if (cache->entry_live != NULL && !cache->entry_live[entry_index]) {
            continue;
        }
        const unsigned char *entry_key = cache->keys + (entry_index * cache->key_size);
        if (memcmp(entry_key, key, cache->key_size) != 0) {
            continue;
        }

        const size_t out_entry_index = first_entry + match_index;
        const size_t key_offset = first_key_offset + (match_index * cache->key_size);
        memcpy(out_entries->keys + key_offset, entry_key, cache->key_size);
        out_entries->key_offsets[out_entry_index] = key_offset;
        out_entries->key_sizes[out_entry_index] = cache->key_size;
        out_entries->row_ids[out_entry_index] = cache->row_ids[entry_index];
        ++match_index;
    }
    out_entries->entry_count = first_entry + match_count;
    out_entries->key_bytes = first_key_offset + additional_key_bytes;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result durable_exact_index_cache_for(
    FILE *file,
    const mylite_storage_header *header,
    const char *filename,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size,
    mylite_storage_exact_index_cache **out_cache
) {
    *out_cache = NULL;
    if (active_statement_for_any_owner(filename) != NULL || active_read_snapshot_for(filename)) {
        return MYLITE_STORAGE_OK;
    }

    mylite_storage_exact_index_cache *cache =
        find_durable_exact_index_cache(filename, header, table_id, index_number, key_size);
    const int created_cache = cache == NULL;
    if (cache == NULL) {
        if (durable_exact_index_caches.count >= MYLITE_STORAGE_DURABLE_EXACT_INDEX_CACHE_LIMIT) {
            clear_durable_exact_index_caches(NULL);
        }
        mylite_storage_result result = append_durable_exact_index_cache(
            filename,
            header,
            table_id,
            index_number,
            key_size,
            &cache
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }

        result = load_exact_index_cache(file, header, cache);
        if (result != MYLITE_STORAGE_OK) {
            free_exact_index_cache(cache);
            if (created_cache && durable_exact_index_caches.count > 0U) {
                --durable_exact_index_caches.count;
            }
            return result;
        }
    }

    *out_cache = cache;
    return MYLITE_STORAGE_OK;
}

static void promote_statement_exact_index_caches(const mylite_storage_statement *statement) {
    if (statement == NULL || statement->exact_index_caches.count == 0U) {
        return;
    }

    const mylite_storage_header *header =
        statement->has_current_header ? &statement->current_header : &statement->header;
    for (size_t i = 0U; i < statement->exact_index_caches.count; ++i) {
        const mylite_storage_exact_index_cache *source =
            statement->exact_index_caches.entries + i;
        mylite_storage_exact_index_cache *destination = ensure_durable_exact_index_cache(
            statement->filename,
            header,
            source->table_id,
            source->index_number,
            source->key_size
        );
        if (destination != NULL) {
            (void)copy_exact_index_cache_entries(destination, source);
        }
    }
}

static mylite_storage_result seed_active_exact_index_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size,
    mylite_storage_exact_index_cache *cache,
    int *out_seeded_cache
) {
    *out_seeded_cache = 0;
    const mylite_storage_exact_index_cache *durable_cache =
        find_durable_exact_index_cache(filename, header, table_id, index_number, key_size);
    if (durable_cache == NULL) {
        return MYLITE_STORAGE_OK;
    }

    const mylite_storage_result result = copy_exact_index_cache_entries(cache, durable_cache);
    if (result == MYLITE_STORAGE_OK) {
        *out_seeded_cache = 1;
    }
    return result;
}

static mylite_storage_result append_active_exact_index_cache_entries(
    const char *filename,
    unsigned long long table_id,
    unsigned long long row_id,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count
) {
    mylite_storage_statement *statement = active_exact_index_cache_statement_for(filename);
    if (statement == NULL || statement->exact_index_caches.count == 0U) {
        return MYLITE_STORAGE_OK;
    }

    for (size_t i = 0U; i < index_entry_count; ++i) {
        mylite_storage_exact_index_cache *cache = find_exact_index_cache(
            &statement->exact_index_caches,
            table_id,
            index_entries[i].index_number,
            index_entries[i].key_size
        );
        if (cache == NULL) {
            continue;
        }

        const mylite_storage_result result =
            append_exact_index_cache_entry(cache, index_entries[i].key, row_id);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }
    return MYLITE_STORAGE_OK;
}

static void replace_active_exact_index_cache_entries(
    const char *filename,
    unsigned long long table_id,
    unsigned long long old_row_id,
    unsigned long long new_row_id,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count
) {
    mylite_storage_statement *statement = active_exact_index_cache_statement_for(filename);
    if (statement == NULL || statement->exact_index_caches.count == 0U) {
        return;
    }

    for (size_t i = 0U; i < statement->exact_index_caches.count; ++i) {
        mylite_storage_exact_index_cache *cache = statement->exact_index_caches.entries + i;
        if (cache->table_id != table_id) {
            continue;
        }
        remove_exact_index_cache_entries_by_row_id(cache, old_row_id);
        if (new_row_id == 0ULL) {
            continue;
        }
        for (size_t entry_index = 0U; entry_index < index_entry_count; ++entry_index) {
            const mylite_storage_index_entry *index_entry = index_entries + entry_index;
            if (index_entry->index_number != cache->index_number ||
                index_entry->key_size != cache->key_size) {
                continue;
            }
            if (append_exact_index_cache_entry(cache, index_entry->key, new_row_id) !=
                MYLITE_STORAGE_OK) {
                clear_exact_index_caches(statement);
                return;
            }
        }
    }
}

static void invalidate_exact_index_caches(const char *filename) {
    clear_exact_index_caches(active_exact_index_cache_statement_for(filename));
    clear_statement_chain_live_row_caches(active_statement_for(filename));
    clear_statement_chain_live_row_id_caches(active_statement_for(filename));
    clear_statement_chain_row_payload_caches(active_statement_for(filename));
    clear_durable_exact_index_caches(filename);
}

static mylite_storage_result append_cached_row_payload_to_builder(
    const mylite_storage_row_payload_cache *cache,
    unsigned long long row_id,
    mylite_storage_rowset_builder *builder,
    int *out_used_cache
) {
    *out_used_cache = 0;
    if (cache == NULL) {
        return MYLITE_STORAGE_OK;
    }

    const mylite_storage_row_payload_cache_entry *entry =
        find_row_payload_cache_entry(cache, row_id);
    if (entry == NULL) {
        return MYLITE_STORAGE_OK;
    }

    *out_used_cache = 1;
    return append_row_to_rowset_builder(builder, row_id, entry->row, entry->row_size);
}

static const mylite_storage_row_payload_cache_entry *active_row_payload_cache_entry_for(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
) {
    mylite_storage_row_payload_cache *cache =
        active_row_payload_cache_for(filename, header, table_id);
    return cache == NULL ? NULL : find_row_payload_cache_entry(cache, row_id);
}

static void store_active_row_payload(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
) {
    mylite_storage_row_payload_cache *cache =
        ensure_active_row_payload_cache(filename, header, table_id);
    if (cache == NULL) {
        return;
    }

    if (cache->count >= MYLITE_STORAGE_ACTIVE_ROW_PAYLOAD_ENTRY_LIMIT) {
        mylite_storage_statement *statement = active_row_payload_cache_statement_for(filename);
        clear_row_payload_caches(statement);
        cache = ensure_active_row_payload_cache(filename, header, table_id);
        if (cache == NULL) {
            return;
        }
    }
    (void)put_row_payload_cache_entry(cache, row_id, row, row_size);
}

static void replace_active_row_payload(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long old_row_id,
    unsigned long long new_row_id,
    const unsigned char *new_row,
    size_t new_row_size
) {
    mylite_storage_row_payload_cache *cache =
        active_row_payload_cache_for(filename, header, table_id);
    if (cache == NULL) {
        return;
    }
    if (new_row_id == 0ULL ||
        replace_row_payload_cache_entry(
            cache,
            old_row_id,
            new_row_id,
            new_row,
            new_row_size
        ) != MYLITE_STORAGE_OK) {
        remove_row_payload_cache_entry(cache, old_row_id);
    }
}

static void remove_active_row_payload(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long row_id
) {
    mylite_storage_row_payload_cache *cache =
        active_row_payload_cache_for(filename, header, table_id);
    if (cache != NULL) {
        remove_row_payload_cache_entry(cache, row_id);
    }
}

static mylite_storage_result copy_cached_row_payload(
    const mylite_storage_row_payload_cache_entry *entry,
    unsigned char **out_row,
    size_t *inout_row_capacity,
    size_t *out_row_size
) {
    return copy_row_payload_to_output(
        entry->row,
        entry->row_size,
        out_row,
        inout_row_capacity,
        out_row_size
    );
}

static mylite_storage_result copy_row_payload_to_output(
    const unsigned char *row,
    size_t row_size,
    unsigned char **out_row,
    size_t *inout_row_capacity,
    size_t *out_row_size
) {
    if (row_size == 0U) {
        *out_row_size = 0U;
        return MYLITE_STORAGE_OK;
    }

    unsigned char *target = NULL;
    if (inout_row_capacity != NULL) {
        if (*out_row == NULL || *inout_row_capacity < row_size) {
            target = (unsigned char *)realloc(*out_row, row_size);
            if (target == NULL) {
                return MYLITE_STORAGE_NOMEM;
            }
            *out_row = target;
            *inout_row_capacity = row_size;
        } else {
            target = *out_row;
        }
    } else {
        target = (unsigned char *)malloc(row_size);
        if (target == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        *out_row = target;
    }

    if (target == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    memcpy(target, row, row_size);
    *out_row_size = row_size;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_row_payload_cache *active_row_payload_cache_for(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    mylite_storage_statement *statement = active_row_payload_cache_statement_for(filename);
    if (statement == NULL) {
        return NULL;
    }
    return find_active_row_payload_cache(
        &statement->row_payload_caches,
        filename,
        header,
        table_id
    );
}

static mylite_storage_row_payload_cache *ensure_active_row_payload_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    mylite_storage_statement *statement = active_row_payload_cache_statement_for(filename);
    if (statement == NULL) {
        return NULL;
    }

    mylite_storage_row_payload_cache *cache = find_active_row_payload_cache(
        &statement->row_payload_caches,
        filename,
        header,
        table_id
    );
    if (cache != NULL) {
        return cache;
    }

    if (statement->row_payload_caches.count >= MYLITE_STORAGE_ACTIVE_ROW_PAYLOAD_CACHE_LIMIT) {
        clear_row_payload_caches(statement);
    }
    if (append_active_row_payload_cache(
            &statement->row_payload_caches,
            filename,
            header,
            table_id,
            &cache
        ) != MYLITE_STORAGE_OK) {
        return NULL;
    }
    return cache;
}

static mylite_storage_row_payload_cache *find_active_row_payload_cache(
    mylite_storage_row_payload_cache_set *caches,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    for (size_t i = 0U; i < caches->count; ++i) {
        mylite_storage_row_payload_cache *cache = caches->entries + i;
        if (cache->filename != NULL && strcmp(cache->filename, filename) == 0 &&
            cache->catalog_root_page == header->catalog_root_page &&
            cache->catalog_generation == header->catalog_generation &&
            cache->table_id == table_id) {
            return cache;
        }
    }
    return NULL;
}

static mylite_storage_result append_active_row_payload_cache(
    mylite_storage_row_payload_cache_set *caches,
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **out_cache
) {
    if (caches->count == caches->capacity) {
        const size_t next_capacity = caches->capacity == 0U ? 4U : caches->capacity * 2U;
        if (next_capacity <= caches->capacity ||
            next_capacity > SIZE_MAX / sizeof(*caches->entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_row_payload_cache *entries = (mylite_storage_row_payload_cache *)realloc(
            caches->entries,
            next_capacity * sizeof(*caches->entries)
        );
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        caches->entries = entries;
        caches->capacity = next_capacity;
    }

    mylite_storage_row_payload_cache *cache = caches->entries + caches->count;
    *cache = (mylite_storage_row_payload_cache){
        .filename = copy_filename(filename),
        .catalog_root_page = header->catalog_root_page,
        .catalog_generation = header->catalog_generation,
        .page_count = header->page_count,
        .table_id = table_id,
    };
    if (cache->filename == NULL) {
        *cache = (mylite_storage_row_payload_cache){0};
        return MYLITE_STORAGE_NOMEM;
    }
    ++caches->count;
    *out_cache = cache;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_cached_durable_row_payload_to_builder(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **cache,
    unsigned long long *cache_generation,
    unsigned long long row_id,
    mylite_storage_rowset_builder *builder,
    int *out_used_cache
) {
    *out_used_cache = 0;
    const mylite_storage_row_payload_cache *resolved_cache = durable_row_payload_cache_for_batch(
        filename,
        header,
        table_id,
        cache,
        cache_generation
    );
    return append_cached_row_payload_to_builder(resolved_cache, row_id, builder, out_used_cache);
}

static void store_durable_row_payload(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **cache,
    unsigned long long *cache_generation,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
) {
    if (!durable_row_payload_cache_available(filename)) {
        return;
    }
    *cache = durable_row_payload_cache_for_batch(
        filename,
        header,
        table_id,
        cache,
        cache_generation
    );
    if (*cache == NULL) {
        *cache = ensure_durable_row_payload_cache(
            filename,
            header,
            table_id,
            cache_generation
        );
    }
    if (*cache == NULL) {
        return;
    }

    if ((*cache)->count >= MYLITE_STORAGE_DURABLE_ROW_PAYLOAD_ENTRY_LIMIT) {
        clear_durable_row_payload_caches(filename);
        *cache = ensure_durable_row_payload_cache(
            filename,
            header,
            table_id,
            cache_generation
        );
        if (*cache == NULL) {
            return;
        }
    }
    (void)append_row_payload_cache_entry(*cache, row_id, row, row_size);
}

static void clear_durable_row_payload_caches(const char *filename) {
    size_t write_index = 0U;
    int cleared_cache = 0;
    for (size_t read_index = 0U; read_index < durable_row_payload_caches.count; ++read_index) {
        mylite_storage_row_payload_cache *cache = durable_row_payload_caches.entries + read_index;
        const int clear_cache =
            filename == NULL || (cache->filename != NULL && strcmp(cache->filename, filename) == 0);
        if (clear_cache) {
            cleared_cache = 1;
            free_row_payload_cache(cache);
            continue;
        }
        if (write_index != read_index) {
            durable_row_payload_caches.entries[write_index] = *cache;
        }
        ++write_index;
    }
    durable_row_payload_caches.count = write_index;
    if (durable_row_payload_caches.count == 0U) {
        free(durable_row_payload_caches.entries);
        durable_row_payload_caches = (mylite_storage_row_payload_cache_set){0};
    }
    if (cleared_cache) {
        ++durable_row_payload_caches_generation;
    }
}

static void retarget_durable_row_payload_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    size_t write_index = 0U;
    int changed_cache_set = 0;
    for (size_t read_index = 0U; read_index < durable_row_payload_caches.count; ++read_index) {
        mylite_storage_row_payload_cache *cache = durable_row_payload_caches.entries + read_index;
        const int same_file = cache->filename != NULL && strcmp(cache->filename, filename) == 0;
        if (same_file && cache->table_id == table_id) {
            changed_cache_set = 1;
            free_row_payload_cache(cache);
            continue;
        }
        if (same_file) {
            changed_cache_set = 1;
            cache->catalog_root_page = header->catalog_root_page;
            cache->catalog_generation = header->catalog_generation;
            cache->page_count = header->page_count;
        }
        if (write_index != read_index) {
            durable_row_payload_caches.entries[write_index] = *cache;
            changed_cache_set = 1;
        }
        ++write_index;
    }
    durable_row_payload_caches.count = write_index;
    if (durable_row_payload_caches.count == 0U) {
        free(durable_row_payload_caches.entries);
        durable_row_payload_caches = (mylite_storage_row_payload_cache_set){0};
    }
    if (changed_cache_set) {
        ++durable_row_payload_caches_generation;
    }
}

static mylite_storage_row_payload_cache *durable_row_payload_cache_for(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    if (!durable_row_payload_cache_available(filename)) {
        return NULL;
    }
    return find_durable_row_payload_cache(filename, header, table_id);
}

static mylite_storage_row_payload_cache *durable_row_payload_cache_for_batch(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **cache,
    unsigned long long *cache_generation
) {
    /* Cache pointers are reusable only until the cache set moves or compacts. */
    if (*cache_generation == durable_row_payload_caches_generation) {
        return *cache;
    }
    if (!durable_row_payload_cache_available(filename)) {
        *cache = NULL;
        *cache_generation = durable_row_payload_caches_generation;
        return NULL;
    }

    *cache = find_durable_row_payload_cache(filename, header, table_id);
    *cache_generation = durable_row_payload_caches_generation;
    return *cache;
}

static int durable_row_payload_cache_available(const char *filename) {
    return active_statement_for_any_owner(filename) == NULL && !active_read_snapshot_for(filename);
}

static mylite_storage_row_payload_cache *find_durable_row_payload_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    for (size_t i = 0U; i < durable_row_payload_caches.count; ++i) {
        mylite_storage_row_payload_cache *cache = durable_row_payload_caches.entries + i;
        if (cache->filename != NULL && strcmp(cache->filename, filename) == 0 &&
            cache->catalog_root_page == header->catalog_root_page &&
            cache->catalog_generation == header->catalog_generation &&
            cache->page_count == header->page_count && cache->table_id == table_id) {
            return cache;
        }
    }
    return NULL;
}

static mylite_storage_row_payload_cache *ensure_durable_row_payload_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long *cache_generation
) {
    if (!durable_row_payload_cache_available(filename)) {
        return NULL;
    }

    mylite_storage_row_payload_cache *cache =
        find_durable_row_payload_cache(filename, header, table_id);
    if (cache != NULL) {
        *cache_generation = durable_row_payload_caches_generation;
        return cache;
    }

    if (durable_row_payload_caches.count >= MYLITE_STORAGE_DURABLE_ROW_PAYLOAD_CACHE_LIMIT) {
        clear_durable_row_payload_caches(NULL);
    }
    if (append_durable_row_payload_cache(filename, header, table_id, &cache) !=
        MYLITE_STORAGE_OK) {
        return NULL;
    }
    *cache_generation = durable_row_payload_caches_generation;
    return cache;
}

static mylite_storage_result append_durable_row_payload_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_payload_cache **out_cache
) {
    if (durable_row_payload_caches.count == durable_row_payload_caches.capacity) {
        const size_t next_capacity = durable_row_payload_caches.capacity == 0U
                                         ? 4U
                                         : durable_row_payload_caches.capacity * 2U;
        if (next_capacity <= durable_row_payload_caches.capacity ||
            next_capacity > SIZE_MAX / sizeof(*durable_row_payload_caches.entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_row_payload_cache *entries = (mylite_storage_row_payload_cache *)realloc(
            durable_row_payload_caches.entries,
            next_capacity * sizeof(*durable_row_payload_caches.entries)
        );
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        if (entries != durable_row_payload_caches.entries) {
            ++durable_row_payload_caches_generation;
        }
        durable_row_payload_caches.entries = entries;
        durable_row_payload_caches.capacity = next_capacity;
    }

    mylite_storage_row_payload_cache *cache =
        durable_row_payload_caches.entries + durable_row_payload_caches.count;
    *cache = (mylite_storage_row_payload_cache){
        .catalog_root_page = header->catalog_root_page,
        .catalog_generation = header->catalog_generation,
        .page_count = header->page_count,
        .table_id = table_id,
    };
    cache->filename = copy_filename(filename);
    if (cache->filename == NULL) {
        *cache = (mylite_storage_row_payload_cache){0};
        return MYLITE_STORAGE_NOMEM;
    }
    ++durable_row_payload_caches.count;
    *out_cache = cache;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_row_payload_cache_entry(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
) {
    if (find_row_payload_cache_entry(cache, row_id) != NULL) {
        return MYLITE_STORAGE_OK;
    }
    if (cache->count == cache->capacity) {
        const size_t next_capacity = cache->capacity == 0U ? 64U : cache->capacity * 2U;
        if (next_capacity <= cache->capacity ||
            next_capacity > SIZE_MAX / sizeof(*cache->entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_row_payload_cache_entry *entries = (mylite_storage_row_payload_cache_entry *)
            realloc(cache->entries, next_capacity * sizeof(*cache->entries));
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        cache->entries = entries;
        cache->capacity = next_capacity;
    }
    mylite_storage_result result = ensure_row_payload_cache_buckets(cache, cache->count + 1U);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    unsigned char *row_copy = (unsigned char *)malloc(row_size);
    if (row_copy == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    memcpy(row_copy, row, row_size);
    const size_t entry_index = cache->count;
    cache->entries[entry_index] = (mylite_storage_row_payload_cache_entry){
        .row_id = row_id,
        .row = row_copy,
        .row_size = row_size,
    };
    ++cache->count;
    result = insert_row_payload_cache_bucket(cache, row_id, entry_index);
    if (result != MYLITE_STORAGE_OK) {
        free(row_copy);
        cache->entries[entry_index] = (mylite_storage_row_payload_cache_entry){0};
        --cache->count;
        return result;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result put_row_payload_cache_entry(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
) {
    remove_row_payload_cache_entry(cache, row_id);
    return append_row_payload_cache_entry(cache, row_id, row, row_size);
}

static mylite_storage_result replace_row_payload_cache_entry(
    mylite_storage_row_payload_cache *cache,
    unsigned long long old_row_id,
    unsigned long long new_row_id,
    const unsigned char *new_row,
    size_t new_row_size
) {
    mylite_storage_row_payload_cache_bucket *bucket =
        find_mutable_row_payload_cache_bucket(cache, old_row_id);
    if (bucket == NULL || bucket->entry_index >= cache->count) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    const size_t entry_index = bucket->entry_index;
    mylite_storage_row_payload_cache_entry *entry = cache->entries + entry_index;
    unsigned char *row = entry->row;
    if (entry->row_size != new_row_size) {
        row = (unsigned char *)malloc(new_row_size);
        if (row == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
    }

    if (old_row_id != new_row_id) {
        const mylite_storage_result result =
            insert_row_payload_cache_bucket(cache, new_row_id, entry_index);
        if (result != MYLITE_STORAGE_OK) {
            if (row != entry->row) {
                free(row);
            }
            return result;
        }
        remove_row_payload_cache_bucket(cache, old_row_id);
    }

    if (row != entry->row) {
        free(entry->row);
        entry->row = row;
        entry->row_size = new_row_size;
    }
    memcpy(entry->row, new_row, new_row_size);
    entry->row_id = new_row_id;
    if (old_row_id != new_row_id) {
        maybe_rebuild_row_payload_cache_buckets_after_tombstone(cache);
    }
    return MYLITE_STORAGE_OK;
}

static void remove_row_payload_cache_entry(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id
) {
    mylite_storage_row_payload_cache_bucket *bucket =
        find_mutable_row_payload_cache_bucket(cache, row_id);
    if (bucket == NULL || bucket->entry_index >= cache->count) {
        return;
    }

    const size_t removed_index = bucket->entry_index;
    const size_t last_index = cache->count - 1U;
    int rebuild_buckets = 0;
    free(cache->entries[removed_index].row);
    remove_row_payload_cache_bucket(cache, row_id);
    if (removed_index != last_index) {
        cache->entries[removed_index] = cache->entries[last_index];
        mylite_storage_row_payload_cache_bucket *moved_bucket =
            find_mutable_row_payload_cache_bucket(cache, cache->entries[removed_index].row_id);
        if (moved_bucket != NULL) {
            moved_bucket->entry_index = removed_index;
        } else {
            rebuild_buckets = 1;
        }
    }
    cache->entries[last_index] = (mylite_storage_row_payload_cache_entry){0};
    --cache->count;
    if (rebuild_buckets && cache->bucket_capacity != 0U) {
        (void)rebuild_row_payload_cache_buckets(cache, cache->bucket_capacity);
    } else {
        maybe_rebuild_row_payload_cache_buckets_after_tombstone(cache);
    }
}

static const mylite_storage_row_payload_cache_entry *find_row_payload_cache_entry(
    const mylite_storage_row_payload_cache *cache,
    unsigned long long row_id
) {
    if (cache->bucket_capacity == 0U) {
        return NULL;
    }

    const size_t mask = cache->bucket_capacity - 1U;
    size_t bucket_index = hash_row_id(row_id) & mask;
    for (size_t probes = 0U; probes < cache->bucket_capacity; ++probes) {
        const mylite_storage_row_payload_cache_bucket *bucket = cache->buckets + bucket_index;
        if (bucket->state == MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_EMPTY) {
            return NULL;
        }
        if (bucket->state == MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_OCCUPIED &&
            bucket->row_id == row_id) {
            return bucket->entry_index < cache->count ? cache->entries + bucket->entry_index : NULL;
        }
        bucket_index = (bucket_index + 1U) & mask;
    }
    return NULL;
}

static mylite_storage_row_payload_cache_bucket *find_mutable_row_payload_cache_bucket(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id
) {
    if (cache->bucket_capacity == 0U) {
        return NULL;
    }

    const size_t mask = cache->bucket_capacity - 1U;
    size_t bucket_index = hash_row_id(row_id) & mask;
    for (size_t probes = 0U; probes < cache->bucket_capacity; ++probes) {
        mylite_storage_row_payload_cache_bucket *bucket = cache->buckets + bucket_index;
        if (bucket->state == MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_EMPTY) {
            return NULL;
        }
        if (bucket->state == MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_OCCUPIED &&
            bucket->row_id == row_id) {
            return bucket;
        }
        bucket_index = (bucket_index + 1U) & mask;
    }
    return NULL;
}

static mylite_storage_result ensure_row_payload_cache_buckets(
    mylite_storage_row_payload_cache *cache,
    size_t next_count
) {
    if (next_count > SIZE_MAX / 2U) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t minimum_capacity = next_count * 2U;
    if (cache->bucket_capacity >= minimum_capacity) {
        return MYLITE_STORAGE_OK;
    }

    size_t next_capacity = cache->bucket_capacity == 0U ? 128U : cache->bucket_capacity;
    while (next_capacity < minimum_capacity) {
        if (next_capacity > SIZE_MAX / 2U) {
            return MYLITE_STORAGE_FULL;
        }
        next_capacity *= 2U;
    }
    return rebuild_row_payload_cache_buckets(cache, next_capacity);
}

static mylite_storage_result rebuild_row_payload_cache_buckets(
    mylite_storage_row_payload_cache *cache,
    size_t bucket_capacity
) {
    if (bucket_capacity > SIZE_MAX / sizeof(*cache->buckets)) {
        return MYLITE_STORAGE_FULL;
    }
    mylite_storage_row_payload_cache_bucket *buckets =
        (mylite_storage_row_payload_cache_bucket *)calloc(bucket_capacity, sizeof(*buckets));
    if (buckets == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    free(cache->buckets);
    cache->buckets = buckets;
    cache->bucket_capacity = bucket_capacity;
    cache->tombstone_count = 0U;
    for (size_t i = 0U; i < cache->count; ++i) {
        const mylite_storage_result result =
            insert_row_payload_cache_bucket(cache, cache->entries[i].row_id, i);
        if (result != MYLITE_STORAGE_OK) {
            free(cache->buckets);
            cache->buckets = NULL;
            cache->bucket_capacity = 0U;
            cache->tombstone_count = 0U;
            return result;
        }
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result insert_row_payload_cache_bucket(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id,
    size_t entry_index
) {
    if (cache->bucket_capacity == 0U) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t mask = cache->bucket_capacity - 1U;
    size_t bucket_index = hash_row_id(row_id) & mask;
    mylite_storage_row_payload_cache_bucket *first_deleted_bucket = NULL;
    for (size_t probes = 0U; probes < cache->bucket_capacity; ++probes) {
        mylite_storage_row_payload_cache_bucket *bucket = cache->buckets + bucket_index;
        if (bucket->state == MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_EMPTY) {
            if (first_deleted_bucket != NULL) {
                bucket = first_deleted_bucket;
                --cache->tombstone_count;
            }
            *bucket = (mylite_storage_row_payload_cache_bucket){
                .row_id = row_id,
                .entry_index = entry_index,
                .state = MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_OCCUPIED,
            };
            return MYLITE_STORAGE_OK;
        }
        if (bucket->state == MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_DELETED) {
            if (first_deleted_bucket == NULL) {
                first_deleted_bucket = bucket;
            }
        } else if (bucket->row_id == row_id) {
            bucket->entry_index = entry_index;
            return MYLITE_STORAGE_OK;
        }
        bucket_index = (bucket_index + 1U) & mask;
    }

    if (first_deleted_bucket != NULL) {
        --cache->tombstone_count;
        *first_deleted_bucket = (mylite_storage_row_payload_cache_bucket){
            .row_id = row_id,
            .entry_index = entry_index,
            .state = MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_OCCUPIED,
        };
        return MYLITE_STORAGE_OK;
    }
    return MYLITE_STORAGE_FULL;
}

static void remove_row_payload_cache_bucket(
    mylite_storage_row_payload_cache *cache,
    unsigned long long row_id
) {
    mylite_storage_row_payload_cache_bucket *bucket =
        find_mutable_row_payload_cache_bucket(cache, row_id);
    if (bucket == NULL) {
        return;
    }
    *bucket = (mylite_storage_row_payload_cache_bucket){
        .state = MYLITE_STORAGE_ROW_PAYLOAD_BUCKET_DELETED,
    };
    ++cache->tombstone_count;
}

static void maybe_rebuild_row_payload_cache_buckets_after_tombstone(
    mylite_storage_row_payload_cache *cache
) {
    if (cache->bucket_capacity != 0U && cache->tombstone_count > cache->count) {
        (void)rebuild_row_payload_cache_buckets(cache, cache->bucket_capacity);
    }
}

static size_t hash_row_id(unsigned long long row_id) {
    row_id ^= row_id >> 33U;
    row_id *= 0xff51afd7ed558ccdULL;
    row_id ^= row_id >> 33U;
    row_id *= 0xc4ceb9fe1a85ec53ULL;
    row_id ^= row_id >> 33U;
    return (size_t)row_id;
}

static void free_row_payload_cache(mylite_storage_row_payload_cache *cache) {
    for (size_t i = 0U; i < cache->count; ++i) {
        free(cache->entries[i].row);
    }
    free(cache->entries);
    free(cache->buckets);
    free(cache->filename);
    *cache = (mylite_storage_row_payload_cache){0};
}

static mylite_storage_result read_cached_durable_index_leaf_page(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page,
    int *out_used_cache
) {
    *out_used_cache = 0;
    mylite_storage_index_leaf_page_cache *cache =
        durable_index_leaf_page_cache_for(filename, header);
    if (cache == NULL) {
        return MYLITE_STORAGE_OK;
    }

    const mylite_storage_index_leaf_page_cache_entry *entry =
        find_index_leaf_page_cache_entry(cache, page_id);
    if (entry == NULL) {
        return MYLITE_STORAGE_OK;
    }

    memcpy(page, entry->page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    *out_index_leaf_page = (mylite_storage_index_leaf_page){
        .table_id = entry->table_id,
        .index_number = entry->index_number,
        .key_size = entry->key_size,
        .entry_count = entry->entry_count,
        .used_bytes = entry->used_bytes,
        .payload = page + MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET,
    };
    *out_used_cache = 1;
    return MYLITE_STORAGE_OK;
}

static void store_durable_index_leaf_page(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    const mylite_storage_index_leaf_page *leaf_page
) {
    if (active_statement_for_any_owner(filename) != NULL || active_read_snapshot_for(filename)) {
        return;
    }

    mylite_storage_index_leaf_page_cache *cache =
        durable_index_leaf_page_cache_for(filename, header);
    if (cache == NULL) {
        if (durable_index_leaf_page_caches.count >=
            MYLITE_STORAGE_DURABLE_INDEX_LEAF_PAGE_CACHE_LIMIT) {
            clear_durable_index_leaf_page_caches(NULL);
        }
        if (append_durable_index_leaf_page_cache(filename, header, &cache) != MYLITE_STORAGE_OK) {
            return;
        }
    }
    if (cache->count >= MYLITE_STORAGE_DURABLE_INDEX_LEAF_PAGE_ENTRY_LIMIT) {
        clear_durable_index_leaf_page_caches(filename);
        cache = NULL;
        if (append_durable_index_leaf_page_cache(filename, header, &cache) != MYLITE_STORAGE_OK) {
            return;
        }
    }
    (void)append_index_leaf_page_cache_entry(cache, page_id, page, leaf_page);
}

static void clear_durable_index_leaf_page_caches(const char *filename) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < durable_index_leaf_page_caches.count; ++read_index) {
        mylite_storage_index_leaf_page_cache *cache =
            durable_index_leaf_page_caches.entries + read_index;
        const int clear_cache =
            filename == NULL || (cache->filename != NULL && strcmp(cache->filename, filename) == 0);
        if (clear_cache) {
            free_index_leaf_page_cache(cache);
            continue;
        }
        if (write_index != read_index) {
            durable_index_leaf_page_caches.entries[write_index] = *cache;
        }
        ++write_index;
    }
    durable_index_leaf_page_caches.count = write_index;
    if (durable_index_leaf_page_caches.count == 0U) {
        free(durable_index_leaf_page_caches.entries);
        durable_index_leaf_page_caches = (mylite_storage_index_leaf_page_cache_set){0};
    }
}

static void retarget_durable_index_leaf_page_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header
) {
    for (size_t i = 0U; i < durable_index_leaf_page_caches.count; ++i) {
        mylite_storage_index_leaf_page_cache *cache = durable_index_leaf_page_caches.entries + i;
        if (cache->filename != NULL && strcmp(cache->filename, filename) == 0) {
            cache->catalog_root_page = header->catalog_root_page;
            cache->catalog_generation = header->catalog_generation;
            cache->page_count = header->page_count;
        }
    }
}

static mylite_storage_index_leaf_page_cache *durable_index_leaf_page_cache_for(
    const char *filename,
    const mylite_storage_header *header
) {
    if (active_statement_for_any_owner(filename) != NULL || active_read_snapshot_for(filename)) {
        return NULL;
    }
    return find_durable_index_leaf_page_cache(filename, header);
}

static mylite_storage_index_leaf_page_cache *find_durable_index_leaf_page_cache(
    const char *filename,
    const mylite_storage_header *header
) {
    for (size_t i = 0U; i < durable_index_leaf_page_caches.count; ++i) {
        mylite_storage_index_leaf_page_cache *cache = durable_index_leaf_page_caches.entries + i;
        if (cache->filename != NULL && strcmp(cache->filename, filename) == 0 &&
            cache->catalog_root_page == header->catalog_root_page &&
            cache->catalog_generation == header->catalog_generation &&
            cache->page_count == header->page_count) {
            return cache;
        }
    }
    return NULL;
}

static mylite_storage_result append_durable_index_leaf_page_cache(
    const char *filename,
    const mylite_storage_header *header,
    mylite_storage_index_leaf_page_cache **out_cache
) {
    if (durable_index_leaf_page_caches.count == durable_index_leaf_page_caches.capacity) {
        const size_t next_capacity = durable_index_leaf_page_caches.capacity == 0U
                                         ? 4U
                                         : durable_index_leaf_page_caches.capacity * 2U;
        if (next_capacity <= durable_index_leaf_page_caches.capacity ||
            next_capacity > SIZE_MAX / sizeof(*durable_index_leaf_page_caches.entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_index_leaf_page_cache *entries =
            (mylite_storage_index_leaf_page_cache *)realloc(
                durable_index_leaf_page_caches.entries,
                next_capacity * sizeof(*durable_index_leaf_page_caches.entries)
            );
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        durable_index_leaf_page_caches.entries = entries;
        durable_index_leaf_page_caches.capacity = next_capacity;
    }

    mylite_storage_index_leaf_page_cache *cache =
        durable_index_leaf_page_caches.entries + durable_index_leaf_page_caches.count;
    *cache = (mylite_storage_index_leaf_page_cache){
        .catalog_root_page = header->catalog_root_page,
        .catalog_generation = header->catalog_generation,
        .page_count = header->page_count,
    };
    cache->filename = copy_filename(filename);
    if (cache->filename == NULL) {
        *cache = (mylite_storage_index_leaf_page_cache){0};
        return MYLITE_STORAGE_NOMEM;
    }
    ++durable_index_leaf_page_caches.count;
    *out_cache = cache;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_index_leaf_page_cache_entry(
    mylite_storage_index_leaf_page_cache *cache,
    unsigned long long page_id,
    const unsigned char *page,
    const mylite_storage_index_leaf_page *leaf_page
) {
    if (find_index_leaf_page_cache_entry(cache, page_id) != NULL) {
        return MYLITE_STORAGE_OK;
    }
    if (cache->count == cache->capacity) {
        const size_t next_capacity = cache->capacity == 0U ? 8U : cache->capacity * 2U;
        if (next_capacity <= cache->capacity ||
            next_capacity > SIZE_MAX / sizeof(*cache->entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_index_leaf_page_cache_entry *entries =
            (mylite_storage_index_leaf_page_cache_entry *)
                realloc(cache->entries, next_capacity * sizeof(*cache->entries));
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        cache->entries = entries;
        cache->capacity = next_capacity;
    }

    unsigned char *page_copy = (unsigned char *)malloc(MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    if (page_copy == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    memcpy(page_copy, page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    cache->entries[cache->count++] = (mylite_storage_index_leaf_page_cache_entry){
        .page_id = page_id,
        .table_id = leaf_page->table_id,
        .index_number = leaf_page->index_number,
        .key_size = leaf_page->key_size,
        .entry_count = leaf_page->entry_count,
        .used_bytes = leaf_page->used_bytes,
        .page = page_copy,
    };
    return MYLITE_STORAGE_OK;
}

static const mylite_storage_index_leaf_page_cache_entry *find_index_leaf_page_cache_entry(
    const mylite_storage_index_leaf_page_cache *cache,
    unsigned long long page_id
) {
    for (size_t i = 0U; i < cache->count; ++i) {
        if (cache->entries[i].page_id == page_id) {
            return cache->entries + i;
        }
    }
    return NULL;
}

static void free_index_leaf_page_cache(mylite_storage_index_leaf_page_cache *cache) {
    for (size_t i = 0U; i < cache->count; ++i) {
        free(cache->entries[i].page);
    }
    free(cache->entries);
    free(cache->filename);
    *cache = (mylite_storage_index_leaf_page_cache){0};
}

static void clear_durable_exact_index_caches(const char *filename) {
    clear_cached_read_file(filename);
    clear_durable_live_row_id_caches(filename);
    clear_durable_row_payload_caches(filename);
    clear_durable_index_leaf_page_caches(filename);

    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < durable_exact_index_caches.count; ++read_index) {
        mylite_storage_exact_index_cache *cache = durable_exact_index_caches.entries + read_index;
        const int clear_cache =
            filename == NULL || (cache->filename != NULL && strcmp(cache->filename, filename) == 0);
        if (clear_cache) {
            free_exact_index_cache(cache);
            continue;
        }
        if (write_index != read_index) {
            durable_exact_index_caches.entries[write_index] = *cache;
        }
        ++write_index;
    }
    durable_exact_index_caches.count = write_index;
    if (durable_exact_index_caches.count == 0U) {
        free(durable_exact_index_caches.entries);
        durable_exact_index_caches = (mylite_storage_exact_index_cache_set){0};
    }
}

static void retarget_durable_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    clear_cached_read_file(filename);
    retarget_durable_live_row_id_caches_after_table_mutation(filename, header, table_id);
    retarget_durable_row_payload_caches_after_table_mutation(filename, header, table_id);
    retarget_durable_index_leaf_page_caches_after_table_mutation(filename, header);
    retarget_durable_exact_index_caches_after_table_mutation(filename, header, table_id);
}

static void retarget_durable_exact_index_caches_after_table_mutation(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id
) {
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < durable_exact_index_caches.count; ++read_index) {
        mylite_storage_exact_index_cache *cache = durable_exact_index_caches.entries + read_index;
        const int same_file = cache->filename != NULL && strcmp(cache->filename, filename) == 0;
        if (same_file && cache->table_id == table_id) {
            free_exact_index_cache(cache);
            continue;
        }
        if (same_file) {
            cache->catalog_root_page = header->catalog_root_page;
            cache->catalog_generation = header->catalog_generation;
            cache->page_count = header->page_count;
        }
        if (write_index != read_index) {
            durable_exact_index_caches.entries[write_index] = *cache;
        }
        ++write_index;
    }
    durable_exact_index_caches.count = write_index;
    if (durable_exact_index_caches.count == 0U) {
        free(durable_exact_index_caches.entries);
        durable_exact_index_caches = (mylite_storage_exact_index_cache_set){0};
    }
}

static mylite_storage_exact_index_cache *find_durable_exact_index_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size
) {
    for (size_t i = 0U; i < durable_exact_index_caches.count; ++i) {
        mylite_storage_exact_index_cache *cache = durable_exact_index_caches.entries + i;
        if (cache->filename != NULL && strcmp(cache->filename, filename) == 0 &&
            cache->catalog_root_page == header->catalog_root_page &&
            cache->catalog_generation == header->catalog_generation &&
            cache->page_count == header->page_count && cache->table_id == table_id &&
            cache->index_number == index_number && cache->key_size == key_size) {
            return cache;
        }
    }
    return NULL;
}

static mylite_storage_result append_durable_exact_index_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size,
    mylite_storage_exact_index_cache **out_cache
) {
    mylite_storage_result result = append_exact_index_cache(
        &durable_exact_index_caches,
        table_id,
        index_number,
        key_size,
        out_cache
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    (*out_cache)->filename = copy_filename(filename);
    if ((*out_cache)->filename == NULL) {
        free_exact_index_cache(*out_cache);
        --durable_exact_index_caches.count;
        return MYLITE_STORAGE_NOMEM;
    }
    (*out_cache)->catalog_root_page = header->catalog_root_page;
    (*out_cache)->catalog_generation = header->catalog_generation;
    (*out_cache)->page_count = header->page_count;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_exact_index_cache *ensure_durable_exact_index_cache(
    const char *filename,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size
) {
    mylite_storage_exact_index_cache *cache =
        find_durable_exact_index_cache(filename, header, table_id, index_number, key_size);
    if (cache != NULL) {
        return cache;
    }

    if (durable_exact_index_caches.count >= MYLITE_STORAGE_DURABLE_EXACT_INDEX_CACHE_LIMIT) {
        clear_durable_exact_index_caches(NULL);
    }
    if (append_durable_exact_index_cache(
            filename,
            header,
            table_id,
            index_number,
            key_size,
            &cache
        ) != MYLITE_STORAGE_OK) {
        return NULL;
    }
    return cache;
}

static mylite_storage_result copy_exact_index_cache_entries(
    mylite_storage_exact_index_cache *destination,
    const mylite_storage_exact_index_cache *source
) {
    if (destination->key_size != source->key_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const size_t live_count = source->entry_live == NULL ? source->count : source->live_count;
    unsigned char *keys = NULL;
    unsigned long long *row_ids = NULL;
    unsigned char *entry_live = NULL;
    if (live_count != 0U) {
        if (source->key_size == 0U || live_count > SIZE_MAX / source->key_size ||
            live_count > SIZE_MAX / sizeof(*source->row_ids)) {
            return MYLITE_STORAGE_FULL;
        }
        keys = (unsigned char *)malloc(live_count * source->key_size);
        if (keys == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        row_ids = (unsigned long long *)malloc(live_count * sizeof(*row_ids));
        if (row_ids == NULL) {
            free(keys);
            return MYLITE_STORAGE_NOMEM;
        }
        entry_live = (unsigned char *)malloc(live_count * sizeof(*entry_live));
        if (entry_live == NULL) {
            free(row_ids);
            free(keys);
            return MYLITE_STORAGE_NOMEM;
        }

        size_t write_index = 0U;
        for (size_t read_index = 0U; read_index < source->count; ++read_index) {
            if (source->entry_live != NULL && !source->entry_live[read_index]) {
                continue;
            }
            memcpy(
                keys + (write_index * source->key_size),
                source->keys + (read_index * source->key_size),
                source->key_size
            );
            row_ids[write_index] = source->row_ids[read_index];
            entry_live[write_index] = 1U;
            ++write_index;
        }
    }

    free(destination->keys);
    free(destination->row_ids);
    free(destination->entry_live);
    clear_exact_index_cache_buckets(destination);
    clear_exact_index_cache_row_id_buckets(destination);
    destination->keys = keys;
    destination->row_ids = row_ids;
    destination->entry_live = entry_live;
    destination->count = live_count;
    destination->capacity = live_count;
    destination->live_count = live_count;
    destination->dead_count = 0U;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_exact_index_cache *find_exact_index_cache(
    mylite_storage_exact_index_cache_set *caches,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size
) {
    for (size_t i = 0U; i < caches->count; ++i) {
        mylite_storage_exact_index_cache *cache = caches->entries + i;
        if (cache->table_id == table_id && cache->index_number == index_number &&
            cache->key_size == key_size) {
            return cache;
        }
    }
    return NULL;
}

static mylite_storage_result append_exact_index_cache(
    mylite_storage_exact_index_cache_set *caches,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size,
    mylite_storage_exact_index_cache **out_cache
) {
    if (caches->count == caches->capacity) {
        const size_t next_capacity = caches->capacity == 0U ? 4U : caches->capacity * 2U;
        if (next_capacity <= caches->capacity ||
            next_capacity > SIZE_MAX / sizeof(*caches->entries)) {
            return MYLITE_STORAGE_FULL;
        }
        mylite_storage_exact_index_cache *entries = (mylite_storage_exact_index_cache *)
            realloc(caches->entries, next_capacity * sizeof(*caches->entries));
        if (entries == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        caches->entries = entries;
        caches->capacity = next_capacity;
    }

    mylite_storage_exact_index_cache *cache = caches->entries + caches->count;
    *cache = (mylite_storage_exact_index_cache){
        .table_id = table_id,
        .index_number = index_number,
        .key_size = key_size,
    };
    ++caches->count;
    *out_cache = cache;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result load_exact_index_cache(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_exact_index_cache *cache
) {
    mylite_storage_index_entryset entryset = {
        .size = sizeof(entryset),
    };
    mylite_storage_result result =
        read_live_index_entries(file, header, cache->table_id, cache->index_number, &entryset);
    for (size_t i = 0U; result == MYLITE_STORAGE_OK && i < entryset.entry_count; ++i) {
        if (entryset.key_sizes[i] != cache->key_size) {
            continue;
        }
        result = append_exact_index_cache_entry(
            cache,
            entryset.keys + entryset.key_offsets[i],
            entryset.row_ids[i]
        );
    }
    mylite_storage_free_index_entryset(&entryset);
    return result;
}

static mylite_storage_result append_exact_index_cache_entry(
    mylite_storage_exact_index_cache *cache,
    const unsigned char *key,
    unsigned long long row_id
) {
    if (cache->key_size == 0U || cache->count == SIZE_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t entry_index = cache->count;
    mylite_storage_result result = grow_exact_index_cache_entry_capacity(cache, entry_index + 1U);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    if (cache->buckets_valid) {
        const size_t needed_bucket_count = exact_index_cache_bucket_count(cache->live_count + 1U);
        if (needed_bucket_count == 0U) {
            return MYLITE_STORAGE_FULL;
        }
        if (needed_bucket_count > cache->bucket_count) {
            clear_exact_index_cache_buckets(cache);
        }
    }
    if (cache->row_id_buckets_valid) {
        const size_t needed_bucket_count = exact_index_cache_bucket_count(cache->live_count + 1U);
        if (needed_bucket_count == 0U) {
            return MYLITE_STORAGE_FULL;
        }
        if (needed_bucket_count > cache->row_id_bucket_count) {
            clear_exact_index_cache_row_id_buckets(cache);
        }
    }

    memcpy(cache->keys + (entry_index * cache->key_size), key, cache->key_size);
    cache->row_ids[entry_index] = row_id;
    cache->entry_live[entry_index] = 1U;
    ++cache->count;
    ++cache->live_count;
    if (cache->buckets_valid) {
        link_exact_index_cache_bucket_entry(cache, entry_index);
    }
    if (cache->row_id_buckets_valid) {
        link_exact_index_cache_row_id_bucket_entry(cache, entry_index);
    }
    return MYLITE_STORAGE_OK;
}

static void remove_exact_index_cache_entries_by_row_id(
    mylite_storage_exact_index_cache *cache,
    unsigned long long row_id
) {
    if (cache->live_count == 0U) {
        return;
    }

    if (cache->row_id_buckets_valid ||
        ensure_exact_index_cache_row_id_buckets(cache) == MYLITE_STORAGE_OK) {
        const size_t bucket_index = exact_index_cache_bucket_for_row_id(cache, row_id);
        size_t *current = cache->row_id_bucket_heads + bucket_index;
        while (*current != MYLITE_STORAGE_CACHE_BUCKET_EMPTY) {
            const size_t entry_index = *current;
            if (entry_index >= cache->count) {
                clear_exact_index_cache_row_id_buckets(cache);
                break;
            }

            size_t *next = cache->row_id_bucket_next + entry_index;
            if ((cache->entry_live != NULL && !cache->entry_live[entry_index]) ||
                cache->row_ids[entry_index] == row_id) {
                *current = *next;
                *next = MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
                if (cache->entry_live != NULL && cache->entry_live[entry_index]) {
                    if (cache->buckets_valid) {
                        unlink_exact_index_cache_bucket_entry(cache, entry_index);
                    }
                    cache->entry_live[entry_index] = 0U;
                    --cache->live_count;
                    ++cache->dead_count;
                }
                continue;
            }
            current = next;
        }
        maybe_compact_exact_index_cache_entries(cache);
        return;
    }

    for (size_t entry_index = 0U; entry_index < cache->count; ++entry_index) {
        if (cache->entry_live != NULL && !cache->entry_live[entry_index]) {
            continue;
        }
        if (cache->row_ids[entry_index] != row_id) {
            continue;
        }
        if (cache->buckets_valid) {
            unlink_exact_index_cache_bucket_entry(cache, entry_index);
        }
        if (cache->row_id_buckets_valid) {
            unlink_exact_index_cache_row_id_bucket_entry(cache, entry_index);
        }
        cache->entry_live[entry_index] = 0U;
        --cache->live_count;
        ++cache->dead_count;
    }
    maybe_compact_exact_index_cache_entries(cache);
}

static mylite_storage_result grow_exact_index_cache_entry_capacity(
    mylite_storage_exact_index_cache *cache,
    size_t minimum_capacity
) {
    if (cache->capacity >= minimum_capacity) {
        return MYLITE_STORAGE_OK;
    }

    size_t next_capacity = cache->capacity == 0U ? 16U : cache->capacity;
    while (next_capacity < minimum_capacity) {
        if (next_capacity > SIZE_MAX / 2U) {
            return MYLITE_STORAGE_FULL;
        }
        next_capacity *= 2U;
    }
    if (next_capacity > SIZE_MAX / sizeof(*cache->row_ids) ||
        next_capacity > SIZE_MAX / sizeof(*cache->entry_live) ||
        next_capacity > SIZE_MAX / cache->key_size) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned char *keys = (unsigned char *)realloc(cache->keys, next_capacity * cache->key_size);
    if (keys == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    cache->keys = keys;

    unsigned long long *row_ids =
        (unsigned long long *)realloc(cache->row_ids, next_capacity * sizeof(*cache->row_ids));
    if (row_ids == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    cache->row_ids = row_ids;

    unsigned char *entry_live =
        (unsigned char *)realloc(cache->entry_live, next_capacity * sizeof(*cache->entry_live));
    if (entry_live == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    cache->entry_live = entry_live;

    if (cache->buckets_valid) {
        size_t *bucket_next =
            (size_t *)realloc(cache->bucket_next, next_capacity * sizeof(*cache->bucket_next));
        if (bucket_next == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        cache->bucket_next = bucket_next;
        cache->bucket_next_capacity = next_capacity;
    }
    if (cache->row_id_buckets_valid) {
        size_t *row_id_bucket_next = (size_t *)
            realloc(cache->row_id_bucket_next, next_capacity * sizeof(*cache->row_id_bucket_next));
        if (row_id_bucket_next == NULL) {
            return MYLITE_STORAGE_NOMEM;
        }
        cache->row_id_bucket_next = row_id_bucket_next;
        cache->row_id_bucket_next_capacity = next_capacity;
    }

    cache->capacity = next_capacity;
    return MYLITE_STORAGE_OK;
}

static void maybe_compact_exact_index_cache_entries(mylite_storage_exact_index_cache *cache) {
    if (cache->dead_count > cache->live_count) {
        compact_exact_index_cache_entries(cache);
    }
}

static void compact_exact_index_cache_entries(mylite_storage_exact_index_cache *cache) {
    size_t write_index = 0U;
    const int rebuild_buckets = cache->buckets_valid;
    const int rebuild_row_id_buckets = cache->row_id_buckets_valid;
    for (size_t read_index = 0U; read_index < cache->count; ++read_index) {
        if (cache->entry_live != NULL && !cache->entry_live[read_index]) {
            continue;
        }
        if (write_index != read_index) {
            memmove(
                cache->keys + (write_index * cache->key_size),
                cache->keys + (read_index * cache->key_size),
                cache->key_size
            );
            cache->row_ids[write_index] = cache->row_ids[read_index];
        }
        cache->entry_live[write_index] = 1U;
        ++write_index;
    }
    cache->count = write_index;
    cache->live_count = write_index;
    cache->dead_count = 0U;
    if (rebuild_buckets) {
        clear_exact_index_cache_buckets(cache);
        if (ensure_exact_index_cache_buckets(cache) != MYLITE_STORAGE_OK) {
            clear_exact_index_cache_buckets(cache);
        }
    }
    if (rebuild_row_id_buckets) {
        clear_exact_index_cache_row_id_buckets(cache);
        if (ensure_exact_index_cache_row_id_buckets(cache) != MYLITE_STORAGE_OK) {
            clear_exact_index_cache_row_id_buckets(cache);
        }
    }
}

static void unlink_exact_index_cache_bucket_entry(
    mylite_storage_exact_index_cache *cache,
    size_t entry_index
) {
    if (cache->bucket_count == 0U || entry_index >= cache->count) {
        return;
    }

    const unsigned char *key = cache->keys + (entry_index * cache->key_size);
    const size_t bucket_index = exact_index_cache_bucket_for_key(cache, key);
    size_t *current = cache->bucket_heads + bucket_index;
    while (*current != MYLITE_STORAGE_CACHE_BUCKET_EMPTY) {
        if (*current >= cache->count) {
            return;
        }
        if (*current == entry_index) {
            *current = cache->bucket_next[entry_index];
            cache->bucket_next[entry_index] = MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
            return;
        }
        current = cache->bucket_next + *current;
    }
}

static void link_exact_index_cache_bucket_entry(
    mylite_storage_exact_index_cache *cache,
    size_t entry_index
) {
    if (cache->bucket_count == 0U || entry_index >= cache->count) {
        return;
    }

    const unsigned char *key = cache->keys + (entry_index * cache->key_size);
    const size_t bucket_index = exact_index_cache_bucket_for_key(cache, key);
    size_t *current = cache->bucket_heads + bucket_index;
    while (*current != MYLITE_STORAGE_CACHE_BUCKET_EMPTY) {
        if (*current >= cache->count) {
            return;
        }
        current = cache->bucket_next + *current;
    }
    *current = entry_index;
    cache->bucket_next[entry_index] = MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
}

static mylite_storage_result ensure_exact_index_cache_buckets(
    mylite_storage_exact_index_cache *cache
) {
    if (cache->buckets_valid) {
        return MYLITE_STORAGE_OK;
    }
    clear_exact_index_cache_buckets(cache);
    if (cache->live_count == 0U) {
        cache->buckets_valid = 1;
        return MYLITE_STORAGE_OK;
    }

    const size_t bucket_count = exact_index_cache_bucket_count(cache->live_count);
    if (bucket_count == 0U || bucket_count > SIZE_MAX / sizeof(*cache->bucket_heads) ||
        cache->capacity > SIZE_MAX / sizeof(*cache->bucket_next)) {
        return MYLITE_STORAGE_FULL;
    }

    size_t *bucket_heads = (size_t *)malloc(bucket_count * sizeof(*bucket_heads));
    if (bucket_heads == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    const size_t bucket_next_capacity = cache->capacity == 0U ? cache->count : cache->capacity;
    size_t *bucket_next = (size_t *)malloc(bucket_next_capacity * sizeof(*bucket_next));
    if (bucket_next == NULL) {
        free(bucket_heads);
        return MYLITE_STORAGE_NOMEM;
    }

    for (size_t i = 0U; i < bucket_count; ++i) {
        bucket_heads[i] = MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
    }
    for (size_t i = 0U; i < bucket_next_capacity; ++i) {
        bucket_next[i] = MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
    }
    for (size_t i = cache->count; i > 0U; --i) {
        const size_t entry_index = i - 1U;
        if (cache->entry_live != NULL && !cache->entry_live[entry_index]) {
            continue;
        }
        const unsigned char *entry_key = cache->keys + (entry_index * cache->key_size);
        const size_t bucket_index =
            hash_key_bytes(entry_key, cache->key_size) & (bucket_count - 1U);
        bucket_next[entry_index] = bucket_heads[bucket_index];
        bucket_heads[bucket_index] = entry_index;
    }

    cache->bucket_heads = bucket_heads;
    cache->bucket_next = bucket_next;
    cache->bucket_count = bucket_count;
    cache->bucket_next_capacity = bucket_next_capacity;
    cache->buckets_valid = 1;
    return MYLITE_STORAGE_OK;
}

static size_t exact_index_cache_bucket_for_key(
    const mylite_storage_exact_index_cache *cache,
    const unsigned char *key
) {
    return hash_key_bytes(key, cache->key_size) & (cache->bucket_count - 1U);
}

static size_t exact_index_cache_bucket_count(size_t entry_count) {
    if (entry_count > SIZE_MAX / 2U) {
        return 0U;
    }

    const size_t target_count = entry_count * 2U;
    size_t bucket_count = 16U;
    while (bucket_count < target_count) {
        if (bucket_count > SIZE_MAX / 2U) {
            return 0U;
        }
        bucket_count *= 2U;
    }
    return bucket_count;
}

static size_t hash_key_bytes(const unsigned char *key, size_t key_size) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0U; i < key_size; ++i) {
        hash ^= key[i];
        hash *= 1099511628211ULL;
    }
    return (size_t)hash;
}

static void clear_exact_index_cache_buckets(mylite_storage_exact_index_cache *cache) {
    free(cache->bucket_heads);
    free(cache->bucket_next);
    cache->bucket_heads = NULL;
    cache->bucket_next = NULL;
    cache->bucket_count = 0U;
    cache->bucket_next_capacity = 0U;
    cache->buckets_valid = 0;
}

static void unlink_exact_index_cache_row_id_bucket_entry(
    mylite_storage_exact_index_cache *cache,
    size_t entry_index
) {
    if (cache->row_id_bucket_count == 0U || entry_index >= cache->count) {
        return;
    }

    const size_t bucket_index =
        exact_index_cache_bucket_for_row_id(cache, cache->row_ids[entry_index]);
    size_t *current = cache->row_id_bucket_heads + bucket_index;
    while (*current != MYLITE_STORAGE_CACHE_BUCKET_EMPTY) {
        if (*current >= cache->count) {
            return;
        }
        if (*current == entry_index) {
            *current = cache->row_id_bucket_next[entry_index];
            cache->row_id_bucket_next[entry_index] = MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
            return;
        }
        current = cache->row_id_bucket_next + *current;
    }
}

static void link_exact_index_cache_row_id_bucket_entry(
    mylite_storage_exact_index_cache *cache,
    size_t entry_index
) {
    if (cache->row_id_bucket_count == 0U || entry_index >= cache->count) {
        return;
    }

    const size_t bucket_index =
        exact_index_cache_bucket_for_row_id(cache, cache->row_ids[entry_index]);
    size_t *current = cache->row_id_bucket_heads + bucket_index;
    while (*current != MYLITE_STORAGE_CACHE_BUCKET_EMPTY) {
        if (*current >= cache->count) {
            return;
        }
        current = cache->row_id_bucket_next + *current;
    }
    *current = entry_index;
    cache->row_id_bucket_next[entry_index] = MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
}

static mylite_storage_result ensure_exact_index_cache_row_id_buckets(
    mylite_storage_exact_index_cache *cache
) {
    if (cache->row_id_buckets_valid) {
        return MYLITE_STORAGE_OK;
    }
    clear_exact_index_cache_row_id_buckets(cache);
    if (cache->live_count == 0U) {
        cache->row_id_buckets_valid = 1;
        return MYLITE_STORAGE_OK;
    }

    const size_t bucket_count = exact_index_cache_bucket_count(cache->live_count);
    if (bucket_count == 0U || bucket_count > SIZE_MAX / sizeof(*cache->row_id_bucket_heads) ||
        cache->capacity > SIZE_MAX / sizeof(*cache->row_id_bucket_next)) {
        return MYLITE_STORAGE_FULL;
    }

    size_t *bucket_heads = (size_t *)malloc(bucket_count * sizeof(*bucket_heads));
    if (bucket_heads == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    const size_t bucket_next_capacity = cache->capacity == 0U ? cache->count : cache->capacity;
    size_t *bucket_next = (size_t *)malloc(bucket_next_capacity * sizeof(*bucket_next));
    if (bucket_next == NULL) {
        free(bucket_heads);
        return MYLITE_STORAGE_NOMEM;
    }

    for (size_t i = 0U; i < bucket_count; ++i) {
        bucket_heads[i] = MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
    }
    for (size_t i = 0U; i < bucket_next_capacity; ++i) {
        bucket_next[i] = MYLITE_STORAGE_CACHE_BUCKET_EMPTY;
    }
    for (size_t i = cache->count; i > 0U; --i) {
        const size_t entry_index = i - 1U;
        if (cache->entry_live != NULL && !cache->entry_live[entry_index]) {
            continue;
        }
        const size_t bucket_index = hash_row_id(cache->row_ids[entry_index]) & (bucket_count - 1U);
        bucket_next[entry_index] = bucket_heads[bucket_index];
        bucket_heads[bucket_index] = entry_index;
    }

    cache->row_id_bucket_heads = bucket_heads;
    cache->row_id_bucket_next = bucket_next;
    cache->row_id_bucket_count = bucket_count;
    cache->row_id_bucket_next_capacity = bucket_next_capacity;
    cache->row_id_buckets_valid = 1;
    return MYLITE_STORAGE_OK;
}

static size_t exact_index_cache_bucket_for_row_id(
    const mylite_storage_exact_index_cache *cache,
    unsigned long long row_id
) {
    return hash_row_id(row_id) & (cache->row_id_bucket_count - 1U);
}

static void clear_exact_index_cache_row_id_buckets(mylite_storage_exact_index_cache *cache) {
    free(cache->row_id_bucket_heads);
    free(cache->row_id_bucket_next);
    cache->row_id_bucket_heads = NULL;
    cache->row_id_bucket_next = NULL;
    cache->row_id_bucket_count = 0U;
    cache->row_id_bucket_next_capacity = 0U;
    cache->row_id_buckets_valid = 0;
}

static mylite_storage_result append_index_entry_to_entryset(
    mylite_storage_index_entryset *entryset,
    const mylite_storage_index_entry_page *entry_page
) {
    size_t entry_index = 0U;
    size_t key_offset = 0U;
    mylite_storage_result result = grow_index_entryset_for_append(
        entryset,
        1U,
        entry_page->key_size,
        &entry_index,
        &key_offset
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    memcpy(entryset->keys + key_offset, entry_page->key, entry_page->key_size);
    entryset->key_offsets[entry_index] = key_offset;
    entryset->key_sizes[entry_index] = entry_page->key_size;
    entryset->row_ids[entry_index] = entry_page->row_id;
    entryset->entry_count = entry_index + 1U;
    entryset->key_bytes = key_offset + entry_page->key_size;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result grow_index_entryset_for_append(
    mylite_storage_index_entryset *entryset,
    size_t additional_entry_count,
    size_t additional_key_bytes,
    size_t *out_first_entry,
    size_t *out_first_key_offset
) {
    if (entryset->entry_count > SIZE_MAX - additional_entry_count ||
        entryset->key_bytes > SIZE_MAX - additional_key_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_entry_count = entryset->entry_count + additional_entry_count;
    const size_t new_key_bytes = entryset->key_bytes + additional_key_bytes;
    if (new_entry_count > SIZE_MAX / sizeof(size_t) ||
        new_entry_count > SIZE_MAX / sizeof(unsigned long long)) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned char *keys = (unsigned char *)realloc(entryset->keys, new_key_bytes);
    if (keys == NULL && new_key_bytes != 0U) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->keys = keys;

    size_t *key_offsets =
        (size_t *)realloc(entryset->key_offsets, new_entry_count * sizeof(size_t));
    if (key_offsets == NULL && new_entry_count != 0U) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->key_offsets = key_offsets;

    size_t *key_sizes = (size_t *)realloc(entryset->key_sizes, new_entry_count * sizeof(size_t));
    if (key_sizes == NULL && new_entry_count != 0U) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->key_sizes = key_sizes;

    unsigned long long *row_ids = (unsigned long long *)
        realloc(entryset->row_ids, new_entry_count * sizeof(unsigned long long));
    if (row_ids == NULL && new_entry_count != 0U) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->row_ids = row_ids;

    *out_first_entry = entryset->entry_count;
    *out_first_key_offset = entryset->key_bytes;
    return MYLITE_STORAGE_OK;
}

static void remove_index_entries_by_row_id(
    mylite_storage_index_entryset *entryset,
    unsigned long long row_id
) {
    size_t write_index = 0U;
    size_t key_bytes = 0U;
    for (size_t read_index = 0U; read_index < entryset->entry_count; ++read_index) {
        const size_t key_offset = entryset->key_offsets[read_index];
        const size_t key_size = entryset->key_sizes[read_index];
        if (entryset->row_ids[read_index] == row_id) {
            continue;
        }

        if (key_offset != key_bytes) {
            memmove(entryset->keys + key_bytes, entryset->keys + key_offset, key_size);
        }
        entryset->key_offsets[write_index] = key_bytes;
        entryset->key_sizes[write_index] = key_size;
        entryset->row_ids[write_index] = entryset->row_ids[read_index];
        key_bytes += key_size;
        ++write_index;
    }

    entryset->entry_count = write_index;
    entryset->key_bytes = key_bytes;
}

static void replace_index_entries_row_id(
    mylite_storage_index_entryset *entryset,
    unsigned long long old_row_id,
    unsigned long long new_row_id
) {
    for (size_t i = 0U; i < entryset->entry_count; ++i) {
        if (entryset->row_ids[i] == old_row_id) {
            entryset->row_ids[i] = new_row_id;
        }
    }
}

static void encode_autoincrement_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned long long next_value
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_AUTOINCREMENT_MAGIC_OFFSET,
        k_autoincrement_magic,
        sizeof(k_autoincrement_magic)
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_TYPE_TABLE_STATE
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_TABLE_ID_OFFSET, table_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_NEXT_VALUE_OFFSET, next_value);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET,
        checksum_page_zero_tail(
            page,
            MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET,
            MYLITE_STORAGE_FORMAT_AUTOINCREMENT_NEXT_VALUE_OFFSET + sizeof(uint64_t)
        )
    );
}

static mylite_storage_result read_autoincrement_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_autoincrement_page *out_autoincrement_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (!is_autoincrement_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_row_page(page) || is_row_state_page(page) || is_index_entry_page(page) ||
            is_index_leaf_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_TYPE_OFFSET);
    const unsigned page_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_ID_OFFSET);
    const unsigned long long table_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_TABLE_ID_OFFSET);
    const unsigned long long next_value =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_NEXT_VALUE_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET);
    if (page_type != MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_TYPE_TABLE_STATE ||
        page_version != 1U || format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || next_value == 0ULL || expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_autoincrement_page = (mylite_storage_autoincrement_page){
        .table_id = table_id,
        .next_value = next_value,
    };
    return MYLITE_STORAGE_OK;
}

static int is_autoincrement_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_AUTOINCREMENT_MAGIC_OFFSET,
               k_autoincrement_magic,
               sizeof(k_autoincrement_magic)
           ) == 0;
}

static mylite_storage_result latest_auto_increment_value(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long *out_next_value
) {
    *out_next_value = 1ULL;
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_autoincrement_page autoincrement_page = {0};
        mylite_storage_result result =
            read_autoincrement_page(file, header, page_id, page, &autoincrement_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (autoincrement_page.table_id == table_id) {
            *out_next_value = autoincrement_page.next_value;
        }
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result publish_auto_increment_value(
    FILE *file,
    const char *filename,
    mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long next_value
) {
    if (header->page_count == ULLONG_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    mylite_storage_result result = begin_write_journal(file, filename, header, 0);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    const unsigned long long page_id = header->page_count;
    unsigned char autoincrement_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    encode_autoincrement_page(autoincrement_page, page_id, table_id, next_value);

    result = write_page_at(file, page_id, header->page_size, autoincrement_page);
    if (result == MYLITE_STORAGE_OK) {
        result = publish_header_page_count(file, header, page_id + 1ULL);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = finish_write_journal(file, filename);
    }
    return result;
}

static mylite_storage_result read_definition_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_catalog_entry *entry,
    unsigned char **out_definition,
    size_t *out_definition_size
) {
    if (entry->definition_size > SIZE_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    unsigned char *definition = (unsigned char *)malloc((size_t)entry->definition_size);
    if (definition == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    size_t written = 0U;
    unsigned long long page_id = entry->definition_root_page;
    while (written < (size_t)entry->definition_size) {
        unsigned long long next_page_id = 0ULL;
        const mylite_storage_result result = read_blob_page(
            file,
            header,
            page_id,
            entry->definition_size - written,
            definition,
            &written,
            &next_page_id,
            MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_TABLE_DEFINITION
        );
        if (result != MYLITE_STORAGE_OK) {
            free(definition);
            return result;
        }
        if (next_page_id == 0ULL && written < (size_t)entry->definition_size) {
            free(definition);
            return MYLITE_STORAGE_CORRUPT;
        }
        page_id = next_page_id;
    }

    *out_definition = definition;
    *out_definition_size = (size_t)entry->definition_size;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_foreign_key_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_catalog_entry *entry,
    unsigned char **out_metadata,
    size_t *out_metadata_size
) {
    if (entry->definition_size > SIZE_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    unsigned char *metadata = (unsigned char *)malloc((size_t)entry->definition_size);
    if (metadata == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    size_t written = 0U;
    unsigned long long page_id = entry->definition_root_page;
    while (written < (size_t)entry->definition_size) {
        unsigned long long next_page_id = 0ULL;
        const mylite_storage_result result = read_blob_page(
            file,
            header,
            page_id,
            entry->definition_size - written,
            metadata,
            &written,
            &next_page_id,
            MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_FOREIGN_KEY
        );
        if (result != MYLITE_STORAGE_OK) {
            free(metadata);
            return result;
        }
        if (next_page_id == 0ULL && written < (size_t)entry->definition_size) {
            free(metadata);
            return MYLITE_STORAGE_CORRUPT;
        }
        page_id = next_page_id;
    }

    *out_metadata = metadata;
    *out_metadata_size = (size_t)entry->definition_size;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_row_payload_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_blob_chain chain,
    unsigned char **out_payload
) {
    unsigned char *payload = (unsigned char *)malloc(chain.payload_size);
    if (payload == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    size_t written = 0U;
    unsigned long long page_id = chain.first_page_id;
    while (written < chain.payload_size) {
        unsigned long long next_page_id = 0ULL;
        const mylite_storage_result result = read_blob_page(
            file,
            header,
            page_id,
            chain.payload_size - written,
            payload,
            &written,
            &next_page_id,
            chain.page_type
        );
        if (result != MYLITE_STORAGE_OK) {
            free(payload);
            return result;
        }
        if (next_page_id == 0ULL && written < chain.payload_size) {
            free(payload);
            return MYLITE_STORAGE_CORRUPT;
        }
        page_id = next_page_id;
    }

    *out_payload = payload;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_blob_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned long long expected_remaining,
    unsigned char *out_payload,
    size_t *inout_written,
    unsigned long long *out_next_page_id,
    unsigned expected_page_type
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (memcmp(
            page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
            k_blob_magic,
            sizeof(k_blob_magic)
        ) != 0) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_OFFSET);
    const unsigned page_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_ID_OFFSET);
    const unsigned payload_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_SIZE_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET);
    if (page_type != expected_page_type || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        expected_checksum != actual_checksum ||
        payload_size >
            MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET ||
        payload_size == 0U || payload_size > expected_remaining) {
        return MYLITE_STORAGE_CORRUPT;
    }

    memcpy(
        out_payload + *inout_written,
        page + MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET,
        payload_size
    );
    *inout_written += payload_size;
    *out_next_page_id = get_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_NEXT_PAGE_OFFSET);
    if (*out_next_page_id != 0ULL && *out_next_page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }
    return MYLITE_STORAGE_OK;
}

static char *copy_record_field(const unsigned char *record, unsigned field_index) {
    const size_t field_size = record_field_size(record, field_index);
    char *copy = (char *)malloc(field_size + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, record + record_field_offset(record, field_index), field_size);
    copy[field_size] = '\0';
    return copy;
}

static mylite_storage_result decode_foreign_key_metadata(
    const unsigned char *record,
    const unsigned char *metadata,
    size_t metadata_size,
    mylite_storage_foreign_key_metadata *out_metadata
) {
    if (metadata_size < MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_HEADER_SIZE) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned version =
        get_u32_le(metadata, MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_VERSION_OFFSET);
    const size_t column_count =
        get_u32_le(metadata, MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_COLUMN_COUNT_OFFSET);
    const size_t referenced_key_name_size = get_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_REFERENCED_KEY_LENGTH_OFFSET
    );
    const size_t foreign_column_names_size =
        get_u32_le(metadata, MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_FOREIGN_COLUMNS_SIZE_OFFSET);
    const size_t referenced_column_names_size = get_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_REFERENCED_COLUMNS_SIZE_OFFSET
    );
    size_t expected_size = MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_HEADER_SIZE;
    if (version != MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_VERSION || column_count == 0U ||
        column_count > sizeof(unsigned long long) * CHAR_BIT ||
        referenced_key_name_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += referenced_key_name_size;
    if (foreign_column_names_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += foreign_column_names_size;
    if (referenced_column_names_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += referenced_column_names_size;
    if (metadata_size != expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = copy_foreign_key_metadata_identity(record, out_metadata);
    if (result == MYLITE_STORAGE_OK) {
        out_metadata->column_count = column_count;
        out_metadata->update_action =
            get_u32_le(metadata, MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_UPDATE_ACTION_OFFSET);
        out_metadata->delete_action =
            get_u32_le(metadata, MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_DELETE_ACTION_OFFSET);
        out_metadata->match_option =
            get_u32_le(metadata, MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_MATCH_OPTION_OFFSET);
        out_metadata->nullable_column_bitmap =
            get_u64_le(metadata, MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_NULLABLE_BITMAP_OFFSET);
        out_metadata->referenced_nullable_column_bitmap = get_u64_le(
            metadata,
            MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_REFERENCED_NULLABLE_BITMAP_OFFSET
        );
        if (validate_foreign_key_action(out_metadata->update_action) != MYLITE_STORAGE_OK ||
            validate_foreign_key_action(out_metadata->delete_action) != MYLITE_STORAGE_OK ||
            validate_foreign_key_match_option(out_metadata->match_option) != MYLITE_STORAGE_OK) {
            result = MYLITE_STORAGE_CORRUPT;
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = copy_foreign_key_payload_strings(metadata, metadata_size, out_metadata);
    }
    if (result != MYLITE_STORAGE_OK) {
        mylite_storage_free_foreign_key_metadata(out_metadata);
    }
    return result;
}

static mylite_storage_result copy_foreign_key_metadata_identity(
    const unsigned char *record,
    mylite_storage_foreign_key_metadata *out_metadata
) {
    out_metadata->schema_name = copy_record_field(record, 0U);
    out_metadata->table_name = copy_record_field(record, 1U);
    out_metadata->constraint_name = copy_record_field(record, 2U);
    out_metadata->referenced_schema_name = copy_record_field(record, 3U);
    out_metadata->referenced_table_name = copy_record_field(record, 4U);
    if (out_metadata->schema_name == NULL || out_metadata->table_name == NULL ||
        out_metadata->constraint_name == NULL || out_metadata->referenced_schema_name == NULL ||
        out_metadata->referenced_table_name == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result copy_foreign_key_payload_strings(
    const unsigned char *metadata,
    size_t metadata_size,
    mylite_storage_foreign_key_metadata *out_metadata
) {
    const size_t referenced_key_name_size = get_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_REFERENCED_KEY_LENGTH_OFFSET
    );
    const size_t foreign_column_names_size =
        get_u32_le(metadata, MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_FOREIGN_COLUMNS_SIZE_OFFSET);
    const size_t referenced_column_names_size = get_u32_le(
        metadata,
        MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_REFERENCED_COLUMNS_SIZE_OFFSET
    );

    size_t offset = MYLITE_STORAGE_FORMAT_FOREIGN_KEY_PAYLOAD_HEADER_SIZE;
    out_metadata->referenced_key_name = (char *)malloc(referenced_key_name_size + 1U);
    if (out_metadata->referenced_key_name == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    memcpy(out_metadata->referenced_key_name, metadata + offset, referenced_key_name_size);
    out_metadata->referenced_key_name[referenced_key_name_size] = '\0';
    offset += referenced_key_name_size;

    mylite_storage_result result = copy_serialized_column_names(
        metadata + offset,
        foreign_column_names_size,
        out_metadata->column_count,
        &out_metadata->foreign_column_names
    );
    offset += foreign_column_names_size;
    if (result == MYLITE_STORAGE_OK) {
        result = copy_serialized_column_names(
            metadata + offset,
            referenced_column_names_size,
            out_metadata->column_count,
            &out_metadata->referenced_column_names
        );
    }
    if (result == MYLITE_STORAGE_OK && offset + referenced_column_names_size != metadata_size) {
        result = MYLITE_STORAGE_CORRUPT;
    }
    return result;
}

static mylite_storage_result copy_serialized_column_names(
    const unsigned char *serialized_names,
    size_t serialized_names_size,
    size_t column_count,
    char ***out_column_names
) {
    char **column_names = (char **)calloc(column_count, sizeof(*column_names));
    if (column_names == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    size_t offset = 0U;
    for (size_t i = 0U; i < column_count; ++i) {
        size_t name_size = 0U;
        while (offset + name_size < serialized_names_size &&
               serialized_names[offset + name_size] != '\0') {
            ++name_size;
        }
        if (name_size == 0U || offset + name_size >= serialized_names_size) {
            free_column_names(column_names, column_count);
            return MYLITE_STORAGE_CORRUPT;
        }

        column_names[i] = (char *)malloc(name_size + 1U);
        if (column_names[i] == NULL) {
            free_column_names(column_names, column_count);
            return MYLITE_STORAGE_NOMEM;
        }
        memcpy(column_names[i], serialized_names + offset, name_size);
        column_names[i][name_size] = '\0';
        offset += name_size + 1U;
    }
    if (offset != serialized_names_size) {
        free_column_names(column_names, column_count);
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_column_names = column_names;
    return MYLITE_STORAGE_OK;
}

static void free_column_names(char **column_names, size_t column_count) {
    if (column_names == NULL) {
        return;
    }
    for (size_t i = 0U; i < column_count; ++i) {
        free(column_names[i]);
    }
    free(column_names);
}

static mylite_storage_result list_catalog_schemas(
    const unsigned char *catalog_page,
    mylite_storage_schema_callback callback,
    void *ctx
) {
    mylite_storage_schema_list list = {0};
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        mylite_storage_result result = collect_schema_name(&list, record);
        if (result != MYLITE_STORAGE_OK) {
            free_schema_list(&list);
            return result;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }

    for (size_t i = 0U; i < list.count; ++i) {
        if (callback(ctx, list.names[i]) != 0) {
            free_schema_list(&list);
            return MYLITE_STORAGE_ERROR;
        }
    }

    free_schema_list(&list);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result collect_schema_name(
    mylite_storage_schema_list *list,
    const unsigned char *record
) {
    const size_t schema_name_size = record_field_size(record, 0U);
    const char *schema_name = (const char *)(record + record_field_offset(record, 0U));
    if (schema_list_contains(list, schema_name, schema_name_size)) {
        return MYLITE_STORAGE_OK;
    }

    if (list->count == SIZE_MAX / sizeof(*list->names)) {
        return MYLITE_STORAGE_FULL;
    }
    char **names = (char **)realloc((void *)list->names, (list->count + 1U) * sizeof(*list->names));
    if (names == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    list->names = names;
    if (schema_name_size == SIZE_MAX) {
        return MYLITE_STORAGE_FULL;
    }
    list->names[list->count] = (char *)malloc(schema_name_size + 1U);
    if (list->names[list->count] == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    memcpy(list->names[list->count], schema_name, schema_name_size);
    list->names[list->count][schema_name_size] = '\0';
    ++list->count;
    return MYLITE_STORAGE_OK;
}

static int schema_list_contains(
    const mylite_storage_schema_list *list,
    const char *schema_name,
    size_t schema_name_size
) {
    for (size_t i = 0U; i < list->count; ++i) {
        if (strlen(list->names[i]) == schema_name_size &&
            memcmp(list->names[i], schema_name, schema_name_size) == 0) {
            return 1;
        }
    }
    return 0;
}

static void free_schema_list(mylite_storage_schema_list *list) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0U; i < list->count; ++i) {
        free(list->names[i]);
    }
    free((void *)list->names);
    *list = (mylite_storage_schema_list){0};
}

static mylite_storage_result list_catalog_tables(
    const unsigned char *catalog_page,
    const char *schema_name,
    mylite_storage_table_callback callback,
    void *ctx
) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        const char *record_schema = (const char *)(record + record_field_offset(record, 0U));
        const size_t record_schema_size = record_field_size(record, 0U);
        if (record_is_table(record) && strlen(schema_name) == record_schema_size &&
            memcmp(record_schema, schema_name, record_schema_size) == 0) {
            char *schema_copy = copy_record_field(record, 0U);
            char *table_copy = copy_record_field(record, 1U);
            if (schema_copy == NULL || table_copy == NULL) {
                free(schema_copy);
                free(table_copy);
                return MYLITE_STORAGE_NOMEM;
            }
            const int callback_result = callback(ctx, schema_copy, table_copy);
            free(schema_copy);
            free(table_copy);
            if (callback_result != 0) {
                return MYLITE_STORAGE_ERROR;
            }
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return MYLITE_STORAGE_OK;
}

static unsigned get_u32_le(const unsigned char *page, size_t offset) {
    unsigned value = 0U;
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        value |= (unsigned)page[offset + i] << (unsigned)(i * CHAR_BIT);
    }
    return value;
}

static unsigned long long get_u64_le(const unsigned char *page, size_t offset) {
    unsigned long long value = 0ULL;
    for (size_t i = 0U; i < sizeof(uint64_t); ++i) {
        value |= (unsigned long long)page[offset + i] << (unsigned)(i * CHAR_BIT);
    }
    return value;
}

static void put_u32_le(unsigned char *page, size_t offset, unsigned value) {
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        page[offset + i] = (unsigned char)((value >> (unsigned)(i * CHAR_BIT)) & UINT8_MAX);
    }
}

static void put_u64_le(unsigned char *page, size_t offset, unsigned long long value) {
    for (size_t i = 0U; i < sizeof(uint64_t); ++i) {
        page[offset + i] = (unsigned char)((value >> (unsigned)(i * CHAR_BIT)) & UINT8_MAX);
    }
}

static uint64_t checksum_page(const unsigned char *page, size_t checksum_offset) {
    uint64_t checksum = k_fnv1a64_offset_basis;
    const size_t prefix_size = checksum_offset < MYLITE_STORAGE_FORMAT_PAGE_SIZE
                                   ? checksum_offset
                                   : MYLITE_STORAGE_FORMAT_PAGE_SIZE;
    for (size_t i = 0U; i < prefix_size; ++i) {
        checksum ^= page[i];
        checksum *= k_fnv1a64_prime;
    }
    if (checksum_offset < MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        const size_t remaining_size = MYLITE_STORAGE_FORMAT_PAGE_SIZE - checksum_offset;
        const size_t checksum_size =
            remaining_size < sizeof(uint64_t) ? remaining_size : sizeof(uint64_t);
        checksum = advance_checksum_zero_bytes(checksum, checksum_size);
        const unsigned char *suffix = page + checksum_offset + checksum_size;
        for (size_t i = 0U; i < remaining_size - checksum_size; ++i) {
            checksum ^= suffix[i];
            checksum *= k_fnv1a64_prime;
        }
    }
    return checksum;
}

static uint64_t checksum_page_zero_tail(
    const unsigned char *page,
    size_t checksum_offset,
    size_t used_size
) {
    if (used_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return checksum_page(page, checksum_offset);
    }

    uint64_t checksum = k_fnv1a64_offset_basis;
    const size_t prefix_size = checksum_offset < used_size ? checksum_offset : used_size;
    for (size_t i = 0U; i < prefix_size; ++i) {
        checksum ^= page[i];
        checksum *= k_fnv1a64_prime;
    }
    if (checksum_offset < used_size) {
        const size_t remaining_size = used_size - checksum_offset;
        const size_t checksum_size =
            remaining_size < sizeof(uint64_t) ? remaining_size : sizeof(uint64_t);
        checksum = advance_checksum_zero_bytes(checksum, checksum_size);
        const unsigned char *suffix = page + checksum_offset + checksum_size;
        for (size_t i = 0U; i < remaining_size - checksum_size; ++i) {
            checksum ^= suffix[i];
            checksum *= k_fnv1a64_prime;
        }
    }
    return advance_checksum_zero_bytes(checksum, MYLITE_STORAGE_FORMAT_PAGE_SIZE - used_size);
}

static uint64_t advance_checksum_zero_bytes(uint64_t checksum, size_t byte_count) {
    uint64_t factor = k_fnv1a64_prime;
    while (byte_count > 0U) {
        if ((byte_count & 1U) != 0U) {
            checksum *= factor;
        }
        byte_count >>= 1U;
        if (byte_count > 0U) {
            factor *= factor;
        }
    }
    return checksum;
}

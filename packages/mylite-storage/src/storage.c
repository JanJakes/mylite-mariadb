#include "storage_format.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
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

typedef struct mylite_storage_row_state_map {
    mylite_storage_row_state_entry *entries;
    size_t count;
} mylite_storage_row_state_map;

typedef struct mylite_storage_row_id_list {
    unsigned long long *row_ids;
    size_t count;
} mylite_storage_row_id_list;

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
} mylite_storage_index_entry_write;

typedef struct mylite_storage_live_row_request {
    unsigned long long table_id;
    unsigned long long row_id;
} mylite_storage_live_row_request;

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

typedef struct mylite_storage_recovery_journal {
    unsigned long long page_ids[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES];
    size_t page_count;
} mylite_storage_recovery_journal;

struct mylite_storage_statement {
    FILE *file;
    char *filename;
    struct mylite_storage_statement *parent;
    const void *owner;
    mylite_storage_header header;
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    int owns_file;
    int owns_transaction_journal;
    int preserve_auto_increment_rollback;
};

typedef struct mylite_storage_transaction_journal_snapshot {
    FILE *file;
    mylite_storage_header header;
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
} mylite_storage_transaction_journal_snapshot;

typedef mylite_storage_result (*mylite_storage_row_page_callback)(
    void *ctx,
    const mylite_storage_row_page *row_page
);

static _Thread_local mylite_storage_statement *active_statement;
static _Thread_local const void *active_context_owner;
static _Thread_local mylite_storage_statement *active_read_snapshot;
static _Thread_local mylite_storage_transaction_journal_snapshot active_transaction_journal_snapshot;

static mylite_storage_result path_exists(const char *filename, int *exists);
static mylite_storage_result write_empty_database(FILE *file);
static void initialize_header_page(unsigned char *page);
static void encode_header_page(unsigned char *page, const mylite_storage_header *header);
static void initialize_empty_catalog_page(unsigned char *page);
static void update_catalog_checksum(unsigned char *page);
static char *recovery_journal_path(const char *filename);
static char *transaction_journal_path(const char *filename);
static char *journal_path_with_suffix(const char *filename, const char *suffix, size_t suffix_size);
static mylite_storage_result recover_pending_journals(const char *filename);
static mylite_storage_result recover_pending_journals_locked(FILE *file, const char *filename);
static mylite_storage_result recover_pending_journal_locked(FILE *file, char *journal_filename);
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
static mylite_storage_result finish_transaction_journal(FILE *file, const char *filename);
static mylite_storage_result finish_journal_at_path(FILE *file, char *journal_filename);
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
static mylite_storage_result lock_file(FILE *file, int operation);
static int is_lock_conflict(int error_number);
static mylite_storage_result flush_file(FILE *file);
static mylite_storage_result truncate_file_to_header_page_count(
    FILE *file,
    const mylite_storage_header *header
);
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
static mylite_storage_statement *active_read_snapshot_for(const char *filename);
static int active_statement_has_file(FILE *file);
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
static mylite_storage_result read_checkpoint_snapshot(mylite_storage_statement *statement);
static mylite_storage_result close_statement(mylite_storage_statement *statement);
static void free_statement(mylite_storage_statement *statement);
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
static char *copy_filename(const char *filename);
static mylite_storage_result read_header(FILE *file, mylite_storage_header *out_header);
static mylite_storage_result read_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    unsigned char *out_page
);
static mylite_storage_result write_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page
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
static mylite_storage_result read_index_entry_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_index_entry_page *out_index_entry_page
);
static mylite_storage_result decode_index_entry_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    mylite_storage_index_entry_page *out_index_entry_page
);
static int is_index_entry_page(const unsigned char *page);
static mylite_storage_result prepare_index_leaf_page(
    unsigned char *page,
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
    size_t key_size,
    size_t used_bytes
);
static mylite_storage_result read_index_leaf_page(
    FILE *file,
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
static mylite_storage_result read_live_index_entries(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    mylite_storage_index_entryset *out_entries
);
static mylite_storage_result read_current_index_leaf_exact_entries(
    FILE *file,
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
static mylite_storage_result find_current_index_leaf_row_id(
    FILE *file,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long table_id,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    int *out_used_leaf
);
static mylite_storage_result append_index_leaf_matches_to_entryset(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_index_entryset *out_entries
);
static mylite_storage_result find_index_leaf_row_id(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id
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
static mylite_storage_result scan_exact_index_entries(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
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
static mylite_storage_result read_row_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_row_page *out_row_page
);
static int is_row_page(const unsigned char *page);
static mylite_storage_result validate_live_row(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_row_state_map *row_state_map,
    mylite_storage_live_row_request request,
    unsigned char *page,
    mylite_storage_row_page *out_row_page
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
static int is_row_state_page(const unsigned char *page);
static mylite_storage_row_state_entry *find_row_state_entry(
    const mylite_storage_row_state_map *row_state_map,
    unsigned long long row_id
);
static mylite_storage_result set_row_state_entry(
    mylite_storage_row_state_map *row_state_map,
    const mylite_storage_row_state_page *row_state_page
);
static void free_row_state_map(mylite_storage_row_state_map *row_state_map);
static mylite_storage_result append_row_page_to_rowset(
    void *ctx,
    const mylite_storage_row_page *row_page
);
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
static mylite_storage_result append_row_to_rowset(
    mylite_storage_rowset *rowset,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
);
static mylite_storage_result append_row_id_to_list(
    mylite_storage_row_id_list *list,
    unsigned long long row_id
);
static void remove_row_id_from_list(mylite_storage_row_id_list *list, unsigned long long row_id);
static mylite_storage_result append_index_entry_to_entryset(
    mylite_storage_index_entryset *entryset,
    const mylite_storage_index_entry_page *entry_page
);
static void remove_index_entries_by_row_id(
    mylite_storage_index_entryset *entryset,
    unsigned long long row_id
);
static mylite_storage_result count_row_page(void *ctx, const mylite_storage_row_page *row_page);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
            result = begin_recovery_journal(file, filename, &header, 1);
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
                result = finish_recovery_journal(file, filename);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
    if (result == MYLITE_STORAGE_OK && header.page_count == ULLONG_MAX) {
        result = MYLITE_STORAGE_FULL;
    }
    const unsigned long long root_page = header.page_count;
    unsigned char leaf_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    if (result == MYLITE_STORAGE_OK) {
        result = prepare_index_leaf_page(
            leaf_page,
            root_page,
            table_entry.table_id,
            index_number,
            &entryset
        );
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
        result =
            append_index_root_record(catalog_page, &definition, &lengths, table_entry.table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_recovery_journal(file, filename, &header, 1);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = write_page_at(file, root_page, header.page_size, leaf_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        header.page_count = root_page + 1ULL;
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
            result = finish_recovery_journal(file, filename);
        }
    }

    mylite_storage_free_index_entryset(&entryset);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
        result = begin_recovery_journal(file, filename, &header, 1);
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
            result = finish_recovery_journal(file, filename);
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
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    mylite_storage_row_write_position position = {0};
    if (result == MYLITE_STORAGE_OK) {
        result = begin_recovery_journal(file, filename, &header, 0);
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
        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);
        result = write_page_at(
            file,
            MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
            header.page_size,
            header_page
        );
        if (result == MYLITE_STORAGE_OK) {
            result = finish_recovery_journal(file, filename);
        }
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
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = scan_table_row_pages(file, &header, table_id, append_row_page_to_rowset, out_rows);
    }

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
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = scan_table_row_pages(file, &header, table_id, count_row_page, out_row_count);
    }

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
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_row_page row_page = {0};
    unsigned char row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = build_row_state_map(file, &header, table_id, &row_state_map);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_live_row(
            file,
            &header,
            &row_state_map,
            (mylite_storage_live_row_request){
                .table_id = table_id,
                .row_id = row_id,
            },
            row_buffer,
            &row_page
        );
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
    free_row_state_map(&row_state_map);
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
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_row_page old_row_page = {0};
    unsigned char old_row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_row_write_position position = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = build_row_state_map(file, &header, table_id, &row_state_map);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_live_row(
            file,
            &header,
            &row_state_map,
            (mylite_storage_live_row_request){
                .table_id = table_id,
                .row_id = row_id,
            },
            old_row_buffer,
            &old_row_page
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_recovery_journal(file, filename, &header, 0);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = write_row_payload_pages(file, &header, table_id, row, row_size, &position);
    }
    if (result == MYLITE_STORAGE_OK && position.next_page_id == ULLONG_MAX) {
        result = MYLITE_STORAGE_FULL;
    }
    unsigned long long next_page_id = position.next_page_id;
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
            },
            &next_page_id
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        header.page_count = next_page_id;
        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);
        result = write_page_at(
            file,
            MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
            header.page_size,
            header_page
        );
        if (result == MYLITE_STORAGE_OK) {
            result = finish_recovery_journal(file, filename);
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        *out_new_row_id = position.row_page_id;
    }

    free(old_row_page.owned_payload);
    free_row_state_map(&row_state_map);
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
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_row_page row_page = {0};
    unsigned char row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = build_row_state_map(file, &header, table_id, &row_state_map);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_live_row(
            file,
            &header,
            &row_state_map,
            (mylite_storage_live_row_request){
                .table_id = table_id,
                .row_id = row_id,
            },
            row_buffer,
            &row_page
        );
    }
    if (result == MYLITE_STORAGE_OK && header.page_count == ULLONG_MAX) {
        result = MYLITE_STORAGE_FULL;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = begin_recovery_journal(file, filename, &header, 0);
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
            unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
            encode_header_page(header_page, &header);
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
            if (result == MYLITE_STORAGE_OK) {
                result = finish_recovery_journal(file, filename);
            }
        }
    }

    free(row_page.owned_payload);
    free_row_state_map(&row_state_map);
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
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
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
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_live_index_entries(file, &header, table_id, index_number, out_entries);
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
    mylite_storage_row_id_list row_ids = {0};
    int used_leaf = 0;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &table_entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_current_index_leaf_row_id(
            file,
            &header,
            catalog_page,
            table_entry.table_id,
            schema_name,
            table_name,
            index_number,
            key,
            key_size,
            out_row_id,
            &used_leaf
        );
    }
    if (result == MYLITE_STORAGE_OK && !used_leaf) {
        result = scan_exact_index_row_ids(
            file,
            &header,
            table_entry.table_id,
            index_number,
            key,
            key_size,
            &row_ids
        );
    }
    if (result == MYLITE_STORAGE_OK && row_ids.count != 0U) {
        *out_row_id = row_ids.row_ids[0];
    }

    free(row_ids.row_ids);
    if (close_existing_file(file) != MYLITE_STORAGE_OK && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_row_id = 0ULL;
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
        result = read_current_index_leaf_exact_entries(
            file,
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
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = build_row_state_map(file, &header, table_id, &row_state_map);
    }
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         result == MYLITE_STORAGE_OK && page_id < header.page_count && !*out_exists;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_index_entry_page entry_page = {0};
        result = read_index_entry_page(file, &header, page_id, page, &entry_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            break;
        }
        if (entry_page.table_id != table_id ||
            find_row_state_entry(&row_state_map, entry_page.row_id) != NULL ||
            entry_page.key_size < key_prefix_size ||
            memcmp(entry_page.key, key_prefix, key_prefix_size) != 0) {
            continue;
        }

        *out_exists = 1;
    }

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
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
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
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
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
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
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

    if (statement->owns_transaction_journal) {
        const mylite_storage_result journal_result =
            finish_transaction_journal(statement->file, statement->filename);
        if (journal_result != MYLITE_STORAGE_OK) {
            return journal_result;
        }
    }

    active_statement = statement->parent;
    mylite_storage_result result = close_statement(statement);
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
        result = write_page_at(
            statement->file,
            MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
            statement->header.page_size,
            statement->header_page
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = publish_rollback_auto_increment_values(statement, &auto_increment_values);
    }
    free_rollback_auto_increment_values(&auto_increment_values);
    if (result == MYLITE_STORAGE_OK) {
        mylite_storage_header header = {0};
        result = read_header(statement->file, &header);
        if (result == MYLITE_STORAGE_OK) {
            result = truncate_file_to_header_page_count(statement->file, &header);
        }
    }
    if (result == MYLITE_STORAGE_OK) {
        result = flush_file(statement->file);
    }
    if (result == MYLITE_STORAGE_OK && statement->owns_transaction_journal) {
        result = finish_transaction_journal(statement->file, statement->filename);
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    active_statement = statement->parent;
    mylite_storage_result close_result = close_statement(statement);
    free_statement(statement);
    return close_result;
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
    if (result == MYLITE_STORAGE_OK) {
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
        return MYLITE_STORAGE_OK;
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

static mylite_storage_result read_checkpoint_snapshot(mylite_storage_statement *statement) {
    mylite_storage_result result = read_page_at(
        statement->file,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        statement->header_page
    );
    if (result == MYLITE_STORAGE_OK) {
        result = decode_header_page(statement->header_page, &statement->header);
    }
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
    return result;
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
    FILE *file = fopen(filename, "rb");
    if (file != NULL) {
        if (close_existing_file(file) != MYLITE_STORAGE_OK) {
            return MYLITE_STORAGE_IOERR;
        }
        *exists = 1;
        return MYLITE_STORAGE_OK;
    }

    if (errno == ENOENT) {
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
        checksum_page(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET)
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
        checksum_page(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET)
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

static mylite_storage_result recover_pending_journals(const char *filename) {
    char *journal_filename = recovery_journal_path(filename);
    if (journal_filename == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    char *transaction_filename = transaction_journal_path(filename);
    if (transaction_filename == NULL) {
        free(journal_filename);
        return MYLITE_STORAGE_NOMEM;
    }

    int journal_exists = 0;
    int transaction_exists = 0;
    mylite_storage_result result = path_exists(journal_filename, &journal_exists);
    if (result == MYLITE_STORAGE_OK) {
        result = path_exists(transaction_filename, &transaction_exists);
    }
    free(journal_filename);
    free(transaction_filename);
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
    char *journal_filename = recovery_journal_path(filename);
    if (journal_filename == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    mylite_storage_result result = recover_pending_journal_locked(file, journal_filename);
    free(journal_filename);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    journal_filename = transaction_journal_path(filename);
    if (journal_filename == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    result = recover_pending_journal_locked(file, journal_filename);
    free(journal_filename);
    return result;
}

static mylite_storage_result recover_pending_journal_locked(FILE *file, char *journal_filename) {
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
    if (fwrite(page, 1U, size, file) != size) {
        return MYLITE_STORAGE_IOERR;
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
    if (fflush(file) != 0) {
        return MYLITE_STORAGE_IOERR;
    }
    if (fsync(fileno(file)) != 0) {
        return MYLITE_STORAGE_IOERR;
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
    if (file_descriptor < 0 || ftruncate(file_descriptor, (off_t)file_size) != 0) {
        return MYLITE_STORAGE_IOERR;
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
    return MYLITE_STORAGE_IOERR;
}

static mylite_storage_result open_existing_file(const char *filename, FILE **out_file) {
    mylite_storage_statement *statement = active_statement_for(filename);
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
    return fclose(file) == 0 ? MYLITE_STORAGE_OK : MYLITE_STORAGE_IOERR;
}

static void free_statement(mylite_storage_statement *statement) {
    if (statement == NULL) {
        return;
    }

    if (statement->file != NULL && statement->owns_file) {
        fclose(statement->file);
    }
    free(statement->filename);
    free(statement);
}

static char *copy_filename(const char *filename) {
    const size_t filename_size = strlen(filename) + 1U;
    char *copy = (char *)malloc(filename_size);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, filename, filename_size);
    return copy;
}

static mylite_storage_result read_header(FILE *file, mylite_storage_header *out_header) {
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

    if (page_size == 0U || page_id > ULLONG_MAX / page_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long offset = page_id * (unsigned long long)page_size;
    if (offset > (unsigned long long)LONG_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        return MYLITE_STORAGE_IOERR;
    }

    const size_t read_count = fread(out_page, 1U, page_size, file);
    if (read_count != page_size) {
        return ferror(file) ? MYLITE_STORAGE_IOERR : MYLITE_STORAGE_CORRUPT;
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page
) {
    if (page_size == 0U || page_id > ULLONG_MAX / page_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long offset = page_id * (unsigned long long)page_size;
    if (offset > (unsigned long long)LONG_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        return MYLITE_STORAGE_IOERR;
    }
    return write_page(file, page, page_size);
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

static mylite_storage_result read_catalog_root(
    FILE *file,
    const mylite_storage_header *header,
    unsigned char *out_page
) {
    mylite_storage_result result =
        read_page_at(file, header->catalog_root_page, header->page_size, out_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return validate_catalog_root_bytes(out_page, header);
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
        checksum_page(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result find_table_id(
    FILE *file,
    const mylite_storage_header *header,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_table_id
) {
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry entry = {0};
    mylite_storage_result result = read_catalog_root(file, header, catalog_page);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &entry);
    }
    if (result == MYLITE_STORAGE_OK) {
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
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET,
        checksum_page(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET)
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

static mylite_storage_result write_index_entry_pages(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_index_entry_write write,
    unsigned long long *out_next_page_id
) {
    if (write.index_entry_count > ULLONG_MAX - write.first_page_id) {
        return MYLITE_STORAGE_FULL;
    }

    for (size_t i = 0U; i < write.index_entry_count; ++i) {
        const unsigned long long page_id = write.first_page_id + (unsigned long long)i;
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
    }

    *out_next_page_id = write.first_page_id + (unsigned long long)write.index_entry_count;
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
        checksum_page(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result read_index_entry_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_index_entry_page *out_index_entry_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return decode_index_entry_page(header, page_id, page, out_index_entry_page);
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

static mylite_storage_result prepare_index_leaf_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned index_number,
    const mylite_storage_index_entryset *entryset
) {
    size_t key_size = 0U;
    mylite_storage_result result = index_entryset_fixed_key_size(entryset, &key_size);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    size_t *order = NULL;
    result = build_raw_index_entry_order(entryset, &order);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    const size_t cell_size = MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size;
    size_t used_bytes = MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET;
    if (entryset->entry_count != 0U) {
        if (cell_size == 0U ||
            entryset->entry_count > (MYLITE_STORAGE_FORMAT_PAGE_SIZE - used_bytes) / cell_size) {
            free(order);
            return MYLITE_STORAGE_FULL;
        }
        used_bytes += entryset->entry_count * cell_size;
    }

    encode_index_leaf_page(
        page,
        page_id,
        table_id,
        index_number,
        entryset,
        order,
        key_size,
        used_bytes
    );
    free(order);
    return MYLITE_STORAGE_OK;
}

static void encode_index_leaf_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned index_number,
    const mylite_storage_index_entryset *entryset,
    const size_t *order,
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
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET,
        (unsigned)entryset->entry_count
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_USED_BYTES_OFFSET, (unsigned)used_bytes);

    const size_t cell_size = MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size;
    for (size_t i = 0U; i < entryset->entry_count; ++i) {
        const size_t entry_index = order[i];
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
        checksum_page(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result read_index_leaf_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_index_leaf_page *out_index_leaf_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return decode_index_leaf_page(header, page_id, page, out_index_leaf_page);
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
    mylite_storage_result result = build_row_state_map(file, header, table_id, &row_state_map);
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         result == MYLITE_STORAGE_OK && page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_index_entry_page entry_page = {0};
        result = read_index_entry_page(file, header, page_id, page, &entry_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            break;
        }
        if (entry_page.table_id != table_id || entry_page.index_number != index_number ||
            find_row_state_entry(&row_state_map, entry_page.row_id) != NULL) {
            continue;
        }

        result = append_index_entry_to_entryset(out_entries, &entry_page);
    }

    free_row_state_map(&row_state_map);
    return result;
}

static mylite_storage_result read_current_index_leaf_exact_entries(
    FILE *file,
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
    if (root_entry.definition_root_page + 1ULL != header->page_count) {
        return MYLITE_STORAGE_OK;
    }

    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_index_leaf_page leaf_page = {0};
    result = read_index_leaf_page(file, header, root_entry.definition_root_page, page, &leaf_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (leaf_page.table_id != table_id || leaf_page.index_number != index_number ||
        leaf_page.entry_count != root_entry.definition_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_used_leaf = 1;
    return append_index_leaf_matches_to_entryset(&leaf_page, key, key_size, out_entries);
}

static mylite_storage_result find_current_index_leaf_row_id(
    FILE *file,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long table_id,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id,
    int *out_used_leaf
) {
    *out_used_leaf = 0;
    *out_row_id = 0ULL;

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
    if (root_entry.definition_root_page + 1ULL != header->page_count) {
        return MYLITE_STORAGE_OK;
    }

    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_index_leaf_page leaf_page = {0};
    result = read_index_leaf_page(file, header, root_entry.definition_root_page, page, &leaf_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (leaf_page.table_id != table_id || leaf_page.index_number != index_number ||
        leaf_page.entry_count != root_entry.definition_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_used_leaf = 1;
    return find_index_leaf_row_id(&leaf_page, key, key_size, out_row_id);
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

    for (size_t i = lower; i < leaf_page->entry_count; ++i) {
        if (compare_leaf_key(leaf_page, i, key, key_size) != 0) {
            break;
        }
        const mylite_storage_index_entry_page entry_page = {
            .table_id = leaf_page->table_id,
            .row_id = index_leaf_entry_row_id(leaf_page, i),
            .index_number = leaf_page->index_number,
            .key_size = leaf_page->key_size,
            .key = index_leaf_entry_key(leaf_page, i),
        };
        const mylite_storage_result result =
            append_index_entry_to_entryset(out_entries, &entry_page);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result find_index_leaf_row_id(
    const mylite_storage_index_leaf_page *leaf_page,
    const unsigned char *key,
    size_t key_size,
    unsigned long long *out_row_id
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
    if (lower < leaf_page->entry_count && compare_leaf_key(leaf_page, lower, key, key_size) == 0) {
        *out_row_id = index_leaf_entry_row_id(leaf_page, lower);
    }
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
                find_row_state_entry(&row_state_map, entry_page.row_id) == NULL &&
                entry_page.key_size == key_size && memcmp(entry_page.key, key, key_size) == 0) {
                result = append_row_id_to_list(out_row_ids, entry_page.row_id);
            }
            continue;
        }

        if (is_row_state_page(page)) {
            mylite_storage_row_state_page row_state_page = {0};
            result = decode_row_state_page(header, page_id, page, &row_state_page);
            if (result == MYLITE_STORAGE_OK && row_state_page.table_id == table_id) {
                result = set_row_state_entry(&row_state_map, &row_state_page);
                if (result == MYLITE_STORAGE_OK) {
                    remove_row_id_from_list(out_row_ids, row_state_page.source_row_id);
                }
            }
            continue;
        }

        if (!is_exact_index_scan_skip_page(page)) {
            result = MYLITE_STORAGE_CORRUPT;
        }
    }

    free_row_state_map(&row_state_map);
    if (result != MYLITE_STORAGE_OK) {
        free(out_row_ids->row_ids);
        *out_row_ids = (mylite_storage_row_id_list){0};
    }
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
                find_row_state_entry(&row_state_map, entry_page.row_id) == NULL &&
                entry_page.key_size == key_size && memcmp(entry_page.key, key, key_size) == 0) {
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
                    remove_index_entries_by_row_id(out_entries, row_state_page.source_row_id);
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

    unsigned char *owned_payload = NULL;
    const unsigned char *payload = page + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    if (overflow_root_page != 0ULL) {
        mylite_storage_result payload_result = read_row_payload_blob_pages(
            file,
            header,
            (mylite_storage_blob_chain){
                .first_page_id = overflow_root_page,
                .payload_size = row_size,
                .page_type = MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_ROW_PAYLOAD,
            },
            &owned_payload
        );
        if (payload_result != MYLITE_STORAGE_OK) {
            return payload_result;
        }
        payload = owned_payload;
    }

    *out_row_page = (mylite_storage_row_page){
        .row_id = page_id,
        .table_id = table_id,
        .row_size = row_size,
        .row_count = row_count,
        .payload = payload,
        .owned_payload = owned_payload,
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

static mylite_storage_result validate_live_row(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_row_state_map *row_state_map,
    mylite_storage_live_row_request request,
    unsigned char *page,
    mylite_storage_row_page *out_row_page
) {
    if (request.row_id <= header->catalog_root_page || request.row_id >= header->page_count) {
        return MYLITE_STORAGE_NOTFOUND;
    }
    if (find_row_state_entry(row_state_map, request.row_id) != NULL) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    mylite_storage_row_page row_page = {0};
    mylite_storage_result result = read_row_page(file, header, request.row_id, page, &row_page);
    if (result == MYLITE_STORAGE_NOTFOUND) {
        return MYLITE_STORAGE_NOTFOUND;
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (row_page.table_id != request.table_id) {
        free(row_page.owned_payload);
        return MYLITE_STORAGE_NOTFOUND;
    }

    *out_row_page = row_page;
    return MYLITE_STORAGE_OK;
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
        checksum_page(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET)
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

    if (row_state_map->count == SIZE_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_count = row_state_map->count + 1U;
    mylite_storage_row_state_entry *entries = (mylite_storage_row_state_entry *)
        realloc(row_state_map->entries, new_count * sizeof(mylite_storage_row_state_entry));
    if (entries == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    row_state_map->entries = entries;
    row_state_map->entries[row_state_map->count] = (mylite_storage_row_state_entry){
        .source_row_id = row_state_page->source_row_id,
        .replacement_row_id = row_state_page->replacement_row_id,
        .state_kind = row_state_page->state_kind,
    };
    row_state_map->count = new_count;
    return MYLITE_STORAGE_OK;
}

static void free_row_state_map(mylite_storage_row_state_map *row_state_map) {
    free(row_state_map->entries);
    *row_state_map = (mylite_storage_row_state_map){0};
}

static mylite_storage_result append_row_page_to_rowset(
    void *ctx,
    const mylite_storage_row_page *row_page
) {
    mylite_storage_rowset *rowset = (mylite_storage_rowset *)ctx;
    for (size_t i = 0U; i < row_page->row_count; ++i) {
        const mylite_storage_result result = append_row_to_rowset(
            rowset,
            row_page->row_id,
            row_page->payload + (i * row_page->row_size),
            row_page->row_size
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }

    return MYLITE_STORAGE_OK;
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

    result = begin_recovery_journal(file, filename, header, 0);
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
        result = finish_recovery_journal(file, filename);
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
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    encode_header_page(header_page, header);
    return write_page_at(
        file,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        header->page_size,
        header_page
    );
}

static mylite_storage_result append_row_to_rowset(
    mylite_storage_rowset *rowset,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
) {
    if (rowset->row_count == SIZE_MAX || row_size > SIZE_MAX - rowset->row_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_row_count = rowset->row_count + 1U;
    const size_t new_row_bytes = rowset->row_bytes + row_size;

    unsigned char *rows = (unsigned char *)realloc(rowset->rows, new_row_bytes);
    if (rows == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    rowset->rows = rows;

    size_t *row_offsets = (size_t *)realloc(rowset->row_offsets, new_row_count * sizeof(size_t));
    if (row_offsets == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    rowset->row_offsets = row_offsets;

    size_t *row_sizes = (size_t *)realloc(rowset->row_sizes, new_row_count * sizeof(size_t));
    if (row_sizes == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    rowset->row_sizes = row_sizes;

    unsigned long long *row_ids =
        (unsigned long long *)realloc(rowset->row_ids, new_row_count * sizeof(unsigned long long));
    if (row_ids == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    rowset->row_ids = row_ids;

    memcpy(rowset->rows + rowset->row_bytes, row, row_size);
    rowset->row_offsets[rowset->row_count] = rowset->row_bytes;
    rowset->row_sizes[rowset->row_count] = row_size;
    rowset->row_ids[rowset->row_count] = row_id;
    rowset->row_count = new_row_count;
    rowset->row_bytes = new_row_bytes;
    if (rowset->row_count == 1U) {
        rowset->row_size = row_size;
    } else if (rowset->row_size != row_size) {
        rowset->row_size = 0U;
    }
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

static mylite_storage_result append_index_entry_to_entryset(
    mylite_storage_index_entryset *entryset,
    const mylite_storage_index_entry_page *entry_page
) {
    if (entryset->entry_count == SIZE_MAX ||
        entry_page->key_size > SIZE_MAX - entryset->key_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_entry_count = entryset->entry_count + 1U;
    const size_t new_key_bytes = entryset->key_bytes + entry_page->key_size;
    if (new_entry_count > SIZE_MAX / sizeof(size_t) ||
        new_entry_count > SIZE_MAX / sizeof(unsigned long long)) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned char *keys = (unsigned char *)realloc(entryset->keys, new_key_bytes);
    if (keys == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->keys = keys;

    size_t *key_offsets =
        (size_t *)realloc(entryset->key_offsets, new_entry_count * sizeof(size_t));
    if (key_offsets == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->key_offsets = key_offsets;

    size_t *key_sizes = (size_t *)realloc(entryset->key_sizes, new_entry_count * sizeof(size_t));
    if (key_sizes == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->key_sizes = key_sizes;

    unsigned long long *row_ids = (unsigned long long *)
        realloc(entryset->row_ids, new_entry_count * sizeof(unsigned long long));
    if (row_ids == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->row_ids = row_ids;

    memcpy(entryset->keys + entryset->key_bytes, entry_page->key, entry_page->key_size);
    entryset->key_offsets[entryset->entry_count] = entryset->key_bytes;
    entryset->key_sizes[entryset->entry_count] = entry_page->key_size;
    entryset->row_ids[entryset->entry_count] = entry_page->row_id;
    entryset->entry_count = new_entry_count;
    entryset->key_bytes = new_key_bytes;
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

static mylite_storage_result count_row_page(void *ctx, const mylite_storage_row_page *row_page) {
    unsigned long long *row_count = (unsigned long long *)ctx;
    if (row_page->row_count > ULLONG_MAX - *row_count) {
        return MYLITE_STORAGE_FULL;
    }

    *row_count += (unsigned long long)row_page->row_count;
    return MYLITE_STORAGE_OK;
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
        checksum_page(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET)
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

    mylite_storage_result result = begin_recovery_journal(file, filename, header, 0);
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
        result = finish_recovery_journal(file, filename);
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
    uint64_t checksum = UINT64_C(1469598103934665603);
    for (size_t i = 0U; i < MYLITE_STORAGE_FORMAT_PAGE_SIZE; ++i) {
        const unsigned char byte =
            i >= checksum_offset && i < checksum_offset + sizeof(uint64_t) ? 0U : page[i];
        checksum ^= byte;
        checksum *= UINT64_C(1099511628211);
    }
    return checksum;
}

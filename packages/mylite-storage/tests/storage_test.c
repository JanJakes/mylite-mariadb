#include "storage_format.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mylite/storage.h>

typedef struct index_entries_test_context {
    const char *filename;
    const unsigned char *row_1;
    size_t row_1_size;
    const unsigned char *row_2;
    size_t row_2_size;
    const unsigned char *updated_row_1;
    size_t updated_row_1_size;
    const mylite_storage_index_entry *row_1_entries;
    size_t row_1_entry_count;
    const mylite_storage_index_entry *row_2_entries;
    size_t row_2_entry_count;
    const mylite_storage_index_entry *update_entries;
    size_t update_entry_count;
    const unsigned char *key_1;
    size_t key_1_size;
    const unsigned char *key_2;
    size_t key_2_size;
    const unsigned char *key_9;
    size_t key_9_size;
    const unsigned char *title_u;
    size_t title_u_size;
    unsigned long long row_1_id;
    unsigned long long row_2_id;
    unsigned long long updated_row_1_id;
} index_entries_test_context;

typedef struct statement_checkpoint_test_context {
    const char *filename;
    const char *journal_filename;
    const unsigned char *row_1;
    size_t row_1_size;
    const unsigned char *row_2;
    size_t row_2_size;
    const mylite_storage_table_definition *rollback_definition;
    const mylite_storage_index_entry *row_1_entry;
    const mylite_storage_index_entry *row_2_entry;
    const unsigned char *key_1;
    size_t key_1_size;
    unsigned long long row_1_id;
    unsigned long long row_2_id;
} statement_checkpoint_test_context;

typedef struct transaction_journal_test_context {
    const char *filename;
    const char *journal_filename;
    const char *transaction_journal_filename;
} transaction_journal_test_context;

typedef struct lock_child {
    pid_t pid;
    int release_fd;
} lock_child;

typedef struct transaction_child {
    pid_t pid;
    int release_fd;
    unsigned long long row_id;
} transaction_child;

typedef struct timed_lock_request {
    int operation;
    unsigned milliseconds;
} timed_lock_request;

static const unsigned k_busy_timeout_expiry_ms = 20U;
static const unsigned k_busy_timeout_release_ms = 50U;
static const unsigned k_busy_timeout_wait_ms = 1000U;
static const useconds_t k_microseconds_per_millisecond = 1000U;
static const unsigned long long k_autoincrement_set_value = 5ULL;
static const unsigned long long k_autoincrement_lower_value = 3ULL;
static const unsigned long long k_autoincrement_ignored_advance = 4ULL;
static const unsigned long long k_autoincrement_advanced_value = 9ULL;

static void test_capabilities(void);
static void test_create_empty_database(void);
static void test_schema_records(void);
static void test_missing_file(void);
static void test_rejects_bad_magic(void);
static void test_rejects_bad_checksum(void);
static void test_rejects_newer_format_version(void);
static void test_rejects_bad_catalog_root(void);
static void test_store_and_read_table_definition(void);
static void test_store_large_table_definition(void);
static void test_foreign_key_metadata_records(void);
static void assert_foreign_key_metadata(
    const mylite_storage_foreign_key_metadata *metadata,
    const char *table_name,
    const char *referenced_table_name,
    const char *referenced_key_name
);
static void test_append_and_read_rows(void);
static void test_append_and_read_large_row_payload(void);
static void test_update_and_delete_rows(void);
static void test_many_row_state_pages_scan(void);
static void test_active_live_row_validation_cache(void);
static void test_reusable_live_row_cache_clears_row_ids(void);
static void test_active_row_payload_cache(void);
static void test_active_row_payload_cache_large_window(void);
static void test_active_row_payload_cache_validates_update(void);
static void test_active_table_entry_cache_catalog_invalidation(void);
static void test_active_row_payload_cache_many_replacements(void);
static void test_durable_live_row_cache(void);
static void test_deferred_durable_cache_retarget(void);
static void test_active_live_row_list_maintenance(void);
static void test_index_entries(void);
static void test_exact_index_cache_fixed_size_keys(void);
static void test_cached_exact_index_entryset_bulk_append(void);
static void test_active_exact_index_cache_many_replacements(void);
static void test_large_append_buffer_savepoint_rollback(void);
static void test_active_update_rewrite(void);
static void test_unchanged_index_update_elision(void);
static void test_indexed_row_batch_cache_reuses_duplicates(void);
static void test_index_root_metadata(void);
static void test_index_leaf_pages(void);
static void test_multi_page_index_leaf_pages(void);
static void test_multi_page_index_leaf_duplicate_boundaries(void);
static void assert_index_prefix_exists(
    const char *filename,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    int expected_exists
);
static void assert_index_entry_lookup(
    const char *filename,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_result expected_result,
    unsigned long long expected_row_id
);
static void assert_exact_index_entries(
    const char *filename,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    const unsigned long long *expected_row_ids,
    size_t expected_count
);
static void assert_indexed_rows_equal(
    const char *filename,
    const unsigned long long *row_ids,
    size_t row_id_count,
    const unsigned char *const *expected_rows,
    const size_t *expected_row_sizes
);
static void append_index_entry_test_rows(index_entries_test_context *ctx);
static void assert_primary_index_entries_after_insert(const index_entries_test_context *ctx);
static void update_index_entry_test_row(index_entries_test_context *ctx);
static void assert_primary_index_entries_after_update(const index_entries_test_context *ctx);
static void delete_index_entry_test_row(const index_entries_test_context *ctx);
static void assert_secondary_index_entries_after_delete(const index_entries_test_context *ctx);
static void assert_index_entry_test_live_rows(const index_entries_test_context *ctx);
static void assert_index_root(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    unsigned long long expected_root_page,
    unsigned long long expected_entry_count
);
static void assert_exact_index_entries_for_table(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    const unsigned long long *expected_row_ids,
    size_t expected_count
);
static void test_autoincrement_state(void);
static void exercise_exact_autoincrement_set(const char *filename);
static void assert_auto_increment_value(const char *filename, unsigned long long expected_value);
static void test_truncate_table_lifecycle(void);
static void append_truncate_test_rows(
    const char *filename,
    const unsigned char *row_1,
    size_t row_1_size,
    const mylite_storage_index_entry *row_1_entry,
    const unsigned char *row_2,
    size_t row_2_size,
    const mylite_storage_index_entry *row_2_entry,
    unsigned long long *out_row_1_id,
    unsigned long long *out_row_2_id
);
static void assert_truncate_test_initial_state(const char *filename);
static void assert_truncate_test_empty_state(
    const char *filename,
    unsigned long long row_1_id,
    unsigned long long row_2_id
);
static void assert_empty_truncate_is_noop(const char *filename);
static void append_truncate_test_reused_row(
    const char *filename,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entry,
    unsigned long long row_2_id,
    unsigned long long *out_row_id
);
static void assert_truncate_test_reused_row(
    const char *filename,
    unsigned long long row_id,
    const unsigned char *key,
    size_t key_size
);
static void test_statement_checkpoints(void);
static void assert_statement_checkpoint_rolls_back_row(statement_checkpoint_test_context *ctx);
static void assert_statement_checkpoint_commits_row(statement_checkpoint_test_context *ctx);
static void assert_statement_checkpoint_rolls_back_catalog(statement_checkpoint_test_context *ctx);
static void assert_nested_statement_checkpoints(statement_checkpoint_test_context *ctx);
static void assert_statement_checkpoint_preserves_marked_auto_increment_rollback(
    statement_checkpoint_test_context *ctx
);
static void test_read_statement_storage_session(void);
static void test_read_checkpoint_snapshot_cache(void);
static void test_read_statement_file_cache_path_replacement(void);
static void test_transaction_journals(void);
static void test_transaction_owner_isolation(void);
static void test_cross_process_transaction_read_snapshot(void);
static void assert_transaction_journal_commits(const transaction_journal_test_context *ctx);
static void assert_transaction_journal_rolls_back(const transaction_journal_test_context *ctx);
static void assert_transaction_journal_preserves_auto_increment_rollback(
    const transaction_journal_test_context *ctx
);
static void assert_transaction_exact_cache_invalidates_on_savepoint_rollback(
    const transaction_journal_test_context *ctx
);
static void assert_transaction_journal_recovers_child_exit(
    const transaction_journal_test_context *ctx
);
static void assert_transaction_and_statement_journals_recover_in_order(
    const transaction_journal_test_context *ctx
);
static void test_cleans_recovery_journal_after_mutations(void);
static void test_recovers_row_publication_journal(void);
static void test_recovers_catalog_publication_journal(void);
static void test_rejects_corrupt_recovery_journal(void);
static void test_rejects_operations_during_exclusive_file_lock(void);
static void test_shared_file_lock_allows_readers_and_blocks_writers(void);
static void test_recovery_requires_exclusive_file_lock(void);
static void test_transaction_recovery_requires_exclusive_file_lock(void);
static void test_busy_timeout_expires_while_lock_held(void);
static void test_busy_timeout_waits_for_lock_release(void);
static void test_write_size_limit_returns_full(void);
static void test_rejects_corrupt_row_page(void);
static void test_rejects_corrupt_row_payload_page(void);
static void test_rejects_corrupt_row_state_page(void);
static void test_rejects_corrupt_index_entry_page(void);
static void test_rejects_corrupt_autoincrement_page(void);
static void test_drop_table_definition(void);
static void test_rename_table_definition(void);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static char *journal_path(const char *filename);
static char *transaction_journal_path(const char *filename);
static long long file_size(const char *path);
static void assert_file_size_matches_header(const char *filename);
static void assert_file_missing(const char *path);
static void assert_post_rowset_layout(const mylite_storage_rowset *rows, size_t row_size);
static void assert_lifecycle_initial_rows(const mylite_storage_rowset *rows);
static void assert_lifecycle_live_rows(
    const mylite_storage_rowset *rows,
    unsigned long long new_row_id
);
static void assert_row_not_found(const char *filename, unsigned long long row_id);
static void assert_row_equals(
    const char *filename,
    unsigned long long row_id,
    const unsigned char *expected_row,
    size_t expected_row_size
);
static void assert_indexed_row_equals(
    const char *filename,
    unsigned long long row_id,
    const unsigned char *expected_row,
    size_t expected_row_size
);
static void assert_find_indexed_row_equals(
    const char *filename,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long expected_row_id,
    const unsigned char *expected_row,
    size_t expected_row_size
);
static void assert_find_indexed_row_not_found(
    const char *filename,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size
);
static void assert_index_entry(
    const mylite_storage_index_entryset *index_entries,
    size_t entry_index,
    unsigned long long row_id,
    const unsigned char *key,
    size_t key_size
);
static void read_test_page(const char *path, unsigned long long page_id, unsigned char *out_page);
static void write_test_recovery_journal(
    const char *filename,
    const unsigned long long *page_ids,
    size_t page_count,
    const unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE]
);
static void write_test_transaction_journal(
    const char *filename,
    const unsigned long long *page_ids,
    size_t page_count,
    const unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE]
);
static void write_test_journal_header_page(
    unsigned char *page,
    const unsigned long long *page_ids,
    size_t page_count
);
static lock_child hold_test_lock(const char *filename, int operation);
static void release_test_lock(lock_child child);
static pid_t hold_test_lock_for(const char *filename, timed_lock_request request);
static void wait_test_lock_child(pid_t pid);
static transaction_child hold_transaction_with_uncommitted_row(
    const char *filename,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entry
);
static void release_transaction_child(transaction_child child);
static void put_test_u32_le(unsigned char *page, size_t offset, unsigned value);
static void put_test_u64_le(unsigned char *page, size_t offset, unsigned long long value);
static unsigned long long checksum_test_page(const unsigned char *page, size_t checksum_offset);
static void flip_file_byte(const char *path, long offset);
static void write_header_format_version(const char *path, unsigned value);
static int collect_table(void *ctx, const char *schema_name, const char *table_name);
static int collect_schema(void *ctx, const char *schema_name);
static int collect_foreign_key(void *ctx, const mylite_storage_foreign_key_metadata *metadata);

typedef struct table_list_capture {
    const char *schema_name;
    const char *table_name;
    unsigned count;
} table_list_capture;

typedef struct schema_list_capture {
    const char *app_schema;
    const char *blog_schema;
    unsigned count;
} schema_list_capture;

typedef struct foreign_key_list_capture {
    const char *expected_constraint_name;
    const char *expected_table_name;
    const char *expected_referenced_table_name;
    size_t expected_column_count;
    unsigned count;
} foreign_key_list_capture;

int main(void) {
    test_capabilities();
    test_create_empty_database();
    test_schema_records();
    test_missing_file();
    test_rejects_bad_magic();
    test_rejects_bad_checksum();
    test_rejects_newer_format_version();
    test_rejects_bad_catalog_root();
    test_store_and_read_table_definition();
    test_store_large_table_definition();
    test_foreign_key_metadata_records();
    test_append_and_read_rows();
    test_append_and_read_large_row_payload();
    test_update_and_delete_rows();
    test_many_row_state_pages_scan();
    test_active_live_row_validation_cache();
    test_reusable_live_row_cache_clears_row_ids();
    test_active_row_payload_cache();
    test_active_row_payload_cache_large_window();
    test_active_row_payload_cache_validates_update();
    test_active_table_entry_cache_catalog_invalidation();
    test_active_row_payload_cache_many_replacements();
    test_durable_live_row_cache();
    test_deferred_durable_cache_retarget();
    test_active_live_row_list_maintenance();
    test_index_entries();
    test_exact_index_cache_fixed_size_keys();
    test_cached_exact_index_entryset_bulk_append();
    test_active_exact_index_cache_many_replacements();
    test_large_append_buffer_savepoint_rollback();
    test_active_update_rewrite();
    test_unchanged_index_update_elision();
    test_indexed_row_batch_cache_reuses_duplicates();
    test_index_root_metadata();
    test_index_leaf_pages();
    test_multi_page_index_leaf_pages();
    test_multi_page_index_leaf_duplicate_boundaries();
    test_autoincrement_state();
    test_truncate_table_lifecycle();
    test_statement_checkpoints();
    test_read_statement_storage_session();
    test_read_checkpoint_snapshot_cache();
    test_read_statement_file_cache_path_replacement();
    test_transaction_journals();
    test_transaction_owner_isolation();
    test_cross_process_transaction_read_snapshot();
    test_cleans_recovery_journal_after_mutations();
    test_recovers_row_publication_journal();
    test_recovers_catalog_publication_journal();
    test_rejects_corrupt_recovery_journal();
    test_rejects_operations_during_exclusive_file_lock();
    test_shared_file_lock_allows_readers_and_blocks_writers();
    test_recovery_requires_exclusive_file_lock();
    test_transaction_recovery_requires_exclusive_file_lock();
    test_busy_timeout_expires_while_lock_held();
    test_busy_timeout_waits_for_lock_release();
    test_write_size_limit_returns_full();
    test_rejects_corrupt_row_page();
    test_rejects_corrupt_row_payload_page();
    test_rejects_corrupt_row_state_page();
    test_rejects_corrupt_index_entry_page();
    test_rejects_corrupt_autoincrement_page();
    test_drop_table_definition();
    test_rename_table_definition();
    return 0;
}

static void test_capabilities(void) {
    const mylite_storage_capabilities capabilities = mylite_storage_get_capabilities();

    assert(strcmp(mylite_storage_engine_name(), MYLITE_STORAGE_ENGINE_NAME) == 0);
    assert(capabilities.size == sizeof(capabilities));
    assert(capabilities.format_version == MYLITE_STORAGE_FORMAT_VERSION);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_FILE_HEADER) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_EMPTY_CATALOG) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_TABLE_DEFINITIONS) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_TABLE_ROWS) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_AUTOINCREMENT) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_BLOB_TEXT_ROWS) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_ROW_LIFECYCLE) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_INDEX_ENTRIES) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_RECOVERY_JOURNAL) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_FILE_LOCKS) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_TRUNCATE) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_SCHEMAS) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_STATEMENT_CHECKPOINTS) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_BUSY_TIMEOUT) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_TRANSACTION_JOURNAL) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_FOREIGN_KEY_METADATA) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_INDEX_ROOTS) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_INDEX_LEAF_PAGES) != 0U);
}

static void test_create_empty_database(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "empty.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_ERROR);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.size == sizeof(header));
    assert(header.format_version == MYLITE_STORAGE_FORMAT_VERSION);
    assert(header.header_version == MYLITE_STORAGE_FORMAT_HEADER_VERSION);
    assert(header.page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    assert(header.checksum_algorithm == MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64);
    assert(header.catalog_root_page == MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID);
    assert(header.catalog_generation == MYLITE_STORAGE_FORMAT_EMPTY_CATALOG_GENERATION);
    assert(header.free_list_root_page == 0ULL);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT);
    assert(
        file_size(filename) ==
        (long long)(MYLITE_STORAGE_FORMAT_PAGE_SIZE * MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT)
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_schema_records(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "schemas.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "blog",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_schema_definition schema_definition = {
        .size = sizeof(schema_definition),
        .schema_name = "app",
        .default_character_set_name = "latin1",
        .default_collation_name = "latin1_bin",
        .schema_comment = "application schema",
    };
    mylite_storage_schema_metadata schema_metadata = {0};
    schema_list_capture schemas = {
        .app_schema = "app",
        .blog_schema = "blog",
    };
    table_list_capture tables = {
        .schema_name = "blog",
        .table_name = "posts",
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_schema_exists(filename, "app") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_store_schema(filename, "app") == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_schema(filename, "app") == MYLITE_STORAGE_OK);
    assert(mylite_storage_schema_exists(filename, "app") == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_schema_definition(filename, &schema_definition) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_read_schema_definition(filename, "app", &schema_metadata) ==
        MYLITE_STORAGE_OK
    );
    assert(strcmp(schema_metadata.default_character_set_name, "latin1") == 0);
    assert(strcmp(schema_metadata.default_collation_name, "latin1_bin") == 0);
    assert(strcmp(schema_metadata.schema_comment, "application schema") == 0);
    mylite_storage_free(schema_metadata.default_character_set_name);
    mylite_storage_free(schema_metadata.default_collation_name);
    mylite_storage_free(schema_metadata.schema_comment);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_schema_exists(filename, "blog") == MYLITE_STORAGE_OK);
    assert(mylite_storage_list_schemas(filename, collect_schema, &schemas) == MYLITE_STORAGE_OK);
    assert(schemas.count == 2U);

    assert(mylite_storage_drop_schema(filename, "app") == MYLITE_STORAGE_OK);
    assert(mylite_storage_schema_exists(filename, "app") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_schema_exists(filename, "blog") == MYLITE_STORAGE_OK);
    assert(mylite_storage_drop_schema(filename, "blog") == MYLITE_STORAGE_OK);
    assert(mylite_storage_schema_exists(filename, "blog") == MYLITE_STORAGE_NOTFOUND);
    assert(
        mylite_storage_list_tables(filename, "blog", collect_table, &tables) == MYLITE_STORAGE_OK
    );
    assert(tables.count == 0U);
    assert(mylite_storage_drop_schema(filename, "blog") == MYLITE_STORAGE_NOTFOUND);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_missing_file(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "missing.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_NOTFOUND);

    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_bad_magic(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "bad-magic.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    flip_file_byte(filename, MYLITE_STORAGE_FORMAT_HEADER_MAGIC_OFFSET);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_CORRUPT);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_bad_checksum(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "bad-checksum.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    flip_file_byte(filename, MYLITE_STORAGE_FORMAT_HEADER_FLAGS_OFFSET);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_CORRUPT);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_newer_format_version(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "newer-version.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_header_format_version(filename, MYLITE_STORAGE_FORMAT_VERSION + 1U);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_UNSUPPORTED);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_bad_catalog_root(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "bad-catalog.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    flip_file_byte(
        filename,
        (long)MYLITE_STORAGE_FORMAT_PAGE_SIZE + MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_CORRUPT);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_store_and_read_table_definition(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "table-definition.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;
    mylite_storage_table_metadata metadata = {
        .size = sizeof(metadata),
    };
    table_list_capture capture = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_ERROR
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.catalog_generation == MYLITE_STORAGE_FORMAT_EMPTY_CATALOG_GENERATION + 1ULL);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 1ULL);
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "missing") == MYLITE_STORAGE_NOTFOUND);
    assert(
        mylite_storage_read_table_definition(
            filename,
            "app",
            "posts",
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == sizeof(definition));
    assert(memcmp(stored_definition, definition, sizeof(definition)) == 0);
    mylite_storage_free(stored_definition);
    assert(
        mylite_storage_read_table_metadata(filename, "app", "posts", &metadata) == MYLITE_STORAGE_OK
    );
    assert(strcmp(metadata.requested_engine_name, "MYLITE") == 0);
    assert(strcmp(metadata.effective_engine_name, "MYLITE") == 0);
    mylite_storage_free(metadata.requested_engine_name);
    mylite_storage_free(metadata.effective_engine_name);

    assert(
        mylite_storage_list_tables(filename, "app", collect_table, &capture) == MYLITE_STORAGE_OK
    );
    assert(capture.count == 1U);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_store_large_table_definition(void) {
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    const size_t definition_size = (payload_capacity * 2U) + 17U;
    char *root = make_temp_root();
    char *filename = path_join(root, "large-table-definition.mylite");
    unsigned char *definition = (unsigned char *)malloc(definition_size);
    assert(definition != NULL);
    for (size_t i = 0U; i < definition_size; ++i) {
        definition[i] = (unsigned char)(i % UINT8_MAX);
    }

    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "large_posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = definition_size,
    };
    mylite_storage_header header = {0};
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 3ULL);
    assert(
        mylite_storage_read_table_definition(
            filename,
            "app",
            "large_posts",
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == definition_size);
    assert(memcmp(stored_definition, definition, definition_size) == 0);

    mylite_storage_free(stored_definition);
    free(definition);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_foreign_key_metadata_records(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    const char *foreign_columns[] = {"user_id", "site_id"};
    const char *referenced_columns[] = {"id", "site_id"};
    char *root = make_temp_root();
    char *filename = path_join(root, "foreign-keys.mylite");
    mylite_storage_table_definition users_definition = {
        .size = sizeof(users_definition),
        .schema_name = "app",
        .table_name = "users",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_table_definition posts_definition = {
        .size = sizeof(posts_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_table_definition rebuilt_users_definition = {
        .size = sizeof(rebuilt_users_definition),
        .schema_name = "app",
        .table_name = "rebuilt_users",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_table_definition rebuilt_posts_definition = {
        .size = sizeof(rebuilt_posts_definition),
        .schema_name = "app",
        .table_name = "rebuilt_posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_foreign_key_definition foreign_key = {
        .size = sizeof(foreign_key),
        .schema_name = "app",
        .table_name = "posts",
        .constraint_name = "fk_posts_users",
        .referenced_schema_name = "app",
        .referenced_table_name = "users",
        .referenced_key_name = "PRIMARY",
        .foreign_column_names = foreign_columns,
        .referenced_column_names = referenced_columns,
        .column_count = 2U,
        .update_action = MYLITE_STORAGE_FOREIGN_KEY_ACTION_NO_ACTION,
        .delete_action = MYLITE_STORAGE_FOREIGN_KEY_ACTION_RESTRICT,
        .match_option = MYLITE_STORAGE_FOREIGN_KEY_MATCH_SIMPLE,
        .nullable_column_bitmap = 0x1ULL,
        .referenced_nullable_column_bitmap = 0x2ULL,
    };
    mylite_storage_foreign_key_metadata metadata = {
        .size = sizeof(metadata),
    };
    foreign_key_list_capture capture = {
        .expected_constraint_name = "fk_posts_users",
        .expected_table_name = "posts",
        .expected_referenced_table_name = "users",
        .expected_column_count = 2U,
    };
    foreign_key_list_capture parent_capture = {
        .expected_constraint_name = "fk_posts_users",
        .expected_table_name = "posts",
        .expected_referenced_table_name = "users",
        .expected_column_count = 2U,
    };
    foreign_key_list_capture renamed_parent_capture = {
        .expected_constraint_name = "fk_posts_users",
        .expected_table_name = "articles",
        .expected_referenced_table_name = "accounts",
        .expected_column_count = 2U,
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &users_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &posts_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_foreign_key_definition(filename, &foreign_key) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_store_foreign_key_definition(filename, &foreign_key) == MYLITE_STORAGE_ERROR
    );
    assert(
        mylite_storage_read_foreign_key_definition(
            filename,
            "app",
            "posts",
            "fk_posts_users",
            &metadata
        ) == MYLITE_STORAGE_OK
    );
    assert_foreign_key_metadata(&metadata, "posts", "users", "PRIMARY");
    mylite_storage_free_foreign_key_metadata(&metadata);

    assert(
        mylite_storage_list_foreign_keys(filename, "app", "posts", collect_foreign_key, &capture) ==
        MYLITE_STORAGE_OK
    );
    assert(capture.count == 1U);
    assert(
        mylite_storage_list_parent_foreign_keys(
            filename,
            "app",
            "users",
            collect_foreign_key,
            &parent_capture
        ) == MYLITE_STORAGE_OK
    );
    assert(parent_capture.count == 1U);

    assert(
        mylite_storage_store_table_definition(filename, &rebuilt_posts_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_rename_table_for_rebuild_backup(
            filename,
            "app",
            "posts",
            "app",
            "posts_backup"
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_table_exists(filename, "app", "posts_backup") == MYLITE_STORAGE_OK);
    capture.count = 0U;
    assert(
        mylite_storage_list_foreign_keys(filename, "app", "posts", collect_foreign_key, &capture) ==
        MYLITE_STORAGE_OK
    );
    assert(capture.count == 1U);
    assert(
        mylite_storage_rename_table(filename, "app", "rebuilt_posts", "app", "posts") ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_drop_table(filename, "app", "posts_backup") == MYLITE_STORAGE_OK);

    assert(
        mylite_storage_store_table_definition(filename, &rebuilt_users_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_rename_table_for_rebuild_backup(
            filename,
            "app",
            "users",
            "app",
            "users_backup"
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_table_exists(filename, "app", "users") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_table_exists(filename, "app", "users_backup") == MYLITE_STORAGE_OK);
    parent_capture.count = 0U;
    assert(
        mylite_storage_list_parent_foreign_keys(
            filename,
            "app",
            "users",
            collect_foreign_key,
            &parent_capture
        ) == MYLITE_STORAGE_OK
    );
    assert(parent_capture.count == 1U);
    assert(
        mylite_storage_rename_table(filename, "app", "rebuilt_users", "app", "users") ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_table_exists(filename, "app", "users") == MYLITE_STORAGE_OK);
    assert(mylite_storage_drop_table(filename, "app", "users_backup") == MYLITE_STORAGE_OK);

    assert(
        mylite_storage_rename_table(filename, "app", "users", "app", "accounts") ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_rename_table(filename, "app", "posts", "app", "articles") ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_read_foreign_key_definition(
            filename,
            "app",
            "posts",
            "fk_posts_users",
            &metadata
        ) == MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_read_foreign_key_definition(
            filename,
            "app",
            "articles",
            "fk_posts_users",
            &metadata
        ) == MYLITE_STORAGE_OK
    );
    assert_foreign_key_metadata(&metadata, "articles", "accounts", "PRIMARY");
    mylite_storage_free_foreign_key_metadata(&metadata);
    assert(
        mylite_storage_list_parent_foreign_keys(
            filename,
            "app",
            "accounts",
            collect_foreign_key,
            &renamed_parent_capture
        ) == MYLITE_STORAGE_OK
    );
    assert(renamed_parent_capture.count == 1U);

    assert(
        mylite_storage_update_foreign_key_referenced_key_name(
            filename,
            "app",
            "articles",
            "fk_posts_users",
            "accounts_id_unique"
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_read_foreign_key_definition(
            filename,
            "app",
            "articles",
            "fk_posts_users",
            &metadata
        ) == MYLITE_STORAGE_OK
    );
    assert_foreign_key_metadata(&metadata, "articles", "accounts", "accounts_id_unique");
    mylite_storage_free_foreign_key_metadata(&metadata);
    renamed_parent_capture.count = 0U;
    assert(
        mylite_storage_list_parent_foreign_keys(
            filename,
            "app",
            "accounts",
            collect_foreign_key,
            &renamed_parent_capture
        ) == MYLITE_STORAGE_OK
    );
    assert(renamed_parent_capture.count == 1U);

    assert(mylite_storage_drop_table(filename, "app", "accounts") == MYLITE_STORAGE_ERROR);
    assert(
        mylite_storage_drop_foreign_key_definition(filename, "app", "articles", "fk_posts_users") ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_read_foreign_key_definition(
            filename,
            "app",
            "articles",
            "fk_posts_users",
            &metadata
        ) == MYLITE_STORAGE_NOTFOUND
    );

    foreign_key.table_name = "articles";
    foreign_key.referenced_table_name = "accounts";
    assert(
        mylite_storage_store_foreign_key_definition(filename, &foreign_key) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_drop_table(filename, "app", "articles") == MYLITE_STORAGE_OK);
    assert(mylite_storage_drop_table(filename, "app", "accounts") == MYLITE_STORAGE_OK);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void assert_foreign_key_metadata(
    const mylite_storage_foreign_key_metadata *metadata,
    const char *table_name,
    const char *referenced_table_name,
    const char *referenced_key_name
) {
    assert(strcmp(metadata->schema_name, "app") == 0);
    assert(strcmp(metadata->table_name, table_name) == 0);
    assert(strcmp(metadata->constraint_name, "fk_posts_users") == 0);
    assert(strcmp(metadata->referenced_schema_name, "app") == 0);
    assert(strcmp(metadata->referenced_table_name, referenced_table_name) == 0);
    assert(strcmp(metadata->referenced_key_name, referenced_key_name) == 0);
    assert(metadata->column_count == 2U);
    assert(strcmp(metadata->foreign_column_names[0], "user_id") == 0);
    assert(strcmp(metadata->foreign_column_names[1], "site_id") == 0);
    assert(strcmp(metadata->referenced_column_names[0], "id") == 0);
    assert(strcmp(metadata->referenced_column_names[1], "site_id") == 0);
    assert(metadata->update_action == MYLITE_STORAGE_FOREIGN_KEY_ACTION_NO_ACTION);
    assert(metadata->delete_action == MYLITE_STORAGE_FOREIGN_KEY_ACTION_RESTRICT);
    assert(metadata->match_option == MYLITE_STORAGE_FOREIGN_KEY_MATCH_SIMPLE);
    assert(metadata->nullable_column_bitmap == 0x1ULL);
    assert(metadata->referenced_nullable_column_bitmap == 0x2ULL);
}

static void test_append_and_read_rows(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char post_row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char post_row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char comment_row[] = {0x00U, 0x03U, 'x', 'y', 'z'};
    char *root = make_temp_root();
    char *filename = path_join(root, "table-rows.mylite");
    mylite_storage_table_definition posts_definition = {
        .size = sizeof(posts_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_table_definition comments_definition = {
        .size = sizeof(comments_definition),
        .schema_name = "app",
        .table_name = "comments",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &posts_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(filename, &comments_definition) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "posts", post_row_1, sizeof(post_row_1)) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "comments", comment_row, sizeof(comment_row)) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "posts", post_row_2, sizeof(post_row_2)) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 5ULL);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert(mylite_storage_count_rows(filename, "app", "comments", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 1ULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert_post_rowset_layout(&rows, sizeof(post_row_1));
    assert(memcmp(rows.rows, post_row_1, sizeof(post_row_1)) == 0);
    assert(memcmp(rows.rows + rows.row_size, post_row_2, sizeof(post_row_2)) == 0);

    mylite_storage_free_rowset(&rows);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_append_and_read_large_row_payload(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char small_row[] = {0x00U, 0x01U, 's'};
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    const size_t large_row_size = (payload_capacity * 2U) + 17U;
    unsigned char *large_row = (unsigned char *)malloc(large_row_size);
    assert(large_row != NULL);
    for (size_t i = 0U; i < large_row_size; ++i) {
        large_row[i] = (unsigned char)(i % UINT8_MAX);
    }

    char *root = make_temp_root();
    char *filename = path_join(root, "large-row-payload.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "payloads",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "payloads", small_row, sizeof(small_row)) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "payloads", large_row, large_row_size) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 6ULL);
    assert(mylite_storage_read_rows(filename, "app", "payloads", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_size == 0U);
    assert(rows.row_count == 2U);
    assert(rows.row_bytes == sizeof(small_row) + large_row_size);
    assert(rows.row_offsets[0] == 0U);
    assert(rows.row_sizes[0] == sizeof(small_row));
    assert(rows.row_ids[0] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 1ULL);
    assert(rows.row_offsets[1] == sizeof(small_row));
    assert(rows.row_sizes[1] == large_row_size);
    assert(rows.row_ids[1] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 5ULL);
    assert(memcmp(rows.rows + rows.row_offsets[0], small_row, sizeof(small_row)) == 0);
    assert(memcmp(rows.rows + rows.row_offsets[1], large_row, large_row_size) == 0);

    mylite_storage_free_rowset(&rows);
    free(large_row);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_update_and_delete_rows(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char row_3[] = {0x00U, 0x03U, 'x', 'y', 'z'};
    static const unsigned char updated_row_1[] = {0x00U, 0x04U, 'u', 'p', 'd', 'a', 't', 'e', 'd'};
    char *root = make_temp_root();
    char *filename = path_join(root, "row-lifecycle.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_count = 0ULL;
    unsigned long long new_row_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row_1, sizeof(row_1)) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "posts", row_2, sizeof(row_2)) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "posts", row_3, sizeof(row_3)) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert_lifecycle_initial_rows(&rows);

    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            rows.row_ids[0],
            updated_row_1,
            sizeof(updated_row_1),
            &new_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(new_row_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 4ULL);
    assert(
        mylite_storage_delete_row(filename, "app", "posts", rows.row_ids[1]) == MYLITE_STORAGE_OK
    );
    assert_row_not_found(filename, rows.row_ids[0]);
    assert_row_not_found(filename, rows.row_ids[1]);
    assert_row_equals(filename, new_row_id, updated_row_1, sizeof(updated_row_1));
    mylite_storage_free_rowset(&rows);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert_lifecycle_live_rows(&rows, new_row_id);
    assert(
        mylite_storage_delete_row(filename, "app", "posts", rows.row_ids[1]) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            rows.row_ids[1],
            row_1,
            sizeof(row_1),
            &new_row_id
        ) == MYLITE_STORAGE_NOTFOUND
    );

    mylite_storage_free_rowset(&rows);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_many_row_state_pages_scan(void) {
    enum { row_count = 180 };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "many-row-states.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned long long original_row_ids[row_count] = {0};
    unsigned long long live_row_ids[row_count] = {0};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (unsigned i = 0U; i < row_count; ++i) {
        const unsigned char row[] = {0x00U, (unsigned char)i, 0x00U, 0x00U};
        assert(
            mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) ==
            MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == row_count);
    for (unsigned i = 0U; i < row_count; ++i) {
        original_row_ids[i] = rows.row_ids[i];
    }
    mylite_storage_free_rowset(&rows);

    for (unsigned i = 0U; i < row_count; ++i) {
        const unsigned char row[] = {0x01U, (unsigned char)i, 0x5aU, 0xa5U};
        assert(
            mylite_storage_update_row(
                filename,
                "app",
                "posts",
                original_row_ids[i],
                row,
                sizeof(row),
                &live_row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }
    for (unsigned i = 0U; i < row_count; i += 3U) {
        assert(
            mylite_storage_delete_row(filename, "app", "posts", live_row_ids[i]) ==
            MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_size == 4U);
    assert(rows.row_count == row_count - (row_count / 3U));
    size_t result_index = 0U;
    for (unsigned i = 0U; i < row_count; ++i) {
        if (i % 3U == 0U) {
            continue;
        }
        const unsigned char expected[] = {0x01U, (unsigned char)i, 0x5aU, 0xa5U};
        assert(rows.row_ids[result_index] == live_row_ids[i]);
        assert(memcmp(rows.rows + (result_index * rows.row_size), expected, sizeof(expected)) == 0);
        ++result_index;
    }
    assert(result_index == rows.row_count);

    mylite_storage_free_rowset(&rows);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_live_row_validation_cache(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x11U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x22U, 'b'};
    static const unsigned char row_3[] = {0x00U, 0x33U, 'c'};
    static const unsigned char row_4[] = {0x00U, 0x44U, 'd'};
    static const unsigned char key_1[] = {0x11U};
    static const unsigned char key_2[] = {0x22U};
    static const unsigned char key_3[] = {0x33U};
    static const unsigned char key_4[] = {0x44U};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-live-row-validation-cache.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 0U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    mylite_storage_index_entry row_3_entry = {
        .size = sizeof(row_3_entry),
        .index_number = 0U,
        .key = key_3,
        .key_size = sizeof(key_3),
    };
    mylite_storage_index_entry row_4_entry = {
        .size = sizeof(row_4_entry),
        .index_number = 0U,
        .key = key_4,
        .key_size = sizeof(key_4),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_4_id = 0ULL;
    unsigned long long rolled_back_row_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            &row_1_entry,
            1U,
            &row_1_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_OK, row_1_id);
    assert_row_equals(filename, row_1_id, row_1, sizeof(row_1));
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1_id,
            row_2,
            sizeof(row_2),
            &row_2_entry,
            1U,
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_2, sizeof(key_2), MYLITE_STORAGE_OK, row_2_id);

    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2_id,
            row_3,
            sizeof(row_3),
            &row_3_entry,
            1U,
            &rolled_back_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert_index_entry_lookup(
        filename,
        0U,
        key_3,
        sizeof(key_3),
        MYLITE_STORAGE_OK,
        rolled_back_row_id
    );
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_index_entry_lookup(filename, 0U, key_3, sizeof(key_3), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_row_equals(filename, row_2_id, row_2, sizeof(row_2));
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2_id,
            row_4,
            sizeof(row_4),
            &row_4_entry,
            1U,
            &row_4_id
        ) == MYLITE_STORAGE_OK
    );
    assert_index_entry_lookup(filename, 0U, key_3, sizeof(key_3), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_4, sizeof(key_4), MYLITE_STORAGE_OK, row_4_id);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    assert_row_not_found(filename, row_1_id);
    assert_row_not_found(filename, row_2_id);
    assert_row_equals(filename, row_4_id, row_4, sizeof(row_4));

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_reusable_live_row_cache_clears_row_ids(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x11U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x22U, 'b'};
    static const unsigned char key_1[] = {0x11U};
    static const unsigned char key_2[] = {0x22U};
    char *root = make_temp_root();
    char *filename = path_join(root, "reusable-live-row-cache-clears-row-ids.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 0U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_id = 0ULL;
    unsigned long long updated_row_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            &row_1_entry,
            1U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_1,
        sizeof(key_1),
        row_id,
        row_1,
        sizeof(row_1)
    );
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_id) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_id,
            row_2,
            sizeof(row_2),
            &row_2_entry,
            1U,
            &updated_row_id
        ) == MYLITE_STORAGE_NOTFOUND
    );
    assert(updated_row_id == 0ULL);
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert_row_not_found(filename, row_id);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_row_payload_cache(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x11U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x22U, 'b'};
    static const unsigned char row_3[] = {0x00U, 0x33U, 'c'};
    static const unsigned char key_1[] = {0x11U};
    static const unsigned char key_2[] = {0x22U};
    static const unsigned char key_3[] = {0x33U};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-row-payload-cache.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 0U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    mylite_storage_index_entry row_3_entry = {
        .size = sizeof(row_3_entry),
        .index_number = 0U,
        .key = key_3,
        .key_size = sizeof(key_3),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_3_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            &row_1_entry,
            1U,
            &row_1_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_1,
        sizeof(key_1),
        row_1_id,
        row_1,
        sizeof(row_1)
    );
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1_id,
            row_2,
            sizeof(row_2),
            &row_2_entry,
            1U,
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert_find_indexed_row_not_found(filename, 0U, key_1, sizeof(key_1));
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_2,
        sizeof(key_2),
        row_2_id,
        row_2,
        sizeof(row_2)
    );

    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2_id,
            row_3,
            sizeof(row_3),
            &row_3_entry,
            1U,
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_3,
        sizeof(key_3),
        row_3_id,
        row_3,
        sizeof(row_3)
    );
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_not_found(filename, 0U, key_3, sizeof(key_3));
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_2,
        sizeof(key_2),
        row_2_id,
        row_2,
        sizeof(row_2)
    );

    assert(mylite_storage_delete_row(filename, "app", "posts", row_2_id) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_not_found(filename, 0U, key_2, sizeof(key_2));
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert_row_not_found(filename, row_1_id);
    assert_row_not_found(filename, row_2_id);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_row_payload_cache_large_window(void) {
    enum { ROW_COUNT = 5000 };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    unsigned char keys[ROW_COUNT][2];
    unsigned char rows[ROW_COUNT][4];
    unsigned long long row_ids[ROW_COUNT] = {0};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-row-payload-cache-large-window.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_statement *transaction = NULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        keys[i][0] = (unsigned char)(i >> CHAR_BIT);
        keys[i][1] = (unsigned char)(i & UINT8_MAX);
        rows[i][0] = 0x00U;
        rows[i][1] = keys[i][0];
        rows[i][2] = keys[i][1];
        rows[i][3] = 0x11U;

        mylite_storage_index_entry row_entry = {
            .size = sizeof(row_entry),
            .index_number = 0U,
            .key = keys[i],
            .key_size = sizeof(keys[i]),
        };
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                rows[i],
                sizeof(rows[i]),
                &row_entry,
                1U,
                &row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        assert_find_indexed_row_equals(
            filename,
            0U,
            keys[i],
            sizeof(keys[i]),
            row_ids[i],
            rows[i],
            sizeof(rows[i])
        );
    }

    assert(row_ids[0] <= (unsigned long long)LONG_MAX / MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    flip_file_byte(
        filename,
        (long)(row_ids[0] * MYLITE_STORAGE_FORMAT_PAGE_SIZE) +
            MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        keys[0],
        sizeof(keys[0]),
        row_ids[0],
        rows[0],
        sizeof(rows[0])
    );

    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_row_payload_cache_validates_update(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x11U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x22U, 'b'};
    static const unsigned char key_1[] = {0x11U};
    static const unsigned char key_2[] = {0x22U};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-row-payload-cache-validates-update.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 0U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    mylite_storage_statement *transaction = NULL;
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            &row_1_entry,
            1U,
            &row_1_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_1,
        sizeof(key_1),
        row_1_id,
        row_1,
        sizeof(row_1)
    );
    assert(row_1_id <= (unsigned long long)LONG_MAX / MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    flip_file_byte(
        filename,
        (long)(row_1_id * MYLITE_STORAGE_FORMAT_PAGE_SIZE) +
            MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET
    );

    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1_id,
            row_2,
            sizeof(row_2),
            &row_2_entry,
            1U,
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert_find_indexed_row_not_found(filename, 0U, key_1, sizeof(key_1));
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_2,
        sizeof(key_2),
        row_2_id,
        row_2,
        sizeof(row_2)
    );

    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_table_entry_cache_catalog_invalidation(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x11U, 'a'};
    static const unsigned char key[] = {0x11U};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-table-entry-cache-catalog-invalidation.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_entry = {
        .size = sizeof(row_entry),
        .index_number = 0U,
        .key = key,
        .key_size = sizeof(key),
    };
    mylite_storage_statement *transaction = NULL;
    unsigned long long row_id = 0ULL;
    unsigned long long found_row_id = 0ULL;
    unsigned char *stored_row = NULL;
    size_t stored_row_size = 0U;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row,
            sizeof(row),
            &row_entry,
            1U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_find_indexed_row(
            filename,
            "app",
            "posts",
            0U,
            key,
            sizeof(key),
            &found_row_id,
            &stored_row,
            &stored_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == row_id);
    assert(stored_row_size == sizeof(row));
    assert(memcmp(stored_row, row, sizeof(row)) == 0);
    mylite_storage_free(stored_row);
    stored_row = NULL;
    stored_row_size = 0U;
    found_row_id = 0ULL;

    assert(
        mylite_storage_rename_table(filename, "app", "posts", "app", "articles") ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_find_indexed_row(
            filename,
            "app",
            "posts",
            0U,
            key,
            sizeof(key),
            &found_row_id,
            &stored_row,
            &stored_row_size
        ) == MYLITE_STORAGE_NOTFOUND
    );
    assert(found_row_id == 0ULL);
    assert(stored_row == NULL);
    assert(stored_row_size == 0U);
    assert(
        mylite_storage_find_indexed_row(
            filename,
            "app",
            "articles",
            0U,
            key,
            sizeof(key),
            &found_row_id,
            &stored_row,
            &stored_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == row_id);
    assert(stored_row_size == sizeof(row));
    assert(memcmp(stored_row, row, sizeof(row)) == 0);
    mylite_storage_free(stored_row);
    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_equals(filename, 0U, key, sizeof(key), row_id, row, sizeof(row));

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_row_payload_cache_many_replacements(void) {
    enum { ROW_COUNT = 160 };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    unsigned char keys[ROW_COUNT][2];
    unsigned char replacement_keys[ROW_COUNT][2];
    unsigned char rows[ROW_COUNT][3];
    unsigned char replacement_rows[ROW_COUNT][4];
    unsigned long long row_ids[ROW_COUNT] = {0};
    unsigned long long replacement_row_ids[ROW_COUNT] = {0};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-row-payload-cache-many-replacements.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_statement *transaction = NULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        keys[i][0] = 0x20U;
        keys[i][1] = (unsigned char)i;
        replacement_keys[i][0] = 0x80U;
        replacement_keys[i][1] = (unsigned char)i;
        rows[i][0] = 0x00U;
        rows[i][1] = (unsigned char)i;
        rows[i][2] = 0x11U;
        replacement_rows[i][0] = 0x00U;
        replacement_rows[i][1] = (unsigned char)i;
        replacement_rows[i][2] = 0x22U;
        replacement_rows[i][3] = 0x33U;

        mylite_storage_index_entry row_entry = {
            .size = sizeof(row_entry),
            .index_number = 0U,
            .key = keys[i],
            .key_size = sizeof(keys[i]),
        };
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                rows[i],
                sizeof(rows[i]),
                &row_entry,
                1U,
                &row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        assert_find_indexed_row_equals(
            filename,
            0U,
            keys[i],
            sizeof(keys[i]),
            row_ids[i],
            rows[i],
            sizeof(rows[i])
        );
    }
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        mylite_storage_index_entry replacement_entry = {
            .size = sizeof(replacement_entry),
            .index_number = 0U,
            .key = replacement_keys[i],
            .key_size = sizeof(replacement_keys[i]),
        };
        assert(
            mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_ids[i],
                replacement_rows[i],
                sizeof(replacement_rows[i]),
                &replacement_entry,
                1U,
                &replacement_row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
        assert_find_indexed_row_not_found(filename, 0U, keys[i], sizeof(keys[i]));
    }
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        assert_find_indexed_row_equals(
            filename,
            0U,
            replacement_keys[i],
            sizeof(replacement_keys[i]),
            replacement_row_ids[i],
            replacement_rows[i],
            sizeof(replacement_rows[i])
        );
    }
    for (size_t i = 0U; i < ROW_COUNT; i += 3U) {
        assert(
            mylite_storage_delete_row(filename, "app", "posts", replacement_row_ids[i]) ==
            MYLITE_STORAGE_OK
        );
        assert_find_indexed_row_not_found(
            filename,
            0U,
            replacement_keys[i],
            sizeof(replacement_keys[i])
        );
    }
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        if (i % 3U == 0U) {
            continue;
        }
        assert_find_indexed_row_equals(
            filename,
            0U,
            replacement_keys[i],
            sizeof(replacement_keys[i]),
            replacement_row_ids[i],
            replacement_rows[i],
            sizeof(replacement_rows[i])
        );
    }
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        if (i % 3U == 0U) {
            assert_row_not_found(filename, replacement_row_ids[i]);
        } else {
            assert_row_equals(
                filename,
                replacement_row_ids[i],
                replacement_rows[i],
                sizeof(replacement_rows[i])
            );
        }
    }

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_durable_live_row_cache(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x11U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x22U, 'b'};
    static const unsigned char row_3[] = {0x00U, 0x33U, 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "durable-live-row-cache.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_3_id = 0ULL;
    unsigned long long row_count = 0ULL;
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            NULL,
            0U,
            &row_1_id
        ) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            NULL,
            0U,
            &row_2_id
        ) ==
        MYLITE_STORAGE_OK
    );

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 2U);
    assert(rows.row_ids[0] == row_1_id);
    assert(rows.row_ids[1] == row_2_id);
    mylite_storage_free_rowset(&rows);

    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_1_id,
            row_3,
            sizeof(row_3),
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 2U);
    assert(rows.row_ids[0] == row_2_id);
    assert(rows.row_ids[1] == row_3_id);
    assert(memcmp(rows.rows + rows.row_offsets[0], row_2, sizeof(row_2)) == 0);
    assert(memcmp(rows.rows + rows.row_offsets[1], row_3, sizeof(row_3)) == 0);
    mylite_storage_free_rowset(&rows);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_2_id) == MYLITE_STORAGE_OK);
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 1ULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    assert(rows.row_ids[0] == row_3_id);
    assert(memcmp(rows.rows + rows.row_offsets[0], row_3, sizeof(row_3)) == 0);
    mylite_storage_free_rowset(&rows);

    assert(mylite_storage_truncate_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 0ULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    mylite_storage_free_rowset(&rows);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_deferred_durable_cache_retarget(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x11U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x22U, 'b'};
    static const unsigned char row_3[] = {0x00U, 0x33U, 'c'};
    static const unsigned char row_4[] = {0x00U, 0x44U, 'd'};
    static const unsigned char row_5[] = {0x00U, 0x55U, 'e'};
    static const unsigned char row_6[] = {0x00U, 0x66U, 'f'};
    char *root = make_temp_root();
    char *filename = path_join(root, "deferred-durable-cache-retarget.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_3_id = 0ULL;
    unsigned long long row_4_id = 0ULL;
    unsigned long long row_5_id = 0ULL;
    unsigned long long row_6_id = 0ULL;
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            NULL,
            0U,
            &row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            NULL,
            0U,
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_1_id,
            row_3,
            sizeof(row_3),
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert_row_not_found(filename, row_1_id);
    assert_row_equals(filename, row_2_id, row_2, sizeof(row_2));
    assert_row_equals(filename, row_3_id, row_3, sizeof(row_3));

    transaction = NULL;
    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_2_id,
            row_4,
            sizeof(row_4),
            &row_4_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert_row_equals(filename, row_2_id, row_2, sizeof(row_2));
    assert_row_equals(filename, row_3_id, row_3, sizeof(row_3));
    assert(row_4_id != 0ULL);
    assert_row_not_found(filename, row_4_id);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    transaction = NULL;
    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_2_id,
            row_5,
            sizeof(row_5),
            &row_5_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_commit_statement(savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert_row_not_found(filename, row_2_id);
    assert_row_equals(filename, row_3_id, row_3, sizeof(row_3));
    assert_row_equals(filename, row_5_id, row_5, sizeof(row_5));

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    transaction = NULL;
    savepoint = NULL;
    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_3_id,
            row_6,
            sizeof(row_6),
            &row_6_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert_row_equals(filename, row_3_id, row_3, sizeof(row_3));
    assert_row_equals(filename, row_5_id, row_5, sizeof(row_5));
    assert(row_6_id != 0ULL);
    assert_row_not_found(filename, row_6_id);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_live_row_list_maintenance(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x11U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x22U, 'b'};
    static const unsigned char row_3[] = {0x00U, 0x33U, 'c'};
    static const unsigned char row_4[] = {0x00U, 0x44U, 'd'};
    static const unsigned char row_5[] = {0x00U, 0x55U, 'e'};
    static const unsigned char row_6[] = {0x00U, 0x66U, 'f'};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-live-row-list-cache.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_3_id = 0ULL;
    unsigned long long row_4_id = 0ULL;
    unsigned long long row_5_id = 0ULL;
    unsigned long long row_6_id = 0ULL;
    unsigned long long row_count = 0ULL;
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            NULL,
            0U,
            &row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            NULL,
            0U,
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_1_id,
            row_3,
            sizeof(row_3),
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_delete_row(filename, "app", "posts", row_2_id) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_4,
            sizeof(row_4),
            NULL,
            0U,
            &row_4_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 2U);
    assert(rows.row_ids[0] == row_3_id);
    assert(rows.row_ids[1] == row_4_id);
    assert(memcmp(rows.rows + rows.row_offsets[0], row_3, sizeof(row_3)) == 0);
    assert(memcmp(rows.rows + rows.row_offsets[1], row_4, sizeof(row_4)) == 0);
    mylite_storage_free_rowset(&rows);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_3_id,
            row_5,
            sizeof(row_5),
            &row_5_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_4_id,
            row_6,
            sizeof(row_6),
            &row_6_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 2U);
    assert(rows.row_ids[0] == row_4_id);
    assert(rows.row_ids[1] == row_5_id);
    assert(memcmp(rows.rows + rows.row_offsets[0], row_4, sizeof(row_4)) == 0);
    assert(memcmp(rows.rows + rows.row_offsets[1], row_5, sizeof(row_5)) == 0);
    mylite_storage_free_rowset(&rows);
    assert_row_not_found(filename, row_6_id);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_index_entries(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char updated_row_1[] = {0x00U, 0x09U, 'u', 'p', 'd'};
    static const unsigned char key_1[] = {0x01U};
    static const unsigned char key_2[] = {0x02U};
    static const unsigned char key_9[] = {0x09U};
    static const unsigned char title_a[] = {'a'};
    static const unsigned char title_u[] = {'u'};
    char *root = make_temp_root();
    char *filename = path_join(root, "index-entries.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entries[] = {
        {.size = sizeof(row_1_entries[0]),
         .index_number = 0U,
         .key = key_1,
         .key_size = sizeof(key_1)},
        {.size = sizeof(row_1_entries[1]),
         .index_number = 1U,
         .key = title_a,
         .key_size = sizeof(title_a)},
    };
    mylite_storage_index_entry row_2_entries[] = {
        {.size = sizeof(row_2_entries[0]),
         .index_number = 0U,
         .key = key_2,
         .key_size = sizeof(key_2)},
        {.size = sizeof(row_2_entries[1]),
         .index_number = 1U,
         .key = title_a,
         .key_size = sizeof(title_a)},
    };
    mylite_storage_index_entry update_entries[] = {
        {.size = sizeof(update_entries[0]),
         .index_number = 0U,
         .key = key_9,
         .key_size = sizeof(key_9)},
        {.size = sizeof(update_entries[1]),
         .index_number = 1U,
         .key = title_u,
         .key_size = sizeof(title_u)},
    };
    index_entries_test_context ctx = {
        .filename = filename,
        .row_1 = row_1,
        .row_1_size = sizeof(row_1),
        .row_2 = row_2,
        .row_2_size = sizeof(row_2),
        .updated_row_1 = updated_row_1,
        .updated_row_1_size = sizeof(updated_row_1),
        .row_1_entries = row_1_entries,
        .row_1_entry_count = sizeof(row_1_entries) / sizeof(row_1_entries[0]),
        .row_2_entries = row_2_entries,
        .row_2_entry_count = sizeof(row_2_entries) / sizeof(row_2_entries[0]),
        .update_entries = update_entries,
        .update_entry_count = sizeof(update_entries) / sizeof(update_entries[0]),
        .key_1 = key_1,
        .key_1_size = sizeof(key_1),
        .key_2 = key_2,
        .key_2_size = sizeof(key_2),
        .key_9 = key_9,
        .key_9_size = sizeof(key_9),
        .title_u = title_u,
        .title_u_size = sizeof(title_u),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    append_index_entry_test_rows(&ctx);
    const unsigned long long title_a_row_ids[] = {ctx.row_1_id, ctx.row_2_id};
    assert_primary_index_entries_after_insert(&ctx);
    assert_index_prefix_exists(filename, key_1, sizeof(key_1), 1);
    assert_index_prefix_exists(filename, key_9, sizeof(key_9), 0);
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_OK, ctx.row_1_id);
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_1,
        sizeof(key_1),
        ctx.row_1_id,
        row_1,
        sizeof(row_1)
    );
    unsigned long long reusable_row_id = 0ULL;
    unsigned char *reusable_row = NULL;
    size_t reusable_row_capacity = 0U;
    size_t reusable_row_size = 0U;
    assert(
        mylite_storage_find_indexed_row_reuse(
            filename,
            "app",
            "posts",
            0U,
            key_1,
            sizeof(key_1),
            &reusable_row_id,
            &reusable_row,
            &reusable_row_capacity,
            &reusable_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(reusable_row_id == ctx.row_1_id);
    assert(reusable_row != NULL);
    assert(reusable_row_capacity >= sizeof(row_1));
    assert(reusable_row_size == sizeof(row_1));
    assert(memcmp(reusable_row, row_1, sizeof(row_1)) == 0);
    unsigned char *const first_reusable_row = reusable_row;
    assert(
        mylite_storage_find_indexed_row_reuse(
            filename,
            "app",
            "posts",
            0U,
            key_2,
            sizeof(key_2),
            &reusable_row_id,
            &reusable_row,
            &reusable_row_capacity,
            &reusable_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(reusable_row_id == ctx.row_2_id);
    assert(reusable_row == first_reusable_row);
    assert(reusable_row_size == sizeof(row_2));
    assert(memcmp(reusable_row, row_2, sizeof(row_2)) == 0);
    assert(
        mylite_storage_find_indexed_row_reuse(
            filename,
            "app",
            "posts",
            0U,
            key_9,
            sizeof(key_9),
            &reusable_row_id,
            &reusable_row,
            &reusable_row_capacity,
            &reusable_row_size
        ) == MYLITE_STORAGE_NOTFOUND
    );
    assert(reusable_row_id == 0ULL);
    assert(reusable_row == first_reusable_row);
    assert(reusable_row_size == 0U);
    mylite_storage_free(reusable_row);
    assert_indexed_row_equals(filename, ctx.row_1_id, row_1, sizeof(row_1));
    assert_index_entry_lookup(filename, 0U, key_2, sizeof(key_2), MYLITE_STORAGE_OK, ctx.row_2_id);
    assert_index_entry_lookup(filename, 0U, key_9, sizeof(key_9), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_9, sizeof(key_9));
    assert_exact_index_entries(
        filename,
        1U,
        title_a,
        sizeof(title_a),
        title_a_row_ids,
        sizeof(title_a_row_ids) / sizeof(title_a_row_ids[0])
    );
    const unsigned char *const title_a_rows[] = {row_1, row_2};
    const size_t title_a_row_sizes[] = {sizeof(row_1), sizeof(row_2)};
    assert_indexed_rows_equal(
        filename,
        title_a_row_ids,
        sizeof(title_a_row_ids) / sizeof(title_a_row_ids[0]),
        title_a_rows,
        title_a_row_sizes
    );
    assert_exact_index_entries(filename, 0U, key_9, sizeof(key_9), NULL, 0U);
    update_index_entry_test_row(&ctx);
    const unsigned long long title_a_after_update_row_ids[] = {ctx.row_2_id};
    const unsigned long long key_9_row_ids[] = {ctx.updated_row_1_id};
    assert_primary_index_entries_after_update(&ctx);
    assert_index_prefix_exists(filename, key_1, sizeof(key_1), 0);
    assert_index_prefix_exists(filename, key_9, sizeof(key_9), 1);
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_1, sizeof(key_1));
    assert_index_entry_lookup(
        filename,
        1U,
        title_a,
        sizeof(title_a),
        MYLITE_STORAGE_OK,
        ctx.row_2_id
    );
    assert_index_entry_lookup(
        filename,
        0U,
        key_9,
        sizeof(key_9),
        MYLITE_STORAGE_OK,
        ctx.updated_row_1_id
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_9,
        sizeof(key_9),
        ctx.updated_row_1_id,
        updated_row_1,
        sizeof(updated_row_1)
    );
    assert_exact_index_entries(
        filename,
        1U,
        title_a,
        sizeof(title_a),
        title_a_after_update_row_ids,
        sizeof(title_a_after_update_row_ids) / sizeof(title_a_after_update_row_ids[0])
    );
    assert_exact_index_entries(
        filename,
        0U,
        key_9,
        sizeof(key_9),
        key_9_row_ids,
        sizeof(key_9_row_ids) / sizeof(key_9_row_ids[0])
    );
    delete_index_entry_test_row(&ctx);
    const unsigned long long title_u_row_ids[] = {ctx.updated_row_1_id};
    assert_secondary_index_entries_after_delete(&ctx);
    assert_index_prefix_exists(filename, key_2, sizeof(key_2), 0);
    assert_index_prefix_exists(filename, title_u, sizeof(title_u), 1);
    assert_index_entry_lookup(filename, 0U, key_2, sizeof(key_2), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_2, sizeof(key_2));
    assert_index_entry_lookup(
        filename,
        1U,
        title_u,
        sizeof(title_u),
        MYLITE_STORAGE_OK,
        ctx.updated_row_1_id
    );
    assert_find_indexed_row_equals(
        filename,
        1U,
        title_u,
        sizeof(title_u),
        ctx.updated_row_1_id,
        updated_row_1,
        sizeof(updated_row_1)
    );
    assert_exact_index_entries(filename, 0U, key_2, sizeof(key_2), NULL, 0U);
    assert_exact_index_entries(filename, 1U, title_a, sizeof(title_a), NULL, 0U);
    assert_exact_index_entries(
        filename,
        1U,
        title_u,
        sizeof(title_u),
        title_u_row_ids,
        sizeof(title_u_row_ids) / sizeof(title_u_row_ids[0])
    );
    assert_index_entry_test_live_rows(&ctx);

    assert(
        mylite_storage_find_index_entry(filename, "app", "posts", 0U, NULL, sizeof(key_1), NULL) ==
        MYLITE_STORAGE_MISUSE
    );
    unsigned long long misuse_row_id = 0ULL;
    unsigned char *misuse_row = NULL;
    size_t misuse_row_size = 0U;
    assert(
        mylite_storage_find_indexed_row(
            filename,
            "app",
            "posts",
            0U,
            NULL,
            sizeof(key_1),
            &misuse_row_id,
            &misuse_row,
            &misuse_row_size
        ) == MYLITE_STORAGE_MISUSE
    );
    mylite_storage_index_entryset misuse_entries = {
        .size = sizeof(misuse_entries),
    };
    assert(
        mylite_storage_read_exact_index_entries(
            filename,
            "app",
            "posts",
            0U,
            NULL,
            sizeof(key_1),
            &misuse_entries
        ) == MYLITE_STORAGE_MISUSE
    );
    mylite_storage_rowset misuse_rows = {
        .size = sizeof(misuse_rows),
    };
    assert(
        mylite_storage_read_indexed_rows(filename, "app", "posts", NULL, 1U, &misuse_rows) ==
        MYLITE_STORAGE_MISUSE
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_exact_index_cache_fixed_size_keys(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x41U, 0x42U};
    static const unsigned char key_1[] = {0x1aU};
    static const unsigned char missing_key_1[] = {0x1bU};
    static const unsigned char key_2[] = {0x2aU, 0x2bU};
    static const unsigned char missing_key_2[] = {0x2aU, 0x2cU};
    static const unsigned char key_4[] = {0x4aU, 0x4bU, 0x4cU, 0x4dU};
    static const unsigned char missing_key_4[] = {0x4aU, 0x4bU, 0x4cU, 0x4eU};
    static const unsigned char key_8[] = {
        0x8aU,
        0x8bU,
        0x8cU,
        0x8dU,
        0x8eU,
        0x8fU,
        0x90U,
        0x91U,
    };
    static const unsigned char missing_key_8[] = {
        0x8aU,
        0x8bU,
        0x8cU,
        0x8dU,
        0x8eU,
        0x8fU,
        0x90U,
        0x92U,
    };
    char *root = make_temp_root();
    char *filename = path_join(root, "exact-index-cache-fixed-size-keys.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry entries[] = {
        {.size = sizeof(entries[0]), .index_number = 0U, .key = key_1, .key_size = sizeof(key_1)},
        {.size = sizeof(entries[1]), .index_number = 1U, .key = key_2, .key_size = sizeof(key_2)},
        {.size = sizeof(entries[2]), .index_number = 2U, .key = key_4, .key_size = sizeof(key_4)},
        {.size = sizeof(entries[3]), .index_number = 3U, .key = key_8, .key_size = sizeof(key_8)},
    };
    unsigned long long row_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row,
            sizeof(row),
            entries,
            sizeof(entries) / sizeof(entries[0]),
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    const unsigned long long expected_row_ids[] = {row_id};

    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_OK, row_id);
    assert_index_entry_lookup(
        filename,
        0U,
        missing_key_1,
        sizeof(missing_key_1),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_exact_index_entries(filename, 0U, key_1, sizeof(key_1), expected_row_ids, 1U);
    assert_exact_index_entries(filename, 0U, missing_key_1, sizeof(missing_key_1), NULL, 0U);

    assert_index_entry_lookup(filename, 1U, key_2, sizeof(key_2), MYLITE_STORAGE_OK, row_id);
    assert_index_entry_lookup(
        filename,
        1U,
        missing_key_2,
        sizeof(missing_key_2),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_exact_index_entries(filename, 1U, key_2, sizeof(key_2), expected_row_ids, 1U);
    assert_exact_index_entries(filename, 1U, missing_key_2, sizeof(missing_key_2), NULL, 0U);

    assert_index_entry_lookup(filename, 2U, key_4, sizeof(key_4), MYLITE_STORAGE_OK, row_id);
    assert_index_entry_lookup(
        filename,
        2U,
        missing_key_4,
        sizeof(missing_key_4),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_exact_index_entries(filename, 2U, key_4, sizeof(key_4), expected_row_ids, 1U);
    assert_exact_index_entries(filename, 2U, missing_key_4, sizeof(missing_key_4), NULL, 0U);

    assert_index_entry_lookup(filename, 3U, key_8, sizeof(key_8), MYLITE_STORAGE_OK, row_id);
    assert_index_entry_lookup(
        filename,
        3U,
        missing_key_8,
        sizeof(missing_key_8),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_exact_index_entries(filename, 3U, key_8, sizeof(key_8), expected_row_ids, 1U);
    assert_exact_index_entries(filename, 3U, missing_key_8, sizeof(missing_key_8), NULL, 0U);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_cached_exact_index_entryset_bulk_append(void) {
    enum { DUPLICATE_ENTRY_COUNT = 48 };

    const size_t duplicate_entry_count = (size_t)DUPLICATE_ENTRY_COUNT;
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char duplicate_key[] = {'d', 'u', 'p'};
    static const unsigned char missing_key[] = {'m', 'i', 's'};
    char *root = make_temp_root();
    char *filename = path_join(root, "cached-exact-entryset.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned long long expected_row_ids[DUPLICATE_ENTRY_COUNT] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < duplicate_entry_count; ++i) {
        const unsigned char row[] = {
            0x00U,
            (unsigned char)i,
            (unsigned char)(i >> 8U),
        };
        const mylite_storage_index_entry secondary_entry = {
            .size = sizeof(secondary_entry),
            .index_number = 1U,
            .key = duplicate_key,
            .key_size = sizeof(duplicate_key),
        };
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row,
                sizeof(row),
                &secondary_entry,
                1U,
                &expected_row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }

    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_exact_index_entries(
            filename,
            "app",
            "posts",
            1U,
            duplicate_key,
            sizeof(duplicate_key),
            &entries
        ) == MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == duplicate_entry_count);
    assert(entries.key_bytes == duplicate_entry_count * sizeof(duplicate_key));
    for (size_t i = 0U; i < duplicate_entry_count; ++i) {
        assert_index_entry(&entries, i, expected_row_ids[i], duplicate_key, sizeof(duplicate_key));
    }
    mylite_storage_free_index_entryset(&entries);

    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_exact_index_entries(
            filename,
            "app",
            "posts",
            1U,
            duplicate_key,
            sizeof(duplicate_key),
            &entries
        ) == MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == duplicate_entry_count);
    assert(entries.key_bytes == duplicate_entry_count * sizeof(duplicate_key));
    for (size_t i = 0U; i < duplicate_entry_count; ++i) {
        assert_index_entry(&entries, i, expected_row_ids[i], duplicate_key, sizeof(duplicate_key));
    }
    mylite_storage_free_index_entryset(&entries);

    assert_index_entry_lookup(
        filename,
        1U,
        duplicate_key,
        sizeof(duplicate_key),
        MYLITE_STORAGE_OK,
        expected_row_ids[0]
    );
    assert_exact_index_entries(filename, 1U, missing_key, sizeof(missing_key), NULL, 0U);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_exact_index_cache_many_replacements(void) {
    enum { ROW_COUNT = 96 };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char old_secondary_key[] = {'o', 'l', 'd'};
    static const unsigned char new_secondary_key[] = {'n', 'e', 'w'};
    static const unsigned char final_secondary_key[] = {'f', 'i', 'n'};
    unsigned char primary_keys[ROW_COUNT][2];
    unsigned char rows[ROW_COUNT][3];
    unsigned char replacement_rows[ROW_COUNT][4];
    unsigned char final_rows[ROW_COUNT][4];
    unsigned long long row_ids[ROW_COUNT] = {0};
    unsigned long long replacement_row_ids[ROW_COUNT] = {0};
    unsigned long long final_row_ids[ROW_COUNT] = {0};
    unsigned long long live_final_row_ids[ROW_COUNT] = {0};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-exact-index-cache-many-replacements.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_statement *transaction = NULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        primary_keys[i][0] = 0x30U;
        primary_keys[i][1] = (unsigned char)i;
        rows[i][0] = 0x00U;
        rows[i][1] = (unsigned char)i;
        rows[i][2] = 0x41U;
        replacement_rows[i][0] = 0x00U;
        replacement_rows[i][1] = (unsigned char)i;
        replacement_rows[i][2] = 0x42U;
        replacement_rows[i][3] = 0x43U;
        final_rows[i][0] = 0x00U;
        final_rows[i][1] = (unsigned char)i;
        final_rows[i][2] = 0x44U;
        final_rows[i][3] = 0x45U;
        mylite_storage_index_entry row_entries[] = {
            {.size = sizeof(row_entries[0]),
             .index_number = 0U,
             .key = primary_keys[i],
             .key_size = sizeof(primary_keys[i])},
            {.size = sizeof(row_entries[1]),
             .index_number = 1U,
             .key = old_secondary_key,
             .key_size = sizeof(old_secondary_key)},
        };
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                rows[i],
                sizeof(rows[i]),
                row_entries,
                sizeof(row_entries) / sizeof(row_entries[0]),
                &row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert_exact_index_entries(
        filename,
        1U,
        old_secondary_key,
        sizeof(old_secondary_key),
        row_ids,
        ROW_COUNT
    );
    assert_index_entry_lookup(
        filename,
        1U,
        old_secondary_key,
        sizeof(old_secondary_key),
        MYLITE_STORAGE_OK,
        row_ids[0]
    );
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        mylite_storage_index_entry replacement_entries[] = {
            {.size = sizeof(replacement_entries[0]),
             .index_number = 0U,
             .key = primary_keys[i],
             .key_size = sizeof(primary_keys[i])},
            {.size = sizeof(replacement_entries[1]),
             .index_number = 1U,
             .key = new_secondary_key,
             .key_size = sizeof(new_secondary_key)},
        };
        assert(
            mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_ids[i],
                replacement_rows[i],
                sizeof(replacement_rows[i]),
                replacement_entries,
                sizeof(replacement_entries) / sizeof(replacement_entries[0]),
                &replacement_row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }
    assert_index_entry_lookup(
        filename,
        1U,
        old_secondary_key,
        sizeof(old_secondary_key),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_index_entry_lookup(
        filename,
        1U,
        new_secondary_key,
        sizeof(new_secondary_key),
        MYLITE_STORAGE_OK,
        replacement_row_ids[0]
    );
    assert_exact_index_entries(
        filename,
        1U,
        old_secondary_key,
        sizeof(old_secondary_key),
        NULL,
        0U
    );
    assert_exact_index_entries(
        filename,
        1U,
        new_secondary_key,
        sizeof(new_secondary_key),
        replacement_row_ids,
        ROW_COUNT
    );

    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        mylite_storage_index_entry replacement_entries[] = {
            {.size = sizeof(replacement_entries[0]),
             .index_number = 0U,
             .key = primary_keys[i],
             .key_size = sizeof(primary_keys[i])},
            {.size = sizeof(replacement_entries[1]),
             .index_number = 1U,
             .key = final_secondary_key,
             .key_size = sizeof(final_secondary_key)},
        };
        assert(
            mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                replacement_row_ids[i],
                final_rows[i],
                sizeof(final_rows[i]),
                replacement_entries,
                sizeof(replacement_entries) / sizeof(replacement_entries[0]),
                &final_row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }
    assert_index_entry_lookup(
        filename,
        1U,
        new_secondary_key,
        sizeof(new_secondary_key),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_index_entry_lookup(
        filename,
        1U,
        final_secondary_key,
        sizeof(final_secondary_key),
        MYLITE_STORAGE_OK,
        final_row_ids[0]
    );
    assert_exact_index_entries(
        filename,
        1U,
        new_secondary_key,
        sizeof(new_secondary_key),
        NULL,
        0U
    );
    assert_exact_index_entries(
        filename,
        1U,
        final_secondary_key,
        sizeof(final_secondary_key),
        final_row_ids,
        ROW_COUNT
    );

    size_t live_count = 0U;
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        if (i % 4U == 0U) {
            assert(
                mylite_storage_delete_row(filename, "app", "posts", final_row_ids[i]) ==
                MYLITE_STORAGE_OK
            );
            continue;
        }
        live_final_row_ids[live_count++] = final_row_ids[i];
    }
    assert_index_entry_lookup(
        filename,
        1U,
        final_secondary_key,
        sizeof(final_secondary_key),
        MYLITE_STORAGE_OK,
        final_row_ids[1]
    );
    assert_exact_index_entries(
        filename,
        1U,
        final_secondary_key,
        sizeof(final_secondary_key),
        live_final_row_ids,
        live_count
    );
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    assert_exact_index_entries(
        filename,
        1U,
        final_secondary_key,
        sizeof(final_secondary_key),
        live_final_row_ids,
        live_count
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_large_append_buffer_savepoint_rollback(void) {
    enum { ROW_COUNT = 560, SAVEPOINT_UPDATE_COUNT = 24 };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    unsigned char primary_keys[ROW_COUNT][3];
    unsigned char initial_secondary_keys[ROW_COUNT][3];
    unsigned char middle_secondary_keys[ROW_COUNT][3];
    unsigned char final_secondary_keys[SAVEPOINT_UPDATE_COUNT][3];
    unsigned char initial_rows[ROW_COUNT][4];
    unsigned char middle_rows[ROW_COUNT][4];
    unsigned char final_rows[SAVEPOINT_UPDATE_COUNT][4];
    unsigned long long row_ids[ROW_COUNT] = {0};
    unsigned long long middle_row_ids[ROW_COUNT] = {0};
    unsigned long long final_row_ids[SAVEPOINT_UPDATE_COUNT] = {0};
    char *root = make_temp_root();
    char *filename = path_join(root, "large-append-buffer-savepoint-rollback.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        primary_keys[i][0] = 0x10U;
        primary_keys[i][1] = (unsigned char)(i >> 8U);
        primary_keys[i][2] = (unsigned char)i;
        initial_secondary_keys[i][0] = 0x20U;
        initial_secondary_keys[i][1] = (unsigned char)(i >> 8U);
        initial_secondary_keys[i][2] = (unsigned char)i;
        middle_secondary_keys[i][0] = 0x40U;
        middle_secondary_keys[i][1] = (unsigned char)(i >> 8U);
        middle_secondary_keys[i][2] = (unsigned char)i;
        initial_rows[i][0] = 0x00U;
        initial_rows[i][1] = (unsigned char)(i >> 8U);
        initial_rows[i][2] = (unsigned char)i;
        initial_rows[i][3] = 0x21U;
        middle_rows[i][0] = 0x00U;
        middle_rows[i][1] = (unsigned char)(i >> 8U);
        middle_rows[i][2] = (unsigned char)i;
        middle_rows[i][3] = 0x41U;

        mylite_storage_index_entry entries[] = {
            {.size = sizeof(entries[0]),
             .index_number = 0U,
             .key = primary_keys[i],
             .key_size = sizeof(primary_keys[i])},
            {.size = sizeof(entries[1]),
             .index_number = 1U,
             .key = initial_secondary_keys[i],
             .key_size = sizeof(initial_secondary_keys[i])},
        };
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                initial_rows[i],
                sizeof(initial_rows[i]),
                entries,
                sizeof(entries) / sizeof(entries[0]),
                &row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < ROW_COUNT; ++i) {
        mylite_storage_index_entry replacement_entries[] = {
            {.size = sizeof(replacement_entries[0]),
             .index_number = 0U,
             .key = primary_keys[i],
             .key_size = sizeof(primary_keys[i])},
            {.size = sizeof(replacement_entries[1]),
             .index_number = 1U,
             .key = middle_secondary_keys[i],
             .key_size = sizeof(middle_secondary_keys[i])},
        };
        assert(
            mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_ids[i],
                middle_rows[i],
                sizeof(middle_rows[i]),
                replacement_entries,
                sizeof(replacement_entries) / sizeof(replacement_entries[0]),
                &middle_row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }

    assert_find_indexed_row_not_found(
        filename,
        1U,
        initial_secondary_keys[0],
        sizeof(initial_secondary_keys[0])
    );
    assert_find_indexed_row_equals(
        filename,
        1U,
        middle_secondary_keys[ROW_COUNT - 1U],
        sizeof(middle_secondary_keys[ROW_COUNT - 1U]),
        middle_row_ids[ROW_COUNT - 1U],
        middle_rows[ROW_COUNT - 1U],
        sizeof(middle_rows[ROW_COUNT - 1U])
    );

    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < SAVEPOINT_UPDATE_COUNT; ++i) {
        final_secondary_keys[i][0] = 0x60U;
        final_secondary_keys[i][1] = (unsigned char)(i >> 8U);
        final_secondary_keys[i][2] = (unsigned char)i;
        final_rows[i][0] = 0x00U;
        final_rows[i][1] = (unsigned char)(i >> 8U);
        final_rows[i][2] = (unsigned char)i;
        final_rows[i][3] = 0x61U;
        mylite_storage_index_entry replacement_entries[] = {
            {.size = sizeof(replacement_entries[0]),
             .index_number = 0U,
             .key = primary_keys[i],
             .key_size = sizeof(primary_keys[i])},
            {.size = sizeof(replacement_entries[1]),
             .index_number = 1U,
             .key = final_secondary_keys[i],
             .key_size = sizeof(final_secondary_keys[i])},
        };
        assert(
            mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                middle_row_ids[i],
                final_rows[i],
                sizeof(final_rows[i]),
                replacement_entries,
                sizeof(replacement_entries) / sizeof(replacement_entries[0]),
                &final_row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }
    assert_find_indexed_row_equals(
        filename,
        1U,
        final_secondary_keys[SAVEPOINT_UPDATE_COUNT - 1U],
        sizeof(final_secondary_keys[SAVEPOINT_UPDATE_COUNT - 1U]),
        final_row_ids[SAVEPOINT_UPDATE_COUNT - 1U],
        final_rows[SAVEPOINT_UPDATE_COUNT - 1U],
        sizeof(final_rows[SAVEPOINT_UPDATE_COUNT - 1U])
    );

    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_not_found(
        filename,
        1U,
        final_secondary_keys[SAVEPOINT_UPDATE_COUNT - 1U],
        sizeof(final_secondary_keys[SAVEPOINT_UPDATE_COUNT - 1U])
    );
    assert_find_indexed_row_equals(
        filename,
        1U,
        middle_secondary_keys[SAVEPOINT_UPDATE_COUNT - 1U],
        sizeof(middle_secondary_keys[SAVEPOINT_UPDATE_COUNT - 1U]),
        middle_row_ids[SAVEPOINT_UPDATE_COUNT - 1U],
        middle_rows[SAVEPOINT_UPDATE_COUNT - 1U],
        sizeof(middle_rows[SAVEPOINT_UPDATE_COUNT - 1U])
    );
    assert_file_size_matches_header(filename);

    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == ROW_COUNT);
    assert_find_indexed_row_equals(
        filename,
        1U,
        middle_secondary_keys[ROW_COUNT - 1U],
        sizeof(middle_secondary_keys[ROW_COUNT - 1U]),
        middle_row_ids[ROW_COUNT - 1U],
        middle_rows[ROW_COUNT - 1U],
        sizeof(middle_rows[ROW_COUNT - 1U])
    );
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_update_rewrite(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char primary_key[] = {0x10U, 0x01U};
    static const unsigned char initial_secondary_key[] = {0x20U, 0x01U};
    static const unsigned char middle_secondary_key[] = {0x30U, 0x01U};
    static const unsigned char final_secondary_key[] = {0x40U, 0x01U};
    static const unsigned char savepoint_secondary_key[] = {0x50U, 0x01U};
    static const unsigned char initial_row[] = {0x01U, 'i', 'n', 'i', 't'};
    static const unsigned char middle_row[] = {0x02U, 'm', 'i', 'd'};
    static const unsigned char final_row[] = {0x03U, 'f', 'i', 'n', 'a', 'l'};
    static const unsigned char savepoint_row[] = {0x04U, 's', 'p'};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-update-rewrite.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry initial_entries[] = {
        {.size = sizeof(initial_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(initial_entries[1]),
         .index_number = 1U,
         .key = initial_secondary_key,
         .key_size = sizeof(initial_secondary_key)},
    };
    mylite_storage_index_entry middle_entries[] = {
        {.size = sizeof(middle_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(middle_entries[1]),
         .index_number = 1U,
         .key = middle_secondary_key,
         .key_size = sizeof(middle_secondary_key)},
    };
    mylite_storage_index_entry final_entries[] = {
        {.size = sizeof(final_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(final_entries[1]),
         .index_number = 1U,
         .key = final_secondary_key,
         .key_size = sizeof(final_secondary_key)},
    };
    mylite_storage_index_entry savepoint_entries[] = {
        {.size = sizeof(savepoint_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(savepoint_entries[1]),
         .index_number = 1U,
         .key = savepoint_secondary_key,
         .key_size = sizeof(savepoint_secondary_key)},
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *prior_statement = NULL;
    mylite_storage_statement *outer_savepoint = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_id = 0ULL;
    unsigned long long middle_row_id = 0ULL;
    unsigned long long final_row_id = 0ULL;
    unsigned long long savepoint_row_id = 0ULL;
    unsigned long long row_count = 0ULL;
    mylite_storage_header before_savepoint_update_header = {
        .size = sizeof(before_savepoint_update_header),
    };
    mylite_storage_header after_savepoint_update_header = {
        .size = sizeof(after_savepoint_update_header),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            initial_row,
            sizeof(initial_row),
            initial_entries,
            sizeof(initial_entries) / sizeof(initial_entries[0]),
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert_find_indexed_row_equals(
        filename,
        1U,
        initial_secondary_key,
        sizeof(initial_secondary_key),
        row_id,
        initial_row,
        sizeof(initial_row)
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert_index_entry_lookup(
        filename,
        1U,
        initial_secondary_key,
        sizeof(initial_secondary_key),
        MYLITE_STORAGE_OK,
        row_id
    );
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_id,
            middle_row,
            sizeof(middle_row),
            middle_entries,
            sizeof(middle_entries) / sizeof(middle_entries[0]),
            &middle_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(middle_row_id != row_id);
    assert_find_indexed_row_equals(
        filename,
        1U,
        middle_secondary_key,
        sizeof(middle_secondary_key),
        middle_row_id,
        middle_row,
        sizeof(middle_row)
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        middle_row_id,
        middle_row,
        sizeof(middle_row)
    );
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            middle_row_id,
            final_row,
            sizeof(final_row),
            final_entries,
            sizeof(final_entries) / sizeof(final_entries[0]),
            &final_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(final_row_id == middle_row_id);
    assert_index_entry_lookup(
        filename,
        1U,
        initial_secondary_key,
        sizeof(initial_secondary_key),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_index_entry_lookup(
        filename,
        1U,
        middle_secondary_key,
        sizeof(middle_secondary_key),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_find_indexed_row_equals(
        filename,
        1U,
        final_secondary_key,
        sizeof(final_secondary_key),
        final_row_id,
        final_row,
        sizeof(final_row)
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        final_row_id,
        final_row,
        sizeof(final_row)
    );
    assert_row_not_found(filename, row_id);
    assert_row_equals(filename, final_row_id, final_row, sizeof(final_row));
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 1ULL);

    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert_file_size_matches_header(filename);
    assert_find_indexed_row_equals(
        filename,
        1U,
        initial_secondary_key,
        sizeof(initial_secondary_key),
        row_id,
        initial_row,
        sizeof(initial_row)
    );
    assert_find_indexed_row_not_found(
        filename,
        1U,
        final_secondary_key,
        sizeof(final_secondary_key)
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_statement(filename, &prior_statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_id,
            middle_row,
            sizeof(middle_row),
            middle_entries,
            sizeof(middle_entries) / sizeof(middle_entries[0]),
            &middle_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(middle_row_id != row_id);
    assert(mylite_storage_commit_statement(prior_statement) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_equals(
        filename,
        1U,
        middle_secondary_key,
        sizeof(middle_secondary_key),
        middle_row_id,
        middle_row,
        sizeof(middle_row)
    );
    assert(mylite_storage_begin_statement(filename, &outer_savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_open_header(filename, &before_savepoint_update_header) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            middle_row_id,
            savepoint_row,
            sizeof(savepoint_row),
            savepoint_entries,
            sizeof(savepoint_entries) / sizeof(savepoint_entries[0]),
            &savepoint_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(savepoint_row_id == middle_row_id);
    assert(
        mylite_storage_open_header(filename, &after_savepoint_update_header) == MYLITE_STORAGE_OK
    );
    assert(after_savepoint_update_header.page_count == before_savepoint_update_header.page_count);
    assert_find_indexed_row_equals(
        filename,
        1U,
        savepoint_secondary_key,
        sizeof(savepoint_secondary_key),
        savepoint_row_id,
        savepoint_row,
        sizeof(savepoint_row)
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        savepoint_row_id,
        savepoint_row,
        sizeof(savepoint_row)
    );
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_not_found(
        filename,
        1U,
        savepoint_secondary_key,
        sizeof(savepoint_secondary_key)
    );
    assert_find_indexed_row_equals(
        filename,
        1U,
        middle_secondary_key,
        sizeof(middle_secondary_key),
        middle_row_id,
        middle_row,
        sizeof(middle_row)
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        middle_row_id,
        middle_row,
        sizeof(middle_row)
    );
    assert(mylite_storage_commit_statement(outer_savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert_file_size_matches_header(filename);
    assert_find_indexed_row_equals(
        filename,
        1U,
        middle_secondary_key,
        sizeof(middle_secondary_key),
        middle_row_id,
        middle_row,
        sizeof(middle_row)
    );
    assert_find_indexed_row_not_found(
        filename,
        1U,
        initial_secondary_key,
        sizeof(initial_secondary_key)
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_unchanged_index_update_elision(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char primary_key[] = {0x10U, 0x01U};
    static const unsigned char initial_secondary_key[] = {0x20U, 0x01U};
    static const unsigned char changed_secondary_key[] = {0x30U, 0x01U};
    static const unsigned char initial_row[] = {0x01U, 'i', 'n', 'i', 't'};
    static const unsigned char payload_update_row[] = {0x02U, 'p', 'a', 'y'};
    static const unsigned char secondary_update_row[] = {0x03U, 's', 'e', 'c'};
    static const unsigned char unchanged_entries[] = {0U, 0U};
    static const unsigned char changed_secondary_entries[] = {0U, 1U};
    char *root = make_temp_root();
    char *filename = path_join(root, "unchanged-index-update-elision.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry initial_entries[] = {
        {.size = sizeof(initial_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(initial_entries[1]),
         .index_number = 1U,
         .key = initial_secondary_key,
         .key_size = sizeof(initial_secondary_key)},
    };
    mylite_storage_index_entry changed_entries[] = {
        {.size = sizeof(changed_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(changed_entries[1]),
         .index_number = 1U,
         .key = changed_secondary_key,
         .key_size = sizeof(changed_secondary_key)},
    };
    unsigned long long row_id = 0ULL;
    unsigned long long payload_update_row_id = 0ULL;
    unsigned long long secondary_update_row_id = 0ULL;
    unsigned long long row_count = 0ULL;
    mylite_storage_header header = {
        .size = sizeof(header),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            initial_row,
            sizeof(initial_row),
            initial_entries,
            sizeof(initial_entries) / sizeof(initial_entries[0]),
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);
    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 1U) == MYLITE_STORAGE_OK);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_payload_update_pages = header.page_count;
    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            row_id,
            payload_update_row,
            sizeof(payload_update_row),
            initial_entries,
            sizeof(initial_entries) / sizeof(initial_entries[0]),
            unchanged_entries,
            &payload_update_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(payload_update_row_id != row_id);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_payload_update_pages + 2ULL);

    const unsigned long long payload_update_row_ids[] = {payload_update_row_id};
    assert_exact_index_entries(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        payload_update_row_ids,
        sizeof(payload_update_row_ids) / sizeof(payload_update_row_ids[0])
    );
    assert_exact_index_entries(
        filename,
        1U,
        initial_secondary_key,
        sizeof(initial_secondary_key),
        payload_update_row_ids,
        sizeof(payload_update_row_ids) / sizeof(payload_update_row_ids[0])
    );
    assert_index_prefix_exists(filename, initial_secondary_key, sizeof(initial_secondary_key), 1);
    assert_row_not_found(filename, row_id);
    assert_row_equals(
        filename,
        payload_update_row_id,
        payload_update_row,
        sizeof(payload_update_row)
    );

    const unsigned long long before_secondary_update_pages = header.page_count;
    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            payload_update_row_id,
            secondary_update_row,
            sizeof(secondary_update_row),
            changed_entries,
            sizeof(changed_entries) / sizeof(changed_entries[0]),
            changed_secondary_entries,
            &secondary_update_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(secondary_update_row_id != payload_update_row_id);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_secondary_update_pages + 3ULL);

    const unsigned long long secondary_update_row_ids[] = {secondary_update_row_id};
    assert_exact_index_entries(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        secondary_update_row_ids,
        sizeof(secondary_update_row_ids) / sizeof(secondary_update_row_ids[0])
    );
    assert_exact_index_entries(
        filename,
        1U,
        initial_secondary_key,
        sizeof(initial_secondary_key),
        NULL,
        0U
    );
    assert_index_prefix_exists(filename, initial_secondary_key, sizeof(initial_secondary_key), 0);
    assert_exact_index_entries(
        filename,
        1U,
        changed_secondary_key,
        sizeof(changed_secondary_key),
        secondary_update_row_ids,
        sizeof(secondary_update_row_ids) / sizeof(secondary_update_row_ids[0])
    );
    assert_index_prefix_exists(filename, changed_secondary_key, sizeof(changed_secondary_key), 1);
    assert_find_indexed_row_equals(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        secondary_update_row_id,
        secondary_update_row,
        sizeof(secondary_update_row)
    );
    assert_find_indexed_row_not_found(
        filename,
        1U,
        initial_secondary_key,
        sizeof(initial_secondary_key)
    );
    assert_find_indexed_row_equals(
        filename,
        1U,
        changed_secondary_key,
        sizeof(changed_secondary_key),
        secondary_update_row_id,
        secondary_update_row,
        sizeof(secondary_update_row)
    );
    assert_row_not_found(filename, payload_update_row_id);
    assert_row_equals(
        filename,
        secondary_update_row_id,
        secondary_update_row,
        sizeof(secondary_update_row)
    );
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 1ULL);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_indexed_row_batch_cache_reuses_duplicates(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'b', 'b'};
    char *root = make_temp_root();
    char *filename = path_join(root, "indexed-row-batch-cache.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            NULL,
            0U,
            &row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            NULL,
            0U,
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );

    const unsigned long long row_ids[] = {row_1_id, row_2_id, row_1_id, row_2_id, row_1_id};
    const unsigned char *const expected_rows[] = {row_1, row_2, row_1, row_2, row_1};
    const size_t expected_row_sizes[] = {
        sizeof(row_1),
        sizeof(row_2),
        sizeof(row_1),
        sizeof(row_2),
        sizeof(row_1),
    };
    assert_indexed_rows_equal(
        filename,
        row_ids,
        sizeof(row_ids) / sizeof(row_ids[0]),
        expected_rows,
        expected_row_sizes
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void assert_index_prefix_exists(
    const char *filename,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    int expected_exists
) {
    int exists = 0;
    assert(
        mylite_storage_index_prefix_exists(
            filename,
            "app",
            "posts",
            key_prefix,
            key_prefix_size,
            &exists
        ) == MYLITE_STORAGE_OK
    );
    assert(exists == expected_exists);
}

static void assert_index_entry_lookup(
    const char *filename,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    mylite_storage_result expected_result,
    unsigned long long expected_row_id
) {
    unsigned long long row_id = 0ULL;
    assert(
        mylite_storage_find_index_entry(
            filename,
            "app",
            "posts",
            index_number,
            key,
            key_size,
            &row_id
        ) == expected_result
    );
    assert(row_id == expected_row_id);
}

static void assert_exact_index_entries(
    const char *filename,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    const unsigned long long *expected_row_ids,
    size_t expected_count
) {
    mylite_storage_index_entryset index_entries = {
        .size = sizeof(index_entries),
    };
    assert(
        mylite_storage_read_exact_index_entries(
            filename,
            "app",
            "posts",
            index_number,
            key,
            key_size,
            &index_entries
        ) == MYLITE_STORAGE_OK
    );
    assert(index_entries.entry_count == expected_count);
    assert(index_entries.key_bytes == expected_count * key_size);
    for (size_t i = 0; i < expected_count; ++i) {
        assert_index_entry(&index_entries, i, expected_row_ids[i], key, key_size);
    }
    mylite_storage_free_index_entryset(&index_entries);
}

static void assert_indexed_rows_equal(
    const char *filename,
    const unsigned long long *row_ids,
    size_t row_id_count,
    const unsigned char *const *expected_rows,
    const size_t *expected_row_sizes
) {
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(
        mylite_storage_read_indexed_rows(filename, "app", "posts", row_ids, row_id_count, &rows) ==
        MYLITE_STORAGE_OK
    );
    assert(rows.row_count == row_id_count);
    for (size_t i = 0U; i < row_id_count; ++i) {
        assert(rows.row_ids[i] == row_ids[i]);
        assert(rows.row_sizes[i] == expected_row_sizes[i]);
        assert(rows.row_offsets[i] <= rows.row_bytes);
        assert(rows.row_sizes[i] <= rows.row_bytes - rows.row_offsets[i]);
        assert(
            memcmp(rows.rows + rows.row_offsets[i], expected_rows[i], expected_row_sizes[i]) == 0
        );
    }
    mylite_storage_free_rowset(&rows);
}

static void append_index_entry_test_rows(index_entries_test_context *ctx) {
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            ctx->row_1,
            ctx->row_1_size,
            ctx->row_1_entries,
            ctx->row_1_entry_count,
            &ctx->row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(ctx->row_1_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 1ULL);
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            ctx->row_2,
            ctx->row_2_size,
            ctx->row_2_entries,
            ctx->row_2_entry_count,
            &ctx->row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(ctx->row_2_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 4ULL);
}

static void assert_primary_index_entries_after_insert(const index_entries_test_context *ctx) {
    mylite_storage_index_entryset index_entries = {
        .size = sizeof(index_entries),
    };

    assert(
        mylite_storage_read_index_entries(ctx->filename, "app", "posts", 0U, &index_entries) ==
        MYLITE_STORAGE_OK
    );
    assert(index_entries.entry_count == 2U);
    assert(index_entries.key_bytes == ctx->key_1_size + ctx->key_2_size);
    assert_index_entry(&index_entries, 0U, ctx->row_1_id, ctx->key_1, ctx->key_1_size);
    assert_index_entry(&index_entries, 1U, ctx->row_2_id, ctx->key_2, ctx->key_2_size);
    mylite_storage_free_index_entryset(&index_entries);
}

static void update_index_entry_test_row(index_entries_test_context *ctx) {
    assert(
        mylite_storage_update_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            ctx->row_1_id,
            ctx->updated_row_1,
            ctx->updated_row_1_size,
            ctx->update_entries,
            ctx->update_entry_count,
            &ctx->updated_row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(ctx->updated_row_1_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 7ULL);
}

static void assert_primary_index_entries_after_update(const index_entries_test_context *ctx) {
    mylite_storage_index_entryset index_entries = {
        .size = sizeof(index_entries),
    };

    assert(
        mylite_storage_read_index_entries(ctx->filename, "app", "posts", 0U, &index_entries) ==
        MYLITE_STORAGE_OK
    );
    assert(index_entries.entry_count == 2U);
    assert_index_entry(&index_entries, 0U, ctx->row_2_id, ctx->key_2, ctx->key_2_size);
    assert_index_entry(&index_entries, 1U, ctx->updated_row_1_id, ctx->key_9, ctx->key_9_size);
    mylite_storage_free_index_entryset(&index_entries);
}

static void delete_index_entry_test_row(const index_entries_test_context *ctx) {
    assert(
        mylite_storage_delete_row(ctx->filename, "app", "posts", ctx->row_2_id) == MYLITE_STORAGE_OK
    );
}

static void assert_secondary_index_entries_after_delete(const index_entries_test_context *ctx) {
    mylite_storage_index_entryset index_entries = {
        .size = sizeof(index_entries),
    };

    assert(
        mylite_storage_read_index_entries(ctx->filename, "app", "posts", 1U, &index_entries) ==
        MYLITE_STORAGE_OK
    );
    assert(index_entries.entry_count == 1U);
    assert_index_entry(&index_entries, 0U, ctx->updated_row_1_id, ctx->title_u, ctx->title_u_size);
    mylite_storage_free_index_entryset(&index_entries);
}

static void assert_index_entry_test_live_rows(const index_entries_test_context *ctx) {
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_read_rows(ctx->filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    assert(rows.row_ids[0] == ctx->updated_row_1_id);
    assert(memcmp(rows.rows, ctx->updated_row_1, ctx->updated_row_1_size) == 0);
    mylite_storage_free_rowset(&rows);
}

static void test_index_root_metadata(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char key[] = {0x01U, 0x00U, 0x00U, 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "index-root-metadata.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry index_entry = {
        .size = sizeof(index_entry),
        .index_number = 0U,
        .key = key,
        .key_size = sizeof(key),
    };
    mylite_storage_index_root_metadata metadata = {
        .size = sizeof(metadata),
    };
    unsigned long long row_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_read_index_root(filename, "app", "posts", 0U, &metadata) ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(metadata.root_page == 0ULL);

    mylite_storage_index_root_definition invalid_root = {
        .size = sizeof(invalid_root),
        .schema_name = "app",
        .table_name = "posts",
        .index_number = 0U,
        .root_page = 0ULL,
        .entry_count = 1ULL,
    };
    assert(mylite_storage_store_index_root(filename, &invalid_root) == MYLITE_STORAGE_MISUSE);

    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row,
            sizeof(row),
            &index_entry,
            1U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );

    const unsigned long long root_page = row_id + 1ULL;
    mylite_storage_index_root_definition index_root = {
        .size = sizeof(index_root),
        .schema_name = "app",
        .table_name = "posts",
        .index_number = 0U,
        .root_page = root_page,
        .entry_count = 1ULL,
    };
    assert(mylite_storage_store_index_root(filename, &index_root) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 0U, root_page, 1ULL);

    index_root.entry_count = 2ULL;
    assert(mylite_storage_store_index_root(filename, &index_root) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 0U, root_page, 2ULL);
    assert(mylite_storage_drop_index_root(filename, "app", "posts", 1U) == MYLITE_STORAGE_NOTFOUND);

    assert(
        mylite_storage_rename_table(filename, "app", "posts", "app", "articles") ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_read_index_root(filename, "app", "posts", 0U, &metadata) ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_index_root(filename, "app", "articles", 0U, root_page, 2ULL);

    assert(mylite_storage_drop_index_root(filename, "app", "articles", 0U) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_read_index_root(filename, "app", "articles", 0U, &metadata) ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_drop_index_root(filename, "app", "articles", 0U) == MYLITE_STORAGE_NOTFOUND
    );

    index_root.table_name = "articles";
    assert(mylite_storage_store_index_root(filename, &index_root) == MYLITE_STORAGE_OK);
    assert(mylite_storage_drop_table(filename, "app", "articles") == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_read_index_root(filename, "app", "articles", 0U, &metadata) ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_drop_index_root(filename, "app", "articles", 0U) == MYLITE_STORAGE_NOTFOUND
    );

    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    index_root.table_name = "posts";
    assert(mylite_storage_store_index_root(filename, &index_root) == MYLITE_STORAGE_OK);
    assert(mylite_storage_drop_schema(filename, "app") == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_read_index_root(filename, "app", "posts", 0U, &metadata) ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(mylite_storage_store_index_root(filename, &index_root) == MYLITE_STORAGE_NOTFOUND);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_index_leaf_pages(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'b'};
    static const unsigned char row_3[] = {0x00U, 0x03U, 'c'};
    static const unsigned char updated_row_2[] = {0x00U, 0x20U, 'd'};
    static const unsigned char key_1[] = {0x01U, 0x00U, 0x00U, 0x00U};
    static const unsigned char key_2[] = {0x02U, 0x00U, 0x00U, 0x00U};
    static const unsigned char key_3[] = {0x03U, 0x00U, 0x00U, 0x00U};
    static const unsigned char updated_key_2[] = {0x20U, 0x00U, 0x00U, 0x00U};
    static const unsigned char secondary_key[] = {0x09U, 0x00U, 0x00U, 0x00U};
    static const unsigned char updated_secondary_key[] = {0x0aU, 0x00U, 0x00U, 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "index-leaf-pages.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entries[] = {
        {
            .size = sizeof(row_1_entries[0]),
            .index_number = 0U,
            .key = key_1,
            .key_size = sizeof(key_1),
        },
        {
            .size = sizeof(row_1_entries[0]),
            .index_number = 1U,
            .key = secondary_key,
            .key_size = sizeof(secondary_key),
        },
    };
    mylite_storage_index_entry row_2_entries[] = {
        {
            .size = sizeof(row_2_entries[0]),
            .index_number = 0U,
            .key = key_2,
            .key_size = sizeof(key_2),
        },
        {
            .size = sizeof(row_2_entries[0]),
            .index_number = 1U,
            .key = secondary_key,
            .key_size = sizeof(secondary_key),
        },
    };
    mylite_storage_index_entry row_3_entries[] = {
        {
            .size = sizeof(row_3_entries[0]),
            .index_number = 0U,
            .key = key_3,
            .key_size = sizeof(key_3),
        },
        {
            .size = sizeof(row_3_entries[0]),
            .index_number = 1U,
            .key = secondary_key,
            .key_size = sizeof(secondary_key),
        },
    };
    mylite_storage_index_entry row_2_update_entries[] = {
        {
            .size = sizeof(row_2_update_entries[0]),
            .index_number = 0U,
            .key = updated_key_2,
            .key_size = sizeof(updated_key_2),
        },
        {
            .size = sizeof(row_2_update_entries[0]),
            .index_number = 1U,
            .key = updated_secondary_key,
            .key_size = sizeof(updated_secondary_key),
        },
    };
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_3_id = 0ULL;
    unsigned long long updated_row_2_id = 0ULL;
    unsigned long long row_id = 0ULL;
    mylite_storage_header header = {
        .size = sizeof(header),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            row_1_entries,
            sizeof(row_1_entries) / sizeof(row_1_entries[0]),
            &row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            row_2_entries,
            sizeof(row_2_entries) / sizeof(row_2_entries[0]),
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 0U, header.page_count - 1ULL, 2ULL);
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_OK, row_1_id);
    assert_index_entry_lookup(filename, 0U, key_3, sizeof(key_3), MYLITE_STORAGE_NOTFOUND, 0ULL);

    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 1U) == MYLITE_STORAGE_OK);
    const unsigned long long first_secondary_row_ids[] = {row_1_id, row_2_id};
    assert_exact_index_entries(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        first_secondary_row_ids,
        sizeof(first_secondary_row_ids) / sizeof(first_secondary_row_ids[0])
    );

    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_3,
            sizeof(row_3),
            row_3_entries,
            sizeof(row_3_entries) / sizeof(row_3_entries[0]),
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );
    const unsigned long long append_tail_row_ids[] = {row_1_id, row_2_id, row_3_id};
    assert_exact_index_entries(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        append_tail_row_ids,
        sizeof(append_tail_row_ids) / sizeof(append_tail_row_ids[0])
    );

    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2_id,
            updated_row_2,
            sizeof(updated_row_2),
            row_2_update_entries,
            sizeof(row_2_update_entries) / sizeof(row_2_update_entries[0]),
            &updated_row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert_index_entry_lookup(filename, 0U, key_2, sizeof(key_2), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(
        filename,
        0U,
        updated_key_2,
        sizeof(updated_key_2),
        MYLITE_STORAGE_OK,
        updated_row_2_id
    );
    const unsigned long long update_tail_row_ids[] = {row_1_id, row_3_id};
    assert_exact_index_entries(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        update_tail_row_ids,
        sizeof(update_tail_row_ids) / sizeof(update_tail_row_ids[0])
    );
    const unsigned long long updated_secondary_row_ids[] = {updated_row_2_id};
    assert_exact_index_entries(
        filename,
        1U,
        updated_secondary_key,
        sizeof(updated_secondary_key),
        updated_secondary_row_ids,
        sizeof(updated_secondary_row_ids) / sizeof(updated_secondary_row_ids[0])
    );

    assert(mylite_storage_delete_row(filename, "app", "posts", row_1_id) == MYLITE_STORAGE_OK);
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_NOTFOUND, 0ULL);
    const unsigned long long delete_tail_row_ids[] = {row_3_id};
    assert_exact_index_entries(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        delete_tail_row_ids,
        sizeof(delete_tail_row_ids) / sizeof(delete_tail_row_ids[0])
    );

    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 1U) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_rename_table(filename, "app", "posts", "app", "articles") ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_find_index_entry(
            filename,
            "app",
            "articles",
            0U,
            updated_key_2,
            sizeof(updated_key_2),
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(row_id == updated_row_2_id);
    assert_exact_index_entries_for_table(
        filename,
        "app",
        "articles",
        1U,
        secondary_key,
        sizeof(secondary_key),
        delete_tail_row_ids,
        sizeof(delete_tail_row_ids) / sizeof(delete_tail_row_ids[0])
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_multi_page_index_leaf_pages(void) {
    enum { entry_count = 400U };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "multi-page-index-leaf-pages.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned long long row_ids[entry_count];
    mylite_storage_header header = {
        .size = sizeof(header),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (unsigned i = 0U; i < entry_count; ++i) {
        unsigned char row[8] = {0};
        unsigned char key[4] = {0};
        put_test_u32_le(row, 0U, i + 1U);
        put_test_u32_le(key, 0U, i + 1U);
        mylite_storage_index_entry index_entry = {
            .size = sizeof(index_entry),
            .index_number = 0U,
            .key = key,
            .key_size = sizeof(key),
        };
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row,
                sizeof(row),
                &index_entry,
                1U,
                &row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const size_t entry_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + 4U);
    const size_t expected_leaf_pages = ((entry_count - 1U) / entry_capacity) + 1U;
    assert_index_root(
        filename,
        "app",
        "posts",
        0U,
        header.page_count - (unsigned long long)expected_leaf_pages,
        entry_count
    );

    unsigned char first_key[4] = {0};
    unsigned char second_page_key[4] = {0};
    unsigned char last_key[4] = {0};
    unsigned char missing_key[4] = {0};
    put_test_u32_le(first_key, 0U, 1U);
    put_test_u32_le(second_page_key, 0U, (unsigned)entry_capacity + 1U);
    put_test_u32_le(last_key, 0U, entry_count);
    put_test_u32_le(missing_key, 0U, entry_count + 1U);
    assert_index_entry_lookup(
        filename,
        0U,
        first_key,
        sizeof(first_key),
        MYLITE_STORAGE_OK,
        row_ids[0]
    );
    assert_index_entry_lookup(
        filename,
        0U,
        second_page_key,
        sizeof(second_page_key),
        MYLITE_STORAGE_OK,
        row_ids[entry_capacity]
    );
    assert_index_entry_lookup(
        filename,
        0U,
        last_key,
        sizeof(last_key),
        MYLITE_STORAGE_OK,
        row_ids[entry_count - 1U]
    );
    assert_index_entry_lookup(
        filename,
        0U,
        missing_key,
        sizeof(missing_key),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );

    unsigned char tail_row[8] = {0};
    unsigned char tail_key[4] = {0};
    unsigned long long tail_row_id = 0ULL;
    put_test_u32_le(tail_row, 0U, entry_count + 1U);
    put_test_u32_le(tail_key, 0U, 200U);
    mylite_storage_index_entry tail_entry = {
        .size = sizeof(tail_entry),
        .index_number = 0U,
        .key = tail_key,
        .key_size = sizeof(tail_key),
    };
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            tail_row,
            sizeof(tail_row),
            &tail_entry,
            1U,
            &tail_row_id
        ) == MYLITE_STORAGE_OK
    );
    const unsigned long long tail_expected_row_ids[] = {row_ids[199], tail_row_id};
    assert_exact_index_entries(
        filename,
        0U,
        tail_key,
        sizeof(tail_key),
        tail_expected_row_ids,
        sizeof(tail_expected_row_ids) / sizeof(tail_expected_row_ids[0])
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_multi_page_index_leaf_duplicate_boundaries(void) {
    enum { entry_count = 420U };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "multi-page-index-leaf-duplicate-boundaries.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned long long row_ids[entry_count];
    const size_t entry_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + 4U);
    const unsigned duplicate_count = (unsigned)entry_capacity + 4U;
    assert(duplicate_count < entry_count);

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (unsigned i = 0U; i < entry_count; ++i) {
        unsigned char row[8] = {0};
        unsigned char key[4] = {0};
        const unsigned key_value = i < duplicate_count ? 7U : 8U + i - duplicate_count;
        put_test_u32_le(row, 0U, i + 1U);
        put_test_u32_le(key, 0U, key_value);
        mylite_storage_index_entry index_entry = {
            .size = sizeof(index_entry),
            .index_number = 0U,
            .key = key,
            .key_size = sizeof(key),
        };
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row,
                sizeof(row),
                &index_entry,
                1U,
                &row_ids[i]
            ) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);

    unsigned char duplicate_key[4] = {0};
    unsigned char last_key[4] = {0};
    unsigned char missing_key[4] = {0};
    put_test_u32_le(duplicate_key, 0U, 7U);
    put_test_u32_le(last_key, 0U, 8U + entry_count - duplicate_count - 1U);
    put_test_u32_le(missing_key, 0U, 6U);
    assert_exact_index_entries(
        filename,
        0U,
        duplicate_key,
        sizeof(duplicate_key),
        row_ids,
        duplicate_count
    );
    assert_index_entry_lookup(
        filename,
        0U,
        last_key,
        sizeof(last_key),
        MYLITE_STORAGE_OK,
        row_ids[entry_count - 1U]
    );
    assert_index_entry_lookup(
        filename,
        0U,
        missing_key,
        sizeof(missing_key),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void assert_index_root(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    unsigned long long expected_root_page,
    unsigned long long expected_entry_count
) {
    mylite_storage_index_root_metadata metadata = {
        .size = sizeof(metadata),
    };
    assert(
        mylite_storage_read_index_root(
            filename,
            schema_name,
            table_name,
            index_number,
            &metadata
        ) == MYLITE_STORAGE_OK
    );
    assert(metadata.root_page == expected_root_page);
    assert(metadata.entry_count == expected_entry_count);
}

static void assert_exact_index_entries_for_table(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    const unsigned long long *expected_row_ids,
    size_t expected_count
) {
    mylite_storage_index_entryset index_entries = {
        .size = sizeof(index_entries),
    };
    assert(
        mylite_storage_read_exact_index_entries(
            filename,
            schema_name,
            table_name,
            index_number,
            key,
            key_size,
            &index_entries
        ) == MYLITE_STORAGE_OK
    );
    assert(index_entries.entry_count == expected_count);
    assert(index_entries.key_bytes == expected_count * key_size);
    for (size_t i = 0; i < expected_count; ++i) {
        assert_index_entry(&index_entries, i, expected_row_ids[i], key, key_size);
    }
    mylite_storage_free_index_entryset(&index_entries);
}

static void test_autoincrement_state(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "autoincrement-state.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert_auto_increment_value(filename, 1ULL);
    exercise_exact_autoincrement_set(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 4ULL);

    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_drop_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    assert(rows.rows == NULL);
    assert_auto_increment_value(filename, 1ULL);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void exercise_exact_autoincrement_set(const char *filename) {
    assert(
        mylite_storage_set_auto_increment(filename, "app", "posts", 0ULL) == MYLITE_STORAGE_MISUSE
    );
    assert(
        mylite_storage_set_auto_increment(filename, "app", "posts", k_autoincrement_set_value) ==
        MYLITE_STORAGE_OK
    );
    assert_auto_increment_value(filename, k_autoincrement_set_value);
    assert(
        mylite_storage_advance_auto_increment(
            filename,
            "app",
            "posts",
            k_autoincrement_ignored_advance
        ) == MYLITE_STORAGE_OK
    );
    assert_auto_increment_value(filename, k_autoincrement_set_value);
    assert(
        mylite_storage_set_auto_increment(filename, "app", "posts", k_autoincrement_lower_value) ==
        MYLITE_STORAGE_OK
    );
    assert_auto_increment_value(filename, k_autoincrement_lower_value);
    assert(
        mylite_storage_advance_auto_increment(
            filename,
            "app",
            "posts",
            k_autoincrement_advanced_value
        ) == MYLITE_STORAGE_OK
    );
    assert_auto_increment_value(filename, k_autoincrement_advanced_value);
}

static void assert_auto_increment_value(const char *filename, unsigned long long expected_value) {
    unsigned long long next_value = 0ULL;
    assert(
        mylite_storage_read_auto_increment(filename, "app", "posts", &next_value) ==
        MYLITE_STORAGE_OK
    );
    assert(next_value == expected_value);
}

static void test_truncate_table_lifecycle(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char row_3[] = {0x00U, 0x01U, 'r', 'e', 'u', 's', 'e'};
    static const unsigned char key_1[] = {0x01U};
    static const unsigned char key_2[] = {0x02U};
    char *root = make_temp_root();
    char *filename = path_join(root, "truncate-table.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 0U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    mylite_storage_index_entry row_3_entry = {
        .size = sizeof(row_3_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_3_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    append_truncate_test_rows(
        filename,
        row_1,
        sizeof(row_1),
        &row_1_entry,
        row_2,
        sizeof(row_2),
        &row_2_entry,
        &row_1_id,
        &row_2_id
    );
    assert_truncate_test_initial_state(filename);
    assert(mylite_storage_truncate_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert_truncate_test_empty_state(filename, row_1_id, row_2_id);
    assert_empty_truncate_is_noop(filename);
    append_truncate_test_reused_row(
        filename,
        row_3,
        sizeof(row_3),
        &row_3_entry,
        row_2_id,
        &row_3_id
    );
    assert_truncate_test_reused_row(filename, row_3_id, key_1, sizeof(key_1));

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void append_truncate_test_rows(
    const char *filename,
    const unsigned char *row_1,
    size_t row_1_size,
    const mylite_storage_index_entry *row_1_entry,
    const unsigned char *row_2,
    size_t row_2_size,
    const mylite_storage_index_entry *row_2_entry,
    unsigned long long *out_row_1_id,
    unsigned long long *out_row_2_id
) {
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            row_1_size,
            row_1_entry,
            1U,
            out_row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2,
            row_2_size,
            row_2_entry,
            1U,
            out_row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_advance_auto_increment(filename, "app", "posts", 9ULL) == MYLITE_STORAGE_OK
    );
}

static void assert_truncate_test_initial_state(const char *filename) {
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    unsigned long long row_count = 0ULL;
    unsigned long long next_value = 0ULL;

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 2U);
    mylite_storage_free_index_entryset(&entries);
    assert(
        mylite_storage_read_auto_increment(filename, "app", "posts", &next_value) ==
        MYLITE_STORAGE_OK
    );
    assert(next_value == 9ULL);
}

static void assert_truncate_test_empty_state(
    const char *filename,
    unsigned long long row_1_id,
    unsigned long long row_2_id
) {
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    mylite_storage_table_metadata metadata = {
        .size = sizeof(metadata),
    };
    unsigned long long row_count = 0ULL;
    unsigned long long next_value = 0ULL;

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 0ULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    assert(rows.rows == NULL);
    mylite_storage_free_rowset(&rows);
    assert_row_not_found(filename, row_1_id);
    assert_row_not_found(filename, row_2_id);
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 0U);
    mylite_storage_free_index_entryset(&entries);
    assert(
        mylite_storage_read_auto_increment(filename, "app", "posts", &next_value) ==
        MYLITE_STORAGE_OK
    );
    assert(next_value == 1ULL);
    assert(
        mylite_storage_read_table_metadata(filename, "app", "posts", &metadata) == MYLITE_STORAGE_OK
    );
    assert(strcmp(metadata.requested_engine_name, "InnoDB") == 0);
    assert(strcmp(metadata.effective_engine_name, "MYLITE") == 0);
    mylite_storage_free(metadata.requested_engine_name);
    mylite_storage_free(metadata.effective_engine_name);
}

static void assert_empty_truncate_is_noop(const char *filename) {
    mylite_storage_header header_before = {0};
    mylite_storage_header header_after = {0};

    assert(mylite_storage_open_header(filename, &header_before) == MYLITE_STORAGE_OK);
    assert(mylite_storage_truncate_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header_after) == MYLITE_STORAGE_OK);
    assert(header_before.page_count == header_after.page_count);
}

static void append_truncate_test_reused_row(
    const char *filename,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entry,
    unsigned long long row_2_id,
    unsigned long long *out_row_id
) {
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row,
            row_size,
            index_entry,
            1U,
            out_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(*out_row_id > row_2_id);
}

static void assert_truncate_test_reused_row(
    const char *filename,
    unsigned long long row_id,
    const unsigned char *key,
    size_t key_size
) {
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 1ULL);
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 1U);
    assert_index_entry(&entries, 0U, row_id, key, key_size);
    mylite_storage_free_index_entryset(&entries);
}

static void test_statement_checkpoints(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char key_1[] = {0x01U};
    static const unsigned char key_2[] = {0x02U};
    char *root = make_temp_root();
    char *filename = path_join(root, "statement-checkpoint.mylite");
    char *journal_filename = journal_path(filename);
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_table_definition rollback_definition = {
        .size = sizeof(rollback_definition),
        .schema_name = "app",
        .table_name = "rollback_posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 0U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    statement_checkpoint_test_context ctx = {
        .filename = filename,
        .journal_filename = journal_filename,
        .row_1 = row_1,
        .row_1_size = sizeof(row_1),
        .row_2 = row_2,
        .row_2_size = sizeof(row_2),
        .rollback_definition = &rollback_definition,
        .row_1_entry = &row_1_entry,
        .row_2_entry = &row_2_entry,
        .key_1 = key_1,
        .key_1_size = sizeof(key_1),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);

    assert_statement_checkpoint_rolls_back_row(&ctx);
    assert_statement_checkpoint_commits_row(&ctx);
    assert_statement_checkpoint_rolls_back_catalog(&ctx);
    assert_nested_statement_checkpoints(&ctx);
    assert_statement_checkpoint_preserves_marked_auto_increment_rollback(&ctx);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
}

static void assert_statement_checkpoint_rolls_back_row(statement_checkpoint_test_context *ctx) {
    mylite_storage_statement *statement = NULL;
    mylite_storage_header checkpoint_header = {0};
    mylite_storage_header active_header = {0};
    mylite_storage_header rollback_header = {0};
    unsigned long long row_count = 0ULL;
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    const long long checkpoint_file_size = file_size(ctx->filename);

    assert(mylite_storage_open_header(ctx->filename, &checkpoint_header) == MYLITE_STORAGE_OK);
    assert(!mylite_storage_statement_active(ctx->filename));
    assert(mylite_storage_begin_statement(ctx->filename, &statement) == MYLITE_STORAGE_OK);
    assert(statement != NULL);
    assert(mylite_storage_statement_active(ctx->filename));
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            ctx->row_1,
            ctx->row_1_size,
            ctx->row_1_entry,
            1U,
            &ctx->row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(access(ctx->journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(ctx->filename, &active_header) == MYLITE_STORAGE_OK);
    assert(active_header.page_count > checkpoint_header.page_count);
    assert(
        mylite_storage_count_rows(ctx->filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK
    );
    assert(row_count == 1ULL);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert_file_missing(ctx->journal_filename);
    assert(!mylite_storage_statement_active(ctx->filename));
    assert(file_size(ctx->filename) == checkpoint_file_size);
    assert_file_size_matches_header(ctx->filename);
    assert(mylite_storage_open_header(ctx->filename, &rollback_header) == MYLITE_STORAGE_OK);
    assert(rollback_header.page_count == checkpoint_header.page_count);

    assert(
        mylite_storage_count_rows(ctx->filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK
    );
    assert(row_count == 0ULL);
    assert(
        mylite_storage_read_index_entries(ctx->filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 0U);
    mylite_storage_free_index_entryset(&entries);
    assert(mylite_storage_table_exists(ctx->filename, "app", "posts") == MYLITE_STORAGE_OK);
}

static void assert_statement_checkpoint_commits_row(statement_checkpoint_test_context *ctx) {
    mylite_storage_statement *statement = NULL;
    unsigned long long row_count = 0ULL;
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };

    assert(mylite_storage_begin_statement(ctx->filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_statement_active(ctx->filename));
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            ctx->row_1,
            ctx->row_1_size,
            ctx->row_1_entry,
            1U,
            &ctx->row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(access(ctx->journal_filename, F_OK) == 0);
    assert(mylite_storage_commit_statement(statement) == MYLITE_STORAGE_OK);
    assert_file_missing(ctx->journal_filename);
    assert(!mylite_storage_statement_active(ctx->filename));

    assert(
        mylite_storage_count_rows(ctx->filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK
    );
    assert(row_count == 1ULL);
    assert(
        mylite_storage_read_index_entries(ctx->filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 1U);
    assert_index_entry(&entries, 0U, ctx->row_1_id, ctx->key_1, ctx->key_1_size);
    mylite_storage_free_index_entryset(&entries);
}

static void assert_statement_checkpoint_rolls_back_catalog(statement_checkpoint_test_context *ctx) {
    mylite_storage_statement *statement = NULL;
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_begin_statement(ctx->filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_statement_active(ctx->filename));
    assert(
        mylite_storage_store_table_definition(ctx->filename, ctx->rollback_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            ctx->row_2,
            ctx->row_2_size,
            ctx->row_2_entry,
            1U,
            &ctx->row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "rollback_posts") == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert(!mylite_storage_statement_active(ctx->filename));

    assert(
        mylite_storage_table_exists(ctx->filename, "app", "rollback_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_count_rows(ctx->filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK
    );
    assert(row_count == 1ULL);
    assert_row_not_found(ctx->filename, ctx->row_2_id);
}

static void assert_nested_statement_checkpoints(statement_checkpoint_test_context *ctx) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    mylite_storage_table_definition outer_catalog_definition = {
        .size = sizeof(outer_catalog_definition),
        .schema_name = "app",
        .table_name = "outer_catalog_posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_table_definition inner_catalog_definition = {
        .size = sizeof(inner_catalog_definition),
        .schema_name = "app",
        .table_name = "inner_catalog_posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_statement *outer = NULL;
    mylite_storage_statement *inner = NULL;
    mylite_storage_statement *reentrant = NULL;
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_begin_nested_statement(NULL, &inner) == MYLITE_STORAGE_MISUSE);
    assert(inner == NULL);
    assert(mylite_storage_begin_statement(ctx->filename, &outer) == MYLITE_STORAGE_OK);
    assert(outer != NULL);
    assert(mylite_storage_statement_active(ctx->filename));
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            ctx->row_2,
            ctx->row_2_size,
            ctx->row_2_entry,
            1U,
            &ctx->row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_count_rows(ctx->filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK
    );
    assert(row_count == 2ULL);

    assert(mylite_storage_begin_nested_statement(outer, &inner) == MYLITE_STORAGE_OK);
    assert(inner != NULL);
    assert(mylite_storage_commit_statement(outer) == MYLITE_STORAGE_MISUSE);
    assert(mylite_storage_begin_nested_statement(outer, &reentrant) == MYLITE_STORAGE_MISUSE);
    assert(reentrant == NULL);
    assert(mylite_storage_table_exists(ctx->filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(ctx->filename, ctx->rollback_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "rollback_posts") == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(inner) == MYLITE_STORAGE_OK);
    assert(mylite_storage_statement_active(ctx->filename));
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "rollback_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    row_count = 0ULL;
    assert(
        mylite_storage_count_rows(ctx->filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK
    );
    assert(row_count == 2ULL);

    assert(mylite_storage_begin_nested_statement(outer, &inner) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(ctx->filename, ctx->rollback_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_commit_statement(inner) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "rollback_posts") == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_rollback_statement(outer) == MYLITE_STORAGE_OK);
    assert(!mylite_storage_statement_active(ctx->filename));
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "rollback_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    row_count = 0ULL;
    assert(
        mylite_storage_count_rows(ctx->filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK
    );
    assert(row_count == 1ULL);
    assert_row_not_found(ctx->filename, ctx->row_2_id);

    assert(mylite_storage_begin_statement(ctx->filename, &outer) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(ctx->filename, &outer_catalog_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_begin_nested_statement(outer, &inner) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(ctx->filename, &inner_catalog_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "inner_catalog_posts") ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(inner) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "inner_catalog_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "outer_catalog_posts") ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_commit_statement(outer) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "outer_catalog_posts") ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_table_exists(ctx->filename, "app", "inner_catalog_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
}

static void assert_statement_checkpoint_preserves_marked_auto_increment_rollback(
    statement_checkpoint_test_context *ctx
) {
    static const unsigned char row_3[] = {0x00U, 0x03U, 'g', 'h', 'i'};
    static const unsigned char row_4[] = {0x00U, 0x04U, 'j', 'k', 'l'};
    mylite_storage_statement *statement = NULL;
    unsigned long long row_id = 0ULL;
    unsigned long long no_marker_row_id = 0ULL;
    unsigned long long row_count = 0ULL;

    assert_auto_increment_value(ctx->filename, 1ULL);
    assert(mylite_storage_begin_statement(ctx->filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_advance_auto_increment(ctx->filename, "app", "posts", 11ULL) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_preserve_auto_increment_on_rollback(ctx->filename) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            row_3,
            sizeof(row_3),
            NULL,
            0U,
            &row_id
        ) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert_auto_increment_value(ctx->filename, 11ULL);
    assert_file_size_matches_header(ctx->filename);
    assert_row_not_found(ctx->filename, row_id);
    assert(
        mylite_storage_count_rows(ctx->filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK
    );
    assert(row_count == 1ULL);

    assert(mylite_storage_begin_statement(ctx->filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_advance_auto_increment(ctx->filename, "app", "posts", 13ULL) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            row_4,
            sizeof(row_4),
            NULL,
            0U,
            &no_marker_row_id
        ) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert_auto_increment_value(ctx->filename, 11ULL);
    assert_file_size_matches_header(ctx->filename);
    assert_row_not_found(ctx->filename, no_marker_row_id);
}

static void test_read_statement_storage_session(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "read-statement-session.mylite");
    int reader_owner = 0;
    int writer_owner = 0;
    mylite_storage_statement *read_statement = NULL;
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);

    mylite_storage_set_context_owner(&reader_owner);
    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    mylite_storage_free_rowset(&rows);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    assert(rows.row_bytes == sizeof(row));
    assert(rows.row_sizes[0] == sizeof(row));
    assert(memcmp(rows.rows, row, sizeof(row)) == 0);
    mylite_storage_free_rowset(&rows);

    mylite_storage_set_context_owner(&writer_owner);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_BUSY
    );

    mylite_storage_set_context_owner(&reader_owner);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    read_statement = NULL;

    mylite_storage_set_context_owner(&writer_owner);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    mylite_storage_set_context_owner(NULL);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_read_checkpoint_snapshot_cache(void) {
    static const unsigned char posts_definition[] = {0x01U, 'p', 'o', 's', 't'};
    static const unsigned char comments_definition[] = {0x01U, 'c', 'm', 't', 's'};
    char *root = make_temp_root();
    char *filename = path_join(root, "read-checkpoint-cache.mylite");
    mylite_storage_statement *read_statement = NULL;
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;
    mylite_storage_table_definition posts = {
        .size = sizeof(posts),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = posts_definition,
        .definition_size = sizeof(posts_definition),
    };
    mylite_storage_table_definition comments = {
        .size = sizeof(comments),
        .schema_name = "app",
        .table_name = "comments",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = comments_definition,
        .definition_size = sizeof(comments_definition),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &posts) == MYLITE_STORAGE_OK);

    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    read_statement = NULL;

    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    read_statement = NULL;

    assert(mylite_storage_store_table_definition(filename, &comments) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(
        mylite_storage_read_table_definition(
            filename,
            "app",
            "comments",
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == sizeof(comments_definition));
    assert(memcmp(stored_definition, comments_definition, sizeof(comments_definition)) == 0);
    mylite_storage_free(stored_definition);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_read_statement_file_cache_path_replacement(void) {
    static const unsigned char posts_definition[] = {0x01U, 'p', 'o', 's', 't'};
    static const unsigned char comments_definition[] = {0x01U, 'c', 'm', 't', 's'};
    char *root = make_temp_root();
    char *filename = path_join(root, "read-file-cache.mylite");
    char *replacement_filename = path_join(root, "replacement.mylite");
    mylite_storage_statement *read_statement = NULL;
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;
    mylite_storage_table_definition posts = {
        .size = sizeof(posts),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = posts_definition,
        .definition_size = sizeof(posts_definition),
    };
    mylite_storage_table_definition comments = {
        .size = sizeof(comments),
        .schema_name = "app",
        .table_name = "comments",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = comments_definition,
        .definition_size = sizeof(comments_definition),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &posts) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    read_statement = NULL;

    assert(mylite_storage_create_empty(replacement_filename) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(replacement_filename, &comments) == MYLITE_STORAGE_OK
    );
    assert(rename(replacement_filename, filename) == 0);

    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_read_table_definition(
            filename,
            "app",
            "comments",
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == sizeof(comments_definition));
    assert(memcmp(stored_definition, comments_definition, sizeof(comments_definition)) == 0);
    mylite_storage_free(stored_definition);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(replacement_filename);
    free(filename);
    free(root);
}

static void test_transaction_journals(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "transaction-journal.mylite");
    char *journal_filename = journal_path(filename);
    char *transaction_journal_filename = transaction_journal_path(filename);
    transaction_journal_test_context ctx = {
        .filename = filename,
        .journal_filename = journal_filename,
        .transaction_journal_filename = transaction_journal_filename,
    };
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert_file_missing(transaction_journal_filename);

    assert_transaction_journal_commits(&ctx);
    assert_transaction_journal_rolls_back(&ctx);
    assert_transaction_journal_preserves_auto_increment_rollback(&ctx);
    assert_transaction_exact_cache_invalidates_on_savepoint_rollback(&ctx);
    assert_transaction_journal_recovers_child_exit(&ctx);
    assert_transaction_and_statement_journals_recover_in_order(&ctx);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(transaction_journal_filename);
    free(filename);
    free(root);
}

static void test_transaction_owner_isolation(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char key_1[] = {'a'};
    static int owner_1 = 0;
    static int owner_2 = 0;
    char *root = make_temp_root();
    char *filename = path_join(root, "transaction-owner-isolation.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    unsigned long long row_id = 0ULL;
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_context_owner() == NULL);
    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);

    mylite_storage_set_context_owner(&owner_1);
    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(mylite_storage_statement_active(filename));
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            &row_1_entry,
            1U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 1ULL);
    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row_2, sizeof(row_2)) ==
        MYLITE_STORAGE_OK
    );
    row_count = 0ULL;
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);

    mylite_storage_set_context_owner(&owner_2);
    assert(!mylite_storage_statement_active(filename));
    row_count = 1ULL;
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 0ULL);
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 0U);
    mylite_storage_free_index_entryset(&entries);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row_2, sizeof(row_2)) ==
        MYLITE_STORAGE_BUSY
    );

    mylite_storage_set_context_owner(&owner_1);
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert(!mylite_storage_statement_active(filename));

    mylite_storage_set_context_owner(&owner_2);
    row_count = 0ULL;
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 1ULL);
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 1U);
    assert_index_entry(&entries, 0U, row_id, key_1, sizeof(key_1));
    mylite_storage_free_index_entryset(&entries);

    mylite_storage_set_context_owner(NULL);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_cross_process_transaction_read_snapshot(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char key_1[] = {'a'};
    static const unsigned char key_2[] = {'d'};
    char *root = make_temp_root();
    char *filename = path_join(root, "cross-process-read-snapshot.mylite");
    char *transaction_journal_filename = transaction_journal_path(filename);
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 0U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            &row_1_entry,
            1U,
            &row_1_id
        ) == MYLITE_STORAGE_OK
    );

    transaction_child child =
        hold_transaction_with_uncommitted_row(filename, row_2, sizeof(row_2), &row_2_entry);
    assert(access(transaction_journal_filename, F_OK) == 0);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 1ULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    assert(rows.row_ids[0] == row_1_id);
    assert(rows.row_sizes[0] == sizeof(row_1));
    assert(memcmp(rows.rows + rows.row_offsets[0], row_1, sizeof(row_1)) == 0);
    mylite_storage_free_rowset(&rows);
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 1U);
    assert_index_entry(&entries, 0U, row_1_id, key_1, sizeof(key_1));
    mylite_storage_free_index_entryset(&entries);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row_2, sizeof(row_2)) ==
        MYLITE_STORAGE_BUSY
    );

    release_transaction_child(child);
    assert_file_missing(transaction_journal_filename);

    row_count = 0ULL;
    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 2U);
    assert_index_entry(&entries, 0U, row_1_id, key_1, sizeof(key_1));
    assert_index_entry(&entries, 1U, child.row_id, key_2, sizeof(key_2));
    mylite_storage_free_index_entryset(&entries);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(transaction_journal_filename);
    free(filename);
    free(root);
}

static void assert_transaction_journal_commits(const transaction_journal_test_context *ctx) {
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char key_1[] = {'a'};
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_id = 0ULL;

    assert(mylite_storage_begin_transaction(ctx->filename, &transaction) == MYLITE_STORAGE_OK);
    assert(access(ctx->transaction_journal_filename, F_OK) == 0);
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            &row_1_entry,
            1U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert_file_missing(ctx->journal_filename);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert_file_missing(ctx->transaction_journal_filename);
    assert(mylite_storage_read_rows(ctx->filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    mylite_storage_free_rowset(&rows);
}

static void assert_transaction_journal_rolls_back(const transaction_journal_test_context *ctx) {
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char key_2[] = {'d'};
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 0U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    unsigned long long row_id = 0ULL;

    assert(mylite_storage_begin_transaction(ctx->filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            &row_2_entry,
            1U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert_file_missing(ctx->journal_filename);
    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert_file_missing(ctx->transaction_journal_filename);
    assert_file_size_matches_header(ctx->filename);
    assert(mylite_storage_read_rows(ctx->filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    mylite_storage_free_rowset(&rows);
    assert(
        mylite_storage_read_index_entries(ctx->filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 1U);
    mylite_storage_free_index_entryset(&entries);
}

static void assert_transaction_journal_preserves_auto_increment_rollback(
    const transaction_journal_test_context *ctx
) {
    static const unsigned char row_3[] = {0x00U, 0x03U, 'g', 'h', 'i'};
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_id = 0ULL;

    assert_auto_increment_value(ctx->filename, 1ULL);
    assert(mylite_storage_begin_transaction(ctx->filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_advance_auto_increment(ctx->filename, "app", "posts", 7ULL) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(ctx->filename, "app", "posts", row_3, sizeof(row_3)) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert_file_missing(ctx->transaction_journal_filename);
    assert_auto_increment_value(ctx->filename, 7ULL);
    assert(mylite_storage_read_rows(ctx->filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    mylite_storage_free_rowset(&rows);

    assert(mylite_storage_begin_transaction(ctx->filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_set_auto_increment(ctx->filename, "app", "posts", 3ULL) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert_auto_increment_value(ctx->filename, 7ULL);

    assert(mylite_storage_begin_transaction(ctx->filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_advance_auto_increment(ctx->filename, "app", "posts", 11ULL) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_begin_statement(ctx->filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_advance_auto_increment(ctx->filename, "app", "posts", 13ULL) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            row_3,
            sizeof(row_3),
            NULL,
            0U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_auto_increment_value(ctx->filename, 13ULL);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert_auto_increment_value(ctx->filename, 13ULL);
    assert_row_not_found(ctx->filename, row_id);
}

static void assert_transaction_exact_cache_invalidates_on_savepoint_rollback(
    const transaction_journal_test_context *ctx
) {
    static const unsigned char row_1[] = {0x00U, 0xf1U, 'c', 'a', 'c'};
    static const unsigned char row_2[] = {0x00U, 0xf2U, 'c', 'a', 'd'};
    static const unsigned char key_1[] = {0xf1U};
    static const unsigned char key_2[] = {0xf2U};
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 0U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 0U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;

    assert(mylite_storage_begin_transaction(ctx->filename, &transaction) == MYLITE_STORAGE_OK);
    assert_index_entry_lookup(
        ctx->filename,
        0U,
        key_1,
        sizeof(key_1),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            &row_1_entry,
            1U,
            &row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert_index_entry_lookup(ctx->filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_OK, row_1_id);

    assert(mylite_storage_begin_statement(ctx->filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            &row_2_entry,
            1U,
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert_index_entry_lookup(ctx->filename, 0U, key_2, sizeof(key_2), MYLITE_STORAGE_OK, row_2_id);
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_index_entry_lookup(
        ctx->filename,
        0U,
        key_2,
        sizeof(key_2),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );

    row_2_id = 0ULL;
    assert(
        mylite_storage_append_row_with_index_entries(
            ctx->filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            &row_2_entry,
            1U,
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert_index_entry_lookup(ctx->filename, 0U, key_2, sizeof(key_2), MYLITE_STORAGE_OK, row_2_id);
    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert_index_entry_lookup(
        ctx->filename,
        0U,
        key_1,
        sizeof(key_1),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_index_entry_lookup(
        ctx->filename,
        0U,
        key_2,
        sizeof(key_2),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
}

static void assert_transaction_journal_recovers_child_exit(
    const transaction_journal_test_context *ctx
) {
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        if (mylite_storage_begin_transaction(ctx->filename, &child_transaction) !=
            MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row(ctx->filename, "app", "posts", row_2, sizeof(row_2)) !=
            MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(ctx->transaction_journal_filename, F_OK) == 0);
    assert(mylite_storage_read_rows(ctx->filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    mylite_storage_free_rowset(&rows);
    assert_file_missing(ctx->transaction_journal_filename);
}

static void assert_transaction_and_statement_journals_recover_in_order(
    const transaction_journal_test_context *ctx
) {
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char row_3[] = {0x00U, 0x03U, 'g', 'h', 'i'};
    unsigned char transaction_saved_pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                                         [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    unsigned char statement_saved_pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                                       [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    const unsigned long long transaction_page_ids[] = {
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID,
    };
    const unsigned long long statement_page_ids[] = {MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    read_test_page(ctx->filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, transaction_saved_pages[0]);
    read_test_page(
        ctx->filename,
        MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID,
        transaction_saved_pages[1]
    );
    assert(
        mylite_storage_append_row(ctx->filename, "app", "posts", row_2, sizeof(row_2)) ==
        MYLITE_STORAGE_OK
    );
    read_test_page(ctx->filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, statement_saved_pages[0]);
    assert(
        mylite_storage_append_row(ctx->filename, "app", "posts", row_3, sizeof(row_3)) ==
        MYLITE_STORAGE_OK
    );
    write_test_recovery_journal(ctx->filename, statement_page_ids, 1U, statement_saved_pages);
    write_test_transaction_journal(
        ctx->filename,
        transaction_page_ids,
        2U,
        transaction_saved_pages
    );
    assert(access(ctx->journal_filename, F_OK) == 0);
    assert(access(ctx->transaction_journal_filename, F_OK) == 0);
    assert(mylite_storage_read_rows(ctx->filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    mylite_storage_free_rowset(&rows);
    assert_file_missing(ctx->journal_filename);
    assert_file_missing(ctx->transaction_journal_filename);
}

static void test_cleans_recovery_journal_after_mutations(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    char *root = make_temp_root();
    char *filename = path_join(root, "clean-journal.mylite");
    char *journal_filename = journal_path(filename);
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned long long row_id = 0ULL;
    unsigned long long new_row_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row_1, sizeof(row_1)) ==
        MYLITE_STORAGE_OK
    );
    assert_file_missing(journal_filename);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            NULL,
            0U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert_file_missing(journal_filename);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_id,
            row_1,
            sizeof(row_1),
            &new_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert_file_missing(journal_filename);
    assert(mylite_storage_delete_row(filename, "app", "posts", new_row_id) == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);
    assert(
        mylite_storage_advance_auto_increment(filename, "app", "posts", 7ULL) == MYLITE_STORAGE_OK
    );
    assert_file_missing(journal_filename);
    assert(mylite_storage_truncate_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);
    assert(mylite_storage_drop_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_write_size_limit_returns_full(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "size-limit-full.mylite");
    char *journal_filename = journal_path(filename);
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);

    const long long current_file_size = file_size(filename);
    assert(current_file_size > 0);

    struct rlimit original_limit;
    assert(getrlimit(RLIMIT_FSIZE, &original_limit) == 0);
    assert(
        original_limit.rlim_max == RLIM_INFINITY ||
        (rlim_t)current_file_size <= original_limit.rlim_max
    );

    struct sigaction ignore_action;
    memset(&ignore_action, 0, sizeof(ignore_action));
    ignore_action.sa_handler = SIG_IGN;
    assert(sigemptyset(&ignore_action.sa_mask) == 0);
    struct sigaction original_action;
    assert(sigaction(SIGXFSZ, &ignore_action, &original_action) == 0);

    struct rlimit limited = original_limit;
    limited.rlim_cur = (rlim_t)current_file_size;
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    const mylite_storage_result result =
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row));
    assert(setrlimit(RLIMIT_FSIZE, &original_limit) == 0);
    assert(sigaction(SIGXFSZ, &original_action, NULL) == 0);

    assert(result == MYLITE_STORAGE_FULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    mylite_storage_free_rowset(&rows);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_recovers_row_publication_journal(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "row-recovery.mylite");
    char *journal_filename = journal_path(filename);
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned char saved_pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    const unsigned long long page_ids[] = {MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    read_test_page(filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, saved_pages[0]);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    write_test_recovery_journal(filename, page_ids, 1U, saved_pages);

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    mylite_storage_free_rowset(&rows);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_recovers_catalog_publication_journal(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-recovery.mylite");
    char *journal_filename = journal_path(filename);
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned char saved_pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    const unsigned long long page_ids[] = {
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID,
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    read_test_page(filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, saved_pages[0]);
    read_test_page(filename, MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID, saved_pages[1]);
    assert(mylite_storage_drop_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);
    write_test_recovery_journal(filename, page_ids, 2U, saved_pages);

    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_recovery_journal(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-journal.mylite");
    char *journal_filename = journal_path(filename);
    mylite_storage_header header = {0};
    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    FILE *journal_file = NULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    journal_file = fopen(journal_filename, "wb");
    assert(journal_file != NULL);
    assert(fwrite(page, 1U, sizeof(page), journal_file) == sizeof(page));
    assert(fclose(journal_file) == 0);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_CORRUPT);
    assert(access(journal_filename, F_OK) == 0);

    assert(unlink(journal_filename) == 0);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_rejects_operations_during_exclusive_file_lock(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "exclusive-lock.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    lock_child child = hold_test_lock(filename, LOCK_EX);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_BUSY);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_BUSY
    );

    release_test_lock(child);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_shared_file_lock_allows_readers_and_blocks_writers(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "shared-lock.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    lock_child child = hold_test_lock(filename, LOCK_SH);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_BUSY
    );

    release_test_lock(child);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_recovery_requires_exclusive_file_lock(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "recovery-lock.mylite");
    char *journal_filename = journal_path(filename);
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned char saved_pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    const unsigned long long page_ids[] = {MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    read_test_page(filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, saved_pages[0]);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    write_test_recovery_journal(filename, page_ids, 1U, saved_pages);

    lock_child child = hold_test_lock(filename, LOCK_SH);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_BUSY);
    assert(access(journal_filename, F_OK) == 0);
    release_test_lock(child);

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    mylite_storage_free_rowset(&rows);
    assert_file_missing(journal_filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_transaction_recovery_requires_exclusive_file_lock(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "transaction-recovery-lock.mylite");
    char *transaction_journal_filename = transaction_journal_path(filename);
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned char saved_pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    const unsigned long long page_ids[] = {
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID,
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    read_test_page(filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, saved_pages[0]);
    read_test_page(filename, MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID, saved_pages[1]);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    write_test_transaction_journal(filename, page_ids, 2U, saved_pages);

    lock_child child = hold_test_lock(filename, LOCK_SH);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_BUSY);
    assert(access(transaction_journal_filename, F_OK) == 0);
    release_test_lock(child);

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    mylite_storage_free_rowset(&rows);
    assert_file_missing(transaction_journal_filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(transaction_journal_filename);
    free(filename);
    free(root);
}

static void test_busy_timeout_expires_while_lock_held(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "busy-timeout-expires.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    lock_child child = hold_test_lock(filename, LOCK_EX);

    mylite_storage_set_busy_timeout(k_busy_timeout_expiry_ms);
    assert(mylite_storage_busy_timeout() == k_busy_timeout_expiry_ms);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_BUSY);
    mylite_storage_set_busy_timeout(0U);

    release_test_lock(child);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_busy_timeout_waits_for_lock_release(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "busy-timeout-waits.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    pid_t child = hold_test_lock_for(
        filename,
        (timed_lock_request){
            .operation = LOCK_EX,
            .milliseconds = k_busy_timeout_release_ms,
        }
    );

    mylite_storage_set_busy_timeout(k_busy_timeout_wait_ms);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    mylite_storage_set_busy_timeout(0U);
    wait_test_lock_child(child);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_row_page(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-row-page.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    flip_file_byte(
        filename,
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 3U) + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET)
    );
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_CORRUPT);
    assert(
        mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_CORRUPT
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_row_payload_page(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    const size_t row_size = payload_capacity + 17U;
    unsigned char *row = (unsigned char *)malloc(row_size);
    assert(row != NULL);
    for (size_t i = 0U; i < row_size; ++i) {
        row[i] = (unsigned char)(i % UINT8_MAX);
    }

    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-row-payload-page.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "payloads",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "payloads", row, row_size) == MYLITE_STORAGE_OK
    );
    flip_file_byte(
        filename,
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 3U) + MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET)
    );
    assert(mylite_storage_read_rows(filename, "app", "payloads", &rows) == MYLITE_STORAGE_CORRUPT);
    assert(
        mylite_storage_count_rows(filename, "app", "payloads", &row_count) == MYLITE_STORAGE_CORRUPT
    );

    free(row);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_row_state_page(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-row-state-page.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    assert(
        mylite_storage_delete_row(filename, "app", "posts", rows.row_ids[0]) == MYLITE_STORAGE_OK
    );
    mylite_storage_free_rowset(&rows);

    flip_file_byte(
        filename,
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 4U) + MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_OFFSET)
    );
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_CORRUPT);
    assert(
        mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_CORRUPT
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_index_entry_page(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char key[] = {0x01U};
    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-index-entry-page.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry index_entry = {
        .size = sizeof(index_entry),
        .index_number = 0U,
        .key = key,
        .key_size = sizeof(key),
    };
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row,
            sizeof(row),
            &index_entry,
            1U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(row_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 1ULL);

    flip_file_byte(
        filename,
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 4U) + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET)
    );
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_CORRUPT
    );
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    mylite_storage_free_rowset(&rows);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_autoincrement_page(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-autoincrement-page.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned long long next_value = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_advance_auto_increment(filename, "app", "posts", 2ULL) == MYLITE_STORAGE_OK
    );
    flip_file_byte(
        filename,
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 3U) +
               MYLITE_STORAGE_FORMAT_AUTOINCREMENT_NEXT_VALUE_OFFSET)
    );
    assert(
        mylite_storage_read_auto_increment(filename, "app", "posts", &next_value) ==
        MYLITE_STORAGE_CORRUPT
    );
    assert(
        mylite_storage_advance_auto_increment(filename, "app", "posts", 3ULL) ==
        MYLITE_STORAGE_CORRUPT
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_drop_table_definition(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "drop-table.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    table_list_capture capture = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_drop_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_NOTFOUND);
    assert(
        mylite_storage_list_tables(filename, "app", collect_table, &capture) == MYLITE_STORAGE_OK
    );
    assert(capture.count == 0U);
    assert(mylite_storage_drop_table(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);

    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    assert(rows.rows == NULL);
    assert(
        mylite_storage_list_tables(filename, "app", collect_table, &capture) == MYLITE_STORAGE_OK
    );
    assert(capture.count == 1U);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rename_table_definition(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "rename-table.mylite");
    mylite_storage_table_definition posts_definition = {
        .size = sizeof(posts_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_table_definition target_definition = {
        .size = sizeof(target_definition),
        .schema_name = "app",
        .table_name = "target_posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_table_metadata metadata = {
        .size = sizeof(metadata),
    };
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;
    table_list_capture capture = {
        .schema_name = "blog",
        .table_name = "articles",
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &posts_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(filename, &target_definition) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_rename_table(filename, "app", "missing", "blog", "articles") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_rename_table(filename, "app", "missing", "app", "target_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_rename_table(filename, "app", "posts", "app", "target_posts") ==
        MYLITE_STORAGE_ERROR
    );
    assert(
        mylite_storage_rename_table(filename, "app", "posts", "blog", "articles") ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_table_exists(filename, "blog", "articles") == MYLITE_STORAGE_OK);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_read_rows(filename, "blog", "articles", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_size == sizeof(row));
    assert(rows.row_count == 1U);
    assert(memcmp(rows.rows, row, sizeof(row)) == 0);
    mylite_storage_free_rowset(&rows);
    assert(
        mylite_storage_read_table_definition(
            filename,
            "blog",
            "articles",
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == sizeof(definition));
    assert(memcmp(stored_definition, definition, sizeof(definition)) == 0);
    mylite_storage_free(stored_definition);
    assert(
        mylite_storage_read_table_metadata(filename, "blog", "articles", &metadata) ==
        MYLITE_STORAGE_OK
    );
    assert(strcmp(metadata.requested_engine_name, "InnoDB") == 0);
    assert(strcmp(metadata.effective_engine_name, "MYLITE") == 0);
    mylite_storage_free(metadata.requested_engine_name);
    mylite_storage_free(metadata.effective_engine_name);
    assert(
        mylite_storage_list_tables(filename, "blog", collect_table, &capture) == MYLITE_STORAGE_OK
    );
    assert(capture.count == 1U);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-storage-test.XXXXXX";
    char *root = mkdtemp(template_path);
    assert(root != NULL);

    char *copy = strdup(root);
    assert(copy != NULL);
    return copy;
}

static char *path_join(const char *directory, const char *name) {
    const size_t directory_len = strlen(directory);
    const size_t name_len = strlen(name);
    char *path = (char *)malloc(directory_len + name_len + 2U);
    assert(path != NULL);
    memcpy(path, directory, directory_len);
    path[directory_len] = '/';
    memcpy(path + directory_len + 1U, name, name_len + 1U);
    return path;
}

static char *journal_path(const char *filename) {
    static const char suffix[] = "-journal";
    const size_t filename_len = strlen(filename);
    char *path = (char *)malloc(filename_len + sizeof(suffix));
    assert(path != NULL);
    memcpy(path, filename, filename_len);
    memcpy(path + filename_len, suffix, sizeof(suffix));
    return path;
}

static char *transaction_journal_path(const char *filename) {
    static const char suffix[] = "-transaction-journal";
    const size_t filename_len = strlen(filename);
    char *path = (char *)malloc(filename_len + sizeof(suffix));
    assert(path != NULL);
    memcpy(path, filename, filename_len);
    memcpy(path + filename_len, suffix, sizeof(suffix));
    return path;
}

static long long file_size(const char *path) {
    struct stat path_stat;
    assert(stat(path, &path_stat) == 0);
    return (long long)path_stat.st_size;
}

static void assert_file_size_matches_header(const char *filename) {
    mylite_storage_header header = {0};
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(file_size(filename) == (long long)(header.page_count * header.page_size));
}

static void assert_file_missing(const char *path) {
    errno = 0;
    assert(access(path, F_OK) != 0);
    assert(errno == ENOENT);
}

static void assert_post_rowset_layout(const mylite_storage_rowset *rows, size_t row_size) {
    assert(rows->row_size == row_size);
    assert(rows->row_count == 2U);
    assert(rows->row_bytes == row_size * 2U);
    assert(rows->row_offsets[0] == 0U);
    assert(rows->row_sizes[0] == row_size);
    assert(rows->row_ids[0] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 2ULL);
    assert(rows->row_offsets[1] == row_size);
    assert(rows->row_sizes[1] == row_size);
    assert(rows->row_ids[1] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 4ULL);
}

static void assert_lifecycle_initial_rows(const mylite_storage_rowset *rows) {
    assert(rows->row_count == 3U);
    assert(rows->row_ids[0] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 1ULL);
    assert(rows->row_ids[1] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 2ULL);
    assert(rows->row_ids[2] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 3ULL);
}

static void assert_lifecycle_live_rows(
    const mylite_storage_rowset *rows,
    unsigned long long new_row_id
) {
    static const unsigned char row_3[] = {0x00U, 0x03U, 'x', 'y', 'z'};
    static const unsigned char updated_row_1[] = {0x00U, 0x04U, 'u', 'p', 'd', 'a', 't', 'e', 'd'};

    assert(rows->row_size == 0U);
    assert(rows->row_count == 2U);
    assert(rows->row_ids[0] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 3ULL);
    assert(rows->row_ids[1] == new_row_id);
    assert(memcmp(rows->rows + rows->row_offsets[0], row_3, sizeof(row_3)) == 0);
    assert(memcmp(rows->rows + rows->row_offsets[1], updated_row_1, sizeof(updated_row_1)) == 0);
}

static void assert_row_not_found(const char *filename, unsigned long long row_id) {
    unsigned char *stored_row = NULL;
    size_t stored_row_size = 0U;

    assert(
        mylite_storage_read_row(filename, "app", "posts", row_id, &stored_row, &stored_row_size) ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(stored_row == NULL);
    assert(stored_row_size == 0U);
}

static void assert_row_equals(
    const char *filename,
    unsigned long long row_id,
    const unsigned char *expected_row,
    size_t expected_row_size
) {
    unsigned char *stored_row = NULL;
    size_t stored_row_size = 0U;

    assert(
        mylite_storage_read_row(filename, "app", "posts", row_id, &stored_row, &stored_row_size) ==
        MYLITE_STORAGE_OK
    );
    assert(stored_row_size == expected_row_size);
    assert(memcmp(stored_row, expected_row, expected_row_size) == 0);
    mylite_storage_free(stored_row);
}

static void assert_indexed_row_equals(
    const char *filename,
    unsigned long long row_id,
    const unsigned char *expected_row,
    size_t expected_row_size
) {
    unsigned char *stored_row = NULL;
    size_t stored_row_size = 0U;

    assert(
        mylite_storage_read_indexed_row(
            filename,
            "app",
            "posts",
            row_id,
            &stored_row,
            &stored_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_row_size == expected_row_size);
    assert(memcmp(stored_row, expected_row, expected_row_size) == 0);
    mylite_storage_free(stored_row);
}

static void assert_find_indexed_row_equals(
    const char *filename,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long expected_row_id,
    const unsigned char *expected_row,
    size_t expected_row_size
) {
    unsigned long long row_id = 0ULL;
    unsigned char *stored_row = NULL;
    size_t stored_row_size = 0U;

    assert(
        mylite_storage_find_indexed_row(
            filename,
            "app",
            "posts",
            index_number,
            key,
            key_size,
            &row_id,
            &stored_row,
            &stored_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(row_id == expected_row_id);
    assert(stored_row_size == expected_row_size);
    assert(memcmp(stored_row, expected_row, expected_row_size) == 0);
    mylite_storage_free(stored_row);
}

static void assert_find_indexed_row_not_found(
    const char *filename,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size
) {
    unsigned long long row_id = 0ULL;
    unsigned char *stored_row = NULL;
    size_t stored_row_size = 0U;

    assert(
        mylite_storage_find_indexed_row(
            filename,
            "app",
            "posts",
            index_number,
            key,
            key_size,
            &row_id,
            &stored_row,
            &stored_row_size
        ) == MYLITE_STORAGE_NOTFOUND
    );
    assert(row_id == 0ULL);
    assert(stored_row == NULL);
    assert(stored_row_size == 0U);
}

static void assert_index_entry(
    const mylite_storage_index_entryset *index_entries,
    size_t entry_index,
    unsigned long long row_id,
    const unsigned char *key,
    size_t key_size
) {
    assert(index_entries->row_ids[entry_index] == row_id);
    assert(index_entries->key_sizes[entry_index] == key_size);
    assert(
        memcmp(index_entries->keys + index_entries->key_offsets[entry_index], key, key_size) == 0
    );
}

static void read_test_page(const char *path, unsigned long long page_id, unsigned char *out_page) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, (long)(page_id * MYLITE_STORAGE_FORMAT_PAGE_SIZE), SEEK_SET) == 0);
    assert(
        fread(out_page, 1U, MYLITE_STORAGE_FORMAT_PAGE_SIZE, file) ==
        MYLITE_STORAGE_FORMAT_PAGE_SIZE
    );
    assert(fclose(file) == 0);
}

static void write_test_recovery_journal(
    const char *filename,
    const unsigned long long *page_ids,
    size_t page_count,
    const unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE]
) {
    char *journal_filename = journal_path(filename);
    unsigned char journal_header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    FILE *file = fopen(journal_filename, "wb");
    assert(file != NULL);

    write_test_journal_header_page(journal_header_page, page_ids, page_count);
    assert(
        fwrite(journal_header_page, 1U, sizeof(journal_header_page), file) ==
        sizeof(journal_header_page)
    );
    for (size_t i = 0U; i < page_count; ++i) {
        assert(
            fwrite(pages[i], 1U, MYLITE_STORAGE_FORMAT_PAGE_SIZE, file) ==
            MYLITE_STORAGE_FORMAT_PAGE_SIZE
        );
    }

    assert(fclose(file) == 0);
    free(journal_filename);
}

static void write_test_transaction_journal(
    const char *filename,
    const unsigned long long *page_ids,
    size_t page_count,
    const unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE]
) {
    char *journal_filename = transaction_journal_path(filename);
    unsigned char journal_header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    FILE *file = fopen(journal_filename, "wb");
    assert(file != NULL);

    write_test_journal_header_page(journal_header_page, page_ids, page_count);
    assert(
        fwrite(journal_header_page, 1U, sizeof(journal_header_page), file) ==
        sizeof(journal_header_page)
    );
    for (size_t i = 0U; i < page_count; ++i) {
        assert(
            fwrite(pages[i], 1U, MYLITE_STORAGE_FORMAT_PAGE_SIZE, file) ==
            MYLITE_STORAGE_FORMAT_PAGE_SIZE
        );
    }

    assert(fclose(file) == 0);
    free(journal_filename);
}

static void write_test_journal_header_page(
    unsigned char *page,
    const unsigned long long *page_ids,
    size_t page_count
) {
    static const unsigned char magic[8] = {'M', 'Y', 'L', 'J', 'N', 'L', '1', '\0'};

    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(page + MYLITE_STORAGE_FORMAT_JOURNAL_MAGIC_OFFSET, magic, sizeof(magic));
    put_test_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_TYPE_ROLLBACK
    );
    put_test_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_JOURNAL_VERSION
    );
    put_test_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_test_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_test_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_PRIMARY_PAGE_SIZE_OFFSET,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE
    );
    put_test_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_PROTECTED_PAGE_COUNT_OFFSET,
        (unsigned)page_count
    );
    for (size_t i = 0U; i < page_count; ++i) {
        put_test_u64_le(
            page,
            MYLITE_STORAGE_FORMAT_JOURNAL_PROTECTED_PAGE_IDS_OFFSET + (i * sizeof(uint64_t)),
            page_ids[i]
        );
    }
    put_test_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_OFFSET,
        checksum_test_page(page, MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_OFFSET)
    );
}

static lock_child hold_test_lock(const char *filename, int operation) {
    int ready_pipe[2];
    int release_pipe[2];
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        FILE *file = fopen(filename, "r+b");
        if (file == NULL || flock(fileno(file), operation) != 0) {
            _exit(2);
        }
        const unsigned char ready = 1U;
        if (write(ready_pipe[1], &ready, sizeof(ready)) != (ssize_t)sizeof(ready)) {
            _exit(3);
        }
        unsigned char release = 0U;
        (void)read(release_pipe[0], &release, sizeof(release));
        fclose(file);
        close(ready_pipe[1]);
        close(release_pipe[0]);
        _exit(0);
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    unsigned char ready = 0U;
    assert(read(ready_pipe[0], &ready, sizeof(ready)) == (ssize_t)sizeof(ready));
    assert(ready == 1U);
    close(ready_pipe[0]);

    return (lock_child){
        .pid = pid,
        .release_fd = release_pipe[1],
    };
}

static void release_test_lock(lock_child child) {
    const unsigned char release = 1U;
    assert(write(child.release_fd, &release, sizeof(release)) == (ssize_t)sizeof(release));
    assert(close(child.release_fd) == 0);

    int status = 0;
    assert(waitpid(child.pid, &status, 0) == child.pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static pid_t hold_test_lock_for(const char *filename, timed_lock_request request) {
    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(ready_pipe[0]);
        FILE *file = fopen(filename, "r+b");
        if (file == NULL || flock(fileno(file), request.operation) != 0) {
            _exit(2);
        }
        const unsigned char ready = 1U;
        if (write(ready_pipe[1], &ready, sizeof(ready)) != (ssize_t)sizeof(ready)) {
            _exit(3);
        }
        usleep((useconds_t)request.milliseconds * k_microseconds_per_millisecond);
        fclose(file);
        close(ready_pipe[1]);
        _exit(0);
    }

    close(ready_pipe[1]);
    unsigned char ready = 0U;
    assert(read(ready_pipe[0], &ready, sizeof(ready)) == (ssize_t)sizeof(ready));
    assert(ready == 1U);
    close(ready_pipe[0]);
    return pid;
}

static void wait_test_lock_child(pid_t pid) {
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static transaction_child hold_transaction_with_uncommitted_row(
    const char *filename,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entry
) {
    int ready_pipe[2];
    int release_pipe[2];
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);

        mylite_storage_statement *transaction = NULL;
        unsigned long long row_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row,
                row_size,
                index_entry,
                1U,
                &row_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        if (write(ready_pipe[1], &row_id, sizeof(row_id)) != (ssize_t)sizeof(row_id)) {
            _exit(4);
        }

        unsigned char release = 0U;
        (void)read(release_pipe[0], &release, sizeof(release));
        if (mylite_storage_commit_statement(transaction) != MYLITE_STORAGE_OK) {
            _exit(5);
        }
        close(ready_pipe[1]);
        close(release_pipe[0]);
        _exit(0);
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    unsigned long long row_id = 0ULL;
    assert(read(ready_pipe[0], &row_id, sizeof(row_id)) == (ssize_t)sizeof(row_id));
    close(ready_pipe[0]);

    return (transaction_child){
        .pid = pid,
        .release_fd = release_pipe[1],
        .row_id = row_id,
    };
}

static void release_transaction_child(transaction_child child) {
    const unsigned char release = 1U;
    assert(write(child.release_fd, &release, sizeof(release)) == (ssize_t)sizeof(release));
    assert(close(child.release_fd) == 0);

    int status = 0;
    assert(waitpid(child.pid, &status, 0) == child.pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static void put_test_u32_le(unsigned char *page, size_t offset, unsigned value) {
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        page[offset + i] = (unsigned char)((value >> (unsigned)(i * CHAR_BIT)) & UINT8_MAX);
    }
}

static void put_test_u64_le(unsigned char *page, size_t offset, unsigned long long value) {
    for (size_t i = 0U; i < sizeof(uint64_t); ++i) {
        page[offset + i] = (unsigned char)((value >> (unsigned)(i * CHAR_BIT)) & UINT8_MAX);
    }
}

static unsigned long long checksum_test_page(const unsigned char *page, size_t checksum_offset) {
    unsigned long long checksum = UINT64_C(1469598103934665603);
    for (size_t i = 0U; i < MYLITE_STORAGE_FORMAT_PAGE_SIZE; ++i) {
        const unsigned char byte =
            i >= checksum_offset && i < checksum_offset + sizeof(uint64_t) ? 0U : page[i];
        checksum ^= byte;
        checksum *= UINT64_C(1099511628211);
    }
    return checksum;
}

static void flip_file_byte(const char *path, long offset) {
    FILE *file = fopen(path, "r+b");
    assert(file != NULL);
    assert(fseek(file, offset, SEEK_SET) == 0);
    const int byte = fgetc(file);
    assert(byte != EOF);
    assert(fseek(file, offset, SEEK_SET) == 0);
    assert(fputc(byte ^ 0x01, file) != EOF);
    assert(fclose(file) == 0);
}

static void write_header_format_version(const char *path, unsigned value) {
    FILE *file = fopen(path, "r+b");
    assert(file != NULL);
    assert(fseek(file, (long)MYLITE_STORAGE_FORMAT_HEADER_FORMAT_VERSION_OFFSET, SEEK_SET) == 0);
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        assert(fputc((int)((value >> (unsigned)(i * CHAR_BIT)) & UINT8_MAX), file) != EOF);
    }
    assert(fclose(file) == 0);
}

static int collect_table(void *ctx, const char *schema_name, const char *table_name) {
    table_list_capture *capture = (table_list_capture *)ctx;
    const char *expected_schema_name = capture->schema_name == NULL ? "app" : capture->schema_name;
    const char *expected_table_name = capture->table_name == NULL ? "posts" : capture->table_name;
    assert(strcmp(schema_name, expected_schema_name) == 0);
    assert(strcmp(table_name, expected_table_name) == 0);
    ++capture->count;
    return 0;
}

static int collect_schema(void *ctx, const char *schema_name) {
    schema_list_capture *capture = (schema_list_capture *)ctx;
    assert(
        strcmp(schema_name, capture->app_schema) == 0 ||
        strcmp(schema_name, capture->blog_schema) == 0
    );
    ++capture->count;
    return 0;
}

static int collect_foreign_key(void *ctx, const mylite_storage_foreign_key_metadata *metadata) {
    foreign_key_list_capture *capture = (foreign_key_list_capture *)ctx;
    assert(strcmp(metadata->constraint_name, capture->expected_constraint_name) == 0);
    assert(strcmp(metadata->table_name, capture->expected_table_name) == 0);
    assert(strcmp(metadata->referenced_table_name, capture->expected_referenced_table_name) == 0);
    assert(metadata->column_count == capture->expected_column_count);
    ++capture->count;
    return 0;
}

#include "storage_format.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
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
static void test_index_entries(void);
static void assert_index_prefix_exists(
    const char *filename,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    int expected_exists
);
static void append_index_entry_test_rows(index_entries_test_context *ctx);
static void assert_primary_index_entries_after_insert(const index_entries_test_context *ctx);
static void update_index_entry_test_row(index_entries_test_context *ctx);
static void assert_primary_index_entries_after_update(const index_entries_test_context *ctx);
static void delete_index_entry_test_row(const index_entries_test_context *ctx);
static void assert_secondary_index_entries_after_delete(const index_entries_test_context *ctx);
static void assert_index_entry_test_live_rows(const index_entries_test_context *ctx);
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
static void test_transaction_journals(void);
static void assert_transaction_journal_commits(const transaction_journal_test_context *ctx);
static void assert_transaction_journal_rolls_back(const transaction_journal_test_context *ctx);
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
static void test_busy_timeout_expires_while_lock_held(void);
static void test_busy_timeout_waits_for_lock_release(void);
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
    test_index_entries();
    test_autoincrement_state();
    test_truncate_table_lifecycle();
    test_statement_checkpoints();
    test_transaction_journals();
    test_cleans_recovery_journal_after_mutations();
    test_recovers_row_publication_journal();
    test_recovers_catalog_publication_journal();
    test_rejects_corrupt_recovery_journal();
    test_rejects_operations_during_exclusive_file_lock();
    test_shared_file_lock_allows_readers_and_blocks_writers();
    test_recovery_requires_exclusive_file_lock();
    test_busy_timeout_expires_while_lock_held();
    test_busy_timeout_waits_for_lock_release();
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

static void test_index_entries(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char updated_row_1[] = {0x00U, 0x09U, 'u', 'p', 'd'};
    static const unsigned char key_1[] = {0x01U};
    static const unsigned char key_2[] = {0x02U};
    static const unsigned char key_9[] = {0x09U};
    static const unsigned char title_a[] = {'a'};
    static const unsigned char title_d[] = {'d'};
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
         .key = title_d,
         .key_size = sizeof(title_d)},
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
    assert_primary_index_entries_after_insert(&ctx);
    assert_index_prefix_exists(filename, key_1, sizeof(key_1), 1);
    assert_index_prefix_exists(filename, key_9, sizeof(key_9), 0);
    update_index_entry_test_row(&ctx);
    assert_primary_index_entries_after_update(&ctx);
    assert_index_prefix_exists(filename, key_1, sizeof(key_1), 0);
    assert_index_prefix_exists(filename, key_9, sizeof(key_9), 1);
    delete_index_entry_test_row(&ctx);
    assert_secondary_index_entries_after_delete(&ctx);
    assert_index_prefix_exists(filename, key_2, sizeof(key_2), 0);
    assert_index_prefix_exists(filename, title_u, sizeof(title_u), 1);
    assert_index_entry_test_live_rows(&ctx);

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

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void assert_statement_checkpoint_rolls_back_row(statement_checkpoint_test_context *ctx) {
    mylite_storage_statement *statement = NULL;
    unsigned long long row_count = 0ULL;
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };

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
    assert(
        mylite_storage_count_rows(ctx->filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK
    );
    assert(row_count == 1ULL);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert(!mylite_storage_statement_active(ctx->filename));

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
    assert(mylite_storage_commit_statement(statement) == MYLITE_STORAGE_OK);
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
    mylite_storage_statement *outer = NULL;
    mylite_storage_statement *inner = NULL;
    unsigned long long row_count = 0ULL;

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

    assert(mylite_storage_begin_statement(ctx->filename, &inner) == MYLITE_STORAGE_OK);
    assert(inner != NULL);
    assert(mylite_storage_commit_statement(outer) == MYLITE_STORAGE_MISUSE);
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

    assert(mylite_storage_begin_statement(ctx->filename, &inner) == MYLITE_STORAGE_OK);
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
    assert_transaction_journal_recovers_child_exit(&ctx);
    assert_transaction_and_statement_journals_recover_in_order(&ctx);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
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
    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert_file_missing(ctx->transaction_journal_filename);
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

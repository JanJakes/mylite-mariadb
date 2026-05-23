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

#ifdef MYLITE_STORAGE_TEST_HOOKS
mylite_storage_result mylite_storage_test_encode_maintained_index_root_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned index_number,
    size_t key_size,
    const mylite_storage_index_entryset *entryset
);
mylite_storage_result mylite_storage_test_decode_maintained_index_root_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    unsigned long long *out_table_id,
    unsigned *out_index_number,
    size_t *out_key_size,
    size_t *out_entry_count
);
mylite_storage_result mylite_storage_test_encode_index_branch_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned index_number,
    unsigned level,
    size_t key_size,
    const unsigned long long *child_page_ids,
    const unsigned long long *child_max_row_ids,
    const unsigned char *child_max_keys,
    size_t child_count
);
mylite_storage_result mylite_storage_test_encode_index_branch_page_with_entry_count(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned index_number,
    unsigned level,
    size_t key_size,
    unsigned long long entry_count,
    const unsigned long long *child_page_ids,
    const unsigned long long *child_max_row_ids,
    const unsigned char *child_max_keys,
    size_t child_count
);
mylite_storage_result mylite_storage_test_decode_index_branch_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    unsigned long long *out_table_id,
    unsigned *out_index_number,
    unsigned *out_level,
    size_t *out_key_size,
    size_t *out_child_count
);
mylite_storage_result mylite_storage_test_find_index_branch_child_page(
    const mylite_storage_header *header,
    unsigned long long page_id,
    const unsigned char *page,
    const unsigned char *key,
    size_t key_size,
    unsigned long long row_id,
    unsigned long long *out_child_page_id
);
mylite_storage_result mylite_storage_test_reclaim_removed_branch_leaf_page(
    const char *filename,
    unsigned long long leaf_page_id
);
mylite_storage_result mylite_storage_test_allocate_catalog_page_run(
    const char *filename,
    unsigned long long page_count,
    unsigned long long *out_first_page_id,
    int *out_reused_run
);
mylite_storage_result mylite_storage_test_reclaim_catalog_page_run(
    const char *filename,
    unsigned long long first_page_id,
    unsigned long long page_count
);
void mylite_storage_test_encode_free_list_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long next_root_page,
    unsigned long long run_page_count
);
mylite_storage_result mylite_storage_test_protect_active_dirty_pages(
    const char *filename,
    const unsigned long long *page_ids,
    size_t page_count
);
mylite_storage_result mylite_storage_test_flip_active_page_byte(
    const char *filename,
    unsigned long long page_id,
    size_t offset
);
int mylite_storage_test_reusable_read_statement_cached(void);
int mylite_storage_test_statement_owns_filename(const mylite_storage_statement *statement);
int mylite_storage_test_statement_filename_is(
    const mylite_storage_statement *statement,
    const char *filename
);
int mylite_storage_test_statement_has_table_entry_cache(const mylite_storage_statement *statement);
int mylite_storage_test_statement_has_live_row_cache(const mylite_storage_statement *statement);
int mylite_storage_test_statement_has_row_state_map_cache(
    const mylite_storage_statement *statement
);
int mylite_storage_test_statement_has_row_payload_cache(const mylite_storage_statement *statement);
int mylite_storage_test_statement_exact_index_cache_count(
    const mylite_storage_statement *statement
);
int mylite_storage_test_durable_exact_index_cache_has_filename_identity(const char *filename);
int mylite_storage_test_durable_exact_index_cache_count(const char *filename);
int mylite_storage_test_durable_row_payload_cache_has_filename_identity(const char *filename);
#endif

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

typedef struct maintained_index_root_rollback_fixture {
    char *root;
    char *filename;
    char *journal_filename;
    char *transaction_journal_filename;
    unsigned long long row_1_id;
    unsigned long long row_2_id;
    unsigned long long primary_root_page;
    unsigned long long secondary_root_page;
    unsigned char primary_root[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char secondary_root[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
} maintained_index_root_rollback_fixture;

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

static const unsigned char k_maintained_root_definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
static const unsigned char k_maintained_root_row_1[] = {0x00U, 0x01U, 'a'};
static const unsigned char k_maintained_root_row_2[] = {0x00U, 0x02U, 'b'};
static const unsigned char k_maintained_root_row_3[] = {0x00U, 0x03U, 'c'};
static const unsigned char k_maintained_root_updated_row_2[] = {0x00U, 0x20U, 'd'};
static const unsigned char k_maintained_root_key_1[] = {0x01U, 0x00U, 0x00U, 0x00U};
static const unsigned char k_maintained_root_key_2[] = {0x02U, 0x00U, 0x00U, 0x00U};
static const unsigned char k_maintained_root_key_3[] = {0x03U, 0x00U, 0x00U, 0x00U};
static const unsigned char k_maintained_root_updated_key_2[] = {
    0x20U,
    0x00U,
    0x00U,
    0x00U,
};
static const unsigned char k_maintained_root_secondary_key_1[] = {
    0x11U,
    0x00U,
    0x00U,
    0x00U,
};
static const unsigned char k_maintained_root_secondary_key_2[] = {
    0x12U,
    0x00U,
    0x00U,
    0x00U,
};
static const unsigned char k_maintained_root_secondary_key_3[] = {
    0x13U,
    0x00U,
    0x00U,
    0x00U,
};
static const unsigned char k_maintained_root_updated_secondary_key_2[] = {
    0x22U,
    0x00U,
    0x00U,
    0x00U,
};
static const mylite_storage_index_entry k_maintained_root_row_1_entries[] = {
    {
        .size = sizeof(mylite_storage_index_entry),
        .index_number = 0U,
        .key = k_maintained_root_key_1,
        .key_size = sizeof(k_maintained_root_key_1),
    },
    {
        .size = sizeof(mylite_storage_index_entry),
        .index_number = 1U,
        .key = k_maintained_root_secondary_key_1,
        .key_size = sizeof(k_maintained_root_secondary_key_1),
    },
};
static const mylite_storage_index_entry k_maintained_root_row_2_entries[] = {
    {
        .size = sizeof(mylite_storage_index_entry),
        .index_number = 0U,
        .key = k_maintained_root_key_2,
        .key_size = sizeof(k_maintained_root_key_2),
    },
    {
        .size = sizeof(mylite_storage_index_entry),
        .index_number = 1U,
        .key = k_maintained_root_secondary_key_2,
        .key_size = sizeof(k_maintained_root_secondary_key_2),
    },
};
static const mylite_storage_index_entry k_maintained_root_row_3_entries[] = {
    {
        .size = sizeof(mylite_storage_index_entry),
        .index_number = 0U,
        .key = k_maintained_root_key_3,
        .key_size = sizeof(k_maintained_root_key_3),
    },
    {
        .size = sizeof(mylite_storage_index_entry),
        .index_number = 1U,
        .key = k_maintained_root_secondary_key_3,
        .key_size = sizeof(k_maintained_root_secondary_key_3),
    },
};
static const mylite_storage_index_entry k_maintained_root_updated_row_2_entries[] = {
    {
        .size = sizeof(mylite_storage_index_entry),
        .index_number = 0U,
        .key = k_maintained_root_updated_key_2,
        .key_size = sizeof(k_maintained_root_updated_key_2),
    },
    {
        .size = sizeof(mylite_storage_index_entry),
        .index_number = 1U,
        .key = k_maintained_root_updated_secondary_key_2,
        .key_size = sizeof(k_maintained_root_updated_secondary_key_2),
    },
};

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
static void test_multi_page_catalog_chain(void);
static void test_catalog_free_list_reuses_reclaimed_chain(void);
static void test_catalog_free_list_reuses_non_root_chain_run(void);
static void test_catalog_free_list_removes_exhausted_non_root_run(void);
static void test_catalog_free_list_appends_without_suitable_chain_run(void);
static void test_catalog_free_list_prepend_coalescing(void);
static void test_branch_free_list_prepend_coalescing(void);
static void test_catalog_free_list_append_coalescing(void);
static void test_branch_free_list_append_coalescing(void);
static void test_catalog_free_list_chain_prepend_coalescing(void);
static void test_catalog_free_list_chain_append_coalescing(void);
static void test_catalog_free_list_chain_bridge_coalescing(void);
static void test_branch_free_list_chain_prepend_coalescing(void);
static void test_free_list_reclaim_rejects_overlapping_run(void);
static void test_catalog_free_list_skips_active_statement_checkpoint(void);
static void test_rejects_corrupt_free_list_root(void);
static void test_rejects_corrupt_promoted_free_list_root(void);
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
static void test_active_dirty_page_rollback_restores_existing_page(void);
static void test_recovers_active_dirty_page_journal(void);
static void test_extends_recovery_journal_for_active_dirty_page(void);
static void test_preplanned_active_dirty_page_journal_set(void);
static void test_many_row_state_pages_scan(void);
static void test_active_live_row_validation_cache(void);
static void test_reusable_live_row_cache_clears_row_ids(void);
static void test_active_row_payload_cache(void);
static void test_active_row_payload_cache_large_window(void);
static void test_active_row_payload_cache_validates_update(void);
static void test_active_table_entry_cache_catalog_invalidation(void);
static void test_active_table_entry_cache_mutable_name_buffers(void);
static void test_filename_identity_scope_mutable_filename_buffer(void);
static void test_durable_lookup_caches_borrow_filename_identity(void);
static void test_active_row_payload_cache_many_replacements(void);
static void test_durable_live_row_cache(void);
static void test_deferred_durable_cache_retarget(void);
static void test_active_live_row_list_maintenance(void);
static void test_transaction_live_row_cache_nested_update_scope(void);
static void test_index_entries(void);
static void test_exact_index_cache_fixed_size_keys(void);
static void test_cached_exact_index_entryset_bulk_append(void);
static void test_full_index_read_seeds_exact_cache(void);
static void test_active_exact_index_cache_many_replacements(void);
static void test_active_exact_index_cache_after_mutation_creation(void);
static void test_additive_table_catalog_retargets_durable_exact_cache(void);
static void test_transaction_duplicate_probe_promotes_exact_cache(void);
static void test_active_exact_index_cache_retargets_omitted_unchanged_entry(void);
static void test_large_append_buffer_savepoint_rollback(void);
static void test_active_update_rewrite(void);
static void test_active_statement_update_row_scope(void);
static void test_active_single_index_same_size_rollback_after_checksum_refresh(void);
static void test_active_row_only_same_size_rollback_after_checksum_refresh(void);
static void test_unchanged_index_update_elision(void);
static void test_indexed_row_batch_cache_reuses_duplicates(void);
static void test_index_root_metadata(void);
static void test_maintained_index_root_page_format(void);
static void test_index_branch_page_format(void);
static void test_index_leaf_pages(void);
static void test_maintained_index_root_overflow_tail(void);
static void test_branch_arbitrary_child_removal(void);
static void test_branch_refold_child_count_delete(void);
static void test_branch_child_count_delete_collapse(void);
static void test_maintained_index_root_transaction_rollback(void);
static void test_batched_index_leaf_pages(void);
static void test_multi_page_index_leaf_pages(void);
static void test_branch_prefix_lookup_uses_root_page(void);
static void test_noncontiguous_branch_leaf_children(void);
static void test_multi_level_branch_navigation(void);
static void test_branch_page_full_root_split(void);
static void test_multi_page_index_leaf_duplicate_boundaries(void);
static void test_full_index_reads_use_leaf_runs(void);
static void assert_index_prefix_exists(
    const char *filename,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    int expected_exists
);
static void assert_index_prefix_exists_for_index(
    const char *filename,
    unsigned index_number,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    unsigned long long skip_row_id,
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
static void assert_prefix_index_entries(
    const char *filename,
    unsigned index_number,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    const unsigned char *const *expected_keys,
    size_t expected_key_size,
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
static void assert_index_root_page_type(
    const char *filename,
    unsigned long long root_page,
    unsigned expected_page_type
);
static void assert_free_list_run(
    const char *filename,
    unsigned long long page_id,
    unsigned long long expected_next_root_page,
    unsigned long long expected_run_start_page,
    unsigned long long expected_run_page_count
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
static void prepare_maintained_index_root_rollback_fixture(
    maintained_index_root_rollback_fixture *fixture,
    const char *filename_base
);
static void assert_maintained_index_root_fixture_initial_state(
    const maintained_index_root_rollback_fixture *fixture
);
static void assert_maintained_index_root_fixture_inserted_state(
    const maintained_index_root_rollback_fixture *fixture,
    unsigned long long row_3_id
);
static void snapshot_maintained_index_root_pages(
    const maintained_index_root_rollback_fixture *fixture,
    unsigned char *primary_root,
    unsigned char *secondary_root
);
static void assert_maintained_index_root_pages_match(
    const maintained_index_root_rollback_fixture *fixture,
    const unsigned char *expected_primary_root,
    const unsigned char *expected_secondary_root
);
static void assert_maintained_index_root_pages_changed(
    const maintained_index_root_rollback_fixture *fixture,
    const unsigned char *expected_primary_root,
    const unsigned char *expected_secondary_root
);
static void destroy_maintained_index_root_rollback_fixture(
    maintained_index_root_rollback_fixture *fixture
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
static void test_active_read_statement_context_detection(void);
static void test_read_checkpoint_snapshot_cache(void);
static void test_read_statement_storage_reuse(void);
static void test_read_statement_storage_reuse_replaces_filename(void);
static void test_read_statement_filename_identity_borrows_filename(void);
static void test_read_statement_index_entry_uses_table_entry_cache(void);
static void test_read_statement_indexed_row_uses_table_entry_cache(void);
static void test_read_statement_exact_index_cache_promotes_to_durable(void);
static void test_read_statement_multi_page_catalog_image_cache(void);
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
static void test_recovers_legacy_row_publication_journal(void);
static void test_recovers_multi_page_protected_journal(void);
static void test_recovers_catalog_publication_journal(void);
static void test_recovers_free_list_root_journal(void);
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
static void assert_find_indexed_row_into_equals(
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
static void assert_read_statement_index_lookup_uses_table_entry_cache(
    int fetch_payload,
    const char *filename_basename
);
static void assert_index_entry(
    const mylite_storage_index_entryset *index_entries,
    size_t entry_index,
    unsigned long long row_id,
    const unsigned char *key,
    size_t key_size
);
static void read_test_page(const char *path, unsigned long long page_id, unsigned char *out_page);
static void write_test_page(
    const char *path,
    unsigned long long page_id,
    const unsigned char *page
);
static void write_test_recovery_journal(
    const char *filename,
    const unsigned long long *page_ids,
    size_t page_count,
    const unsigned char pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE]
);
static void write_test_legacy_recovery_journal(
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
static void write_test_journal_header_page_with_format(
    unsigned char *page,
    const unsigned long long *page_ids,
    size_t page_count,
    unsigned version,
    unsigned checksum_offset
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
static void put_test_u32_be(unsigned char *page, size_t offset, unsigned value);
static void put_test_u64_le(unsigned char *page, size_t offset, unsigned long long value);
static unsigned get_test_u32_le(const unsigned char *page, size_t offset);
static unsigned long long get_test_u64_le(const unsigned char *page, size_t offset);
static unsigned long long checksum_test_page(const unsigned char *page, size_t checksum_offset);
static void rechecksum_test_index_root_page(unsigned char *page);
static void rechecksum_test_index_branch_page(unsigned char *page);
static void rechecksum_test_index_leaf_page(unsigned char *page);
static void flip_file_byte(const char *path, long offset);
static void write_test_header_page_count_and_free_list_root(
    const char *path,
    unsigned long long page_count,
    unsigned long long free_list_root_page
);
static void write_header_format_version(const char *path, unsigned value);
static int count_app_table(void *ctx, const char *schema_name, const char *table_name);
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
    test_multi_page_catalog_chain();
    test_catalog_free_list_reuses_reclaimed_chain();
    test_catalog_free_list_reuses_non_root_chain_run();
    test_catalog_free_list_removes_exhausted_non_root_run();
    test_catalog_free_list_appends_without_suitable_chain_run();
    test_catalog_free_list_prepend_coalescing();
    test_branch_free_list_prepend_coalescing();
    test_catalog_free_list_append_coalescing();
    test_branch_free_list_append_coalescing();
    test_catalog_free_list_chain_prepend_coalescing();
    test_catalog_free_list_chain_append_coalescing();
    test_catalog_free_list_chain_bridge_coalescing();
    test_branch_free_list_chain_prepend_coalescing();
    test_free_list_reclaim_rejects_overlapping_run();
    test_catalog_free_list_skips_active_statement_checkpoint();
    test_rejects_corrupt_free_list_root();
    test_rejects_corrupt_promoted_free_list_root();
    test_foreign_key_metadata_records();
    test_append_and_read_rows();
    test_append_and_read_large_row_payload();
    test_update_and_delete_rows();
    test_active_dirty_page_rollback_restores_existing_page();
    test_recovers_active_dirty_page_journal();
    test_extends_recovery_journal_for_active_dirty_page();
    test_preplanned_active_dirty_page_journal_set();
    test_many_row_state_pages_scan();
    test_active_live_row_validation_cache();
    test_reusable_live_row_cache_clears_row_ids();
    test_active_row_payload_cache();
    test_active_row_payload_cache_large_window();
    test_active_row_payload_cache_validates_update();
    test_active_table_entry_cache_catalog_invalidation();
    test_active_table_entry_cache_mutable_name_buffers();
    test_filename_identity_scope_mutable_filename_buffer();
    test_durable_lookup_caches_borrow_filename_identity();
    test_active_row_payload_cache_many_replacements();
    test_durable_live_row_cache();
    test_deferred_durable_cache_retarget();
    test_active_live_row_list_maintenance();
    test_transaction_live_row_cache_nested_update_scope();
    test_index_entries();
    test_exact_index_cache_fixed_size_keys();
    test_cached_exact_index_entryset_bulk_append();
    test_full_index_read_seeds_exact_cache();
    test_active_exact_index_cache_many_replacements();
    test_active_exact_index_cache_after_mutation_creation();
    test_additive_table_catalog_retargets_durable_exact_cache();
    test_transaction_duplicate_probe_promotes_exact_cache();
    test_active_exact_index_cache_retargets_omitted_unchanged_entry();
    test_large_append_buffer_savepoint_rollback();
    test_active_update_rewrite();
    test_active_statement_update_row_scope();
    test_active_single_index_same_size_rollback_after_checksum_refresh();
    test_active_row_only_same_size_rollback_after_checksum_refresh();
    test_unchanged_index_update_elision();
    test_indexed_row_batch_cache_reuses_duplicates();
    test_index_root_metadata();
    test_maintained_index_root_page_format();
    test_index_branch_page_format();
    test_index_leaf_pages();
    test_maintained_index_root_overflow_tail();
    test_branch_arbitrary_child_removal();
    test_branch_refold_child_count_delete();
    test_branch_child_count_delete_collapse();
    test_maintained_index_root_transaction_rollback();
    test_batched_index_leaf_pages();
    test_multi_page_index_leaf_pages();
    test_branch_prefix_lookup_uses_root_page();
    test_noncontiguous_branch_leaf_children();
    test_multi_level_branch_navigation();
    test_branch_page_full_root_split();
    test_multi_page_index_leaf_duplicate_boundaries();
    test_full_index_reads_use_leaf_runs();
    test_autoincrement_state();
    test_truncate_table_lifecycle();
    test_statement_checkpoints();
    test_read_statement_storage_session();
    test_active_read_statement_context_detection();
    test_read_checkpoint_snapshot_cache();
    test_read_statement_storage_reuse();
    test_read_statement_storage_reuse_replaces_filename();
    test_read_statement_filename_identity_borrows_filename();
    test_read_statement_index_entry_uses_table_entry_cache();
    test_read_statement_indexed_row_uses_table_entry_cache();
    test_read_statement_exact_index_cache_promotes_to_durable();
    test_read_statement_multi_page_catalog_image_cache();
    test_read_statement_file_cache_path_replacement();
    test_transaction_journals();
    test_transaction_owner_isolation();
    test_cross_process_transaction_read_snapshot();
    test_cleans_recovery_journal_after_mutations();
    test_recovers_row_publication_journal();
    test_recovers_legacy_row_publication_journal();
    test_recovers_multi_page_protected_journal();
    test_recovers_catalog_publication_journal();
    test_recovers_free_list_root_journal();
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
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 2ULL);
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
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 4ULL);
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

static void test_multi_page_catalog_chain(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};

    enum { table_count = 96 };

    char *root = make_temp_root();
    char *filename = path_join(root, "multi-page-catalog.mylite");
    char *journal_filename = journal_path(filename);
    char table_names[table_count][16] = {{0}};
    unsigned char catalog_root_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char catalog_next_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char saved_pages[MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES]
                             [MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {{0}};
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    mylite_storage_statement *statement = NULL;
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;
    unsigned listed_table_count = 0U;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    for (unsigned i = 0U; i < table_count; ++i) {
        assert(snprintf(table_names[i], sizeof(table_names[i]), "t%03u", i) > 0);
        mylite_storage_table_definition table_definition = {
            .size = sizeof(table_definition),
            .schema_name = "app",
            .table_name = table_names[i],
            .requested_engine_name = "MYLITE",
            .effective_engine_name = "MYLITE",
            .definition = definition,
            .definition_size = sizeof(definition),
        };
        assert(
            mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    read_test_page(filename, header.catalog_root_page, catalog_root_page);
    const unsigned long long next_page =
        get_test_u64_le(catalog_root_page, MYLITE_STORAGE_FORMAT_CATALOG_NEXT_PAGE_OFFSET);
    assert(next_page == header.catalog_root_page + 1ULL);
    assert(next_page < header.page_count);
    read_test_page(filename, next_page, catalog_next_page);
    assert(
        get_test_u64_le(catalog_next_page, MYLITE_STORAGE_FORMAT_CATALOG_PAGE_ID_OFFSET) ==
        next_page
    );
    assert(mylite_storage_table_exists(filename, "app", table_names[0]) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_table_exists(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_read_table_definition(
            filename,
            "app",
            table_names[table_count - 1U],
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == sizeof(definition));
    assert(memcmp(stored_definition, definition, sizeof(definition)) == 0);
    mylite_storage_free(stored_definition);

    assert(
        mylite_storage_list_tables(filename, "app", count_app_table, &listed_table_count) ==
        MYLITE_STORAGE_OK
    );
    assert(listed_table_count == table_count);

    const mylite_storage_index_root_definition index_root = {
        .size = sizeof(index_root),
        .schema_name = "app",
        .table_name = table_names[table_count - 1U],
        .index_number = 7U,
        .root_page = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT,
        .entry_count = 123ULL,
    };
    assert(mylite_storage_store_index_root(filename, &index_root) == MYLITE_STORAGE_OK);
    assert_index_root(
        filename,
        "app",
        table_names[table_count - 1U],
        7U,
        MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT,
        123ULL
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_drop_table(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_table_exists(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_table_exists(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_OK
    );

    const unsigned long long page_ids[] = {
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        header.catalog_root_page,
        header.free_list_root_page,
    };
    read_test_page(filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, saved_pages[0]);
    read_test_page(filename, header.catalog_root_page, saved_pages[1]);
    assert(header.free_list_root_page != 0ULL);
    read_test_page(filename, header.free_list_root_page, saved_pages[2]);
    assert(
        mylite_storage_drop_table(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_table_exists(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_NOTFOUND
    );
    write_test_recovery_journal(filename, page_ids, 3U, saved_pages);
    assert(
        mylite_storage_table_exists(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_OK
    );
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_catalog_free_list_reuses_reclaimed_chain(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};

    enum { table_count = 96 };

    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-reuse.mylite");
    char table_names[table_count][16] = {{0}};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    mylite_storage_header before_drop_header = {
        .size = sizeof(before_drop_header),
    };
    mylite_storage_header before_reuse_header = {
        .size = sizeof(before_reuse_header),
    };
    mylite_storage_header after_reuse_header = {
        .size = sizeof(after_reuse_header),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    for (unsigned i = 0U; i < table_count; ++i) {
        assert(snprintf(table_names[i], sizeof(table_names[i]), "t%03u", i) > 0);
        const mylite_storage_table_definition table_definition = {
            .size = sizeof(table_definition),
            .schema_name = "app",
            .table_name = table_names[i],
            .requested_engine_name = "MYLITE",
            .effective_engine_name = "MYLITE",
            .definition = definition,
            .definition_size = sizeof(definition),
        };
        assert(
            mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_open_header(filename, &before_drop_header) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_drop_table(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &before_reuse_header) == MYLITE_STORAGE_OK);
    assert(before_reuse_header.free_list_root_page != 0ULL);
    assert(before_reuse_header.catalog_root_page != before_drop_header.catalog_root_page);
    read_test_page(filename, before_reuse_header.free_list_root_page, free_list_page);
    assert(
        get_test_u32_le(free_list_page, MYLITE_STORAGE_FORMAT_FREE_LIST_PAGE_TYPE_OFFSET) ==
        MYLITE_STORAGE_FORMAT_FREE_LIST_PAGE_TYPE_FREE_RUN
    );
    assert(
        get_test_u64_le(free_list_page, MYLITE_STORAGE_FORMAT_FREE_LIST_RUN_PAGE_COUNT_OFFSET) >=
        2ULL
    );

    assert(mylite_storage_store_schema(filename, "archive") == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &after_reuse_header) == MYLITE_STORAGE_OK);
    assert(after_reuse_header.page_count == before_reuse_header.page_count);
    assert(after_reuse_header.catalog_root_page != before_reuse_header.catalog_root_page);
    assert(mylite_storage_schema_exists(filename, "archive") == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_table_exists(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_NOTFOUND
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_catalog_free_list_reuses_non_root_chain_run(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-non-root-reuse.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned long long first_page_id = 0ULL;
    int reused_run = 0;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 7ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 5ULL, 1ULL);
    write_test_page(filename, 2ULL, free_list_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 5ULL, 0ULL, 3ULL);
    write_test_page(filename, 5ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 8ULL, 2ULL);

    assert(
        mylite_storage_test_allocate_catalog_page_run(
            filename,
            2ULL,
            &first_page_id,
            &reused_run
        ) == MYLITE_STORAGE_OK
    );
    assert(reused_run == 1);
    assert(first_page_id == 6ULL);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 8ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 5ULL, 2ULL, 1ULL);
    assert_free_list_run(filename, 5ULL, 0ULL, 5ULL, 1ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_catalog_free_list_removes_exhausted_non_root_run(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-non-root-remove.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned long long first_page_id = 0ULL;
    int reused_run = 0;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 6ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 5ULL, 1ULL);
    write_test_page(filename, 2ULL, free_list_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 5ULL, 0ULL, 2ULL);
    write_test_page(filename, 5ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 7ULL, 2ULL);

    assert(
        mylite_storage_test_allocate_catalog_page_run(
            filename,
            2ULL,
            &first_page_id,
            &reused_run
        ) == MYLITE_STORAGE_OK
    );
    assert(reused_run == 1);
    assert(first_page_id == 5ULL);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 7ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 1ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_catalog_free_list_appends_without_suitable_chain_run(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-no-suitable-run.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned long long first_page_id = 0ULL;
    int reused_run = 0;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 6ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 5ULL, 1ULL);
    write_test_page(filename, 2ULL, free_list_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 5ULL, 0ULL, 1ULL);
    write_test_page(filename, 5ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 7ULL, 2ULL);

    assert(
        mylite_storage_test_allocate_catalog_page_run(
            filename,
            2ULL,
            &first_page_id,
            &reused_run
        ) == MYLITE_STORAGE_OK
    );
    assert(reused_run == 0);
    assert(first_page_id == 7ULL);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 7ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 5ULL, 2ULL, 1ULL);
    assert_free_list_run(filename, 5ULL, 0ULL, 5ULL, 1ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_catalog_free_list_prepend_coalescing(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-coalescing.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 5ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 4ULL, 0ULL, 2ULL);
    write_test_page(filename, 4ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 6ULL, 4ULL);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 6ULL);
    assert(header.free_list_root_page == 4ULL);
    assert_free_list_run(filename, 4ULL, 0ULL, 4ULL, 2ULL);

    assert(mylite_storage_test_reclaim_catalog_page_run(filename, 2ULL, 2ULL) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 6ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 4ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_branch_free_list_prepend_coalescing(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "branch-free-list-coalescing.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 4ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 3ULL, 0ULL, 2ULL);
    write_test_page(filename, 3ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 5ULL, 3ULL);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 5ULL);
    assert(header.free_list_root_page == 3ULL);
    assert_free_list_run(filename, 3ULL, 0ULL, 3ULL, 2ULL);

    assert(
        mylite_storage_test_reclaim_removed_branch_leaf_page(filename, 2ULL) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 5ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 3ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_catalog_free_list_append_coalescing(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-append-coalescing.mylite");
    char *journal_filename = journal_path(filename);
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    mylite_storage_statement *statement = NULL;
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 4ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 0ULL, 2ULL);
    write_test_page(filename, 2ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 5ULL, 2ULL);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 5ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 2ULL);

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_test_reclaim_catalog_page_run(filename, 4ULL, 1ULL) == MYLITE_STORAGE_OK);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 5ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 3ULL);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 5ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 2ULL);

    assert(mylite_storage_test_reclaim_catalog_page_run(filename, 4ULL, 1ULL) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 5ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 3ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
#endif
}

static void test_branch_free_list_append_coalescing(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "branch-free-list-append-coalescing.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 4ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 0ULL, 2ULL);
    write_test_page(filename, 2ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 5ULL, 2ULL);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 5ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 2ULL);

    assert(
        mylite_storage_test_reclaim_removed_branch_leaf_page(filename, 4ULL) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 5ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 3ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_catalog_free_list_chain_prepend_coalescing(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-chain-prepend-coalescing.mylite");
    char *journal_filename = journal_path(filename);
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    mylite_storage_statement *statement = NULL;
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 6ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 5ULL, 1ULL);
    write_test_page(filename, 2ULL, free_list_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 5ULL, 0ULL, 2ULL);
    write_test_page(filename, 5ULL, free_list_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 4ULL, 0ULL, 1ULL);
    write_test_page(filename, 4ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 7ULL, 2ULL);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 7ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 5ULL, 2ULL, 1ULL);
    assert_free_list_run(filename, 5ULL, 0ULL, 5ULL, 2ULL);

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_test_reclaim_catalog_page_run(filename, 4ULL, 1ULL) == MYLITE_STORAGE_OK);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 7ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 4ULL, 2ULL, 1ULL);
    assert_free_list_run(filename, 4ULL, 0ULL, 4ULL, 3ULL);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 7ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 5ULL, 2ULL, 1ULL);
    assert_free_list_run(filename, 5ULL, 0ULL, 5ULL, 2ULL);

    assert(mylite_storage_test_reclaim_catalog_page_run(filename, 4ULL, 1ULL) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 7ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 4ULL, 2ULL, 1ULL);
    assert_free_list_run(filename, 4ULL, 0ULL, 4ULL, 3ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
#endif
}

static void test_catalog_free_list_chain_append_coalescing(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-chain-append-coalescing.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 6ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 5ULL, 2ULL, 1ULL);
    write_test_page(filename, 5ULL, free_list_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 0ULL, 1ULL);
    write_test_page(filename, 2ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 7ULL, 5ULL);

    assert(mylite_storage_test_reclaim_catalog_page_run(filename, 3ULL, 1ULL) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 7ULL);
    assert(header.free_list_root_page == 5ULL);
    assert_free_list_run(filename, 5ULL, 2ULL, 5ULL, 1ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 2ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_catalog_free_list_chain_bridge_coalescing(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-chain-bridge-coalescing.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 6ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 5ULL, 1ULL);
    write_test_page(filename, 2ULL, free_list_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 5ULL, 0ULL, 1ULL);
    write_test_page(filename, 5ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 7ULL, 2ULL);

    assert(mylite_storage_test_reclaim_catalog_page_run(filename, 3ULL, 2ULL) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 7ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 4ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_branch_free_list_chain_prepend_coalescing(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "branch-free-list-chain-prepend-coalescing.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 6ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 5ULL, 1ULL);
    write_test_page(filename, 2ULL, free_list_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 5ULL, 0ULL, 2ULL);
    write_test_page(filename, 5ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 7ULL, 2ULL);

    assert(
        mylite_storage_test_reclaim_removed_branch_leaf_page(filename, 4ULL) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 7ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 4ULL, 2ULL, 1ULL);
    assert_free_list_run(filename, 4ULL, 0ULL, 4ULL, 3ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_free_list_reclaim_rejects_overlapping_run(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "free-list-overlapping-reclaim.mylite");
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char empty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_test_page(filename, 5ULL, empty_page);
    mylite_storage_test_encode_free_list_page(free_list_page, 2ULL, 0ULL, 3ULL);
    write_test_page(filename, 2ULL, free_list_page);
    write_test_header_page_count_and_free_list_root(filename, 6ULL, 2ULL);

    assert(
        mylite_storage_test_reclaim_catalog_page_run(filename, 3ULL, 1ULL) == MYLITE_STORAGE_CORRUPT
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == 6ULL);
    assert(header.free_list_root_page == 2ULL);
    assert_free_list_run(filename, 2ULL, 0ULL, 2ULL, 3ULL);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_catalog_free_list_skips_active_statement_checkpoint(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "catalog-free-list-active-statement.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header before_statement_header = {
        .size = sizeof(before_statement_header),
    };
    mylite_storage_header after_commit_header = {
        .size = sizeof(after_commit_header),
    };
    mylite_storage_statement *statement = NULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &before_statement_header) == MYLITE_STORAGE_OK);
    assert(before_statement_header.free_list_root_page != 0ULL);

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_schema(filename, "archive") == MYLITE_STORAGE_OK);
    assert(mylite_storage_commit_statement(statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &after_commit_header) == MYLITE_STORAGE_OK);
    assert(after_commit_header.page_count > before_statement_header.page_count);
    assert(mylite_storage_schema_exists(filename, "archive") == MYLITE_STORAGE_OK);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_free_list_root(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-free-list.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {
        .size = sizeof(header),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.free_list_root_page != 0ULL);

    flip_file_byte(
        filename,
        (long)(header.free_list_root_page * MYLITE_STORAGE_FORMAT_PAGE_SIZE) +
            (long)MYLITE_STORAGE_FORMAT_FREE_LIST_MAGIC_OFFSET
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_CORRUPT);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_promoted_free_list_root(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-promoted-free-list.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    unsigned char free_list_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.free_list_root_page != 0ULL);

    read_test_page(filename, header.free_list_root_page, free_list_page);
    assert(
        get_test_u64_le(free_list_page, MYLITE_STORAGE_FORMAT_FREE_LIST_RUN_PAGE_COUNT_OFFSET) ==
        1ULL
    );
    const unsigned long long corrupt_next_root_page = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
    assert(corrupt_next_root_page < header.page_count);
    assert(corrupt_next_root_page != header.catalog_root_page);
    put_test_u64_le(
        free_list_page,
        MYLITE_STORAGE_FORMAT_FREE_LIST_NEXT_ROOT_PAGE_OFFSET,
        corrupt_next_root_page
    );
    put_test_u64_le(
        free_list_page,
        MYLITE_STORAGE_FORMAT_FREE_LIST_CHECKSUM_OFFSET,
        checksum_test_page(free_list_page, MYLITE_STORAGE_FORMAT_FREE_LIST_CHECKSUM_OFFSET)
    );
    write_test_page(filename, header.free_list_root_page, free_list_page);

    assert(mylite_storage_store_schema(filename, "archive") == MYLITE_STORAGE_CORRUPT);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert_file_size_matches_header(filename);

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
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 6ULL);

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
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 7ULL);
    assert(mylite_storage_read_rows(filename, "app", "payloads", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_size == 0U);
    assert(rows.row_count == 2U);
    assert(rows.row_bytes == sizeof(small_row) + large_row_size);
    assert(rows.row_offsets[0] == 0U);
    assert(rows.row_sizes[0] == sizeof(small_row));
    assert(rows.row_ids[0] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 2ULL);
    assert(rows.row_offsets[1] == sizeof(small_row));
    assert(rows.row_sizes[1] == large_row_size);
    assert(rows.row_ids[1] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 6ULL);
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
    assert(new_row_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 5ULL);
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

static void test_active_dirty_page_rollback_restores_existing_page(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'r', 'o', 'w'};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-dirty-page-rollback.mylite");
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
    unsigned char before_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char dirty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char restored_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_statement *statement = NULL;
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
            NULL,
            0U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    read_test_page(filename, row_id, before_page);

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_test_flip_active_page_byte(
            filename,
            row_id,
            MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET
        ) == MYLITE_STORAGE_OK
    );
    assert(access(journal_filename, F_OK) == 0);
    read_test_page(filename, row_id, dirty_page);
    assert(memcmp(before_page, dirty_page, sizeof(before_page)) != 0);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);

    read_test_page(filename, row_id, restored_page);
    assert(memcmp(before_page, restored_page, sizeof(before_page)) == 0);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    assert(rows.row_size == sizeof(row));
    assert(memcmp(rows.rows, row, sizeof(row)) == 0);
    mylite_storage_free_rowset(&rows);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
#endif
}

static void test_recovers_active_dirty_page_journal(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'r', 'o', 'w'};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-dirty-page-journal-recovery.mylite");
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
    unsigned char before_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char dirty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char recovered_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
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
            NULL,
            0U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    read_test_page(filename, row_id, before_page);

    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        mylite_storage_statement *statement = NULL;
        if (mylite_storage_begin_statement(filename, &statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_test_flip_active_page_byte(
                filename,
                row_id,
                MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    read_test_page(filename, row_id, dirty_page);
    assert(memcmp(before_page, dirty_page, sizeof(before_page)) != 0);

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    assert(rows.row_size == sizeof(row));
    assert(memcmp(rows.rows, row, sizeof(row)) == 0);
    mylite_storage_free_rowset(&rows);
    assert_file_missing(journal_filename);

    read_test_page(filename, row_id, recovered_page);
    assert(memcmp(before_page, recovered_page, sizeof(before_page)) == 0);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
#endif
}

static void test_extends_recovery_journal_for_active_dirty_page(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'o', 'n', 'e'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 't', 'w', 'o'};
    char *root = make_temp_root();
    char *filename = path_join(root, "extended-active-dirty-page-journal.mylite");
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
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_statement *statement = NULL;
    unsigned char before_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char dirty_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char restored_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned long long row_1_id = 0ULL;

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
    read_test_page(filename, row_1_id, before_page);

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row_2, sizeof(row_2)) ==
        MYLITE_STORAGE_OK
    );
    assert(access(journal_filename, F_OK) == 0);
    assert(
        mylite_storage_test_flip_active_page_byte(
            filename,
            row_1_id,
            MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET
        ) == MYLITE_STORAGE_OK
    );
    read_test_page(filename, row_1_id, dirty_page);
    assert(memcmp(before_page, dirty_page, sizeof(before_page)) != 0);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);
    read_test_page(filename, row_1_id, restored_page);
    assert(memcmp(before_page, restored_page, sizeof(before_page)) == 0);

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 1U);
    assert(rows.row_size == sizeof(row_1));
    assert(memcmp(rows.rows, row_1, sizeof(row_1)) == 0);
    mylite_storage_free_rowset(&rows);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
#endif
}

static void test_preplanned_active_dirty_page_journal_set(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'o', 'n', 'e'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 't', 'w', 'o'};
    char *root = make_temp_root();
    char *filename = path_join(root, "preplanned-active-dirty-pages.mylite");
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
    unsigned char before_1[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char before_2[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char dirty_1[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char dirty_2[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char restored_1[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char restored_2[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_statement *statement = NULL;
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
    read_test_page(filename, row_1_id, before_1);
    read_test_page(filename, row_2_id, before_2);

    const unsigned long long page_ids[] = {row_1_id, row_2_id};
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_test_protect_active_dirty_pages(
            filename,
            page_ids,
            sizeof(page_ids) / sizeof(page_ids[0])
        ) == MYLITE_STORAGE_OK
    );
    assert(access(journal_filename, F_OK) == 0);
    assert(
        mylite_storage_test_flip_active_page_byte(
            filename,
            row_1_id,
            MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_test_flip_active_page_byte(
            filename,
            row_2_id,
            MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET
        ) == MYLITE_STORAGE_OK
    );
    read_test_page(filename, row_1_id, dirty_1);
    read_test_page(filename, row_2_id, dirty_2);
    assert(memcmp(before_1, dirty_1, sizeof(before_1)) != 0);
    assert(memcmp(before_2, dirty_2, sizeof(before_2)) != 0);

    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);
    read_test_page(filename, row_1_id, restored_1);
    read_test_page(filename, row_2_id, restored_2);
    assert(memcmp(before_1, restored_1, sizeof(before_1)) == 0);
    assert(memcmp(before_2, restored_2, sizeof(before_2)) == 0);

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 2U);
    assert(rows.row_size == sizeof(row_1));
    assert(memcmp(rows.rows + rows.row_offsets[0], row_1, sizeof(row_1)) == 0);
    assert(memcmp(rows.rows + rows.row_offsets[1], row_2, sizeof(row_2)) == 0);
    mylite_storage_free_rowset(&rows);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
#endif
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

static void test_active_table_entry_cache_mutable_name_buffers(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char posts_row[] = {0x00U, 0x11U, 'p'};
    static const unsigned char articles_row[] = {0x00U, 0x11U, 'a'};
    static const unsigned char key[] = {0x11U};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-table-entry-cache-mutable-name-buffers.mylite");
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
    unsigned long long posts_row_id = 0ULL;
    unsigned long long articles_row_id = 0ULL;
    unsigned long long found_row_id = 0ULL;
    unsigned char *stored_row = NULL;
    size_t stored_row_size = 0U;
    char schema_name[16];
    char table_name[16];

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    table_definition.schema_name = "blog";
    table_definition.table_name = "articles";
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            posts_row,
            sizeof(posts_row),
            &row_entry,
            1U,
            &posts_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "blog",
            "articles",
            articles_row,
            sizeof(articles_row),
            &row_entry,
            1U,
            &articles_row_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    strcpy(schema_name, "app");
    strcpy(table_name, "posts");
    assert(
        mylite_storage_find_indexed_row(
            filename,
            schema_name,
            table_name,
            0U,
            key,
            sizeof(key),
            &found_row_id,
            &stored_row,
            &stored_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == posts_row_id);
    assert(stored_row_size == sizeof(posts_row));
    assert(memcmp(stored_row, posts_row, sizeof(posts_row)) == 0);
    mylite_storage_free(stored_row);
    stored_row = NULL;
    stored_row_size = 0U;
    found_row_id = 0ULL;

    strcpy(schema_name, "blog");
    strcpy(table_name, "articles");
    assert(
        mylite_storage_find_indexed_row(
            filename,
            schema_name,
            table_name,
            0U,
            key,
            sizeof(key),
            &found_row_id,
            &stored_row,
            &stored_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == articles_row_id);
    assert(stored_row_size == sizeof(articles_row));
    assert(memcmp(stored_row, articles_row, sizeof(articles_row)) == 0);
    mylite_storage_free(stored_row);
    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_filename_identity_scope_mutable_filename_buffer(void) {
    char *root = make_temp_root();
    char *first_filename = path_join(root, "filename-identity-first.mylite");
    char *second_filename = path_join(root, "filename-identity-second.mylite");
    char filename_buffer[PATH_MAX];
    mylite_storage_filename_identity_scope scope = {0};
    mylite_storage_statement *transaction = NULL;
    int written = 0;

    assert(mylite_storage_create_empty(first_filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_create_empty(second_filename) == MYLITE_STORAGE_OK);

    written = snprintf(filename_buffer, sizeof(filename_buffer), "%s", first_filename);
    assert(written > 0 && (size_t)written < sizeof(filename_buffer));
    mylite_storage_begin_filename_identity_scope(filename_buffer, &scope);
    assert(mylite_storage_begin_transaction(filename_buffer, &transaction) == MYLITE_STORAGE_OK);
    assert(mylite_storage_statement_active(filename_buffer));
    mylite_storage_end_filename_identity_scope(&scope);

    written = snprintf(filename_buffer, sizeof(filename_buffer), "%s", second_filename);
    assert(written > 0 && (size_t)written < sizeof(filename_buffer));
    assert(!mylite_storage_statement_active(filename_buffer));

    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert(unlink(first_filename) == 0);
    assert(unlink(second_filename) == 0);
    assert(rmdir(root) == 0);
    free(second_filename);
    free(first_filename);
    free(root);
}

static void test_durable_lookup_caches_borrow_filename_identity(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x11U, 'a'};
    static const unsigned char key[] = {0x11U};
    char *root = make_temp_root();
    char *unscoped_filename = path_join(root, "durable-cache-identity-unscoped.mylite");
    char *scoped_filename = path_join(root, "durable-cache-identity-scoped.mylite");
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
    unsigned long long unscoped_row_id = 0ULL;
    unsigned long long scoped_row_id = 0ULL;
    unsigned long long found_row_id = 0ULL;
    size_t found_row_size = 0U;
    unsigned char found_row[sizeof(row)] = {0};
    mylite_storage_filename_identity_scope filename_scope = {0};

    assert(mylite_storage_create_empty(unscoped_filename) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(unscoped_filename, &table_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            unscoped_filename,
            "app",
            "posts",
            row,
            sizeof(row),
            &index_entry,
            1U,
            &unscoped_row_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_create_empty(scoped_filename) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(scoped_filename, &table_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            scoped_filename,
            "app",
            "posts",
            row,
            sizeof(row),
            &index_entry,
            1U,
            &scoped_row_id
        ) == MYLITE_STORAGE_OK
    );

    assert(
        mylite_storage_find_indexed_row_into(
            unscoped_filename,
            "app",
            "posts",
            0U,
            key,
            sizeof(key),
            &found_row_id,
            found_row,
            sizeof(found_row),
            &found_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == unscoped_row_id);
    assert(found_row_size == sizeof(row));
    assert(memcmp(found_row, row, sizeof(row)) == 0);
    assert(!mylite_storage_test_durable_exact_index_cache_has_filename_identity(unscoped_filename));
    assert(!mylite_storage_test_durable_row_payload_cache_has_filename_identity(unscoped_filename));

    found_row_id = 0ULL;
    found_row_size = 0U;
    memset(found_row, 0, sizeof(found_row));
    mylite_storage_begin_filename_identity_scope(scoped_filename, &filename_scope);
    assert(
        mylite_storage_find_indexed_row_into(
            scoped_filename,
            "app",
            "posts",
            0U,
            key,
            sizeof(key),
            &found_row_id,
            found_row,
            sizeof(found_row),
            &found_row_size
        ) == MYLITE_STORAGE_OK
    );
    mylite_storage_end_filename_identity_scope(&filename_scope);
    assert(found_row_id == scoped_row_id);
    assert(found_row_size == sizeof(row));
    assert(memcmp(found_row, row, sizeof(row)) == 0);
    assert(mylite_storage_test_durable_exact_index_cache_has_filename_identity(scoped_filename));
    assert(mylite_storage_test_durable_row_payload_cache_has_filename_identity(scoped_filename));
    mylite_storage_clear_thread_caches();
    assert(!mylite_storage_test_durable_exact_index_cache_has_filename_identity(scoped_filename));
    assert(!mylite_storage_test_durable_row_payload_cache_has_filename_identity(scoped_filename));

    assert(unlink(unscoped_filename) == 0);
    assert(unlink(scoped_filename) == 0);
    assert(rmdir(root) == 0);
    free(scoped_filename);
    free(unscoped_filename);
    free(root);
#endif
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

static void test_transaction_live_row_cache_nested_update_scope(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x11U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x22U, 'b'};
    static const unsigned char row_3[] = {0x00U, 0x33U, 'c'};
    static const unsigned char row_4[] = {0x00U, 0x44U, 'd'};
    char *root = make_temp_root();
    char *filename = path_join(root, "transaction-live-row-cache.mylite");
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

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(!mylite_storage_test_statement_has_live_row_cache(transaction));
    assert(!mylite_storage_test_statement_has_row_state_map_cache(transaction));

    assert(mylite_storage_begin_nested_statement(transaction, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_1_id,
            row_2,
            sizeof(row_2),
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(row_2_id != 0ULL);
    assert(mylite_storage_test_statement_has_live_row_cache(transaction));
    assert(mylite_storage_test_statement_has_row_state_map_cache(transaction));
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    savepoint = NULL;
    assert(!mylite_storage_test_statement_has_live_row_cache(transaction));
    assert(!mylite_storage_test_statement_has_row_state_map_cache(transaction));
    assert_row_equals(filename, row_1_id, row_1, sizeof(row_1));
    if (row_2_id != row_1_id) {
        assert_row_not_found(filename, row_2_id);
    }

    assert(mylite_storage_begin_nested_statement(transaction, &savepoint) == MYLITE_STORAGE_OK);
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
    assert(row_3_id != 0ULL);
    assert(mylite_storage_test_statement_has_live_row_cache(transaction));
    assert(mylite_storage_commit_statement(savepoint) == MYLITE_STORAGE_OK);
    savepoint = NULL;
    assert(mylite_storage_test_statement_has_live_row_cache(transaction));

    assert(mylite_storage_begin_nested_statement(transaction, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row(
            filename,
            "app",
            "posts",
            row_3_id,
            row_4,
            sizeof(row_4),
            &row_4_id
        ) == MYLITE_STORAGE_OK
    );
    assert(row_4_id != 0ULL);
    assert(mylite_storage_commit_statement(savepoint) == MYLITE_STORAGE_OK);
    savepoint = NULL;
    assert(mylite_storage_test_statement_has_live_row_cache(transaction));
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    transaction = NULL;

    if (row_1_id != row_4_id) {
        assert_row_not_found(filename, row_1_id);
    }
    if (row_3_id != row_4_id) {
        assert_row_not_found(filename, row_3_id);
    }
    assert_row_equals(filename, row_4_id, row_4, sizeof(row_4));

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
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
    assert_find_indexed_row_into_equals(
        filename,
        0U,
        key_1,
        sizeof(key_1),
        ctx.row_1_id,
        row_1,
        sizeof(row_1)
    );
    unsigned long long fixed_row_id = 0ULL;
    unsigned char fixed_row[sizeof(row_1)];
    size_t fixed_row_size = 0U;
    assert(
        mylite_storage_find_indexed_row_into(
            filename,
            "app",
            "posts",
            0U,
            key_1,
            sizeof(key_1),
            &fixed_row_id,
            fixed_row,
            sizeof(fixed_row) - 1U,
            &fixed_row_size
        ) == MYLITE_STORAGE_FULL
    );
    assert(fixed_row_id == 0ULL);
    assert(fixed_row_size == sizeof(row_1));
    assert(
        mylite_storage_find_indexed_row_into(
            filename,
            "app",
            "posts",
            0U,
            key_1,
            sizeof(key_1),
            &fixed_row_id,
            NULL,
            sizeof(fixed_row),
            &fixed_row_size
        ) == MYLITE_STORAGE_MISUSE
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
    fixed_row_id = 123ULL;
    fixed_row_size = 123U;
    assert(
        mylite_storage_find_indexed_row_into(
            filename,
            "app",
            "posts",
            0U,
            key_9,
            sizeof(key_9),
            &fixed_row_id,
            fixed_row,
            sizeof(fixed_row),
            &fixed_row_size
        ) == MYLITE_STORAGE_NOTFOUND
    );
    assert(fixed_row_id == 0ULL);
    assert(fixed_row_size == 0U);
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

static void test_full_index_read_seeds_exact_cache(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x01U, 'a'};
    static const unsigned char row_2[] = {0x02U, 'b'};
    static const unsigned char key_1[] = {'a', 'b', 'c'};
    static const unsigned char key_1_prefix[] = {'a', 'b'};
    static const unsigned char key_2[] = {'d', 'e', 'f'};
    char *root = make_temp_root();
    char *filename = path_join(root, "full-index-read-exact-cache.mylite");
    int owner = 0;
    mylite_storage_statement *read_statement = NULL;
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
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
    mylite_storage_index_entry row_1_entry = {
        .size = sizeof(row_1_entry),
        .index_number = 1U,
        .key = key_1,
        .key_size = sizeof(key_1),
    };
    mylite_storage_index_entry row_2_entry = {
        .size = sizeof(row_2_entry),
        .index_number = 1U,
        .key = key_2,
        .key_size = sizeof(key_2),
    };
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;

    mylite_storage_clear_thread_caches();
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
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2,
            sizeof(row_2),
            &row_2_entry,
            1U,
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );

    assert(
        mylite_storage_read_exact_index_entries(
            filename,
            "app",
            "posts",
            1U,
            key_1_prefix,
            sizeof(key_1_prefix),
            &entries
        ) == MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 0U);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);
    mylite_storage_free_index_entryset(&entries);
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };

    mylite_storage_set_context_owner(&owner);
    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(mylite_storage_test_statement_exact_index_cache_count(read_statement) == 0);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);

    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 1U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 2U);
    assert_index_entry(&entries, 0U, row_1_id, key_1, sizeof(key_1));
    assert_index_entry(&entries, 1U, row_2_id, key_2, sizeof(key_2));
    assert(mylite_storage_test_statement_exact_index_cache_count(read_statement) == 1);
    mylite_storage_free_index_entryset(&entries);

    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 1U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 2U);
    assert_index_entry(&entries, 0U, row_1_id, key_1, sizeof(key_1));
    assert_index_entry(&entries, 1U, row_2_id, key_2, sizeof(key_2));
    assert(mylite_storage_test_statement_exact_index_cache_count(read_statement) == 1);
    mylite_storage_free_index_entryset(&entries);

    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    mylite_storage_set_context_owner(NULL);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 2);

    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 1U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 2U);
    assert_index_entry(&entries, 0U, row_1_id, key_1, sizeof(key_1));
    assert_index_entry(&entries, 1U, row_2_id, key_2, sizeof(key_2));
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 2);
    mylite_storage_free_index_entryset(&entries);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_1_id) == MYLITE_STORAGE_OK);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 0);

    mylite_storage_clear_thread_caches();
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
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
        assert(final_row_ids[i] == replacement_row_ids[i]);
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

static void test_active_exact_index_cache_after_mutation_creation(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char primary_key[] = {'i', 'd', '1'};
    static const unsigned char old_secondary_key[] = {'o', 'l', 'd'};
    static const unsigned char new_secondary_key[] = {'n', 'e', 'w'};
    static const unsigned char row[] = {0x01U, 0x02U, 0x03U};
    static const unsigned char updated_row[] = {0x04U, 0x05U, 0x06U};
    unsigned char found_row[sizeof(updated_row)] = {0};
    size_t found_row_size = 0U;
    unsigned long long row_id = 0ULL;
    unsigned long long updated_row_id = 0ULL;
    unsigned long long found_row_id = 0ULL;
    char *root = make_temp_root();
    char *filename = path_join(root, "active-exact-index-cache-after-mutation.mylite");
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
        {.size = sizeof(entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(entries[1]),
         .index_number = 1U,
         .key = old_secondary_key,
         .key_size = sizeof(old_secondary_key)},
    };
    mylite_storage_index_entry updated_entries[] = {
        {.size = sizeof(updated_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(updated_entries[1]),
         .index_number = 1U,
         .key = new_secondary_key,
         .key_size = sizeof(new_secondary_key)},
    };
    mylite_storage_statement *transaction = NULL;

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

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_id,
            updated_row,
            sizeof(updated_row),
            updated_entries,
            sizeof(updated_entries) / sizeof(updated_entries[0]),
            &updated_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_test_statement_exact_index_cache_count(transaction) == 0);

    assert(
        mylite_storage_find_indexed_row_in_statement_into(
            transaction,
            "app",
            "posts",
            0U,
            primary_key,
            sizeof(primary_key),
            &found_row_id,
            found_row,
            sizeof(found_row),
            &found_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == updated_row_id);
    assert(found_row_size == sizeof(updated_row));
    assert(memcmp(found_row, updated_row, sizeof(updated_row)) == 0);
    assert(mylite_storage_test_statement_exact_index_cache_count(transaction) == 1);

    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_additive_table_catalog_retargets_durable_exact_cache(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x01U, 0x02U, 0x03U};
    static const unsigned char primary_key[] = {'i', 'd', '1'};
    unsigned long long row_id = 0ULL;
    unsigned long long found_row_id = 0ULL;
    char *root = make_temp_root();
    char *filename = path_join(root, "additive-table-cache-retarget.mylite");
    const mylite_storage_table_definition posts_definition = {
        .size = sizeof(posts_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    const mylite_storage_table_definition comments_definition = {
        .size = sizeof(comments_definition),
        .schema_name = "app",
        .table_name = "comments",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    const mylite_storage_schema_definition schema_definition = {
        .size = sizeof(schema_definition),
        .schema_name = "app",
        .default_character_set_name = "utf8mb4",
        .default_collation_name = "utf8mb4_uca1400_ai_ci",
        .schema_comment = "app schema",
    };
    const mylite_storage_schema_definition rollback_schema_definition = {
        .size = sizeof(rollback_schema_definition),
        .schema_name = "rollback_app",
        .default_character_set_name = "utf8mb4",
        .default_collation_name = "utf8mb4_uca1400_ai_ci",
        .schema_comment = "rolled back schema",
    };
    const mylite_storage_index_entry primary_entry = {
        .size = sizeof(primary_entry),
        .index_number = 0U,
        .key = primary_key,
        .key_size = sizeof(primary_key),
    };
    mylite_storage_statement *statement = NULL;

    mylite_storage_clear_thread_caches();
    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &posts_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row,
            sizeof(row),
            &primary_entry,
            1U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 0);
    assert_index_entry_lookup(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        MYLITE_STORAGE_OK,
        row_id
    );
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_schema_definition(filename, &rollback_schema_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    found_row_id = 0ULL;
    assert(
        mylite_storage_find_index_entry(
            filename,
            "app",
            "posts",
            0U,
            primary_key,
            sizeof(primary_key),
            &found_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == row_id);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_schema_definition(filename, &schema_definition) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_commit_statement(statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);
    assert(
        mylite_storage_store_table_definition(filename, &comments_definition) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);
    assert(
        mylite_storage_find_index_entry(
            filename,
            "app",
            "posts",
            0U,
            primary_key,
            sizeof(primary_key),
            &found_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == row_id);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);

    mylite_storage_clear_thread_caches();
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_transaction_duplicate_probe_promotes_exact_cache(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x01U, 0x02U, 0x03U};
    static const unsigned char primary_key[] = {'i', 'd', '1'};
    unsigned long long row_id = 0ULL;
    unsigned long long found_row_id = 0ULL;
    char *root = make_temp_root();
    char *filename = path_join(root, "transaction-duplicate-probe-cache.mylite");
    const mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    const mylite_storage_index_entry primary_entry = {
        .size = sizeof(primary_entry),
        .index_number = 0U,
        .key = primary_key,
        .key_size = sizeof(primary_key),
    };
    mylite_storage_statement *transaction = NULL;

    mylite_storage_clear_thread_caches();
    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_find_index_entry(
            filename,
            "app",
            "posts",
            0U,
            primary_key,
            sizeof(primary_key),
            &found_row_id
        ) == MYLITE_STORAGE_NOTFOUND
    );
    assert(found_row_id == 0ULL);
    assert(mylite_storage_test_statement_exact_index_cache_count(transaction) == 1);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row,
            sizeof(row),
            &primary_entry,
            1U,
            &row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_test_statement_exact_index_cache_count(transaction) == 1);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);
    assert_index_entry_lookup(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        MYLITE_STORAGE_OK,
        row_id
    );
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);

    mylite_storage_clear_thread_caches();
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_active_exact_index_cache_retargets_omitted_unchanged_entry(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x01U, 'r', 'o', 'w'};
    static const unsigned char updated_row[] = {0x02U, 'u', 'p', 'd', 'a', 't', 'e', 'd'};
    static const unsigned char primary_key[] = {'i', 'd', '1'};
    static const unsigned char old_secondary_key[] = {'o', 'l', 'd'};
    static const unsigned char new_secondary_key[] = {'n', 'e', 'w'};
    static const unsigned char changed_entries[] = {1U};
    unsigned char found_row[sizeof(updated_row)] = {0};
    size_t found_row_size = 0U;
    unsigned long long row_id = 0ULL;
    unsigned long long updated_row_id = 0ULL;
    unsigned long long found_row_id = 0ULL;
    char *root = make_temp_root();
    char *filename = path_join(root, "active-exact-cache-omitted-entry.mylite");
    const mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    const mylite_storage_index_entry initial_entries[] = {
        {.size = sizeof(initial_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(initial_entries[1]),
         .index_number = 1U,
         .key = old_secondary_key,
         .key_size = sizeof(old_secondary_key)},
    };
    const mylite_storage_index_entry changed_secondary_entry = {
        .size = sizeof(changed_secondary_entry),
        .index_number = 1U,
        .key = new_secondary_key,
        .key_size = sizeof(new_secondary_key),
    };
    mylite_storage_statement *transaction = NULL;

    mylite_storage_clear_thread_caches();
    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row,
            sizeof(row),
            initial_entries,
            sizeof(initial_entries) / sizeof(initial_entries[0]),
            &row_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_find_indexed_row_in_statement_into(
            transaction,
            "app",
            "posts",
            0U,
            primary_key,
            sizeof(primary_key),
            &found_row_id,
            found_row,
            sizeof(found_row),
            &found_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == row_id);
    assert(found_row_size == sizeof(row));
    assert(memcmp(found_row, row, sizeof(row)) == 0);
    assert(mylite_storage_test_statement_exact_index_cache_count(transaction) == 1);

    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            row_id,
            updated_row,
            sizeof(updated_row),
            &changed_secondary_entry,
            1U,
            changed_entries,
            &updated_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(updated_row_id != row_id);
    assert(mylite_storage_test_statement_exact_index_cache_count(transaction) == 1);

    memset(found_row, 0, sizeof(found_row));
    found_row_id = 0ULL;
    found_row_size = 0U;
    assert(
        mylite_storage_find_indexed_row_in_statement_into(
            transaction,
            "app",
            "posts",
            0U,
            primary_key,
            sizeof(primary_key),
            &found_row_id,
            found_row,
            sizeof(found_row),
            &found_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == updated_row_id);
    assert(found_row_size == sizeof(updated_row));
    assert(memcmp(found_row, updated_row, sizeof(updated_row)) == 0);
    assert(mylite_storage_test_statement_exact_index_cache_count(transaction) == 1);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);

    mylite_storage_clear_thread_caches();
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
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

static void test_active_statement_update_row_scope(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char primary_key[] = {0x10U, 0x01U};
    static const unsigned char initial_row[] = {0x01U, 'i', 'n', 'i', 't'};
    static const unsigned char updated_row[] = {0x02U, 'u', 'p', 'd', 't'};
    static const unsigned char final_row[] = {0x03U, 'f', 'i', 'n', 'l'};
    static const unsigned char unchanged_entries[] = {0U};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-statement-update-scope.mylite");
    char *other_filename = path_join(root, "other.mylite");
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
        {.size = sizeof(entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
    };
    mylite_storage_statement *statement = NULL;
    unsigned long long row_id = 0ULL;
    unsigned long long updated_row_id = 0ULL;
    unsigned long long final_row_id = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            initial_row,
            sizeof(initial_row),
            entries,
            sizeof(entries) / sizeof(entries[0]),
            &row_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_statement_matches(statement, filename) == 1);
    assert(mylite_storage_statement_matches(statement, other_filename) == 0);
    assert(mylite_storage_statement_matches(NULL, filename) == 0);
    assert(
        mylite_storage_update_row_preserving_index_entries_in_statement(
            statement,
            "app",
            "posts",
            row_id,
            updated_row,
            sizeof(updated_row),
            &updated_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert_row_equals(filename, updated_row_id, updated_row, sizeof(updated_row));
    assert(
        mylite_storage_update_row_with_index_entry_changes_in_statement(
            statement,
            "app",
            "posts",
            updated_row_id,
            final_row,
            sizeof(final_row),
            entries,
            sizeof(entries) / sizeof(entries[0]),
            unchanged_entries,
            &final_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_commit_statement(statement) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_equals(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        final_row_id,
        final_row,
        sizeof(final_row)
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(other_filename);
    free(filename);
    free(root);
}

static void test_active_single_index_same_size_rollback_after_checksum_refresh(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char primary_key[] = {0x10U, 0x01U};
    static const unsigned char initial_secondary_key[] = {0x20U, 0x01U};
    static const unsigned char working_secondary_key[] = {0x30U, 0x01U};
    static const unsigned char final_secondary_key[] = {0x40U, 0x01U};
    static const unsigned char savepoint_secondary_key[] = {0x50U, 0x01U};
    static const unsigned char larger_secondary_key[] = {0x60U, 0x01U};
    static const unsigned char initial_row[] = {0x01U, 'i', 'n', 'i', 't'};
    static const unsigned char working_row[] = {0x02U, 'w', 'o', 'r', 'k'};
    static const unsigned char final_row[] = {0x03U, 'f', 'i', 'n', 'l'};
    static const unsigned char savepoint_row[] = {0x04U, 's', 'a', 'v', 'e'};
    static const unsigned char larger_row[] = {0x05U, 'l', 'a', 'r', 'g', 'e'};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-single-index-same-size-rollback.mylite");
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
    mylite_storage_index_entry working_entries[] = {
        {.size = sizeof(working_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(working_entries[1]),
         .index_number = 1U,
         .key = working_secondary_key,
         .key_size = sizeof(working_secondary_key)},
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
    mylite_storage_index_entry larger_entries[] = {
        {.size = sizeof(larger_entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
        {.size = sizeof(larger_entries[1]),
         .index_number = 1U,
         .key = larger_secondary_key,
         .key_size = sizeof(larger_secondary_key)},
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_id = 0ULL;
    unsigned long long working_row_id = 0ULL;
    unsigned long long final_row_id = 0ULL;
    unsigned long long savepoint_row_id = 0ULL;
    unsigned long long larger_row_id = 0ULL;
    mylite_storage_header before_savepoint_update_header = {
        .size = sizeof(before_savepoint_update_header),
    };
    mylite_storage_header after_savepoint_update_header = {
        .size = sizeof(after_savepoint_update_header),
    };
    mylite_storage_header before_larger_update_header = {
        .size = sizeof(before_larger_update_header),
    };
    mylite_storage_header after_larger_update_header = {
        .size = sizeof(after_larger_update_header),
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

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_id,
            working_row,
            sizeof(working_row),
            working_entries,
            sizeof(working_entries) / sizeof(working_entries[0]),
            &working_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(working_row_id != row_id);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            working_row_id,
            final_row,
            sizeof(final_row),
            final_entries,
            sizeof(final_entries) / sizeof(final_entries[0]),
            &final_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(final_row_id == working_row_id);
    assert_find_indexed_row_equals(
        filename,
        1U,
        final_secondary_key,
        sizeof(final_secondary_key),
        final_row_id,
        final_row,
        sizeof(final_row)
    );

    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_open_header(filename, &before_savepoint_update_header) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            final_row_id,
            savepoint_row,
            sizeof(savepoint_row),
            savepoint_entries,
            sizeof(savepoint_entries) / sizeof(savepoint_entries[0]),
            &savepoint_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(savepoint_row_id == final_row_id);
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
        final_secondary_key,
        sizeof(final_secondary_key),
        final_row_id,
        final_row,
        sizeof(final_row)
    );

    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            final_row_id,
            working_row,
            sizeof(working_row),
            working_entries,
            sizeof(working_entries) / sizeof(working_entries[0]),
            &working_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            working_row_id,
            final_row,
            sizeof(final_row),
            final_entries,
            sizeof(final_entries) / sizeof(final_entries[0]),
            &final_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(final_row_id == working_row_id);

    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &before_larger_update_header) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            final_row_id,
            savepoint_row,
            sizeof(savepoint_row),
            savepoint_entries,
            sizeof(savepoint_entries) / sizeof(savepoint_entries[0]),
            &savepoint_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(savepoint_row_id == final_row_id);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            savepoint_row_id,
            larger_row,
            sizeof(larger_row),
            larger_entries,
            sizeof(larger_entries) / sizeof(larger_entries[0]),
            &larger_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(larger_row_id == final_row_id);
    assert(mylite_storage_open_header(filename, &after_larger_update_header) == MYLITE_STORAGE_OK);
    assert(after_larger_update_header.page_count == before_larger_update_header.page_count);

    assert_find_indexed_row_equals(
        filename,
        1U,
        larger_secondary_key,
        sizeof(larger_secondary_key),
        larger_row_id,
        larger_row,
        sizeof(larger_row)
    );
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_find_indexed_row_not_found(
        filename,
        1U,
        larger_secondary_key,
        sizeof(larger_secondary_key)
    );
    assert_find_indexed_row_not_found(
        filename,
        1U,
        savepoint_secondary_key,
        sizeof(savepoint_secondary_key)
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

    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert_row_not_found(filename, row_id);
    assert_find_indexed_row_equals(
        filename,
        1U,
        final_secondary_key,
        sizeof(final_secondary_key),
        final_row_id,
        final_row,
        sizeof(final_row)
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_active_row_only_same_size_rollback_after_checksum_refresh(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char primary_key[] = {0x10U, 0x01U};
    static const unsigned char initial_row[] = {0x01U, 'i', 'n', 'i', 't'};
    static const unsigned char middle_row[] = {0x02U, 'm', 'i', 'd', 'd'};
    static const unsigned char final_row[] = {0x03U, 'f', 'i', 'n', 'l'};
    static const unsigned char larger_row[] = {0x04U, 'l', 'a', 'r', 'g', 'e'};
    static const unsigned char working_row[] = {0x05U, 'w', 'o', 'r', 'k'};
    static const unsigned char unchanged_entries[] = {0U};
    char *root = make_temp_root();
    char *filename = path_join(root, "active-row-only-same-size-rollback.mylite");
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
        {.size = sizeof(entries[0]),
         .index_number = 0U,
         .key = primary_key,
         .key_size = sizeof(primary_key)},
    };
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_id = 0ULL;
    unsigned long long middle_row_id = 0ULL;
    unsigned long long final_row_id = 0ULL;
    unsigned long long larger_row_id = 0ULL;
    unsigned long long working_row_id = 0ULL;
    mylite_storage_header before_final_update_header = {
        .size = sizeof(before_final_update_header),
    };
    mylite_storage_header after_final_update_header = {
        .size = sizeof(after_final_update_header),
    };
    mylite_storage_header before_larger_update_header = {
        .size = sizeof(before_larger_update_header),
    };
    mylite_storage_header after_larger_update_header = {
        .size = sizeof(after_larger_update_header),
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
            entries,
            sizeof(entries) / sizeof(entries[0]),
            &row_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_begin_transaction(filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            row_id,
            middle_row,
            sizeof(middle_row),
            entries,
            sizeof(entries) / sizeof(entries[0]),
            unchanged_entries,
            &middle_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(middle_row_id != row_id);
    assert_find_indexed_row_equals(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        middle_row_id,
        middle_row,
        sizeof(middle_row)
    );

    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &before_final_update_header) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            middle_row_id,
            final_row,
            sizeof(final_row),
            entries,
            sizeof(entries) / sizeof(entries[0]),
            unchanged_entries,
            &final_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(final_row_id == middle_row_id);
    assert(mylite_storage_open_header(filename, &after_final_update_header) == MYLITE_STORAGE_OK);
    assert(after_final_update_header.page_count == before_final_update_header.page_count);

    assert_row_equals(filename, final_row_id, final_row, sizeof(final_row));
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_row_equals(filename, middle_row_id, middle_row, sizeof(middle_row));
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
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            middle_row_id,
            working_row,
            sizeof(working_row),
            entries,
            sizeof(entries) / sizeof(entries[0]),
            unchanged_entries,
            &working_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        working_row_id,
        working_row,
        sizeof(working_row)
    );

    assert(mylite_storage_begin_statement(filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &before_larger_update_header) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            working_row_id,
            final_row,
            sizeof(final_row),
            entries,
            sizeof(entries) / sizeof(entries[0]),
            unchanged_entries,
            &final_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(final_row_id == working_row_id);
    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            final_row_id,
            larger_row,
            sizeof(larger_row),
            entries,
            sizeof(entries) / sizeof(entries[0]),
            unchanged_entries,
            &larger_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(larger_row_id == working_row_id);
    assert(mylite_storage_open_header(filename, &after_larger_update_header) == MYLITE_STORAGE_OK);
    assert(after_larger_update_header.page_count == before_larger_update_header.page_count);

    assert_row_equals(filename, larger_row_id, larger_row, sizeof(larger_row));
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_row_equals(filename, working_row_id, working_row, sizeof(working_row));
    assert_find_indexed_row_equals(
        filename,
        0U,
        primary_key,
        sizeof(primary_key),
        working_row_id,
        working_row,
        sizeof(working_row)
    );

    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert_row_not_found(filename, row_id);
    if (working_row_id != middle_row_id) {
        assert_row_not_found(filename, middle_row_id);
    }
    assert_row_equals(filename, working_row_id, working_row, sizeof(working_row));

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
    assert(header.page_count == before_secondary_update_pages + 2ULL);

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

static void assert_index_prefix_exists_for_index(
    const char *filename,
    unsigned index_number,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    unsigned long long skip_row_id,
    int expected_exists
) {
    int exists = 0;
    assert(
        mylite_storage_index_prefix_exists_for_index(
            filename,
            "app",
            "posts",
            index_number,
            key_prefix,
            key_prefix_size,
            skip_row_id,
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

static void assert_prefix_index_entries(
    const char *filename,
    unsigned index_number,
    const unsigned char *key_prefix,
    size_t key_prefix_size,
    const unsigned char *const *expected_keys,
    size_t expected_key_size,
    const unsigned long long *expected_row_ids,
    size_t expected_count
) {
    mylite_storage_index_entryset index_entries = {
        .size = sizeof(index_entries),
    };
    assert(
        mylite_storage_read_index_prefix_entries(
            filename,
            "app",
            "posts",
            index_number,
            key_prefix,
            key_prefix_size,
            &index_entries
        ) == MYLITE_STORAGE_OK
    );
    assert(index_entries.entry_count == expected_count);
    assert(index_entries.key_bytes == expected_count * expected_key_size);
    for (size_t i = 0; i < expected_count; ++i) {
        assert_index_entry(
            &index_entries,
            i,
            expected_row_ids[i],
            expected_keys[i],
            expected_key_size
        );
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
    assert(ctx->row_1_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 2ULL);
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
    assert(ctx->row_2_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 5ULL);
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
    assert(ctx->updated_row_1_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 8ULL);
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

static void test_maintained_index_root_page_format(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char key_1[] = {0x01U, 0x00U, 0x00U, 0x00U};
    static const unsigned char key_2[] = {0x02U, 0x00U, 0x00U, 0x00U};
    unsigned char keys[sizeof(key_1) + sizeof(key_2)] = {0};
    memcpy(keys, key_2, sizeof(key_2));
    memcpy(keys + sizeof(key_2), key_1, sizeof(key_1));
    unsigned char one_key[sizeof(key_1)] = {0};
    memcpy(one_key, key_1, sizeof(one_key));
    size_t key_offsets[] = {0U, sizeof(key_2)};
    size_t key_sizes[] = {sizeof(key_2), sizeof(key_1)};
    unsigned long long row_ids[] = {11ULL, 10ULL};
    size_t one_key_offsets[] = {0U};
    size_t one_key_sizes[] = {sizeof(one_key)};
    unsigned long long one_row_ids[] = {6ULL};
    mylite_storage_index_entryset entryset = {
        .size = sizeof(entryset),
        .keys = keys,
        .key_bytes = sizeof(keys),
        .entry_count = 2U,
        .key_offsets = key_offsets,
        .key_sizes = key_sizes,
        .row_ids = row_ids,
    };
    mylite_storage_index_entryset one_entryset = {
        .size = sizeof(one_entryset),
        .keys = one_key,
        .key_bytes = sizeof(one_key),
        .entry_count = 1U,
        .key_offsets = one_key_offsets,
        .key_sizes = one_key_sizes,
        .row_ids = one_row_ids,
    };
    mylite_storage_index_entryset empty_entryset = {
        .size = sizeof(empty_entryset),
    };
    mylite_storage_header header = {
        .size = sizeof(header),
        .format_version = MYLITE_STORAGE_FORMAT_VERSION,
        .header_version = MYLITE_STORAGE_FORMAT_HEADER_VERSION,
        .page_size = MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        .page_count = 12ULL,
    };
    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char corrupt_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned long long table_id = 0ULL;
    unsigned index_number = 0U;
    size_t key_size = 0U;
    size_t entry_count = 0U;

    assert(
        mylite_storage_test_encode_maintained_index_root_page(
            page,
            4ULL,
            3ULL,
            1U,
            sizeof(key_1),
            &entryset
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_OK
    );
    assert(table_id == 3ULL);
    assert(index_number == 1U);
    assert(key_size == sizeof(key_1));
    assert(entry_count == 2U);
    assert(
        get_test_u64_le(
            page + MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET,
            MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_ROW_ID_OFFSET
        ) == 10ULL
    );
    assert(
        memcmp(
            page + MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET +
                MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_KEY_OFFSET,
            key_1,
            sizeof(key_1)
        ) == 0
    );
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            6ULL,
            page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u64_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_ROOT_TABLE_ID_OFFSET, 0ULL);
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u32_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_ROOT_KEY_SIZE_OFFSET, 0U);
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u32_le(
        corrupt_page,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET
    );
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u64_le(
        corrupt_page + MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_ROW_ID_OFFSET,
        header.page_count
    );
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u32_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAGS_OFFSET, 0U);
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u32_le(
        corrupt_page,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAGS_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAG_SINGLE_PAGE |
            MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAG_HAS_OVERFLOW_TAIL
    );
    put_test_u64_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_ROOT_OVERFLOW_TAIL_PAGE_OFFSET, 8ULL);
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_OK
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u64_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_ROOT_OVERFLOW_TAIL_PAGE_OFFSET, 8ULL);
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u32_le(
        corrupt_page,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAGS_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAG_SINGLE_PAGE |
            MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAG_HAS_OVERFLOW_TAIL
    );
    put_test_u64_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_ROOT_OVERFLOW_TAIL_PAGE_OFFSET, 4ULL);
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u32_le(
        corrupt_page,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAGS_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAG_SINGLE_PAGE |
            MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAG_HAS_OVERFLOW_TAIL
    );
    put_test_u64_le(
        corrupt_page,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_OVERFLOW_TAIL_PAGE_OFFSET,
        header.page_count
    );
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    corrupt_page[MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET] ^= 0x01U;
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    memcpy(
        corrupt_page + MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET +
            MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_KEY_OFFSET,
        key_2,
        sizeof(key_2)
    );
    memcpy(
        corrupt_page + MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET +
            MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_HEADER_SIZE + sizeof(key_1) +
            MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_KEY_OFFSET,
        key_1,
        sizeof(key_1)
    );
    rechecksum_test_index_root_page(corrupt_page);
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    assert(
        mylite_storage_test_encode_maintained_index_root_page(
            page,
            6ULL,
            3ULL,
            2U,
            sizeof(key_1),
            &one_entryset
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            6ULL,
            page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_OK
    );
    assert(table_id == 3ULL);
    assert(index_number == 2U);
    assert(key_size == sizeof(key_1));
    assert(entry_count == 1U);

    assert(
        mylite_storage_test_encode_maintained_index_root_page(
            page,
            4ULL,
            0ULL,
            1U,
            sizeof(key_1),
            &entryset
        ) == MYLITE_STORAGE_MISUSE
    );
    assert(
        mylite_storage_test_encode_maintained_index_root_page(
            page,
            4ULL,
            3ULL,
            1U,
            sizeof(key_1) + 1U,
            &entryset
        ) == MYLITE_STORAGE_MISUSE
    );

    assert(
        mylite_storage_test_encode_maintained_index_root_page(
            page,
            5ULL,
            3ULL,
            1U,
            sizeof(key_1),
            &empty_entryset
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_test_decode_maintained_index_root_page(
            &header,
            5ULL,
            page,
            &table_id,
            &index_number,
            &key_size,
            &entry_count
        ) == MYLITE_STORAGE_OK
    );
    assert(entry_count == 0U);
    assert(key_size == sizeof(key_1));
#endif
}

static void test_index_branch_page_format(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    enum { branch_overflow_key_size = 2048U };

    static const unsigned char key_1[] = {0x01U, 0x00U, 0x00U, 0x00U};
    static const unsigned char key_2[] = {0x02U, 0x00U, 0x00U, 0x00U};
    static const unsigned char key_3[] = {0x03U, 0x00U, 0x00U, 0x00U};
    unsigned char child_max_keys[sizeof(key_1) * 3U] = {0};
    unsigned long long child_page_ids[] = {5ULL, 6ULL, 7ULL};
    unsigned long long child_max_row_ids[] = {10ULL, 20ULL, 30ULL};
    unsigned char oversized_child_max_keys[branch_overflow_key_size * 2U] = {0};
    unsigned long long oversized_child_page_ids[] = {5ULL, 6ULL};
    unsigned long long oversized_child_max_row_ids[] = {10ULL, 20ULL};
    mylite_storage_header header = {
        .size = sizeof(header),
        .format_version = MYLITE_STORAGE_FORMAT_VERSION,
        .header_version = MYLITE_STORAGE_FORMAT_HEADER_VERSION,
        .page_size = MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        .page_count = 12ULL,
    };
    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char corrupt_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned long long table_id = 0ULL;
    unsigned long long child_page_id = 0ULL;
    unsigned index_number = 0U;
    unsigned level = 0U;
    size_t key_size = 0U;
    size_t child_count = 0U;

    memcpy(child_max_keys, key_1, sizeof(key_1));
    memcpy(child_max_keys + sizeof(key_1), key_2, sizeof(key_2));
    memcpy(child_max_keys + (sizeof(key_1) * 2U), key_3, sizeof(key_3));

    assert(
        mylite_storage_test_encode_index_branch_page(
            page,
            4ULL,
            3ULL,
            1U,
            1U,
            sizeof(key_1),
            child_page_ids,
            child_max_row_ids,
            child_max_keys,
            3U
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_OK
    );
    assert(table_id == 3ULL);
    assert(index_number == 1U);
    assert(level == 1U);
    assert(key_size == sizeof(key_1));
    assert(child_count == 3U);
    assert(get_test_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) == 3ULL);

    const size_t cell_size = MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_HEADER_SIZE + sizeof(key_1);
    const unsigned char *first_cell = page + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    const unsigned char *second_cell = first_cell + cell_size;
    assert(
        get_test_u64_le(first_cell, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET) ==
        5ULL
    );
    assert(
        get_test_u64_le(second_cell, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET) ==
        20ULL
    );
    assert(
        memcmp(
            second_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_2,
            sizeof(key_2)
        ) == 0
    );

    assert(
        mylite_storage_test_find_index_branch_child_page(
            &header,
            4ULL,
            page,
            key_1,
            sizeof(key_1),
            9ULL,
            &child_page_id
        ) == MYLITE_STORAGE_OK
    );
    assert(child_page_id == 5ULL);
    assert(
        mylite_storage_test_find_index_branch_child_page(
            &header,
            4ULL,
            page,
            key_1,
            sizeof(key_1),
            10ULL,
            &child_page_id
        ) == MYLITE_STORAGE_OK
    );
    assert(child_page_id == 5ULL);
    assert(
        mylite_storage_test_find_index_branch_child_page(
            &header,
            4ULL,
            page,
            key_1,
            sizeof(key_1),
            11ULL,
            &child_page_id
        ) == MYLITE_STORAGE_OK
    );
    assert(child_page_id == 6ULL);
    assert(
        mylite_storage_test_find_index_branch_child_page(
            &header,
            4ULL,
            page,
            key_2,
            sizeof(key_2),
            20ULL,
            &child_page_id
        ) == MYLITE_STORAGE_OK
    );
    assert(child_page_id == 6ULL);
    assert(
        mylite_storage_test_find_index_branch_child_page(
            &header,
            4ULL,
            page,
            key_3,
            sizeof(key_3),
            31ULL,
            &child_page_id
        ) == MYLITE_STORAGE_NOTFOUND
    );

    assert(
        mylite_storage_test_find_index_branch_child_page(
            &header,
            4ULL,
            page,
            key_1,
            sizeof(key_1) - 1U,
            9ULL,
            &child_page_id
        ) == MYLITE_STORAGE_MISUSE
    );

    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            6ULL,
            page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u64_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_TABLE_ID_OFFSET, 0ULL);
    rechecksum_test_index_branch_page(corrupt_page);
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u32_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_LEVEL_OFFSET, 0U);
    rechecksum_test_index_branch_page(corrupt_page);
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u32_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_KEY_SIZE_OFFSET, 0U);
    rechecksum_test_index_branch_page(corrupt_page);
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u32_le(
        corrupt_page,
        MYLITE_STORAGE_FORMAT_INDEX_BRANCH_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET
    );
    rechecksum_test_index_branch_page(corrupt_page);
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u64_le(
        corrupt_page + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET,
        4ULL
    );
    rechecksum_test_index_branch_page(corrupt_page);
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u64_le(
        corrupt_page + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET,
        header.page_count
    );
    rechecksum_test_index_branch_page(corrupt_page);
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    put_test_u64_le(corrupt_page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET, 2ULL);
    rechecksum_test_index_branch_page(corrupt_page);
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    memcpy(
        corrupt_page + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET +
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
        key_3,
        sizeof(key_3)
    );
    rechecksum_test_index_branch_page(corrupt_page);
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    memcpy(
        corrupt_page + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + cell_size +
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
        key_1,
        sizeof(key_1)
    );
    put_test_u64_le(
        corrupt_page + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + cell_size,
        MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET,
        10ULL
    );
    rechecksum_test_index_branch_page(corrupt_page);
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    memcpy(corrupt_page, page, sizeof(corrupt_page));
    corrupt_page[MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET] ^= 0x01U;
    assert(
        mylite_storage_test_decode_index_branch_page(
            &header,
            4ULL,
            corrupt_page,
            &table_id,
            &index_number,
            &level,
            &key_size,
            &child_count
        ) == MYLITE_STORAGE_CORRUPT
    );

    assert(
        mylite_storage_test_encode_index_branch_page(
            page,
            4ULL,
            0ULL,
            1U,
            1U,
            sizeof(key_1),
            child_page_ids,
            child_max_row_ids,
            child_max_keys,
            3U
        ) == MYLITE_STORAGE_MISUSE
    );
    assert(
        mylite_storage_test_encode_index_branch_page(
            page,
            4ULL,
            3ULL,
            1U,
            0U,
            sizeof(key_1),
            child_page_ids,
            child_max_row_ids,
            child_max_keys,
            3U
        ) == MYLITE_STORAGE_MISUSE
    );
    assert(
        mylite_storage_test_encode_index_branch_page(
            page,
            4ULL,
            3ULL,
            1U,
            1U,
            sizeof(key_1),
            child_page_ids,
            child_max_row_ids,
            child_max_keys,
            0U
        ) == MYLITE_STORAGE_MISUSE
    );
    assert(
        mylite_storage_test_encode_index_branch_page(
            page,
            4ULL,
            3ULL,
            1U,
            1U,
            branch_overflow_key_size,
            oversized_child_page_ids,
            oversized_child_max_row_ids,
            oversized_child_max_keys,
            2U
        ) == MYLITE_STORAGE_FULL
    );
#endif
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
    static const unsigned char missing_prefix[] = {0xffU};
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
    assert_index_root_page_type(
        filename,
        header.page_count - 1ULL,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    const unsigned long long primary_root_page = header.page_count - 1ULL;
    mylite_storage_index_root_definition stale_primary_root = {
        .size = sizeof(stale_primary_root),
        .schema_name = "app",
        .table_name = "posts",
        .index_number = 0U,
        .root_page = primary_root_page,
        .entry_count = 99ULL,
    };
    assert(mylite_storage_store_index_root(filename, &stale_primary_root) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 0U, primary_root_page, 2ULL);
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_OK, row_1_id);
    assert_index_entry_lookup(filename, 0U, key_3, sizeof(key_3), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_prefix_exists_for_index(filename, 0U, key_1, sizeof(key_1), 0ULL, 1);
    assert_index_prefix_exists_for_index(filename, 0U, key_1, sizeof(key_1), row_1_id, 0);
    const unsigned char *key_1_prefix_keys[] = {key_1};
    const unsigned long long key_1_prefix_rows[] = {row_1_id};
    assert_prefix_index_entries(
        filename,
        0U,
        key_1,
        1U,
        key_1_prefix_keys,
        sizeof(key_1),
        key_1_prefix_rows,
        sizeof(key_1_prefix_rows) / sizeof(key_1_prefix_rows[0])
    );
    assert_prefix_index_entries(
        filename,
        0U,
        missing_prefix,
        sizeof(missing_prefix),
        NULL,
        sizeof(key_1),
        NULL,
        0U
    );

    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 1U) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 1U, header.page_count - 1ULL, 2ULL);
    assert_index_root_page_type(
        filename,
        header.page_count - 1ULL,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    const unsigned long long secondary_root_page = header.page_count - 1ULL;
    const unsigned long long first_secondary_row_ids[] = {row_1_id, row_2_id};
    assert_exact_index_entries(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        first_secondary_row_ids,
        sizeof(first_secondary_row_ids) / sizeof(first_secondary_row_ids[0])
    );
    assert_index_prefix_exists_for_index(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        0ULL,
        1
    );
    assert_index_prefix_exists_for_index(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        row_1_id,
        1
    );
    const unsigned char *secondary_prefix_keys[] = {secondary_key, secondary_key};
    assert_prefix_index_entries(
        filename,
        1U,
        secondary_key,
        1U,
        secondary_prefix_keys,
        sizeof(secondary_key),
        first_secondary_row_ids,
        sizeof(first_secondary_row_ids) / sizeof(first_secondary_row_ids[0])
    );

    const unsigned long long before_maintained_insert_pages = header.page_count;
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
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_maintained_insert_pages + 1ULL);
    assert_index_entry_lookup(filename, 0U, key_3, sizeof(key_3), MYLITE_STORAGE_OK, row_3_id);
    assert_index_prefix_exists_for_index(filename, 0U, key_3, sizeof(key_3), 0ULL, 1);
    assert_index_prefix_exists_for_index(filename, 0U, key_3, sizeof(key_3), row_3_id, 0);
    assert_index_prefix_exists_for_index(
        filename,
        0U,
        missing_prefix,
        sizeof(missing_prefix),
        0ULL,
        0
    );
    const unsigned char *key_3_prefix_keys[] = {key_3};
    const unsigned long long key_3_prefix_rows[] = {row_3_id};
    assert_prefix_index_entries(
        filename,
        0U,
        key_3,
        1U,
        key_3_prefix_keys,
        sizeof(key_3),
        key_3_prefix_rows,
        sizeof(key_3_prefix_rows) / sizeof(key_3_prefix_rows[0])
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

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_maintained_update_pages = header.page_count;
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
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_maintained_update_pages + 2ULL);
    assert_index_root(filename, "app", "posts", 0U, primary_root_page, 3ULL);
    assert_index_root(filename, "app", "posts", 1U, secondary_root_page, 3ULL);
    assert_index_entry_lookup(filename, 0U, key_2, sizeof(key_2), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(
        filename,
        0U,
        updated_key_2,
        sizeof(updated_key_2),
        MYLITE_STORAGE_OK,
        updated_row_2_id
    );
    assert_index_prefix_exists_for_index(filename, 0U, key_2, sizeof(key_2), 0ULL, 0);
    assert_prefix_index_entries(filename, 0U, key_2, 1U, NULL, sizeof(key_2), NULL, 0U);
    assert_index_prefix_exists_for_index(
        filename,
        0U,
        updated_key_2,
        sizeof(updated_key_2),
        updated_row_2_id,
        0
    );
    const unsigned char *updated_key_2_prefix_keys[] = {updated_key_2};
    const unsigned long long updated_key_2_prefix_rows[] = {updated_row_2_id};
    assert_prefix_index_entries(
        filename,
        0U,
        updated_key_2,
        1U,
        updated_key_2_prefix_keys,
        sizeof(updated_key_2),
        updated_key_2_prefix_rows,
        sizeof(updated_key_2_prefix_rows) / sizeof(updated_key_2_prefix_rows[0])
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
    assert_index_prefix_exists_for_index(
        filename,
        1U,
        updated_secondary_key,
        sizeof(updated_secondary_key),
        0ULL,
        1
    );

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_maintained_delete_pages = header.page_count;
    assert(mylite_storage_delete_row(filename, "app", "posts", row_1_id) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_maintained_delete_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, primary_root_page, 2ULL);
    assert_index_root(filename, "app", "posts", 1U, secondary_root_page, 2ULL);
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_prefix_exists_for_index(filename, 0U, key_1, sizeof(key_1), 0ULL, 0);
    assert_prefix_index_entries(filename, 0U, key_1, 1U, NULL, sizeof(key_1), NULL, 0U);
    const unsigned long long delete_tail_row_ids[] = {row_3_id};
    assert_exact_index_entries(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        delete_tail_row_ids,
        sizeof(delete_tail_row_ids) / sizeof(delete_tail_row_ids[0])
    );
    assert_index_prefix_exists_for_index(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        row_3_id,
        0
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

static void test_maintained_index_root_overflow_tail(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_0[] = {0x00U, 0x00U, 'z'};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'b'};
    static const unsigned char row_3[] = {0x00U, 0x03U, 'c'};
    static const unsigned char cross_child_row_2[] = {0x00U, 0x25U, 'b'};
    static const unsigned char updated_interior_row_2[] = {0x00U, 0x20U, 'b'};
    static const unsigned char row_5[] = {0x00U, 0x05U, 'e'};
    static const unsigned char row_6[] = {0x00U, 0x06U, 'f'};
    static const unsigned char row_7[] = {0x00U, 0x07U, 'g'};
    static const unsigned char row_8[] = {0x00U, 0x08U, 'h'};
    static const unsigned char row_9[] = {0x00U, 0x09U, 'i'};
    static const unsigned char row_10[] = {0x00U, 0x0aU, 'j'};
    static const unsigned char updated_row_4[] = {0x00U, 0x40U, 'e'};
    static const size_t key_size = 1322U;
    char *root = make_temp_root();
    char *filename = path_join(root, "maintained-index-root-overflow-tail.mylite");
    char *journal_filename = journal_path(filename);
    char *transaction_journal_filename = transaction_journal_path(filename);
    unsigned char row_4[9000U] = {0};
    unsigned char key_0[1322U] = {0};
    unsigned char key_1[1322U] = {0};
    unsigned char key_2[1322U] = {0};
    unsigned char key_2b[1322U] = {0};
    unsigned char key_3[1322U] = {0};
    unsigned char key_4[1322U] = {0};
    unsigned char key_5[1322U] = {0};
    unsigned char key_6[1322U] = {0};
    unsigned char key_7[1322U] = {0};
    unsigned char key_8[1322U] = {0};
    unsigned char key_9[1322U] = {0};
    unsigned char key_10[1322U] = {0};
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_0_entry[] = {
        {
            .size = sizeof(row_0_entry[0]),
            .index_number = 0U,
            .key = key_0,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_1_entry[] = {
        {
            .size = sizeof(row_1_entry[0]),
            .index_number = 0U,
            .key = key_1,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_2_entry[] = {
        {
            .size = sizeof(row_2_entry[0]),
            .index_number = 0U,
            .key = key_2,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry cross_child_row_2_entry[] = {
        {
            .size = sizeof(cross_child_row_2_entry[0]),
            .index_number = 0U,
            .key = key_5,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry updated_interior_row_2_entry[] = {
        {
            .size = sizeof(updated_interior_row_2_entry[0]),
            .index_number = 0U,
            .key = key_2b,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_3_entry[] = {
        {
            .size = sizeof(row_3_entry[0]),
            .index_number = 0U,
            .key = key_3,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_4_entry[] = {
        {
            .size = sizeof(row_4_entry[0]),
            .index_number = 0U,
            .key = key_4,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry updated_row_4_entry[] = {
        {
            .size = sizeof(updated_row_4_entry[0]),
            .index_number = 0U,
            .key = key_5,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_6_entry[] = {
        {
            .size = sizeof(row_6_entry[0]),
            .index_number = 0U,
            .key = key_6,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_7_entry[] = {
        {
            .size = sizeof(row_7_entry[0]),
            .index_number = 0U,
            .key = key_7,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_8_entry[] = {
        {
            .size = sizeof(row_8_entry[0]),
            .index_number = 0U,
            .key = key_8,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_9_entry[] = {
        {
            .size = sizeof(row_9_entry[0]),
            .index_number = 0U,
            .key = key_9,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_10_entry[] = {
        {
            .size = sizeof(row_10_entry[0]),
            .index_number = 0U,
            .key = key_10,
            .key_size = key_size,
        },
    };
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_3_id = 0ULL;
    unsigned long long row_4_id = 0ULL;
    unsigned long long row_5_id = 0ULL;
    unsigned long long row_6_id = 0ULL;
    unsigned long long row_7_id = 0ULL;
    unsigned long long row_8_id = 0ULL;
    unsigned long long row_9_id = 0ULL;
    unsigned long long row_10_id = 0ULL;
    unsigned long long updated_row_4_id = 0ULL;
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    mylite_storage_statement *statement = NULL;
    unsigned char root_page_bytes[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    int status = 0;

    const size_t root_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_HEADER_SIZE + key_size);
    assert(root_capacity == 3U);
    key_1[0] = 0x01U;
    key_2[0] = 0x02U;
    key_2b[0] = 0x02U;
    key_2b[1] = 0x80U;
    key_3[0] = 0x03U;
    key_4[0] = 0x04U;
    key_5[0] = 0x05U;
    key_6[0] = 0x03U;
    key_6[1] = 0x80U;
    key_7[0] = 0x07U;
    key_8[0] = 0x08U;
    key_9[0] = 0x09U;
    key_10[0] = 0x0aU;
    row_4[0] = 0x00U;
    row_4[1] = 0x04U;
    row_4[2] = 'd';

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            row_1_entry,
            sizeof(row_1_entry) / sizeof(row_1_entry[0]),
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
            row_2_entry,
            sizeof(row_2_entry) / sizeof(row_2_entry[0]),
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_3,
            sizeof(row_3),
            row_3_entry,
            sizeof(row_3_entry) / sizeof(row_3_entry[0]),
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long root_page = header.page_count - 1ULL;
    assert_index_root(filename, "app", "posts", 0U, root_page, 3ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );

    const unsigned long long before_overflow_pages = header.page_count;
    unsigned long long rolled_back_row_4_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_4,
            sizeof(row_4),
            row_4_entry,
            sizeof(row_4_entry) / sizeof(row_4_entry[0]),
            &rolled_back_row_4_id
        ) == MYLITE_STORAGE_OK
    );
    assert(access(journal_filename, F_OK) == 0);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    assert_index_entry_lookup(
        filename,
        0U,
        key_4,
        key_size,
        MYLITE_STORAGE_OK,
        rolled_back_row_4_id
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_overflow_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 3ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_entry_lookup(filename, 0U, key_4, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);

    const pid_t statement_pid = fork();
    assert(statement_pid >= 0);
    if (statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_row_4_id = 0ULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_4,
                sizeof(row_4),
                row_4_entry,
                sizeof(row_4_entry) / sizeof(row_4_entry[0]),
                &child_row_4_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_4_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(waitpid(statement_pid, &status, 0) == statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    assert_index_entry_lookup(filename, 0U, key_4, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_overflow_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 3ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );

    const pid_t transaction_pid = fork();
    assert(transaction_pid >= 0);
    if (transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_row_4_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_4,
                sizeof(row_4),
                row_4_entry,
                sizeof(row_4_entry) / sizeof(row_4_entry[0]),
                &child_row_4_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_4_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(waitpid(transaction_pid, &status, 0) == transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    assert_index_entry_lookup(filename, 0U, key_4, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_overflow_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 3ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );

    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_4,
            sizeof(row_4),
            row_4_entry,
            sizeof(row_4_entry) / sizeof(row_4_entry[0]),
            &row_4_id
        ) == MYLITE_STORAGE_OK
    );
    const unsigned long long overflow_blob_pages =
        ((sizeof(row_4) - 1U) /
         (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET)) +
        1ULL;
    const unsigned long long overflow_tail_page =
        before_overflow_pages + overflow_blob_pages + 1ULL;
    const unsigned long long promoted_first_leaf_page = overflow_tail_page + 1ULL;
    const size_t promoted_leaf_pages = 2U;
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == promoted_first_leaf_page + promoted_leaf_pages);
    assert(overflow_tail_page > root_page + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    assert_index_root_page_type(
        filename,
        promoted_first_leaf_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_LEAF
    );
    assert_index_root_page_type(
        filename,
        promoted_first_leaf_page + 1ULL,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_LEAF
    );
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        4ULL
    );

    assert_index_entry_lookup(filename, 0U, key_1, key_size, MYLITE_STORAGE_OK, row_1_id);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_OK, row_3_id);
    assert_index_entry_lookup(filename, 0U, key_4, key_size, MYLITE_STORAGE_OK, row_4_id);
    assert_find_indexed_row_equals(filename, 0U, key_4, key_size, row_4_id, row_4, sizeof(row_4));

    const unsigned long long overflow_row_ids[] = {row_4_id};
    assert_exact_index_entries(
        filename,
        0U,
        key_4,
        key_size,
        overflow_row_ids,
        sizeof(overflow_row_ids) / sizeof(overflow_row_ids[0])
    );
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 4U);
    assert(entries.key_bytes == 4U * key_size);
    assert_index_entry(&entries, 0U, row_1_id, key_1, key_size);
    assert_index_entry(&entries, 1U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 2U, row_3_id, key_3, key_size);
    assert_index_entry(&entries, 3U, row_4_id, key_4, key_size);
    mylite_storage_free_index_entryset(&entries);

    const unsigned long long before_branch_insert_pages = header.page_count;
    unsigned long long rolled_back_row_6_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_6,
            sizeof(row_6),
            row_6_entry,
            sizeof(row_6_entry) / sizeof(row_6_entry[0]),
            &rolled_back_row_6_id
        ) == MYLITE_STORAGE_OK
    );
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_insert_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);
    assert_index_entry_lookup(
        filename,
        0U,
        key_6,
        key_size,
        MYLITE_STORAGE_OK,
        rolled_back_row_6_id
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_insert_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);
    assert_index_entry_lookup(filename, 0U, key_6, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);

    const pid_t branch_statement_pid = fork();
    assert(branch_statement_pid >= 0);
    if (branch_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_row_6_id = 0ULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_6,
                sizeof(row_6),
                row_6_entry,
                sizeof(row_6_entry) / sizeof(row_6_entry[0]),
                &child_row_6_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_6_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(waitpid(branch_statement_pid, &status, 0) == branch_statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        5ULL
    );
    assert_index_entry_lookup(filename, 0U, key_6, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_insert_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);

    const pid_t branch_transaction_pid = fork();
    assert(branch_transaction_pid >= 0);
    if (branch_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_row_6_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_6,
                sizeof(row_6),
                row_6_entry,
                sizeof(row_6_entry) / sizeof(row_6_entry[0]),
                &child_row_6_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_6_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(waitpid(branch_transaction_pid, &status, 0) == branch_transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        5ULL
    );
    assert_index_entry_lookup(filename, 0U, key_6, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_insert_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);

    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_6,
            sizeof(row_6),
            row_6_entry,
            sizeof(row_6_entry) / sizeof(row_6_entry[0]),
            &row_6_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_insert_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);
    assert_index_entry_lookup(filename, 0U, key_6, key_size, MYLITE_STORAGE_OK, row_6_id);
    assert_find_indexed_row_equals(filename, 0U, key_6, key_size, row_6_id, row_6, sizeof(row_6));
    read_test_page(filename, promoted_first_leaf_page + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 2U
    );
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 5U);
    assert(entries.key_bytes == 5U * key_size);
    assert_index_entry(&entries, 0U, row_1_id, key_1, key_size);
    assert_index_entry(&entries, 1U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 2U, row_3_id, key_3, key_size);
    assert_index_entry(&entries, 3U, row_6_id, key_6, key_size);
    assert_index_entry(&entries, 4U, row_4_id, key_4, key_size);
    mylite_storage_free_index_entryset(&entries);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_cross_child_update_pages = header.page_count;
    unsigned long long rolled_back_cross_child_row_2_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2_id,
            cross_child_row_2,
            sizeof(cross_child_row_2),
            cross_child_row_2_entry,
            sizeof(cross_child_row_2_entry) / sizeof(cross_child_row_2_entry[0]),
            &rolled_back_cross_child_row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(rolled_back_cross_child_row_2_id != 0ULL);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_cross_child_update_pages + 2ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(
        filename,
        0U,
        key_5,
        key_size,
        MYLITE_STORAGE_OK,
        rolled_back_cross_child_row_2_id
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_5,
        key_size,
        rolled_back_cross_child_row_2_id,
        cross_child_row_2,
        sizeof(cross_child_row_2)
    );
    read_test_page(filename, promoted_first_leaf_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 2U
    );
    read_test_page(filename, promoted_first_leaf_page + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 3U
    );
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 5U);
    assert(entries.key_bytes == 5U * key_size);
    assert_index_entry(&entries, 0U, row_1_id, key_1, key_size);
    assert_index_entry(&entries, 1U, row_3_id, key_3, key_size);
    assert_index_entry(&entries, 2U, row_6_id, key_6, key_size);
    assert_index_entry(&entries, 3U, row_4_id, key_4, key_size);
    assert_index_entry(&entries, 4U, rolled_back_cross_child_row_2_id, key_5, key_size);
    mylite_storage_free_index_entryset(&entries);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_cross_child_update_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);

    const pid_t cross_child_update_statement_pid = fork();
    assert(cross_child_update_statement_pid >= 0);
    if (cross_child_update_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_cross_child_row_2_id = 0ULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_2_id,
                cross_child_row_2,
                sizeof(cross_child_row_2),
                cross_child_row_2_entry,
                sizeof(cross_child_row_2_entry) / sizeof(cross_child_row_2_entry[0]),
                &child_cross_child_row_2_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_cross_child_row_2_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(
        waitpid(cross_child_update_statement_pid, &status, 0) == cross_child_update_statement_pid
    );
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_cross_child_update_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);

    const pid_t cross_child_update_transaction_pid = fork();
    assert(cross_child_update_transaction_pid >= 0);
    if (cross_child_update_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_cross_child_row_2_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_2_id,
                cross_child_row_2,
                sizeof(cross_child_row_2),
                cross_child_row_2_entry,
                sizeof(cross_child_row_2_entry) / sizeof(cross_child_row_2_entry[0]),
                &child_cross_child_row_2_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_cross_child_row_2_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(
        waitpid(cross_child_update_transaction_pid, &status, 0) ==
        cross_child_update_transaction_pid
    );
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_cross_child_update_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_interior_delete_pages = header.page_count;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_delete_row(filename, "app", "posts", row_2_id) == MYLITE_STORAGE_OK);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_delete_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    read_test_page(filename, promoted_first_leaf_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 2U
    );
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 4U);
    assert(entries.key_bytes == 4U * key_size);
    assert_index_entry(&entries, 0U, row_1_id, key_1, key_size);
    assert_index_entry(&entries, 1U, row_3_id, key_3, key_size);
    assert_index_entry(&entries, 2U, row_6_id, key_6, key_size);
    assert_index_entry(&entries, 3U, row_4_id, key_4, key_size);
    mylite_storage_free_index_entryset(&entries);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_delete_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);

    const pid_t interior_delete_statement_pid = fork();
    assert(interior_delete_statement_pid >= 0);
    if (interior_delete_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_2_id) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(interior_delete_statement_pid, &status, 0) == interior_delete_statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_delete_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);

    const pid_t interior_delete_transaction_pid = fork();
    assert(interior_delete_transaction_pid >= 0);
    if (interior_delete_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_2_id) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(interior_delete_transaction_pid, &status, 0) == interior_delete_transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_delete_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_interior_update_pages = header.page_count;
    unsigned long long rolled_back_interior_row_2_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_2_id,
            updated_interior_row_2,
            sizeof(updated_interior_row_2),
            updated_interior_row_2_entry,
            sizeof(updated_interior_row_2_entry) / sizeof(updated_interior_row_2_entry[0]),
            &rolled_back_interior_row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(rolled_back_interior_row_2_id != 0ULL);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_update_pages + 2ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);
    read_test_page(filename, root_page, root_page_bytes);
    const size_t branch_cell_size = MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_HEADER_SIZE + key_size;
    const unsigned char *first_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    assert(
        memcmp(
            first_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_3,
            key_size
        ) == 0
    );
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(
        filename,
        0U,
        key_2b,
        key_size,
        MYLITE_STORAGE_OK,
        rolled_back_interior_row_2_id
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_update_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);
    assert_index_entry_lookup(filename, 0U, key_2b, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);

    const pid_t interior_update_statement_pid = fork();
    assert(interior_update_statement_pid >= 0);
    if (interior_update_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_updated_row_2_id = 0ULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_2_id,
                updated_interior_row_2,
                sizeof(updated_interior_row_2),
                updated_interior_row_2_entry,
                sizeof(updated_interior_row_2_entry) / sizeof(updated_interior_row_2_entry[0]),
                &child_updated_row_2_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_updated_row_2_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(waitpid(interior_update_statement_pid, &status, 0) == interior_update_statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);
    assert_index_entry_lookup(filename, 0U, key_2b, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_update_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);

    const pid_t interior_update_transaction_pid = fork();
    assert(interior_update_transaction_pid >= 0);
    if (interior_update_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_updated_row_2_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_2_id,
                updated_interior_row_2,
                sizeof(updated_interior_row_2),
                updated_interior_row_2_entry,
                sizeof(updated_interior_row_2_entry) / sizeof(updated_interior_row_2_entry[0]),
                &child_updated_row_2_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_updated_row_2_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(waitpid(interior_update_transaction_pid, &status, 0) == interior_update_transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, key_2, key_size, MYLITE_STORAGE_OK, row_2_id);
    assert_index_entry_lookup(filename, 0U, key_2b, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_update_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_high_key_insert_pages = header.page_count;
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_5,
            sizeof(row_5),
            updated_row_4_entry,
            sizeof(updated_row_4_entry) / sizeof(updated_row_4_entry[0]),
            &row_5_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_high_key_insert_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    const unsigned char *last_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + branch_cell_size;
    assert(
        get_test_u64_le(
            last_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == promoted_first_leaf_page + 1ULL
    );
    assert(
        get_test_u64_le(
            last_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_5_id
    );
    assert(
        memcmp(
            last_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_5,
            key_size
        ) == 0
    );
    read_test_page(filename, promoted_first_leaf_page + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 3U
    );
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_OK, row_5_id);
    assert_find_indexed_row_equals(filename, 0U, key_5, key_size, row_5_id, row_5, sizeof(row_5));
    const unsigned long long post_promotion_insert_row_ids[] = {row_5_id};
    assert_exact_index_entries(
        filename,
        0U,
        key_5,
        key_size,
        post_promotion_insert_row_ids,
        sizeof(post_promotion_insert_row_ids) / sizeof(post_promotion_insert_row_ids[0])
    );

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_interior_split_pages = header.page_count;
    unsigned long long rolled_back_row_0_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_0,
            sizeof(row_0),
            row_0_entry,
            sizeof(row_0_entry) / sizeof(row_0_entry[0]),
            &rolled_back_row_0_id
        ) == MYLITE_STORAGE_OK
    );
    assert(rolled_back_row_0_id == before_interior_split_pages);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_split_pages + 2ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    const unsigned char *interior_split_first_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    const unsigned char *interior_split_second_cell = interior_split_first_cell + branch_cell_size;
    const unsigned char *interior_split_third_cell = interior_split_second_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            interior_split_first_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == promoted_first_leaf_page
    );
    assert(
        get_test_u64_le(
            interior_split_second_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == before_interior_split_pages + 1ULL
    );
    assert(
        get_test_u64_le(
            interior_split_third_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == promoted_first_leaf_page + 1ULL
    );
    read_test_page(filename, promoted_first_leaf_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 3U
    );
    read_test_page(filename, before_interior_split_pages + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 1U
    );
    assert_index_entry_lookup(
        filename,
        0U,
        key_0,
        key_size,
        MYLITE_STORAGE_OK,
        rolled_back_row_0_id
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_0,
        key_size,
        rolled_back_row_0_id,
        row_0,
        sizeof(row_0)
    );
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 7U);
    assert(entries.key_bytes == 7U * key_size);
    assert_index_entry(&entries, 0U, rolled_back_row_0_id, key_0, key_size);
    assert_index_entry(&entries, 1U, row_1_id, key_1, key_size);
    assert_index_entry(&entries, 2U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 3U, row_3_id, key_3, key_size);
    assert_index_entry(&entries, 4U, row_6_id, key_6, key_size);
    assert_index_entry(&entries, 5U, row_4_id, key_4, key_size);
    assert_index_entry(&entries, 6U, row_5_id, key_5, key_size);
    mylite_storage_free_index_entryset(&entries);

    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_split_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    assert_index_entry_lookup(filename, 0U, key_0, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_OK, row_3_id);

    const pid_t interior_split_statement_pid = fork();
    assert(interior_split_statement_pid >= 0);
    if (interior_split_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_row_0_id = 0ULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_0,
                sizeof(row_0),
                row_0_entry,
                sizeof(row_0_entry) / sizeof(row_0_entry[0]),
                &child_row_0_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_0_id == before_interior_split_pages ? 0 : 4);
    }
    status = 0;
    assert(waitpid(interior_split_statement_pid, &status, 0) == interior_split_statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    assert_index_entry_lookup(filename, 0U, key_0, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_OK, row_3_id);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_split_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);

    const pid_t interior_split_transaction_pid = fork();
    assert(interior_split_transaction_pid >= 0);
    if (interior_split_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_row_0_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_0,
                sizeof(row_0),
                row_0_entry,
                sizeof(row_0_entry) / sizeof(row_0_entry[0]),
                &child_row_0_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_0_id == before_interior_split_pages ? 0 : 4);
    }
    status = 0;
    assert(waitpid(interior_split_transaction_pid, &status, 0) == interior_split_transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    assert_index_entry_lookup(filename, 0U, key_0, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_OK, row_3_id);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_interior_split_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_split_insert_pages = header.page_count;
    unsigned long long refold_rolled_back_row_7_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_delete_row(filename, "app", "posts", row_1_id) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_7,
            sizeof(row_7),
            row_7_entry,
            sizeof(row_7_entry) / sizeof(row_7_entry[0]),
            &refold_rolled_back_row_7_id
        ) == MYLITE_STORAGE_OK
    );
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_insert_pages + 4ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    const unsigned char *refold_first_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    const unsigned char *refold_second_branch_cell = refold_first_branch_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            refold_first_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == before_split_insert_pages + 2ULL
    );
    assert(
        get_test_u64_le(
            refold_second_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == before_split_insert_pages + 3ULL
    );
    assert_index_entry_lookup(
        filename,
        0U,
        key_7,
        key_size,
        MYLITE_STORAGE_OK,
        refold_rolled_back_row_7_id
    );
    assert_index_entry_lookup(filename, 0U, key_1, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_insert_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    assert_index_entry_lookup(filename, 0U, key_1, key_size, MYLITE_STORAGE_OK, row_1_id);
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);

    const pid_t overlay_transaction_pid = fork();
    assert(overlay_transaction_pid >= 0);
    if (overlay_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_row_7_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_1_id) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_7,
                sizeof(row_7),
                row_7_entry,
                sizeof(row_7_entry) / sizeof(row_7_entry[0]),
                &child_row_7_id
            ) != MYLITE_STORAGE_OK) {
            _exit(4);
        }
        _exit(child_row_7_id == 0ULL ? 5 : 0);
    }
    status = 0;
    assert(waitpid(overlay_transaction_pid, &status, 0) == overlay_transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    refold_first_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    refold_second_branch_cell = refold_first_branch_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            refold_first_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == before_split_insert_pages + 2ULL
    );
    assert(
        get_test_u64_le(
            refold_second_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == before_split_insert_pages + 3ULL
    );
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_insert_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    assert_index_entry_lookup(filename, 0U, key_1, key_size, MYLITE_STORAGE_OK, row_1_id);
    assert_index_entry_lookup(filename, 0U, key_6, key_size, MYLITE_STORAGE_OK, row_6_id);

    unsigned long long rolled_back_row_7_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_7,
            sizeof(row_7),
            row_7_entry,
            sizeof(row_7_entry) / sizeof(row_7_entry[0]),
            &rolled_back_row_7_id
        ) == MYLITE_STORAGE_OK
    );
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_insert_pages + 2ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);
    assert_index_entry_lookup(
        filename,
        0U,
        key_7,
        key_size,
        MYLITE_STORAGE_OK,
        rolled_back_row_7_id
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_insert_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);

    const pid_t split_statement_pid = fork();
    assert(split_statement_pid >= 0);
    if (split_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_row_7_id = 0ULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_7,
                sizeof(row_7),
                row_7_entry,
                sizeof(row_7_entry) / sizeof(row_7_entry[0]),
                &child_row_7_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_7_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(waitpid(split_statement_pid, &status, 0) == split_statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        7ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_insert_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);

    const pid_t split_transaction_pid = fork();
    assert(split_transaction_pid >= 0);
    if (split_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_row_7_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_7,
                sizeof(row_7),
                row_7_entry,
                sizeof(row_7_entry) / sizeof(row_7_entry[0]),
                &child_row_7_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_7_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(waitpid(split_transaction_pid, &status, 0) == split_transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        7ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_insert_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);

    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_7,
            sizeof(row_7),
            row_7_entry,
            sizeof(row_7_entry) / sizeof(row_7_entry[0]),
            &row_7_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_insert_pages + 2ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        7ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    const unsigned char *split_branch_cell = root_page_bytes +
                                             MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET +
                                             (2U * branch_cell_size);
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == before_split_insert_pages + 1ULL
    );
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_7_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_7,
            key_size
        ) == 0
    );
    read_test_page(filename, before_split_insert_pages + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 1U
    );
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_OK, row_7_id);
    assert_find_indexed_row_equals(filename, 0U, key_7, key_size, row_7_id, row_7, sizeof(row_7));

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_final_leaf_append_pages = header.page_count;
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_8,
            sizeof(row_8),
            row_8_entry,
            sizeof(row_8_entry) / sizeof(row_8_entry[0]),
            &row_8_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_append_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    read_test_page(filename, before_split_insert_pages + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 2U
    );
    assert_index_entry_lookup(filename, 0U, key_8, key_size, MYLITE_STORAGE_OK, row_8_id);
    assert_find_indexed_row_equals(filename, 0U, key_8, key_size, row_8_id, row_8, sizeof(row_8));

    const unsigned long long before_final_leaf_delete_pages = header.page_count;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_delete_row(filename, "app", "posts", row_8_id) == MYLITE_STORAGE_OK);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_delete_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        7ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_7_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_7,
            key_size
        ) == 0
    );
    read_test_page(filename, before_split_insert_pages + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 1U
    );
    assert_index_entry_lookup(filename, 0U, key_8, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_delete_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    assert_index_entry_lookup(filename, 0U, key_8, key_size, MYLITE_STORAGE_OK, row_8_id);

    const pid_t final_delete_statement_pid = fork();
    assert(final_delete_statement_pid >= 0);
    if (final_delete_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_8_id) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(final_delete_statement_pid, &status, 0) == final_delete_statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        7ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_7_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_7,
            key_size
        ) == 0
    );
    assert_index_entry_lookup(filename, 0U, key_8, key_size, MYLITE_STORAGE_OK, row_8_id);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_delete_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    assert_index_entry_lookup(filename, 0U, key_8, key_size, MYLITE_STORAGE_OK, row_8_id);

    const pid_t final_delete_transaction_pid = fork();
    assert(final_delete_transaction_pid >= 0);
    if (final_delete_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_8_id) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(final_delete_transaction_pid, &status, 0) == final_delete_transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        7ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_7_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_7,
            key_size
        ) == 0
    );
    assert_index_entry_lookup(filename, 0U, key_8, key_size, MYLITE_STORAGE_OK, row_8_id);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_delete_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    assert_index_entry_lookup(filename, 0U, key_8, key_size, MYLITE_STORAGE_OK, row_8_id);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_8_id) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_delete_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        7ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_7_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_7,
            key_size
        ) == 0
    );
    read_test_page(filename, before_split_insert_pages + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 1U
    );
    assert_index_entry_lookup(filename, 0U, key_8, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_8, key_size);
    assert_exact_index_entries(filename, 0U, key_8, key_size, NULL, 0U);

    const unsigned long long before_final_leaf_update_append_pages = header.page_count;
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_9,
            sizeof(row_9),
            row_9_entry,
            sizeof(row_9_entry) / sizeof(row_9_entry[0]),
            &row_9_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_update_append_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    read_test_page(filename, before_split_insert_pages + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 2U
    );
    assert_index_entry_lookup(filename, 0U, key_9, key_size, MYLITE_STORAGE_OK, row_9_id);
    assert_find_indexed_row_equals(filename, 0U, key_9, key_size, row_9_id, row_9, sizeof(row_9));

    const unsigned long long before_final_leaf_update_pages = header.page_count;
    unsigned long long rolled_back_row_10_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_9_id,
            row_10,
            sizeof(row_10),
            row_10_entry,
            sizeof(row_10_entry) / sizeof(row_10_entry[0]),
            &rolled_back_row_10_id
        ) == MYLITE_STORAGE_OK
    );
    assert(access(journal_filename, F_OK) == 0);
    assert(rolled_back_row_10_id == before_final_leaf_update_pages);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_update_pages + 2ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        8ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    split_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET +
                        (2U * branch_cell_size);
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == rolled_back_row_10_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_10,
            key_size
        ) == 0
    );
    read_test_page(filename, before_split_insert_pages + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 2U
    );
    assert_index_entry_lookup(filename, 0U, key_9, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(
        filename,
        0U,
        key_10,
        key_size,
        MYLITE_STORAGE_OK,
        rolled_back_row_10_id
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_update_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    read_test_page(filename, root_page, root_page_bytes);
    split_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET +
                        (2U * branch_cell_size);
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_9_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_9,
            key_size
        ) == 0
    );
    assert_index_entry_lookup(filename, 0U, key_9, key_size, MYLITE_STORAGE_OK, row_9_id);
    assert_index_entry_lookup(filename, 0U, key_10, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);

    const pid_t final_update_statement_pid = fork();
    assert(final_update_statement_pid >= 0);
    if (final_update_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_row_10_id = 0ULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_9_id,
                row_10,
                sizeof(row_10),
                row_10_entry,
                sizeof(row_10_entry) / sizeof(row_10_entry[0]),
                &child_row_10_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_10_id == before_final_leaf_update_pages ? 0 : 4);
    }
    status = 0;
    assert(waitpid(final_update_statement_pid, &status, 0) == final_update_statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        8ULL
    );
    split_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET +
                        (2U * branch_cell_size);
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == before_final_leaf_update_pages
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_10,
            key_size
        ) == 0
    );
    assert_index_entry_lookup(filename, 0U, key_10, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_update_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    assert_index_entry_lookup(filename, 0U, key_9, key_size, MYLITE_STORAGE_OK, row_9_id);

    const pid_t final_update_transaction_pid = fork();
    assert(final_update_transaction_pid >= 0);
    if (final_update_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_row_10_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_update_row_with_index_entries(
                filename,
                "app",
                "posts",
                row_9_id,
                row_10,
                sizeof(row_10),
                row_10_entry,
                sizeof(row_10_entry) / sizeof(row_10_entry[0]),
                &child_row_10_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_10_id == before_final_leaf_update_pages ? 0 : 4);
    }
    status = 0;
    assert(waitpid(final_update_transaction_pid, &status, 0) == final_update_transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        8ULL
    );
    split_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET +
                        (2U * branch_cell_size);
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == before_final_leaf_update_pages
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_10,
            key_size
        ) == 0
    );
    assert_index_entry_lookup(filename, 0U, key_10, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_update_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    assert_index_entry_lookup(filename, 0U, key_9, key_size, MYLITE_STORAGE_OK, row_9_id);

    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_9_id,
            row_10,
            sizeof(row_10),
            row_10_entry,
            sizeof(row_10_entry) / sizeof(row_10_entry[0]),
            &row_10_id
        ) == MYLITE_STORAGE_OK
    );
    assert(row_10_id == before_final_leaf_update_pages);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_leaf_update_pages + 2ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 8ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        8ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    split_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET +
                        (2U * branch_cell_size);
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_10_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_10,
            key_size
        ) == 0
    );
    read_test_page(filename, before_split_insert_pages + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 2U
    );
    assert_index_entry_lookup(filename, 0U, key_9, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_10, key_size, MYLITE_STORAGE_OK, row_10_id);
    assert_find_indexed_row_not_found(filename, 0U, key_9, key_size);
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_10,
        key_size,
        row_10_id,
        row_10,
        sizeof(row_10)
    );
    assert_exact_index_entries(filename, 0U, key_9, key_size, NULL, 0U);
    const unsigned long long final_update_row_ids[] = {row_10_id};
    assert_exact_index_entries(
        filename,
        0U,
        key_10,
        key_size,
        final_update_row_ids,
        sizeof(final_update_row_ids) / sizeof(final_update_row_ids[0])
    );
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 8U);
    assert(entries.key_bytes == 8U * key_size);
    assert_index_entry(&entries, 0U, row_1_id, key_1, key_size);
    assert_index_entry(&entries, 1U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 2U, row_3_id, key_3, key_size);
    assert_index_entry(&entries, 3U, row_6_id, key_6, key_size);
    assert_index_entry(&entries, 4U, row_4_id, key_4, key_size);
    assert_index_entry(&entries, 5U, row_5_id, key_5, key_size);
    assert_index_entry(&entries, 6U, row_7_id, key_7, key_size);
    assert_index_entry(&entries, 7U, row_10_id, key_10, key_size);
    mylite_storage_free_index_entryset(&entries);

    const unsigned long long before_updated_final_leaf_delete_pages = header.page_count;
    assert(mylite_storage_delete_row(filename, "app", "posts", row_10_id) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_updated_final_leaf_delete_pages + 1ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        7ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    split_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET +
                        (2U * branch_cell_size);
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_7_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_7,
            key_size
        ) == 0
    );
    read_test_page(filename, before_split_insert_pages + 1ULL, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 1U
    );
    assert_index_entry_lookup(filename, 0U, key_10, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_10, key_size);
    assert_exact_index_entries(filename, 0U, key_10, key_size, NULL, 0U);

    const unsigned long long before_final_child_removal_pages = header.page_count;
    const unsigned long long before_final_child_removal_free_list_root = header.free_list_root_page;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_delete_row(filename, "app", "posts", row_7_id) == MYLITE_STORAGE_OK);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_child_removal_pages + 1ULL);
    assert(header.free_list_root_page == before_split_insert_pages + 1ULL);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_final_child_removal_free_list_root,
        before_split_insert_pages + 1ULL,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    split_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + branch_cell_size;
    const unsigned long long branch_root_collapse_leaf_page = get_test_u64_le(
        split_branch_cell,
        MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
    );
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_5_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_5,
            key_size
        ) == 0
    );
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_child_removal_pages);
    assert(header.free_list_root_page == before_final_child_removal_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    split_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET +
                        (2U * branch_cell_size);
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_7_id
    );
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_OK, row_7_id);

    const pid_t final_child_removal_statement_pid = fork();
    assert(final_child_removal_statement_pid >= 0);
    if (final_child_removal_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_7_id) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(
        waitpid(final_child_removal_statement_pid, &status, 0) == final_child_removal_statement_pid
    );
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    split_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + branch_cell_size;
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_5_id
    );
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_OK, row_7_id);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_child_removal_pages);
    assert(header.free_list_root_page == before_final_child_removal_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);

    const pid_t final_child_removal_transaction_pid = fork();
    assert(final_child_removal_transaction_pid >= 0);
    if (final_child_removal_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_7_id) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(
        waitpid(final_child_removal_transaction_pid, &status, 0) ==
        final_child_removal_transaction_pid
    );
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    split_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + branch_cell_size;
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_5_id
    );
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_OK, row_7_id);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_child_removal_pages);
    assert(header.free_list_root_page == before_final_child_removal_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_7_id) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_final_child_removal_pages + 1ULL);
    assert(header.free_list_root_page == before_split_insert_pages + 1ULL);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_final_child_removal_free_list_root,
        before_split_insert_pages + 1ULL,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    split_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + branch_cell_size;
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_5_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_5,
            key_size
        ) == 0
    );
    assert_index_entry_lookup(filename, 0U, key_7, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_7, key_size);
    assert_exact_index_entries(filename, 0U, key_7, key_size, NULL, 0U);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_6_id) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 0U, root_page, 5ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    split_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + branch_cell_size;
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_5_id
    );
    assert_index_entry_lookup(filename, 0U, key_6, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_6, key_size);
    assert_exact_index_entries(filename, 0U, key_6, key_size, NULL, 0U);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_5_id) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    split_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + branch_cell_size;
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == row_4_id
    );
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_5, key_size);
    assert_exact_index_entries(filename, 0U, key_5, key_size, NULL, 0U);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_1_id) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    assert_index_entry_lookup(filename, 0U, key_1, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_4, key_size, MYLITE_STORAGE_OK, row_4_id);
    assert_find_indexed_row_not_found(filename, 0U, key_1, key_size);
    assert_find_indexed_row_equals(filename, 0U, key_4, key_size, row_4_id, row_4, sizeof(row_4));
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 3U);
    assert(entries.key_bytes == 3U * key_size);
    assert_index_entry(&entries, 0U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 1U, row_3_id, key_3, key_size);
    assert_index_entry(&entries, 2U, row_4_id, key_4, key_size);
    mylite_storage_free_index_entryset(&entries);

    assert(
        mylite_storage_update_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_4_id,
            updated_row_4,
            sizeof(updated_row_4),
            updated_row_4_entry,
            sizeof(updated_row_4_entry) / sizeof(updated_row_4_entry[0]),
            &updated_row_4_id
        ) == MYLITE_STORAGE_OK
    );
    assert(updated_row_4_id != row_4_id);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    split_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + branch_cell_size;
    assert(
        get_test_u64_le(
            split_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_ROW_ID_OFFSET
        ) == updated_row_4_id
    );
    assert(
        memcmp(
            split_branch_cell + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_MAX_KEY_OFFSET,
            key_5,
            key_size
        ) == 0
    );
    assert_index_entry_lookup(filename, 0U, key_4, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_OK, updated_row_4_id);
    assert_find_indexed_row_not_found(filename, 0U, key_4, key_size);
    assert_find_indexed_row_equals(
        filename,
        0U,
        key_5,
        key_size,
        updated_row_4_id,
        updated_row_4,
        sizeof(updated_row_4)
    );
    const unsigned long long updated_overflow_row_ids[] = {updated_row_4_id};
    assert_exact_index_entries(
        filename,
        0U,
        key_5,
        key_size,
        updated_overflow_row_ids,
        sizeof(updated_overflow_row_ids) / sizeof(updated_overflow_row_ids[0])
    );
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 3U);
    assert(entries.key_bytes == 3U * key_size);
    assert_index_entry(&entries, 0U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 1U, row_3_id, key_3, key_size);
    assert_index_entry(&entries, 2U, updated_row_4_id, key_5, key_size);
    mylite_storage_free_index_entryset(&entries);

    const unsigned long long before_branch_root_collapse_pages = header.page_count;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_delete_row(filename, "app", "posts", updated_row_4_id) == MYLITE_STORAGE_OK
    );
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_root_collapse_pages + 1ULL);
    assert(header.free_list_root_page == branch_root_collapse_leaf_page);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_split_insert_pages + 1ULL,
        branch_root_collapse_leaf_page,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 2ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_root_collapse_pages);
    assert(header.free_list_root_page == before_split_insert_pages + 1ULL);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_final_child_removal_free_list_root,
        before_split_insert_pages + 1ULL,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_OK, updated_row_4_id);

    const pid_t branch_root_collapse_statement_pid = fork();
    assert(branch_root_collapse_statement_pid >= 0);
    if (branch_root_collapse_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", updated_row_4_id) !=
            MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(
        waitpid(branch_root_collapse_statement_pid, &status, 0) ==
        branch_root_collapse_statement_pid
    );
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_OK, updated_row_4_id);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_root_collapse_pages);
    assert(header.free_list_root_page == before_split_insert_pages + 1ULL);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_final_child_removal_free_list_root,
        before_split_insert_pages + 1ULL,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );

    const pid_t branch_root_collapse_transaction_pid = fork();
    assert(branch_root_collapse_transaction_pid >= 0);
    if (branch_root_collapse_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", updated_row_4_id) !=
            MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(
        waitpid(branch_root_collapse_transaction_pid, &status, 0) ==
        branch_root_collapse_transaction_pid
    );
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_OK, updated_row_4_id);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_root_collapse_pages);
    assert(header.free_list_root_page == before_split_insert_pages + 1ULL);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_final_child_removal_free_list_root,
        before_split_insert_pages + 1ULL,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 4ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );

    assert(
        mylite_storage_delete_row(filename, "app", "posts", updated_row_4_id) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_branch_root_collapse_pages + 1ULL);
    assert(header.free_list_root_page == branch_root_collapse_leaf_page);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_split_insert_pages + 1ULL,
        branch_root_collapse_leaf_page,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 2ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_entry_lookup(filename, 0U, key_5, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_5, key_size);
    assert_exact_index_entries(filename, 0U, key_5, key_size, NULL, 0U);
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 2U);
    assert(entries.key_bytes == 2U * key_size);
    assert_index_entry(&entries, 0U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 1U, row_3_id, key_3, key_size);
    mylite_storage_free_index_entryset(&entries);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(transaction_journal_filename);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_branch_arbitrary_child_removal(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_0[] = {0x00U, 0x00U, 'z'};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a'};
    static const unsigned char row_2[] = {0x00U, 0x02U, 'b'};
    static const unsigned char row_3[] = {0x00U, 0x03U, 'c'};
    static const unsigned char row_4[] = {0x00U, 0x04U, 'd'};
    static const unsigned char row_5[] = {0x00U, 0x05U, 'e'};
    static const unsigned char row_6[] = {0x00U, 0x06U, 'f'};
    static const size_t key_size = 1322U;
    char *root = make_temp_root();
    char *filename = path_join(root, "branch-arbitrary-child-removal.mylite");
    char *journal_filename = journal_path(filename);
    char *transaction_journal_filename = transaction_journal_path(filename);
    unsigned char key_0[1322U] = {0};
    unsigned char key_1[1322U] = {0};
    unsigned char key_2[1322U] = {0};
    unsigned char key_3[1322U] = {0};
    unsigned char key_4[1322U] = {0};
    unsigned char key_5[1322U] = {0};
    unsigned char key_6[1322U] = {0};
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_index_entry row_0_entry[] = {
        {
            .size = sizeof(row_0_entry[0]),
            .index_number = 0U,
            .key = key_0,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_1_entry[] = {
        {
            .size = sizeof(row_1_entry[0]),
            .index_number = 0U,
            .key = key_1,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_2_entry[] = {
        {
            .size = sizeof(row_2_entry[0]),
            .index_number = 0U,
            .key = key_2,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_3_entry[] = {
        {
            .size = sizeof(row_3_entry[0]),
            .index_number = 0U,
            .key = key_3,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_4_entry[] = {
        {
            .size = sizeof(row_4_entry[0]),
            .index_number = 0U,
            .key = key_4,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_5_entry[] = {
        {
            .size = sizeof(row_5_entry[0]),
            .index_number = 0U,
            .key = key_5,
            .key_size = key_size,
        },
    };
    mylite_storage_index_entry row_6_entry[] = {
        {
            .size = sizeof(row_6_entry[0]),
            .index_number = 0U,
            .key = key_6,
            .key_size = key_size,
        },
    };
    unsigned long long row_0_id = 0ULL;
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_3_id = 0ULL;
    unsigned long long row_4_id = 0ULL;
    unsigned long long row_5_id = 0ULL;
    unsigned long long row_6_id = 0ULL;
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    mylite_storage_statement *statement = NULL;
    unsigned char root_page_bytes[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    int status = 0;

    const size_t root_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_HEADER_SIZE + key_size);
    const size_t branch_cell_size = MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_HEADER_SIZE + key_size;
    assert(root_capacity == 3U);
    key_1[0] = 0x01U;
    key_2[0] = 0x02U;
    key_3[0] = 0x03U;
    key_4[0] = 0x04U;
    key_5[0] = 0x05U;
    key_6[0] = 0x03U;
    key_6[1] = 0x80U;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_1,
            sizeof(row_1),
            row_1_entry,
            sizeof(row_1_entry) / sizeof(row_1_entry[0]),
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
            row_2_entry,
            sizeof(row_2_entry) / sizeof(row_2_entry[0]),
            &row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_3,
            sizeof(row_3),
            row_3_entry,
            sizeof(row_3_entry) / sizeof(row_3_entry[0]),
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_6,
            sizeof(row_6),
            row_6_entry,
            sizeof(row_6_entry) / sizeof(row_6_entry[0]),
            &row_6_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_4,
            sizeof(row_4),
            row_4_entry,
            sizeof(row_4_entry) / sizeof(row_4_entry[0]),
            &row_4_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_5,
            sizeof(row_5),
            row_5_entry,
            sizeof(row_5_entry) / sizeof(row_5_entry[0]),
            &row_5_id
        ) == MYLITE_STORAGE_OK
    );

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_rebuild_pages = header.page_count;
    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long root_page = before_rebuild_pages;
    const unsigned long long first_leaf_page = root_page + 1ULL;
    const unsigned long long second_leaf_page = root_page + 2ULL;
    assert(header.page_count == before_rebuild_pages + 3ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    const unsigned char *first_branch_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    const unsigned char *second_branch_cell = first_branch_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            first_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == first_leaf_page
    );
    assert(
        get_test_u64_le(
            second_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == second_leaf_page
    );

    const unsigned long long before_same_statement_split_pages = header.page_count;
    const unsigned long long before_same_statement_free_list_root = header.free_list_root_page;
    unsigned long long rolled_back_row_0_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_0,
            sizeof(row_0),
            row_0_entry,
            sizeof(row_0_entry) / sizeof(row_0_entry[0]),
            &rolled_back_row_0_id
        ) == MYLITE_STORAGE_OK
    );
    assert(rolled_back_row_0_id == before_same_statement_split_pages);
    assert(mylite_storage_delete_row(filename, "app", "posts", row_3_id) == MYLITE_STORAGE_OK);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_same_statement_split_pages + 3ULL);
    assert(header.free_list_root_page == before_same_statement_split_pages + 1ULL);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_same_statement_free_list_root,
        before_same_statement_split_pages + 1ULL,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    first_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    second_branch_cell = first_branch_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            first_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == first_leaf_page
    );
    assert(
        get_test_u64_le(
            second_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == second_leaf_page
    );
    assert_index_entry_lookup(
        filename,
        0U,
        key_0,
        key_size,
        MYLITE_STORAGE_OK,
        rolled_back_row_0_id
    );
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_same_statement_split_pages);
    assert(header.free_list_root_page == before_same_statement_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    assert_index_entry_lookup(filename, 0U, key_0, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_OK, row_3_id);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_interior_split_pages = header.page_count;
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            row_0,
            sizeof(row_0),
            row_0_entry,
            sizeof(row_0_entry) / sizeof(row_0_entry[0]),
            &row_0_id
        ) == MYLITE_STORAGE_OK
    );
    assert(row_0_id == before_interior_split_pages);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long removed_leaf_page = before_interior_split_pages + 1ULL;
    assert(header.page_count == before_interior_split_pages + 2ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    first_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    second_branch_cell = first_branch_cell + branch_cell_size;
    const unsigned char *third_branch_cell = second_branch_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            first_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == first_leaf_page
    );
    assert(
        get_test_u64_le(
            second_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == removed_leaf_page
    );
    assert(
        get_test_u64_le(
            third_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == second_leaf_page
    );
    read_test_page(filename, removed_leaf_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 1U
    );
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 7U);
    assert(entries.key_bytes == 7U * key_size);
    assert_index_entry(&entries, 0U, row_0_id, key_0, key_size);
    assert_index_entry(&entries, 1U, row_1_id, key_1, key_size);
    assert_index_entry(&entries, 2U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 3U, row_3_id, key_3, key_size);
    assert_index_entry(&entries, 4U, row_6_id, key_6, key_size);
    assert_index_entry(&entries, 5U, row_4_id, key_4, key_size);
    assert_index_entry(&entries, 6U, row_5_id, key_5, key_size);
    mylite_storage_free_index_entryset(&entries);

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_removal_pages = header.page_count;
    const unsigned long long before_removal_free_list_root = header.free_list_root_page;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_delete_row(filename, "app", "posts", row_3_id) == MYLITE_STORAGE_OK);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_removal_pages + 1ULL);
    assert(header.free_list_root_page == removed_leaf_page);
    assert_free_list_run(
        filename,
        removed_leaf_page,
        before_removal_free_list_root,
        removed_leaf_page,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    first_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    second_branch_cell = first_branch_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            first_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == first_leaf_page
    );
    assert(
        get_test_u64_le(
            second_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == second_leaf_page
    );
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_0, key_size, MYLITE_STORAGE_OK, row_0_id);
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 6U);
    assert(entries.key_bytes == 6U * key_size);
    assert_index_entry(&entries, 0U, row_0_id, key_0, key_size);
    assert_index_entry(&entries, 1U, row_1_id, key_1, key_size);
    assert_index_entry(&entries, 2U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 3U, row_6_id, key_6, key_size);
    assert_index_entry(&entries, 4U, row_4_id, key_4, key_size);
    assert_index_entry(&entries, 5U, row_5_id, key_5, key_size);
    mylite_storage_free_index_entryset(&entries);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_removal_pages);
    assert(header.free_list_root_page == before_removal_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        3U
    );
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_OK, row_3_id);
    assert_index_entry_lookup(filename, 0U, key_0, key_size, MYLITE_STORAGE_OK, row_0_id);

    const pid_t removal_statement_pid = fork();
    assert(removal_statement_pid >= 0);
    if (removal_statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_3_id) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(removal_statement_pid, &status, 0) == removal_statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_OK, row_3_id);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_removal_pages);
    assert(header.free_list_root_page == before_removal_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);

    const pid_t removal_transaction_pid = fork();
    assert(removal_transaction_pid >= 0);
    if (removal_transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_3_id) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(removal_transaction_pid, &status, 0) == removal_transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_OK, row_3_id);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_removal_pages);
    assert(header.free_list_root_page == before_removal_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, 7ULL);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_3_id) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_removal_pages + 1ULL);
    assert(header.free_list_root_page == removed_leaf_page);
    assert_free_list_run(
        filename,
        removed_leaf_page,
        before_removal_free_list_root,
        removed_leaf_page,
        1ULL
    );
    assert_index_root(filename, "app", "posts", 0U, root_page, 6ULL);
    read_test_page(filename, root_page, root_page_bytes);
    assert(
        get_test_u64_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        6ULL
    );
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    first_branch_cell = root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    second_branch_cell = first_branch_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            first_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == first_leaf_page
    );
    assert(
        get_test_u64_le(
            second_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == second_leaf_page
    );
    assert_index_entry_lookup(filename, 0U, key_3, key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_find_indexed_row_not_found(filename, 0U, key_3, key_size);
    assert_index_entry_lookup(filename, 0U, key_0, key_size, MYLITE_STORAGE_OK, row_0_id);
    assert_find_indexed_row_equals(filename, 0U, key_0, key_size, row_0_id, row_0, sizeof(row_0));
    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 6U);
    assert(entries.key_bytes == 6U * key_size);
    assert_index_entry(&entries, 0U, row_0_id, key_0, key_size);
    assert_index_entry(&entries, 1U, row_1_id, key_1, key_size);
    assert_index_entry(&entries, 2U, row_2_id, key_2, key_size);
    assert_index_entry(&entries, 3U, row_6_id, key_6, key_size);
    assert_index_entry(&entries, 4U, row_4_id, key_4, key_size);
    assert_index_entry(&entries, 5U, row_5_id, key_5, key_size);
    mylite_storage_free_index_entryset(&entries);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(transaction_journal_filename);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_branch_refold_child_count_delete(void) {
    enum {
        entry_count = 7U,
        key_size = 1322U,
    };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    unsigned char rows[entry_count][4] = {{0}};
    unsigned char keys[entry_count][key_size] = {{0}};
    unsigned long long row_ids[entry_count] = {0};
    char *root = make_temp_root();
    char *filename = path_join(root, "branch-refold-child-count-delete.mylite");
    char *journal_filename = journal_path(filename);
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
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    mylite_storage_statement *statement = NULL;
    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    int status = 0;

    const size_t leaf_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size);
    const size_t root_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_HEADER_SIZE + key_size);
    const size_t branch_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_HEADER_SIZE + key_size);
    const size_t branch_cell_size = MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_HEADER_SIZE + key_size;
    assert(leaf_capacity == 3U);
    assert(root_capacity == 3U);
    assert(branch_capacity >= 3U);

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < entry_count; ++i) {
        put_test_u32_le(rows[i], 0U, (unsigned)i + 1U);
        keys[i][0] = (unsigned char)(i + 1U);
        mylite_storage_index_entry index_entry = {
            .size = sizeof(index_entry),
            .index_number = 0U,
            .key = keys[i],
            .key_size = key_size,
        };
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                rows[i],
                sizeof(rows[i]),
                &index_entry,
                1U,
                row_ids + i
            ) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_rebuild_pages = header.page_count;
    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long root_page = before_rebuild_pages;
    const unsigned long long first_leaf_page = root_page + 1ULL;
    const unsigned long long second_leaf_page = root_page + 2ULL;
    const unsigned long long reclaimed_leaf_page = root_page + 3ULL;
    const unsigned long long before_delete_pages = header.page_count;
    const unsigned long long before_delete_free_list_root = header.free_list_root_page;
    assert(header.page_count == before_rebuild_pages + 4ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count);

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_delete_row(filename, "app", "posts", row_ids[4]) == MYLITE_STORAGE_OK);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages + 1ULL);
    assert(header.free_list_root_page == reclaimed_leaf_page);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count - 1U);
    read_test_page(filename, root_page, page);
    assert(
        get_test_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        entry_count - 1U
    );
    assert(get_test_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) == 2U);
    const unsigned char *first_branch_cell =
        page + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    const unsigned char *second_branch_cell = first_branch_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            first_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == first_leaf_page
    );
    assert(
        get_test_u64_le(
            second_branch_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == second_leaf_page
    );
    read_test_page(filename, first_leaf_page, page);
    assert(get_test_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 3U);
    read_test_page(filename, second_leaf_page, page);
    assert(get_test_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET) == 3U);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_delete_free_list_root,
        reclaimed_leaf_page,
        1ULL
    );
    assert_index_entry_lookup(filename, 0U, keys[4], key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, keys[6], key_size, MYLITE_STORAGE_OK, row_ids[6]);
    assert_find_indexed_row_equals(
        filename,
        0U,
        keys[6],
        key_size,
        row_ids[6],
        rows[6],
        sizeof(rows[6])
    );
    const unsigned char *expected_prefix_keys[] = {keys[6]};
    const unsigned long long expected_prefix_row_ids[] = {row_ids[6]};
    assert_prefix_index_entries(
        filename,
        0U,
        keys[6],
        1U,
        expected_prefix_keys,
        key_size,
        expected_prefix_row_ids,
        sizeof(expected_prefix_row_ids) / sizeof(expected_prefix_row_ids[0])
    );
    assert_index_prefix_exists_for_index(filename, 0U, keys[6], 1U, 0ULL, 1);
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == entry_count - 1U);
    assert_index_entry(&entries, 0U, row_ids[0], keys[0], key_size);
    assert_index_entry(&entries, 1U, row_ids[1], keys[1], key_size);
    assert_index_entry(&entries, 2U, row_ids[2], keys[2], key_size);
    assert_index_entry(&entries, 3U, row_ids[3], keys[3], key_size);
    assert_index_entry(&entries, 4U, row_ids[5], keys[5], key_size);
    assert_index_entry(&entries, 5U, row_ids[6], keys[6], key_size);
    mylite_storage_free_index_entryset(&entries);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages);
    assert(header.free_list_root_page == before_delete_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count);
    assert_index_entry_lookup(filename, 0U, keys[4], key_size, MYLITE_STORAGE_OK, row_ids[4]);

    const pid_t statement_pid = fork();
    assert(statement_pid >= 0);
    if (statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_ids[4]) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(statement_pid, &status, 0) == statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, keys[4], key_size, MYLITE_STORAGE_OK, row_ids[4]);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages);
    assert(header.free_list_root_page == before_delete_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count);

    const pid_t transaction_pid = fork();
    assert(transaction_pid >= 0);
    if (transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_ids[4]) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(transaction_pid, &status, 0) == transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, keys[4], key_size, MYLITE_STORAGE_OK, row_ids[4]);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages);
    assert(header.free_list_root_page == before_delete_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_ids[4]) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages + 1ULL);
    assert(header.free_list_root_page == reclaimed_leaf_page);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count - 1U);
    read_test_page(filename, root_page, page);
    assert(get_test_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) == 2U);
    assert_index_entry_lookup(filename, 0U, keys[4], key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, keys[6], key_size, MYLITE_STORAGE_OK, row_ids[6]);
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == entry_count - 1U);
    assert_index_entry(&entries, 0U, row_ids[0], keys[0], key_size);
    assert_index_entry(&entries, 1U, row_ids[1], keys[1], key_size);
    assert_index_entry(&entries, 2U, row_ids[2], keys[2], key_size);
    assert_index_entry(&entries, 3U, row_ids[3], keys[3], key_size);
    assert_index_entry(&entries, 4U, row_ids[5], keys[5], key_size);
    assert_index_entry(&entries, 5U, row_ids[6], keys[6], key_size);
    mylite_storage_free_index_entryset(&entries);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(transaction_journal_filename);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_branch_child_count_delete_collapse(void) {
    enum {
        entry_count = 4U,
        key_size = 1322U,
    };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    unsigned char rows[entry_count][4] = {{0}};
    unsigned char keys[entry_count][key_size] = {{0}};
    unsigned long long row_ids[entry_count] = {0};
    char *root = make_temp_root();
    char *filename = path_join(root, "branch-child-count-delete-collapse.mylite");
    char *journal_filename = journal_path(filename);
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
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    mylite_storage_statement *statement = NULL;
    int status = 0;

    const size_t leaf_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size);
    const size_t root_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_ROOT_ENTRY_HEADER_SIZE + key_size);
    assert(leaf_capacity == 3U);
    assert(root_capacity == 3U);

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < entry_count; ++i) {
        put_test_u32_le(rows[i], 0U, (unsigned)i + 1U);
        keys[i][0] = (unsigned char)(i + 1U);
        mylite_storage_index_entry index_entry = {
            .size = sizeof(index_entry),
            .index_number = 0U,
            .key = keys[i],
            .key_size = key_size,
        };
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                rows[i],
                sizeof(rows[i]),
                &index_entry,
                1U,
                row_ids + i
            ) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_rebuild_pages = header.page_count;
    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long root_page = before_rebuild_pages;
    const unsigned long long reclaimed_leaf_page = root_page + 2ULL;
    const unsigned long long before_delete_pages = header.page_count;
    const unsigned long long before_delete_free_list_root = header.free_list_root_page;
    assert(header.page_count == before_rebuild_pages + 3ULL);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );

    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_delete_row(filename, "app", "posts", row_ids[1]) == MYLITE_STORAGE_OK);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages + 1ULL);
    assert(header.free_list_root_page == reclaimed_leaf_page);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count - 1U);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_entry_lookup(filename, 0U, keys[1], key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, keys[3], key_size, MYLITE_STORAGE_OK, row_ids[3]);
    assert_find_indexed_row_equals(
        filename,
        0U,
        keys[3],
        key_size,
        row_ids[3],
        rows[3],
        sizeof(rows[3])
    );
    const unsigned char *expected_prefix_keys[] = {keys[3]};
    const unsigned long long expected_prefix_row_ids[] = {row_ids[3]};
    assert_prefix_index_entries(
        filename,
        0U,
        keys[3],
        1U,
        expected_prefix_keys,
        key_size,
        expected_prefix_row_ids,
        sizeof(expected_prefix_row_ids) / sizeof(expected_prefix_row_ids[0])
    );
    assert_index_prefix_exists_for_index(filename, 0U, keys[3], 1U, 0ULL, 1);
    assert_free_list_run(
        filename,
        header.free_list_root_page,
        before_delete_free_list_root,
        reclaimed_leaf_page,
        1ULL
    );
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == entry_count - 1U);
    assert_index_entry(&entries, 0U, row_ids[0], keys[0], key_size);
    assert_index_entry(&entries, 1U, row_ids[2], keys[2], key_size);
    assert_index_entry(&entries, 2U, row_ids[3], keys[3], key_size);
    mylite_storage_free_index_entryset(&entries);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages);
    assert(header.free_list_root_page == before_delete_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    assert_index_entry_lookup(filename, 0U, keys[1], key_size, MYLITE_STORAGE_OK, row_ids[1]);

    const pid_t statement_pid = fork();
    assert(statement_pid >= 0);
    if (statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_ids[1]) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(statement_pid, &status, 0) == statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, keys[1], key_size, MYLITE_STORAGE_OK, row_ids[1]);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages);
    assert(header.free_list_root_page == before_delete_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count);

    const pid_t transaction_pid = fork();
    assert(transaction_pid >= 0);
    if (transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_delete_row(filename, "app", "posts", row_ids[1]) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(0);
    }
    status = 0;
    assert(waitpid(transaction_pid, &status, 0) == transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    assert_index_entry_lookup(filename, 0U, keys[1], key_size, MYLITE_STORAGE_OK, row_ids[1]);
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages);
    assert(header.free_list_root_page == before_delete_free_list_root);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count);

    assert(mylite_storage_delete_row(filename, "app", "posts", row_ids[1]) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_delete_pages + 1ULL);
    assert(header.free_list_root_page == reclaimed_leaf_page);
    assert_index_root(filename, "app", "posts", 0U, root_page, entry_count - 1U);
    assert_index_root_page_type(
        filename,
        root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_entry_lookup(filename, 0U, keys[1], key_size, MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, keys[3], key_size, MYLITE_STORAGE_OK, row_ids[3]);
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == entry_count - 1U);
    assert_index_entry(&entries, 0U, row_ids[0], keys[0], key_size);
    assert_index_entry(&entries, 1U, row_ids[2], keys[2], key_size);
    assert_index_entry(&entries, 2U, row_ids[3], keys[3], key_size);
    mylite_storage_free_index_entryset(&entries);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(transaction_journal_filename);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_maintained_index_root_transaction_rollback(void) {
    maintained_index_root_rollback_fixture fixture = {0};
    mylite_storage_statement *statement = NULL;
    mylite_storage_statement *transaction = NULL;
    mylite_storage_statement *savepoint = NULL;
    unsigned long long row_3_id = 0ULL;
    unsigned long long updated_row_2_id = 0ULL;
    int status = 0;
    unsigned char post_insert_primary_root[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char post_insert_secondary_root[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};

    prepare_maintained_index_root_rollback_fixture(
        &fixture,
        "maintained-index-root-transaction-rollback.mylite"
    );

    assert(mylite_storage_begin_statement(fixture.filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            fixture.filename,
            "app",
            "posts",
            k_maintained_root_row_3,
            sizeof(k_maintained_root_row_3),
            k_maintained_root_row_3_entries,
            sizeof(k_maintained_root_row_3_entries) / sizeof(k_maintained_root_row_3_entries[0]),
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );
    assert(access(fixture.journal_filename, F_OK) == 0);
    assert_maintained_index_root_pages_changed(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_maintained_index_root_fixture_inserted_state(&fixture, row_3_id);
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert_file_missing(fixture.journal_filename);
    assert_maintained_index_root_pages_match(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_maintained_index_root_fixture_initial_state(&fixture);

    assert(mylite_storage_begin_statement(fixture.filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            fixture.filename,
            "app",
            "posts",
            fixture.row_2_id,
            k_maintained_root_updated_row_2,
            sizeof(k_maintained_root_updated_row_2),
            k_maintained_root_updated_row_2_entries,
            sizeof(k_maintained_root_updated_row_2_entries) /
                sizeof(k_maintained_root_updated_row_2_entries[0]),
            &updated_row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(access(fixture.journal_filename, F_OK) == 0);
    assert_maintained_index_root_pages_changed(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_index_root(fixture.filename, "app", "posts", 0U, fixture.primary_root_page, 2ULL);
    assert_index_root(fixture.filename, "app", "posts", 1U, fixture.secondary_root_page, 2ULL);
    assert_index_entry_lookup(
        fixture.filename,
        0U,
        k_maintained_root_key_2,
        sizeof(k_maintained_root_key_2),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_find_indexed_row_equals(
        fixture.filename,
        0U,
        k_maintained_root_updated_key_2,
        sizeof(k_maintained_root_updated_key_2),
        updated_row_2_id,
        k_maintained_root_updated_row_2,
        sizeof(k_maintained_root_updated_row_2)
    );
    assert_exact_index_entries(
        fixture.filename,
        1U,
        k_maintained_root_updated_secondary_key_2,
        sizeof(k_maintained_root_updated_secondary_key_2),
        &updated_row_2_id,
        1U
    );
    assert_index_prefix_exists(
        fixture.filename,
        k_maintained_root_updated_key_2,
        sizeof(k_maintained_root_updated_key_2),
        1
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert_file_missing(fixture.journal_filename);
    assert_maintained_index_root_pages_match(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_maintained_index_root_fixture_initial_state(&fixture);

    assert(mylite_storage_begin_statement(fixture.filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_delete_row(fixture.filename, "app", "posts", fixture.row_1_id) ==
        MYLITE_STORAGE_OK
    );
    assert(access(fixture.journal_filename, F_OK) == 0);
    assert_maintained_index_root_pages_changed(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_index_root(fixture.filename, "app", "posts", 0U, fixture.primary_root_page, 1ULL);
    assert_index_root(fixture.filename, "app", "posts", 1U, fixture.secondary_root_page, 1ULL);
    assert_find_indexed_row_not_found(
        fixture.filename,
        0U,
        k_maintained_root_key_1,
        sizeof(k_maintained_root_key_1)
    );
    assert_exact_index_entries(
        fixture.filename,
        1U,
        k_maintained_root_secondary_key_1,
        sizeof(k_maintained_root_secondary_key_1),
        NULL,
        0U
    );
    assert_index_prefix_exists(
        fixture.filename,
        k_maintained_root_key_1,
        sizeof(k_maintained_root_key_1),
        0
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    assert_file_missing(fixture.journal_filename);
    assert_maintained_index_root_pages_match(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_maintained_index_root_fixture_initial_state(&fixture);

    row_3_id = 0ULL;
    updated_row_2_id = 0ULL;
    assert(mylite_storage_begin_transaction(fixture.filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            fixture.filename,
            "app",
            "posts",
            k_maintained_root_row_3,
            sizeof(k_maintained_root_row_3),
            k_maintained_root_row_3_entries,
            sizeof(k_maintained_root_row_3_entries) / sizeof(k_maintained_root_row_3_entries[0]),
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_update_row_with_index_entries(
            fixture.filename,
            "app",
            "posts",
            fixture.row_2_id,
            k_maintained_root_updated_row_2,
            sizeof(k_maintained_root_updated_row_2),
            k_maintained_root_updated_row_2_entries,
            sizeof(k_maintained_root_updated_row_2_entries) /
                sizeof(k_maintained_root_updated_row_2_entries[0]),
            &updated_row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_delete_row(fixture.filename, "app", "posts", fixture.row_1_id) ==
        MYLITE_STORAGE_OK
    );
    assert_maintained_index_root_pages_changed(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_find_indexed_row_not_found(
        fixture.filename,
        0U,
        k_maintained_root_key_1,
        sizeof(k_maintained_root_key_1)
    );
    assert_find_indexed_row_equals(
        fixture.filename,
        0U,
        k_maintained_root_key_3,
        sizeof(k_maintained_root_key_3),
        row_3_id,
        k_maintained_root_row_3,
        sizeof(k_maintained_root_row_3)
    );
    assert_find_indexed_row_equals(
        fixture.filename,
        0U,
        k_maintained_root_updated_key_2,
        sizeof(k_maintained_root_updated_key_2),
        updated_row_2_id,
        k_maintained_root_updated_row_2,
        sizeof(k_maintained_root_updated_row_2)
    );
    assert(mylite_storage_rollback_statement(transaction) == MYLITE_STORAGE_OK);
    assert_file_size_matches_header(fixture.filename);
    assert_maintained_index_root_pages_match(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_maintained_index_root_fixture_initial_state(&fixture);

    row_3_id = 0ULL;
    updated_row_2_id = 0ULL;
    assert(mylite_storage_begin_transaction(fixture.filename, &transaction) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            fixture.filename,
            "app",
            "posts",
            k_maintained_root_row_3,
            sizeof(k_maintained_root_row_3),
            k_maintained_root_row_3_entries,
            sizeof(k_maintained_root_row_3_entries) / sizeof(k_maintained_root_row_3_entries[0]),
            &row_3_id
        ) == MYLITE_STORAGE_OK
    );
    snapshot_maintained_index_root_pages(
        &fixture,
        post_insert_primary_root,
        post_insert_secondary_root
    );
    assert_maintained_index_root_fixture_inserted_state(&fixture, row_3_id);

    assert(mylite_storage_begin_statement(fixture.filename, &savepoint) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_update_row_with_index_entries(
            fixture.filename,
            "app",
            "posts",
            fixture.row_2_id,
            k_maintained_root_updated_row_2,
            sizeof(k_maintained_root_updated_row_2),
            k_maintained_root_updated_row_2_entries,
            sizeof(k_maintained_root_updated_row_2_entries) /
                sizeof(k_maintained_root_updated_row_2_entries[0]),
            &updated_row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_delete_row(fixture.filename, "app", "posts", fixture.row_1_id) ==
        MYLITE_STORAGE_OK
    );
    assert_maintained_index_root_pages_changed(
        &fixture,
        post_insert_primary_root,
        post_insert_secondary_root
    );
    assert(mylite_storage_rollback_statement(savepoint) == MYLITE_STORAGE_OK);
    assert_maintained_index_root_pages_match(
        &fixture,
        post_insert_primary_root,
        post_insert_secondary_root
    );
    assert_maintained_index_root_fixture_inserted_state(&fixture, row_3_id);
    assert(mylite_storage_commit_statement(transaction) == MYLITE_STORAGE_OK);
    assert_file_size_matches_header(fixture.filename);
    assert_maintained_index_root_fixture_inserted_state(&fixture, row_3_id);
    destroy_maintained_index_root_rollback_fixture(&fixture);

    prepare_maintained_index_root_rollback_fixture(
        &fixture,
        "maintained-index-root-statement-journal-extension.mylite"
    );
    const pid_t statement_extension_pid = fork();
    assert(statement_extension_pid >= 0);
    if (statement_extension_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_row_id = 0ULL;
        if (mylite_storage_begin_statement(fixture.filename, &child_statement) !=
            MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_advance_auto_increment(fixture.filename, "app", "posts", 7ULL) !=
            MYLITE_STORAGE_OK) {
            _exit(3);
        }
        if (mylite_storage_preserve_auto_increment_on_rollback(fixture.filename) !=
            MYLITE_STORAGE_OK) {
            _exit(4);
        }
        if (mylite_storage_append_row_with_index_entries(
                fixture.filename,
                "app",
                "posts",
                k_maintained_root_row_3,
                sizeof(k_maintained_root_row_3),
                k_maintained_root_row_3_entries,
                sizeof(k_maintained_root_row_3_entries) /
                    sizeof(k_maintained_root_row_3_entries[0]),
                &child_row_id
            ) != MYLITE_STORAGE_OK) {
            _exit(5);
        }
        _exit(child_row_id == 0ULL ? 6 : 0);
    }
    status = 0;
    assert(waitpid(statement_extension_pid, &status, 0) == statement_extension_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(fixture.journal_filename, F_OK) == 0);
    assert_maintained_index_root_pages_changed(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_find_indexed_row_not_found(
        fixture.filename,
        0U,
        k_maintained_root_key_3,
        sizeof(k_maintained_root_key_3)
    );
    assert_file_missing(fixture.journal_filename);
    assert_maintained_index_root_pages_match(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_maintained_index_root_fixture_initial_state(&fixture);
    assert_auto_increment_value(fixture.filename, 1ULL);
    destroy_maintained_index_root_rollback_fixture(&fixture);

    prepare_maintained_index_root_rollback_fixture(
        &fixture,
        "maintained-index-root-transaction-recovery.mylite"
    );
    const pid_t transaction_pid = fork();
    assert(transaction_pid >= 0);
    if (transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_row_3_id = 0ULL;
        unsigned long long child_updated_row_2_id = 0ULL;
        if (mylite_storage_begin_transaction(fixture.filename, &child_transaction) !=
            MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                fixture.filename,
                "app",
                "posts",
                k_maintained_root_row_3,
                sizeof(k_maintained_root_row_3),
                k_maintained_root_row_3_entries,
                sizeof(k_maintained_root_row_3_entries) /
                    sizeof(k_maintained_root_row_3_entries[0]),
                &child_row_3_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        if (mylite_storage_update_row_with_index_entries(
                fixture.filename,
                "app",
                "posts",
                fixture.row_2_id,
                k_maintained_root_updated_row_2,
                sizeof(k_maintained_root_updated_row_2),
                k_maintained_root_updated_row_2_entries,
                sizeof(k_maintained_root_updated_row_2_entries) /
                    sizeof(k_maintained_root_updated_row_2_entries[0]),
                &child_updated_row_2_id
            ) != MYLITE_STORAGE_OK) {
            _exit(4);
        }
        if (mylite_storage_delete_row(fixture.filename, "app", "posts", fixture.row_1_id) !=
            MYLITE_STORAGE_OK) {
            _exit(5);
        }
        _exit(child_row_3_id == 0ULL || child_updated_row_2_id == 0ULL ? 6 : 0);
    }
    status = 0;
    assert(waitpid(transaction_pid, &status, 0) == transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(fixture.journal_filename);
    assert(access(fixture.transaction_journal_filename, F_OK) == 0);
    assert_maintained_index_root_pages_changed(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_find_indexed_row_not_found(
        fixture.filename,
        0U,
        k_maintained_root_key_3,
        sizeof(k_maintained_root_key_3)
    );
    assert_file_missing(fixture.transaction_journal_filename);
    assert_maintained_index_root_pages_match(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_maintained_index_root_fixture_initial_state(&fixture);
    destroy_maintained_index_root_rollback_fixture(&fixture);

    prepare_maintained_index_root_rollback_fixture(
        &fixture,
        "maintained-index-root-statement-recovery.mylite"
    );
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_row_id = 0ULL;
        if (mylite_storage_begin_statement(fixture.filename, &child_statement) !=
            MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                fixture.filename,
                "app",
                "posts",
                k_maintained_root_row_3,
                sizeof(k_maintained_root_row_3),
                k_maintained_root_row_3_entries,
                sizeof(k_maintained_root_row_3_entries) /
                    sizeof(k_maintained_root_row_3_entries[0]),
                &child_row_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_id == 0ULL ? 4 : 0);
    }
    status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(fixture.journal_filename, F_OK) == 0);
    assert_maintained_index_root_pages_changed(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_find_indexed_row_not_found(
        fixture.filename,
        0U,
        k_maintained_root_key_3,
        sizeof(k_maintained_root_key_3)
    );
    assert_file_missing(fixture.journal_filename);
    assert_maintained_index_root_pages_match(
        &fixture,
        fixture.primary_root,
        fixture.secondary_root
    );
    assert_maintained_index_root_fixture_initial_state(&fixture);
    destroy_maintained_index_root_rollback_fixture(&fixture);
}

static void prepare_maintained_index_root_rollback_fixture(
    maintained_index_root_rollback_fixture *fixture,
    const char *filename_base
) {
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = k_maintained_root_definition,
        .definition_size = sizeof(k_maintained_root_definition),
    };
    mylite_storage_header header = {
        .size = sizeof(header),
    };

    *fixture = (maintained_index_root_rollback_fixture){0};
    fixture->root = make_temp_root();
    fixture->filename = path_join(fixture->root, filename_base);
    fixture->journal_filename = journal_path(fixture->filename);
    fixture->transaction_journal_filename = transaction_journal_path(fixture->filename);

    assert(mylite_storage_create_empty(fixture->filename) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(fixture->filename, &table_definition) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            fixture->filename,
            "app",
            "posts",
            k_maintained_root_row_1,
            sizeof(k_maintained_root_row_1),
            k_maintained_root_row_1_entries,
            sizeof(k_maintained_root_row_1_entries) / sizeof(k_maintained_root_row_1_entries[0]),
            &fixture->row_1_id
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row_with_index_entries(
            fixture->filename,
            "app",
            "posts",
            k_maintained_root_row_2,
            sizeof(k_maintained_root_row_2),
            k_maintained_root_row_2_entries,
            sizeof(k_maintained_root_row_2_entries) / sizeof(k_maintained_root_row_2_entries[0]),
            &fixture->row_2_id
        ) == MYLITE_STORAGE_OK
    );

    assert(
        mylite_storage_rebuild_index_leaf(fixture->filename, "app", "posts", 0U) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(fixture->filename, &header) == MYLITE_STORAGE_OK);
    fixture->primary_root_page = header.page_count - 1ULL;
    assert_index_root(fixture->filename, "app", "posts", 0U, fixture->primary_root_page, 2ULL);
    assert_index_root_page_type(
        fixture->filename,
        fixture->primary_root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );

    assert(
        mylite_storage_rebuild_index_leaf(fixture->filename, "app", "posts", 1U) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(fixture->filename, &header) == MYLITE_STORAGE_OK);
    fixture->secondary_root_page = header.page_count - 1ULL;
    assert_index_root(fixture->filename, "app", "posts", 1U, fixture->secondary_root_page, 2ULL);
    assert_index_root_page_type(
        fixture->filename,
        fixture->secondary_root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );

    snapshot_maintained_index_root_pages(fixture, fixture->primary_root, fixture->secondary_root);
    assert_maintained_index_root_fixture_initial_state(fixture);
}

static void assert_maintained_index_root_fixture_initial_state(
    const maintained_index_root_rollback_fixture *fixture
) {
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    const unsigned long long row_1_ids[] = {fixture->row_1_id};
    const unsigned long long row_2_ids[] = {fixture->row_2_id};

    assert_index_root(fixture->filename, "app", "posts", 0U, fixture->primary_root_page, 2ULL);
    assert_index_root(fixture->filename, "app", "posts", 1U, fixture->secondary_root_page, 2ULL);
    assert_maintained_index_root_pages_match(
        fixture,
        fixture->primary_root,
        fixture->secondary_root
    );
    assert_find_indexed_row_equals(
        fixture->filename,
        0U,
        k_maintained_root_key_1,
        sizeof(k_maintained_root_key_1),
        fixture->row_1_id,
        k_maintained_root_row_1,
        sizeof(k_maintained_root_row_1)
    );
    assert_find_indexed_row_equals(
        fixture->filename,
        0U,
        k_maintained_root_key_2,
        sizeof(k_maintained_root_key_2),
        fixture->row_2_id,
        k_maintained_root_row_2,
        sizeof(k_maintained_root_row_2)
    );
    assert_exact_index_entries(
        fixture->filename,
        1U,
        k_maintained_root_secondary_key_1,
        sizeof(k_maintained_root_secondary_key_1),
        row_1_ids,
        sizeof(row_1_ids) / sizeof(row_1_ids[0])
    );
    assert_exact_index_entries(
        fixture->filename,
        1U,
        k_maintained_root_secondary_key_2,
        sizeof(k_maintained_root_secondary_key_2),
        row_2_ids,
        sizeof(row_2_ids) / sizeof(row_2_ids[0])
    );
    assert_exact_index_entries(
        fixture->filename,
        1U,
        k_maintained_root_secondary_key_3,
        sizeof(k_maintained_root_secondary_key_3),
        NULL,
        0U
    );
    assert_index_prefix_exists(
        fixture->filename,
        k_maintained_root_key_1,
        sizeof(k_maintained_root_key_1),
        1
    );
    assert_index_prefix_exists(
        fixture->filename,
        k_maintained_root_key_3,
        sizeof(k_maintained_root_key_3),
        0
    );
    assert_index_prefix_exists(
        fixture->filename,
        k_maintained_root_updated_key_2,
        sizeof(k_maintained_root_updated_key_2),
        0
    );

    assert(
        mylite_storage_read_index_entries(fixture->filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 2U);
    assert(entries.key_bytes == 2U * sizeof(k_maintained_root_key_1));
    assert_index_entry(
        &entries,
        0U,
        fixture->row_1_id,
        k_maintained_root_key_1,
        sizeof(k_maintained_root_key_1)
    );
    assert_index_entry(
        &entries,
        1U,
        fixture->row_2_id,
        k_maintained_root_key_2,
        sizeof(k_maintained_root_key_2)
    );
    mylite_storage_free_index_entryset(&entries);

    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(fixture->filename, "app", "posts", 1U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 2U);
    assert(entries.key_bytes == 2U * sizeof(k_maintained_root_secondary_key_1));
    assert_index_entry(
        &entries,
        0U,
        fixture->row_1_id,
        k_maintained_root_secondary_key_1,
        sizeof(k_maintained_root_secondary_key_1)
    );
    assert_index_entry(
        &entries,
        1U,
        fixture->row_2_id,
        k_maintained_root_secondary_key_2,
        sizeof(k_maintained_root_secondary_key_2)
    );
    mylite_storage_free_index_entryset(&entries);
}

static void assert_maintained_index_root_fixture_inserted_state(
    const maintained_index_root_rollback_fixture *fixture,
    unsigned long long row_3_id
) {
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    const unsigned long long row_3_ids[] = {row_3_id};

    assert_index_root(fixture->filename, "app", "posts", 0U, fixture->primary_root_page, 3ULL);
    assert_index_root(fixture->filename, "app", "posts", 1U, fixture->secondary_root_page, 3ULL);
    assert_find_indexed_row_equals(
        fixture->filename,
        0U,
        k_maintained_root_key_3,
        sizeof(k_maintained_root_key_3),
        row_3_id,
        k_maintained_root_row_3,
        sizeof(k_maintained_root_row_3)
    );
    assert_exact_index_entries(
        fixture->filename,
        1U,
        k_maintained_root_secondary_key_3,
        sizeof(k_maintained_root_secondary_key_3),
        row_3_ids,
        sizeof(row_3_ids) / sizeof(row_3_ids[0])
    );
    assert_index_prefix_exists(
        fixture->filename,
        k_maintained_root_key_3,
        sizeof(k_maintained_root_key_3),
        1
    );

    assert(
        mylite_storage_read_index_entries(fixture->filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 3U);
    assert(entries.key_bytes == 3U * sizeof(k_maintained_root_key_1));
    assert_index_entry(
        &entries,
        0U,
        fixture->row_1_id,
        k_maintained_root_key_1,
        sizeof(k_maintained_root_key_1)
    );
    assert_index_entry(
        &entries,
        1U,
        fixture->row_2_id,
        k_maintained_root_key_2,
        sizeof(k_maintained_root_key_2)
    );
    assert_index_entry(
        &entries,
        2U,
        row_3_id,
        k_maintained_root_key_3,
        sizeof(k_maintained_root_key_3)
    );
    mylite_storage_free_index_entryset(&entries);
}

static void snapshot_maintained_index_root_pages(
    const maintained_index_root_rollback_fixture *fixture,
    unsigned char *primary_root,
    unsigned char *secondary_root
) {
    read_test_page(fixture->filename, fixture->primary_root_page, primary_root);
    read_test_page(fixture->filename, fixture->secondary_root_page, secondary_root);
}

static void assert_maintained_index_root_pages_match(
    const maintained_index_root_rollback_fixture *fixture,
    const unsigned char *expected_primary_root,
    const unsigned char *expected_secondary_root
) {
    unsigned char primary_root[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char secondary_root[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    snapshot_maintained_index_root_pages(fixture, primary_root, secondary_root);
    assert(memcmp(primary_root, expected_primary_root, sizeof(primary_root)) == 0);
    assert(memcmp(secondary_root, expected_secondary_root, sizeof(secondary_root)) == 0);
}

static void assert_maintained_index_root_pages_changed(
    const maintained_index_root_rollback_fixture *fixture,
    const unsigned char *expected_primary_root,
    const unsigned char *expected_secondary_root
) {
    unsigned char primary_root[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char secondary_root[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    snapshot_maintained_index_root_pages(fixture, primary_root, secondary_root);
    assert(memcmp(primary_root, expected_primary_root, sizeof(primary_root)) != 0);
    assert(memcmp(secondary_root, expected_secondary_root, sizeof(secondary_root)) != 0);
}

static void destroy_maintained_index_root_rollback_fixture(
    maintained_index_root_rollback_fixture *fixture
) {
    assert_file_missing(fixture->journal_filename);
    assert_file_missing(fixture->transaction_journal_filename);
    assert(unlink(fixture->filename) == 0);
    assert(rmdir(fixture->root) == 0);
    free(fixture->transaction_journal_filename);
    free(fixture->journal_filename);
    free(fixture->filename);
    free(fixture->root);
    *fixture = (maintained_index_root_rollback_fixture){0};
}

static void test_batched_index_leaf_pages(void) {
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
    const unsigned rebuild_indexes[] = {0U, 1U};
    const unsigned duplicate_indexes[] = {0U, 0U};
    char *root = make_temp_root();
    char *filename = path_join(root, "batched-index-leaf-pages.mylite");
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
            .key = secondary_key,
            .key_size = sizeof(secondary_key),
        },
    };
    unsigned char row_2_update_changed[] = {1U, 0U};
    unsigned long long row_1_id = 0ULL;
    unsigned long long row_2_id = 0ULL;
    unsigned long long row_3_id = 0ULL;
    unsigned long long updated_row_2_id = 0ULL;
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
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

    assert(
        mylite_storage_rebuild_index_leaves(
            filename,
            "app",
            "posts",
            duplicate_indexes,
            sizeof(duplicate_indexes) / sizeof(duplicate_indexes[0])
        ) == MYLITE_STORAGE_MISUSE
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long first_rebuild_root = header.page_count;
    assert(
        mylite_storage_rebuild_index_leaves(
            filename,
            "app",
            "posts",
            rebuild_indexes,
            sizeof(rebuild_indexes) / sizeof(rebuild_indexes[0])
        ) == MYLITE_STORAGE_OK
    );
    assert_index_root(filename, "app", "posts", 0U, first_rebuild_root, 2ULL);
    assert_index_root(filename, "app", "posts", 1U, first_rebuild_root + 1ULL, 2ULL);
    assert_index_root_page_type(
        filename,
        first_rebuild_root,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_root_page_type(
        filename,
        first_rebuild_root + 1ULL,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_OK, row_1_id);
    assert_index_entry_lookup(filename, 0U, key_2, sizeof(key_2), MYLITE_STORAGE_OK, row_2_id);
    const unsigned long long initial_secondary_row_ids[] = {row_1_id, row_2_id};
    assert_exact_index_entries(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        initial_secondary_row_ids,
        sizeof(initial_secondary_row_ids) / sizeof(initial_secondary_row_ids[0])
    );
    assert_index_entry_lookup(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        MYLITE_STORAGE_OK,
        row_1_id
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
    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            row_2_id,
            updated_row_2,
            sizeof(updated_row_2),
            row_2_update_entries,
            sizeof(row_2_update_entries) / sizeof(row_2_update_entries[0]),
            row_2_update_changed,
            &updated_row_2_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_delete_row(filename, "app", "posts", row_1_id) == MYLITE_STORAGE_OK);
    assert_index_entry_lookup(filename, 0U, key_1, sizeof(key_1), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(filename, 0U, key_2, sizeof(key_2), MYLITE_STORAGE_NOTFOUND, 0ULL);
    assert_index_entry_lookup(
        filename,
        0U,
        updated_key_2,
        sizeof(updated_key_2),
        MYLITE_STORAGE_OK,
        updated_row_2_id
    );
    const unsigned long long tail_secondary_row_ids[] = {row_3_id, updated_row_2_id};
    assert_exact_index_entries(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        tail_secondary_row_ids,
        sizeof(tail_secondary_row_ids) / sizeof(tail_secondary_row_ids[0])
    );

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long second_rebuild_root = header.page_count;
    assert(
        mylite_storage_rebuild_index_leaves(
            filename,
            "app",
            "posts",
            rebuild_indexes,
            sizeof(rebuild_indexes) / sizeof(rebuild_indexes[0])
        ) == MYLITE_STORAGE_OK
    );
    assert_index_root(filename, "app", "posts", 0U, second_rebuild_root, 2ULL);
    assert_index_root(filename, "app", "posts", 1U, second_rebuild_root + 1ULL, 2ULL);
    assert_index_root_page_type(
        filename,
        second_rebuild_root,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    assert_index_root_page_type(
        filename,
        second_rebuild_root + 1ULL,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_ROOT
    );
    const unsigned long long rebuilt_secondary_row_ids[] = {row_3_id, updated_row_2_id};
    assert_exact_index_entries(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        rebuilt_secondary_row_ids,
        sizeof(rebuilt_secondary_row_ids) / sizeof(rebuilt_secondary_row_ids[0])
    );
    assert_index_entry_lookup(
        filename,
        1U,
        secondary_key,
        sizeof(secondary_key),
        MYLITE_STORAGE_OK,
        row_3_id
    );
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 1U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == 2U);
    assert_index_entry(&entries, 0U, row_3_id, secondary_key, sizeof(secondary_key));
    assert_index_entry(&entries, 1U, updated_row_2_id, secondary_key, sizeof(secondary_key));
    mylite_storage_free_index_entryset(&entries);

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
    const size_t expected_branch_pages = expected_leaf_pages + 1U;
    const unsigned long long immutable_branch_root_page =
        header.page_count - (unsigned long long)expected_branch_pages;
    assert_index_root(filename, "app", "posts", 0U, immutable_branch_root_page, entry_count);
    assert_index_root_page_type(
        filename,
        immutable_branch_root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );
    assert_index_root_page_type(
        filename,
        immutable_branch_root_page + 1ULL,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_LEAF
    );
    unsigned char branch_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    read_test_page(filename, immutable_branch_root_page, branch_page);
    assert(
        get_test_u64_le(branch_page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        entry_count
    );

    unsigned char first_key[4] = {0};
    unsigned char second_page_key[4] = {0};
    unsigned char last_key[4] = {0};
    unsigned char missing_key[4] = {0};
    unsigned char first_byte_prefix[] = {0x01U};
    unsigned char missing_two_byte_prefix[] = {0xffU, 0xffU};
    unsigned char first_byte_prefix_key_1[4] = {0};
    unsigned char first_byte_prefix_key_257[4] = {0};
    put_test_u32_le(first_key, 0U, 1U);
    put_test_u32_le(second_page_key, 0U, (unsigned)entry_capacity + 1U);
    put_test_u32_le(last_key, 0U, entry_count);
    put_test_u32_le(missing_key, 0U, entry_count + 1U);
    put_test_u32_le(first_byte_prefix_key_1, 0U, 1U);
    put_test_u32_le(first_byte_prefix_key_257, 0U, 257U);
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
    assert_index_prefix_exists_for_index(filename, 0U, first_key, sizeof(first_key), 0ULL, 1);
    assert_index_prefix_exists_for_index(
        filename,
        0U,
        first_byte_prefix,
        sizeof(first_byte_prefix),
        0ULL,
        1
    );
    assert_index_prefix_exists_for_index(
        filename,
        0U,
        first_byte_prefix,
        sizeof(first_byte_prefix),
        row_ids[0],
        1
    );
    assert_index_prefix_exists_for_index(
        filename,
        0U,
        second_page_key,
        sizeof(second_page_key),
        0ULL,
        1
    );
    assert_index_prefix_exists_for_index(filename, 0U, last_key, sizeof(last_key), 0ULL, 1);
    assert_index_prefix_exists_for_index(filename, 0U, missing_key, sizeof(missing_key), 0ULL, 0);
    assert_index_prefix_exists_for_index(
        filename,
        0U,
        missing_two_byte_prefix,
        sizeof(missing_two_byte_prefix),
        0ULL,
        0
    );
    assert_index_prefix_exists_for_index(
        filename,
        0U,
        second_page_key,
        sizeof(second_page_key),
        row_ids[entry_capacity],
        0
    );
    const unsigned char *second_page_prefix_keys[] = {second_page_key};
    const unsigned long long second_page_prefix_row_ids[] = {row_ids[entry_capacity]};
    assert_prefix_index_entries(
        filename,
        0U,
        second_page_key,
        sizeof(second_page_key),
        second_page_prefix_keys,
        sizeof(second_page_key),
        second_page_prefix_row_ids,
        sizeof(second_page_prefix_row_ids) / sizeof(second_page_prefix_row_ids[0])
    );
    const unsigned char *first_byte_prefix_keys[] = {
        first_byte_prefix_key_1,
        first_byte_prefix_key_257,
    };
    const unsigned long long first_byte_prefix_row_ids[] = {
        row_ids[0],
        row_ids[256],
    };
    assert_prefix_index_entries(
        filename,
        0U,
        first_byte_prefix,
        sizeof(first_byte_prefix),
        first_byte_prefix_keys,
        sizeof(first_byte_prefix_key_1),
        first_byte_prefix_row_ids,
        sizeof(first_byte_prefix_row_ids) / sizeof(first_byte_prefix_row_ids[0])
    );
    assert_prefix_index_entries(
        filename,
        0U,
        missing_two_byte_prefix,
        sizeof(missing_two_byte_prefix),
        NULL,
        sizeof(missing_key),
        NULL,
        0U
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
    const unsigned long long before_tail_insert_pages = header.page_count;
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
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_tail_insert_pages + 1ULL);
    read_test_page(filename, immutable_branch_root_page, branch_page);
    assert(
        get_test_u64_le(branch_page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET) ==
        entry_count + 1ULL
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
    assert_index_prefix_exists_for_index(filename, 0U, tail_key, sizeof(tail_key), 0ULL, 1);
    assert_index_prefix_exists_for_index(filename, 0U, tail_key, sizeof(tail_key), row_ids[199], 1);
    assert_index_prefix_exists_for_index(filename, 0U, tail_key, sizeof(tail_key), tail_row_id, 1);
    const unsigned char *tail_prefix_keys[] = {
        tail_key,
        tail_key,
    };
    assert_prefix_index_entries(
        filename,
        0U,
        tail_key,
        sizeof(tail_key),
        tail_prefix_keys,
        sizeof(tail_key),
        tail_expected_row_ids,
        sizeof(tail_expected_row_ids) / sizeof(tail_expected_row_ids[0])
    );

    mylite_storage_index_root_definition stale_leaf_root = {
        .size = sizeof(stale_leaf_root),
        .schema_name = "app",
        .table_name = "posts",
        .index_number = 0U,
        .root_page = immutable_branch_root_page,
        .entry_count = entry_count - 1ULL,
    };
    assert(mylite_storage_store_index_root(filename, &stale_leaf_root) == MYLITE_STORAGE_OK);
    assert_index_root(filename, "app", "posts", 0U, immutable_branch_root_page, entry_count + 1ULL);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_branch_prefix_lookup_uses_root_page(void) {
    enum { entry_count = 1200U };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "branch-prefix-lookup.mylite");
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
    assert(expected_leaf_pages >= 4U);
    const size_t expected_branch_pages = expected_leaf_pages + 1U;
    const unsigned long long immutable_branch_root_page =
        header.page_count - (unsigned long long)expected_branch_pages;
    assert_index_root_page_type(
        filename,
        immutable_branch_root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );

    const unsigned last_page_first_value =
        (unsigned)(((expected_leaf_pages - 1U) * entry_capacity) + 1U);
    assert(last_page_first_value <= entry_count);
    unsigned char last_page_first_key[4] = {0};
    put_test_u32_le(last_page_first_key, 0U, last_page_first_value);

    const unsigned long long corrupt_child_offset = (unsigned long long)(expected_leaf_pages / 2U);
    assert(corrupt_child_offset != 0ULL);
    assert(corrupt_child_offset + 1ULL < (unsigned long long)expected_leaf_pages);
    const unsigned long long corrupt_leaf_page =
        immutable_branch_root_page + 1ULL + corrupt_child_offset;
    assert(corrupt_leaf_page <= (unsigned long long)LONG_MAX / MYLITE_STORAGE_FORMAT_PAGE_SIZE);

    mylite_storage_clear_thread_caches();
    flip_file_byte(
        filename,
        (long)(corrupt_leaf_page * MYLITE_STORAGE_FORMAT_PAGE_SIZE) +
            MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET
    );

    const unsigned char *expected_keys[] = {last_page_first_key};
    const unsigned long long expected_row_ids[] = {row_ids[last_page_first_value - 1U]};
    assert_prefix_index_entries(
        filename,
        0U,
        last_page_first_key,
        sizeof(last_page_first_key),
        expected_keys,
        sizeof(last_page_first_key),
        expected_row_ids,
        sizeof(expected_row_ids) / sizeof(expected_row_ids[0])
    );
    assert_index_prefix_exists_for_index(
        filename,
        0U,
        last_page_first_key,
        sizeof(last_page_first_key),
        0ULL,
        1
    );

    mylite_storage_clear_thread_caches();
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_noncontiguous_branch_leaf_children(void) {
    enum { entry_count = 400U };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "noncontiguous-branch-leaf-children.mylite");
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
        put_test_u32_be(key, 0U, i + 1U);
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
    assert(expected_leaf_pages == 2U);
    const unsigned long long branch_root_page = header.page_count - 3ULL;
    const unsigned long long first_leaf_page = branch_root_page + 1ULL;
    const unsigned long long second_leaf_page = branch_root_page + 2ULL;
    assert_index_root_page_type(
        filename,
        branch_root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );

    unsigned char branch_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char first_leaf[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char second_leaf[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    read_test_page(filename, branch_root_page, branch_page);
    read_test_page(filename, first_leaf_page, first_leaf);
    read_test_page(filename, second_leaf_page, second_leaf);

    put_test_u64_le(first_leaf, MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAGE_ID_OFFSET, second_leaf_page);
    rechecksum_test_index_leaf_page(first_leaf);
    put_test_u64_le(second_leaf, MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAGE_ID_OFFSET, first_leaf_page);
    rechecksum_test_index_leaf_page(second_leaf);
    write_test_page(filename, second_leaf_page, first_leaf);
    write_test_page(filename, first_leaf_page, second_leaf);

    const size_t branch_cell_size = MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_HEADER_SIZE + 4U;
    put_test_u64_le(
        branch_page + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET,
        second_leaf_page
    );
    put_test_u64_le(
        branch_page + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET + branch_cell_size,
        MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET,
        first_leaf_page
    );
    rechecksum_test_index_branch_page(branch_page);
    write_test_page(filename, branch_root_page, branch_page);
    mylite_storage_clear_thread_caches();

    unsigned char first_key[4] = {0};
    unsigned char second_page_key[4] = {0};
    unsigned char last_key[4] = {0};
    put_test_u32_be(first_key, 0U, 1U);
    put_test_u32_be(second_page_key, 0U, (unsigned)entry_capacity + 1U);
    put_test_u32_be(last_key, 0U, entry_count);

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
    const unsigned char *second_page_prefix_keys[] = {second_page_key};
    const unsigned long long second_page_prefix_row_ids[] = {row_ids[entry_capacity]};
    assert_prefix_index_entries(
        filename,
        0U,
        second_page_key,
        sizeof(second_page_key),
        second_page_prefix_keys,
        sizeof(second_page_key),
        second_page_prefix_row_ids,
        sizeof(second_page_prefix_row_ids) / sizeof(second_page_prefix_row_ids[0])
    );

    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == entry_count);
    assert_index_entry(&entries, 0U, row_ids[0], first_key, sizeof(first_key));
    assert_index_entry(
        &entries,
        entry_capacity,
        row_ids[entry_capacity],
        second_page_key,
        sizeof(second_page_key)
    );
    assert_index_entry(
        &entries,
        entry_count - 1U,
        row_ids[entry_count - 1U],
        last_key,
        sizeof(last_key)
    );
    mylite_storage_free_index_entryset(&entries);

    mylite_storage_clear_thread_caches();
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_multi_level_branch_navigation(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    enum {
        entry_count = 1200U,
        max_test_leaf_pages = 16U,
        key_size = 4U,
    };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "multi-level-branch-navigation.mylite");
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
        unsigned char key[key_size] = {0};
        put_test_u32_le(row, 0U, i + 1U);
        put_test_u32_be(key, 0U, i + 1U);
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
        (MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size);
    const size_t expected_leaf_pages = ((entry_count - 1U) / entry_capacity) + 1U;
    assert(expected_leaf_pages >= 4U);
    assert(expected_leaf_pages <= max_test_leaf_pages);
    const unsigned long long branch_root_page =
        header.page_count - (unsigned long long)(expected_leaf_pages + 1U);
    const unsigned long long first_leaf_page = branch_root_page + 1ULL;
    const unsigned long long lower_left_branch_page = header.page_count;
    const unsigned long long lower_right_branch_page = header.page_count + 1ULL;
    assert_index_root_page_type(
        filename,
        branch_root_page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_BRANCH
    );

    unsigned long long leaf_page_ids[max_test_leaf_pages] = {0};
    unsigned long long leaf_max_row_ids[max_test_leaf_pages] = {0};
    unsigned char leaf_max_keys[max_test_leaf_pages * key_size];
    size_t leaf_entry_counts[max_test_leaf_pages] = {0};
    unsigned long long table_id = 0ULL;
    for (size_t i = 0U; i < expected_leaf_pages; ++i) {
        unsigned char leaf_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
        const unsigned long long leaf_page_id = first_leaf_page + (unsigned long long)i;
        read_test_page(filename, leaf_page_id, leaf_page);
        if (i == 0U) {
            table_id = get_test_u64_le(leaf_page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_TABLE_ID_OFFSET);
        }
        assert(
            get_test_u64_le(leaf_page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_TABLE_ID_OFFSET) == table_id
        );
        assert(
            get_test_u32_le(leaf_page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_INDEX_NUMBER_OFFSET) == 0U
        );
        assert(
            get_test_u32_le(leaf_page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_KEY_SIZE_OFFSET) == key_size
        );
        const size_t leaf_entry_count =
            get_test_u32_le(leaf_page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET);
        assert(leaf_entry_count != 0U);
        const size_t cell_size = MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size;
        const unsigned char *last_cell = leaf_page +
                                         MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET +
                                         ((leaf_entry_count - 1U) * cell_size);
        leaf_page_ids[i] = leaf_page_id;
        leaf_entry_counts[i] = leaf_entry_count;
        leaf_max_row_ids[i] =
            get_test_u64_le(last_cell, MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_ROW_ID_OFFSET);
        memcpy(
            leaf_max_keys + (i * key_size),
            last_cell + MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_KEY_OFFSET,
            key_size
        );
    }

    const size_t split_leaf = expected_leaf_pages / 2U;
    assert(split_leaf != 0U);
    assert(split_leaf < expected_leaf_pages);
    unsigned long long left_entry_count = 0ULL;
    unsigned long long right_entry_count = 0ULL;
    for (size_t i = 0U; i < expected_leaf_pages; ++i) {
        if (i < split_leaf) {
            left_entry_count += (unsigned long long)leaf_entry_counts[i];
        } else {
            right_entry_count += (unsigned long long)leaf_entry_counts[i];
        }
    }
    assert(left_entry_count + right_entry_count == entry_count);

    unsigned char lower_left_branch[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    unsigned char lower_right_branch[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    assert(
        mylite_storage_test_encode_index_branch_page_with_entry_count(
            lower_left_branch,
            lower_left_branch_page,
            table_id,
            0U,
            1U,
            key_size,
            left_entry_count,
            leaf_page_ids,
            leaf_max_row_ids,
            leaf_max_keys,
            split_leaf
        ) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_test_encode_index_branch_page_with_entry_count(
            lower_right_branch,
            lower_right_branch_page,
            table_id,
            0U,
            1U,
            key_size,
            right_entry_count,
            leaf_page_ids + split_leaf,
            leaf_max_row_ids + split_leaf,
            leaf_max_keys + (split_leaf * key_size),
            expected_leaf_pages - split_leaf
        ) == MYLITE_STORAGE_OK
    );

    unsigned long long root_child_page_ids[] = {lower_left_branch_page, lower_right_branch_page};
    unsigned long long root_child_max_row_ids[] = {
        leaf_max_row_ids[split_leaf - 1U],
        leaf_max_row_ids[expected_leaf_pages - 1U],
    };
    unsigned char root_child_max_keys[key_size * 2U];
    memcpy(root_child_max_keys, leaf_max_keys + ((split_leaf - 1U) * key_size), key_size);
    memcpy(
        root_child_max_keys + key_size,
        leaf_max_keys + ((expected_leaf_pages - 1U) * key_size),
        key_size
    );
    unsigned char root_branch[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    assert(
        mylite_storage_test_encode_index_branch_page_with_entry_count(
            root_branch,
            branch_root_page,
            table_id,
            0U,
            2U,
            key_size,
            entry_count,
            root_child_page_ids,
            root_child_max_row_ids,
            root_child_max_keys,
            2U
        ) == MYLITE_STORAGE_OK
    );
    write_test_page(filename, lower_left_branch_page, lower_left_branch);
    write_test_page(filename, lower_right_branch_page, lower_right_branch);
    write_test_page(filename, branch_root_page, root_branch);
    write_test_header_page_count_and_free_list_root(
        filename,
        header.page_count + 2ULL,
        header.free_list_root_page
    );
    mylite_storage_clear_thread_caches();

    unsigned char first_key[key_size] = {0};
    unsigned char split_key[key_size] = {0};
    unsigned char last_key[key_size] = {0};
    put_test_u32_be(first_key, 0U, 1U);
    put_test_u32_be(split_key, 0U, (unsigned)(split_leaf * entry_capacity) + 1U);
    put_test_u32_be(last_key, 0U, entry_count);
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
        split_key,
        sizeof(split_key),
        MYLITE_STORAGE_OK,
        row_ids[split_leaf * entry_capacity]
    );
    assert_index_entry_lookup(
        filename,
        0U,
        last_key,
        sizeof(last_key),
        MYLITE_STORAGE_OK,
        row_ids[entry_count - 1U]
    );

    const unsigned char *split_prefix_keys[] = {split_key};
    const unsigned long long split_prefix_row_ids[] = {row_ids[split_leaf * entry_capacity]};
    assert_prefix_index_entries(
        filename,
        0U,
        split_key,
        sizeof(split_key),
        split_prefix_keys,
        sizeof(split_key),
        split_prefix_row_ids,
        sizeof(split_prefix_row_ids) / sizeof(split_prefix_row_ids[0])
    );
    assert_index_prefix_exists_for_index(filename, 0U, split_key, sizeof(split_key), 0ULL, 1);

    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == entry_count);
    assert_index_entry(&entries, 0U, row_ids[0], first_key, sizeof(first_key));
    assert_index_entry(
        &entries,
        split_leaf * entry_capacity,
        row_ids[split_leaf * entry_capacity],
        split_key,
        sizeof(split_key)
    );
    assert_index_entry(
        &entries,
        entry_count - 1U,
        row_ids[entry_count - 1U],
        last_key,
        sizeof(last_key)
    );
    mylite_storage_free_index_entryset(&entries);

    unsigned char appended_row[8] = {0};
    unsigned char appended_key[key_size] = {0};
    unsigned long long appended_row_id = 0ULL;
    put_test_u32_le(appended_row, 0U, entry_count + 1U);
    put_test_u32_be(appended_key, 0U, entry_count + 1U);
    mylite_storage_index_entry appended_index_entry = {
        .size = sizeof(appended_index_entry),
        .index_number = 0U,
        .key = appended_key,
        .key_size = sizeof(appended_key),
    };
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            appended_row,
            sizeof(appended_row),
            &appended_index_entry,
            1U,
            &appended_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert_index_entry_lookup(
        filename,
        0U,
        appended_key,
        sizeof(appended_key),
        MYLITE_STORAGE_OK,
        appended_row_id
    );
    const unsigned char *appended_prefix_keys[] = {appended_key};
    const unsigned long long appended_prefix_row_ids[] = {appended_row_id};
    assert_prefix_index_entries(
        filename,
        0U,
        appended_key,
        sizeof(appended_key),
        appended_prefix_keys,
        sizeof(appended_key),
        appended_prefix_row_ids,
        sizeof(appended_prefix_row_ids) / sizeof(appended_prefix_row_ids[0])
    );

    mylite_storage_clear_thread_caches();
    assert(expected_leaf_pages > 3U);
    flip_file_byte(
        filename,
        (long)(leaf_page_ids[1] * MYLITE_STORAGE_FORMAT_PAGE_SIZE) +
            MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET
    );
    assert_index_entry_lookup(
        filename,
        0U,
        last_key,
        sizeof(last_key),
        MYLITE_STORAGE_OK,
        row_ids[entry_count - 1U]
    );

    mylite_storage_clear_thread_caches();
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_branch_page_full_root_split(void) {
    enum { key_size = 512U };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "branch-page-full-root-split.mylite");
    char *journal_filename = journal_path(filename);
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
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    const size_t leaf_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size);
    const size_t branch_capacity =
        (MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET) /
        (MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_HEADER_SIZE + key_size);
    assert(leaf_capacity >= 2U);
    assert(branch_capacity >= 2U);
    assert(branch_capacity < 32U);
    const size_t entry_count = leaf_capacity * branch_capacity;
    unsigned long long *row_ids = (unsigned long long *)calloc(entry_count + 1U, sizeof(*row_ids));
    assert(row_ids != NULL);

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < entry_count; ++i) {
        unsigned char row[8] = {0};
        unsigned char key[key_size] = {0};
        put_test_u32_le(row, 0U, (unsigned)i + 1U);
        put_test_u32_be(key, key_size - sizeof(uint32_t), (unsigned)i + 1U);
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
                row_ids + i
            ) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long before_rebuild_pages = header.page_count;
    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    const unsigned long long root_page = before_rebuild_pages;
    const unsigned long long before_split_pages = header.page_count;
    assert(header.page_count == before_rebuild_pages + 1ULL + (unsigned long long)branch_capacity);
    assert_index_root(filename, "app", "posts", 0U, root_page, (unsigned long long)entry_count);

    unsigned char root_page_bytes[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    read_test_page(filename, root_page, root_page_bytes);
    assert(get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_LEVEL_OFFSET) == 1U);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        branch_capacity
    );

    unsigned char appended_row[8] = {0};
    unsigned char first_key[key_size] = {0};
    unsigned char appended_key[key_size] = {0};
    put_test_u32_be(first_key, key_size - sizeof(uint32_t), 1U);
    put_test_u32_le(appended_row, 0U, (unsigned)entry_count + 1U);
    put_test_u32_be(appended_key, key_size - sizeof(uint32_t), (unsigned)entry_count + 1U);
    mylite_storage_index_entry appended_index_entry = {
        .size = sizeof(appended_index_entry),
        .index_number = 0U,
        .key = appended_key,
        .key_size = sizeof(appended_key),
    };

    mylite_storage_statement *statement = NULL;
    unsigned long long rolled_back_row_id = 0ULL;
    assert(mylite_storage_begin_statement(filename, &statement) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            appended_row,
            sizeof(appended_row),
            &appended_index_entry,
            1U,
            &rolled_back_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(rolled_back_row_id == before_split_pages);
    assert(access(journal_filename, F_OK) == 0);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_pages + 4ULL);
    assert_index_root(
        filename,
        "app",
        "posts",
        0U,
        root_page,
        (unsigned long long)entry_count + 1ULL
    );
    read_test_page(filename, root_page, root_page_bytes);
    assert(get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_LEVEL_OFFSET) == 2U);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    assert_index_entry_lookup(
        filename,
        0U,
        appended_key,
        sizeof(appended_key),
        MYLITE_STORAGE_OK,
        rolled_back_row_id
    );
    assert(mylite_storage_rollback_statement(statement) == MYLITE_STORAGE_OK);
    statement = NULL;
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, (unsigned long long)entry_count);
    read_test_page(filename, root_page, root_page_bytes);
    assert(get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_LEVEL_OFFSET) == 1U);
    assert_index_entry_lookup(
        filename,
        0U,
        appended_key,
        sizeof(appended_key),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );

    const pid_t statement_pid = fork();
    assert(statement_pid >= 0);
    if (statement_pid == 0) {
        mylite_storage_statement *child_statement = NULL;
        unsigned long long child_row_id = 0ULL;
        if (mylite_storage_begin_statement(filename, &child_statement) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                appended_row,
                sizeof(appended_row),
                &appended_index_entry,
                1U,
                &child_row_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_id == before_split_pages ? 0 : 4);
    }
    int status = 0;
    assert(waitpid(statement_pid, &status, 0) == statement_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(journal_filename, F_OK) == 0);
    assert_index_entry_lookup(
        filename,
        0U,
        appended_key,
        sizeof(appended_key),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, (unsigned long long)entry_count);

    const pid_t transaction_pid = fork();
    assert(transaction_pid >= 0);
    if (transaction_pid == 0) {
        mylite_storage_statement *child_transaction = NULL;
        unsigned long long child_row_id = 0ULL;
        if (mylite_storage_begin_transaction(filename, &child_transaction) != MYLITE_STORAGE_OK) {
            _exit(2);
        }
        if (mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                appended_row,
                sizeof(appended_row),
                &appended_index_entry,
                1U,
                &child_row_id
            ) != MYLITE_STORAGE_OK) {
            _exit(3);
        }
        _exit(child_row_id == before_split_pages ? 0 : 4);
    }
    status = 0;
    assert(waitpid(transaction_pid, &status, 0) == transaction_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert_file_missing(journal_filename);
    assert(access(transaction_journal_filename, F_OK) == 0);
    assert_index_entry_lookup(
        filename,
        0U,
        appended_key,
        sizeof(appended_key),
        MYLITE_STORAGE_NOTFOUND,
        0ULL
    );
    assert_file_missing(transaction_journal_filename);
    assert_file_size_matches_header(filename);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_pages);
    assert_index_root(filename, "app", "posts", 0U, root_page, (unsigned long long)entry_count);

    unsigned long long appended_row_id = 0ULL;
    assert(
        mylite_storage_append_row_with_index_entries(
            filename,
            "app",
            "posts",
            appended_row,
            sizeof(appended_row),
            &appended_index_entry,
            1U,
            &appended_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(appended_row_id == before_split_pages);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == before_split_pages + 4ULL);
    assert_index_root(
        filename,
        "app",
        "posts",
        0U,
        root_page,
        (unsigned long long)entry_count + 1ULL
    );
    read_test_page(filename, root_page, root_page_bytes);
    assert(get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_LEVEL_OFFSET) == 2U);
    assert(
        get_test_u32_le(root_page_bytes, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHILD_COUNT_OFFSET) ==
        2U
    );
    const size_t branch_cell_size = MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_HEADER_SIZE + key_size;
    const unsigned char *left_root_cell =
        root_page_bytes + MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET;
    const unsigned char *right_root_cell = left_root_cell + branch_cell_size;
    assert(
        get_test_u64_le(
            left_root_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == before_split_pages + 2ULL
    );
    assert(
        get_test_u64_le(
            right_root_cell,
            MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CELL_CHILD_PAGE_ID_OFFSET
        ) == before_split_pages + 3ULL
    );
    assert_index_entry_lookup(
        filename,
        0U,
        appended_key,
        sizeof(appended_key),
        MYLITE_STORAGE_OK,
        appended_row_id
    );
    assert_find_indexed_row_equals(
        filename,
        0U,
        appended_key,
        sizeof(appended_key),
        appended_row_id,
        appended_row,
        sizeof(appended_row)
    );
    const unsigned char *expected_prefix_keys[] = {appended_key};
    const unsigned long long expected_prefix_row_ids[] = {appended_row_id};
    assert_prefix_index_entries(
        filename,
        0U,
        appended_key,
        sizeof(appended_key),
        expected_prefix_keys,
        sizeof(appended_key),
        expected_prefix_row_ids,
        sizeof(expected_prefix_row_ids) / sizeof(expected_prefix_row_ids[0])
    );
    assert_index_prefix_exists_for_index(filename, 0U, appended_key, sizeof(appended_key), 0ULL, 1);

    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == entry_count + 1U);
    assert_index_entry(&entries, 0U, row_ids[0], first_key, key_size);
    assert_index_entry(&entries, entry_count, appended_row_id, appended_key, sizeof(appended_key));
    mylite_storage_free_index_entryset(&entries);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(row_ids);
    free(transaction_journal_filename);
    free(journal_filename);
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

static void test_full_index_reads_use_leaf_runs(void) {
    enum { entry_count = 400U };

    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "full-index-leaf-run-reads.mylite");
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
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (unsigned i = 0U; i < entry_count; ++i) {
        unsigned char row[8] = {0};
        unsigned char key[4] = {0};
        const unsigned key_value = entry_count - i;
        put_test_u32_le(row, 0U, key_value);
        put_test_u32_be(key, 0U, key_value);
        mylite_storage_index_entry index_entry = {
            .size = sizeof(index_entry),
            .index_number = 0U,
            .key = key,
            .key_size = sizeof(key),
        };
        unsigned long long row_id = 0ULL;
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
        row_ids[key_value - 1U] = row_id;
    }

    assert(mylite_storage_rebuild_index_leaf(filename, "app", "posts", 0U) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == entry_count);
    for (unsigned i = 0U; i < entry_count; ++i) {
        unsigned char key[4] = {0};
        put_test_u32_be(key, 0U, i + 1U);
        assert_index_entry(&entries, i, row_ids[i], key, sizeof(key));
    }
    mylite_storage_free_index_entryset(&entries);

    unsigned char tail_row[8] = {0};
    unsigned char tail_key[4] = {0};
    unsigned long long tail_row_id = 0ULL;
    put_test_u32_le(tail_row, 0U, entry_count + 1U);
    put_test_u32_be(tail_key, 0U, entry_count + 1U);
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

    unsigned char unchanged_update_row[8] = {0};
    unsigned char unchanged_update_key[4] = {0};
    unsigned char unchanged_index_changed[] = {0U};
    unsigned long long unchanged_update_row_id = 0ULL;
    put_test_u32_le(unchanged_update_row, 0U, 1000U);
    put_test_u32_be(unchanged_update_key, 0U, 10U);
    mylite_storage_index_entry unchanged_update_entry = {
        .size = sizeof(unchanged_update_entry),
        .index_number = 0U,
        .key = unchanged_update_key,
        .key_size = sizeof(unchanged_update_key),
    };
    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            row_ids[9],
            unchanged_update_row,
            sizeof(unchanged_update_row),
            &unchanged_update_entry,
            1U,
            unchanged_index_changed,
            &unchanged_update_row_id
        ) == MYLITE_STORAGE_OK
    );
    row_ids[9] = unchanged_update_row_id;

    unsigned char changed_update_row[8] = {0};
    unsigned char changed_update_key[4] = {0};
    unsigned char changed_index_changed[] = {1U};
    unsigned long long changed_update_row_id = 0ULL;
    put_test_u32_le(changed_update_row, 0U, 2000U);
    put_test_u32_be(changed_update_key, 0U, entry_count + 2U);
    mylite_storage_index_entry changed_update_entry = {
        .size = sizeof(changed_update_entry),
        .index_number = 0U,
        .key = changed_update_key,
        .key_size = sizeof(changed_update_key),
    };
    assert(
        mylite_storage_update_row_with_index_entry_changes(
            filename,
            "app",
            "posts",
            row_ids[19],
            changed_update_row,
            sizeof(changed_update_row),
            &changed_update_entry,
            1U,
            changed_index_changed,
            &changed_update_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_delete_row(filename, "app", "posts", row_ids[29]) == MYLITE_STORAGE_OK);

    entries = (mylite_storage_index_entryset){
        .size = sizeof(entries),
    };
    assert(
        mylite_storage_read_index_entries(filename, "app", "posts", 0U, &entries) ==
        MYLITE_STORAGE_OK
    );
    assert(entries.entry_count == entry_count);

    size_t entry_index = 0U;
    for (unsigned key_value = 1U; key_value <= entry_count; ++key_value) {
        if (key_value == 20U || key_value == 30U) {
            continue;
        }
        unsigned char key[4] = {0};
        put_test_u32_be(key, 0U, key_value);
        assert_index_entry(&entries, entry_index, row_ids[key_value - 1U], key, sizeof(key));
        ++entry_index;
    }
    assert_index_entry(&entries, entry_index, tail_row_id, tail_key, sizeof(tail_key));
    ++entry_index;
    assert_index_entry(
        &entries,
        entry_index,
        changed_update_row_id,
        changed_update_key,
        sizeof(changed_update_key)
    );
    ++entry_index;
    assert(entry_index == entries.entry_count);
    mylite_storage_free_index_entryset(&entries);

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

static void assert_index_root_page_type(
    const char *filename,
    unsigned long long root_page,
    unsigned expected_page_type
) {
    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    read_test_page(filename, root_page, page);
    assert(
        get_test_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET) == expected_page_type
    );
}

static void assert_free_list_run(
    const char *filename,
    unsigned long long page_id,
    unsigned long long expected_next_root_page,
    unsigned long long expected_run_start_page,
    unsigned long long expected_run_page_count
) {
    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    read_test_page(filename, page_id, page);
    assert(
        get_test_u32_le(page, MYLITE_STORAGE_FORMAT_FREE_LIST_PAGE_TYPE_OFFSET) ==
        MYLITE_STORAGE_FORMAT_FREE_LIST_PAGE_TYPE_FREE_RUN
    );
    assert(get_test_u64_le(page, MYLITE_STORAGE_FORMAT_FREE_LIST_PAGE_ID_OFFSET) == page_id);
    assert(
        get_test_u64_le(page, MYLITE_STORAGE_FORMAT_FREE_LIST_NEXT_ROOT_PAGE_OFFSET) ==
        expected_next_root_page
    );
    assert(
        get_test_u64_le(page, MYLITE_STORAGE_FORMAT_FREE_LIST_RUN_START_PAGE_OFFSET) ==
        expected_run_start_page
    );
    assert(
        get_test_u64_le(page, MYLITE_STORAGE_FORMAT_FREE_LIST_RUN_PAGE_COUNT_OFFSET) ==
        expected_run_page_count
    );
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
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 5ULL);

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
    assert(mylite_storage_preserve_auto_increment_on_rollback(ctx->filename) == MYLITE_STORAGE_OK);
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
        ) == MYLITE_STORAGE_OK
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

static void test_active_read_statement_context_detection(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "active-read-context.mylite");
    char *other_filename = path_join(root, "other-active-read-context.mylite");
    int owner = 0;
    int other_owner = 0;
    mylite_storage_statement *read_statement = NULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_create_empty(other_filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_context_has_active_read_statement(filename) == 0);

    mylite_storage_set_context_owner(&owner);
    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(mylite_storage_context_has_active_read_statement(filename) == 1);
    assert(mylite_storage_context_has_active_read_statement(other_filename) == 0);

    mylite_storage_set_context_owner(&other_owner);
    assert(mylite_storage_context_has_active_read_statement(filename) == 0);

    mylite_storage_set_context_owner(&owner);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_context_has_active_read_statement(filename) == 0);
    mylite_storage_set_context_owner(NULL);

    assert(unlink(other_filename) == 0);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(other_filename);
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

static void test_read_statement_storage_reuse(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'p', 'o', 's', 't'};
    char *root = make_temp_root();
    char *filename = path_join(root, "read-statement-reuse.mylite");
    mylite_storage_statement *first_statement = NULL;
    mylite_storage_statement *second_statement = NULL;
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;
    mylite_storage_table_definition posts = {
        .size = sizeof(posts),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_read_statement(filename, &first_statement) == MYLITE_STORAGE_OK);
    assert(first_statement != NULL);
    assert(mylite_storage_test_reusable_read_statement_cached() == 0);
    assert(mylite_storage_end_read_statement(first_statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_test_reusable_read_statement_cached() == 1);

    assert(mylite_storage_store_table_definition(filename, &posts) == MYLITE_STORAGE_OK);
    assert(mylite_storage_begin_read_statement(filename, &second_statement) == MYLITE_STORAGE_OK);
    assert(second_statement == first_statement);
    assert(mylite_storage_test_reusable_read_statement_cached() == 0);
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
    assert(mylite_storage_end_read_statement(second_statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_test_reusable_read_statement_cached() == 1);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_read_statement_storage_reuse_replaces_filename(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char first_definition[] = {0x01U, 'f', 'i', 'r', 's', 't'};
    static const unsigned char second_definition[] = {0x01U, 's', 'e', 'c', 'o', 'n', 'd'};
    char *root = make_temp_root();
    char *first_filename = path_join(root, "read-statement-reuse-first.mylite");
    char *second_filename = path_join(root, "read-statement-reuse-second.mylite");
    mylite_storage_statement *first_statement = NULL;
    mylite_storage_statement *second_statement = NULL;
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;
    mylite_storage_table_definition first_table = {
        .size = sizeof(first_table),
        .schema_name = "app",
        .table_name = "first_table",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = first_definition,
        .definition_size = sizeof(first_definition),
    };
    mylite_storage_table_definition second_table = {
        .size = sizeof(second_table),
        .schema_name = "app",
        .table_name = "second_table",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = second_definition,
        .definition_size = sizeof(second_definition),
    };

    assert(mylite_storage_create_empty(first_filename) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(first_filename, &first_table) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_create_empty(second_filename) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(second_filename, &second_table) == MYLITE_STORAGE_OK
    );

    assert(
        mylite_storage_begin_read_statement(first_filename, &first_statement) == MYLITE_STORAGE_OK
    );
    assert(first_statement != NULL);
    assert(mylite_storage_end_read_statement(first_statement) == MYLITE_STORAGE_OK);
    assert(mylite_storage_test_reusable_read_statement_cached() == 1);

    assert(
        mylite_storage_begin_read_statement(second_filename, &second_statement) == MYLITE_STORAGE_OK
    );
    assert(second_statement == first_statement);
    assert(
        mylite_storage_read_table_definition(
            second_filename,
            "app",
            "second_table",
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == sizeof(second_definition));
    assert(memcmp(stored_definition, second_definition, sizeof(second_definition)) == 0);
    mylite_storage_free(stored_definition);
    assert(mylite_storage_end_read_statement(second_statement) == MYLITE_STORAGE_OK);

    assert(unlink(first_filename) == 0);
    assert(unlink(second_filename) == 0);
    assert(rmdir(root) == 0);
    free(second_filename);
    free(first_filename);
    free(root);
#endif
}

static void test_read_statement_filename_identity_borrows_filename(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    char *root = make_temp_root();
    char *filename = path_join(root, "read-statement-filename-borrow.mylite");
    mylite_storage_statement *read_statement = NULL;
    mylite_storage_filename_identity_scope filename_scope = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);

    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(mylite_storage_test_statement_owns_filename(read_statement) == 1);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    read_statement = NULL;

    mylite_storage_begin_filename_identity_scope(filename, &filename_scope);
    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(mylite_storage_test_statement_owns_filename(read_statement) == 0);
    assert(mylite_storage_test_statement_filename_is(read_statement, filename) == 1);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    mylite_storage_end_filename_identity_scope(&filename_scope);
    read_statement = NULL;

    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(mylite_storage_test_statement_owns_filename(read_statement) == 1);
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_read_statement_index_entry_uses_table_entry_cache(void) {
    assert_read_statement_index_lookup_uses_table_entry_cache(
        0,
        "read-statement-index-entry-cache.mylite"
    );
}

static void test_read_statement_indexed_row_uses_table_entry_cache(void) {
    assert_read_statement_index_lookup_uses_table_entry_cache(
        1,
        "read-statement-indexed-row-cache.mylite"
    );
}

static void assert_read_statement_index_lookup_uses_table_entry_cache(
    int fetch_payload,
    const char *filename_basename
) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x11U, 'a'};
    static const unsigned char key[] = {0x11U};
    char *root = make_temp_root();
    char *filename = path_join(root, filename_basename);
    int owner = 0;
    mylite_storage_statement *read_statement = NULL;
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
    unsigned long long row_id = 0ULL;
    unsigned long long found_row_id = 0ULL;
    size_t found_row_size = 0U;
    unsigned char found_row[sizeof(row)] = {0};

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

    mylite_storage_set_context_owner(&owner);
    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(!mylite_storage_test_statement_has_table_entry_cache(read_statement));
    assert(!mylite_storage_test_statement_has_row_payload_cache(read_statement));

    if (fetch_payload) {
        assert(
            mylite_storage_find_indexed_row_into(
                filename,
                "app",
                "posts",
                0U,
                key,
                sizeof(key),
                &found_row_id,
                found_row,
                sizeof(found_row),
                &found_row_size
            ) == MYLITE_STORAGE_OK
        );
    } else {
        assert(
            mylite_storage_find_index_entry(
                filename,
                "app",
                "posts",
                0U,
                key,
                sizeof(key),
                &found_row_id
            ) == MYLITE_STORAGE_OK
        );
    }
    assert(found_row_id == row_id);
    if (fetch_payload) {
        assert(found_row_size == sizeof(row));
        assert(memcmp(found_row, row, sizeof(row)) == 0);
        assert(mylite_storage_test_statement_has_row_payload_cache(read_statement));
    } else {
        assert(!mylite_storage_test_statement_has_row_payload_cache(read_statement));
    }
    assert(mylite_storage_test_statement_has_table_entry_cache(read_statement));

    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    mylite_storage_set_context_owner(NULL);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_read_statement_exact_index_cache_promotes_to_durable(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x11U, 'a'};
    static const unsigned char key[] = {0x11U};
    char *root = make_temp_root();
    char *filename = path_join(root, "read-statement-exact-cache-promotion.mylite");
    int owner = 0;
    mylite_storage_statement *read_statement = NULL;
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
    unsigned long long row_id = 0ULL;
    unsigned long long found_row_id = 0ULL;

    mylite_storage_clear_thread_caches();
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

    mylite_storage_set_context_owner(&owner);
    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 0);
    assert(
        mylite_storage_find_index_entry(
            filename,
            "app",
            "posts",
            0U,
            key,
            sizeof(key),
            &found_row_id
        ) == MYLITE_STORAGE_OK
    );
    assert(found_row_id == row_id);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 0);

    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);
    mylite_storage_set_context_owner(NULL);
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 1);
    mylite_storage_clear_thread_caches();
    assert(mylite_storage_test_durable_exact_index_cache_count(filename) == 0);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
#endif
}

static void test_read_statement_multi_page_catalog_image_cache(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};

    enum { table_count = 96 };

    char *root = make_temp_root();
    char *filename = path_join(root, "read-statement-catalog-image-cache.mylite");
    char table_names[table_count][16] = {{0}};
    unsigned char catalog_root_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    mylite_storage_header header = {
        .size = sizeof(header),
    };
    mylite_storage_statement *read_statement = NULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    for (unsigned i = 0U; i < table_count; ++i) {
        assert(snprintf(table_names[i], sizeof(table_names[i]), "t%03u", i) > 0);
        const mylite_storage_table_definition table_definition = {
            .size = sizeof(table_definition),
            .schema_name = "app",
            .table_name = table_names[i],
            .requested_engine_name = "MYLITE",
            .effective_engine_name = "MYLITE",
            .definition = definition,
            .definition_size = sizeof(definition),
        };
        assert(
            mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK
        );
    }

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    read_test_page(filename, header.catalog_root_page, catalog_root_page);
    const unsigned long long next_page =
        get_test_u64_le(catalog_root_page, MYLITE_STORAGE_FORMAT_CATALOG_NEXT_PAGE_OFFSET);
    assert(next_page == header.catalog_root_page + 1ULL);
    assert(mylite_storage_begin_read_statement(filename, &read_statement) == MYLITE_STORAGE_OK);
    assert(read_statement != NULL);

    flip_file_byte(
        filename,
        (long)(next_page * MYLITE_STORAGE_FORMAT_PAGE_SIZE) +
            (long)MYLITE_STORAGE_FORMAT_CATALOG_MAGIC_OFFSET
    );
    assert(mylite_storage_table_exists(filename, "app", table_names[0]) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_table_exists(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_end_read_statement(read_statement) == MYLITE_STORAGE_OK);

    assert(
        mylite_storage_table_exists(filename, "app", table_names[table_count - 1U]) ==
        MYLITE_STORAGE_CORRUPT
    );

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
        mylite_storage_set_auto_increment(ctx->filename, "app", "posts", 3ULL) == MYLITE_STORAGE_OK
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
    mylite_storage_header transaction_saved_header = {
        .size = sizeof(transaction_saved_header),
    };
    const unsigned long long statement_page_ids[] = {MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(
        mylite_storage_open_header(ctx->filename, &transaction_saved_header) == MYLITE_STORAGE_OK
    );
    const unsigned long long transaction_page_ids[] = {
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        transaction_saved_header.catalog_root_page,
    };
    read_test_page(ctx->filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, transaction_saved_pages[0]);
    read_test_page(
        ctx->filename,
        transaction_saved_header.catalog_root_page,
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

static void test_recovers_legacy_row_publication_journal(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "legacy-row-recovery.mylite");
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
    write_test_legacy_recovery_journal(filename, page_ids, 1U, saved_pages);

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

static void test_recovers_multi_page_protected_journal(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row_1[] = {0x00U, 0x01U, 'a', 'a', 'a'};
    static const unsigned char row_2[] = {0x00U, 0x01U, 'b', 'b', 'b'};
    static const unsigned char row_3[] = {0x00U, 0x01U, 'c', 'c', 'c'};
    static const unsigned char row_4[] = {0x00U, 0x01U, 'd', 'd', 'd'};
    const unsigned char *rows_to_write[] = {row_1, row_2, row_3, row_4};
    char *root = make_temp_root();
    char *filename = path_join(root, "multi-page-protected-journal.mylite");
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
    unsigned long long page_ids[5U] = {MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    for (size_t i = 0U; i < sizeof(rows_to_write) / sizeof(rows_to_write[0]); ++i) {
        assert(
            mylite_storage_append_row_with_index_entries(
                filename,
                "app",
                "posts",
                rows_to_write[i],
                sizeof(row_1),
                NULL,
                0U,
                page_ids + i + 1U
            ) == MYLITE_STORAGE_OK
        );
    }

    read_test_page(filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, saved_pages[0]);
    for (size_t i = 1U; i < sizeof(page_ids) / sizeof(page_ids[0]); ++i) {
        read_test_page(filename, page_ids[i], saved_pages[i]);
        flip_file_byte(
            filename,
            (long)(page_ids[i] * MYLITE_STORAGE_FORMAT_PAGE_SIZE) +
                (long)MYLITE_STORAGE_FORMAT_ROW_MAGIC_OFFSET
        );
    }
    write_test_recovery_journal(
        filename,
        page_ids,
        sizeof(page_ids) / sizeof(page_ids[0]),
        saved_pages
    );

    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 4U);
    assert(rows.row_size == sizeof(row_1));
    assert(rows.row_bytes == sizeof(row_1) * 4U);
    for (size_t i = 0U; i < 4U; ++i) {
        assert(rows.row_offsets[i] == sizeof(row_1) * i);
        assert(rows.row_sizes[i] == sizeof(row_1));
        assert(rows.row_ids[i] == page_ids[i + 1U]);
    }
    assert(memcmp(rows.rows + (0U * rows.row_size), row_1, sizeof(row_1)) == 0);
    assert(memcmp(rows.rows + (1U * rows.row_size), row_2, sizeof(row_2)) == 0);
    assert(memcmp(rows.rows + (2U * rows.row_size), row_3, sizeof(row_3)) == 0);
    assert(memcmp(rows.rows + (3U * rows.row_size), row_4, sizeof(row_4)) == 0);
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
    mylite_storage_header saved_header = {
        .size = sizeof(saved_header),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &saved_header) == MYLITE_STORAGE_OK);
    const unsigned long long page_ids[] = {
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        saved_header.catalog_root_page,
        saved_header.free_list_root_page,
    };
    read_test_page(filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, saved_pages[0]);
    read_test_page(filename, saved_header.catalog_root_page, saved_pages[1]);
    assert(saved_header.free_list_root_page != 0ULL);
    read_test_page(filename, saved_header.free_list_root_page, saved_pages[2]);
    assert(mylite_storage_drop_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);
    write_test_recovery_journal(filename, page_ids, 3U, saved_pages);

    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert_file_missing(journal_filename);
    assert_file_size_matches_header(filename);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(journal_filename);
    free(filename);
    free(root);
}

static void test_recovers_free_list_root_journal(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "free-list-recovery.mylite");
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
    mylite_storage_header saved_header = {
        .size = sizeof(saved_header),
    };
    mylite_storage_header recovered_header = {
        .size = sizeof(recovered_header),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &saved_header) == MYLITE_STORAGE_OK);
    assert(saved_header.free_list_root_page != 0ULL);

    const unsigned long long page_ids[] = {
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        saved_header.catalog_root_page,
        saved_header.free_list_root_page,
    };
    read_test_page(filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, saved_pages[0]);
    read_test_page(filename, saved_header.catalog_root_page, saved_pages[1]);
    read_test_page(filename, saved_header.free_list_root_page, saved_pages[2]);
    flip_file_byte(
        filename,
        (long)(saved_header.free_list_root_page * MYLITE_STORAGE_FORMAT_PAGE_SIZE) +
            (long)MYLITE_STORAGE_FORMAT_FREE_LIST_MAGIC_OFFSET
    );
    write_test_recovery_journal(filename, page_ids, 3U, saved_pages);

    assert(mylite_storage_open_header(filename, &recovered_header) == MYLITE_STORAGE_OK);
    assert(recovered_header.free_list_root_page == saved_header.free_list_root_page);
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
    mylite_storage_header saved_header = {
        .size = sizeof(saved_header),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &saved_header) == MYLITE_STORAGE_OK);
    const unsigned long long page_ids[] = {
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        saved_header.catalog_root_page,
    };
    read_test_page(filename, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, saved_pages[0]);
    read_test_page(filename, saved_header.catalog_root_page, saved_pages[1]);
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
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 4U) + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET)
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
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 4U) + MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET)
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
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 5U) + MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_OFFSET)
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
    assert(row_id == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 2ULL);

    flip_file_byte(
        filename,
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 5U) + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET)
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
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 4U) +
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
    assert(rows->row_ids[0] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 3ULL);
    assert(rows->row_offsets[1] == row_size);
    assert(rows->row_sizes[1] == row_size);
    assert(rows->row_ids[1] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 5ULL);
}

static void assert_lifecycle_initial_rows(const mylite_storage_rowset *rows) {
    assert(rows->row_count == 3U);
    assert(rows->row_ids[0] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 2ULL);
    assert(rows->row_ids[1] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 3ULL);
    assert(rows->row_ids[2] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 4ULL);
}

static void assert_lifecycle_live_rows(
    const mylite_storage_rowset *rows,
    unsigned long long new_row_id
) {
    static const unsigned char row_3[] = {0x00U, 0x03U, 'x', 'y', 'z'};
    static const unsigned char updated_row_1[] = {0x00U, 0x04U, 'u', 'p', 'd', 'a', 't', 'e', 'd'};

    assert(rows->row_size == 0U);
    assert(rows->row_count == 2U);
    assert(rows->row_ids[0] == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 4ULL);
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

static void assert_find_indexed_row_into_equals(
    const char *filename,
    unsigned index_number,
    const unsigned char *key,
    size_t key_size,
    unsigned long long expected_row_id,
    const unsigned char *expected_row,
    size_t expected_row_size
) {
    unsigned long long row_id = 0ULL;
    unsigned char stored_row[32];
    size_t stored_row_size = 0U;

    assert(expected_row_size <= sizeof(stored_row));
    memset(stored_row, 0xA5, sizeof(stored_row));
    assert(
        mylite_storage_find_indexed_row_into(
            filename,
            "app",
            "posts",
            index_number,
            key,
            key_size,
            &row_id,
            stored_row,
            sizeof(stored_row),
            &stored_row_size
        ) == MYLITE_STORAGE_OK
    );
    assert(row_id == expected_row_id);
    assert(stored_row_size == expected_row_size);
    assert(memcmp(stored_row, expected_row, expected_row_size) == 0);
    if (expected_row_size < sizeof(stored_row)) {
        assert(stored_row[expected_row_size] == 0xA5U);
    }
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

static void write_test_page(
    const char *path,
    unsigned long long page_id,
    const unsigned char *page
) {
    FILE *file = fopen(path, "r+b");
    assert(file != NULL);
    assert(fseek(file, (long)(page_id * MYLITE_STORAGE_FORMAT_PAGE_SIZE), SEEK_SET) == 0);
    assert(
        fwrite(page, 1U, MYLITE_STORAGE_FORMAT_PAGE_SIZE, file) == MYLITE_STORAGE_FORMAT_PAGE_SIZE
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

static void write_test_legacy_recovery_journal(
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

    write_test_journal_header_page_with_format(
        journal_header_page,
        page_ids,
        page_count,
        MYLITE_STORAGE_FORMAT_JOURNAL_LEGACY_VERSION,
        MYLITE_STORAGE_FORMAT_JOURNAL_LEGACY_CHECKSUM_OFFSET
    );
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
    write_test_journal_header_page_with_format(
        page,
        page_ids,
        page_count,
        MYLITE_STORAGE_FORMAT_JOURNAL_VERSION,
        MYLITE_STORAGE_FORMAT_JOURNAL_CHECKSUM_OFFSET
    );
}

static void write_test_journal_header_page_with_format(
    unsigned char *page,
    const unsigned long long *page_ids,
    size_t page_count,
    unsigned version,
    unsigned checksum_offset
) {
    static const unsigned char magic[8] = {'M', 'Y', 'L', 'J', 'N', 'L', '1', '\0'};

    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(page + MYLITE_STORAGE_FORMAT_JOURNAL_MAGIC_OFFSET, magic, sizeof(magic));
    put_test_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_TYPE_ROLLBACK
    );
    put_test_u32_le(page, MYLITE_STORAGE_FORMAT_JOURNAL_PAGE_VERSION_OFFSET, version);
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
    put_test_u64_le(page, checksum_offset, checksum_test_page(page, checksum_offset));
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

static void put_test_u32_be(unsigned char *page, size_t offset, unsigned value) {
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        const unsigned shift = (unsigned)((sizeof(uint32_t) - 1U - i) * CHAR_BIT);
        page[offset + i] = (unsigned char)((value >> shift) & UINT8_MAX);
    }
}

static void put_test_u64_le(unsigned char *page, size_t offset, unsigned long long value) {
    for (size_t i = 0U; i < sizeof(uint64_t); ++i) {
        page[offset + i] = (unsigned char)((value >> (unsigned)(i * CHAR_BIT)) & UINT8_MAX);
    }
}

static unsigned get_test_u32_le(const unsigned char *page, size_t offset) {
    unsigned value = 0U;
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        value |= (unsigned)page[offset + i] << (unsigned)(i * CHAR_BIT);
    }
    return value;
}

static unsigned long long get_test_u64_le(const unsigned char *page, size_t offset) {
    unsigned long long value = 0ULL;
    for (size_t i = 0U; i < sizeof(uint64_t); ++i) {
        value |= (unsigned long long)page[offset + i] << (unsigned)(i * CHAR_BIT);
    }
    return value;
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

static void rechecksum_test_index_root_page(unsigned char *page) {
    put_test_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_ROOT_CHECKSUM_OFFSET, 0ULL);
    put_test_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_ROOT_CHECKSUM_OFFSET,
        checksum_test_page(page, MYLITE_STORAGE_FORMAT_INDEX_ROOT_CHECKSUM_OFFSET)
    );
}

static void rechecksum_test_index_branch_page(unsigned char *page) {
    put_test_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHECKSUM_OFFSET, 0ULL);
    put_test_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHECKSUM_OFFSET,
        checksum_test_page(page, MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHECKSUM_OFFSET)
    );
}

static void rechecksum_test_index_leaf_page(unsigned char *page) {
    put_test_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET, 0ULL);
    put_test_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET,
        checksum_test_page(page, MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET)
    );
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

static void write_test_header_page_count_and_free_list_root(
    const char *path,
    unsigned long long page_count,
    unsigned long long free_list_root_page
) {
    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE] = {0};
    read_test_page(path, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, page);
    put_test_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_PAGE_COUNT_OFFSET, page_count);
    put_test_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_FREE_LIST_ROOT_PAGE_OFFSET,
        free_list_root_page
    );
    put_test_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET, 0ULL);
    put_test_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET,
        checksum_test_page(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET)
    );
    write_test_page(path, MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID, page);
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

static int count_app_table(void *ctx, const char *schema_name, const char *table_name) {
    unsigned *count = (unsigned *)ctx;
    assert(strcmp(schema_name, "app") == 0);
    assert(table_name != NULL && table_name[0] != '\0');
    ++*count;
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

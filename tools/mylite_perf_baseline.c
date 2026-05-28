#include <mylite/mylite.h>
#include <mylite/storage.h>

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BENCHMARK_DEFAULT_MAX_ARGUMENT 1000000ULL
#define BENCHMARK_PROFILE_MAX_ITERATIONS 100000000ULL
#define BENCHMARK_STORAGE_COUNTER_SLOT_LIMIT 32U

#ifdef MYLITE_STORAGE_TEST_HOOKS
void mylite_storage_test_reset_branch_leaf_range_plan_read_count(void);
unsigned long long mylite_storage_test_branch_leaf_range_plan_read_count(void);
void mylite_storage_test_reset_branch_refold_root_read_count(void);
unsigned long long mylite_storage_test_branch_refold_root_read_count(void);
void mylite_storage_test_reset_branch_refold_entryset_read_count(void);
unsigned long long mylite_storage_test_branch_refold_entryset_read_count(void);
unsigned long long mylite_storage_test_branch_refold_entryset_cache_hit_count(void);
void mylite_storage_test_reset_level_two_branch_leaf_plan_read_count(void);
unsigned long long mylite_storage_test_level_two_branch_leaf_plan_read_count(void);
void mylite_storage_test_reset_active_branch_page_plan_read_count(void);
unsigned long long mylite_storage_test_active_branch_page_plan_read_count(void);
void mylite_storage_test_reset_packed_index_tail_append_scan_page_count(void);
unsigned long long mylite_storage_test_packed_index_tail_append_scan_page_count(void);
unsigned long long mylite_storage_test_packed_index_tail_append_scan_row_page_count(void);
unsigned long long mylite_storage_test_packed_index_tail_append_scan_other_index_page_count(void);
unsigned long long mylite_storage_test_packed_index_tail_append_scan_same_index_page_count(void);
unsigned long long mylite_storage_test_packed_index_tail_append_scan_missing_page_count(void);
unsigned long long mylite_storage_test_packed_index_tail_append_scan_invalid_page_count(void);
void mylite_storage_test_reset_branch_insert_writer_decode_counts(void);
unsigned long long mylite_storage_test_branch_insert_writer_branch_decode_count(void);
unsigned long long mylite_storage_test_branch_insert_writer_leaf_decode_count(void);
void mylite_storage_test_reset_branch_tail_overlay_scan_counts(void);
unsigned long long mylite_storage_test_branch_tail_overlay_scan_count(void);
unsigned long long mylite_storage_test_branch_tail_overlay_scan_read_count(void);
unsigned long long mylite_storage_test_branch_tail_overlay_scan_index_entry_page_count(void);
unsigned long long mylite_storage_test_branch_tail_overlay_scan_row_state_page_count(void);
unsigned long long mylite_storage_test_branch_tail_overlay_scan_row_page_skip_count(void);
unsigned long long mylite_storage_test_branch_tail_overlay_scan_index_structure_skip_count(void);
unsigned long long mylite_storage_test_branch_tail_overlay_scan_other_skip_count(void);
unsigned long long mylite_storage_test_branch_tail_overlay_scan_overlay_hit_count(void);
void mylite_storage_test_reset_prepared_insert_profile_counts(void);
unsigned long long mylite_storage_test_checksum_page_count(void);
unsigned long long mylite_storage_test_checksum_page_zero_tail_count(void);
size_t mylite_storage_test_checksum_page_family_slot_count(void);
const char *mylite_storage_test_checksum_page_family_slot_name(size_t slot);
unsigned long long mylite_storage_test_checksum_page_family_count(size_t slot);
unsigned long long mylite_storage_test_checksum_page_zero_tail_family_count(size_t slot);
unsigned long long mylite_storage_test_dirty_checksum_refresh_family_count(size_t slot);
size_t mylite_storage_test_dirty_checksum_refresh_source_slot_count(void);
const char *mylite_storage_test_dirty_checksum_refresh_source_slot_name(size_t slot);
unsigned long long mylite_storage_test_dirty_checksum_refresh_source_count(size_t slot);
unsigned long long mylite_storage_test_raw_index_entry_order_build_count(void);
unsigned long long mylite_storage_test_raw_index_entry_order_probe_count(void);
void mylite_storage_test_reset_prepared_update_storage_counts(void);
unsigned long long mylite_storage_test_indexed_row_file_read_count(void);
unsigned long long mylite_storage_test_indexed_row_statement_read_count(void);
unsigned long long mylite_storage_test_preserving_index_update_file_count(void);
unsigned long long mylite_storage_test_preserving_index_update_statement_count(void);
unsigned long long mylite_storage_test_changed_index_update_file_count(void);
unsigned long long mylite_storage_test_changed_index_update_statement_count(void);
unsigned long long mylite_storage_test_update_maintained_root_plan_count(void);
unsigned long long mylite_storage_test_update_maintained_root_update_count(void);
unsigned long long mylite_storage_test_update_maintained_root_retarget_count(void);
unsigned long long mylite_storage_test_update_maintained_root_no_plan_cache_hit_count(void);
unsigned long long mylite_storage_test_update_maintained_root_no_plan_cache_store_count(void);
unsigned long long mylite_storage_test_update_active_rewrite_attempt_count(void);
unsigned long long mylite_storage_test_update_active_rewrite_success_count(void);
unsigned long long mylite_storage_test_update_active_row_only_rewrite_count(void);
unsigned long long mylite_storage_test_update_active_single_index_rewrite_count(void);
unsigned long long mylite_storage_test_update_active_rewrite_maintained_root_skip_count(void);
unsigned long long mylite_storage_test_update_inline_write_count(void);
unsigned long long mylite_storage_test_update_append_write_count(void);
#endif

#ifdef MYLITE_STORAGE_TEST_HOOKS
typedef struct prepared_insert_checksum_snapshot {
    unsigned long long full_page_count;
    unsigned long long zero_tail_count;
    size_t source_count;
    unsigned long long dirty_refresh_source_counts[BENCHMARK_STORAGE_COUNTER_SLOT_LIMIT];
} prepared_insert_checksum_snapshot;
#endif

typedef enum benchmark_phase {
    BENCHMARK_PHASE_ALL,
    BENCHMARK_PHASE_PREPARED_SCALAR_SELECTS,
    BENCHMARK_PHASE_PREPARED_INSERT_COMPONENTS,
    BENCHMARK_PHASE_POINT_SELECTS,
    BENCHMARK_PHASE_DIRECT_PK_SELECTS,
    BENCHMARK_PHASE_PREPARED_PK_SELECTS,
    BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS,
    BENCHMARK_PHASE_PREPARED_PK_SELECT_MISS_COMPONENTS,
    BENCHMARK_PHASE_PREPARED_TEXT_SELECT_COMPONENTS,
    BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW,
    BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS,
    BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ,
    BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS,
    BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ,
    BENCHMARK_PHASE_STORAGE_READ_STATEMENTS,
    BENCHMARK_PHASE_STORAGE_ROW_UPDATES,
    BENCHMARK_PHASE_STORAGE_ROW_UPDATE_COMPONENTS,
    BENCHMARK_PHASE_STORAGE_INDEXED_ROW_UPDATE_COMPONENTS,
    BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS,
    BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS,
    BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS,
    BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS,
    BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
    BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
    BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
    BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
    BENCHMARK_PHASE_UPDATES,
    BENCHMARK_PHASE_DIRECT_UPDATES,
    BENCHMARK_PHASE_PREPARED_UPDATES,
    BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS,
    BENCHMARK_PHASE_PREPARED_ASSIGNMENT_UPDATE_COMPONENTS,
    BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_COMPONENTS,
    BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENTS,
} benchmark_phase;

typedef enum benchmark_metric {
    BENCHMARK_METRIC_OPEN_SETUP,
    BENCHMARK_METRIC_PREPARED_SCALAR_SELECTS,
    BENCHMARK_METRIC_DIRECT_INSERTS,
    BENCHMARK_METRIC_PREPARED_INSERTS,
    BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_STEP,
    BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_RESET,
    BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_COMMIT,
    BENCHMARK_METRIC_DIRECT_PK_SELECTS,
    BENCHMARK_METRIC_PREPARED_PK_SELECTS,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_ROW,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_DONE,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_RESET,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_MISS_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_MISS_COMPONENT_STEP,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_MISS_COMPONENT_RESET,
    BENCHMARK_METRIC_PREPARE_TEXT_SELECT_ROWS,
    BENCHMARK_METRIC_WARM_TEXT_SELECT_CACHE,
    BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_ROW,
    BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_DONE,
    BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_RESET,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_RESET_AFTER_ROW,
    BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS,
    BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ,
    BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS,
    BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS_ONE_READ,
    BENCHMARK_METRIC_STORAGE_READ_STATEMENTS,
    BENCHMARK_METRIC_STORAGE_ROW_UPDATES,
    BENCHMARK_METRIC_STORAGE_ROW_UPDATE_COMPONENT_BEGIN,
    BENCHMARK_METRIC_STORAGE_ROW_UPDATE_COMPONENT_MUTATE,
    BENCHMARK_METRIC_STORAGE_ROW_UPDATE_COMPONENT_COMMIT,
    BENCHMARK_METRIC_STORAGE_INDEXED_ROW_UPDATE_COMPONENT_BEGIN,
    BENCHMARK_METRIC_STORAGE_INDEXED_ROW_UPDATE_COMPONENT_MUTATE,
    BENCHMARK_METRIC_STORAGE_INDEXED_ROW_UPDATE_COMPONENT_COMMIT,
    BENCHMARK_METRIC_DIRECT_SECONDARY_SELECTS,
    BENCHMARK_METRIC_PREPARED_SECONDARY_SELECTS,
    BENCHMARK_METRIC_PREPARE_LEAF_ROWS,
    BENCHMARK_METRIC_PUBLISH_LEAF_INDEX,
    BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_SELECTS,
    BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_SELECTS,
    BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
    BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
    BENCHMARK_METRIC_PREPARE_LEAF_TAIL_ROWS,
    BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
    BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
    BENCHMARK_METRIC_DIRECT_UPDATES,
    BENCHMARK_METRIC_PREPARED_UPDATES,
    BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_STEP,
    BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_RESET,
    BENCHMARK_METRIC_PREPARED_ASSIGNMENT_UPDATE_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_ASSIGNMENT_UPDATE_COMPONENT_STEP,
    BENCHMARK_METRIC_PREPARED_ASSIGNMENT_UPDATE_COMPONENT_RESET,
    BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_COMPONENT_STEP,
    BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_COMPONENT_RESET,
    BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENT_STEP,
    BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENT_RESET,
    BENCHMARK_METRIC_ORDERED_SCAN,
    BENCHMARK_METRIC_COUNT,
} benchmark_metric;

typedef enum prepared_row_only_update_mode {
    PREPARED_ROW_ONLY_UPDATE_HIT,
    PREPARED_ROW_ONLY_UPDATE_MISS,
} prepared_row_only_update_mode;

typedef struct benchmark_metric_definition {
    benchmark_metric metric;
    const char *name;
} benchmark_metric_definition;

typedef struct benchmark_config {
    size_t rows;
    size_t iterations;
    benchmark_phase phase;
    double max_us[BENCHMARK_METRIC_COUNT];
} benchmark_config;

typedef struct benchmark_context {
    const benchmark_config *config;
    const char *root;
    char *filename;
    mylite_db *db;
    int published_leaf_secondary_index;
} benchmark_context;

typedef struct scalar_result {
    unsigned long long value;
    int rows;
} scalar_result;

typedef struct scan_result {
    size_t rows;
    uint64_t checksum;
} scan_result;

typedef struct secondary_result {
    size_t rows;
    uint64_t checksum;
} secondary_result;

static int parse_config(int argc, char **argv, benchmark_config *config);
static int parse_positive_size_argument(
    const char *argument,
    unsigned long long max_value,
    const char *description,
    size_t *out_value
);
static int parse_phase_argument(const char *argument, benchmark_config *config);
static int parse_threshold_argument(const char *argument, benchmark_config *config);
static int find_metric_by_name(const char *name, size_t name_size, benchmark_metric *out_metric);
static const char *benchmark_phase_name(benchmark_phase phase);
static const char *benchmark_metric_name(benchmark_metric metric);
static int run_benchmark(const benchmark_config *config);
static void print_usage(const char *program);
static int print_result(
    const benchmark_config *config,
    benchmark_metric metric,
    const char *operation,
    size_t count,
    uint64_t elapsed_ns
);
static int print_database_file_summary(const benchmark_context *ctx);
static int setup_database(benchmark_context *ctx);
static int benchmark_prepared_scalar_selects(benchmark_context *ctx);
static int benchmark_insert_rows(benchmark_context *ctx);
static int benchmark_prepared_insert_rows(benchmark_context *ctx);
static int benchmark_prepared_insert_components(benchmark_context *ctx);
static void reset_prepared_insert_storage_counters(void);
static void print_prepared_insert_storage_counters(void);
#ifdef MYLITE_STORAGE_TEST_HOOKS
static void snapshot_prepared_insert_checksum_counters(prepared_insert_checksum_snapshot *snapshot);
static void print_prepared_insert_checksum_phase_counters(
    const prepared_insert_checksum_snapshot *before_commit,
    const prepared_insert_checksum_snapshot *after_commit,
    const prepared_insert_checksum_snapshot *after_verification
);
#endif
static void reset_prepared_update_storage_counters(void);
static void print_prepared_update_storage_counters(void);
static int benchmark_point_selects(benchmark_context *ctx);
static int benchmark_prepared_point_selects(benchmark_context *ctx);
static int benchmark_prepared_point_select_components(benchmark_context *ctx);
static int benchmark_prepared_point_select_miss_components(benchmark_context *ctx);
static int benchmark_prepared_text_select_components(benchmark_context *ctx);
static int step_prepared_text_select_component_iteration(
    benchmark_context *ctx,
    mylite_stmt *stmt,
    size_t id,
    uint64_t *checksum,
    uint64_t *bind_ns,
    uint64_t *row_ns,
    uint64_t *done_ns,
    uint64_t *reset_ns
);
static int prepare_text_select_rows(benchmark_context *ctx);
static int benchmark_prepared_point_select_reset_after_row(benchmark_context *ctx);
static int benchmark_storage_entry_lookups(benchmark_context *ctx);
static int benchmark_storage_entry_lookups_in_one_read_statement(benchmark_context *ctx);
static int benchmark_storage_entry_lookups_with_scope(
    benchmark_context *ctx,
    int one_read_statement,
    benchmark_metric metric,
    const char *operation
);
static int benchmark_storage_point_lookups(benchmark_context *ctx);
static int benchmark_storage_point_lookups_in_one_read_statement(benchmark_context *ctx);
static int benchmark_storage_point_lookups_with_scope(
    benchmark_context *ctx,
    int one_read_statement,
    benchmark_metric metric,
    const char *operation
);
static int benchmark_storage_read_statements(benchmark_context *ctx);
static int benchmark_storage_row_updates(benchmark_context *ctx);
static int benchmark_storage_row_update_components(benchmark_context *ctx);
static int benchmark_storage_row_update_loop(benchmark_context *ctx, int components);
static int benchmark_storage_indexed_row_update_components(benchmark_context *ctx);
static size_t find_storage_entry_index_for_row_id(
    const mylite_storage_index_entryset *entries,
    unsigned long long row_id
);
static int benchmark_secondary_selects(benchmark_context *ctx);
static int benchmark_prepared_secondary_selects(benchmark_context *ctx);
static int publish_secondary_leaf_index(benchmark_context *ctx);
static int benchmark_leaf_secondary_selects(benchmark_context *ctx);
static int benchmark_prepared_leaf_secondary_selects(benchmark_context *ctx);
static int benchmark_leaf_secondary_range_limit_selects(
    benchmark_context *ctx,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
);
static int benchmark_prepared_leaf_secondary_range_limit_selects(
    benchmark_context *ctx,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
);
static int benchmark_updates(benchmark_context *ctx);
static int benchmark_prepared_updates(benchmark_context *ctx);
static int benchmark_prepared_update_components(benchmark_context *ctx);
static int benchmark_prepared_assignment_update_components(benchmark_context *ctx);
static int benchmark_prepared_row_only_update_components(benchmark_context *ctx);
static int benchmark_prepared_row_only_update_miss_components(benchmark_context *ctx);
static int benchmark_prepared_row_only_update_components_for_mode(
    benchmark_context *ctx,
    prepared_row_only_update_mode mode
);
static int prepare_row_only_update_rows(benchmark_context *ctx);
static int benchmark_ordered_scan(benchmark_context *ctx);
static int benchmark_secondary_selects_for_index(
    benchmark_context *ctx,
    const char *table_name,
    const char *index_name,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
);
static int benchmark_prepared_secondary_selects_for_index(
    benchmark_context *ctx,
    const char *table_name,
    const char *index_name,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
);
static int prepare_secondary_leaf_table(benchmark_context *ctx);
static int append_secondary_leaf_tail_rows(benchmark_context *ctx);
static int verify_secondary_leaf_index_root(benchmark_context *ctx);
static size_t secondary_value_for_row(benchmark_context *ctx, size_t row_number);
static size_t secondary_value_for_iteration(benchmark_context *ctx, size_t iteration);
static size_t secondary_tail_row_count(benchmark_context *ctx);
static uint64_t secondary_expected_checksum(benchmark_context *ctx, size_t value, size_t *out_rows);
static size_t secondary_bucket_count(benchmark_context *ctx);
static int verify_row_count(benchmark_context *ctx, size_t expected);
static int exec_sql(benchmark_context *ctx, const char *sql);
static int query_uint64(benchmark_context *ctx, const char *sql, unsigned long long *out_value);
static int scalar_callback(void *data, int column_count, char **values, char **column_names);
static int secondary_callback(void *data, int column_count, char **values, char **column_names);
static int scan_callback(void *data, int column_count, char **values, char **column_names);
static void report_database_error(benchmark_context *ctx, const char *operation);
static uint64_t monotonic_ns(void);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

static const benchmark_metric_definition k_metric_definitions[] = {
    {BENCHMARK_METRIC_OPEN_SETUP, "open-setup"},
    {BENCHMARK_METRIC_PREPARED_SCALAR_SELECTS, "prepared-scalar-selects"},
    {BENCHMARK_METRIC_DIRECT_INSERTS, "direct-inserts"},
    {BENCHMARK_METRIC_PREPARED_INSERTS, "prepared-inserts"},
    {BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_BIND, "prepared-insert-bind"},
    {BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_STEP, "prepared-insert-step"},
    {BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_RESET, "prepared-insert-reset"},
    {BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_COMMIT, "prepared-insert-commit"},
    {BENCHMARK_METRIC_DIRECT_PK_SELECTS, "direct-pk-selects"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECTS, "prepared-pk-selects"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_BIND, "prepared-pk-select-bind"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_ROW, "prepared-pk-select-row"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_DONE, "prepared-pk-select-done"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_RESET, "prepared-pk-select-reset"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_MISS_COMPONENT_BIND, "prepared-pk-select-miss-bind"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_MISS_COMPONENT_STEP, "prepared-pk-select-miss-step"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_MISS_COMPONENT_RESET, "prepared-pk-select-miss-reset"},
    {BENCHMARK_METRIC_PREPARE_TEXT_SELECT_ROWS, "prepare-text-select-rows"},
    {BENCHMARK_METRIC_WARM_TEXT_SELECT_CACHE, "warm-text-select-cache"},
    {BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_BIND, "prepared-text-select-bind"},
    {BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_ROW, "prepared-text-select-row"},
    {BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_DONE, "prepared-text-select-done"},
    {BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_RESET, "prepared-text-select-reset"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_RESET_AFTER_ROW, "prepared-pk-select-reset-after-row"},
    {BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS, "storage-pk-entry-lookups"},
    {BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ, "storage-pk-entry-lookups-one-read"},
    {BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS, "storage-pk-row-lookups"},
    {BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS_ONE_READ, "storage-pk-row-lookups-one-read"},
    {BENCHMARK_METRIC_STORAGE_READ_STATEMENTS, "storage-read-statements"},
    {BENCHMARK_METRIC_STORAGE_ROW_UPDATES, "storage-row-updates"},
    {BENCHMARK_METRIC_STORAGE_ROW_UPDATE_COMPONENT_BEGIN, "storage-row-update-begin"},
    {BENCHMARK_METRIC_STORAGE_ROW_UPDATE_COMPONENT_MUTATE, "storage-row-update-mutate"},
    {BENCHMARK_METRIC_STORAGE_ROW_UPDATE_COMPONENT_COMMIT, "storage-row-update-commit"},
    {BENCHMARK_METRIC_STORAGE_INDEXED_ROW_UPDATE_COMPONENT_BEGIN,
     "storage-indexed-row-update-begin"},
    {BENCHMARK_METRIC_STORAGE_INDEXED_ROW_UPDATE_COMPONENT_MUTATE,
     "storage-indexed-row-update-mutate"},
    {BENCHMARK_METRIC_STORAGE_INDEXED_ROW_UPDATE_COMPONENT_COMMIT,
     "storage-indexed-row-update-commit"},
    {BENCHMARK_METRIC_DIRECT_SECONDARY_SELECTS, "direct-secondary-selects"},
    {BENCHMARK_METRIC_PREPARED_SECONDARY_SELECTS, "prepared-secondary-selects"},
    {BENCHMARK_METRIC_PREPARE_LEAF_ROWS, "prepare-leaf-rows"},
    {BENCHMARK_METRIC_PUBLISH_LEAF_INDEX, "publish-leaf-index"},
    {BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_SELECTS, "direct-leaf-secondary-selects"},
    {BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_SELECTS, "prepared-leaf-secondary-selects"},
    {BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
     "direct-leaf-secondary-range-limit-selects"},
    {BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
     "prepared-leaf-secondary-range-limit-selects"},
    {BENCHMARK_METRIC_PREPARE_LEAF_TAIL_ROWS, "prepare-leaf-tail-rows"},
    {BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
     "direct-leaf-secondary-tail-range-limit-selects"},
    {BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
     "prepared-leaf-secondary-tail-range-limit-selects"},
    {BENCHMARK_METRIC_DIRECT_UPDATES, "direct-updates"},
    {BENCHMARK_METRIC_PREPARED_UPDATES, "prepared-updates"},
    {BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_BIND, "prepared-update-bind"},
    {BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_STEP, "prepared-update-step"},
    {BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_RESET, "prepared-update-reset"},
    {BENCHMARK_METRIC_PREPARED_ASSIGNMENT_UPDATE_COMPONENT_BIND, "prepared-assignment-update-bind"},
    {BENCHMARK_METRIC_PREPARED_ASSIGNMENT_UPDATE_COMPONENT_STEP, "prepared-assignment-update-step"},
    {BENCHMARK_METRIC_PREPARED_ASSIGNMENT_UPDATE_COMPONENT_RESET,
     "prepared-assignment-update-reset"},
    {BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_COMPONENT_BIND, "prepared-row-only-update-bind"},
    {BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_COMPONENT_STEP, "prepared-row-only-update-step"},
    {BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_COMPONENT_RESET, "prepared-row-only-update-reset"},
    {BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENT_BIND,
     "prepared-row-only-update-miss-bind"},
    {BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENT_STEP,
     "prepared-row-only-update-miss-step"},
    {BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENT_RESET,
     "prepared-row-only-update-miss-reset"},
    {BENCHMARK_METRIC_ORDERED_SCAN, "ordered-scan"},
};

int main(int argc, char **argv) {
    benchmark_config config = {
        .rows = 100U,
        .iterations = 100U,
        .phase = BENCHMARK_PHASE_ALL,
    };

    if (parse_config(argc, argv, &config) != 0) {
        return 2;
    }

    return run_benchmark(&config);
}

static int parse_config(int argc, char **argv, benchmark_config *config) {
    int numeric_argument = 0;
    int iterations_seen = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        if (strncmp(argv[i], "--phase=", 8U) == 0) {
            if (parse_phase_argument(argv[i] + 8U, config) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (strncmp(argv[i], "--max-us=", 9U) == 0) {
            if (parse_threshold_argument(argv[i] + 9U, config) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (strncmp(argv[i], "--profile-iterations=", 21U) == 0) {
            if (iterations_seen) {
                fprintf(stderr, "Iterations were specified more than once\n");
                print_usage(argv[0]);
                return 1;
            }
            if (parse_positive_size_argument(
                    argv[i] + 21U,
                    BENCHMARK_PROFILE_MAX_ITERATIONS,
                    "profile iterations",
                    &config->iterations
                ) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            iterations_seen = 1;
            continue;
        }
        if (strncmp(argv[i], "--", 2U) == 0) {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }

        size_t value = 0U;
        if (parse_positive_size_argument(
                argv[i],
                BENCHMARK_DEFAULT_MAX_ARGUMENT,
                "ordinary rows or iterations",
                &value
            ) != 0) {
            print_usage(argv[0]);
            return 1;
        }

        ++numeric_argument;
        if (numeric_argument == 1) {
            config->rows = value;
        } else if (numeric_argument == 2) {
            if (iterations_seen) {
                fprintf(stderr, "Iterations were specified more than once\n");
                print_usage(argv[0]);
                return 1;
            }
            config->iterations = value;
            iterations_seen = 1;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    return 0;
}

static int parse_positive_size_argument(
    const char *argument,
    unsigned long long max_value,
    const char *description,
    size_t *out_value
) {
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(argument, &end, 10);
    if (errno != 0 || end == argument || *end != '\0' || value == 0U || value > max_value) {
        fprintf(
            stderr,
            "Expected positive %s up to %llu, got: %s\n",
            description,
            max_value,
            argument
        );
        return 1;
    }

    *out_value = (size_t)value;
    return 0;
}

static int parse_phase_argument(const char *argument, benchmark_config *config) {
    if (strcmp(argument, "all") == 0) {
        config->phase = BENCHMARK_PHASE_ALL;
        return 0;
    }
    if (strcmp(argument, "prepared-scalar-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_SCALAR_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-insert-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_INSERT_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "point-selects") == 0) {
        config->phase = BENCHMARK_PHASE_POINT_SELECTS;
        return 0;
    }
    if (strcmp(argument, "direct-pk-selects") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_PK_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-pk-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_PK_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-pk-select-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "prepared-pk-select-miss-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_PK_SELECT_MISS_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "prepared-text-select-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_TEXT_SELECT_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "prepared-pk-select-reset-after-row") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW;
        return 0;
    }
    if (strcmp(argument, "storage-pk-entry-lookups") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS;
        return 0;
    }
    if (strcmp(argument, "storage-pk-entry-lookups-one-read") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ;
        return 0;
    }
    if (strcmp(argument, "storage-pk-row-lookups") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS;
        return 0;
    }
    if (strcmp(argument, "storage-pk-row-lookups-one-read") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ;
        return 0;
    }
    if (strcmp(argument, "storage-read-statements") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_READ_STATEMENTS;
        return 0;
    }
    if (strcmp(argument, "storage-row-updates") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_ROW_UPDATES;
        return 0;
    }
    if (strcmp(argument, "storage-row-update-components") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_ROW_UPDATE_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "storage-indexed-row-update-components") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_INDEXED_ROW_UPDATE_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "direct-secondary-selects") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-secondary-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS;
        return 0;
    }
    if (strcmp(argument, "direct-leaf-secondary-selects") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-leaf-secondary-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS;
        return 0;
    }
    if (strcmp(argument, "direct-leaf-secondary-range-limit-selects") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-leaf-secondary-range-limit-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS;
        return 0;
    }
    if (strcmp(argument, "direct-leaf-secondary-tail-range-limit-selects") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-leaf-secondary-tail-range-limit-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS;
        return 0;
    }
    if (strcmp(argument, "updates") == 0) {
        config->phase = BENCHMARK_PHASE_UPDATES;
        return 0;
    }
    if (strcmp(argument, "direct-updates") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_UPDATES;
        return 0;
    }
    if (strcmp(argument, "prepared-updates") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_UPDATES;
        return 0;
    }
    if (strcmp(argument, "prepared-update-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "prepared-assignment-update-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_ASSIGNMENT_UPDATE_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "prepared-row-only-update-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "prepared-row-only-update-miss-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENTS;
        return 0;
    }

    fprintf(
        stderr,
        "Expected phase `all`, `prepared-scalar-selects`, `prepared-insert-components`, "
        "`point-selects`, `direct-pk-selects`, `prepared-pk-selects`, "
        "`prepared-pk-select-components`, `prepared-pk-select-miss-components`, "
        "`prepared-text-select-components`, `prepared-pk-select-reset-after-row`, "
        "`storage-pk-entry-lookups`, "
        "`storage-pk-entry-lookups-one-read`, `storage-pk-row-lookups`, "
        "`storage-pk-row-lookups-one-read`, `storage-read-statements`, "
        "`storage-row-updates`, `storage-row-update-components`, "
        "`storage-indexed-row-update-components`, "
        "`direct-secondary-selects`, `prepared-secondary-selects`, "
        "`direct-leaf-secondary-selects`, `prepared-leaf-secondary-selects`, "
        "`direct-leaf-secondary-range-limit-selects`, "
        "`prepared-leaf-secondary-range-limit-selects`, "
        "`direct-leaf-secondary-tail-range-limit-selects`, "
        "`prepared-leaf-secondary-tail-range-limit-selects`, "
        "`updates`, `direct-updates`, `prepared-updates`, "
        "`prepared-update-components`, `prepared-assignment-update-components`, "
        "`prepared-row-only-update-components`, or "
        "`prepared-row-only-update-miss-components`, got: %s\n",
        argument
    );
    return 1;
}

static int parse_threshold_argument(const char *argument, benchmark_config *config) {
    const char *separator = strchr(argument, ':');
    if (separator == NULL || separator == argument || separator[1] == '\0') {
        fprintf(stderr, "Expected threshold as <metric>:<max_us>, got: %s\n", argument);
        return 1;
    }

    benchmark_metric metric = BENCHMARK_METRIC_COUNT;
    if (find_metric_by_name(argument, (size_t)(separator - argument), &metric) != 0) {
        fprintf(
            stderr,
            "Unknown benchmark metric in threshold: %.*s\n",
            (int)(separator - argument),
            argument
        );
        return 1;
    }

    char *end = NULL;
    errno = 0;
    const double max_us = strtod(separator + 1, &end);
    if (errno != 0 || end == separator + 1 || *end != '\0' || max_us <= 0.0 || max_us != max_us ||
        max_us > 1000000000.0) {
        fprintf(stderr, "Expected positive threshold microseconds, got: %s\n", separator + 1);
        return 1;
    }

    config->max_us[metric] = max_us;
    return 0;
}

static int find_metric_by_name(const char *name, size_t name_size, benchmark_metric *out_metric) {
    for (size_t i = 0U; i < sizeof(k_metric_definitions) / sizeof(k_metric_definitions[0]); ++i) {
        const char *candidate = k_metric_definitions[i].name;
        if (strlen(candidate) == name_size && strncmp(candidate, name, name_size) == 0) {
            *out_metric = k_metric_definitions[i].metric;
            return 0;
        }
    }
    return 1;
}

static const char *benchmark_phase_name(benchmark_phase phase) {
    switch (phase) {
    case BENCHMARK_PHASE_ALL:
        return "all";
    case BENCHMARK_PHASE_PREPARED_SCALAR_SELECTS:
        return "prepared-scalar-selects";
    case BENCHMARK_PHASE_PREPARED_INSERT_COMPONENTS:
        return "prepared-insert-components";
    case BENCHMARK_PHASE_POINT_SELECTS:
        return "point-selects";
    case BENCHMARK_PHASE_DIRECT_PK_SELECTS:
        return "direct-pk-selects";
    case BENCHMARK_PHASE_PREPARED_PK_SELECTS:
        return "prepared-pk-selects";
    case BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS:
        return "prepared-pk-select-components";
    case BENCHMARK_PHASE_PREPARED_PK_SELECT_MISS_COMPONENTS:
        return "prepared-pk-select-miss-components";
    case BENCHMARK_PHASE_PREPARED_TEXT_SELECT_COMPONENTS:
        return "prepared-text-select-components";
    case BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW:
        return "prepared-pk-select-reset-after-row";
    case BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS:
        return "storage-pk-entry-lookups";
    case BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ:
        return "storage-pk-entry-lookups-one-read";
    case BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS:
        return "storage-pk-row-lookups";
    case BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ:
        return "storage-pk-row-lookups-one-read";
    case BENCHMARK_PHASE_STORAGE_READ_STATEMENTS:
        return "storage-read-statements";
    case BENCHMARK_PHASE_STORAGE_ROW_UPDATES:
        return "storage-row-updates";
    case BENCHMARK_PHASE_STORAGE_ROW_UPDATE_COMPONENTS:
        return "storage-row-update-components";
    case BENCHMARK_PHASE_STORAGE_INDEXED_ROW_UPDATE_COMPONENTS:
        return "storage-indexed-row-update-components";
    case BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS:
        return "direct-secondary-selects";
    case BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS:
        return "prepared-secondary-selects";
    case BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS:
        return "direct-leaf-secondary-selects";
    case BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS:
        return "prepared-leaf-secondary-selects";
    case BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS:
        return "direct-leaf-secondary-range-limit-selects";
    case BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS:
        return "prepared-leaf-secondary-range-limit-selects";
    case BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS:
        return "direct-leaf-secondary-tail-range-limit-selects";
    case BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS:
        return "prepared-leaf-secondary-tail-range-limit-selects";
    case BENCHMARK_PHASE_UPDATES:
        return "updates";
    case BENCHMARK_PHASE_DIRECT_UPDATES:
        return "direct-updates";
    case BENCHMARK_PHASE_PREPARED_UPDATES:
        return "prepared-updates";
    case BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS:
        return "prepared-update-components";
    case BENCHMARK_PHASE_PREPARED_ASSIGNMENT_UPDATE_COMPONENTS:
        return "prepared-assignment-update-components";
    case BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_COMPONENTS:
        return "prepared-row-only-update-components";
    case BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENTS:
        return "prepared-row-only-update-miss-components";
    }
    return "unknown";
}

static int run_benchmark(const benchmark_config *config) {
    benchmark_context ctx = {
        .config = config,
        .root = NULL,
        .filename = NULL,
        .db = NULL,
        .published_leaf_secondary_index = 0,
    };
    int result = 1;
    uint64_t start_ns;
    const int scalar_phase = config->phase == BENCHMARK_PHASE_PREPARED_SCALAR_SELECTS;
    const int point_select_phase =
        config->phase == BENCHMARK_PHASE_POINT_SELECTS ||
        config->phase == BENCHMARK_PHASE_DIRECT_PK_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECT_MISS_COMPONENTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_TEXT_SELECT_COMPONENTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW ||
        config->phase == BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS ||
        config->phase == BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ ||
        config->phase == BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS ||
        config->phase == BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ ||
        config->phase == BENCHMARK_PHASE_STORAGE_READ_STATEMENTS;
    const int secondary_select_phase =
        config->phase == BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS ||
        config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS ||
        config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS ||
        config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS;
    const int storage_update_phase =
        config->phase == BENCHMARK_PHASE_STORAGE_ROW_UPDATES ||
        config->phase == BENCHMARK_PHASE_STORAGE_ROW_UPDATE_COMPONENTS ||
        config->phase == BENCHMARK_PHASE_STORAGE_INDEXED_ROW_UPDATE_COMPONENTS;

    ctx.root = make_temp_root();
    if (ctx.root == NULL) {
        return 1;
    }

    printf("# MyLite Performance Baseline\n\n");
    printf("Rows: %zu\n", config->rows);
    printf("Iterations: %zu\n", config->iterations);
    printf("Phase: %s\n", benchmark_phase_name(config->phase));
    printf("Storage route: `ENGINE=InnoDB` through the MyLite storage engine\n\n");
    printf("| Operation | Count | Total ms | us/op |\n");
    printf("| --- | ---: | ---: | ---: |\n");

    start_ns = monotonic_ns();
    if (setup_database(&ctx) != 0) {
        goto cleanup;
    }
    if (print_result(
            config,
            BENCHMARK_METRIC_OPEN_SETUP,
            "open and schema setup",
            1U,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }

    if (config->phase == BENCHMARK_PHASE_ALL || scalar_phase) {
        if (benchmark_prepared_scalar_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (scalar_phase) {
            result = 0;
            goto cleanup;
        }
    }

    if (benchmark_insert_rows(&ctx) != 0) {
        goto cleanup;
    }
    if (verify_row_count(&ctx, config->rows) != 0) {
        goto cleanup;
    }
    if (config->phase == BENCHMARK_PHASE_PREPARED_INSERT_COMPONENTS) {
        if (benchmark_prepared_insert_components(&ctx) != 0) {
            goto cleanup;
        }
        result = 0;
        goto cleanup;
    }
    if (storage_update_phase) {
        if (config->phase == BENCHMARK_PHASE_STORAGE_ROW_UPDATE_COMPONENTS) {
            if (benchmark_storage_row_update_components(&ctx) != 0) {
                goto cleanup;
            }
        } else if (config->phase == BENCHMARK_PHASE_STORAGE_INDEXED_ROW_UPDATE_COMPONENTS) {
            if (benchmark_storage_indexed_row_update_components(&ctx) != 0) {
                goto cleanup;
            }
        } else if (benchmark_storage_row_updates(&ctx) != 0) {
            goto cleanup;
        }
        if (verify_row_count(&ctx, config->rows) != 0) {
            goto cleanup;
        }
        if (benchmark_ordered_scan(&ctx) != 0) {
            goto cleanup;
        }
        result = 0;
        goto cleanup;
    }
    if (point_select_phase) {
        if (config->phase == BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS) {
            if (benchmark_storage_entry_lookups(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ) {
            if (benchmark_storage_entry_lookups_in_one_read_statement(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS) {
            if (benchmark_storage_point_lookups(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ) {
            if (benchmark_storage_point_lookups_in_one_read_statement(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_STORAGE_READ_STATEMENTS) {
            if (benchmark_storage_read_statements(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_MISS_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_TEXT_SELECT_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW &&
            benchmark_point_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase != BENCHMARK_PHASE_DIRECT_PK_SELECTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_MISS_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_TEXT_SELECT_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW &&
            benchmark_prepared_point_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS &&
            benchmark_prepared_point_select_components(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECT_MISS_COMPONENTS &&
            benchmark_prepared_point_select_miss_components(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_PREPARED_TEXT_SELECT_COMPONENTS &&
            benchmark_prepared_text_select_components(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase != BENCHMARK_PHASE_DIRECT_PK_SELECTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_MISS_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_TEXT_SELECT_COMPONENTS &&
            benchmark_prepared_point_select_reset_after_row(&ctx) != 0) {
            goto cleanup;
        }
        result = 0;
        goto cleanup;
    }
    if (secondary_select_phase) {
        if (config->phase == BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS &&
            benchmark_secondary_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS &&
            benchmark_prepared_secondary_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS ||
            config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS ||
            config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS ||
            config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS ||
            config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS ||
            config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS) {
            if (publish_secondary_leaf_index(&ctx) != 0) {
                goto cleanup;
            }
            if ((config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS ||
                 config->phase ==
                     BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS) &&
                append_secondary_leaf_tail_rows(&ctx) != 0) {
                goto cleanup;
            }
            if (config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS &&
                benchmark_leaf_secondary_selects(&ctx) != 0) {
                goto cleanup;
            }
            if (config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS &&
                benchmark_prepared_leaf_secondary_selects(&ctx) != 0) {
                goto cleanup;
            }
            if (config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS &&
                benchmark_leaf_secondary_range_limit_selects(
                    &ctx,
                    BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
                    "direct published-root secondary range LIMIT selects",
                    "Published secondary range-limit rows",
                    "Published secondary range-limit checksum"
                ) != 0) {
                goto cleanup;
            }
            if (config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS &&
                benchmark_prepared_leaf_secondary_range_limit_selects(
                    &ctx,
                    BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
                    "prepared published-root secondary range LIMIT selects",
                    "Prepared published secondary range-limit rows",
                    "Prepared published secondary range-limit checksum"
                ) != 0) {
                goto cleanup;
            }
            if (config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS &&
                benchmark_leaf_secondary_range_limit_selects(
                    &ctx,
                    BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
                    "direct published-root secondary tail-overlay range LIMIT selects",
                    "Published secondary tail range-limit rows",
                    "Published secondary tail range-limit checksum"
                ) != 0) {
                goto cleanup;
            }
            if (config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS &&
                benchmark_prepared_leaf_secondary_range_limit_selects(
                    &ctx,
                    BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
                    "prepared published-root secondary tail-overlay range LIMIT selects",
                    "Prepared published secondary tail range-limit rows",
                    "Prepared published secondary tail range-limit checksum"
                ) != 0) {
                goto cleanup;
            }
        }
        result = 0;
        goto cleanup;
    }
    if (benchmark_prepared_insert_rows(&ctx) != 0) {
        goto cleanup;
    }
    if (config->phase != BENCHMARK_PHASE_ALL) {
        goto updates;
    }
    if (benchmark_point_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_point_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_point_select_reset_after_row(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_secondary_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_secondary_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (publish_secondary_leaf_index(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_leaf_secondary_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_leaf_secondary_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_leaf_secondary_range_limit_selects(
            &ctx,
            BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
            "direct published-root secondary range LIMIT selects",
            "Published secondary range-limit rows",
            "Published secondary range-limit checksum"
        ) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_leaf_secondary_range_limit_selects(
            &ctx,
            BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_RANGE_LIMIT_SELECTS,
            "prepared published-root secondary range LIMIT selects",
            "Prepared published secondary range-limit rows",
            "Prepared published secondary range-limit checksum"
        ) != 0) {
        goto cleanup;
    }
    if (append_secondary_leaf_tail_rows(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_leaf_secondary_range_limit_selects(
            &ctx,
            BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
            "direct published-root secondary tail-overlay range LIMIT selects",
            "Published secondary tail range-limit rows",
            "Published secondary tail range-limit checksum"
        ) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_leaf_secondary_range_limit_selects(
            &ctx,
            BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_TAIL_RANGE_LIMIT_SELECTS,
            "prepared published-root secondary tail-overlay range LIMIT selects",
            "Prepared published secondary tail range-limit rows",
            "Prepared published secondary tail range-limit checksum"
        ) != 0) {
        goto cleanup;
    }
updates:
    if (config->phase != BENCHMARK_PHASE_PREPARED_UPDATES &&
        config->phase != BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS &&
        config->phase != BENCHMARK_PHASE_PREPARED_ASSIGNMENT_UPDATE_COMPONENTS &&
        config->phase != BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_COMPONENTS &&
        config->phase != BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENTS) {
        if (benchmark_updates(&ctx) != 0) {
            goto cleanup;
        }
    }
    if (config->phase == BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS) {
        if (benchmark_prepared_update_components(&ctx) != 0) {
            goto cleanup;
        }
    } else if (config->phase == BENCHMARK_PHASE_PREPARED_ASSIGNMENT_UPDATE_COMPONENTS) {
        if (benchmark_prepared_assignment_update_components(&ctx) != 0) {
            goto cleanup;
        }
    } else if (config->phase == BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_COMPONENTS) {
        if (benchmark_prepared_row_only_update_components(&ctx) != 0) {
            goto cleanup;
        }
    } else if (config->phase == BENCHMARK_PHASE_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENTS) {
        if (benchmark_prepared_row_only_update_miss_components(&ctx) != 0) {
            goto cleanup;
        }
    } else if (config->phase != BENCHMARK_PHASE_DIRECT_UPDATES) {
        if (benchmark_prepared_updates(&ctx) != 0) {
            goto cleanup;
        }
    }
    if (verify_row_count(&ctx, config->rows) != 0) {
        goto cleanup;
    }
    if (benchmark_ordered_scan(&ctx) != 0) {
        goto cleanup;
    }

    result = 0;

cleanup:
    if (ctx.db != NULL && mylite_close(ctx.db) != MYLITE_OK) {
        fprintf(stderr, "Failed to close database: %s\n", mylite_errmsg(ctx.db));
        result = 1;
    }
    if (result == 0 && print_database_file_summary(&ctx) != 0) {
        result = 1;
    }
    free(ctx.filename);
    if (getenv("MYLITE_PERF_KEEP_ROOT") != NULL) {
        fprintf(stderr, "Keeping benchmark root: %s\n", ctx.root);
    } else {
        remove_tree(ctx.root);
    }
    free((void *)ctx.root);
    return result;
}

static void print_usage(const char *program) {
    fprintf(
        stderr,
        "Usage: %s [--phase=all|prepared-scalar-selects|prepared-insert-components|"
        "point-selects|direct-pk-selects|prepared-pk-selects|prepared-pk-select-components|"
        "prepared-pk-select-miss-components|prepared-text-select-components|"
        "prepared-pk-select-reset-after-row|updates|"
        "direct-updates|"
        "prepared-updates|prepared-update-components|"
        "prepared-assignment-update-components|prepared-row-only-update-components|"
        "prepared-row-only-update-miss-components|"
        "direct-secondary-selects|"
        "prepared-secondary-selects|"
        "direct-leaf-secondary-selects|prepared-leaf-secondary-selects|"
        "direct-leaf-secondary-range-limit-selects|"
        "prepared-leaf-secondary-range-limit-selects|"
        "direct-leaf-secondary-tail-range-limit-selects|"
        "prepared-leaf-secondary-tail-range-limit-selects|"
        "storage-pk-entry-lookups|storage-pk-entry-lookups-one-read|storage-pk-row-lookups|"
        "storage-pk-row-lookups-one-read|storage-read-statements|storage-row-updates|"
        "storage-row-update-components|storage-indexed-row-update-components] "
        "[--max-us=<metric>:<value>] [--profile-iterations=<n>] [rows] [iterations]\n"
        "\n"
        "Defaults: phase=all rows=100 iterations=100.\n"
        "Positional rows and iterations accept values up to 1000000. "
        "--profile-iterations accepts up to 100000000 explicit iterations for local "
        "sampling runs.\n"
        "Focused scalar, point-select, secondary-select, and update phases skip unrelated "
        "timings after setup.\n"
        "Thresholds are opt-in and may be supplied more than once. Metrics: "
        "open-setup, prepared-scalar-selects, direct-inserts, prepared-inserts, "
        "prepared-insert-bind, prepared-insert-step, prepared-insert-reset, "
        "direct-pk-selects, prepared-pk-selects, prepared-pk-select-bind, "
        "prepared-pk-select-row, prepared-pk-select-done, prepared-pk-select-reset, "
        "prepared-pk-select-miss-bind, prepared-pk-select-miss-step, "
        "prepared-pk-select-miss-reset, "
        "prepare-text-select-rows, warm-text-select-cache, "
        "prepared-text-select-bind, prepared-text-select-row, "
        "prepared-text-select-done, prepared-text-select-reset, "
        "prepared-pk-select-reset-after-row, "
        "storage-pk-entry-lookups, storage-pk-entry-lookups-one-read, "
        "storage-pk-row-lookups, storage-pk-row-lookups-one-read, "
        "storage-read-statements, storage-row-updates, storage-row-update-components, "
        "storage-indexed-row-update-begin, storage-indexed-row-update-mutate, "
        "storage-indexed-row-update-commit, "
        "direct-secondary-selects, prepared-secondary-selects, "
        "prepare-leaf-rows, publish-leaf-index, "
        "direct-leaf-secondary-selects, "
        "prepared-leaf-secondary-selects, "
        "direct-leaf-secondary-range-limit-selects, "
        "prepared-leaf-secondary-range-limit-selects, "
        "prepare-leaf-tail-rows, "
        "direct-leaf-secondary-tail-range-limit-selects, "
        "prepared-leaf-secondary-tail-range-limit-selects, "
        "direct-updates, prepared-updates, "
        "prepared-update-bind, prepared-update-step, prepared-update-reset, "
        "prepared-assignment-update-bind, prepared-assignment-update-step, "
        "prepared-assignment-update-reset, prepared-row-only-update-bind, "
        "prepared-row-only-update-step, prepared-row-only-update-reset, "
        "prepared-row-only-update-miss-bind, prepared-row-only-update-miss-step, "
        "prepared-row-only-update-miss-reset, ordered-scan.\n"
        "Set MYLITE_PERF_KEEP_ROOT=1 to keep the temporary benchmark directory.\n",
        program
    );
}

static int print_result(
    const benchmark_config *config,
    benchmark_metric metric,
    const char *operation,
    size_t count,
    uint64_t elapsed_ns
) {
    const double total_ms = (double)elapsed_ns / 1000000.0;
    const double micros_per_operation =
        count == 0U ? 0.0 : (double)elapsed_ns / (double)count / 1000.0;

    printf("| %s | %zu | %.3f | %.3f |\n", operation, count, total_ms, micros_per_operation);
    if (config->max_us[metric] > 0.0 && micros_per_operation > config->max_us[metric]) {
        fprintf(
            stderr,
            "Benchmark metric %s exceeded %.3f us/op: %.3f us/op\n",
            benchmark_metric_name(metric),
            config->max_us[metric],
            micros_per_operation
        );
        return 1;
    }
    return 0;
}

static const char *benchmark_metric_name(benchmark_metric metric) {
    for (size_t i = 0U; i < sizeof(k_metric_definitions) / sizeof(k_metric_definitions[0]); ++i) {
        if (k_metric_definitions[i].metric == metric) {
            return k_metric_definitions[i].name;
        }
    }
    return "unknown";
}

static int print_database_file_summary(const benchmark_context *ctx) {
    if (ctx->filename == NULL) {
        return 0;
    }

    struct stat status;
    if (stat(ctx->filename, &status) != 0) {
        fprintf(
            stderr,
            "Failed to stat benchmark database %s: %s\n",
            ctx->filename,
            strerror(errno)
        );
        return 1;
    }
    if (status.st_size < 0) {
        fprintf(stderr, "Benchmark database %s reported a negative size\n", ctx->filename);
        return 1;
    }

    mylite_storage_header header = {
        .size = sizeof(header),
    };
    const mylite_storage_result header_result = mylite_storage_open_header(ctx->filename, &header);
    if (header_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read benchmark database header: %d\n", header_result);
        return 1;
    }

    const unsigned long long bytes = (unsigned long long)status.st_size;
    printf("\nDatabase file:\n\n");
    printf("| Metric | Value |\n");
    printf("| --- | ---: |\n");
    printf("| final bytes | %llu |\n", bytes);
    printf("| header page size | %u |\n", header.page_size);
    printf("| header page count | %llu |\n", header.page_count);
    return 0;
}

static int setup_database(benchmark_context *ctx) {
    char *runtime_root = path_join(ctx->root, "runtime");
    if (runtime_root == NULL) {
        return 1;
    }
    if (mkdir(runtime_root, 0700) != 0) {
        fprintf(
            stderr,
            "Failed to create runtime directory %s: %s\n",
            runtime_root,
            strerror(errno)
        );
        free(runtime_root);
        return 1;
    }

    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = runtime_root,
    };
    ctx->filename = path_join(ctx->root, "perf.mylite");
    if (ctx->filename == NULL) {
        free(runtime_root);
        return 1;
    }

    const int open_result = mylite_open_v2(
        ctx->filename,
        &ctx->db,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE,
        &config
    );
    free(runtime_root);
    if (open_result != MYLITE_OK) {
        report_database_error(ctx, "open database");
        return 1;
    }

    if (exec_sql(ctx, "CREATE DATABASE perf") != 0 || exec_sql(ctx, "USE perf") != 0) {
        return 1;
    }

    return exec_sql(
        ctx,
        "CREATE TABLE perf_rows ("
        "id INT NOT NULL PRIMARY KEY,"
        "value INT NOT NULL,"
        "pad VARCHAR(64) NOT NULL,"
        "KEY value_key (value)"
        ") ENGINE=InnoDB"
    );
}

static int benchmark_prepared_scalar_selects(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t checksum = 0U;
    int result = 1;

    if (mylite_prepare(
            ctx->db,
            "SELECT CAST(? AS SIGNED) + 1",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare scalar select");
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const unsigned long long input = (unsigned long long)(i % 1000000U);
        const unsigned long long expected = input + 1U;
        if (mylite_bind_int64(stmt, 1U, (long long)input) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared scalar select");
            goto cleanup;
        }

        const int row_result = mylite_step(stmt);
        if (row_result != MYLITE_ROW) {
            fprintf(stderr, "Prepared scalar select returned no row\n");
            report_database_error(ctx, "prepared scalar select");
            goto cleanup;
        }

        unsigned long long value = 0U;
        const mylite_value_type value_type = mylite_column_type(stmt, 0U);
        if (value_type == MYLITE_TYPE_INT64) {
            value = (unsigned long long)mylite_column_int64(stmt, 0U);
        } else if (value_type == MYLITE_TYPE_UINT64) {
            value = mylite_column_uint64(stmt, 0U);
        } else {
            fprintf(stderr, "Prepared scalar select returned a non-integer value\n");
            goto cleanup;
        }
        if (value != expected) {
            fprintf(
                stderr,
                "Prepared scalar select returned %llu; expected %llu\n",
                value,
                expected
            );
            goto cleanup;
        }
        checksum += value;

        const int done_result = mylite_step(stmt);
        if (done_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared scalar select returned extra rows\n");
            report_database_error(ctx, "prepared scalar select completion");
            goto cleanup;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared scalar select");
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_SCALAR_SELECTS,
            "prepared scalar selects",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared scalar checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared scalar select");
        return 1;
    }
    return result;
}

static int benchmark_insert_rows(benchmark_context *ctx) {
    uint64_t start_ns;
    int result = 1;

    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }

    start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        char sql[160];
        const int written = snprintf(
            sql,
            sizeof(sql),
            "INSERT INTO perf_rows (id, value, pad) VALUES (%zu, %zu, 'row-%zu')",
            i + 1U,
            secondary_value_for_row(ctx, i + 1U),
            i + 1U
        );
        if (written < 0 || (size_t)written >= sizeof(sql) || exec_sql(ctx, sql) != 0) {
            goto rollback;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_DIRECT_INSERTS,
            "direct inserts in one transaction",
            ctx->config->rows,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_prepared_insert_rows(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    int result = 1;

    if (exec_sql(
            ctx,
            "CREATE TABLE perf_prepared_rows ("
            "id INT NOT NULL PRIMARY KEY,"
            "value INT NOT NULL,"
            "pad VARCHAR(64) NOT NULL,"
            "KEY value_key (value)"
            ") ENGINE=InnoDB"
        ) != 0) {
        return 1;
    }
    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "INSERT INTO perf_prepared_rows (id, value, pad) VALUES (?, ?, ?)",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare row insert");
        goto rollback;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        char pad[32];
        const size_t row_id = i + 1U;
        const int written = snprintf(pad, sizeof(pad), "row-%zu", row_id);
        if (written < 0 || (size_t)written >= sizeof(pad)) {
            goto rollback;
        }
        if (mylite_bind_int64(stmt, 1U, (long long)row_id) != MYLITE_OK ||
            mylite_bind_int64(stmt, 2U, (long long)secondary_value_for_row(ctx, row_id)) !=
                MYLITE_OK ||
            mylite_bind_text(stmt, 3U, pad, MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared row insert");
            goto rollback;
        }

        const int step_result = mylite_step(stmt);
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared row insert failed for id %zu\n", row_id);
            report_database_error(ctx, "prepared row insert");
            goto rollback;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared row insert");
            goto rollback;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_INSERTS,
            "prepared inserts in one transaction",
            ctx->config->rows,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize prepared row insert");
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    unsigned long long row_count = 0U;
    if (query_uint64(ctx, "SELECT COUNT(*) FROM perf_prepared_rows", &row_count) != 0) {
        return 1;
    }
    if (row_count != (unsigned long long)ctx->config->rows) {
        fprintf(stderr, "Expected %zu prepared rows, got %llu\n", ctx->config->rows, row_count);
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared row insert");
        result = 1;
    }
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_prepared_insert_components(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t bind_ns = 0U;
    uint64_t step_ns = 0U;
    uint64_t reset_ns = 0U;
    uint64_t commit_ns = 0U;
    int transaction_open = 0;
    int result = 1;
#ifdef MYLITE_STORAGE_TEST_HOOKS
    prepared_insert_checksum_snapshot before_commit_counters = {0};
    prepared_insert_checksum_snapshot after_commit_counters = {0};
    prepared_insert_checksum_snapshot after_verification_counters = {0};
#endif

    if (exec_sql(
            ctx,
            "CREATE TABLE perf_prepared_component_rows ("
            "id INT NOT NULL PRIMARY KEY,"
            "value INT NOT NULL,"
            "pad VARCHAR(64) NOT NULL,"
            "KEY value_key (value)"
            ") ENGINE=InnoDB"
        ) != 0) {
        return 1;
    }
    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    transaction_open = 1;
    if (mylite_prepare(
            ctx->db,
            "INSERT INTO perf_prepared_component_rows (id, value, pad) VALUES (?, ?, ?)",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare prepared insert components");
        goto rollback;
    }
    reset_prepared_insert_storage_counters();

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        char pad[32];
        const size_t row_id = i + 1U;
        const int written = snprintf(pad, sizeof(pad), "row-%zu", row_id);
        if (written < 0 || (size_t)written >= sizeof(pad)) {
            goto rollback;
        }

        uint64_t start_ns = monotonic_ns();
        int bind_result = mylite_bind_int64(stmt, 1U, (long long)row_id);
        if (bind_result == MYLITE_OK) {
            bind_result =
                mylite_bind_int64(stmt, 2U, (long long)secondary_value_for_row(ctx, row_id));
        }
        if (bind_result == MYLITE_OK) {
            bind_result = mylite_bind_text(stmt, 3U, pad, MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT);
        }
        bind_ns += monotonic_ns() - start_ns;
        if (bind_result != MYLITE_OK) {
            report_database_error(ctx, "bind prepared insert component");
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int step_result = mylite_step(stmt);
        step_ns += monotonic_ns() - start_ns;
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared insert component failed for id %zu\n", row_id);
            report_database_error(ctx, "prepared insert component");
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int reset_result = mylite_reset(stmt);
        reset_ns += monotonic_ns() - start_ns;
        if (reset_result != MYLITE_OK) {
            report_database_error(ctx, "reset prepared insert component");
            goto rollback;
        }
    }

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize prepared insert components");
        goto rollback;
    }
    stmt = NULL;

#ifdef MYLITE_STORAGE_TEST_HOOKS
    snapshot_prepared_insert_checksum_counters(&before_commit_counters);
#endif

    uint64_t start_ns = monotonic_ns();
    if (exec_sql(ctx, "COMMIT") != 0) {
        goto rollback;
    }
    commit_ns = monotonic_ns() - start_ns;
    transaction_open = 0;
#ifdef MYLITE_STORAGE_TEST_HOOKS
    snapshot_prepared_insert_checksum_counters(&after_commit_counters);
#endif

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_BIND,
            "prepared insert bind component",
            ctx->config->iterations,
            bind_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_STEP,
            "prepared insert step component",
            ctx->config->iterations,
            step_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_RESET,
            "prepared insert reset component",
            ctx->config->iterations,
            reset_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_INSERT_COMPONENT_COMMIT,
            "prepared insert commit component",
            1U,
            commit_ns
        ) != 0) {
        goto rollback;
    }
    unsigned long long row_count = 0U;
    if (query_uint64(ctx, "SELECT COUNT(*) FROM perf_prepared_component_rows", &row_count) != 0) {
        return 1;
    }
    if (row_count != (unsigned long long)ctx->config->iterations) {
        fprintf(
            stderr,
            "Expected %zu prepared component rows, got %llu\n",
            ctx->config->iterations,
            row_count
        );
        return 1;
    }
#ifdef MYLITE_STORAGE_TEST_HOOKS
    snapshot_prepared_insert_checksum_counters(&after_verification_counters);
#endif
    print_prepared_insert_storage_counters();
#ifdef MYLITE_STORAGE_TEST_HOOKS
    print_prepared_insert_checksum_phase_counters(
        &before_commit_counters,
        &after_commit_counters,
        &after_verification_counters
    );
#endif
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared insert components");
        result = 1;
    }
    if (result != 0 && transaction_open) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static void reset_prepared_insert_storage_counters(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    mylite_storage_test_reset_branch_leaf_range_plan_read_count();
    mylite_storage_test_reset_branch_refold_root_read_count();
    mylite_storage_test_reset_branch_refold_entryset_read_count();
    mylite_storage_test_reset_level_two_branch_leaf_plan_read_count();
    mylite_storage_test_reset_active_branch_page_plan_read_count();
    mylite_storage_test_reset_packed_index_tail_append_scan_page_count();
    mylite_storage_test_reset_branch_insert_writer_decode_counts();
    mylite_storage_test_reset_branch_tail_overlay_scan_counts();
    mylite_storage_test_reset_prepared_insert_profile_counts();
#endif
}

static void print_prepared_insert_storage_counters(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    printf("\nPrepared insert storage counters:\n\n");
    printf("| Counter | Value |\n");
    printf("| --- | ---: |\n");
    printf(
        "| branch leaf-range plan reads | %llu |\n",
        mylite_storage_test_branch_leaf_range_plan_read_count()
    );
    printf(
        "| branch refold root reads | %llu |\n",
        mylite_storage_test_branch_refold_root_read_count()
    );
    printf(
        "| branch refold entryset reads | %llu |\n",
        mylite_storage_test_branch_refold_entryset_read_count()
    );
    printf(
        "| branch refold entryset cache hits | %llu |\n",
        mylite_storage_test_branch_refold_entryset_cache_hit_count()
    );
    printf(
        "| level-two branch leaf plan reads | %llu |\n",
        mylite_storage_test_level_two_branch_leaf_plan_read_count()
    );
    printf(
        "| active branch page plan reads | %llu |\n",
        mylite_storage_test_active_branch_page_plan_read_count()
    );
    printf(
        "| packed index tail-append scan pages | %llu |\n",
        mylite_storage_test_packed_index_tail_append_scan_page_count()
    );
    printf(
        "| packed index tail-append row-page scans | %llu |\n",
        mylite_storage_test_packed_index_tail_append_scan_row_page_count()
    );
    printf(
        "| packed index tail-append other-index scans | %llu |\n",
        mylite_storage_test_packed_index_tail_append_scan_other_index_page_count()
    );
    printf(
        "| packed index tail-append same-index blockers | %llu |\n",
        mylite_storage_test_packed_index_tail_append_scan_same_index_page_count()
    );
    printf(
        "| packed index tail-append missing-page blockers | %llu |\n",
        mylite_storage_test_packed_index_tail_append_scan_missing_page_count()
    );
    printf(
        "| packed index tail-append invalid-page blockers | %llu |\n",
        mylite_storage_test_packed_index_tail_append_scan_invalid_page_count()
    );
    printf(
        "| branch insert writer branch decodes | %llu |\n",
        mylite_storage_test_branch_insert_writer_branch_decode_count()
    );
    printf(
        "| branch insert writer leaf decodes | %llu |\n",
        mylite_storage_test_branch_insert_writer_leaf_decode_count()
    );
    printf(
        "| branch tail overlay scans | %llu |\n",
        mylite_storage_test_branch_tail_overlay_scan_count()
    );
    printf(
        "| branch tail overlay scan reads | %llu |\n",
        mylite_storage_test_branch_tail_overlay_scan_read_count()
    );
    printf(
        "| branch tail overlay index-entry scans | %llu |\n",
        mylite_storage_test_branch_tail_overlay_scan_index_entry_page_count()
    );
    printf(
        "| branch tail overlay row-state scans | %llu |\n",
        mylite_storage_test_branch_tail_overlay_scan_row_state_page_count()
    );
    printf(
        "| branch tail overlay row-page skips | %llu |\n",
        mylite_storage_test_branch_tail_overlay_scan_row_page_skip_count()
    );
    printf(
        "| branch tail overlay index-structure skips | %llu |\n",
        mylite_storage_test_branch_tail_overlay_scan_index_structure_skip_count()
    );
    printf(
        "| branch tail overlay other skips | %llu |\n",
        mylite_storage_test_branch_tail_overlay_scan_other_skip_count()
    );
    printf(
        "| branch tail overlay overlay hits | %llu |\n",
        mylite_storage_test_branch_tail_overlay_scan_overlay_hit_count()
    );
    printf("| full-page checksum calls | %llu |\n", mylite_storage_test_checksum_page_count());
    printf(
        "| zero-tail checksum calls | %llu |\n",
        mylite_storage_test_checksum_page_zero_tail_count()
    );
    printf(
        "| raw entry order builds | %llu |\n",
        mylite_storage_test_raw_index_entry_order_build_count()
    );
    printf(
        "| raw entry order probes | %llu |\n",
        mylite_storage_test_raw_index_entry_order_probe_count()
    );
    printf("\nPrepared insert checksum counters by page family:\n\n");
    printf("| Page family | Full-page | Zero-tail | Dirty refresh |\n");
    printf("| --- | ---: | ---: | ---: |\n");
    const size_t checksum_family_count = mylite_storage_test_checksum_page_family_slot_count();
    for (size_t i = 0U; i < checksum_family_count; ++i) {
        printf(
            "| %s | %llu | %llu | %llu |\n",
            mylite_storage_test_checksum_page_family_slot_name(i),
            mylite_storage_test_checksum_page_family_count(i),
            mylite_storage_test_checksum_page_zero_tail_family_count(i),
            mylite_storage_test_dirty_checksum_refresh_family_count(i)
        );
    }
    printf("\nPrepared insert dirty checksum refresh counters by source:\n\n");
    printf("| Source | Refreshes |\n");
    printf("| --- | ---: |\n");
    const size_t checksum_source_count =
        mylite_storage_test_dirty_checksum_refresh_source_slot_count();
    for (size_t i = 0U; i < checksum_source_count; ++i) {
        printf(
            "| %s | %llu |\n",
            mylite_storage_test_dirty_checksum_refresh_source_slot_name(i),
            mylite_storage_test_dirty_checksum_refresh_source_count(i)
        );
    }
#endif
}

#ifdef MYLITE_STORAGE_TEST_HOOKS
static void snapshot_prepared_insert_checksum_counters(
    prepared_insert_checksum_snapshot *snapshot
) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->full_page_count = mylite_storage_test_checksum_page_count();
    snapshot->zero_tail_count = mylite_storage_test_checksum_page_zero_tail_count();
    snapshot->source_count = mylite_storage_test_dirty_checksum_refresh_source_slot_count();
    if (snapshot->source_count > BENCHMARK_STORAGE_COUNTER_SLOT_LIMIT) {
        snapshot->source_count = BENCHMARK_STORAGE_COUNTER_SLOT_LIMIT;
    }
    for (size_t i = 0U; i < snapshot->source_count; ++i) {
        snapshot->dirty_refresh_source_counts[i] =
            mylite_storage_test_dirty_checksum_refresh_source_count(i);
    }
}

static unsigned long long counter_delta(unsigned long long after, unsigned long long before) {
    return after >= before ? after - before : 0ULL;
}

static unsigned long long dirty_refresh_source_total(
    const prepared_insert_checksum_snapshot *snapshot
) {
    unsigned long long total = 0ULL;
    for (size_t i = 0U; i < snapshot->source_count; ++i) {
        total += snapshot->dirty_refresh_source_counts[i];
    }
    return total;
}

static unsigned long long dirty_refresh_source_delta(
    const prepared_insert_checksum_snapshot *after,
    const prepared_insert_checksum_snapshot *before,
    size_t slot
) {
    const unsigned long long after_count =
        slot < after->source_count ? after->dirty_refresh_source_counts[slot] : 0ULL;
    const unsigned long long before_count =
        slot < before->source_count ? before->dirty_refresh_source_counts[slot] : 0ULL;
    return counter_delta(after_count, before_count);
}

static void print_prepared_insert_checksum_phase_row(
    const char *phase,
    unsigned long long full_page_count,
    unsigned long long zero_tail_count,
    unsigned long long dirty_refresh_count
) {
    printf(
        "| %s | %llu | %llu | %llu |\n",
        phase,
        full_page_count,
        zero_tail_count,
        dirty_refresh_count
    );
}

static void print_prepared_insert_checksum_phase_counters(
    const prepared_insert_checksum_snapshot *before_commit,
    const prepared_insert_checksum_snapshot *after_commit,
    const prepared_insert_checksum_snapshot *after_verification
) {
    const unsigned long long before_commit_dirty_refresh =
        dirty_refresh_source_total(before_commit);
    const unsigned long long after_commit_dirty_refresh = dirty_refresh_source_total(after_commit);
    const unsigned long long after_verification_dirty_refresh =
        dirty_refresh_source_total(after_verification);

    printf("\nPrepared insert checksum counters by phase:\n\n");
    printf("| Phase | Full-page | Zero-tail | Dirty refresh |\n");
    printf("| --- | ---: | ---: | ---: |\n");
    print_prepared_insert_checksum_phase_row(
        "insert loop",
        before_commit->full_page_count,
        before_commit->zero_tail_count,
        before_commit_dirty_refresh
    );
    print_prepared_insert_checksum_phase_row(
        "commit",
        counter_delta(after_commit->full_page_count, before_commit->full_page_count),
        counter_delta(after_commit->zero_tail_count, before_commit->zero_tail_count),
        counter_delta(after_commit_dirty_refresh, before_commit_dirty_refresh)
    );
    print_prepared_insert_checksum_phase_row(
        "verification",
        counter_delta(after_verification->full_page_count, after_commit->full_page_count),
        counter_delta(after_verification->zero_tail_count, after_commit->zero_tail_count),
        counter_delta(after_verification_dirty_refresh, after_commit_dirty_refresh)
    );

    printf("\nPrepared insert dirty checksum refresh source counters by phase:\n\n");
    printf("| Source | Insert loop | Commit | Verification |\n");
    printf("| --- | ---: | ---: | ---: |\n");
    const size_t source_count = after_verification->source_count;
    for (size_t i = 0U; i < source_count; ++i) {
        printf(
            "| %s | %llu | %llu | %llu |\n",
            mylite_storage_test_dirty_checksum_refresh_source_slot_name(i),
            i < before_commit->source_count ? before_commit->dirty_refresh_source_counts[i] : 0ULL,
            dirty_refresh_source_delta(after_commit, before_commit, i),
            dirty_refresh_source_delta(after_verification, after_commit, i)
        );
    }
}
#endif

static void reset_prepared_update_storage_counters(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    mylite_storage_test_reset_prepared_update_storage_counts();
#endif
}

static void print_prepared_update_storage_counters(void) {
#ifdef MYLITE_STORAGE_TEST_HOOKS
    printf("\nPrepared update storage counters:\n\n");
    printf("| Counter | Value |\n");
    printf("| --- | ---: |\n");
    printf(
        "| indexed row file-scope reads | %llu |\n",
        mylite_storage_test_indexed_row_file_read_count()
    );
    printf(
        "| indexed row statement-scope reads | %llu |\n",
        mylite_storage_test_indexed_row_statement_read_count()
    );
    printf(
        "| preserving-index update file-scope writes | %llu |\n",
        mylite_storage_test_preserving_index_update_file_count()
    );
    printf(
        "| preserving-index update statement-scope writes | %llu |\n",
        mylite_storage_test_preserving_index_update_statement_count()
    );
    printf(
        "| changed-index update file-scope writes | %llu |\n",
        mylite_storage_test_changed_index_update_file_count()
    );
    printf(
        "| changed-index update statement-scope writes | %llu |\n",
        mylite_storage_test_changed_index_update_statement_count()
    );
    printf(
        "| maintained-root update plans | %llu |\n",
        mylite_storage_test_update_maintained_root_plan_count()
    );
    printf(
        "| maintained-root update writes | %llu |\n",
        mylite_storage_test_update_maintained_root_update_count()
    );
    printf(
        "| maintained-root retarget writes | %llu |\n",
        mylite_storage_test_update_maintained_root_retarget_count()
    );
    printf(
        "| maintained-root no-plan cache hits | %llu |\n",
        mylite_storage_test_update_maintained_root_no_plan_cache_hit_count()
    );
    printf(
        "| maintained-root no-plan cache stores | %llu |\n",
        mylite_storage_test_update_maintained_root_no_plan_cache_store_count()
    );
    printf(
        "| active rewrite attempts | %llu |\n",
        mylite_storage_test_update_active_rewrite_attempt_count()
    );
    printf(
        "| active rewrite successes | %llu |\n",
        mylite_storage_test_update_active_rewrite_success_count()
    );
    printf(
        "| active row-only rewrite successes | %llu |\n",
        mylite_storage_test_update_active_row_only_rewrite_count()
    );
    printf(
        "| active single-index rewrite successes | %llu |\n",
        mylite_storage_test_update_active_single_index_rewrite_count()
    );
    printf(
        "| active rewrite maintained-root skips | %llu |\n",
        mylite_storage_test_update_active_rewrite_maintained_root_skip_count()
    );
    printf("| inline update writes | %llu |\n", mylite_storage_test_update_inline_write_count());
    printf("| append update writes | %llu |\n", mylite_storage_test_update_append_write_count());
#endif
}

static int benchmark_point_selects(benchmark_context *ctx) {
    uint64_t checksum = 0U;
    const uint64_t start_ns = monotonic_ns();

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        char sql[80];
        unsigned long long value = 0U;
        const int written =
            snprintf(sql, sizeof(sql), "SELECT value FROM perf_rows WHERE id = %zu", id);
        if (written < 0 || (size_t)written >= sizeof(sql) || query_uint64(ctx, sql, &value) != 0) {
            return 1;
        }
        checksum += (uint64_t)value;
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_DIRECT_PK_SELECTS,
            "direct primary-key point selects",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
    printf("Point-select checksum: %" PRIu64 "\n", checksum);
    return 0;
}

static int benchmark_prepared_point_selects(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t checksum = 0U;
    int result = 1;

    if (mylite_prepare(
            ctx->db,
            "SELECT value FROM perf_rows WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare primary-key point select");
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        if (mylite_bind_int64(stmt, 1U, (long long)id) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key point select");
            goto cleanup;
        }

        const int row_result = mylite_step(stmt);
        if (row_result != MYLITE_ROW) {
            fprintf(stderr, "Prepared primary-key point select returned no row for id %zu\n", id);
            report_database_error(ctx, "prepared primary-key point select");
            goto cleanup;
        }
        if (mylite_column_type(stmt, 0U) != MYLITE_TYPE_INT64) {
            fprintf(stderr, "Prepared primary-key point select returned a non-integer value\n");
            goto cleanup;
        }
        checksum += (uint64_t)mylite_column_int64(stmt, 0U);

        const int done_result = mylite_step(stmt);
        if (done_result != MYLITE_DONE) {
            fprintf(
                stderr,
                "Prepared primary-key point select returned extra rows for id %zu\n",
                id
            );
            report_database_error(ctx, "prepared primary-key point select completion");
            goto cleanup;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key point select");
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECTS,
            "prepared primary-key point selects",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared point-select checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key point select");
        return 1;
    }
    return result;
}

static int benchmark_prepared_point_select_components(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t checksum = 0U;
    uint64_t bind_ns = 0U;
    uint64_t row_ns = 0U;
    uint64_t done_ns = 0U;
    uint64_t reset_ns = 0U;
    int result = 1;

    if (mylite_prepare(
            ctx->db,
            "SELECT value FROM perf_rows WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare primary-key point select components");
        return 1;
    }

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        uint64_t start_ns = monotonic_ns();
        const int bind_result = mylite_bind_int64(stmt, 1U, (long long)id);
        bind_ns += monotonic_ns() - start_ns;
        if (bind_result != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key point select components");
            goto cleanup;
        }

        start_ns = monotonic_ns();
        const int row_result = mylite_step(stmt);
        const int row_is_int =
            row_result == MYLITE_ROW && mylite_column_type(stmt, 0U) == MYLITE_TYPE_INT64;
        if (row_is_int) {
            checksum += (uint64_t)mylite_column_int64(stmt, 0U);
        }
        row_ns += monotonic_ns() - start_ns;
        if (row_result != MYLITE_ROW) {
            fprintf(
                stderr,
                "Prepared primary-key point select component phase returned no row for id %zu\n",
                id
            );
            report_database_error(ctx, "prepared primary-key point select components");
            goto cleanup;
        }
        if (!row_is_int) {
            fprintf(
                stderr,
                "Prepared primary-key point select component phase returned a non-integer value\n"
            );
            goto cleanup;
        }

        start_ns = monotonic_ns();
        const int done_result = mylite_step(stmt);
        done_ns += monotonic_ns() - start_ns;
        if (done_result != MYLITE_DONE) {
            fprintf(
                stderr,
                "Prepared primary-key point select component phase returned extra rows for id "
                "%zu\n",
                id
            );
            report_database_error(ctx, "prepared primary-key point select component drain");
            goto cleanup;
        }

        start_ns = monotonic_ns();
        const int reset_result = mylite_reset(stmt);
        reset_ns += monotonic_ns() - start_ns;
        if (reset_result != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key point select components");
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_BIND,
            "prepared primary-key bind component",
            ctx->config->iterations,
            bind_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_ROW,
            "prepared primary-key row component",
            ctx->config->iterations,
            row_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_DONE,
            "prepared primary-key done component",
            ctx->config->iterations,
            done_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_RESET,
            "prepared primary-key reset component",
            ctx->config->iterations,
            reset_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared point-select component checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key point select components");
        return 1;
    }
    return result;
}

static int benchmark_prepared_point_select_miss_components(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t bind_ns = 0U;
    uint64_t step_ns = 0U;
    uint64_t reset_ns = 0U;
    uint64_t misses = 0U;
    int result = 1;

    if (mylite_prepare(
            ctx->db,
            "SELECT value FROM perf_rows WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare primary-key point miss components");
        return 1;
    }

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = ctx->config->rows + (i % ctx->config->rows) + 1U;
        uint64_t start_ns = monotonic_ns();
        const int bind_result = mylite_bind_int64(stmt, 1U, (long long)id);
        bind_ns += monotonic_ns() - start_ns;
        if (bind_result != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key point miss components");
            goto cleanup;
        }

        start_ns = monotonic_ns();
        const int step_result = mylite_step(stmt);
        step_ns += monotonic_ns() - start_ns;
        if (step_result != MYLITE_DONE) {
            fprintf(
                stderr,
                "Prepared primary-key point miss component phase returned a row for id %zu\n",
                id
            );
            report_database_error(ctx, "prepared primary-key point miss components");
            goto cleanup;
        }
        ++misses;

        start_ns = monotonic_ns();
        const int reset_result = mylite_reset(stmt);
        reset_ns += monotonic_ns() - start_ns;
        if (reset_result != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key point miss components");
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_MISS_COMPONENT_BIND,
            "prepared primary-key miss bind component",
            ctx->config->iterations,
            bind_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_MISS_COMPONENT_STEP,
            "prepared primary-key miss step component",
            ctx->config->iterations,
            step_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_MISS_COMPONENT_RESET,
            "prepared primary-key miss reset component",
            ctx->config->iterations,
            reset_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared point-select miss count: %" PRIu64 "\n", misses);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key point miss components");
        return 1;
    }
    return result;
}

static int benchmark_prepared_text_select_components(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t checksum = 0U;
    uint64_t bind_ns = 0U;
    uint64_t row_ns = 0U;
    uint64_t done_ns = 0U;
    uint64_t reset_ns = 0U;
    int result = 1;

    uint64_t start_ns = monotonic_ns();
    if (prepare_text_select_rows(ctx) != 0) {
        return 1;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARE_TEXT_SELECT_ROWS,
            "prepare text-key select rows",
            ctx->config->rows,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "SELECT id FROM perf_text_rows WHERE slug = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare text-key point select components");
        return 1;
    }

    start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        if (step_prepared_text_select_component_iteration(
                ctx,
                stmt,
                i + 1U,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL
            ) != 0) {
            goto cleanup;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_WARM_TEXT_SELECT_CACHE,
            "warm text-key select cache",
            ctx->config->rows,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        if (step_prepared_text_select_component_iteration(
                ctx,
                stmt,
                (i % ctx->config->rows) + 1U,
                &checksum,
                &bind_ns,
                &row_ns,
                &done_ns,
                &reset_ns
            ) != 0) {
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_BIND,
            "prepared text-key bind component",
            ctx->config->iterations,
            bind_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_ROW,
            "prepared text-key row component",
            ctx->config->iterations,
            row_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_DONE,
            "prepared text-key done component",
            ctx->config->iterations,
            done_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_TEXT_SELECT_COMPONENT_RESET,
            "prepared text-key reset component",
            ctx->config->iterations,
            reset_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared text-key point-select component checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared text-key point select components");
        return 1;
    }
    return result;
}

static int step_prepared_text_select_component_iteration(
    benchmark_context *ctx,
    mylite_stmt *stmt,
    size_t id,
    uint64_t *checksum,
    uint64_t *bind_ns,
    uint64_t *row_ns,
    uint64_t *done_ns,
    uint64_t *reset_ns
) {
    char slug[32];
    const int written = snprintf(slug, sizeof(slug), "slug-%zu", id);
    if (written < 0 || (size_t)written >= sizeof(slug)) {
        return 1;
    }

    uint64_t start_ns = monotonic_ns();
    const int bind_result =
        mylite_bind_text(stmt, 1U, slug, MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT);
    if (bind_ns != NULL) {
        *bind_ns += monotonic_ns() - start_ns;
    }
    if (bind_result != MYLITE_OK) {
        report_database_error(ctx, "bind prepared text-key point select components");
        return 1;
    }

    start_ns = monotonic_ns();
    const int row_result = mylite_step(stmt);
    const int row_is_int =
        row_result == MYLITE_ROW && mylite_column_type(stmt, 0U) == MYLITE_TYPE_INT64;
    const unsigned long long value =
        row_is_int ? (unsigned long long)mylite_column_int64(stmt, 0U) : 0ULL;
    if (row_ns != NULL) {
        *row_ns += monotonic_ns() - start_ns;
    }
    if (row_result != MYLITE_ROW) {
        fprintf(
            stderr,
            "Prepared text-key point select component phase returned no row for slug %s\n",
            slug
        );
        report_database_error(ctx, "prepared text-key point select components");
        return 1;
    }
    if (!row_is_int) {
        fprintf(
            stderr,
            "Prepared text-key point select component phase returned a non-integer value\n"
        );
        return 1;
    }
    if (value != (unsigned long long)id) {
        fprintf(
            stderr,
            "Prepared text-key point select component phase returned id %llu for slug %s; "
            "expected %zu\n",
            value,
            slug,
            id
        );
        return 1;
    }
    if (checksum != NULL) {
        *checksum += (uint64_t)value;
    }

    start_ns = monotonic_ns();
    const int done_result = mylite_step(stmt);
    if (done_ns != NULL) {
        *done_ns += monotonic_ns() - start_ns;
    }
    if (done_result != MYLITE_DONE) {
        fprintf(
            stderr,
            "Prepared text-key point select component phase returned extra rows for slug %s\n",
            slug
        );
        report_database_error(ctx, "prepared text-key point select component drain");
        return 1;
    }

    start_ns = monotonic_ns();
    const int reset_result = mylite_reset(stmt);
    if (reset_ns != NULL) {
        *reset_ns += monotonic_ns() - start_ns;
    }
    if (reset_result != MYLITE_OK) {
        report_database_error(ctx, "reset prepared text-key point select components");
        return 1;
    }
    return 0;
}

static int prepare_text_select_rows(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    int result = 1;

    if (exec_sql(
            ctx,
            "CREATE TABLE perf_text_rows ("
            "id INT NOT NULL PRIMARY KEY,"
            "slug VARCHAR(64) NOT NULL,"
            "value INT NOT NULL,"
            "UNIQUE KEY slug_key (slug)"
            ") ENGINE=InnoDB"
        ) != 0) {
        return 1;
    }
    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "INSERT INTO perf_text_rows (id, slug, value) VALUES (?, ?, ?)",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare text-key rows");
        goto rollback;
    }

    for (size_t i = 0; i < ctx->config->rows; ++i) {
        const size_t row_id = i + 1U;
        char slug[32];
        const int written = snprintf(slug, sizeof(slug), "slug-%zu", row_id);
        if (written < 0 || (size_t)written >= sizeof(slug)) {
            goto rollback;
        }
        if (mylite_bind_int64(stmt, 1U, (long long)row_id) != MYLITE_OK ||
            mylite_bind_text(stmt, 2U, slug, MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT) !=
                MYLITE_OK ||
            mylite_bind_int64(stmt, 3U, (long long)secondary_value_for_row(ctx, row_id)) !=
                MYLITE_OK) {
            report_database_error(ctx, "bind text-key row");
            goto rollback;
        }

        const int step_result = mylite_step(stmt);
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared text-key row insert failed for id %zu\n", row_id);
            report_database_error(ctx, "prepared text-key row insert");
            goto rollback;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared text-key row insert");
            goto rollback;
        }
    }

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize prepared text-key row insert");
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared text-key row insert");
        result = 1;
    }
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_prepared_point_select_reset_after_row(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t checksum = 0U;
    int result = 1;

    if (mylite_prepare(
            ctx->db,
            "SELECT value FROM perf_rows WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare primary-key point select reset-after-row");
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        if (mylite_bind_int64(stmt, 1U, (long long)id) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key point select reset-after-row");
            goto cleanup;
        }

        const int row_result = mylite_step(stmt);
        if (row_result != MYLITE_ROW) {
            fprintf(
                stderr,
                "Prepared primary-key reset-after-row point select returned no row for id %zu\n",
                id
            );
            report_database_error(ctx, "prepared primary-key point select reset-after-row");
            goto cleanup;
        }
        if (mylite_column_type(stmt, 0U) != MYLITE_TYPE_INT64) {
            fprintf(
                stderr,
                "Prepared primary-key reset-after-row point select returned a non-integer value\n"
            );
            goto cleanup;
        }
        checksum += (uint64_t)mylite_column_int64(stmt, 0U);

        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key point select after row");
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_RESET_AFTER_ROW,
            "prepared primary-key point selects reset after row",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared reset-after-row point-select checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key point select reset-after-row");
        return 1;
    }
    return result;
}

static int benchmark_storage_entry_lookups(benchmark_context *ctx) {
    return benchmark_storage_entry_lookups_with_scope(
        ctx,
        0,
        BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS,
        "storage primary-key entry lookups"
    );
}

static int benchmark_storage_entry_lookups_in_one_read_statement(benchmark_context *ctx) {
    return benchmark_storage_entry_lookups_with_scope(
        ctx,
        1,
        BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ,
        "storage primary-key entry lookups in one read statement"
    );
}

static int benchmark_storage_entry_lookups_with_scope(
    benchmark_context *ctx,
    int one_read_statement,
    benchmark_metric metric,
    const char *operation
) {
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    mylite_storage_statement *shared_statement = NULL;
    uint64_t row_id_checksum = 0U;
    int result = 1;

    mylite_storage_result storage_result =
        mylite_storage_read_index_entries(ctx->filename, "perf", "perf_rows", 0U, &entries);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read primary-key index entries: %d\n", storage_result);
        return 1;
    }
    if (entries.entry_count != ctx->config->rows) {
        fprintf(
            stderr,
            "Expected %zu primary-key entries, got %zu\n",
            ctx->config->rows,
            entries.entry_count
        );
        goto cleanup;
    }

    mylite_storage_filename_identity_scope filename_scope = {0};
    mylite_storage_table_name_identity_scope table_scope = {0};
    mylite_storage_begin_filename_identity_scope(ctx->filename, &filename_scope);
    mylite_storage_begin_table_name_identity_scope("perf", "perf_rows", &table_scope);

    if (one_read_statement) {
        storage_result = mylite_storage_begin_read_statement(ctx->filename, &shared_statement);
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
            goto end_scopes;
        }
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t entry_index = i % entries.entry_count;
        if (entry_index >= entries.entry_count ||
            entries.key_offsets[entry_index] > entries.key_bytes ||
            entries.key_sizes[entry_index] > entries.key_bytes - entries.key_offsets[entry_index]) {
            fprintf(stderr, "Primary-key index entry %zu is corrupt\n", entry_index);
            goto end_scopes;
        }

        mylite_storage_statement *statement = NULL;
        if (!one_read_statement) {
            storage_result = mylite_storage_begin_read_statement(ctx->filename, &statement);
            if (storage_result != MYLITE_STORAGE_OK) {
                fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
                goto end_read_statement;
            }
        }

        unsigned long long row_id = 0ULL;
        storage_result = mylite_storage_find_index_entry(
            ctx->filename,
            "perf",
            "perf_rows",
            0U,
            entries.keys + entries.key_offsets[entry_index],
            entries.key_sizes[entry_index],
            &row_id
        );
        mylite_storage_result end_result = MYLITE_STORAGE_OK;
        if (!one_read_statement) {
            end_result = mylite_storage_end_read_statement(statement);
        }
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Storage entry lookup failed: %d\n", storage_result);
            goto end_read_statement;
        }
        if (end_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", end_result);
            goto end_read_statement;
        }
        if (row_id != entries.row_ids[entry_index]) {
            fprintf(
                stderr,
                "Storage entry lookup returned row_id=%llu for entry %zu; expected row_id=%llu\n",
                row_id,
                entry_index,
                entries.row_ids[entry_index]
            );
            goto end_read_statement;
        }

        row_id_checksum += row_id;
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto end_read_statement;
    }
    printf("Storage entry-lookup row-id checksum: %" PRIu64 "\n", row_id_checksum);
    result = 0;

end_read_statement:
    if (shared_statement != NULL) {
        const mylite_storage_result end_result =
            mylite_storage_end_read_statement(shared_statement);
        if (end_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", end_result);
            result = 1;
        }
    }
end_scopes:
    mylite_storage_end_table_name_identity_scope(&table_scope);
    mylite_storage_end_filename_identity_scope(&filename_scope);
cleanup:
    mylite_storage_free_index_entryset(&entries);
    return result;
}

static int benchmark_storage_point_lookups(benchmark_context *ctx) {
    return benchmark_storage_point_lookups_with_scope(
        ctx,
        0,
        BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS,
        "storage primary-key row lookups"
    );
}

static int benchmark_storage_point_lookups_in_one_read_statement(benchmark_context *ctx) {
    return benchmark_storage_point_lookups_with_scope(
        ctx,
        1,
        BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS_ONE_READ,
        "storage primary-key row lookups in one read statement"
    );
}

static int benchmark_storage_point_lookups_with_scope(
    benchmark_context *ctx,
    int one_read_statement,
    benchmark_metric metric,
    const char *operation
) {
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    mylite_storage_statement *shared_statement = NULL;
    unsigned char row[1024];
    uint64_t row_id_checksum = 0U;
    uint64_t row_size_checksum = 0U;
    int result = 1;

    mylite_storage_result storage_result =
        mylite_storage_read_index_entries(ctx->filename, "perf", "perf_rows", 0U, &entries);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read primary-key index entries: %d\n", storage_result);
        return 1;
    }
    if (entries.entry_count != ctx->config->rows) {
        fprintf(
            stderr,
            "Expected %zu primary-key entries, got %zu\n",
            ctx->config->rows,
            entries.entry_count
        );
        goto cleanup;
    }

    mylite_storage_filename_identity_scope filename_scope = {0};
    mylite_storage_table_name_identity_scope table_scope = {0};
    mylite_storage_begin_filename_identity_scope(ctx->filename, &filename_scope);
    mylite_storage_begin_table_name_identity_scope("perf", "perf_rows", &table_scope);

    if (one_read_statement) {
        storage_result = mylite_storage_begin_read_statement(ctx->filename, &shared_statement);
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
            goto end_scopes;
        }
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t entry_index = i % entries.entry_count;
        if (entry_index >= entries.entry_count ||
            entries.key_offsets[entry_index] > entries.key_bytes ||
            entries.key_sizes[entry_index] > entries.key_bytes - entries.key_offsets[entry_index]) {
            fprintf(stderr, "Primary-key index entry %zu is corrupt\n", entry_index);
            goto end_scopes;
        }

        mylite_storage_statement *statement = NULL;
        if (!one_read_statement) {
            storage_result = mylite_storage_begin_read_statement(ctx->filename, &statement);
            if (storage_result != MYLITE_STORAGE_OK) {
                fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
                goto end_read_statement;
            }
        }

        unsigned long long row_id = 0ULL;
        size_t row_size = 0U;
        storage_result = mylite_storage_find_indexed_row_into(
            ctx->filename,
            "perf",
            "perf_rows",
            0U,
            entries.keys + entries.key_offsets[entry_index],
            entries.key_sizes[entry_index],
            &row_id,
            row,
            sizeof(row),
            &row_size
        );
        mylite_storage_result end_result = MYLITE_STORAGE_OK;
        if (!one_read_statement) {
            end_result = mylite_storage_end_read_statement(statement);
        }
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Storage point lookup failed: %d\n", storage_result);
            goto end_read_statement;
        }
        if (end_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", end_result);
            goto end_read_statement;
        }
        if (row_id != entries.row_ids[entry_index] || row_size == 0U || row_size > sizeof(row)) {
            fprintf(
                stderr,
                "Storage point lookup returned row_id=%llu row_size=%zu for entry %zu; "
                "expected row_id=%llu\n",
                row_id,
                row_size,
                entry_index,
                entries.row_ids[entry_index]
            );
            goto end_read_statement;
        }

        row_id_checksum += row_id;
        row_size_checksum += row_size;
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto end_read_statement;
    }
    printf("Storage point-lookup row-id checksum: %" PRIu64 "\n", row_id_checksum);
    printf("Storage point-lookup row-size checksum: %" PRIu64 "\n", row_size_checksum);
    result = 0;

end_read_statement:
    if (shared_statement != NULL) {
        const mylite_storage_result end_result =
            mylite_storage_end_read_statement(shared_statement);
        if (end_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", end_result);
            result = 1;
        }
    }
end_scopes:
    mylite_storage_end_table_name_identity_scope(&table_scope);
    mylite_storage_end_filename_identity_scope(&filename_scope);
cleanup:
    mylite_storage_free_index_entryset(&entries);
    return result;
}

static int benchmark_storage_read_statements(benchmark_context *ctx) {
    uint64_t opened_statement_count = 0U;
    int result = 1;

    mylite_storage_filename_identity_scope filename_scope = {0};
    mylite_storage_begin_filename_identity_scope(ctx->filename, &filename_scope);

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        mylite_storage_statement *statement = NULL;
        mylite_storage_result storage_result =
            mylite_storage_begin_read_statement(ctx->filename, &statement);
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
            goto end_scope;
        }
        if (statement != NULL) {
            ++opened_statement_count;
        }

        storage_result = mylite_storage_end_read_statement(statement);
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", storage_result);
            goto end_scope;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_STORAGE_READ_STATEMENTS,
            "storage read statement begin/end pairs",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto end_scope;
    }
    printf("Storage read-statement opened count: %" PRIu64 "\n", opened_statement_count);
    result = 0;

end_scope:
    mylite_storage_end_filename_identity_scope(&filename_scope);
    return result;
}

static int benchmark_storage_row_updates(benchmark_context *ctx) {
    return benchmark_storage_row_update_loop(ctx, 0);
}

static int benchmark_storage_row_update_components(benchmark_context *ctx) {
    return benchmark_storage_row_update_loop(ctx, 1);
}

static int benchmark_storage_row_update_loop(benchmark_context *ctx, int components) {
    mylite_storage_index_entryset primary_entries = {
        .size = sizeof(primary_entries),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_statement *transaction = NULL;
    unsigned long long *row_ids = NULL;
    uint64_t row_id_checksum = 0U;
    uint64_t begin_ns = 0U;
    uint64_t mutate_ns = 0U;
    uint64_t commit_ns = 0U;
    int result = 1;

    mylite_storage_result storage_result =
        mylite_storage_read_index_entries(ctx->filename, "perf", "perf_rows", 0U, &primary_entries);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read primary-key index entries: %d\n", storage_result);
        return 1;
    }
    if (primary_entries.entry_count != ctx->config->rows) {
        fprintf(
            stderr,
            "Expected %zu primary entries, got %zu\n",
            ctx->config->rows,
            primary_entries.entry_count
        );
        goto cleanup;
    }

    row_ids = malloc(ctx->config->rows * sizeof(*row_ids));
    if (row_ids == NULL) {
        fprintf(stderr, "Failed to allocate storage update row-id map\n");
        goto cleanup;
    }
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        row_ids[i] = primary_entries.row_ids[i];
    }

    storage_result = mylite_storage_read_indexed_rows(
        ctx->filename,
        "perf",
        "perf_rows",
        primary_entries.row_ids,
        primary_entries.entry_count,
        &rows
    );
    if (storage_result != MYLITE_STORAGE_OK || rows.row_count != primary_entries.entry_count) {
        fprintf(stderr, "Failed to load storage update row payloads: %d\n", storage_result);
        goto cleanup;
    }
    for (size_t i = 0; i < rows.row_count; ++i) {
        if (rows.row_offsets[i] > rows.row_bytes ||
            rows.row_sizes[i] > rows.row_bytes - rows.row_offsets[i]) {
            fprintf(stderr, "Storage update row payload %zu is corrupt\n", i);
            goto cleanup;
        }
    }

    mylite_storage_filename_identity_scope filename_scope = {0};
    mylite_storage_table_name_identity_scope table_scope = {0};
    mylite_storage_begin_filename_identity_scope(ctx->filename, &filename_scope);
    mylite_storage_begin_table_name_identity_scope("perf", "perf_rows", &table_scope);

    storage_result = mylite_storage_begin_transaction(ctx->filename, &transaction);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to begin storage update transaction: %d\n", storage_result);
        goto end_scopes;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t entry_index = i % primary_entries.entry_count;
        const unsigned char *row = rows.rows + rows.row_offsets[entry_index];
        const size_t row_size = rows.row_sizes[entry_index];

        mylite_storage_statement *statement = NULL;
        uint64_t component_start_ns = 0U;
        if (components) {
            component_start_ns = monotonic_ns();
        }
        storage_result = mylite_storage_begin_nested_statement(transaction, &statement);
        if (components) {
            begin_ns += monotonic_ns() - component_start_ns;
        }
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to begin storage update statement: %d\n", storage_result);
            goto rollback;
        }

        unsigned long long new_row_id = 0ULL;
        if (components) {
            component_start_ns = monotonic_ns();
        }
        storage_result = mylite_storage_update_row_preserving_index_entries(
            ctx->filename,
            "perf",
            "perf_rows",
            row_ids[entry_index],
            row,
            row_size,
            &new_row_id
        );
        if (components) {
            mutate_ns += monotonic_ns() - component_start_ns;
        }
        if (storage_result == MYLITE_STORAGE_OK) {
            if (components) {
                component_start_ns = monotonic_ns();
            }
            storage_result = mylite_storage_commit_statement(statement);
            if (components) {
                commit_ns += monotonic_ns() - component_start_ns;
            }
            statement = NULL;
        }
        if (storage_result != MYLITE_STORAGE_OK) {
            if (statement != NULL) {
                (void)mylite_storage_rollback_statement(statement);
            }
            fprintf(stderr, "Storage row update failed: %d\n", storage_result);
            goto rollback;
        }

        row_ids[entry_index] = new_row_id;
        row_id_checksum += new_row_id;
    }

    if (components) {
        if (print_result(
                ctx->config,
                BENCHMARK_METRIC_STORAGE_ROW_UPDATE_COMPONENT_BEGIN,
                "storage row update nested statement begin component",
                ctx->config->iterations,
                begin_ns
            ) != 0) {
            goto rollback;
        }
        if (print_result(
                ctx->config,
                BENCHMARK_METRIC_STORAGE_ROW_UPDATE_COMPONENT_MUTATE,
                "storage row update mutation component",
                ctx->config->iterations,
                mutate_ns
            ) != 0) {
            goto rollback;
        }
        if (print_result(
                ctx->config,
                BENCHMARK_METRIC_STORAGE_ROW_UPDATE_COMPONENT_COMMIT,
                "storage row update nested statement commit component",
                ctx->config->iterations,
                commit_ns
            ) != 0) {
            goto rollback;
        }
    } else {
        if (print_result(
                ctx->config,
                BENCHMARK_METRIC_STORAGE_ROW_UPDATES,
                "storage row updates in one transaction",
                ctx->config->iterations,
                monotonic_ns() - start_ns
            ) != 0) {
            goto rollback;
        }
    }
    printf("Storage row-update row-id checksum: %" PRIu64 "\n", row_id_checksum);

    storage_result = mylite_storage_commit_statement(transaction);
    transaction = NULL;
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to commit storage update transaction: %d\n", storage_result);
        goto end_scopes;
    }
    result = 0;
    goto end_scopes;

rollback:
    if (transaction != NULL) {
        (void)mylite_storage_rollback_statement(transaction);
        transaction = NULL;
    }
end_scopes:
    mylite_storage_end_table_name_identity_scope(&table_scope);
    mylite_storage_end_filename_identity_scope(&filename_scope);
cleanup:
    free(row_ids);
    mylite_storage_free_rowset(&rows);
    mylite_storage_free_index_entryset(&primary_entries);
    return result;
}

static int benchmark_storage_indexed_row_update_components(benchmark_context *ctx) {
    mylite_storage_index_entryset primary_entries = {
        .size = sizeof(primary_entries),
    };
    mylite_storage_index_entryset secondary_entries = {
        .size = sizeof(secondary_entries),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_statement *transaction = NULL;
    unsigned long long *row_ids = NULL;
    size_t *secondary_entry_indexes = NULL;
    size_t *alternate_primary_indexes = NULL;
    unsigned char *secondary_key_is_alternate = NULL;
    uint64_t row_id_checksum = 0U;
    uint64_t begin_ns = 0U;
    uint64_t mutate_ns = 0U;
    uint64_t commit_ns = 0U;
    int result = 1;

    if (ctx->config->rows < 2U) {
        fprintf(stderr, "Storage indexed row update components require at least two rows\n");
        return 1;
    }

    mylite_storage_result storage_result =
        mylite_storage_read_index_entries(ctx->filename, "perf", "perf_rows", 0U, &primary_entries);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read primary-key index entries: %d\n", storage_result);
        return 1;
    }
    storage_result = mylite_storage_read_index_entries(
        ctx->filename,
        "perf",
        "perf_rows",
        1U,
        &secondary_entries
    );
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read secondary index entries: %d\n", storage_result);
        goto cleanup;
    }
    if (primary_entries.entry_count != ctx->config->rows ||
        secondary_entries.entry_count != ctx->config->rows) {
        fprintf(
            stderr,
            "Expected %zu primary and secondary entries, got %zu/%zu\n",
            ctx->config->rows,
            primary_entries.entry_count,
            secondary_entries.entry_count
        );
        goto cleanup;
    }

    row_ids = malloc(ctx->config->rows * sizeof(*row_ids));
    secondary_entry_indexes = malloc(ctx->config->rows * sizeof(*secondary_entry_indexes));
    alternate_primary_indexes = malloc(ctx->config->rows * sizeof(*alternate_primary_indexes));
    secondary_key_is_alternate = calloc(ctx->config->rows, sizeof(*secondary_key_is_alternate));
    if (row_ids == NULL || secondary_entry_indexes == NULL || alternate_primary_indexes == NULL ||
        secondary_key_is_alternate == NULL) {
        fprintf(stderr, "Failed to allocate storage indexed update maps\n");
        goto cleanup;
    }

    for (size_t i = 0; i < ctx->config->rows; ++i) {
        row_ids[i] = primary_entries.row_ids[i];
        secondary_entry_indexes[i] =
            find_storage_entry_index_for_row_id(&secondary_entries, primary_entries.row_ids[i]);
        if (secondary_entry_indexes[i] == SIZE_MAX) {
            fprintf(stderr, "Missing secondary entry for storage row id %llu\n", row_ids[i]);
            goto cleanup;
        }
    }
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        const size_t current_secondary_index = secondary_entry_indexes[i];
        const unsigned char *current_key =
            secondary_entries.keys + secondary_entries.key_offsets[current_secondary_index];
        const size_t current_key_size = secondary_entries.key_sizes[current_secondary_index];
        alternate_primary_indexes[i] = SIZE_MAX;
        for (size_t candidate = 0; candidate < ctx->config->rows; ++candidate) {
            const size_t candidate_secondary_index = secondary_entry_indexes[candidate];
            const unsigned char *candidate_key =
                secondary_entries.keys + secondary_entries.key_offsets[candidate_secondary_index];
            const size_t candidate_key_size =
                secondary_entries.key_sizes[candidate_secondary_index];
            if (candidate_key_size == current_key_size &&
                memcmp(candidate_key, current_key, current_key_size) != 0) {
                alternate_primary_indexes[i] = candidate;
                break;
            }
        }
        if (alternate_primary_indexes[i] == SIZE_MAX) {
            fprintf(stderr, "Storage indexed update component phase needs distinct keys\n");
            goto cleanup;
        }
    }

    storage_result = mylite_storage_read_indexed_rows(
        ctx->filename,
        "perf",
        "perf_rows",
        primary_entries.row_ids,
        primary_entries.entry_count,
        &rows
    );
    if (storage_result != MYLITE_STORAGE_OK || rows.row_count != primary_entries.entry_count) {
        fprintf(stderr, "Failed to load storage indexed update row payloads: %d\n", storage_result);
        goto cleanup;
    }
    for (size_t i = 0; i < rows.row_count; ++i) {
        if (rows.row_offsets[i] > rows.row_bytes ||
            rows.row_sizes[i] > rows.row_bytes - rows.row_offsets[i]) {
            fprintf(stderr, "Storage indexed update row payload %zu is corrupt\n", i);
            goto cleanup;
        }
    }

    mylite_storage_filename_identity_scope filename_scope = {0};
    mylite_storage_table_name_identity_scope table_scope = {0};
    mylite_storage_begin_filename_identity_scope(ctx->filename, &filename_scope);
    mylite_storage_begin_table_name_identity_scope("perf", "perf_rows", &table_scope);

    storage_result = mylite_storage_begin_transaction(ctx->filename, &transaction);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to begin storage indexed update transaction: %d\n", storage_result);
        goto end_scopes;
    }

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t entry_index = i % primary_entries.entry_count;
        const size_t target_primary_index = secondary_key_is_alternate[entry_index]
                                                ? entry_index
                                                : alternate_primary_indexes[entry_index];
        const size_t target_secondary_index = secondary_entry_indexes[target_primary_index];
        const unsigned char *row = rows.rows + rows.row_offsets[entry_index];
        const size_t row_size = rows.row_sizes[entry_index];
        const mylite_storage_index_entry index_entries[] = {
            {
                .size = sizeof(index_entries[0]),
                .index_number = 0U,
                .key = primary_entries.keys + primary_entries.key_offsets[entry_index],
                .key_size = primary_entries.key_sizes[entry_index],
            },
            {
                .size = sizeof(index_entries[1]),
                .index_number = 1U,
                .key =
                    secondary_entries.keys + secondary_entries.key_offsets[target_secondary_index],
                .key_size = secondary_entries.key_sizes[target_secondary_index],
            },
        };
        const unsigned char index_entry_changed[] = {0U, 1U};

        mylite_storage_statement *statement = NULL;
        uint64_t component_start_ns = monotonic_ns();
        storage_result = mylite_storage_begin_nested_statement(transaction, &statement);
        begin_ns += monotonic_ns() - component_start_ns;
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(
                stderr,
                "Failed to begin storage indexed update statement: %d\n",
                storage_result
            );
            goto rollback;
        }

        unsigned long long new_row_id = 0ULL;
        component_start_ns = monotonic_ns();
        storage_result = mylite_storage_update_row_with_index_entry_changes(
            ctx->filename,
            "perf",
            "perf_rows",
            row_ids[entry_index],
            row,
            row_size,
            index_entries,
            sizeof(index_entries) / sizeof(index_entries[0]),
            index_entry_changed,
            &new_row_id
        );
        mutate_ns += monotonic_ns() - component_start_ns;
        if (storage_result == MYLITE_STORAGE_OK) {
            component_start_ns = monotonic_ns();
            storage_result = mylite_storage_commit_statement(statement);
            commit_ns += monotonic_ns() - component_start_ns;
            statement = NULL;
        }
        if (storage_result != MYLITE_STORAGE_OK) {
            if (statement != NULL) {
                (void)mylite_storage_rollback_statement(statement);
            }
            fprintf(stderr, "Storage indexed row update failed: %d\n", storage_result);
            goto rollback;
        }

        row_ids[entry_index] = new_row_id;
        secondary_key_is_alternate[entry_index] = !secondary_key_is_alternate[entry_index];
        row_id_checksum += new_row_id;
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_STORAGE_INDEXED_ROW_UPDATE_COMPONENT_BEGIN,
            "storage indexed row update nested statement begin component",
            ctx->config->iterations,
            begin_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_STORAGE_INDEXED_ROW_UPDATE_COMPONENT_MUTATE,
            "storage indexed row update mutation component",
            ctx->config->iterations,
            mutate_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_STORAGE_INDEXED_ROW_UPDATE_COMPONENT_COMMIT,
            "storage indexed row update nested statement commit component",
            ctx->config->iterations,
            commit_ns
        ) != 0) {
        goto rollback;
    }
    printf("Storage indexed row-update row-id checksum: %" PRIu64 "\n", row_id_checksum);

    storage_result = mylite_storage_commit_statement(transaction);
    transaction = NULL;
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(
            stderr,
            "Failed to commit storage indexed update transaction: %d\n",
            storage_result
        );
        goto end_scopes;
    }
    result = 0;
    goto end_scopes;

rollback:
    if (transaction != NULL) {
        (void)mylite_storage_rollback_statement(transaction);
        transaction = NULL;
    }
end_scopes:
    mylite_storage_end_table_name_identity_scope(&table_scope);
    mylite_storage_end_filename_identity_scope(&filename_scope);
cleanup:
    free(secondary_key_is_alternate);
    free(alternate_primary_indexes);
    free(secondary_entry_indexes);
    free(row_ids);
    mylite_storage_free_rowset(&rows);
    mylite_storage_free_index_entryset(&secondary_entries);
    mylite_storage_free_index_entryset(&primary_entries);
    return result;
}

static size_t find_storage_entry_index_for_row_id(
    const mylite_storage_index_entryset *entries,
    unsigned long long row_id
) {
    for (size_t i = 0; i < entries->entry_count; ++i) {
        if (entries->row_ids[i] == row_id) {
            return i;
        }
    }
    return SIZE_MAX;
}

static int benchmark_secondary_selects(benchmark_context *ctx) {
    return benchmark_secondary_selects_for_index(
        ctx,
        "perf_rows",
        "value_key",
        BENCHMARK_METRIC_DIRECT_SECONDARY_SELECTS,
        "direct secondary-index exact selects",
        "Secondary exact-select rows",
        "Secondary exact-select checksum"
    );
}

static int benchmark_prepared_secondary_selects(benchmark_context *ctx) {
    return benchmark_prepared_secondary_selects_for_index(
        ctx,
        "perf_rows",
        "value_key",
        BENCHMARK_METRIC_PREPARED_SECONDARY_SELECTS,
        "prepared secondary-index exact selects",
        "Prepared secondary exact-select rows",
        "Prepared secondary exact-select checksum"
    );
}

static int publish_secondary_leaf_index(benchmark_context *ctx) {
    if (prepare_secondary_leaf_table(ctx) != 0) {
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    if (exec_sql(ctx, "CREATE INDEX value_leaf_key ON perf_leaf_rows (value) ALGORITHM=COPY") !=
        0) {
        return 1;
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PUBLISH_LEAF_INDEX,
            "publish secondary index root",
            1U,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
    return verify_secondary_leaf_index_root(ctx);
}

static int benchmark_leaf_secondary_selects(benchmark_context *ctx) {
    if (!ctx->published_leaf_secondary_index) {
        printf("Published secondary exact selects: skipped; no published root\n");
        return 0;
    }

    return benchmark_secondary_selects_for_index(
        ctx,
        "perf_leaf_rows",
        "value_leaf_key",
        BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_SELECTS,
        "direct published-root secondary-index exact selects",
        "Published secondary exact-select rows",
        "Published secondary exact-select checksum"
    );
}

static int benchmark_prepared_leaf_secondary_selects(benchmark_context *ctx) {
    if (!ctx->published_leaf_secondary_index) {
        printf("Prepared published secondary exact selects: skipped; no published root\n");
        return 0;
    }

    return benchmark_prepared_secondary_selects_for_index(
        ctx,
        "perf_leaf_rows",
        "value_leaf_key",
        BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_SELECTS,
        "prepared published-root secondary-index exact selects",
        "Prepared published secondary exact-select rows",
        "Prepared published secondary exact-select checksum"
    );
}

static int benchmark_leaf_secondary_range_limit_selects(
    benchmark_context *ctx,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
) {
    if (!ctx->published_leaf_secondary_index) {
        printf("%s: skipped; no published root\n", operation);
        return 0;
    }

    size_t total_rows = 0U;
    uint64_t checksum = 0U;
    const uint64_t start_ns = monotonic_ns();

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t value = secondary_value_for_iteration(ctx, i);
        char sql[160];
        secondary_result result = {
            .rows = 0U,
            .checksum = 0U,
        };
        const uint64_t expected_checksum = (uint64_t)value + (uint64_t)value;
        const int written = snprintf(
            sql,
            sizeof(sql),
            "SELECT id, value FROM perf_leaf_rows FORCE INDEX (value_leaf_key) "
            "WHERE value >= %zu ORDER BY value, id LIMIT 1",
            value
        );
        if (written < 0 || (size_t)written >= sizeof(sql)) {
            return 1;
        }
        if (mylite_exec(ctx->db, sql, secondary_callback, &result, NULL) != MYLITE_OK) {
            report_database_error(ctx, operation);
            return 1;
        }
        if (result.rows != 1U || result.checksum != expected_checksum) {
            fprintf(
                stderr,
                "%s for value %zu returned "
                "%zu rows/%" PRIu64 " checksum; expected 1/%" PRIu64 "\n",
                operation,
                value,
                result.rows,
                result.checksum,
                expected_checksum
            );
            return 1;
        }
        total_rows += result.rows;
        checksum += result.checksum;
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
    printf("%s: %zu\n", rows_label, total_rows);
    printf("%s: %" PRIu64 "\n", checksum_label, checksum);
    return 0;
}

static int benchmark_prepared_leaf_secondary_range_limit_selects(
    benchmark_context *ctx,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
) {
    if (!ctx->published_leaf_secondary_index) {
        printf("%s: skipped; no published root\n", operation);
        return 0;
    }

    mylite_stmt *stmt = NULL;
    size_t total_rows = 0U;
    uint64_t checksum = 0U;
    int result = 1;

    if (mylite_prepare(
            ctx->db,
            "SELECT id, value FROM perf_leaf_rows FORCE INDEX (value_leaf_key) "
            "WHERE value >= ? ORDER BY value, id LIMIT 1",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, operation);
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t value = secondary_value_for_iteration(ctx, i);
        const uint64_t expected_checksum = (uint64_t)value + (uint64_t)value;
        if (mylite_bind_int64(stmt, 1U, (long long)value) != MYLITE_OK) {
            report_database_error(ctx, "prepared published-root secondary range LIMIT selects");
            goto cleanup;
        }

        int step_result = mylite_step(stmt);
        if (step_result != MYLITE_ROW) {
            fprintf(stderr, "%s returned no row for value %zu\n", operation, value);
            report_database_error(ctx, operation);
            goto cleanup;
        }
        if (mylite_column_type(stmt, 0U) != MYLITE_TYPE_INT64 ||
            mylite_column_type(stmt, 1U) != MYLITE_TYPE_INT64) {
            fprintf(stderr, "%s returned non-integers\n", operation);
            goto cleanup;
        }
        const uint64_t row_checksum =
            (uint64_t)mylite_column_int64(stmt, 0U) + (uint64_t)mylite_column_int64(stmt, 1U);
        if (row_checksum != expected_checksum) {
            fprintf(
                stderr,
                "%s for value %zu returned "
                "checksum %" PRIu64 "; expected %" PRIu64 "\n",
                operation,
                value,
                row_checksum,
                expected_checksum
            );
            goto cleanup;
        }
        step_result = mylite_step(stmt);
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "%s returned extra rows\n", operation);
            goto cleanup;
        }
        ++total_rows;
        checksum += row_checksum;
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, operation);
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }
    printf("%s: %zu\n", rows_label, total_rows);
    printf("%s: %" PRIu64 "\n", checksum_label, checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, operation);
        return 1;
    }
    return result;
}

static int benchmark_secondary_selects_for_index(
    benchmark_context *ctx,
    const char *table_name,
    const char *index_name,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
) {
    size_t total_rows = 0U;
    uint64_t checksum = 0U;
    const uint64_t start_ns = monotonic_ns();

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t value = secondary_value_for_iteration(ctx, i);
        char sql[128];
        secondary_result result = {
            .rows = 0U,
            .checksum = 0U,
        };
        size_t expected_rows = 0U;
        const uint64_t expected_checksum = secondary_expected_checksum(ctx, value, &expected_rows);
        const int written = snprintf(
            sql,
            sizeof(sql),
            "SELECT id, value FROM %s FORCE INDEX (%s) "
            "WHERE value = %zu ORDER BY id",
            table_name,
            index_name,
            value
        );
        if (written < 0 || (size_t)written >= sizeof(sql)) {
            return 1;
        }
        if (mylite_exec(ctx->db, sql, secondary_callback, &result, NULL) != MYLITE_OK) {
            report_database_error(ctx, operation);
            return 1;
        }
        if (result.rows != expected_rows || result.checksum != expected_checksum) {
            fprintf(
                stderr,
                "%s for value %zu returned %zu rows/%" PRIu64 " checksum; expected %zu/%" PRIu64
                "\n",
                operation,
                value,
                result.rows,
                result.checksum,
                expected_rows,
                expected_checksum
            );
            return 1;
        }
        total_rows += result.rows;
        checksum += result.checksum;
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
    printf("%s: %zu\n", rows_label, total_rows);
    printf("%s: %" PRIu64 "\n", checksum_label, checksum);
    return 0;
}

static int benchmark_prepared_secondary_selects_for_index(
    benchmark_context *ctx,
    const char *table_name,
    const char *index_name,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
) {
    mylite_stmt *stmt = NULL;
    size_t total_rows = 0U;
    uint64_t checksum = 0U;
    int result = 1;
    char sql[160];
    const int written = snprintf(
        sql,
        sizeof(sql),
        "SELECT id, value FROM %s FORCE INDEX (%s) "
        "WHERE value = ? ORDER BY id",
        table_name,
        index_name
    );
    if (written < 0 || (size_t)written >= sizeof(sql)) {
        return 1;
    }

    if (mylite_prepare(ctx->db, sql, MYLITE_NUL_TERMINATED, &stmt, NULL) != MYLITE_OK) {
        report_database_error(ctx, operation);
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t value = secondary_value_for_iteration(ctx, i);
        secondary_result selected = {
            .rows = 0U,
            .checksum = 0U,
        };
        size_t expected_rows = 0U;
        const uint64_t expected_checksum = secondary_expected_checksum(ctx, value, &expected_rows);
        if (mylite_bind_int64(stmt, 1U, (long long)value) != MYLITE_OK) {
            report_database_error(ctx, operation);
            goto cleanup;
        }

        for (;;) {
            const int step_result = mylite_step(stmt);
            if (step_result == MYLITE_DONE) {
                break;
            }
            if (step_result != MYLITE_ROW) {
                fprintf(stderr, "%s failed for value %zu\n", operation, value);
                report_database_error(ctx, operation);
                goto cleanup;
            }
            if (mylite_column_type(stmt, 0U) != MYLITE_TYPE_INT64 ||
                mylite_column_type(stmt, 1U) != MYLITE_TYPE_INT64) {
                fprintf(stderr, "%s returned non-integer values\n", operation);
                goto cleanup;
            }
            selected.checksum +=
                (uint64_t)mylite_column_int64(stmt, 0U) + (uint64_t)mylite_column_int64(stmt, 1U);
            ++selected.rows;
        }

        if (selected.rows != expected_rows || selected.checksum != expected_checksum) {
            fprintf(
                stderr,
                "%s for value %zu returned %zu rows/%" PRIu64 " checksum; expected %zu/%" PRIu64
                "\n",
                operation,
                value,
                selected.rows,
                selected.checksum,
                expected_rows,
                expected_checksum
            );
            goto cleanup;
        }
        total_rows += selected.rows;
        checksum += selected.checksum;
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, operation);
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }
    printf("%s: %zu\n", rows_label, total_rows);
    printf("%s: %" PRIu64 "\n", checksum_label, checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, operation);
        return 1;
    }
    return result;
}

static int prepare_secondary_leaf_table(benchmark_context *ctx) {
    uint64_t start_ns;
    int result = 1;

    if (exec_sql(
            ctx,
            "CREATE TABLE perf_leaf_rows ("
            "id INT NOT NULL PRIMARY KEY,"
            "value INT NOT NULL,"
            "pad VARCHAR(64) NOT NULL"
            ") ENGINE=InnoDB"
        ) != 0) {
        return 1;
    }
    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }

    start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        char sql[176];
        const int written = snprintf(
            sql,
            sizeof(sql),
            "INSERT INTO perf_leaf_rows (id, value, pad) VALUES (%zu, %zu, 'row-%zu')",
            i + 1U,
            secondary_value_for_row(ctx, i + 1U),
            i + 1U
        );
        if (written < 0 || (size_t)written >= sizeof(sql) || exec_sql(ctx, sql) != 0) {
            goto rollback;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARE_LEAF_ROWS,
            "prepare secondary leaf benchmark rows",
            ctx->config->rows,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int append_secondary_leaf_tail_rows(benchmark_context *ctx) {
    const size_t tail_rows = secondary_tail_row_count(ctx);
    const size_t bucket_count = secondary_bucket_count(ctx);
    uint64_t start_ns;
    int result = 1;

    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }

    start_ns = monotonic_ns();
    for (size_t i = 0; i < tail_rows; ++i) {
        char sql[184];
        const size_t id = ctx->config->rows + i + 1U;
        const size_t value = bucket_count + (i % bucket_count) + 1U;
        const int written = snprintf(
            sql,
            sizeof(sql),
            "INSERT INTO perf_leaf_rows (id, value, pad) VALUES (%zu, %zu, 'tail-%zu')",
            id,
            value,
            id
        );
        if (written < 0 || (size_t)written >= sizeof(sql) || exec_sql(ctx, sql) != 0) {
            goto rollback;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARE_LEAF_TAIL_ROWS,
            "append secondary leaf benchmark tail rows",
            tail_rows,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    printf("Published secondary tail rows: %zu\n", tail_rows);
    result = 0;

rollback:
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int verify_secondary_leaf_index_root(benchmark_context *ctx) {
    enum { value_leaf_key_index_number = 1U };

    mylite_storage_index_root_metadata metadata = {
        .size = sizeof(metadata),
    };
    const mylite_storage_result result = mylite_storage_read_index_root(
        ctx->filename,
        "perf",
        "perf_leaf_rows",
        value_leaf_key_index_number,
        &metadata
    );

    if (result == MYLITE_STORAGE_NOTFOUND) {
        printf("Published secondary root: skipped; no published root\n");
        return 0;
    }
    if (result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read published secondary root metadata: %d\n", result);
        return 1;
    }
    if (metadata.entry_count != (unsigned long long)ctx->config->rows) {
        fprintf(
            stderr,
            "Published secondary root has %llu entries; expected %zu\n",
            metadata.entry_count,
            ctx->config->rows
        );
        return 1;
    }

    ctx->published_leaf_secondary_index = 1;
    printf(
        "Published secondary root: table perf_leaf_rows index %u, entries %llu\n",
        value_leaf_key_index_number,
        metadata.entry_count
    );
    return 0;
}

static size_t secondary_value_for_row(benchmark_context *ctx, size_t row_number) {
    return ((row_number - 1U) % secondary_bucket_count(ctx)) + 1U;
}

static size_t secondary_value_for_iteration(benchmark_context *ctx, size_t iteration) {
    return (iteration % secondary_bucket_count(ctx)) + 1U;
}

static size_t secondary_tail_row_count(benchmark_context *ctx) {
    const size_t tenth = ctx->config->rows / 10U;
    return tenth == 0U ? 1U : tenth;
}

static uint64_t secondary_expected_checksum(
    benchmark_context *ctx,
    size_t value,
    size_t *out_rows
) {
    const size_t bucket_count = secondary_bucket_count(ctx);
    size_t rows = 0U;
    uint64_t checksum = 0U;
    if (value <= bucket_count && value <= ctx->config->rows) {
        const size_t first_id = value;
        const size_t last_id = value + ((ctx->config->rows - value) / bucket_count) * bucket_count;
        rows = ((last_id - first_id) / bucket_count) + 1U;
        checksum = ((uint64_t)rows * ((uint64_t)first_id + (uint64_t)last_id)) / 2U +
                   (uint64_t)rows * (uint64_t)value;
    }
    *out_rows = rows;
    return checksum;
}

static size_t secondary_bucket_count(benchmark_context *ctx) {
    return ctx->config->rows < 10U ? 1U : 10U;
}

static int benchmark_updates(benchmark_context *ctx) {
    const uint64_t start_ns = monotonic_ns();
    int result = 1;

    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        char sql[96];
        const int written =
            snprintf(sql, sizeof(sql), "UPDATE perf_rows SET value = value + 1 WHERE id = %zu", id);
        if (written < 0 || (size_t)written >= sizeof(sql) || exec_sql(ctx, sql) != 0) {
            goto rollback;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_DIRECT_UPDATES,
            "direct primary-key updates in one transaction",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_prepared_updates(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    int result = 1;

    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "UPDATE perf_rows SET value = value + 1 WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare primary-key update");
        goto rollback;
    }

    reset_prepared_update_storage_counters();
    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        if (mylite_bind_int64(stmt, 1U, (long long)id) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key update");
            goto rollback;
        }

        const int step_result = mylite_step(stmt);
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared primary-key update failed for id %zu\n", id);
            report_database_error(ctx, "prepared primary-key update");
            goto rollback;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key update");
            goto rollback;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_UPDATES,
            "prepared primary-key updates in one transaction",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }
    print_prepared_update_storage_counters();

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize prepared primary-key update");
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key update");
        result = 1;
    }
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_prepared_update_components(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t bind_ns = 0U;
    uint64_t step_ns = 0U;
    uint64_t reset_ns = 0U;
    int result = 1;

    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "UPDATE perf_rows SET value = value + 1 WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare primary-key update components");
        goto rollback;
    }

    reset_prepared_update_storage_counters();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        uint64_t start_ns = monotonic_ns();
        const int bind_result = mylite_bind_int64(stmt, 1U, (long long)id);
        bind_ns += monotonic_ns() - start_ns;
        if (bind_result != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key update component");
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int step_result = mylite_step(stmt);
        step_ns += monotonic_ns() - start_ns;
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared primary-key update failed for id %zu\n", id);
            report_database_error(ctx, "prepared primary-key update component");
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int reset_result = mylite_reset(stmt);
        reset_ns += monotonic_ns() - start_ns;
        if (reset_result != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key update component");
            goto rollback;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_BIND,
            "prepared primary-key update bind component",
            ctx->config->iterations,
            bind_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_STEP,
            "prepared primary-key update step component",
            ctx->config->iterations,
            step_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_RESET,
            "prepared primary-key update reset component",
            ctx->config->iterations,
            reset_ns
        ) != 0) {
        goto rollback;
    }
    print_prepared_update_storage_counters();

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize prepared primary-key update components");
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key update components");
        result = 1;
    }
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_prepared_assignment_update_components(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t bind_ns = 0U;
    uint64_t step_ns = 0U;
    uint64_t reset_ns = 0U;
    int result = 1;

    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "UPDATE perf_rows SET value = ? WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare prepared assignment update components");
        goto rollback;
    }

    reset_prepared_update_storage_counters();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        const long long value = (long long)(ctx->config->rows + i + 1U);
        uint64_t start_ns = monotonic_ns();
        int bind_result = mylite_bind_int64(stmt, 1U, value);
        if (bind_result == MYLITE_OK) {
            bind_result = mylite_bind_int64(stmt, 2U, (long long)id);
        }
        bind_ns += monotonic_ns() - start_ns;
        if (bind_result != MYLITE_OK) {
            report_database_error(ctx, "bind prepared assignment update component");
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int step_result = mylite_step(stmt);
        step_ns += monotonic_ns() - start_ns;
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared assignment update failed for id %zu\n", id);
            report_database_error(ctx, "prepared assignment update component");
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int reset_result = mylite_reset(stmt);
        reset_ns += monotonic_ns() - start_ns;
        if (reset_result != MYLITE_OK) {
            report_database_error(ctx, "reset prepared assignment update component");
            goto rollback;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_ASSIGNMENT_UPDATE_COMPONENT_BIND,
            "prepared assignment update bind component",
            ctx->config->iterations,
            bind_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_ASSIGNMENT_UPDATE_COMPONENT_STEP,
            "prepared assignment update step component",
            ctx->config->iterations,
            step_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_ASSIGNMENT_UPDATE_COMPONENT_RESET,
            "prepared assignment update reset component",
            ctx->config->iterations,
            reset_ns
        ) != 0) {
        goto rollback;
    }
    print_prepared_update_storage_counters();

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize prepared assignment update components");
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared assignment update components");
        result = 1;
    }
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_prepared_row_only_update_components(benchmark_context *ctx) {
    return benchmark_prepared_row_only_update_components_for_mode(
        ctx,
        PREPARED_ROW_ONLY_UPDATE_HIT
    );
}

static int benchmark_prepared_row_only_update_miss_components(benchmark_context *ctx) {
    return benchmark_prepared_row_only_update_components_for_mode(
        ctx,
        PREPARED_ROW_ONLY_UPDATE_MISS
    );
}

static int benchmark_prepared_row_only_update_components_for_mode(
    benchmark_context *ctx,
    prepared_row_only_update_mode mode
) {
    mylite_stmt *stmt = NULL;
    uint64_t bind_ns = 0U;
    uint64_t step_ns = 0U;
    uint64_t reset_ns = 0U;
    int result = 1;
    const int no_match = mode == PREPARED_ROW_ONLY_UPDATE_MISS;
    const benchmark_metric bind_metric =
        no_match ? BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENT_BIND
                 : BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_COMPONENT_BIND;
    const benchmark_metric step_metric =
        no_match ? BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENT_STEP
                 : BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_COMPONENT_STEP;
    const benchmark_metric reset_metric =
        no_match ? BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_MISS_COMPONENT_RESET
                 : BENCHMARK_METRIC_PREPARED_ROW_ONLY_UPDATE_COMPONENT_RESET;
    const char *prepare_operation = no_match ? "prepare prepared row-only update miss components"
                                             : "prepare prepared row-only update components";
    const char *bind_operation = no_match ? "bind prepared row-only update miss component"
                                          : "bind prepared row-only update component";
    const char *step_operation =
        no_match ? "prepared row-only update miss component" : "prepared row-only update component";
    const char *reset_operation = no_match ? "reset prepared row-only update miss component"
                                           : "reset prepared row-only update component";
    const char *finalize_operation = no_match ? "finalize prepared row-only update miss components"
                                              : "finalize prepared row-only update components";
    const char *bind_result_operation = no_match ? "prepared row-only update miss bind component"
                                                 : "prepared row-only update bind component";
    const char *step_result_operation = no_match ? "prepared row-only update miss step component"
                                                 : "prepared row-only update step component";
    const char *reset_result_operation = no_match ? "prepared row-only update miss reset component"
                                                  : "prepared row-only update reset component";
    const char *checksum_operation =
        no_match ? "Row-only update miss checksum" : "Row-only update checksum";
    const unsigned long long expected_checksum =
        no_match ? 0ULL : (unsigned long long)ctx->config->iterations;

    if (prepare_row_only_update_rows(ctx) != 0) {
        return 1;
    }
    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "UPDATE perf_row_only_rows SET counter = counter + 1 WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, prepare_operation);
        goto rollback;
    }

    reset_prepared_update_storage_counters();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = no_match ? ctx->config->rows + (i % ctx->config->rows) + 1U
                                   : (i % ctx->config->rows) + 1U;

        uint64_t start_ns = monotonic_ns();
        const int bind_result = mylite_bind_int64(stmt, 1U, (long long)id);
        bind_ns += monotonic_ns() - start_ns;
        if (bind_result != MYLITE_OK) {
            report_database_error(ctx, bind_operation);
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int step_result = mylite_step(stmt);
        step_ns += monotonic_ns() - start_ns;
        if (step_result != MYLITE_DONE) {
            fprintf(
                stderr,
                "Prepared row-only update%s failed for id %zu\n",
                no_match ? " miss" : "",
                id
            );
            report_database_error(ctx, step_operation);
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int reset_result = mylite_reset(stmt);
        reset_ns += monotonic_ns() - start_ns;
        if (reset_result != MYLITE_OK) {
            report_database_error(ctx, reset_operation);
            goto rollback;
        }
    }

    if (print_result(
            ctx->config,
            bind_metric,
            bind_result_operation,
            ctx->config->iterations,
            bind_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            step_metric,
            step_result_operation,
            ctx->config->iterations,
            step_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            reset_metric,
            reset_result_operation,
            ctx->config->iterations,
            reset_ns
        ) != 0) {
        goto rollback;
    }
    print_prepared_update_storage_counters();

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, finalize_operation);
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    unsigned long long checksum = 0U;
    if (query_uint64(ctx, "SELECT SUM(counter) FROM perf_row_only_rows", &checksum) != 0) {
        return 1;
    }
    printf("%s: %llu\n", checksum_operation, checksum);
    if (checksum != expected_checksum) {
        fprintf(
            stderr,
            "Expected %s %llu, got %llu\n",
            checksum_operation,
            expected_checksum,
            checksum
        );
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, finalize_operation);
        result = 1;
    }
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int prepare_row_only_update_rows(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    int result = 1;

    if (exec_sql(
            ctx,
            "CREATE TABLE perf_row_only_rows ("
            "id INT NOT NULL PRIMARY KEY,"
            "counter INT NOT NULL,"
            "pad VARCHAR(64) NOT NULL"
            ") ENGINE=InnoDB"
        ) != 0) {
        return 1;
    }
    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "INSERT INTO perf_row_only_rows (id, counter, pad) VALUES (?, 0, ?)",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare row-only update rows");
        goto rollback;
    }

    for (size_t i = 0; i < ctx->config->rows; ++i) {
        char pad[32];
        const size_t row_id = i + 1U;
        const int written = snprintf(pad, sizeof(pad), "row-%zu", row_id);
        if (written < 0 || (size_t)written >= sizeof(pad)) {
            goto rollback;
        }
        if (mylite_bind_int64(stmt, 1U, (long long)row_id) != MYLITE_OK ||
            mylite_bind_text(stmt, 2U, pad, MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT) != MYLITE_OK) {
            report_database_error(ctx, "bind row-only update row");
            goto rollback;
        }

        const int step_result = mylite_step(stmt);
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Row-only update row insert failed for id %zu\n", row_id);
            report_database_error(ctx, "row-only update row insert");
            goto rollback;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset row-only update row insert");
            goto rollback;
        }
    }

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize row-only update rows");
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize row-only update rows");
        result = 1;
    }
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_ordered_scan(benchmark_context *ctx) {
    scan_result scan = {
        .rows = 0U,
        .checksum = 0U,
    };
    const uint64_t start_ns = monotonic_ns();

    if (mylite_exec(
            ctx->db,
            "SELECT id, value, pad FROM perf_rows ORDER BY id",
            scan_callback,
            &scan,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "ordered scan");
        return 1;
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_ORDERED_SCAN,
            "direct ordered full scan",
            scan.rows,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
    printf("Scan checksum: %" PRIu64 "\n", scan.checksum);
    if (scan.rows != ctx->config->rows) {
        fprintf(stderr, "Expected %zu scan rows, got %zu\n", ctx->config->rows, scan.rows);
        return 1;
    }
    return 0;
}

static int verify_row_count(benchmark_context *ctx, size_t expected) {
    unsigned long long count = 0U;
    if (query_uint64(ctx, "SELECT COUNT(*) FROM perf_rows", &count) != 0) {
        return 1;
    }
    if (count != (unsigned long long)expected) {
        fprintf(stderr, "Expected %zu rows, got %llu\n", expected, count);
        return 1;
    }
    return 0;
}

static int exec_sql(benchmark_context *ctx, const char *sql) {
    const int result = mylite_exec(ctx->db, sql, NULL, NULL, NULL);
    if (result != MYLITE_OK) {
        report_database_error(ctx, sql);
        return 1;
    }
    return 0;
}

static int query_uint64(benchmark_context *ctx, const char *sql, unsigned long long *out_value) {
    scalar_result result = {
        .value = 0U,
        .rows = 0,
    };

    if (mylite_exec(ctx->db, sql, scalar_callback, &result, NULL) != MYLITE_OK) {
        report_database_error(ctx, sql);
        return 1;
    }
    if (result.rows != 1) {
        fprintf(stderr, "Expected one scalar row from %s, got %d\n", sql, result.rows);
        return 1;
    }
    *out_value = result.value;
    return 0;
}

static int scalar_callback(void *data, int column_count, char **values, char **column_names) {
    (void)column_names;
    scalar_result *result = data;
    if (column_count != 1 || values[0] == NULL) {
        return 1;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(values[0], &end, 10);
    if (errno != 0 || end == values[0] || *end != '\0') {
        return 1;
    }
    result->value = value;
    ++result->rows;
    return 0;
}

static int secondary_callback(void *data, int column_count, char **values, char **column_names) {
    (void)column_names;
    secondary_result *result = data;
    if (column_count != 2 || values[0] == NULL || values[1] == NULL) {
        return 1;
    }

    for (int i = 0; i < 2; ++i) {
        char *end = NULL;
        errno = 0;
        const unsigned long long value = strtoull(values[i], &end, 10);
        if (errno != 0 || end == values[i] || *end != '\0') {
            return 1;
        }
        result->checksum += (uint64_t)value;
    }
    ++result->rows;
    return 0;
}

static int scan_callback(void *data, int column_count, char **values, char **column_names) {
    (void)column_names;
    scan_result *result = data;
    if (column_count != 3 || values[0] == NULL || values[1] == NULL || values[2] == NULL) {
        return 1;
    }

    for (int i = 0; i < 2; ++i) {
        char *end = NULL;
        errno = 0;
        const unsigned long long value = strtoull(values[i], &end, 10);
        if (errno != 0 || end == values[i] || *end != '\0') {
            return 1;
        }
        result->checksum += (uint64_t)value;
    }
    result->checksum += (uint64_t)strlen(values[2]);
    ++result->rows;
    return 0;
}

static void report_database_error(benchmark_context *ctx, const char *operation) {
    if (ctx->db == NULL) {
        fprintf(stderr, "%s failed before database handle was available\n", operation);
        return;
    }
    fprintf(stderr, "%s failed: %s\n", operation, mylite_errmsg(ctx->db));
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-perf-baseline.XXXXXX";
    char *root = mkdtemp(template_path);
    if (root == NULL) {
        fprintf(stderr, "Failed to create temporary directory: %s\n", strerror(errno));
        return NULL;
    }

    char *copy = malloc(strlen(root) + 1U);
    if (copy == NULL) {
        fprintf(stderr, "Out of memory\n");
        remove_tree(root);
        return NULL;
    }
    strcpy(copy, root);
    return copy;
}

static char *path_join(const char *directory, const char *name) {
    const size_t directory_len = strlen(directory);
    const size_t name_len = strlen(name);
    char *path = malloc(directory_len + 1U + name_len + 1U);
    if (path == NULL) {
        fprintf(stderr, "Out of memory\n");
        return NULL;
    }

    memcpy(path, directory, directory_len);
    path[directory_len] = '/';
    memcpy(path + directory_len + 1U, name, name_len + 1U);
    return path;
}

static void remove_tree(const char *path) {
    if (path == NULL) {
        return;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        unlink(path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *child = path_join(path, entry->d_name);
        if (child != NULL) {
            remove_tree_entry(child);
            free(child);
        }
    }
    closedir(dir);
    rmdir(path);
}

static void remove_tree_entry(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        remove_tree(path);
        return;
    }
    unlink(path);
}

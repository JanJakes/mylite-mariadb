#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_WAIT_POLL_INTERVAL_US 10000
#define MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO 1205U
#define MYLITE_TEST_DEADLOCK_ERRNO 1213U
#define MYLITE_TEST_DUPLICATE_KEY_ERRNO 1062U
#define MYLITE_TEST_TRIGGER_ALREADY_EXISTS_ERRNO 1359U
#define MYLITE_TEST_VIEW_CHECK_FAILED_ERRNO 1369U
#define MYLITE_TEST_ROW_IS_REFERENCED_ERRNO 1451U
#define MYLITE_TEST_NO_REFERENCED_ROW_ERRNO 1452U
#define MYLITE_TEST_WRONG_FK_OPTION_FOR_GENERATED_COLUMN_ERRNO 1905U
#define MYLITE_TEST_CHECK_CONSTRAINT_ERRNO 4025U
#define MYLITE_TEST_CHILD_OK 0
#define MYLITE_TEST_CHILD_OPEN_FAILED 2
#define MYLITE_TEST_CHILD_EXEC_FAILED 3
#define MYLITE_TEST_CHILD_DEADLOCK 4
#define MYLITE_TEST_CHILD_LOCK_WAIT_TIMEOUT 5
#define MYLITE_TEST_CHILD_CLOSE_FAILED 6
#define MYLITE_TEST_CHILD_EXPECTED_ERROR 7
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TABLE_OFFSET 56
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_COUNT_OFFSET 60
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_DESCRIPTOR_SIZE 32
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TYPE_OFFSET 0
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_DATA_OFFSET 8
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SEGMENT_TYPE 6U
#define MYLITE_TEST_CONCURRENCY_PAGE_WRITE_LOCK_SEGMENT_TYPE 10U
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET 64
#define MYLITE_TEST_CONCURRENCY_REDO_STATE_SEGMENT_TYPE 7U
#define MYLITE_TEST_CONCURRENCY_REDO_STATE_LATEST_LSN_OFFSET 32
#define MYLITE_TEST_CONCURRENCY_REDO_STATE_VISIBLE_LSN_OFFSET 40
#define MYLITE_TEST_CONCURRENCY_REDO_STATE_WRITTEN_LSN_OFFSET 72
#define MYLITE_TEST_CONCURRENCY_PAGE_INDEX_SEGMENT_TYPE 8U
#define MYLITE_TEST_CONCURRENCY_PAGE_INDEX_ACTIVE_COUNT_OFFSET 40
#define MYLITE_TEST_CONCURRENCY_RECOVERY_HEADER_SIZE 128
#define MYLITE_TEST_CONCURRENCY_CHECKPOINT_LATEST_LSN_OFFSET 128
#define MYLITE_TEST_CONCURRENCY_CHECKPOINT_VISIBLE_LSN_OFFSET 136
#define MYLITE_TEST_PAGE_LOG_HEADER_SIZE 64
#define MYLITE_TEST_PAGE_LOG_RECORD_HEADER_SIZE 64
#define MYLITE_TEST_PAGE_LOG_RECORD_COMMIT_LSN_OFFSET 32
#define MYLITE_TEST_PAGE_LOG_RECORD_PAYLOAD_SIZE_OFFSET 40
#define MYLITE_TEST_STRESS_WRITER_COUNT 4U
#define MYLITE_TEST_STRESS_ITERATIONS 24U
#define MYLITE_TEST_STRESS_READER_POLLS 48U
#define MYLITE_TEST_STRESS_ITERATIONS_MAX 10000U
#define MYLITE_TEST_STRESS_READER_POLLS_MAX 20000U
#define MYLITE_TEST_COMMIT_RACE_WORKER_COUNT 4U
#define MYLITE_TEST_DDL_STRESS_WORKER_COUNT 3U
#define MYLITE_TEST_DDL_STRESS_DML_WORKER_COUNT 2U
#define MYLITE_TEST_DDL_STRESS_ROUNDS 3U
#define MYLITE_TEST_DDL_STRESS_ROUNDS_MAX 200U
#define MYLITE_TEST_DDL_STRESS_DML_UPDATES_PER_ROUND 8U
#define MYLITE_TEST_TEMP_STRESS_WORKER_COUNT 4U
#define MYLITE_TEST_TEMP_STRESS_ROUNDS 12U
#define MYLITE_TEST_TEMP_STRESS_ROUNDS_MAX 2000U
#define MYLITE_TEST_TX_STRESS_WORKER_COUNT 3U
#define MYLITE_TEST_TX_STRESS_ROUNDS 12U
#define MYLITE_TEST_TX_STRESS_ROUNDS_MAX 5000U
#define MYLITE_TEST_CHECKSUM_STRESS_WORKER_COUNT 4U
#define MYLITE_TEST_CHECKSUM_STRESS_ROWS_PER_WORKER 8U
#define MYLITE_TEST_CHECKSUM_STRESS_ROUNDS 48U
#define MYLITE_TEST_CHECKSUM_STRESS_ROUNDS_MAX 5000U
#define MYLITE_TEST_RANDOM_TX_STRESS_WORKER_COUNT 4U
#define MYLITE_TEST_RANDOM_TX_STRESS_ROW_COUNT 16U
#define MYLITE_TEST_RANDOM_TX_STRESS_ROUNDS 24U
#define MYLITE_TEST_RANDOM_TX_STRESS_ROUNDS_MAX 5000U
#define MYLITE_TEST_RANDOM_TX_STRESS_PAD_BYTES 3600U
#define MYLITE_TEST_RANDOM_TX_STRESS_MAX_ATTEMPTS 200U
#define MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT 3U
#define MYLITE_TEST_FK_GRAPH_STRESS_ROUNDS 12U
#define MYLITE_TEST_FK_GRAPH_STRESS_ROUNDS_MAX 2000U
#define MYLITE_TEST_FK_GRAPH_STRESS_MAX_ATTEMPTS 200U
#define MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT 2U
#define MYLITE_TEST_AUTO_INCREMENT_ROWS_PER_WORKER 12U
#define MYLITE_TEST_PURGE_HISTORY_UPDATES 64U
#define MYLITE_TEST_DDL_WORKER_COUNT 3U
#define MYLITE_TEST_DDL_TABLES_PER_WORKER 4U
#ifndef MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
#  define MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS 0
#endif

extern uint64_t mylite_ownerless_innodb_current_lsn(void);
extern uint64_t mylite_ownerless_innodb_checkpoint_lsn(void);

typedef struct open_database_paths {
    const char *database_path;
    const char *runtime_root;
} open_database_paths;

typedef struct child_pipes {
    int ready_write_fd;
    int release_read_fd;
} child_pipes;

typedef struct query_result {
    unsigned long long value;
} query_result;

typedef struct show_create_trigger_expectation {
    const char *trigger_name;
    const char *statement_substring;
    unsigned seen_rows;
} show_create_trigger_expectation;

typedef struct ownerless_table_wait_negative_case {
    const char *name;
    const char *sql;
} ownerless_table_wait_negative_case;

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
typedef struct wait_child_or_pipe_result {
    int child_result;
    int pipe_message;
    int timed_out;
} wait_child_or_pipe_result;
#endif

typedef void (*ownerless_test_fn)(void);

static void run_all_ownerless_sql_tests(void);
static void run_ownerless_sql_test_case(ownerless_test_fn test_fn);
static void test_two_processes_update_different_innodb_rows(void);
static void test_two_processes_update_same_innodb_row(void);
static void test_two_processes_update_different_innodb_tables(void);
static void test_ownerless_concurrent_transaction_commits(void);
static void test_two_processes_deadlock_on_innodb_rows(void);
static void test_ownerless_gap_lock_blocks_insert(void);
static void test_ownerless_savepoint_rollback_is_peer_visible_after_commit(void);
static void test_ownerless_serializable_read_blocks_peer_update(void);
static void test_ownerless_serializable_prevents_write_skew(void);
static void test_ownerless_auto_increment_assigns_distinct_ids(void);
static void test_ownerless_auto_increment_ddl_refreshes_peer_high_water(void);
static void test_four_processes_mix_ownerless_reads_and_writes(void);
static void test_ownerless_independent_table_stress(void);
static void test_ownerless_concurrent_ddl_stress(void);
static void test_ownerless_temporary_table_stress(void);
static void test_ownerless_transaction_mix_stress(void);
static void test_ownerless_checksum_stress(void);
static void test_ownerless_random_transaction_stress(void);
static void test_ownerless_foreign_key_graph_stress(void);
static void test_ownerless_active_reader_pressure_reclaims_after_release(void);
static void test_ownerless_active_reader_pressure_limit_blocks_writes(void);
static void test_ownerless_active_reader_pressure_limit_blocks_write_classes(void);
static void test_ownerless_active_reader_pressure_diagnostics(void);
static void test_ownerless_expanding_page_pressure_reclaims_after_release(void);
static void test_ownerless_no_live_pressure_reclaim_advances_visible_lsn(void);
static void test_ownerless_purge_preserves_cross_process_snapshot(void);
static void test_ownerless_native_checkpoint_evidence(void);
static void test_ownerless_native_checkpoint_reclaims_page_log(void);
static void test_ownerless_live_idle_peer_reclaims_page_log(void);
static void test_ownerless_live_snapshot_pin_blocks_page_log_reclaim(void);
static void test_ownerless_live_snapshot_pin_synthesizes_page_boundary(void);
static void test_killed_ownerless_snapshot_pin_allows_live_page_log_reclaim(void);
static void test_process_reads_committed_external_update(void);
static void test_prepared_process_reads_committed_external_update(void);
static void test_transaction_first_read_sees_committed_external_update(void);
static void test_prepared_transaction_first_read_sees_committed_external_update(void);
static void test_transaction_with_local_write_first_read_sees_committed_external_update(void);
static void test_transaction_with_local_write_snapshot_hides_later_external_update(void);
static void test_consistent_snapshot_transaction_hides_later_external_update(void);
static void test_read_committed_transaction_observes_later_external_update(void);
static void test_next_read_committed_transaction_observes_later_external_update(void);
static void test_shared_readonly_process_reads_committed_external_update(void);
static void test_rebuild_checkpoints_committed_page_versions(void);
static void test_ownerless_alter_waits_for_active_transaction(void);
static void test_ownerless_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_large_truncate_refreshes_peer_allocation(void);
static void test_ownerless_local_ddl_survives_dictionary_flush(void);
static void test_concurrent_ownerless_ddl_allocates_unique_metadata(void);
static void test_ownerless_broader_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_online_ddl_options_refresh_peer_dictionary(void);
static void test_ownerless_generated_column_alter_refreshes_peer_dictionary(void);
static void test_ownerless_charset_convert_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_row_format_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_table_comment_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_force_rebuild_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_column_default_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_instant_column_variants_refresh_peer_dictionary(void);
static void test_ownerless_schema_lifecycle_refreshes_peer_dictionary(void);
static void test_ownerless_schema_default_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_schema_idempotent_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_cross_schema_rename_refreshes_peer_dictionary(void);
static void test_ownerless_multi_rename_cycle_refreshes_peer_dictionary(void);
static void test_ownerless_view_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_view_ddl_variants_refresh_peer_dictionary(void);
static void test_ownerless_view_check_option_refreshes_peer_dictionary(void);
static void test_ownerless_trigger_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_trigger_ddl_variants_refresh_peer_dictionary(void);
static void test_ownerless_trigger_ordering_refreshes_peer_dictionary(void);
static void test_ownerless_trigger_idempotent_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_rejects_stored_routine_ddl(void);
static void test_ownerless_index_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_rename_index_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_ignored_index_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_unique_index_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_primary_key_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_foreign_key_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_foreign_key_actions_cross_process(void);
static void test_ownerless_composite_foreign_keys_cross_process(void);
static void test_ownerless_foreign_key_deep_cascade_cross_process(void);
static void test_ownerless_generated_column_foreign_key_cross_process(void);
static void test_ownerless_generated_column_foreign_key_policy(void);
static void test_ownerless_cyclic_foreign_key_cross_process(void);
static void test_ownerless_cyclic_foreign_key_variants_cross_process(void);
static void test_ownerless_foreign_key_rename_refreshes_peer_dictionary(void);
static void test_ownerless_foreign_key_child_rename_refreshes_peer_dictionary(void);
static void test_ownerless_foreign_key_cross_schema_rename_refreshes_peer_dictionary(void);
static void test_ownerless_foreign_key_cross_schema_child_rename_refreshes_peer_dictionary(void);
static void test_ownerless_foreign_key_multi_rename_refreshes_peer_dictionary(void);
static void test_ownerless_foreign_key_cross_schema_multi_rename_refreshes_peer_dictionary(void);
static void test_ownerless_check_constraint_ddl_refreshes_peer_dictionary(void);
static void test_ownerless_rejects_table_admin_sql(void);
static void test_ownerless_rejects_lock_tables_sql(void);
static void test_ownerless_rejects_flush_table_lock_sql(void);
static void test_ownerless_rejects_read_uncommitted_isolation(void);
static void test_ownerless_rejects_sequence_sql(void);
static void test_ownerless_rejects_table_directory_options(void);
static void test_ownerless_rejects_special_index_ddl(void);
static void test_ownerless_rejects_partition_ddl(void);
static void test_ownerless_rejects_tablespace_management_ddl(void);
static void test_ownerless_temporary_tablespace_allows_peer_temp_tables(void);
static void test_crashed_ownerless_temporary_table_peer_is_recovered(void);
static void test_ownerless_rejects_non_innodb_engines(void);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void test_crashed_page_publish_before_append_rebuilds_ownerless_state(void);
static void test_crashed_page_publish_rebuilds_ownerless_state(void);
static void test_crashed_checkpoint_rebuilds_ownerless_state(void);
static void test_crashed_visible_publish_without_checkpoint_preserves_committed_update(void);
static void test_crashed_visible_checkpoint_preserves_committed_update(void);
static void test_ownerless_active_pin_reclaims_page_log_with_boundary(void);
static void test_crashed_redo_reservation_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_redo_written_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_redo_latest_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_redo_latest_checkpoint_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_redo_gap_blocks_later_writer_until_rebuild(void);
static void test_crashed_native_checkpoint_reclaim_preserves_committed_update(void);
static void test_native_checkpoint_reclaim_race_preserves_newer_peer_commit(void);
static void test_consistent_snapshot_start_pin_blocks_live_reclaim_before_execute(void);
static void test_ownerless_table_wait_sql_negative_proof(void);
static void test_crashed_trx_registration_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_record_lock_before_grant_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_record_lock_grant_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_dictionary_ddl_begin_rebuilds_ownerless_state(void);
static void test_crashed_dictionary_ddl_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_dictionary_ddl_finish_allows_peer_cleanup(void);
#endif
static void test_crashed_ownerless_writer_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void initialize_database(open_database_paths paths);
static void update_first_row_until_released(open_database_paths paths, child_pipes pipes);
static void update_first_row_without_commit_until_killed(open_database_paths paths, int ready_fd);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void lock_first_row_for_update_until_released(open_database_paths paths, child_pipes pipes);
static void update_first_row_until_trx_register_fault(open_database_paths paths, int ready_fd);
static void update_first_row_until_record_lock_before_grant_fault(
    open_database_paths paths,
    int ready_fd
);
static void update_first_row_until_record_lock_grant_fault(open_database_paths paths, int ready_fd);
#endif
static void hold_ownerless_open_until_released(open_database_paths paths, child_pipes pipes);
static void assert_ownerless_open_returns_busy(open_database_paths paths);
static void update_second_row(open_database_paths paths);
static void update_first_row_by_two(open_database_paths paths);
static void update_first_table_until_released(open_database_paths paths, child_pipes pipes);
static void update_second_table_until_released(open_database_paths paths, child_pipes pipes);
static void commit_race_update_row_after_signal(
    open_database_paths paths,
    unsigned table_id,
    unsigned delta,
    child_pipes pipes
);
static void update_table_pair_after_signal(
    open_database_paths paths,
    const char *first_table,
    const char *second_table,
    unsigned increment,
    child_pipes pipes
);
static void hold_gap_lock_until_released(open_database_paths paths, child_pipes pipes);
static void insert_gap_row_expect_lock_timeout(open_database_paths paths);
static void update_with_savepoint_rollback_until_released(
    open_database_paths paths,
    child_pipes pipes
);
static void hold_serializable_read_until_released(open_database_paths paths, child_pipes pipes);
static void update_first_row_expect_lock_timeout(open_database_paths paths);
static void run_serializable_write_skew_candidate_after_signal(
    open_database_paths paths,
    unsigned doctor_id,
    child_pipes pipes
);
static void insert_auto_increment_rows_after_signal(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
static void alter_auto_increment_after_signal(open_database_paths paths, child_pipes pipes);
static void increment_mix_row_after_signal(
    open_database_paths paths,
    unsigned row_id,
    child_pipes pipes
);
static void read_mix_total_after_signal(open_database_paths paths, child_pipes pipes);
static void run_ownerless_stress_writer(
    open_database_paths paths,
    unsigned table_id,
    child_pipes pipes
);
static void run_ownerless_stress_reader(open_database_paths paths, child_pipes pipes);
static void run_ownerless_ddl_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
static void run_ownerless_ddl_stress_dml_worker(
    open_database_paths paths,
    unsigned row_id,
    child_pipes pipes
);
static void run_ownerless_ddl_stress_reader(open_database_paths paths, child_pipes pipes);
static void run_ownerless_temp_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
static void run_ownerless_tx_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
static void run_ownerless_checksum_stress_writer(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
static void run_ownerless_prepared_checksum_stress_writer(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
static void run_ownerless_checksum_stress_reader(open_database_paths paths, child_pipes pipes);
static void run_ownerless_random_tx_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
static void run_ownerless_random_tx_stress_reader(open_database_paths paths, child_pipes pipes);
static void run_ownerless_fk_graph_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
static void hold_repeatable_read_snapshot_until_released(
    open_database_paths paths,
    child_pipes pipes
);
static void hold_expanding_page_snapshot_until_released(
    open_database_paths paths,
    child_pipes pipes
);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void start_consistent_snapshot_after_pin_fault(open_database_paths paths, child_pipes pipes);
static void hold_reclaim_boundary_snapshot_until_released(
    open_database_paths paths,
    child_pipes pipes
);
#endif
static void churn_ownerless_history(open_database_paths paths);
static void update_first_row_by_seven_after_signal(open_database_paths paths, int start_read_fd);
static void hold_select_for_update_until_released(open_database_paths paths, child_pipes pipes);
static void alter_ownerless_sql_expect_lock_timeout(open_database_paths paths);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void ownerless_sql_expect_lock_timeout_with_table_wait_fault(
    open_database_paths paths,
    const ownerless_table_wait_negative_case *test_case,
    int ready_fd
);
#endif
static void assert_shared_readonly_open_returns_busy(open_database_paths paths);
static void run_ownerless_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void truncate_ownerless_large_table_after_signal(
    open_database_paths paths,
    child_pipes pipes
);
static void create_ownerless_ddl_tables_after_signal(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
static void run_ownerless_broader_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_online_ddl_options_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_generated_column_alter_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_charset_convert_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_row_format_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_table_comment_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_force_rebuild_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_column_default_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_instant_column_variant_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_schema_lifecycle_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_schema_default_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_schema_idempotent_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_cross_schema_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_multi_rename_cycle_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_view_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_view_ddl_variant_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_view_check_option_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_trigger_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_trigger_ddl_variant_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_trigger_ordering_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_trigger_idempotent_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_index_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_rename_index_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_ignored_index_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_unique_index_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_primary_key_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_foreign_key_ddl_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_foreign_key_action_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_composite_foreign_key_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_foreign_key_deep_cascade_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_generated_column_foreign_key_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_generated_column_foreign_key_policy_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_cyclic_foreign_key_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_cyclic_foreign_key_variants_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_foreign_key_rename_sequence(open_database_paths paths, child_pipes pipes);
static void run_ownerless_foreign_key_child_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_foreign_key_cross_schema_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_foreign_key_cross_schema_child_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_foreign_key_multi_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_foreign_key_cross_schema_multi_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void run_ownerless_check_constraint_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
);
static void hold_ownerless_temporary_table_until_released(
    open_database_paths paths,
    unsigned value,
    child_pipes pipes
);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void update_first_row_until_page_publish_before_append_fault(
    open_database_paths paths,
    int ready_fd
);
static void update_first_row_until_page_publish_fault(open_database_paths paths, int ready_fd);
static void open_database_until_checkpoint_fault(open_database_paths paths, int ready_fd);
static void update_first_row_until_visible_publish_fault(
    open_database_paths paths,
    int ready_fd,
    int proceed_fd,
    int fault_ready_fd
);
static void update_first_row_until_visible_checkpoint_fault(
    open_database_paths paths,
    int ready_fd
);
static void update_first_row_until_redo_reserve_fault(open_database_paths paths, int ready_fd);
static void update_first_row_until_redo_written_fault(open_database_paths paths, int ready_fd);
static void update_first_row_until_redo_latest_checkpoint_fault(
    open_database_paths paths,
    int ready_fd
);
static void update_first_row_until_redo_after_checkpoint_fault(
    open_database_paths paths,
    int ready_fd
);
static void update_redo_gap_peer_table_after_signal(open_database_paths paths, int ready_fd);
static void update_first_row_until_native_checkpoint_reclaim_fault(
    open_database_paths paths,
    int ready_fd
);
static void update_first_row_until_native_checkpoint_reclaim_release(
    open_database_paths paths,
    int ready_fd,
    int release_fd
);
static void assert_redo_gap_blocked_values(open_database_paths paths);
static void create_table_until_dictionary_begin_fault(open_database_paths paths, int ready_fd);
static void create_table_until_dictionary_finish_fault(open_database_paths paths, int ready_fd);
static void create_table_until_dictionary_after_finish_fault(
    open_database_paths paths,
    int ready_fd
);
static void create_table_until_dictionary_fault(
    open_database_paths paths,
    int ready_fd,
    const char *fault_name,
    const char *table_name
);
#endif
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static mylite_db *open_database_with_page_log_limit(
    open_database_paths paths,
    unsigned flags,
    unsigned long long limit_bytes
);
static mylite_db *open_database_allowing_failure(open_database_paths paths, unsigned flags);
static int open_database_result(open_database_paths paths, unsigned flags, mylite_db **out_db);
static int open_database_with_page_log_limit_result(
    open_database_paths paths,
    unsigned flags,
    unsigned long long limit_bytes,
    mylite_db **out_db
);
static void exec_ok(mylite_db *db, const char *sql);
static int exec_status(mylite_db *db, const char *sql, unsigned *mariadb_errno);
static void expect_exec_error(mylite_db *db, const char *sql);
static void expect_exec_mariadb_error(mylite_db *db, const char *sql, unsigned expected_errno);
static void expect_exec_busy(mylite_db *db, const char *sql, const char *message_part);
static void expect_readonly_exec_error(mylite_db *db, const char *sql);
static unsigned long long query_unsigned(mylite_db *db, const char *sql);
static void assert_ownerless_pressure_write_policy_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_ddl_tables(mylite_db *db);
static void assert_total_value(open_database_paths paths, unsigned long long expected);
static void assert_ownerless_total_value(open_database_paths paths, unsigned long long expected);
static void assert_commit_race_total(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_sum
);
static uint64_t assert_commit_race_recovery_anchors(
    const char *database_path,
    uint64_t minimum_visible_lsn
);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void assert_total_value_is_one_of(
    open_database_paths paths,
    unsigned long long first_expected,
    unsigned long long second_expected
);
#endif
static void assert_table_total_value_is_one_of(
    open_database_paths paths,
    unsigned long long first_expected,
    unsigned long long second_expected
);
static void assert_table_values(open_database_paths paths);
static void assert_ownerless_stress_total(open_database_paths paths, unsigned long long expected);
static unsigned ownerless_stress_iterations(void);
static unsigned ownerless_stress_reader_polls(void);
static unsigned ownerless_ddl_stress_rounds(void);
static unsigned ownerless_temp_stress_rounds(void);
static unsigned ownerless_tx_stress_rounds(void);
static unsigned ownerless_checksum_stress_rounds(void);
static unsigned ownerless_random_tx_stress_rounds(void);
static unsigned ownerless_fk_graph_stress_rounds(void);
static unsigned long long ownerless_tx_stress_delta(unsigned worker_id, unsigned round);
static unsigned long long ownerless_tx_stress_delta_sum(unsigned worker_id, unsigned rounds);
static unsigned ownerless_checksum_stress_row_id(unsigned worker_id, unsigned round);
static unsigned long long ownerless_checksum_stress_delta(unsigned worker_id, unsigned round);
static void ownerless_random_tx_stress_rows(unsigned worker_id, unsigned round, unsigned rows[3]);
static unsigned long long ownerless_random_tx_stress_delta(
    unsigned worker_id,
    unsigned round,
    unsigned phase
);
static int ownerless_random_tx_stress_exec_retryable(
    mylite_db *db,
    const char *sql,
    unsigned worker_id,
    unsigned round,
    unsigned attempt
);
static void ownerless_random_tx_stress_retry_pause(
    unsigned worker_id,
    unsigned round,
    unsigned attempt
);
static int ownerless_random_tx_stress_rolls_back_transaction(unsigned worker_id, unsigned round);
static int ownerless_random_tx_stress_rolls_back_savepoint(unsigned worker_id, unsigned round);
static void ownerless_checksum_stress_expected(
    unsigned rounds,
    unsigned long long *out_sum,
    unsigned long long *out_versions,
    unsigned long long *out_weighted_sum
);
static void ownerless_random_tx_stress_expected(
    unsigned rounds,
    unsigned long long *out_sum,
    unsigned long long *out_versions,
    unsigned long long *out_weighted_sum
);
static unsigned ownerless_fk_graph_stress_initial_id(unsigned worker_id, unsigned kind);
static unsigned long long ownerless_fk_graph_stress_delta(unsigned worker_id, unsigned round);
static unsigned long long ownerless_fk_graph_stress_delta_sum(unsigned worker_id, unsigned rounds);
static unsigned long long ownerless_fk_graph_stress_total_delta_sum(unsigned rounds);
static int ownerless_fk_graph_stress_exec_retryable(
    mylite_db *db,
    const char *sql,
    unsigned worker_id,
    unsigned round,
    unsigned attempt
);
static void ownerless_fk_graph_stress_expect_mariadb_error(
    mylite_db *db,
    const char *sql,
    unsigned expected_errno,
    unsigned worker_id,
    unsigned round
);
static void ownerless_fk_graph_stress_retry_pause(
    unsigned worker_id,
    unsigned round,
    unsigned attempt
);
static void assert_ownerless_ddl_stress_state(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_total
);
static void assert_ownerless_auto_increment_ddl_state(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_count,
    unsigned long long expected_id_sum,
    unsigned long long expected_value_sum,
    unsigned long long expected_max_id
);
static void assert_ownerless_broader_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_online_ddl_options_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_generated_column_alter_state(
    open_database_paths paths,
    unsigned flags
);
static void assert_ownerless_charset_convert_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_row_format_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_table_comment_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_force_rebuild_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_column_default_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_instant_column_variant_state(
    open_database_paths paths,
    unsigned flags
);
static void assert_ownerless_schema_lifecycle_absent(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_schema_default_ddl_absent(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_schema_idempotent_ddl_absent(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_cross_schema_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_multi_rename_cycle_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_view_ddl_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_view_ddl_variant_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_view_check_option_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_trigger_ddl_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_trigger_ddl_variant_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_trigger_ordering_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_trigger_idempotent_ddl_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_stored_routine_policy_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_index_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_rename_index_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_ignored_index_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_unique_index_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_primary_key_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_foreign_key_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_foreign_key_action_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_composite_foreign_key_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_foreign_key_deep_cascade_state(
    open_database_paths paths,
    unsigned flags
);
static void assert_ownerless_generated_column_foreign_key_state(
    open_database_paths paths,
    unsigned flags
);
static void assert_ownerless_generated_column_foreign_key_policy_state(
    open_database_paths paths,
    unsigned flags
);
static void assert_ownerless_cyclic_foreign_key_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_cyclic_foreign_key_variants_state(
    open_database_paths paths,
    unsigned flags
);
static void assert_ownerless_foreign_key_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_foreign_key_child_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_foreign_key_cross_schema_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_foreign_key_cross_schema_child_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_foreign_key_multi_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_foreign_key_cross_schema_multi_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_check_constraint_ddl_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_table_admin_policy_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_lock_tables_policy_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_flush_table_lock_policy_state(
    open_database_paths paths,
    unsigned flags
);
static void assert_ownerless_sequence_policy_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_table_directory_policy_state(
    open_database_paths paths,
    unsigned flags,
    const char *external_data_path,
    const char *external_index_path
);
static void assert_ownerless_special_index_policy_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_partition_policy_state(open_database_paths paths, unsigned flags);
static void assert_ownerless_tablespace_management_policy_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
);
static void assert_ownerless_temp_stress_permanent_table(open_database_paths paths, unsigned flags);
static void assert_ownerless_checksum_stress_totals(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_sum,
    unsigned long long expected_versions,
    unsigned long long expected_weighted_sum
);
static void assert_ownerless_random_tx_stress_totals(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_sum,
    unsigned long long expected_versions,
    unsigned long long expected_weighted_sum
);
static void assert_ownerless_fk_graph_stress_state(
    open_database_paths paths,
    unsigned flags,
    unsigned rounds
);
static void assert_ownerless_tx_stress_totals(
    open_database_paths paths,
    unsigned flags,
    unsigned rounds
);
static void assert_ownerless_write_skew_invariant(open_database_paths paths, unsigned flags);
static unsigned ownerless_unsigned_env(
    const char *name,
    unsigned default_value,
    unsigned max_value
);
static void assert_concurrency_wal_has_page_versions_or_checkpoint(const char *database_path);
static void assert_concurrency_page_index_has_entries(const char *database_path);
static unsigned count_concurrency_wal_records_at_or_before(
    const char *database_path,
    uint64_t commit_lsn
);
static off_t concurrency_wal_size(const char *database_path);
static int concurrency_wal_is_checkpointed(const char *database_path);
static void assert_concurrency_wal_checkpointed(const char *database_path);
static void remove_concurrency_shm(const char *database_path);
static int capture_first_column(void *ctx, int column_count, char **values, char **columns);
static void assert_show_create_trigger_contains(
    mylite_db *db,
    const char *sql,
    const char *trigger_name,
    const char *statement_substring
);
static int capture_show_create_trigger(void *ctx, int column_count, char **values, char **columns);
static uint64_t wait_for_concurrency_ownerless_write_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
);
static uint64_t read_concurrency_innodb_lock_waiting_count(const char *database_path);
static uint64_t read_concurrency_page_write_lock_waiting_count(const char *database_path);
static uint64_t read_concurrency_lock_waiting_count(
    const char *database_path,
    uint32_t segment_type
);
static uint64_t read_concurrency_redo_visible_lsn(const char *database_path);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static uint64_t read_concurrency_redo_latest_lsn(const char *database_path);
static uint64_t read_concurrency_redo_written_lsn(const char *database_path);
#endif
static uint64_t read_concurrency_checkpoint_latest_lsn(const char *database_path);
static uint64_t read_concurrency_checkpoint_visible_lsn(const char *database_path);
static void write_concurrency_checkpoint_visible_lsn(
    const char *database_path,
    uint64_t visible_lsn
);
static uint64_t read_concurrency_page_index_active_count(const char *database_path);
static uint64_t read_concurrency_shm_segment_offset(int fd, uint32_t segment_type);
static void read_exact_at(int fd, void *buffer, size_t size, off_t offset);
static uint64_t read_native64(const unsigned char *bytes);
static uint32_t read_le32(const unsigned char *bytes);
static uint64_t read_le64(const unsigned char *bytes);
static void sleep_microseconds(unsigned microseconds);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void signal_pipe_message(int pipe_fd);
static void wait_for_pipe_message(int pipe_fd);
static void signal_pipe(int pipe_fd);
static void wait_for_pipe(int pipe_fd);
static void wait_for_child(pid_t child);
static void wait_for_signaled_child(pid_t child, int expected_signal);
static int wait_for_child_result(pid_t child);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void kill_or_reap_child(pid_t child);
static wait_child_or_pipe_result wait_for_child_result_or_pipe_message(
    pid_t child,
    int pipe_fd,
    unsigned timeout_ms
);
#endif
static int path_exists(const char *path);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "stress") == 0) {
        test_ownerless_independent_table_stress();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "ddl-stress") == 0) {
        test_ownerless_concurrent_ddl_stress();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "temp-stress") == 0) {
        test_ownerless_temporary_table_stress();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "tx-stress") == 0) {
        test_ownerless_transaction_mix_stress();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "checksum-stress") == 0) {
        test_ownerless_checksum_stress();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "random-tx-stress") == 0) {
        test_ownerless_random_transaction_stress();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "fk-graph-stress") == 0) {
        test_ownerless_foreign_key_graph_stress();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "active-reader-pressure") == 0) {
        test_ownerless_active_reader_pressure_reclaims_after_release();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "active-reader-pressure-limit") == 0) {
        test_ownerless_active_reader_pressure_limit_blocks_writes();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "active-reader-pressure-write-policy") == 0) {
        test_ownerless_active_reader_pressure_limit_blocks_write_classes();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "active-reader-pressure-diagnostics") == 0) {
        test_ownerless_active_reader_pressure_diagnostics();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "expanding-page-pressure") == 0) {
        test_ownerless_expanding_page_pressure_reclaims_after_release();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "no-live-pressure-reclaim") == 0) {
        test_ownerless_no_live_pressure_reclaim_advances_visible_lsn();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "ddl-refresh") == 0) {
        test_ownerless_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "ddl-allocation") == 0) {
        test_concurrent_ownerless_ddl_allocates_unique_metadata();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "ddl-truncate-refresh") == 0) {
        test_ownerless_large_truncate_refreshes_peer_allocation();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "ddl-broader") == 0) {
        test_ownerless_broader_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "online-ddl-options") == 0) {
        test_ownerless_online_ddl_options_refresh_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "generated-column-alter") == 0) {
        test_ownerless_generated_column_alter_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "charset-convert-ddl") == 0) {
        test_ownerless_charset_convert_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "row-format-ddl") == 0) {
        test_ownerless_row_format_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "table-comment-ddl") == 0) {
        test_ownerless_table_comment_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "force-rebuild-ddl") == 0) {
        test_ownerless_force_rebuild_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "column-default-ddl") == 0) {
        test_ownerless_column_default_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "instant-column-variants") == 0) {
        test_ownerless_instant_column_variants_refresh_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "schema-lifecycle") == 0) {
        test_ownerless_schema_lifecycle_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "schema-default-ddl") == 0) {
        test_ownerless_schema_default_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "schema-idempotent-ddl") == 0) {
        test_ownerless_schema_idempotent_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "cross-schema-rename") == 0) {
        test_ownerless_cross_schema_rename_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "multi-rename-cycle") == 0) {
        test_ownerless_multi_rename_cycle_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "view-ddl") == 0) {
        test_ownerless_view_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "view-ddl-variants") == 0) {
        test_ownerless_view_ddl_variants_refresh_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "view-check-option") == 0) {
        test_ownerless_view_check_option_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "trigger-ddl") == 0) {
        test_ownerless_trigger_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "trigger-ddl-variants") == 0) {
        test_ownerless_trigger_ddl_variants_refresh_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "trigger-ordering") == 0) {
        test_ownerless_trigger_ordering_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "trigger-idempotent-ddl") == 0) {
        test_ownerless_trigger_idempotent_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "routine-policy") == 0) {
        test_ownerless_rejects_stored_routine_ddl();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "index-ddl") == 0) {
        test_ownerless_index_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "rename-index-ddl") == 0) {
        test_ownerless_rename_index_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "ignored-index-ddl") == 0) {
        test_ownerless_ignored_index_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "unique-index-ddl") == 0) {
        test_ownerless_unique_index_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "primary-key-ddl") == 0) {
        test_ownerless_primary_key_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "foreign-key-ddl") == 0) {
        test_ownerless_foreign_key_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "foreign-key-actions") == 0) {
        test_ownerless_foreign_key_actions_cross_process();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "composite-foreign-key") == 0) {
        test_ownerless_composite_foreign_keys_cross_process();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "foreign-key-deep-cascade") == 0) {
        test_ownerless_foreign_key_deep_cascade_cross_process();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "generated-column-foreign-key") == 0) {
        test_ownerless_generated_column_foreign_key_cross_process();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "generated-column-foreign-key-policy") == 0) {
        test_ownerless_generated_column_foreign_key_policy();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "cyclic-foreign-key") == 0) {
        test_ownerless_cyclic_foreign_key_cross_process();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "cyclic-foreign-key-variants") == 0) {
        test_ownerless_cyclic_foreign_key_variants_cross_process();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "foreign-key-rename") == 0) {
        test_ownerless_foreign_key_rename_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "foreign-key-child-rename") == 0) {
        test_ownerless_foreign_key_child_rename_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "foreign-key-cross-schema-rename") == 0) {
        test_ownerless_foreign_key_cross_schema_rename_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "foreign-key-cross-schema-child-rename") == 0) {
        test_ownerless_foreign_key_cross_schema_child_rename_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "foreign-key-multi-rename") == 0) {
        test_ownerless_foreign_key_multi_rename_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "foreign-key-cross-schema-multi-rename") == 0) {
        test_ownerless_foreign_key_cross_schema_multi_rename_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "check-constraint-ddl") == 0) {
        test_ownerless_check_constraint_ddl_refreshes_peer_dictionary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "table-admin-policy") == 0) {
        test_ownerless_rejects_table_admin_sql();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "lock-tables-policy") == 0) {
        test_ownerless_rejects_lock_tables_sql();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "flush-table-lock-policy") == 0) {
        test_ownerless_rejects_flush_table_lock_sql();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "read-uncommitted-policy") == 0) {
        test_ownerless_rejects_read_uncommitted_isolation();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "sequence-policy") == 0) {
        test_ownerless_rejects_sequence_sql();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "table-directory-policy") == 0) {
        test_ownerless_rejects_table_directory_options();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "special-index-policy") == 0) {
        test_ownerless_rejects_special_index_ddl();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "partition-policy") == 0) {
        test_ownerless_rejects_partition_ddl();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "tablespace-policy") == 0) {
        test_ownerless_rejects_tablespace_management_ddl();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "prepared-committed-read") == 0) {
        test_prepared_process_reads_committed_external_update();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "local-write-first-read") == 0) {
        test_transaction_with_local_write_first_read_sees_committed_external_update();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "isolation") == 0) {
        test_transaction_with_local_write_snapshot_hides_later_external_update();
        test_consistent_snapshot_transaction_hides_later_external_update();
        test_read_committed_transaction_observes_later_external_update();
        test_next_read_committed_transaction_observes_later_external_update();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "shared-readonly") == 0) {
        test_shared_readonly_process_reads_committed_external_update();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "checkpoint-evidence") == 0) {
        test_ownerless_native_checkpoint_evidence();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "native-reclaim") == 0) {
        test_ownerless_native_checkpoint_reclaims_page_log();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "live-reclaim") == 0) {
        test_ownerless_live_idle_peer_reclaims_page_log();
        test_ownerless_live_snapshot_pin_blocks_page_log_reclaim();
        test_ownerless_live_snapshot_pin_synthesizes_page_boundary();
        test_killed_ownerless_snapshot_pin_allows_live_page_log_reclaim();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "visibility-prefix") == 0) {
        test_process_reads_committed_external_update();
        test_prepared_process_reads_committed_external_update();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "engine-policy") == 0) {
        test_ownerless_rejects_non_innodb_engines();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "engine-policy-page-publish") == 0) {
        test_ownerless_rejects_non_innodb_engines();
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_page_publish_rebuilds_ownerless_state();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "different-rows") == 0) {
        test_two_processes_update_different_innodb_rows();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "same-row") == 0) {
        test_two_processes_update_same_innodb_row();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "different-tables") == 0) {
        test_two_processes_update_different_innodb_tables();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "commit-race") == 0) {
        test_ownerless_concurrent_transaction_commits();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "deadlock-rows") == 0) {
        test_two_processes_deadlock_on_innodb_rows();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "gap-lock") == 0) {
        test_ownerless_gap_lock_blocks_insert();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "savepoint") == 0) {
        test_ownerless_savepoint_rollback_is_peer_visible_after_commit();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "serializable") == 0) {
        test_ownerless_serializable_read_blocks_peer_update();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "write-skew") == 0) {
        test_ownerless_serializable_prevents_write_skew();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "auto-inc") == 0) {
        test_ownerless_auto_increment_assigns_distinct_ids();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "auto-inc-ddl") == 0) {
        test_ownerless_auto_increment_ddl_refreshes_peer_high_water();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "crash-writer") == 0) {
        test_crashed_ownerless_writer_blocks_peer_cleanup_until_reopen_rebuilds();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "page-publish-before-append-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_page_publish_before_append_rebuilds_ownerless_state();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "visible-checkpoint-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_visible_checkpoint_preserves_committed_update();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "visible-publish-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_visible_publish_without_checkpoint_preserves_committed_update();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "redo-written-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_redo_written_blocks_peer_cleanup_until_reopen_rebuilds();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "redo-latest-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_redo_latest_blocks_peer_cleanup_until_reopen_rebuilds();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "redo-latest-checkpoint-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_redo_latest_checkpoint_blocks_peer_cleanup_until_reopen_rebuilds();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "redo-gap-blocks-writer") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_redo_gap_blocks_later_writer_until_rebuild();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "native-reclaim-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_native_checkpoint_reclaim_preserves_committed_update();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "native-reclaim-race") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_native_checkpoint_reclaim_race_preserves_newer_peer_commit();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "consistent-snapshot-pin-race") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_consistent_snapshot_start_pin_blocks_live_reclaim_before_execute();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "active-pin-reclaim-boundary") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_ownerless_active_pin_reclaims_page_log_with_boundary();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "table-lock-wait-negative-proof") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_ownerless_table_wait_sql_negative_proof();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "trx-register-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_trx_registration_blocks_peer_cleanup_until_reopen_rebuilds();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "record-lock-before-grant-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_record_lock_before_grant_blocks_peer_cleanup_until_reopen_rebuilds();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "record-lock-grant-crash") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_record_lock_grant_blocks_peer_cleanup_until_reopen_rebuilds();
#endif
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "crash-tail") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_page_publish_before_append_rebuilds_ownerless_state();
        test_crashed_page_publish_rebuilds_ownerless_state();
        test_crashed_checkpoint_rebuilds_ownerless_state();
        test_crashed_visible_publish_without_checkpoint_preserves_committed_update();
        test_crashed_visible_checkpoint_preserves_committed_update();
        test_crashed_redo_reservation_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_redo_written_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_redo_latest_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_redo_latest_checkpoint_blocks_peer_cleanup_until_reopen_rebuilds();
        test_redo_gap_blocks_later_writer_until_rebuild();
        test_crashed_native_checkpoint_reclaim_preserves_committed_update();
        test_native_checkpoint_reclaim_race_preserves_newer_peer_commit();
        test_ownerless_active_pin_reclaims_page_log_with_boundary();
        test_crashed_trx_registration_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_record_lock_before_grant_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_record_lock_grant_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_dictionary_ddl_begin_rebuilds_ownerless_state();
        test_crashed_dictionary_ddl_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_dictionary_ddl_finish_allows_peer_cleanup();
#endif
        test_crashed_ownerless_writer_blocks_peer_cleanup_until_reopen_rebuilds();
        return 0;
    }
    if (argc != 1) {
        fprintf(
            stderr,
            "usage: %s [stress|ddl-stress|temp-stress|checksum-stress|"
            "tx-stress|random-tx-stress|fk-graph-stress|"
            "active-reader-pressure|active-reader-pressure-limit|"
            "active-reader-pressure-write-policy|"
            "active-reader-pressure-diagnostics|"
            "expanding-page-pressure|"
            "ddl-refresh|ddl-allocation|ddl-truncate-refresh|ddl-broader|"
            "online-ddl-options|schema-lifecycle|schema-default-ddl|"
            "schema-idempotent-ddl|cross-schema-rename|multi-rename-cycle|"
            "generated-column-alter|charset-convert-ddl|row-format-ddl|"
            "table-comment-ddl|force-rebuild-ddl|column-default-ddl|"
            "instant-column-variants|view-ddl|view-ddl-variants|view-check-option|trigger-ddl|"
            "trigger-ddl-variants|trigger-ordering|trigger-idempotent-ddl|routine-policy|index-ddl|"
            "rename-index-ddl|ignored-index-ddl|unique-index-ddl|"
            "primary-key-ddl|foreign-key-ddl|foreign-key-actions|composite-foreign-key|"
            "foreign-key-deep-cascade|generated-column-foreign-key|"
            "generated-column-foreign-key-policy|cyclic-foreign-key|"
            "cyclic-foreign-key-variants|foreign-key-rename|foreign-key-child-rename|"
            "foreign-key-cross-schema-rename|foreign-key-cross-schema-child-rename|"
            "foreign-key-multi-rename|foreign-key-cross-schema-multi-rename|"
            "check-constraint-ddl|"
            "table-admin-policy|lock-tables-policy|flush-table-lock-policy|"
            "read-uncommitted-policy|sequence-policy|table-directory-policy|"
            "special-index-policy|partition-policy|"
            "tablespace-policy|"
            "prepared-committed-read|local-write-first-read|isolation|"
            "shared-readonly|checkpoint-evidence|native-reclaim|live-reclaim|visibility-prefix|"
            "different-rows|same-row|different-tables|commit-race|deadlock-rows|gap-lock|"
            "savepoint|serializable|write-skew|auto-inc|auto-inc-ddl|engine-policy|"
            "engine-policy-page-publish|"
            "crash-writer|visible-publish-crash|visible-checkpoint-crash|redo-written-crash|"
            "page-publish-before-append-crash|redo-latest-crash|redo-latest-checkpoint-crash|"
            "redo-gap-blocks-writer|"
            "native-reclaim-crash|native-reclaim-race|consistent-snapshot-pin-race|"
            "active-pin-reclaim-boundary|"
            "table-lock-wait-negative-proof|"
            "trx-register-crash|record-lock-before-grant-crash|record-lock-grant-crash|"
            "crash-tail]\n",
            argv[0]
        );
        fflush(stderr);
        return 2;
    }

    run_all_ownerless_sql_tests();
    return 0;
}

static void run_all_ownerless_sql_tests(void) {
    run_ownerless_sql_test_case(test_two_processes_update_different_innodb_rows);
    run_ownerless_sql_test_case(test_two_processes_update_same_innodb_row);
    run_ownerless_sql_test_case(test_two_processes_update_different_innodb_tables);
    run_ownerless_sql_test_case(test_ownerless_concurrent_transaction_commits);
    run_ownerless_sql_test_case(test_two_processes_deadlock_on_innodb_rows);
    run_ownerless_sql_test_case(test_ownerless_gap_lock_blocks_insert);
    run_ownerless_sql_test_case(test_ownerless_savepoint_rollback_is_peer_visible_after_commit);
    run_ownerless_sql_test_case(test_ownerless_serializable_read_blocks_peer_update);
    run_ownerless_sql_test_case(test_ownerless_serializable_prevents_write_skew);
    run_ownerless_sql_test_case(test_ownerless_auto_increment_assigns_distinct_ids);
    run_ownerless_sql_test_case(test_ownerless_auto_increment_ddl_refreshes_peer_high_water);
    run_ownerless_sql_test_case(test_four_processes_mix_ownerless_reads_and_writes);
    run_ownerless_sql_test_case(test_ownerless_independent_table_stress);
    run_ownerless_sql_test_case(test_ownerless_purge_preserves_cross_process_snapshot);
    run_ownerless_sql_test_case(test_process_reads_committed_external_update);
    run_ownerless_sql_test_case(test_prepared_process_reads_committed_external_update);
    run_ownerless_sql_test_case(test_transaction_first_read_sees_committed_external_update);
    run_ownerless_sql_test_case(
        test_prepared_transaction_first_read_sees_committed_external_update
    );
    run_ownerless_sql_test_case(
        test_transaction_with_local_write_first_read_sees_committed_external_update
    );
    run_ownerless_sql_test_case(
        test_transaction_with_local_write_snapshot_hides_later_external_update
    );
    run_ownerless_sql_test_case(test_consistent_snapshot_transaction_hides_later_external_update);
    run_ownerless_sql_test_case(test_read_committed_transaction_observes_later_external_update);
    run_ownerless_sql_test_case(
        test_next_read_committed_transaction_observes_later_external_update
    );
    run_ownerless_sql_test_case(test_shared_readonly_process_reads_committed_external_update);
    run_ownerless_sql_test_case(test_ownerless_native_checkpoint_evidence);
    run_ownerless_sql_test_case(test_ownerless_native_checkpoint_reclaims_page_log);
    run_ownerless_sql_test_case(test_ownerless_live_idle_peer_reclaims_page_log);
    run_ownerless_sql_test_case(test_ownerless_live_snapshot_pin_blocks_page_log_reclaim);
    run_ownerless_sql_test_case(test_ownerless_live_snapshot_pin_synthesizes_page_boundary);
    run_ownerless_sql_test_case(test_killed_ownerless_snapshot_pin_allows_live_page_log_reclaim);
    run_ownerless_sql_test_case(test_ownerless_active_reader_pressure_reclaims_after_release);
    run_ownerless_sql_test_case(test_ownerless_active_reader_pressure_limit_blocks_writes);
    run_ownerless_sql_test_case(test_ownerless_active_reader_pressure_limit_blocks_write_classes);
    run_ownerless_sql_test_case(test_ownerless_active_reader_pressure_diagnostics);
    run_ownerless_sql_test_case(test_ownerless_expanding_page_pressure_reclaims_after_release);
    run_ownerless_sql_test_case(test_ownerless_no_live_pressure_reclaim_advances_visible_lsn);
    run_ownerless_sql_test_case(test_rebuild_checkpoints_committed_page_versions);
    run_ownerless_sql_test_case(test_ownerless_alter_waits_for_active_transaction);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    run_ownerless_sql_test_case(test_ownerless_table_wait_sql_negative_proof);
#endif
    run_ownerless_sql_test_case(test_ownerless_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_large_truncate_refreshes_peer_allocation);
    run_ownerless_sql_test_case(test_ownerless_local_ddl_survives_dictionary_flush);
    run_ownerless_sql_test_case(test_concurrent_ownerless_ddl_allocates_unique_metadata);
    run_ownerless_sql_test_case(test_ownerless_broader_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_online_ddl_options_refresh_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_generated_column_alter_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_charset_convert_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_row_format_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_table_comment_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_force_rebuild_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_column_default_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_instant_column_variants_refresh_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_schema_lifecycle_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_schema_default_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_schema_idempotent_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_cross_schema_rename_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_multi_rename_cycle_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_view_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_view_ddl_variants_refresh_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_view_check_option_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_trigger_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_trigger_ddl_variants_refresh_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_trigger_ordering_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_trigger_idempotent_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_rejects_stored_routine_ddl);
    run_ownerless_sql_test_case(test_ownerless_index_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_rename_index_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_ignored_index_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_unique_index_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_primary_key_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_foreign_key_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_foreign_key_actions_cross_process);
    run_ownerless_sql_test_case(test_ownerless_composite_foreign_keys_cross_process);
    run_ownerless_sql_test_case(test_ownerless_foreign_key_deep_cascade_cross_process);
    run_ownerless_sql_test_case(test_ownerless_generated_column_foreign_key_cross_process);
    run_ownerless_sql_test_case(test_ownerless_generated_column_foreign_key_policy);
    run_ownerless_sql_test_case(test_ownerless_cyclic_foreign_key_cross_process);
    run_ownerless_sql_test_case(test_ownerless_cyclic_foreign_key_variants_cross_process);
    run_ownerless_sql_test_case(test_ownerless_foreign_key_rename_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_foreign_key_child_rename_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(
        test_ownerless_foreign_key_cross_schema_rename_refreshes_peer_dictionary
    );
    run_ownerless_sql_test_case(
        test_ownerless_foreign_key_cross_schema_child_rename_refreshes_peer_dictionary
    );
    run_ownerless_sql_test_case(test_ownerless_foreign_key_multi_rename_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(
        test_ownerless_foreign_key_cross_schema_multi_rename_refreshes_peer_dictionary
    );
    run_ownerless_sql_test_case(test_ownerless_check_constraint_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_rejects_table_admin_sql);
    run_ownerless_sql_test_case(test_ownerless_rejects_lock_tables_sql);
    run_ownerless_sql_test_case(test_ownerless_rejects_flush_table_lock_sql);
    run_ownerless_sql_test_case(test_ownerless_rejects_read_uncommitted_isolation);
    run_ownerless_sql_test_case(test_ownerless_rejects_sequence_sql);
    run_ownerless_sql_test_case(test_ownerless_rejects_table_directory_options);
    run_ownerless_sql_test_case(test_ownerless_rejects_special_index_ddl);
    run_ownerless_sql_test_case(test_ownerless_rejects_partition_ddl);
    run_ownerless_sql_test_case(test_ownerless_rejects_tablespace_management_ddl);
    run_ownerless_sql_test_case(test_ownerless_temporary_tablespace_allows_peer_temp_tables);
    run_ownerless_sql_test_case(test_crashed_ownerless_temporary_table_peer_is_recovered);
    run_ownerless_sql_test_case(test_ownerless_rejects_non_innodb_engines);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    run_ownerless_sql_test_case(test_crashed_page_publish_before_append_rebuilds_ownerless_state);
    run_ownerless_sql_test_case(test_crashed_page_publish_rebuilds_ownerless_state);
    run_ownerless_sql_test_case(test_crashed_checkpoint_rebuilds_ownerless_state);
    run_ownerless_sql_test_case(
        test_crashed_visible_publish_without_checkpoint_preserves_committed_update
    );
    run_ownerless_sql_test_case(test_crashed_visible_checkpoint_preserves_committed_update);
    run_ownerless_sql_test_case(
        test_crashed_redo_reservation_blocks_peer_cleanup_until_reopen_rebuilds
    );
    run_ownerless_sql_test_case(
        test_crashed_redo_written_blocks_peer_cleanup_until_reopen_rebuilds
    );
    run_ownerless_sql_test_case(test_crashed_redo_latest_blocks_peer_cleanup_until_reopen_rebuilds);
    run_ownerless_sql_test_case(
        test_crashed_redo_latest_checkpoint_blocks_peer_cleanup_until_reopen_rebuilds
    );
    run_ownerless_sql_test_case(test_redo_gap_blocks_later_writer_until_rebuild);
    run_ownerless_sql_test_case(test_crashed_native_checkpoint_reclaim_preserves_committed_update);
    run_ownerless_sql_test_case(test_native_checkpoint_reclaim_race_preserves_newer_peer_commit);
    run_ownerless_sql_test_case(
        test_consistent_snapshot_start_pin_blocks_live_reclaim_before_execute
    );
    run_ownerless_sql_test_case(test_ownerless_active_pin_reclaims_page_log_with_boundary);
    run_ownerless_sql_test_case(
        test_crashed_trx_registration_blocks_peer_cleanup_until_reopen_rebuilds
    );
    run_ownerless_sql_test_case(
        test_crashed_record_lock_before_grant_blocks_peer_cleanup_until_reopen_rebuilds
    );
    run_ownerless_sql_test_case(
        test_crashed_record_lock_grant_blocks_peer_cleanup_until_reopen_rebuilds
    );
    run_ownerless_sql_test_case(test_crashed_dictionary_ddl_begin_rebuilds_ownerless_state);
    run_ownerless_sql_test_case(
        test_crashed_dictionary_ddl_blocks_peer_cleanup_until_reopen_rebuilds
    );
    run_ownerless_sql_test_case(test_crashed_dictionary_ddl_finish_allows_peer_cleanup);
#endif
    run_ownerless_sql_test_case(
        test_crashed_ownerless_writer_blocks_peer_cleanup_until_reopen_rebuilds
    );
}

static void run_ownerless_sql_test_case(ownerless_test_fn test_fn) {
    pid_t child = fork();

    assert(child >= 0);
    if (child == 0) {
        test_fn();
        _exit(0);
    }
    wait_for_child(child);
}

static void test_two_processes_update_different_innodb_rows(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-different-rows.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t first_child;
    pid_t second_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    first_child = fork();
    assert(first_child >= 0);
    if (first_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        update_first_row_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    second_child = fork();
    assert(second_child >= 0);
    if (second_child == 0) {
        update_second_row(paths);
    }

    signal_pipe(release_pipe[1]);
    wait_for_child(first_child);
    wait_for_child(second_child);
    assert_total_value(paths, 33U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_two_processes_update_same_innodb_row(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-same-row.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t first_child;
    pid_t second_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    first_child = fork();
    assert(first_child >= 0);
    if (first_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        update_first_row_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    second_child = fork();
    assert(second_child >= 0);
    if (second_child == 0) {
        update_first_row_by_two(paths);
    }

    assert(wait_for_concurrency_ownerless_write_waiting_count(database_path, 1U, 5000U) >= 1U);
    signal_pipe(release_pipe[1]);
    wait_for_child(first_child);
    wait_for_child(second_child);
    assert(wait_for_concurrency_ownerless_write_waiting_count(database_path, 0U, 5000U) == 0U);
    assert_total_value(paths, 33U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_two_processes_update_different_innodb_tables(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-different-tables.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int first_ready_pipe[2];
    int first_release_pipe[2];
    int second_ready_pipe[2];
    int second_release_pipe[2];
    pid_t first_child;
    pid_t second_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(first_ready_pipe) == 0);
    assert(pipe(first_release_pipe) == 0);
    assert(pipe(second_ready_pipe) == 0);
    assert(pipe(second_release_pipe) == 0);

    first_child = fork();
    assert(first_child >= 0);
    if (first_child == 0) {
        close(first_ready_pipe[0]);
        close(first_release_pipe[1]);
        close(second_ready_pipe[0]);
        close(second_ready_pipe[1]);
        close(second_release_pipe[0]);
        close(second_release_pipe[1]);
        update_first_table_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = first_ready_pipe[1],
                .release_read_fd = first_release_pipe[0],
            }
        );
    }

    close(first_ready_pipe[1]);
    close(first_release_pipe[0]);
    wait_for_pipe(first_ready_pipe[0]);

    second_child = fork();
    assert(second_child >= 0);
    if (second_child == 0) {
        close(second_ready_pipe[0]);
        close(second_release_pipe[1]);
        close(first_ready_pipe[0]);
        close(first_release_pipe[1]);
        update_second_table_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = second_ready_pipe[1],
                .release_read_fd = second_release_pipe[0],
            }
        );
    }

    close(second_ready_pipe[1]);
    close(second_release_pipe[0]);
    wait_for_pipe(second_ready_pipe[0]);
    signal_pipe(second_release_pipe[1]);
    signal_pipe(first_release_pipe[1]);
    wait_for_child(second_child);
    wait_for_child(first_child);
    assert_table_values(paths);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_concurrent_transaction_commits(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-commit-race.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipes[MYLITE_TEST_COMMIT_RACE_WORKER_COUNT][2];
    int release_pipes[MYLITE_TEST_COMMIT_RACE_WORKER_COUNT][2];
    pid_t workers[MYLITE_TEST_COMMIT_RACE_WORKER_COUNT];
    mylite_db *db;
    char sql[192];
    unsigned long long expected_sum = 0U;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    for (unsigned table_id = 1U; table_id <= MYLITE_TEST_COMMIT_RACE_WORKER_COUNT; ++table_id) {
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "CREATE TABLE app.ownerless_commit_race_%u ("
                "id INT NOT NULL PRIMARY KEY, "
                "value INT NOT NULL"
                ") ENGINE=InnoDB",
                table_id
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_commit_race_%u VALUES (1, 0)",
                table_id
            ) > 0
        );
        exec_ok(db, sql);
        expected_sum += table_id;
    }
    assert(mylite_close(db) == MYLITE_OK);

    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_COMMIT_RACE_WORKER_COUNT; ++worker_id) {
        assert(pipe(ready_pipes[worker_id]) == 0);
        assert(pipe(release_pipes[worker_id]) == 0);
    }

    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_COMMIT_RACE_WORKER_COUNT; ++worker_id) {
        workers[worker_id] = fork();
        assert(workers[worker_id] >= 0);
        if (workers[worker_id] == 0) {
            close(ready_pipes[worker_id][0]);
            close(release_pipes[worker_id][1]);
            commit_race_update_row_after_signal(
                paths,
                worker_id + 1U,
                worker_id + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipes[worker_id][1],
                    .release_read_fd = release_pipes[worker_id][0],
                }
            );
        }
    }

    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_COMMIT_RACE_WORKER_COUNT; ++worker_id) {
        close(ready_pipes[worker_id][1]);
        close(release_pipes[worker_id][0]);
    }
    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_COMMIT_RACE_WORKER_COUNT; ++worker_id) {
        wait_for_pipe(ready_pipes[worker_id][0]);
    }

    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_COMMIT_RACE_WORKER_COUNT; ++worker_id) {
        signal_pipe(release_pipes[worker_id][1]);
    }
    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_COMMIT_RACE_WORKER_COUNT; ++worker_id) {
        wait_for_child(workers[worker_id]);
    }

    assert_commit_race_total(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW, expected_sum);
    assert_commit_race_total(paths, MYLITE_OPEN_READWRITE, expected_sum);
    const uint64_t checkpoint_visible_lsn = assert_commit_race_recovery_anchors(database_path, 0U);

    remove_concurrency_shm(database_path);
    assert_commit_race_total(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW, expected_sum);
    assert_commit_race_recovery_anchors(database_path, checkpoint_visible_lsn);
    assert_commit_race_total(paths, MYLITE_OPEN_READWRITE, expected_sum);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_two_processes_deadlock_on_innodb_rows(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-deadlock.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int first_ready_pipe[2];
    int second_ready_pipe[2];
    int first_release_pipe[2];
    int second_release_pipe[2];
    pid_t first_child;
    pid_t second_child;
    int first_result;
    int second_result;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(first_ready_pipe) == 0);
    assert(pipe(second_ready_pipe) == 0);
    assert(pipe(first_release_pipe) == 0);
    assert(pipe(second_release_pipe) == 0);

    first_child = fork();
    assert(first_child >= 0);
    if (first_child == 0) {
        close(first_ready_pipe[0]);
        close(first_release_pipe[1]);
        close(second_ready_pipe[0]);
        close(second_ready_pipe[1]);
        close(second_release_pipe[0]);
        close(second_release_pipe[1]);
        update_table_pair_after_signal(
            paths,
            "ownerless_a",
            "ownerless_b",
            1U,
            (child_pipes){
                .ready_write_fd = first_ready_pipe[1],
                .release_read_fd = first_release_pipe[0],
            }
        );
    }

    second_child = fork();
    assert(second_child >= 0);
    if (second_child == 0) {
        close(second_ready_pipe[0]);
        close(second_release_pipe[1]);
        close(first_ready_pipe[0]);
        close(first_ready_pipe[1]);
        close(first_release_pipe[0]);
        close(first_release_pipe[1]);
        update_table_pair_after_signal(
            paths,
            "ownerless_b",
            "ownerless_a",
            2U,
            (child_pipes){
                .ready_write_fd = second_ready_pipe[1],
                .release_read_fd = second_release_pipe[0],
            }
        );
    }

    close(first_ready_pipe[1]);
    close(second_ready_pipe[1]);
    close(first_release_pipe[0]);
    close(second_release_pipe[0]);
    wait_for_pipe(first_ready_pipe[0]);
    wait_for_pipe(second_ready_pipe[0]);
    signal_pipe(first_release_pipe[1]);
    assert(wait_for_concurrency_ownerless_write_waiting_count(database_path, 1U, 5000U) >= 1U);
    signal_pipe(second_release_pipe[1]);

    first_result = wait_for_child_result(first_child);
    second_result = wait_for_child_result(second_child);
    if (!((first_result == MYLITE_TEST_CHILD_OK && second_result == MYLITE_TEST_CHILD_DEADLOCK) ||
          (first_result == MYLITE_TEST_CHILD_DEADLOCK && second_result == MYLITE_TEST_CHILD_OK))) {
        fprintf(
            stderr,
            "ownerless deadlock child results: first=%d second=%d\n",
            first_result,
            second_result
        );
        fflush(stderr);
    }
    assert(
        (first_result == MYLITE_TEST_CHILD_OK && second_result == MYLITE_TEST_CHILD_DEADLOCK) ||
        (first_result == MYLITE_TEST_CHILD_DEADLOCK && second_result == MYLITE_TEST_CHILD_OK)
    );
    assert(wait_for_concurrency_ownerless_write_waiting_count(database_path, 0U, 5000U) == 0U);
    assert_table_total_value_is_one_of(paths, 302U, 304U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_gap_lock_blocks_insert(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-gap-lock.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int holder_ready_pipe[2];
    int holder_release_pipe[2];
    pid_t holder_child;
    pid_t inserter_child;
    int inserter_result;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_gap ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "KEY ownerless_gap_value_idx (value)"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_gap VALUES (10, 10), (20, 20)");
    assert(mylite_close(db) == MYLITE_OK);

    assert(pipe(holder_ready_pipe) == 0);
    assert(pipe(holder_release_pipe) == 0);

    holder_child = fork();
    assert(holder_child >= 0);
    if (holder_child == 0) {
        close(holder_ready_pipe[0]);
        close(holder_release_pipe[1]);
        hold_gap_lock_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = holder_ready_pipe[1],
                .release_read_fd = holder_release_pipe[0],
            }
        );
    }

    close(holder_ready_pipe[1]);
    close(holder_release_pipe[0]);
    wait_for_pipe(holder_ready_pipe[0]);

    inserter_child = fork();
    assert(inserter_child >= 0);
    if (inserter_child == 0) {
        close(holder_ready_pipe[0]);
        close(holder_release_pipe[1]);
        insert_gap_row_expect_lock_timeout(paths);
    }

    inserter_result = wait_for_child_result(inserter_child);
    signal_pipe(holder_release_pipe[1]);
    wait_for_child(holder_child);
    assert(inserter_result == MYLITE_TEST_CHILD_LOCK_WAIT_TIMEOUT);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_gap WHERE id = 15") == 0U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_savepoint_rollback_is_peer_visible_after_commit(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-savepoint.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        update_with_savepoint_rollback_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    assert(query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 2") == 20U);
    assert(mylite_close(db) == MYLITE_OK);

    signal_pipe(release_pipe[1]);
    wait_for_child(child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 11U);
    assert(query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 2") == 20U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_serializable_read_blocks_peer_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-serializable.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    pid_t writer_child;
    int writer_result;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_serializable_read_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        update_first_row_expect_lock_timeout(paths);
    }

    writer_result = wait_for_child_result(writer_child);
    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert(writer_result == MYLITE_TEST_CHILD_LOCK_WAIT_TIMEOUT);
    assert_total_value(paths, 30U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_serializable_prevents_write_skew(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-write-skew.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int first_ready_pipe[2];
    int second_ready_pipe[2];
    int first_release_pipe[2];
    int second_release_pipe[2];
    pid_t first_child;
    pid_t second_child;
    int first_result;
    int second_result;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_write_skew ("
        "doctor_id INT NOT NULL PRIMARY KEY, "
        "on_call INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_write_skew VALUES (1, 1), (2, 1)");
    assert(mylite_close(db) == MYLITE_OK);

    assert(pipe(first_ready_pipe) == 0);
    assert(pipe(second_ready_pipe) == 0);
    assert(pipe(first_release_pipe) == 0);
    assert(pipe(second_release_pipe) == 0);

    first_child = fork();
    assert(first_child >= 0);
    if (first_child == 0) {
        close(first_ready_pipe[0]);
        close(first_release_pipe[1]);
        close(second_ready_pipe[0]);
        close(second_ready_pipe[1]);
        close(second_release_pipe[0]);
        close(second_release_pipe[1]);
        run_serializable_write_skew_candidate_after_signal(
            paths,
            1U,
            (child_pipes){
                .ready_write_fd = first_ready_pipe[1],
                .release_read_fd = first_release_pipe[0],
            }
        );
    }

    second_child = fork();
    assert(second_child >= 0);
    if (second_child == 0) {
        close(second_ready_pipe[0]);
        close(second_release_pipe[1]);
        close(first_ready_pipe[0]);
        close(first_ready_pipe[1]);
        close(first_release_pipe[0]);
        close(first_release_pipe[1]);
        run_serializable_write_skew_candidate_after_signal(
            paths,
            2U,
            (child_pipes){
                .ready_write_fd = second_ready_pipe[1],
                .release_read_fd = second_release_pipe[0],
            }
        );
    }

    close(first_ready_pipe[1]);
    close(second_ready_pipe[1]);
    close(first_release_pipe[0]);
    close(second_release_pipe[0]);
    wait_for_pipe(first_ready_pipe[0]);
    wait_for_pipe(second_ready_pipe[0]);
    signal_pipe(first_release_pipe[1]);
    signal_pipe(second_release_pipe[1]);

    first_result = wait_for_child_result(first_child);
    second_result = wait_for_child_result(second_child);
    if (first_result == MYLITE_TEST_CHILD_OK && second_result == MYLITE_TEST_CHILD_OK) {
        fprintf(stderr, "ownerless write-skew child results unexpectedly both committed\n");
        fflush(stderr);
    }
    assert(first_result != MYLITE_TEST_CHILD_OK || second_result != MYLITE_TEST_CHILD_OK);
    assert(
        first_result == MYLITE_TEST_CHILD_OK || first_result == MYLITE_TEST_CHILD_DEADLOCK ||
        first_result == MYLITE_TEST_CHILD_LOCK_WAIT_TIMEOUT
    );
    assert(
        second_result == MYLITE_TEST_CHILD_OK || second_result == MYLITE_TEST_CHILD_DEADLOCK ||
        second_result == MYLITE_TEST_CHILD_LOCK_WAIT_TIMEOUT
    );

    assert_ownerless_write_skew_invariant(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_write_skew_invariant(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_write_skew_invariant(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_write_skew_invariant(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_auto_increment_assigns_distinct_ids(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-auto-inc.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT][2];
    int release_pipe[MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT][2];
    pid_t children[MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT];
    unsigned long long expected_sum = 0U;
    const unsigned long long expected_count =
        MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT * MYLITE_TEST_AUTO_INCREMENT_ROWS_PER_WORKER;
    const unsigned long long expected_id_sum = (expected_count * (expected_count + 1U)) / 2U;
    mylite_db *db;
    unsigned long long actual_min_id;
    unsigned long long actual_max_id;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_auto_inc ("
        "id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert(mylite_close(db) == MYLITE_OK);

    for (unsigned index = 0U; index < MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT; ++index) {
        assert(pipe(ready_pipe[index]) == 0);
        assert(pipe(release_pipe[index]) == 0);
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            close(ready_pipe[index][0]);
            close(release_pipe[index][1]);
            insert_auto_increment_rows_after_signal(
                paths,
                index + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipe[index][1],
                    .release_read_fd = release_pipe[index][0],
                }
            );
        }
        close(ready_pipe[index][1]);
        close(release_pipe[index][0]);
        for (unsigned row = 0U; row < MYLITE_TEST_AUTO_INCREMENT_ROWS_PER_WORKER; ++row) {
            expected_sum += ((index + 1U) * 1000U) + row;
        }
    }

    for (unsigned index = 0U; index < MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT; ++index) {
        wait_for_pipe(ready_pipe[index][0]);
    }
    for (unsigned index = 0U; index < MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT; ++index) {
        signal_pipe(release_pipe[index][1]);
    }
    for (unsigned index = 0U; index < MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT; ++index) {
        wait_for_child(children[index]);
    }

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_auto_inc") == expected_count);
    assert(
        query_unsigned(db, "SELECT COUNT(DISTINCT id) FROM app.ownerless_auto_inc") ==
        expected_count
    );
    actual_min_id = query_unsigned(db, "SELECT MIN(id) FROM app.ownerless_auto_inc");
    actual_max_id = query_unsigned(db, "SELECT MAX(id) FROM app.ownerless_auto_inc");
    assert(actual_min_id == 1U);
    assert(actual_max_id == expected_count);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_auto_inc") == expected_id_sum);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_auto_inc") == expected_sum);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_auto_increment_ddl_refreshes_peer_high_water(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-auto-inc-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t alter_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_auto_inc_ddl ("
        "id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_auto_inc_ddl (value) VALUES (10)");
    assert(query_unsigned(db, "SELECT MAX(id) FROM app.ownerless_auto_inc_ddl") == 1U);
    assert(mylite_close(db) == MYLITE_OK);

    alter_child = fork();
    assert(alter_child >= 0);
    if (alter_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        alter_auto_increment_after_signal(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe_message(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_auto_inc_ddl") == 1U);

    signal_pipe_message(release_pipe[1]);
    wait_for_pipe_message(ready_pipe[0]);
    exec_ok(db, "INSERT INTO app.ownerless_auto_inc_ddl (value) VALUES (5000)");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_auto_inc_ddl WHERE id = 50 AND value = 5000"
        ) == 1U
    );

    signal_pipe_message(release_pipe[1]);
    wait_for_pipe_message(ready_pipe[0]);
    exec_ok(db, "INSERT INTO app.ownerless_auto_inc_ddl (value) VALUES (5100)");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_auto_inc_ddl WHERE id = 51 AND value = 5100"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);

    close(ready_pipe[0]);
    close(release_pipe[1]);
    wait_for_child(alter_child);

    assert_ownerless_auto_increment_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        3U,
        102U,
        10110U,
        51U
    );
    assert_ownerless_auto_increment_ddl_state(paths, MYLITE_OPEN_READWRITE, 3U, 102U, 10110U, 51U);
    remove_concurrency_shm(database_path);
    assert_ownerless_auto_increment_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        3U,
        102U,
        10110U,
        51U
    );
    assert_ownerless_auto_increment_ddl_state(paths, MYLITE_OPEN_READWRITE, 3U, 102U, 10110U, 51U);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "INSERT INTO app.ownerless_auto_inc_ddl (value) VALUES (5200)");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_auto_inc_ddl WHERE id = 52 AND value = 5200"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_ownerless_auto_increment_ddl_state(paths, MYLITE_OPEN_READWRITE, 4U, 154U, 15310U, 52U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_four_processes_mix_ownerless_reads_and_writes(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-four-processes.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int ready_pipe[4][2];
    int release_pipe[4][2];
    pid_t children[4];

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_mix ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_mix VALUES (1, 0), (2, 0), (3, 0)");
    assert(mylite_close(db) == MYLITE_OK);

    for (unsigned index = 0U; index < 4U; ++index) {
        assert(pipe(ready_pipe[index]) == 0);
        assert(pipe(release_pipe[index]) == 0);
    }

    for (unsigned index = 0U; index < 3U; ++index) {
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            close(ready_pipe[index][0]);
            close(release_pipe[index][1]);
            increment_mix_row_after_signal(
                paths,
                index + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipe[index][1],
                    .release_read_fd = release_pipe[index][0],
                }
            );
        }
    }

    children[3] = fork();
    assert(children[3] >= 0);
    if (children[3] == 0) {
        close(ready_pipe[3][0]);
        close(release_pipe[3][1]);
        read_mix_total_after_signal(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[3][1],
                .release_read_fd = release_pipe[3][0],
            }
        );
    }

    for (unsigned index = 0U; index < 4U; ++index) {
        close(ready_pipe[index][1]);
        close(release_pipe[index][0]);
        wait_for_pipe(ready_pipe[index][0]);
    }
    for (unsigned index = 0U; index < 4U; ++index) {
        signal_pipe(release_pipe[index][1]);
    }
    for (unsigned index = 0U; index < 4U; ++index) {
        wait_for_child(children[index]);
    }
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    const unsigned long long native_total =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_mix");
    assert(mylite_close(db) == MYLITE_OK);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    const unsigned long long ownerless_total =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_mix");
    assert(mylite_close(db) == MYLITE_OK);
    if (native_total != 48U || ownerless_total != 48U) {
        fprintf(
            stderr,
            "ownerless mix totals: native=%llu ownerless=%llu\n",
            native_total,
            ownerless_total
        );
        fflush(stderr);
    }
    assert(native_total == 48U);
    assert(ownerless_total == 48U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_independent_table_stress(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-independent-table-stress.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int ready_pipe[MYLITE_TEST_STRESS_WRITER_COUNT + 1U][2];
    int release_pipe[MYLITE_TEST_STRESS_WRITER_COUNT + 1U][2];
    pid_t children[MYLITE_TEST_STRESS_WRITER_COUNT + 1U];
    char sql[160];
    const unsigned stress_iterations = ownerless_stress_iterations();

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    for (unsigned index = 0U; index < MYLITE_TEST_STRESS_WRITER_COUNT; ++index) {
        const unsigned table_id = index + 1U;
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "CREATE TABLE app.ownerless_stress_%u ("
                "id INT NOT NULL PRIMARY KEY, "
                "value INT NOT NULL"
                ") ENGINE=InnoDB",
                table_id
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_stress_%u VALUES (1, 0)",
                table_id
            ) > 0
        );
        exec_ok(db, sql);
    }
    assert(mylite_close(db) == MYLITE_OK);

    for (unsigned index = 0U; index < MYLITE_TEST_STRESS_WRITER_COUNT + 1U; ++index) {
        assert(pipe(ready_pipe[index]) == 0);
        assert(pipe(release_pipe[index]) == 0);
    }

    for (unsigned index = 0U; index < MYLITE_TEST_STRESS_WRITER_COUNT; ++index) {
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            close(ready_pipe[index][0]);
            close(release_pipe[index][1]);
            run_ownerless_stress_writer(
                paths,
                index + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipe[index][1],
                    .release_read_fd = release_pipe[index][0],
                }
            );
        }
    }

    children[MYLITE_TEST_STRESS_WRITER_COUNT] = fork();
    assert(children[MYLITE_TEST_STRESS_WRITER_COUNT] >= 0);
    if (children[MYLITE_TEST_STRESS_WRITER_COUNT] == 0) {
        const unsigned index = MYLITE_TEST_STRESS_WRITER_COUNT;
        close(ready_pipe[index][0]);
        close(release_pipe[index][1]);
        run_ownerless_stress_reader(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[index][1],
                .release_read_fd = release_pipe[index][0],
            }
        );
    }

    for (unsigned index = 0U; index < MYLITE_TEST_STRESS_WRITER_COUNT + 1U; ++index) {
        close(ready_pipe[index][1]);
        close(release_pipe[index][0]);
        wait_for_pipe(ready_pipe[index][0]);
    }
    for (unsigned index = 0U; index < MYLITE_TEST_STRESS_WRITER_COUNT + 1U; ++index) {
        signal_pipe(release_pipe[index][1]);
    }
    for (unsigned index = 0U; index < MYLITE_TEST_STRESS_WRITER_COUNT + 1U; ++index) {
        wait_for_child(children[index]);
    }

    assert_ownerless_stress_total(paths, MYLITE_TEST_STRESS_WRITER_COUNT * stress_iterations);
    assert_concurrency_wal_checkpointed(database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_stress_total(paths, MYLITE_TEST_STRESS_WRITER_COUNT * stress_iterations);
    assert_concurrency_wal_checkpointed(database_path);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_concurrent_ddl_stress(void) {
    enum {
        dml_worker_base = MYLITE_TEST_DDL_STRESS_WORKER_COUNT,
        reader_index =
            MYLITE_TEST_DDL_STRESS_WORKER_COUNT + MYLITE_TEST_DDL_STRESS_DML_WORKER_COUNT,
        child_count =
            MYLITE_TEST_DDL_STRESS_WORKER_COUNT + MYLITE_TEST_DDL_STRESS_DML_WORKER_COUNT + 1U
    };

    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-ddl-stress.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[child_count][2];
    int release_pipe[child_count][2];
    pid_t children[child_count];
    const unsigned ddl_rounds = ownerless_ddl_stress_rounds();
    const unsigned dml_iterations = ddl_rounds * MYLITE_TEST_DDL_STRESS_DML_UPDATES_PER_ROUND;
    const unsigned long long expected_total =
        30U + (MYLITE_TEST_DDL_STRESS_DML_WORKER_COUNT * dml_iterations);

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    for (unsigned index = 0U; index < child_count; ++index) {
        assert(pipe(ready_pipe[index]) == 0);
        assert(pipe(release_pipe[index]) == 0);
    }

    for (unsigned index = 0U; index < MYLITE_TEST_DDL_STRESS_WORKER_COUNT; ++index) {
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            close(ready_pipe[index][0]);
            close(release_pipe[index][1]);
            run_ownerless_ddl_stress_worker(
                paths,
                index + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipe[index][1],
                    .release_read_fd = release_pipe[index][0],
                }
            );
        }
    }

    for (unsigned index = 0U; index < MYLITE_TEST_DDL_STRESS_DML_WORKER_COUNT; ++index) {
        const unsigned child_index = dml_worker_base + index;
        children[child_index] = fork();
        assert(children[child_index] >= 0);
        if (children[child_index] == 0) {
            close(ready_pipe[child_index][0]);
            close(release_pipe[child_index][1]);
            run_ownerless_ddl_stress_dml_worker(
                paths,
                index + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipe[child_index][1],
                    .release_read_fd = release_pipe[child_index][0],
                }
            );
        }
    }

    children[reader_index] = fork();
    assert(children[reader_index] >= 0);
    if (children[reader_index] == 0) {
        close(ready_pipe[reader_index][0]);
        close(release_pipe[reader_index][1]);
        run_ownerless_ddl_stress_reader(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[reader_index][1],
                .release_read_fd = release_pipe[reader_index][0],
            }
        );
    }

    for (unsigned index = 0U; index < child_count; ++index) {
        close(ready_pipe[index][1]);
        close(release_pipe[index][0]);
        wait_for_pipe(ready_pipe[index][0]);
    }
    for (unsigned index = 0U; index < child_count; ++index) {
        signal_pipe(release_pipe[index][1]);
    }
    for (unsigned index = 0U; index < child_count; ++index) {
        wait_for_child(children[index]);
    }

    assert_ownerless_ddl_stress_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        expected_total
    );
    assert_ownerless_ddl_stress_state(paths, MYLITE_OPEN_READWRITE, expected_total);
    remove_concurrency_shm(database_path);
    assert_ownerless_ddl_stress_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        expected_total
    );
    assert_ownerless_ddl_stress_state(paths, MYLITE_OPEN_READWRITE, expected_total);
    assert_concurrency_wal_checkpointed(database_path);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_temporary_table_stress(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-temporary-table-stress.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[MYLITE_TEST_TEMP_STRESS_WORKER_COUNT][2];
    int release_pipe[MYLITE_TEST_TEMP_STRESS_WORKER_COUNT][2];
    pid_t children[MYLITE_TEST_TEMP_STRESS_WORKER_COUNT];
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    for (unsigned index = 0U; index < MYLITE_TEST_TEMP_STRESS_WORKER_COUNT; ++index) {
        assert(pipe(ready_pipe[index]) == 0);
        assert(pipe(release_pipe[index]) == 0);
    }

    for (unsigned index = 0U; index < MYLITE_TEST_TEMP_STRESS_WORKER_COUNT; ++index) {
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            close(ready_pipe[index][0]);
            close(release_pipe[index][1]);
            run_ownerless_temp_stress_worker(
                paths,
                index + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipe[index][1],
                    .release_read_fd = release_pipe[index][0],
                }
            );
        }
    }

    for (unsigned index = 0U; index < MYLITE_TEST_TEMP_STRESS_WORKER_COUNT; ++index) {
        close(ready_pipe[index][1]);
        close(release_pipe[index][0]);
        wait_for_pipe(ready_pipe[index][0]);
    }

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(mylite_close(db) == MYLITE_OK);

    for (unsigned index = 0U; index < MYLITE_TEST_TEMP_STRESS_WORKER_COUNT; ++index) {
        signal_pipe(release_pipe[index][1]);
    }
    for (unsigned index = 0U; index < MYLITE_TEST_TEMP_STRESS_WORKER_COUNT; ++index) {
        wait_for_child(children[index]);
    }

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_temp_stress ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_temp_stress VALUES (1, 41)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_temp_stress") == 41U);
    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_temp_stress_permanent_table(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_temp_stress_permanent_table(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_temp_stress_permanent_table(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_temp_stress_permanent_table(paths, MYLITE_OPEN_READWRITE);
    assert_concurrency_wal_checkpointed(database_path);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_transaction_mix_stress(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-tx-stress.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[MYLITE_TEST_TX_STRESS_WORKER_COUNT][2];
    int release_pipe[MYLITE_TEST_TX_STRESS_WORKER_COUNT][2];
    pid_t children[MYLITE_TEST_TX_STRESS_WORKER_COUNT];
    mylite_db *db;
    char sql[192];
    const unsigned rounds = ownerless_tx_stress_rounds();

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    for (unsigned worker_id = 1U; worker_id <= MYLITE_TEST_TX_STRESS_WORKER_COUNT; ++worker_id) {
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "CREATE TABLE app.ownerless_tx_stress_%u ("
                "id INT NOT NULL PRIMARY KEY, "
                "value BIGINT UNSIGNED NOT NULL, "
                "version INT UNSIGNED NOT NULL"
                ") ENGINE=InnoDB",
                worker_id
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_tx_stress_%u VALUES "
                "(1, 0, 0), (2, 0, 0), (3, 0, 0)",
                worker_id
            ) > 0
        );
        exec_ok(db, sql);
    }
    assert(mylite_close(db) == MYLITE_OK);

    for (unsigned index = 0U; index < MYLITE_TEST_TX_STRESS_WORKER_COUNT; ++index) {
        assert(pipe(ready_pipe[index]) == 0);
        assert(pipe(release_pipe[index]) == 0);
    }

    for (unsigned index = 0U; index < MYLITE_TEST_TX_STRESS_WORKER_COUNT; ++index) {
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            close(ready_pipe[index][0]);
            close(release_pipe[index][1]);
            run_ownerless_tx_stress_worker(
                paths,
                index + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipe[index][1],
                    .release_read_fd = release_pipe[index][0],
                }
            );
        }
    }

    for (unsigned index = 0U; index < MYLITE_TEST_TX_STRESS_WORKER_COUNT; ++index) {
        close(ready_pipe[index][1]);
        close(release_pipe[index][0]);
        wait_for_pipe(ready_pipe[index][0]);
    }
    for (unsigned index = 0U; index < MYLITE_TEST_TX_STRESS_WORKER_COUNT; ++index) {
        signal_pipe(release_pipe[index][1]);
    }
    for (unsigned index = 0U; index < MYLITE_TEST_TX_STRESS_WORKER_COUNT; ++index) {
        wait_for_child(children[index]);
    }

    assert_ownerless_tx_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        rounds
    );
    assert_ownerless_tx_stress_totals(paths, MYLITE_OPEN_READWRITE, rounds);
    remove_concurrency_shm(database_path);
    assert_ownerless_tx_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        rounds
    );
    assert_ownerless_tx_stress_totals(paths, MYLITE_OPEN_READWRITE, rounds);
    assert_concurrency_wal_checkpointed(database_path);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_checksum_stress(void) {
    enum {
        reader_index = MYLITE_TEST_CHECKSUM_STRESS_WORKER_COUNT,
        child_count = MYLITE_TEST_CHECKSUM_STRESS_WORKER_COUNT + 1U
    };

    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-checksum-stress.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[child_count][2];
    int release_pipe[child_count][2];
    pid_t children[child_count];
    mylite_db *db;
    char sql[256];
    const unsigned rounds = ownerless_checksum_stress_rounds();
    unsigned long long expected_sum = 0U;
    unsigned long long expected_versions = 0U;
    unsigned long long expected_weighted_sum = 0U;

    ownerless_checksum_stress_expected(
        rounds,
        &expected_sum,
        &expected_versions,
        &expected_weighted_sum
    );

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_checksum_stress ("
        "id INT NOT NULL PRIMARY KEY, "
        "worker INT NOT NULL, "
        "slot INT NOT NULL, "
        "value BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "version INT UNSIGNED NOT NULL DEFAULT 0, "
        "KEY worker_slot (worker, slot)"
        ") ENGINE=InnoDB"
    );
    for (unsigned worker_id = 1U; worker_id <= MYLITE_TEST_CHECKSUM_STRESS_WORKER_COUNT;
         ++worker_id) {
        for (unsigned slot = 1U; slot <= MYLITE_TEST_CHECKSUM_STRESS_ROWS_PER_WORKER; ++slot) {
            const unsigned row_id = (worker_id * 1000U) + slot;
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "INSERT INTO app.ownerless_checksum_stress "
                    "VALUES (%u, %u, %u, 0, 0)",
                    row_id,
                    worker_id,
                    slot
                ) > 0
            );
            exec_ok(db, sql);
        }
    }
    assert(mylite_close(db) == MYLITE_OK);

    for (unsigned index = 0U; index < child_count; ++index) {
        assert(pipe(ready_pipe[index]) == 0);
        assert(pipe(release_pipe[index]) == 0);
    }

    for (unsigned index = 0U; index < MYLITE_TEST_CHECKSUM_STRESS_WORKER_COUNT; ++index) {
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            close(ready_pipe[index][0]);
            close(release_pipe[index][1]);
            const child_pipes pipes = {
                .ready_write_fd = ready_pipe[index][1],
                .release_read_fd = release_pipe[index][0],
            };
            if (index % 2U == 0U) {
                run_ownerless_checksum_stress_writer(paths, index + 1U, pipes);
            } else {
                run_ownerless_prepared_checksum_stress_writer(paths, index + 1U, pipes);
            }
        }
    }

    children[reader_index] = fork();
    assert(children[reader_index] >= 0);
    if (children[reader_index] == 0) {
        close(ready_pipe[reader_index][0]);
        close(release_pipe[reader_index][1]);
        run_ownerless_checksum_stress_reader(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[reader_index][1],
                .release_read_fd = release_pipe[reader_index][0],
            }
        );
    }

    for (unsigned index = 0U; index < child_count; ++index) {
        close(ready_pipe[index][1]);
        close(release_pipe[index][0]);
        wait_for_pipe(ready_pipe[index][0]);
    }
    for (unsigned index = 0U; index < child_count; ++index) {
        signal_pipe(release_pipe[index][1]);
    }
    for (unsigned index = 0U; index < child_count; ++index) {
        wait_for_child(children[index]);
    }

    assert_ownerless_checksum_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        expected_sum,
        expected_versions,
        expected_weighted_sum
    );
    assert_ownerless_checksum_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE,
        expected_sum,
        expected_versions,
        expected_weighted_sum
    );
    remove_concurrency_shm(database_path);
    assert_ownerless_checksum_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        expected_sum,
        expected_versions,
        expected_weighted_sum
    );
    assert_ownerless_checksum_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE,
        expected_sum,
        expected_versions,
        expected_weighted_sum
    );
    assert_concurrency_wal_checkpointed(database_path);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_random_transaction_stress(void) {
    enum {
        reader_index = MYLITE_TEST_RANDOM_TX_STRESS_WORKER_COUNT,
        child_count = MYLITE_TEST_RANDOM_TX_STRESS_WORKER_COUNT + 1U
    };

    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-random-tx-stress.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[child_count][2];
    int release_pipe[child_count][2];
    pid_t children[child_count];
    mylite_db *db;
    char sql[256];
    const unsigned rounds = ownerless_random_tx_stress_rounds();
    unsigned long long expected_sum = 0U;
    unsigned long long expected_versions = 0U;
    unsigned long long expected_weighted_sum = 0U;

    ownerless_random_tx_stress_expected(
        rounds,
        &expected_sum,
        &expected_versions,
        &expected_weighted_sum
    );

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_random_tx_stress ("
        "id INT NOT NULL PRIMARY KEY, "
        "value BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "version INT UNSIGNED NOT NULL DEFAULT 0, "
        "pad VARBINARY(3600) NOT NULL"
        ") ENGINE=InnoDB"
    );
    for (unsigned row_id = 1U; row_id <= MYLITE_TEST_RANDOM_TX_STRESS_ROW_COUNT; ++row_id) {
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_random_tx_stress VALUES "
                "(%u, 0, 0, REPEAT('x', %u))",
                row_id,
                MYLITE_TEST_RANDOM_TX_STRESS_PAD_BYTES
            ) > 0
        );
        exec_ok(db, sql);
    }
    assert(mylite_close(db) == MYLITE_OK);

    for (unsigned index = 0U; index < child_count; ++index) {
        assert(pipe(ready_pipe[index]) == 0);
        assert(pipe(release_pipe[index]) == 0);
    }

    for (unsigned index = 0U; index < MYLITE_TEST_RANDOM_TX_STRESS_WORKER_COUNT; ++index) {
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            close(ready_pipe[index][0]);
            close(release_pipe[index][1]);
            run_ownerless_random_tx_stress_worker(
                paths,
                index + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipe[index][1],
                    .release_read_fd = release_pipe[index][0],
                }
            );
        }
    }

    children[reader_index] = fork();
    assert(children[reader_index] >= 0);
    if (children[reader_index] == 0) {
        close(ready_pipe[reader_index][0]);
        close(release_pipe[reader_index][1]);
        run_ownerless_random_tx_stress_reader(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[reader_index][1],
                .release_read_fd = release_pipe[reader_index][0],
            }
        );
    }

    for (unsigned index = 0U; index < child_count; ++index) {
        close(ready_pipe[index][1]);
        close(release_pipe[index][0]);
        wait_for_pipe(ready_pipe[index][0]);
    }
    for (unsigned index = 0U; index < child_count; ++index) {
        signal_pipe(release_pipe[index][1]);
    }
    for (unsigned index = 0U; index < child_count; ++index) {
        wait_for_child(children[index]);
    }

    assert_ownerless_random_tx_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        expected_sum,
        expected_versions,
        expected_weighted_sum
    );
    assert_ownerless_random_tx_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE,
        expected_sum,
        expected_versions,
        expected_weighted_sum
    );
    remove_concurrency_shm(database_path);
    assert_ownerless_random_tx_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        expected_sum,
        expected_versions,
        expected_weighted_sum
    );
    assert_ownerless_random_tx_stress_totals(
        paths,
        MYLITE_OPEN_READWRITE,
        expected_sum,
        expected_versions,
        expected_weighted_sum
    );
    assert_concurrency_wal_checkpointed(database_path);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_graph_stress(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-graph-stress.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT][2];
    int release_pipe[MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT][2];
    pid_t children[MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT];
    mylite_db *db;
    char sql[512];
    const unsigned rounds = ownerless_fk_graph_stress_rounds();

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_graph_root ("
        "id INT NOT NULL PRIMARY KEY, "
        "worker_id INT NOT NULL, "
        "kind INT NOT NULL, "
        "value BIGINT UNSIGNED NOT NULL, "
        "version INT UNSIGNED NOT NULL, "
        "UNIQUE KEY ownerless_fk_graph_root_worker_kind (worker_id, kind)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_graph_cascade_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "worker_id INT NOT NULL, "
        "root_id INT NOT NULL, "
        "value BIGINT UNSIGNED NOT NULL, "
        "version INT UNSIGNED NOT NULL, "
        "INDEX ownerless_fk_graph_cascade_root_idx (root_id), "
        "CONSTRAINT ownerless_fk_graph_cascade_fk "
        "FOREIGN KEY (root_id) "
        "REFERENCES app.ownerless_fk_graph_root (id) "
        "ON UPDATE CASCADE "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_graph_setnull_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "worker_id INT NOT NULL, "
        "root_id INT NULL, "
        "value BIGINT UNSIGNED NOT NULL, "
        "version INT UNSIGNED NOT NULL, "
        "INDEX ownerless_fk_graph_setnull_root_idx (root_id), "
        "CONSTRAINT ownerless_fk_graph_setnull_fk "
        "FOREIGN KEY (root_id) "
        "REFERENCES app.ownerless_fk_graph_root (id) "
        "ON UPDATE CASCADE "
        "ON DELETE SET NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_graph_restrict_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "worker_id INT NOT NULL, "
        "root_id INT NOT NULL, "
        "value BIGINT UNSIGNED NOT NULL, "
        "version INT UNSIGNED NOT NULL, "
        "INDEX ownerless_fk_graph_restrict_root_idx (root_id), "
        "CONSTRAINT ownerless_fk_graph_restrict_fk "
        "FOREIGN KEY (root_id) "
        "REFERENCES app.ownerless_fk_graph_root (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE RESTRICT"
        ") ENGINE=InnoDB"
    );
    for (unsigned worker_id = 1U; worker_id <= MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT;
         ++worker_id) {
        const unsigned cascade_root = ownerless_fk_graph_stress_initial_id(worker_id, 1U);
        const unsigned setnull_root = ownerless_fk_graph_stress_initial_id(worker_id, 2U);
        const unsigned restrict_root = ownerless_fk_graph_stress_initial_id(worker_id, 3U);

        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_fk_graph_root VALUES "
                "(%u, %u, 1, 0, 0), "
                "(%u, %u, 2, 0, 0), "
                "(%u, %u, 3, 0, 0)",
                cascade_root,
                worker_id,
                setnull_root,
                worker_id,
                restrict_root,
                worker_id
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_fk_graph_cascade_child "
                "VALUES (%u, %u, %u, 0, 0)",
                cascade_root + 1U,
                worker_id,
                cascade_root
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_fk_graph_setnull_child "
                "VALUES (%u, %u, %u, 0, 0)",
                setnull_root + 1U,
                worker_id,
                setnull_root
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_fk_graph_restrict_child "
                "VALUES (%u, %u, %u, 0, 0)",
                restrict_root + 1U,
                worker_id,
                restrict_root
            ) > 0
        );
        exec_ok(db, sql);
    }
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);

    for (unsigned index = 0U; index < MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT; ++index) {
        assert(pipe(ready_pipe[index]) == 0);
        assert(pipe(release_pipe[index]) == 0);
    }

    for (unsigned index = 0U; index < MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT; ++index) {
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            close(ready_pipe[index][0]);
            close(release_pipe[index][1]);
            run_ownerless_fk_graph_stress_worker(
                paths,
                index + 1U,
                (child_pipes){
                    .ready_write_fd = ready_pipe[index][1],
                    .release_read_fd = release_pipe[index][0],
                }
            );
        }
    }

    for (unsigned index = 0U; index < MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT; ++index) {
        close(ready_pipe[index][1]);
        close(release_pipe[index][0]);
        wait_for_pipe(ready_pipe[index][0]);
    }
    for (unsigned index = 0U; index < MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT; ++index) {
        signal_pipe(release_pipe[index][1]);
    }
    for (unsigned index = 0U; index < MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT; ++index) {
        wait_for_child(children[index]);
    }

    assert_ownerless_fk_graph_stress_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        rounds
    );
    assert_ownerless_fk_graph_stress_state(paths, MYLITE_OPEN_READWRITE, rounds);
    remove_concurrency_shm(database_path);
    assert_ownerless_fk_graph_stress_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        rounds
    );
    assert_ownerless_fk_graph_stress_state(paths, MYLITE_OPEN_READWRITE, rounds);
    assert_concurrency_wal_checkpointed(database_path);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_purge_preserves_cross_process_snapshot(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-purge-snapshot.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        churn_ownerless_history(paths);
    }
    wait_for_child(writer_child);

    assert_ownerless_total_value(paths, 30U + MYLITE_TEST_PURGE_HISTORY_UPDATES);
    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert_total_value(paths, 30U + MYLITE_TEST_PURGE_HISTORY_UPDATES);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_process_reads_committed_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-committed-read.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 17U);
    assert_concurrency_wal_has_page_versions_or_checkpoint(database_path);
    if (!concurrency_wal_is_checkpointed(database_path)) {
        assert_concurrency_page_index_has_entries(database_path);
    }
    assert(mylite_close(reader) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    if (!concurrency_wal_is_checkpointed(database_path)) {
        assert_concurrency_page_index_has_entries(database_path);
    }
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 17U);
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_prepared_process_reads_committed_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-prepared-committed-read.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    mylite_stmt *stmt;
    const char *tail;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    stmt = NULL;
    tail = NULL;
    assert(
        mylite_prepare(
            reader,
            "SELECT value FROM app.ownerless_sql WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            &tail
        ) == MYLITE_OK
    );
    assert(stmt != NULL);
    assert(tail != NULL && *tail == '\0');
    assert(mylite_bind_parameter_count(stmt) == 1U);
    assert(mylite_bind_int64(stmt, 1, 1) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_uint64(stmt, 0) == 10U);
    assert(mylite_step(stmt) == MYLITE_DONE);

    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(mylite_reset(stmt) == MYLITE_OK);
    assert(mylite_bind_int64(stmt, 1, 1) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_uint64(stmt, 0) == 17U);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_finalize(stmt) == MYLITE_OK);
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_transaction_first_read_sees_committed_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-transaction-first-read.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    exec_ok(reader, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(reader, "START TRANSACTION");

    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 17U);
    exec_ok(reader, "COMMIT");
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_prepared_transaction_first_read_sees_committed_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-prepared-transaction-first-read.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    mylite_stmt *stmt;
    const char *tail;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    exec_ok(reader, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(reader, "START TRANSACTION");

    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    stmt = NULL;
    tail = NULL;
    assert(
        mylite_prepare(
            reader,
            "SELECT value FROM app.ownerless_sql WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            &tail
        ) == MYLITE_OK
    );
    assert(stmt != NULL);
    assert(tail != NULL && *tail == '\0');
    assert(mylite_bind_int64(stmt, 1, 1) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_uint64(stmt, 0) == 17U);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_finalize(stmt) == MYLITE_OK);
    exec_ok(reader, "COMMIT");
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_transaction_with_local_write_first_read_sees_committed_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-local-write-first-read.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    exec_ok(reader, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(reader, "START TRANSACTION");
    exec_ok(reader, "UPDATE app.ownerless_b SET value = value + 1 WHERE id = 1");

    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 17U);
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_b WHERE id = 1") == 201U);
    exec_ok(reader, "COMMIT");
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_transaction_with_local_write_snapshot_hides_later_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-local-write-snapshot.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    exec_ok(reader, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(reader, "START TRANSACTION");
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    exec_ok(reader, "UPDATE app.ownerless_b SET value = value + 1 WHERE id = 1");

    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_b WHERE id = 1") == 201U);
    exec_ok(reader, "COMMIT");
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_consistent_snapshot_transaction_hides_later_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-consistent-snapshot.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(reader, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(reader, "START TRANSACTION WITH CONSISTENT SNAPSHOT");

    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    exec_ok(reader, "COMMIT");
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_read_committed_transaction_observes_later_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-read-committed-snapshot.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(reader, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
    exec_ok(reader, "START TRANSACTION");
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);

    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 17U);
    exec_ok(reader, "COMMIT");
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_next_read_committed_transaction_observes_later_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-next-read-committed-snapshot.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(reader, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED");
    exec_ok(reader, "START TRANSACTION");
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);

    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 17U);
    exec_ok(reader, "COMMIT");
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_shared_readonly_process_reads_committed_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-shared-readonly.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *reader;
    mylite_stmt *stmt = NULL;
    int start_pipe[2];
    pid_t writer_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(start_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(start_pipe[1]);
        update_first_row_by_seven_after_signal(paths, start_pipe[0]);
    }

    close(start_pipe[0]);
    reader = open_database(paths, MYLITE_OPEN_READONLY | MYLITE_OPEN_SHARED_READONLY);
    assert(query_unsigned(reader, "SELECT @@read_only") == 1U);
    {
        mylite_db *same_process_writer = NULL;
        const int writer_result = open_database_result(
            paths,
            MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
            &same_process_writer
        );
        assert(writer_result == MYLITE_BUSY);
        assert(same_process_writer == NULL);
    }
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    assert(
        mylite_prepare(
            reader,
            "SELECT value FROM app.ownerless_sql WHERE id = 1",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_OK
    );
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_int64(stmt, 0) == 10);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_finalize(stmt) == MYLITE_OK);
    stmt = NULL;
    expect_readonly_exec_error(reader, "UPDATE app.ownerless_sql SET value = 11 WHERE id = 1");
    expect_readonly_exec_error(reader, "CREATE TABLE app.readonly_blocked (id INT) ENGINE=InnoDB");
    expect_readonly_exec_error(
        reader,
        "SELECT value FROM app.ownerless_sql WHERE id = 1 FOR UPDATE"
    );
    expect_readonly_exec_error(reader, "SET SESSION TRANSACTION READ WRITE");
    expect_readonly_exec_error(reader, "START TRANSACTION READ WRITE");
    assert(
        mylite_prepare(
            reader,
            "UPDATE app.ownerless_sql SET value = 11 WHERE id = 1",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_READONLY
    );
    assert(stmt == NULL);
    exec_ok(reader, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(reader, "START TRANSACTION READ ONLY");
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    exec_ok(reader, "COMMIT");
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 17U);
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_native_checkpoint_evidence(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-native-checkpoint-evidence.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    uint64_t current_lsn;
    uint64_t checkpoint_lsn;
    uint64_t updated_lsn;
    uint64_t updated_checkpoint_lsn;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_checkpoint_evidence ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_checkpoint_evidence VALUES (1, 10)");

    current_lsn = mylite_ownerless_innodb_current_lsn();
    checkpoint_lsn = mylite_ownerless_innodb_checkpoint_lsn();
    assert(current_lsn > 0U);
    assert(checkpoint_lsn > 0U);
    assert(checkpoint_lsn <= current_lsn);

    exec_ok(db, "UPDATE app.ownerless_checkpoint_evidence SET value = 11 WHERE id = 1");
    updated_lsn = mylite_ownerless_innodb_current_lsn();
    updated_checkpoint_lsn = mylite_ownerless_innodb_checkpoint_lsn();
    assert(updated_lsn >= current_lsn);
    assert(updated_checkpoint_lsn >= checkpoint_lsn);
    assert(updated_checkpoint_lsn <= updated_lsn);
    assert(query_unsigned(db, "SELECT value FROM app.ownerless_checkpoint_evidence") == 11U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_native_checkpoint_reclaims_page_log(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-native-checkpoint-reclaim.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    char insert_sql[176];

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_native_reclaim ("
        "id INT NOT NULL PRIMARY KEY, "
        "value VARBINARY(4000) NOT NULL"
        ") ENGINE=InnoDB"
    );
    for (unsigned id = 1U; id <= 32U; ++id) {
        assert(
            snprintf(
                insert_sql,
                sizeof(insert_sql),
                "INSERT INTO app.ownerless_native_reclaim VALUES (%u, REPEAT('z', 4000))",
                id
            ) > 0
        );
        exec_ok(db, insert_sql);
    }
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_native_reclaim") == 32U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_native_reclaim") == 32U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_native_reclaim") == 32U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_live_idle_peer_reclaims_page_log(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-live-idle-reclaim.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t peer_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 5 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    signal_pipe(release_pipe[1]);
    wait_for_child(peer_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_live_snapshot_pin_blocks_page_log_reclaim(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-live-snapshot-pin-reclaim.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 5 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!concurrency_wal_is_checkpointed(database_path));

    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert(concurrency_wal_is_checkpointed(database_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_live_snapshot_pin_synthesizes_page_boundary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-live-snapshot-boundary.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    mylite_db *db;
    uint64_t snapshot_lsn;
    unsigned boundary_record_count;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(concurrency_wal_is_checkpointed(database_path));
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    snapshot_lsn = read_concurrency_checkpoint_visible_lsn(database_path);
    assert(snapshot_lsn > 0U);
    assert(count_concurrency_wal_records_at_or_before(database_path, snapshot_lsn) == 0U);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 5 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!concurrency_wal_is_checkpointed(database_path));

    boundary_record_count = count_concurrency_wal_records_at_or_before(database_path, snapshot_lsn);
    assert(boundary_record_count > 0U);

    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert(concurrency_wal_is_checkpointed(database_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_active_reader_pressure_reclaims_after_release(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-active-reader-pressure.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    const unsigned rounds =
        ownerless_unsigned_env("MYLITE_OWNERLESS_ACTIVE_READER_PRESSURE_ROUNDS", 8U, 500U);
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(concurrency_wal_is_checkpointed(database_path));
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    for (unsigned round = 0U; round < rounds; ++round) {
        const unsigned expected_sum = 31U + round;

        db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1");
        assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == expected_sum);
        assert(mylite_close(db) == MYLITE_OK);
        assert(!concurrency_wal_is_checkpointed(database_path));
        assert(count_concurrency_wal_records_at_or_before(database_path, UINT64_MAX) > 0U);
    }

    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert(concurrency_wal_is_checkpointed(database_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U + rounds);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U + rounds);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_active_reader_pressure_limit_blocks_writes(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-active-reader-pressure-limit.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    mylite_db *db;
    mylite_stmt *stmt;
    const char *tail;
    off_t retained_wal_size;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(concurrency_wal_is_checkpointed(database_path));
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!concurrency_wal_is_checkpointed(database_path));
    retained_wal_size = concurrency_wal_size(database_path);
    assert(retained_wal_size > 0);

    db = open_database_with_page_log_limit(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        (unsigned long long)retained_wal_size
    );
    expect_exec_busy(
        db,
        "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 2",
        "pressure limit"
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);

    stmt = NULL;
    tail = NULL;
    assert(
        mylite_prepare(
            db,
            "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 2",
            MYLITE_NUL_TERMINATED,
            &stmt,
            &tail
        ) == MYLITE_OK
    );
    assert(stmt != NULL);
    assert(tail != NULL && *tail == '\0');
    assert(mylite_step(stmt) == MYLITE_BUSY);
    assert(mylite_errcode(db) == MYLITE_BUSY);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);

    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert(concurrency_wal_is_checkpointed(database_path));

    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_finalize(stmt) == MYLITE_OK);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 32U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 32U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_active_reader_pressure_limit_blocks_write_classes(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-active-reader-pressure-write-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    mylite_db *db;
    off_t retained_wal_size;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(concurrency_wal_is_checkpointed(database_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_pressure_policy ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_pressure_policy VALUES (1, 10), (2, 20)");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_pressure_drop ("
        "id INT NOT NULL PRIMARY KEY"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_pressure_drop VALUES (1)");
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!concurrency_wal_is_checkpointed(database_path));
    retained_wal_size = concurrency_wal_size(database_path);
    assert(retained_wal_size > 0);

    db = open_database_with_page_log_limit(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        (unsigned long long)retained_wal_size
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_pressure_policy") == 30U);
    exec_ok(db, "START TRANSACTION");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_pressure_policy") == 2U);
    exec_ok(db, "ROLLBACK");

    expect_exec_busy(
        db,
        "INSERT INTO app.ownerless_pressure_policy VALUES (3, 30)",
        "pressure limit"
    );
    expect_exec_busy(
        db,
        "UPDATE app.ownerless_pressure_policy SET value = value + 2 WHERE id = 2",
        "pressure limit"
    );
    expect_exec_busy(
        db,
        "DELETE FROM app.ownerless_pressure_policy WHERE id = 1",
        "pressure limit"
    );
    expect_exec_busy(
        db,
        "CREATE TABLE app.ownerless_pressure_created ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB",
        "pressure limit"
    );
    expect_exec_busy(
        db,
        "ALTER TABLE app.ownerless_pressure_policy "
        "ADD COLUMN note VARCHAR(8) NOT NULL DEFAULT 'ok'",
        "pressure limit"
    );
    expect_exec_busy(db, "DROP TABLE app.ownerless_pressure_drop", "pressure limit");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_pressure_policy") == 30U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_pressure_policy") == 2U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_pressure_policy' "
            "AND column_name = 'note'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_pressure_created'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_pressure_drop'"
        ) == 1U
    );

    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert(concurrency_wal_is_checkpointed(database_path));

    exec_ok(db, "INSERT INTO app.ownerless_pressure_policy VALUES (3, 30)");
    exec_ok(db, "UPDATE app.ownerless_pressure_policy SET value = value + 2 WHERE id = 2");
    exec_ok(db, "DELETE FROM app.ownerless_pressure_policy WHERE id = 1");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_pressure_created ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_pressure_created VALUES (1, 70)");
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_pressure_policy "
        "ADD COLUMN note VARCHAR(8) NOT NULL DEFAULT 'ok'"
    );
    exec_ok(db, "DROP TABLE app.ownerless_pressure_drop");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_pressure_policy") == 52U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_pressure_policy "
            "WHERE note = 'ok'"
        ) == 2U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_pressure_created") == 70U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    assert_ownerless_pressure_write_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_pressure_write_policy_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_pressure_write_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_pressure_write_policy_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_active_reader_pressure_diagnostics(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-active-reader-pressure-diagnostics.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    mylite_db *db;
    mylite_ownerless_pressure_info info = {
        .size = sizeof(info),
    };
    off_t retained_wal_size;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(concurrency_wal_is_checkpointed(database_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(mylite_ownerless_pressure_status(db, &info) == MYLITE_OK);
    assert(info.size == sizeof(info));
    assert(info.active_page_version_pin_count == 0U);
    assert(info.page_version_wal_limit_reached == 0);
    assert(info.oldest_page_version_pin_lsn == 0U);
    assert(info.page_version_wal_bytes == 0U);
    assert(info.page_version_wal_limit_bytes == 0U);
    assert(mylite_close(db) == MYLITE_OK);

    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    info.size = sizeof(info);
    assert(mylite_ownerless_pressure_status(db, &info) == MYLITE_OK);
    assert(info.active_page_version_pin_count == 1U);
    assert(info.page_version_wal_limit_reached == 0);
    assert(info.oldest_page_version_pin_lsn > 0U);
    assert(info.page_version_wal_bytes == (unsigned long long)concurrency_wal_size(database_path));
    assert(info.page_version_wal_limit_bytes == 0U);

    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!concurrency_wal_is_checkpointed(database_path));
    retained_wal_size = concurrency_wal_size(database_path);
    assert(retained_wal_size > 0);

    db = open_database_with_page_log_limit(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        (unsigned long long)retained_wal_size
    );
    info.size = sizeof(info);
    assert(mylite_ownerless_pressure_status(db, &info) == MYLITE_OK);
    assert(info.active_page_version_pin_count == 1U);
    assert(info.page_version_wal_limit_reached == 1);
    assert(info.oldest_page_version_pin_lsn > 0U);
    assert(info.page_version_wal_bytes == (unsigned long long)retained_wal_size);
    assert(info.page_version_wal_limit_bytes == (unsigned long long)retained_wal_size);

    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert(concurrency_wal_is_checkpointed(database_path));

    info.size = sizeof(info);
    assert(mylite_ownerless_pressure_status(db, &info) == MYLITE_OK);
    assert(info.active_page_version_pin_count == 0U);
    assert(info.page_version_wal_limit_reached == 0);
    assert(info.oldest_page_version_pin_lsn == 0U);
    assert(info.page_version_wal_limit_bytes == (unsigned long long)retained_wal_size);

    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 2");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 32U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 32U);
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 32U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_expanding_page_pressure_reclaims_after_release(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-expanding-page-pressure.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    const unsigned rows =
        ownerless_unsigned_env("MYLITE_OWNERLESS_EXPANDING_PAGE_PRESSURE_ROWS", 12U, 128U);
    mylite_db *db;
    char sql[256];

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_expanding_pressure ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "payload VARBINARY(4000) NOT NULL"
        ") ENGINE=InnoDB"
    );
    for (unsigned row = 1U; row <= rows; ++row) {
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_expanding_pressure VALUES (%u, 1, REPEAT('a', 4000))",
                row
            ) > 0
        );
        exec_ok(db, sql);
    }
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_expanding_pressure") == rows);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_expanding_pressure") == rows);
    assert(
        query_unsigned(db, "SELECT SUM(LENGTH(payload)) FROM app.ownerless_expanding_pressure") ==
        rows * 4000ULL
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_expanding_page_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    for (unsigned row = 1U; row <= rows; ++row) {
        const unsigned expected_sum = rows + row;
        const char payload_byte = (char)('b' + (row % 24U));

        db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "UPDATE app.ownerless_expanding_pressure "
                "SET value = value + 1, payload = REPEAT('%c', 4000) WHERE id = %u",
                payload_byte,
                row
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_expanding_pressure") ==
            expected_sum
        );
        assert(mylite_close(db) == MYLITE_OK);
        assert(!concurrency_wal_is_checkpointed(database_path));
        assert(count_concurrency_wal_records_at_or_before(database_path, UINT64_MAX) > 0U);
    }

    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert(concurrency_wal_is_checkpointed(database_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_expanding_pressure") == rows);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_expanding_pressure") == rows * 2ULL
    );
    assert(
        query_unsigned(db, "SELECT SUM(LENGTH(payload)) FROM app.ownerless_expanding_pressure") ==
        rows * 4000ULL
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_expanding_pressure") == rows);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_expanding_pressure") == rows * 2ULL
    );
    assert(
        query_unsigned(db, "SELECT SUM(LENGTH(payload)) FROM app.ownerless_expanding_pressure") ==
        rows * 4000ULL
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_no_live_pressure_reclaim_advances_visible_lsn(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-no-live-pressure-reclaim.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    mylite_db *db;
    uint64_t latest_lsn;
    uint64_t visible_lsn;
    uint64_t lowered_visible_lsn;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(concurrency_wal_is_checkpointed(database_path));
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!concurrency_wal_is_checkpointed(database_path));

    latest_lsn = read_concurrency_checkpoint_latest_lsn(database_path);
    visible_lsn = read_concurrency_checkpoint_visible_lsn(database_path);
    assert(latest_lsn >= visible_lsn);
    assert(visible_lsn > 1U);
    lowered_visible_lsn = visible_lsn - 1U;
    write_concurrency_checkpoint_visible_lsn(database_path, lowered_visible_lsn);
    assert(read_concurrency_checkpoint_visible_lsn(database_path) == lowered_visible_lsn);

    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);
    assert(read_concurrency_checkpoint_visible_lsn(database_path) >= latest_lsn);
    assert(concurrency_wal_is_checkpointed(database_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_killed_ownerless_snapshot_pin_allows_live_page_log_reclaim(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-killed-snapshot-pin-reclaim.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    int reader_ready_pipe[2];
    int reader_release_pipe[2];
    pid_t peer_child;
    pid_t reader_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);
    assert(pipe(reader_ready_pipe) == 0);
    assert(pipe(reader_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(reader_ready_pipe[0]);
        close(reader_ready_pipe[1]);
        close(reader_release_pipe[0]);
        close(reader_release_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(reader_ready_pipe[0]);
        close(reader_release_pipe[1]);
        close(peer_release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = reader_ready_pipe[1],
                .release_read_fd = reader_release_pipe[0],
            }
        );
    }

    close(reader_ready_pipe[1]);
    close(reader_release_pipe[0]);
    wait_for_pipe(reader_ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 5 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!concurrency_wal_is_checkpointed(database_path));

    assert(kill(reader_child, SIGKILL) == 0);
    wait_for_signaled_child(reader_child, SIGKILL);
    assert(close(reader_release_pipe[1]) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 35U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void test_consistent_snapshot_start_pin_blocks_live_reclaim_before_execute(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-consistent-snapshot-pin-race.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t reader_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        start_consistent_snapshot_after_pin_fault(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 5 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 36U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!concurrency_wal_is_checkpointed(database_path));

    signal_pipe(release_pipe[1]);
    wait_for_child(reader_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 36U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_trx_registration_blocks_peer_cleanup_until_reopen_rebuilds(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-trx-register-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        update_first_row_until_trx_register_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_record_lock_before_grant_blocks_peer_cleanup_until_reopen_rebuilds(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-record-lock-before-grant-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int holder_ready_pipe[2];
    int holder_release_pipe[2];
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t holder_child;
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(holder_ready_pipe) == 0);
    assert(pipe(holder_release_pipe) == 0);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(holder_ready_pipe[0]);
        close(holder_ready_pipe[1]);
        close(holder_release_pipe[0]);
        close(holder_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    holder_child = fork();
    assert(holder_child >= 0);
    if (holder_child == 0) {
        close(holder_ready_pipe[0]);
        close(holder_release_pipe[1]);
        close(peer_ready_pipe[0]);
        close(peer_ready_pipe[1]);
        close(peer_release_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        lock_first_row_for_update_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = holder_ready_pipe[1],
                .release_read_fd = holder_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    close(holder_ready_pipe[1]);
    close(holder_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);
    wait_for_pipe(holder_ready_pipe[0]);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(holder_ready_pipe[0]);
        close(holder_release_pipe[1]);
        update_first_row_until_record_lock_before_grant_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(holder_release_pipe[1]);
    wait_for_child(holder_child);
    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_record_lock_grant_blocks_peer_cleanup_until_reopen_rebuilds(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-record-lock-grant-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int holder_ready_pipe[2];
    int holder_release_pipe[2];
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t holder_child;
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(holder_ready_pipe) == 0);
    assert(pipe(holder_release_pipe) == 0);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(holder_ready_pipe[0]);
        close(holder_ready_pipe[1]);
        close(holder_release_pipe[0]);
        close(holder_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    holder_child = fork();
    assert(holder_child >= 0);
    if (holder_child == 0) {
        close(holder_ready_pipe[0]);
        close(holder_release_pipe[1]);
        close(peer_ready_pipe[0]);
        close(peer_ready_pipe[1]);
        close(peer_release_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        update_first_row_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = holder_ready_pipe[1],
                .release_read_fd = holder_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    close(holder_ready_pipe[1]);
    close(holder_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);
    wait_for_pipe(holder_ready_pipe[0]);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(holder_ready_pipe[0]);
        close(holder_release_pipe[1]);
        update_first_row_until_record_lock_grant_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    assert(wait_for_concurrency_ownerless_write_waiting_count(database_path, 1U, 5000U) >= 1U);
    signal_pipe(holder_release_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    wait_for_child(holder_child);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}
#endif

static void test_rebuild_checkpoints_committed_page_versions(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-page-checkpoint.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    char insert_sql[160];
    uint64_t checkpoint_visible_lsn;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_checkpoint ("
        "id INT NOT NULL PRIMARY KEY, "
        "value VARBINARY(4000) NOT NULL"
        ") ENGINE=InnoDB"
    );
    for (unsigned id = 1U; id <= 32U; ++id) {
        assert(
            snprintf(
                insert_sql,
                sizeof(insert_sql),
                "INSERT INTO app.ownerless_checkpoint VALUES (%u, REPEAT('x', 4000))",
                id
            ) > 0
        );
        exec_ok(db, insert_sql);
    }
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_checkpoint") == 32U);
    assert(mylite_close(db) == MYLITE_OK);

    assert_concurrency_wal_has_page_versions_or_checkpoint(database_path);
    assert(concurrency_wal_is_checkpointed(database_path));
    checkpoint_visible_lsn = read_concurrency_checkpoint_visible_lsn(database_path);
    assert(checkpoint_visible_lsn > 0U);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(read_concurrency_redo_visible_lsn(database_path) >= checkpoint_visible_lsn);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_checkpoint") == 32U);
    assert(mylite_close(db) == MYLITE_OK);
    assert_concurrency_wal_has_page_versions_or_checkpoint(database_path);
    if (!concurrency_wal_is_checkpointed(database_path)) {
        assert_concurrency_page_index_has_entries(database_path);
    }

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_alter_waits_for_active_transaction(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-alter-mdl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t holder_child;
    pid_t alter_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    holder_child = fork();
    assert(holder_child >= 0);
    if (holder_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_select_for_update_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    alter_child = fork();
    assert(alter_child >= 0);
    if (alter_child == 0) {
        alter_ownerless_sql_expect_lock_timeout(paths);
    }
    wait_for_child(alter_child);

    signal_pipe(release_pipe[1]);
    wait_for_child(holder_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "ALTER TABLE app.ownerless_sql ADD COLUMN note VARCHAR(32)");
    exec_ok(db, "UPDATE app.ownerless_sql SET note = 'ok' WHERE id = 1");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql WHERE note = 'ok'") == 1U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void test_ownerless_table_wait_sql_negative_proof(void) {
    static const ownerless_table_wait_negative_case test_cases[] = {
        {
            .name = "alter-add-column",
            .sql = "ALTER TABLE app.ownerless_sql ADD COLUMN wait_negative_note VARCHAR(32)",
        },
        {
            .name = "create-index",
            .sql = "CREATE INDEX ownerless_table_wait_negative_idx "
                   "ON app.ownerless_sql(value)",
        },
        {
            .name = "truncate-table",
            .sql = "TRUNCATE TABLE app.ownerless_sql",
        },
        {
            .name = "rename-table",
            .sql = "RENAME TABLE app.ownerless_sql TO app.ownerless_sql_wait_negative",
        },
        {
            .name = "drop-table",
            .sql = "DROP TABLE app.ownerless_sql",
        },
    };
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-table-wait-negative.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int holder_ready_pipe[2];
    int holder_release_pipe[2];
    pid_t holder_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(holder_ready_pipe) == 0);
    assert(pipe(holder_release_pipe) == 0);

    holder_child = fork();
    assert(holder_child >= 0);
    if (holder_child == 0) {
        close(holder_ready_pipe[0]);
        close(holder_release_pipe[1]);
        hold_select_for_update_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = holder_ready_pipe[1],
                .release_read_fd = holder_release_pipe[0],
            }
        );
    }

    close(holder_ready_pipe[1]);
    close(holder_release_pipe[0]);
    wait_for_pipe(holder_ready_pipe[0]);

    for (size_t index = 0U; index < sizeof(test_cases) / sizeof(test_cases[0]); ++index) {
        int table_wait_fault_pipe[2];
        pid_t ddl_child;
        wait_child_or_pipe_result ddl_result;

        assert(pipe(table_wait_fault_pipe) == 0);
        ddl_child = fork();
        assert(ddl_child >= 0);
        if (ddl_child == 0) {
            close(holder_release_pipe[1]);
            close(table_wait_fault_pipe[0]);
            ownerless_sql_expect_lock_timeout_with_table_wait_fault(
                paths,
                &test_cases[index],
                table_wait_fault_pipe[1]
            );
        }

        close(table_wait_fault_pipe[1]);
        ddl_result =
            wait_for_child_result_or_pipe_message(ddl_child, table_wait_fault_pipe[0], 30000U);

        if (ddl_result.pipe_message || ddl_result.timed_out ||
            ddl_result.child_result != MYLITE_TEST_CHILD_OK) {
            fprintf(
                stderr,
                "ownerless table-wait negative proof failed: case=%s pipe_message=%d "
                "timed_out=%d child_result=%d\n",
                test_cases[index].name,
                ddl_result.pipe_message,
                ddl_result.timed_out,
                ddl_result.child_result
            );
            fflush(stderr);
        }
        assert(!ddl_result.pipe_message);
        assert(!ddl_result.timed_out);
        assert(ddl_result.child_result == MYLITE_TEST_CHILD_OK);
    }

    signal_pipe(holder_release_pipe[1]);
    wait_for_child(holder_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "ALTER TABLE app.ownerless_sql ADD COLUMN note VARCHAR(32)");
    exec_ok(db, "UPDATE app.ownerless_sql SET note = 'ok' WHERE id = 1");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql WHERE note = 'ok'") == 1U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql WHERE note = 'ok'") == 1U);
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql WHERE note = 'ok'") == 1U);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql WHERE note = 'ok'") == 1U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}
#endif

static void test_ownerless_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-ddl-refresh.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int ddl_ready_pipe[2];
    int ddl_release_pipe[2];
    pid_t ddl_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ddl_ready_pipe) == 0);
    assert(pipe(ddl_release_pipe) == 0);

    ddl_child = fork();
    assert(ddl_child >= 0);
    if (ddl_child == 0) {
        close(ddl_ready_pipe[0]);
        close(ddl_release_pipe[1]);
        run_ownerless_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = ddl_ready_pipe[1],
                .release_read_fd = ddl_release_pipe[0],
            }
        );
    }

    close(ddl_ready_pipe[1]);
    close(ddl_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT SUM(note) FROM app.ownerless_sql") == 14U);
    exec_ok(db, "UPDATE app.ownerless_sql SET note = note + 1 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(note) FROM app.ownerless_sql") == 15U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_created") == 100U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_renamed") == 100U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' AND table_name = 'ownerless_created'"
        ) == 0U
    );

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_renamed") == 0U);
    exec_ok(db, "INSERT INTO app.ownerless_renamed VALUES (2, 200)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_renamed") == 200U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' AND table_name = 'ownerless_renamed'"
        ) == 0U
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_renamed ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_renamed VALUES (3, 300)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_renamed") == 300U);
    assert(mylite_close(db) == MYLITE_OK);
    close(ddl_ready_pipe[0]);
    close(ddl_release_pipe[1]);
    wait_for_child(ddl_child);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_large_truncate_refreshes_peer_allocation(void) {
    enum {
        initial_rows = 48U,
        reuse_rows = 12U,
    };

    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-large-truncate-refresh.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int ready_pipe[2];
    int release_pipe[2];
    pid_t truncator_child;
    char sql[256];

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    truncator_child = fork();
    assert(truncator_child >= 0);
    if (truncator_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        truncate_ownerless_large_table_after_signal(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_truncate_bounds ("
        "id INT NOT NULL PRIMARY KEY, "
        "payload VARBINARY(4000) NOT NULL"
        ") ENGINE=InnoDB"
    );
    for (unsigned id = 1U; id <= initial_rows; ++id) {
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_truncate_bounds VALUES (%u, REPEAT('x', 4000))",
                id
            ) > 0
        );
        exec_ok(db, sql);
    }
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_truncate_bounds") == initial_rows
    );

    signal_pipe(release_pipe[1]);
    wait_for_child(truncator_child);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_truncate_bounds") == 0U);
    for (unsigned id = 1U; id <= reuse_rows; ++id) {
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_truncate_bounds VALUES (%u, REPEAT('y', 4000))",
                id
            ) > 0
        );
        exec_ok(db, sql);
    }
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_truncate_bounds") == reuse_rows);
    assert(
        query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_truncate_bounds") ==
        (reuse_rows * (reuse_rows + 1U)) / 2U
    );
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_local_ddl_survives_dictionary_flush(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-local-ddl-flush.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_local_ddl_flush ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_local_ddl_flush VALUES (1, 41)");
    exec_ok(db, "FLUSH TABLES");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_local_ddl_flush") == 41U);
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_local_ddl_flush") == 41U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_concurrent_ownerless_ddl_allocates_unique_metadata(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-ddl-allocation.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int ready_pipes[MYLITE_TEST_DDL_WORKER_COUNT][2];
    int release_pipes[MYLITE_TEST_DDL_WORKER_COUNT][2];
    pid_t workers[MYLITE_TEST_DDL_WORKER_COUNT];

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_DDL_WORKER_COUNT; ++worker_id) {
        assert(pipe(ready_pipes[worker_id]) == 0);
        assert(pipe(release_pipes[worker_id]) == 0);
    }

    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_DDL_WORKER_COUNT; ++worker_id) {
        workers[worker_id] = fork();
        assert(workers[worker_id] >= 0);
        if (workers[worker_id] == 0) {
            close(ready_pipes[worker_id][0]);
            close(release_pipes[worker_id][1]);
            create_ownerless_ddl_tables_after_signal(
                paths,
                worker_id,
                (child_pipes){
                    .ready_write_fd = ready_pipes[worker_id][1],
                    .release_read_fd = release_pipes[worker_id][0],
                }
            );
        }
    }

    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_DDL_WORKER_COUNT; ++worker_id) {
        close(ready_pipes[worker_id][1]);
        close(release_pipes[worker_id][0]);
    }

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_DDL_WORKER_COUNT; ++worker_id) {
        wait_for_pipe_message(ready_pipes[worker_id][0]);
    }
    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_DDL_WORKER_COUNT; ++worker_id) {
        signal_pipe_message(release_pipes[worker_id][1]);
    }
    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_DDL_WORKER_COUNT; ++worker_id) {
        wait_for_child(workers[worker_id]);
        assert(close(ready_pipes[worker_id][0]) == 0);
        assert(close(release_pipes[worker_id][1]) == 0);
    }

    assert_ownerless_ddl_tables(db);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_broader_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-broader-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int ddl_ready_pipe[2];
    int ddl_release_pipe[2];
    pid_t ddl_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ddl_ready_pipe) == 0);
    assert(pipe(ddl_release_pipe) == 0);

    ddl_child = fork();
    assert(ddl_child >= 0);
    if (ddl_child == 0) {
        close(ddl_ready_pipe[0]);
        close(ddl_release_pipe[1]);
        run_ownerless_broader_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = ddl_ready_pipe[1],
                .release_read_fd = ddl_release_pipe[0],
            }
        );
    }

    close(ddl_ready_pipe[1]);
    close(ddl_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_child") == 1U);
    exec_ok(db, "DELETE FROM app.ownerless_fk_parent WHERE id = 1");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_child") == 0U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated "
            "WHERE full_name = 'Ada Lovelace' AND name_length = 12"
        ) == 1U
    );
    exec_ok(db, "UPDATE app.ownerless_generated SET last_name = 'Byron' WHERE id = 1");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated "
            "WHERE full_name = 'Ada Byron' AND name_length = 9"
        ) == 1U
    );

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_online "
            "WHERE value = 42 AND status = 'ready'"
        ) == 1U
    );
    exec_ok(db, "UPDATE app.ownerless_online SET status = 'done' WHERE id = 1");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_online' "
            "AND index_name = 'ownerless_online_status_idx'"
        ) == 1U
    );

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_online "
            "WHERE value = 42 AND state = 'done' AND priority = 7"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_online' "
            "AND column_name = 'status'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_online' "
            "AND column_name = 'state'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_online' "
            "AND column_name = 'scratch'"
        ) == 0U
    );
    exec_ok(
        db,
        "UPDATE app.ownerless_online "
        "SET state = 'archived', priority = priority + 1 WHERE id = 1"
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_online "
            "WHERE state = 'archived' AND priority = 8"
        ) == 1U
    );

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_like "
            "WHERE id = 1 AND value = 42 AND state = 'archived' AND priority = 8"
        ) == 1U
    );

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_ctas "
            "WHERE id = 1 AND value = 42 AND state = 'archived' AND priority = 8"
        ) == 1U
    );

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_instant "
            "WHERE id = 1 AND old_value = 10 AND payload = 'base' AND instant_value = 7"
        ) == 1U
    );
    exec_ok(db, "UPDATE app.ownerless_instant SET instant_value = 11 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(instant_value) FROM app.ownerless_instant") == 11U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant' "
            "AND column_name = 'old_value'"
        ) == 0U
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_instant (id, payload, instant_value) "
        "VALUES (2, 'peer', 13)"
    );
    assert(query_unsigned(db, "SELECT SUM(instant_value) FROM app.ownerless_instant") == 24U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant' "
            "AND column_name = 'instant_value'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant' "
            "AND column_name = 'payload'"
        ) == 3U
    );
    exec_ok(db, "UPDATE app.ownerless_instant SET payload = 'done' WHERE id = 2");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_instant "
            "WHERE id = 2 AND instant_value = 13 AND payload = 'done'"
        ) == 1U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(ddl_ready_pipe[0]);
    close(ddl_release_pipe[1]);
    wait_for_child(ddl_child);

    assert_ownerless_broader_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_broader_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_broader_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_broader_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_online_ddl_options_refresh_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-online-ddl-options.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int ddl_ready_pipe[2];
    int ddl_release_pipe[2];
    pid_t ddl_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ddl_ready_pipe) == 0);
    assert(pipe(ddl_release_pipe) == 0);

    ddl_child = fork();
    assert(ddl_child >= 0);
    if (ddl_child == 0) {
        close(ddl_ready_pipe[0]);
        close(ddl_release_pipe[1]);
        run_ownerless_online_ddl_options_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = ddl_ready_pipe[1],
                .release_read_fd = ddl_release_pipe[0],
            }
        );
    }

    close(ddl_ready_pipe[1]);
    close(ddl_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_ddl_options") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_ddl_options") == 30U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ddl_options' "
            "AND index_name = 'ownerless_ddl_options_status_idx'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_ddl_options "
            "FORCE INDEX (ownerless_ddl_options_status_idx) "
            "WHERE status = 'ready'"
        ) == 2U
    );
    exec_ok(db, "UPDATE app.ownerless_ddl_options SET status = 'done' WHERE id = 1");

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ddl_options' "
            "AND index_name = 'ownerless_ddl_options_value_idx'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_ddl_options "
            "FORCE INDEX (ownerless_ddl_options_value_idx) "
            "WHERE value >= 10"
        ) == 2U
    );
    exec_ok(db, "UPDATE app.ownerless_ddl_options SET value = value + 5 WHERE id = 2");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_ddl_options") == 35U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT character_maximum_length FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ddl_options' "
            "AND column_name = 'payload'"
        ) == 80U
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_ddl_options VALUES "
        "(3, 30, 'copy', 'payload-longer-than-the-old-width')"
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_ddl_options") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_ddl_options") == 65U);

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_ddl_options") == 3U);
    exec_ok(db, "UPDATE app.ownerless_ddl_options SET payload = 'rebuilt' WHERE id = 3");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_ddl_options "
            "WHERE id = 3 AND payload = 'rebuilt'"
        ) == 1U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(ddl_ready_pipe[0]);
    close(ddl_release_pipe[1]);
    wait_for_child(ddl_child);

    assert_ownerless_online_ddl_options_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_online_ddl_options_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_online_ddl_options_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_online_ddl_options_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_generated_column_alter_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-generated-column-alter.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int generated_ready_pipe[2];
    int generated_release_pipe[2];
    pid_t generated_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(generated_ready_pipe) == 0);
    assert(pipe(generated_release_pipe) == 0);

    generated_child = fork();
    assert(generated_child >= 0);
    if (generated_child == 0) {
        close(generated_ready_pipe[0]);
        close(generated_release_pipe[1]);
        run_ownerless_generated_column_alter_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = generated_ready_pipe[1],
                .release_read_fd = generated_release_pipe[0],
            }
        );
    }

    close(generated_ready_pipe[1]);
    close(generated_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(generated_release_pipe[1]);
    wait_for_pipe_message(generated_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated_alter "
            "WHERE id = 1 AND first_name = 'Ada' AND last_name = 'Lovelace'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_generated_alter' "
            "AND column_name IN ('full_name', 'name_length')"
        ) == 0U
    );

    signal_pipe_message(generated_release_pipe[1]);
    wait_for_pipe_message(generated_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_generated_alter' "
            "AND column_name IN ('full_name', 'name_length')"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated_alter "
            "WHERE full_name = 'Ada Lovelace' AND name_length = 12"
        ) == 1U
    );
    exec_ok(db, "UPDATE app.ownerless_generated_alter SET last_name = 'Byron' WHERE id = 1");
    exec_ok(
        db,
        "INSERT INTO app.ownerless_generated_alter (id, first_name, last_name) "
        "VALUES (2, 'Grace', 'Hopper')"
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated_alter "
            "WHERE full_name = 'Ada Byron' AND name_length = 9"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated_alter "
            "WHERE full_name = 'Grace Hopper' AND name_length = 12"
        ) == 1U
    );

    signal_pipe_message(generated_release_pipe[1]);
    wait_for_pipe_message(generated_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_generated_alter' "
            "AND column_name IN ('full_name', 'name_length')"
        ) == 0U
    );
    exec_ok(db, "UPDATE app.ownerless_generated_alter SET first_name = 'Rear' WHERE id = 2");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated_alter "
            "WHERE id = 2 AND first_name = 'Rear' AND last_name = 'Hopper'"
        ) == 1U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(generated_ready_pipe[0]);
    close(generated_release_pipe[1]);
    wait_for_child(generated_child);

    assert_ownerless_generated_column_alter_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_generated_column_alter_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_generated_column_alter_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_generated_column_alter_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_charset_convert_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-charset-convert-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int charset_ready_pipe[2];
    int charset_release_pipe[2];
    pid_t charset_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(charset_ready_pipe) == 0);
    assert(pipe(charset_release_pipe) == 0);

    charset_child = fork();
    assert(charset_child >= 0);
    if (charset_child == 0) {
        close(charset_ready_pipe[0]);
        close(charset_release_pipe[1]);
        run_ownerless_charset_convert_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = charset_ready_pipe[1],
                .release_read_fd = charset_release_pipe[0],
            }
        );
    }

    close(charset_ready_pipe[1]);
    close(charset_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(charset_release_pipe[1]);
    wait_for_pipe_message(charset_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_charset_convert_base' "
            "AND column_name = 'name' "
            "AND character_set_name = 'latin1' "
            "AND collation_name = 'latin1_swedish_ci'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_charset_convert_base") == 30U);
    assert(
        query_unsigned(
            db,
            "SELECT SUM(CHAR_LENGTH(name)) FROM app.ownerless_charset_convert_base"
        ) == 9U
    );

    signal_pipe_message(charset_release_pipe[1]);
    wait_for_pipe_message(charset_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_charset_convert_base' "
            "AND column_name = 'name' "
            "AND character_set_name = 'utf8mb4' "
            "AND collation_name = 'utf8mb4_general_ci'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_charset_convert_base") == 30U);
    exec_ok(db, "INSERT INTO app.ownerless_charset_convert_base VALUES (3, 'gamma', 30)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_charset_convert_base") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_charset_convert_base") == 60U);
    assert(
        query_unsigned(
            db,
            "SELECT SUM(CHAR_LENGTH(name)) FROM app.ownerless_charset_convert_base"
        ) == 14U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(charset_ready_pipe[0]);
    close(charset_release_pipe[1]);
    wait_for_child(charset_child);

    assert_ownerless_charset_convert_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_charset_convert_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_charset_convert_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_charset_convert_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_row_format_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-row-format-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int row_format_ready_pipe[2];
    int row_format_release_pipe[2];
    pid_t row_format_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(row_format_ready_pipe) == 0);
    assert(pipe(row_format_release_pipe) == 0);

    row_format_child = fork();
    assert(row_format_child >= 0);
    if (row_format_child == 0) {
        close(row_format_ready_pipe[0]);
        close(row_format_release_pipe[1]);
        run_ownerless_row_format_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = row_format_ready_pipe[1],
                .release_read_fd = row_format_release_pipe[0],
            }
        );
    }

    close(row_format_ready_pipe[1]);
    close(row_format_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(row_format_release_pipe[1]);
    wait_for_pipe_message(row_format_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_row_format_base' "
            "AND ROW_FORMAT = 'Compact'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_row_format_base") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_row_format_base") == 30U);
    assert(
        query_unsigned(db, "SELECT SUM(CHAR_LENGTH(payload)) FROM app.ownerless_row_format_base") ==
        512U
    );

    signal_pipe_message(row_format_release_pipe[1]);
    wait_for_pipe_message(row_format_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_row_format_base' "
            "AND ROW_FORMAT = 'Dynamic'"
        ) == 1U
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_row_format_base VALUES "
        "(3, 30, 'after', REPEAT('c', 256))"
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_row_format_base") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_row_format_base") == 60U);
    assert(
        query_unsigned(db, "SELECT SUM(CHAR_LENGTH(payload)) FROM app.ownerless_row_format_base") ==
        768U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(row_format_ready_pipe[0]);
    close(row_format_release_pipe[1]);
    wait_for_child(row_format_child);

    assert_ownerless_row_format_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_row_format_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_row_format_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_row_format_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_table_comment_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-table-comment-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int comment_ready_pipe[2];
    int comment_release_pipe[2];
    pid_t comment_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(comment_ready_pipe) == 0);
    assert(pipe(comment_release_pipe) == 0);

    comment_child = fork();
    assert(comment_child >= 0);
    if (comment_child == 0) {
        close(comment_ready_pipe[0]);
        close(comment_release_pipe[1]);
        run_ownerless_table_comment_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = comment_ready_pipe[1],
                .release_read_fd = comment_release_pipe[0],
            }
        );
    }

    close(comment_ready_pipe[1]);
    close(comment_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(comment_release_pipe[1]);
    wait_for_pipe_message(comment_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_table_comment_base' "
            "AND table_comment LIKE 'ownerless initial comment%'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_table_comment_base") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_table_comment_base") == 30U);

    signal_pipe_message(comment_release_pipe[1]);
    wait_for_pipe_message(comment_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_table_comment_base' "
            "AND table_comment LIKE 'ownerless updated comment%'"
        ) == 1U
    );
    exec_ok(db, "INSERT INTO app.ownerless_table_comment_base VALUES (3, 30)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_table_comment_base") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_table_comment_base") == 60U);

    assert(mylite_close(db) == MYLITE_OK);
    close(comment_ready_pipe[0]);
    close(comment_release_pipe[1]);
    wait_for_child(comment_child);

    assert_ownerless_table_comment_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_table_comment_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_table_comment_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_table_comment_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_force_rebuild_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-force-rebuild-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int rebuild_ready_pipe[2];
    int rebuild_release_pipe[2];
    pid_t rebuild_child;
    unsigned long long before_table_id;
    unsigned long long before_space;
    unsigned long long after_table_id;
    unsigned long long after_space;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(rebuild_ready_pipe) == 0);
    assert(pipe(rebuild_release_pipe) == 0);

    rebuild_child = fork();
    assert(rebuild_child >= 0);
    if (rebuild_child == 0) {
        close(rebuild_ready_pipe[0]);
        close(rebuild_release_pipe[1]);
        run_ownerless_force_rebuild_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = rebuild_ready_pipe[1],
                .release_read_fd = rebuild_release_pipe[0],
            }
        );
    }

    close(rebuild_ready_pipe[1]);
    close(rebuild_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(rebuild_release_pipe[1]);
    wait_for_pipe_message(rebuild_ready_pipe[0]);
    before_table_id = query_unsigned(
        db,
        "SELECT TABLE_ID FROM information_schema.INNODB_SYS_TABLES "
        "WHERE NAME = 'app/ownerless_force_rebuild_base'"
    );
    before_space = query_unsigned(
        db,
        "SELECT SPACE FROM information_schema.INNODB_SYS_TABLES "
        "WHERE NAME = 'app/ownerless_force_rebuild_base'"
    );
    assert(before_table_id > 0U);
    assert(before_space > 0U);
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_force_rebuild_base "
            "FORCE INDEX (ownerless_force_rebuild_value_idx) "
            "WHERE value >= 20"
        ) == 5U
    );

    signal_pipe_message(rebuild_release_pipe[1]);
    wait_for_pipe_message(rebuild_ready_pipe[0]);
    after_table_id = query_unsigned(
        db,
        "SELECT TABLE_ID FROM information_schema.INNODB_SYS_TABLES "
        "WHERE NAME = 'app/ownerless_force_rebuild_base'"
    );
    after_space = query_unsigned(
        db,
        "SELECT SPACE FROM information_schema.INNODB_SYS_TABLES "
        "WHERE NAME = 'app/ownerless_force_rebuild_base'"
    );
    assert(after_table_id > 0U);
    assert(after_space > 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_force_rebuild_base") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_force_rebuild_base") == 60U);
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_force_rebuild_base "
            "FORCE INDEX (ownerless_force_rebuild_value_idx) "
            "WHERE value >= 20"
        ) == 5U
    );
    exec_ok(db, "INSERT INTO app.ownerless_force_rebuild_base VALUES (4, 40, 400)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_force_rebuild_base") == 4U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_force_rebuild_base") == 100U);

    assert(mylite_close(db) == MYLITE_OK);
    close(rebuild_ready_pipe[0]);
    close(rebuild_release_pipe[1]);
    wait_for_child(rebuild_child);

    assert_ownerless_force_rebuild_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_force_rebuild_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_force_rebuild_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_force_rebuild_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_column_default_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-column-default-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int default_ready_pipe[2];
    int default_release_pipe[2];
    pid_t default_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(default_ready_pipe) == 0);
    assert(pipe(default_release_pipe) == 0);

    default_child = fork();
    assert(default_child >= 0);
    if (default_child == 0) {
        close(default_ready_pipe[0]);
        close(default_release_pipe[1]);
        run_ownerless_column_default_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = default_ready_pipe[1],
                .release_read_fd = default_release_pipe[0],
            }
        );
    }

    close(default_ready_pipe[1]);
    close(default_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(default_release_pipe[1]);
    wait_for_pipe_message(default_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_column_default_alter' "
            "AND column_name = 'value' "
            "AND column_default = '10'"
        ) == 1U
    );
    exec_ok(db, "INSERT INTO app.ownerless_column_default_alter (id) VALUES (2)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_column_default_alter") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_column_default_alter") == 20U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_column_default_alter WHERE note = 'ready'"
        ) == 2U
    );

    signal_pipe_message(default_release_pipe[1]);
    wait_for_pipe_message(default_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_column_default_alter' "
            "AND column_name = 'value' "
            "AND column_default = '25'"
        ) == 1U
    );
    exec_ok(db, "INSERT INTO app.ownerless_column_default_alter (id) VALUES (3)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_column_default_alter") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_column_default_alter") == 45U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_column_default_alter WHERE note = 'done'"
        ) == 1U
    );

    signal_pipe_message(default_release_pipe[1]);
    wait_for_pipe_message(default_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_column_default_alter' "
            "AND column_name = 'value' "
            "AND column_default IS NULL"
        ) == 1U
    );
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_column_default_alter (id, note) "
            "VALUES (4, 'manual')",
            NULL
        ) != MYLITE_OK
    );
    exec_ok(db, "INSERT INTO app.ownerless_column_default_alter (id, value) VALUES (4, 40)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_column_default_alter") == 4U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_column_default_alter") == 85U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_column_default_alter WHERE note = 'done'"
        ) == 2U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(default_ready_pipe[0]);
    close(default_release_pipe[1]);
    wait_for_child(default_child);

    assert_ownerless_column_default_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_column_default_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_column_default_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_column_default_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_instant_column_variants_refresh_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-instant-column-variants.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int instant_ready_pipe[2];
    int instant_release_pipe[2];
    pid_t instant_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(instant_ready_pipe) == 0);
    assert(pipe(instant_release_pipe) == 0);

    instant_child = fork();
    assert(instant_child >= 0);
    if (instant_child == 0) {
        close(instant_ready_pipe[0]);
        close(instant_release_pipe[1]);
        run_ownerless_instant_column_variant_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = instant_ready_pipe[1],
                .release_read_fd = instant_release_pipe[0],
            }
        );
    }

    close(instant_ready_pipe[1]);
    close(instant_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(instant_release_pipe[1]);
    wait_for_pipe_message(instant_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_instant_variants") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'first_note'"
        ) == 0U
    );

    signal_pipe_message(instant_release_pipe[1]);
    wait_for_pipe_message(instant_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'first_note'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'id'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_instant_variants "
            "WHERE id = 1 AND first_note = 'first'"
        ) == 1U
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_instant_variants "
        "(first_note, id, base_value, marker) VALUES ('peer', 2, 20, 'peer')"
    );

    signal_pipe_message(instant_release_pipe[1]);
    wait_for_pipe_message(instant_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'side_value'"
        ) == 4U
    );
    assert(query_unsigned(db, "SELECT SUM(side_value) FROM app.ownerless_instant_variants") == 10U);
    exec_ok(db, "UPDATE app.ownerless_instant_variants SET side_value = 9 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(side_value) FROM app.ownerless_instant_variants") == 14U);

    signal_pipe_message(instant_release_pipe[1]);
    wait_for_pipe_message(instant_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'marker'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'renamed_marker'"
        ) == 5U
    );
    assert(exec_status(db, "SELECT marker FROM app.ownerless_instant_variants", NULL) != MYLITE_OK);
    exec_ok(
        db,
        "UPDATE app.ownerless_instant_variants "
        "SET renamed_marker = 'renamed' WHERE id = 1"
    );

    signal_pipe_message(instant_release_pipe[1]);
    wait_for_pipe_message(instant_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'value_double'"
        ) == 1U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value_double) FROM app.ownerless_instant_variants") == 60U
    );
    exec_ok(db, "UPDATE app.ownerless_instant_variants SET base_value = 25 WHERE id = 2");
    assert(
        query_unsigned(db, "SELECT SUM(value_double) FROM app.ownerless_instant_variants") == 70U
    );

    signal_pipe_message(instant_release_pipe[1]);
    wait_for_pipe_message(instant_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'value_double'"
        ) == 0U
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_instant_variants "
        "(first_note, id, base_value, side_value, renamed_marker) "
        "VALUES ('final', 3, 30, 7, 'final')"
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_instant_variants") == 3U);
    assert(query_unsigned(db, "SELECT SUM(base_value) FROM app.ownerless_instant_variants") == 65U);
    assert(query_unsigned(db, "SELECT SUM(side_value) FROM app.ownerless_instant_variants") == 21U);

    assert(mylite_close(db) == MYLITE_OK);
    close(instant_ready_pipe[0]);
    close(instant_release_pipe[1]);
    wait_for_child(instant_child);

    assert_ownerless_instant_column_variant_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_instant_column_variant_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_instant_column_variant_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_instant_column_variant_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_schema_lifecycle_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-schema-lifecycle.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int schema_ready_pipe[2];
    int schema_release_pipe[2];
    pid_t schema_child;
    char *datadir_path;
    char *schema_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(schema_ready_pipe) == 0);
    assert(pipe(schema_release_pipe) == 0);

    schema_child = fork();
    assert(schema_child >= 0);
    if (schema_child == 0) {
        close(schema_ready_pipe[0]);
        close(schema_release_pipe[1]);
        run_ownerless_schema_lifecycle_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = schema_ready_pipe[1],
                .release_read_fd = schema_release_pipe[0],
            }
        );
    }

    close(schema_ready_pipe[1]);
    close(schema_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(schema_release_pipe[1]);
    wait_for_pipe_message(schema_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_schema' "
            "AND table_name = 'ownerless_schema_table'"
        ) == 1U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM ownerless_schema.ownerless_schema_table") == 10U
    );
    exec_ok(db, "INSERT INTO ownerless_schema.ownerless_schema_table VALUES (2, 20)");
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM ownerless_schema.ownerless_schema_table") == 30U
    );

    signal_pipe_message(schema_release_pipe[1]);
    wait_for_pipe_message(schema_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_schema'"
        ) == 0U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM ownerless_schema.ownerless_schema_table", NULL) !=
        MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(schema_ready_pipe[0]);
    close(schema_release_pipe[1]);
    wait_for_child(schema_child);

    datadir_path = path_join(database_path, "datadir");
    schema_path = path_join(datadir_path, "ownerless_schema");
    assert(!path_exists(schema_path));
    assert_ownerless_schema_lifecycle_absent(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_schema_lifecycle_absent(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_schema_lifecycle_absent(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_schema_lifecycle_absent(paths, MYLITE_OPEN_READWRITE, database_path);

    free(schema_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_schema_default_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-schema-default-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    char *datadir_path = NULL;
    char *schema_path = NULL;
    char *db_opt_path = NULL;
    mylite_db *db;
    int schema_ready_pipe[2];
    int schema_release_pipe[2];
    pid_t schema_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(schema_ready_pipe) == 0);
    assert(pipe(schema_release_pipe) == 0);

    datadir_path = path_join(database_path, "datadir");
    schema_path = path_join(datadir_path, "ownerless_schema_defaults");
    db_opt_path = path_join(schema_path, "db.opt");

    schema_child = fork();
    assert(schema_child >= 0);
    if (schema_child == 0) {
        close(schema_ready_pipe[0]);
        close(schema_release_pipe[1]);
        run_ownerless_schema_default_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = schema_ready_pipe[1],
                .release_read_fd = schema_release_pipe[0],
            }
        );
    }

    close(schema_ready_pipe[1]);
    close(schema_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(schema_release_pipe[1]);
    wait_for_pipe_message(schema_ready_pipe[0]);
    assert(path_exists(schema_path));
    assert(path_exists(db_opt_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema_defaults' "
            "AND default_character_set_name = 'latin1' "
            "AND default_collation_name = 'latin1_swedish_ci'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'ownerless_schema_defaults' "
            "AND table_name = 'ownerless_schema_default_before' "
            "AND column_name = 'name' "
            "AND character_set_name = 'latin1' "
            "AND collation_name = 'latin1_swedish_ci'"
        ) == 1U
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_schema_defaults.ownerless_schema_default_before "
        "VALUES (2, 'peer-before')"
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_schema_defaults.ownerless_schema_default_before"
        ) == 2U
    );

    signal_pipe_message(schema_release_pipe[1]);
    wait_for_pipe_message(schema_ready_pipe[0]);
    assert(path_exists(db_opt_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema_defaults' "
            "AND default_character_set_name = 'utf8mb4' "
            "AND default_collation_name = 'utf8mb4_unicode_ci'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'ownerless_schema_defaults' "
            "AND table_name = 'ownerless_schema_default_before' "
            "AND column_name = 'name' "
            "AND character_set_name = 'latin1' "
            "AND collation_name = 'latin1_swedish_ci'"
        ) == 1U
    );
    exec_ok(
        db,
        "UPDATE ownerless_schema_defaults.ownerless_schema_default_before "
        "SET name = 'after-alter' WHERE id = 1"
    );

    signal_pipe_message(schema_release_pipe[1]);
    wait_for_pipe_message(schema_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'ownerless_schema_defaults' "
            "AND table_name = 'ownerless_schema_default_after' "
            "AND column_name = 'name' "
            "AND character_set_name = 'utf8mb4' "
            "AND collation_name = 'utf8mb4_unicode_ci'"
        ) == 1U
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_schema_defaults.ownerless_schema_default_after "
        "VALUES (2, 'peer-after')"
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_schema_defaults.ownerless_schema_default_after"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_schema_defaults.ownerless_schema_default_before "
            "WHERE name IN ('after-alter', 'peer-before')"
        ) == 2U
    );

    signal_pipe_message(schema_release_pipe[1]);
    wait_for_pipe_message(schema_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema_defaults'"
        ) == 0U
    );
    assert(
        exec_status(
            db,
            "SELECT COUNT(*) FROM ownerless_schema_defaults.ownerless_schema_default_before",
            NULL
        ) != MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(schema_ready_pipe[0]);
    close(schema_release_pipe[1]);
    wait_for_child(schema_child);

    assert(!path_exists(schema_path));
    assert_ownerless_schema_default_ddl_absent(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_schema_default_ddl_absent(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_schema_default_ddl_absent(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_schema_default_ddl_absent(paths, MYLITE_OPEN_READWRITE, database_path);

    free(db_opt_path);
    free(schema_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_schema_idempotent_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-schema-idempotent-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    char *datadir_path = NULL;
    char *schema_path = NULL;
    char *db_opt_path = NULL;
    mylite_db *db;
    int schema_ready_pipe[2];
    int schema_release_pipe[2];
    pid_t schema_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(schema_ready_pipe) == 0);
    assert(pipe(schema_release_pipe) == 0);

    datadir_path = path_join(database_path, "datadir");
    schema_path = path_join(datadir_path, "ownerless_schema_idempotent");
    db_opt_path = path_join(schema_path, "db.opt");

    schema_child = fork();
    assert(schema_child >= 0);
    if (schema_child == 0) {
        close(schema_ready_pipe[0]);
        close(schema_release_pipe[1]);
        run_ownerless_schema_idempotent_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = schema_ready_pipe[1],
                .release_read_fd = schema_release_pipe[0],
            }
        );
    }

    close(schema_ready_pipe[1]);
    close(schema_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(schema_release_pipe[1]);
    wait_for_pipe_message(schema_ready_pipe[0]);
    assert(path_exists(schema_path));
    assert(path_exists(db_opt_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema_idempotent' "
            "AND default_character_set_name = 'latin1' "
            "AND default_collation_name = 'latin1_swedish_ci'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_schema_idempotent' "
            "AND table_name = 'ownerless_schema_idempotent_table'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'ownerless_schema_idempotent' "
            "AND table_name = 'ownerless_schema_idempotent_table' "
            "AND column_name = 'note' "
            "AND character_set_name = 'latin1' "
            "AND collation_name = 'latin1_swedish_ci'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM "
            "ownerless_schema_idempotent.ownerless_schema_idempotent_table"
        ) == 10U
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_schema_idempotent.ownerless_schema_idempotent_table "
        "VALUES (2, 20, 'peer-before')"
    );

    signal_pipe_message(schema_release_pipe[1]);
    wait_for_pipe_message(schema_ready_pipe[0]);
    assert(path_exists(schema_path));
    assert(path_exists(db_opt_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema_idempotent' "
            "AND default_character_set_name = 'latin1' "
            "AND default_collation_name = 'latin1_swedish_ci'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema_idempotent_missing'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM "
            "ownerless_schema_idempotent.ownerless_schema_idempotent_table"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM "
            "ownerless_schema_idempotent.ownerless_schema_idempotent_table"
        ) == 30U
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_schema_idempotent.ownerless_schema_idempotent_table "
        "VALUES (3, 30, 'peer-after')"
    );

    signal_pipe_message(schema_release_pipe[1]);
    wait_for_pipe_message(schema_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema_idempotent'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_schema_idempotent'"
        ) == 0U
    );
    assert(
        exec_status(
            db,
            "SELECT COUNT(*) FROM "
            "ownerless_schema_idempotent.ownerless_schema_idempotent_table",
            NULL
        ) != MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(schema_ready_pipe[0]);
    close(schema_release_pipe[1]);
    wait_for_child(schema_child);

    assert(!path_exists(schema_path));
    assert_ownerless_schema_idempotent_ddl_absent(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_schema_idempotent_ddl_absent(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_schema_idempotent_ddl_absent(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_schema_idempotent_ddl_absent(paths, MYLITE_OPEN_READWRITE, database_path);

    free(db_opt_path);
    free(schema_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_cross_schema_rename_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-cross-schema-rename.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int rename_ready_pipe[2];
    int rename_release_pipe[2];
    pid_t rename_child;
    char *datadir_path;
    char *app_path;
    char *target_schema_path;
    char *source_frm_path;
    char *source_ibd_path;
    char *target_frm_path;
    char *target_ibd_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(rename_ready_pipe) == 0);
    assert(pipe(rename_release_pipe) == 0);

    rename_child = fork();
    assert(rename_child >= 0);
    if (rename_child == 0) {
        close(rename_ready_pipe[0]);
        close(rename_release_pipe[1]);
        run_ownerless_cross_schema_rename_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = rename_ready_pipe[1],
                .release_read_fd = rename_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    target_schema_path = path_join(datadir_path, "ownerless_rename_schema");
    source_frm_path = path_join(app_path, "ownerless_cross_schema_source.frm");
    source_ibd_path = path_join(app_path, "ownerless_cross_schema_source.ibd");
    target_frm_path = path_join(target_schema_path, "ownerless_cross_schema_moved.frm");
    target_ibd_path = path_join(target_schema_path, "ownerless_cross_schema_moved.ibd");

    close(rename_ready_pipe[1]);
    close(rename_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(rename_release_pipe[1]);
    wait_for_pipe_message(rename_ready_pipe[0]);
    assert(path_exists(target_schema_path));
    assert(path_exists(source_frm_path));
    assert(path_exists(source_ibd_path));
    assert(!path_exists(target_frm_path));
    assert(!path_exists(target_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_cross_schema_source'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_rename_schema' "
            "AND table_name = 'ownerless_cross_schema_moved'"
        ) == 0U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_cross_schema_source") == 30U);
    exec_ok(db, "UPDATE app.ownerless_cross_schema_source SET value = value + 1 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_cross_schema_source") == 31U);

    signal_pipe_message(rename_release_pipe[1]);
    wait_for_pipe_message(rename_ready_pipe[0]);
    assert(!path_exists(source_frm_path));
    assert(!path_exists(source_ibd_path));
    assert(path_exists(target_frm_path));
    assert(path_exists(target_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_cross_schema_source'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_rename_schema' "
            "AND table_name = 'ownerless_cross_schema_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_cross_schema_source'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'ownerless_rename_schema/ownerless_cross_schema_moved'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_cross_schema_source", NULL) != MYLITE_OK
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_rename_schema.ownerless_cross_schema_moved"
        ) == 31U
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_rename_schema.ownerless_cross_schema_moved "
        "VALUES (3, 30, 'peer')"
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_rename_schema.ownerless_cross_schema_moved"
        ) == 3U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_rename_schema.ownerless_cross_schema_moved"
        ) == 61U
    );

    signal_pipe_message(rename_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(rename_ready_pipe[0]);
    close(rename_release_pipe[1]);
    wait_for_child(rename_child);

    assert_ownerless_cross_schema_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_cross_schema_rename_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_cross_schema_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_cross_schema_rename_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(target_ibd_path);
    free(target_frm_path);
    free(source_ibd_path);
    free(source_frm_path);
    free(target_schema_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_multi_rename_cycle_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-multi-rename-cycle.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int rename_ready_pipe[2];
    int rename_release_pipe[2];
    pid_t rename_child;
    char *datadir_path;
    char *app_path;
    char *left_frm_path;
    char *left_ibd_path;
    char *right_frm_path;
    char *right_ibd_path;
    char *tmp_frm_path;
    char *tmp_ibd_path;
    unsigned long long left_space_before;
    unsigned long long right_space_before;
    unsigned long long left_space_after;
    unsigned long long right_space_after;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(rename_ready_pipe) == 0);
    assert(pipe(rename_release_pipe) == 0);

    rename_child = fork();
    assert(rename_child >= 0);
    if (rename_child == 0) {
        close(rename_ready_pipe[0]);
        close(rename_release_pipe[1]);
        run_ownerless_multi_rename_cycle_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = rename_ready_pipe[1],
                .release_read_fd = rename_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    left_frm_path = path_join(app_path, "ownerless_rename_cycle_left.frm");
    left_ibd_path = path_join(app_path, "ownerless_rename_cycle_left.ibd");
    right_frm_path = path_join(app_path, "ownerless_rename_cycle_right.frm");
    right_ibd_path = path_join(app_path, "ownerless_rename_cycle_right.ibd");
    tmp_frm_path = path_join(app_path, "ownerless_rename_cycle_tmp.frm");
    tmp_ibd_path = path_join(app_path, "ownerless_rename_cycle_tmp.ibd");

    close(rename_ready_pipe[1]);
    close(rename_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(rename_release_pipe[1]);
    wait_for_pipe_message(rename_ready_pipe[0]);
    assert(path_exists(left_frm_path));
    assert(path_exists(left_ibd_path));
    assert(path_exists(right_frm_path));
    assert(path_exists(right_ibd_path));
    assert(!path_exists(tmp_frm_path));
    assert(!path_exists(tmp_ibd_path));
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_cycle_left") == 30U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_cycle_right") == 300U);
    left_space_before = query_unsigned(
        db,
        "SELECT SPACE FROM information_schema.INNODB_SYS_TABLES "
        "WHERE NAME = 'app/ownerless_rename_cycle_left'"
    );
    right_space_before = query_unsigned(
        db,
        "SELECT SPACE FROM information_schema.INNODB_SYS_TABLES "
        "WHERE NAME = 'app/ownerless_rename_cycle_right'"
    );
    assert(left_space_before > 0U);
    assert(right_space_before > 0U);
    assert(left_space_before != right_space_before);
    exec_ok(db, "UPDATE app.ownerless_rename_cycle_left SET value = value + 1 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_cycle_left") == 31U);

    signal_pipe_message(rename_release_pipe[1]);
    wait_for_pipe_message(rename_ready_pipe[0]);
    assert(path_exists(left_frm_path));
    assert(path_exists(left_ibd_path));
    assert(path_exists(right_frm_path));
    assert(path_exists(right_ibd_path));
    assert(!path_exists(tmp_frm_path));
    assert(!path_exists(tmp_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_cycle_tmp'"
        ) == 0U
    );
    left_space_after = query_unsigned(
        db,
        "SELECT SPACE FROM information_schema.INNODB_SYS_TABLES "
        "WHERE NAME = 'app/ownerless_rename_cycle_left'"
    );
    right_space_after = query_unsigned(
        db,
        "SELECT SPACE FROM information_schema.INNODB_SYS_TABLES "
        "WHERE NAME = 'app/ownerless_rename_cycle_right'"
    );
    assert(left_space_after == right_space_before);
    assert(right_space_after == left_space_before);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_rename_cycle_left") == 2U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_rename_cycle_left") == 30U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_cycle_left") == 300U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_rename_cycle_left WHERE note = 'right'"
        ) == 2U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_rename_cycle_right") == 2U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_rename_cycle_right") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_cycle_right") == 31U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_rename_cycle_right WHERE note = 'left'"
        ) == 2U
    );
    exec_ok(db, "INSERT INTO app.ownerless_rename_cycle_left VALUES (30, 300, 'peer-left')");
    exec_ok(db, "INSERT INTO app.ownerless_rename_cycle_right VALUES (3, 30, 'peer-right')");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_cycle_left") == 600U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_cycle_right") == 61U);

    signal_pipe_message(rename_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(rename_ready_pipe[0]);
    close(rename_release_pipe[1]);
    wait_for_child(rename_child);

    assert_ownerless_multi_rename_cycle_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_multi_rename_cycle_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_multi_rename_cycle_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_multi_rename_cycle_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(tmp_ibd_path);
    free(tmp_frm_path);
    free(right_ibd_path);
    free(right_frm_path);
    free(left_ibd_path);
    free(left_frm_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_view_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-view-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int view_ready_pipe[2];
    int view_release_pipe[2];
    pid_t view_child;
    char *datadir_path;
    char *app_path;
    char *view_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(view_ready_pipe) == 0);
    assert(pipe(view_release_pipe) == 0);

    view_child = fork();
    assert(view_child >= 0);
    if (view_child == 0) {
        close(view_ready_pipe[0]);
        close(view_release_pipe[1]);
        run_ownerless_view_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = view_ready_pipe[1],
                .release_read_fd = view_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    view_path = path_join(app_path, "ownerless_view.frm");

    close(view_ready_pipe[1]);
    close(view_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(path_exists(view_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view' "
            "AND table_type = 'VIEW'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view") == 10U);
    exec_ok(db, "INSERT INTO app.ownerless_view_base VALUES (2, 20)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view") == 30U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view'"
        ) == 0U
    );
    assert(exec_status(db, "SELECT SUM(value) FROM app.ownerless_view", NULL) != MYLITE_OK);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_base") == 30U);
    assert(!path_exists(view_path));

    assert(mylite_close(db) == MYLITE_OK);
    close(view_ready_pipe[0]);
    close(view_release_pipe[1]);
    wait_for_child(view_child);

    assert_ownerless_view_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_view_ddl_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_view_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_view_ddl_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(view_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_view_ddl_variants_refresh_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-view-ddl-variants.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int view_ready_pipe[2];
    int view_release_pipe[2];
    pid_t view_child;
    char *datadir_path;
    char *app_path;
    char *view_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(view_ready_pipe) == 0);
    assert(pipe(view_release_pipe) == 0);

    view_child = fork();
    assert(view_child >= 0);
    if (view_child == 0) {
        close(view_ready_pipe[0]);
        close(view_release_pipe[1]);
        run_ownerless_view_ddl_variant_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = view_ready_pipe[1],
                .release_read_fd = view_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    view_path = path_join(app_path, "ownerless_view_variant.frm");

    close(view_ready_pipe[1]);
    close(view_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(path_exists(view_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_variant'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(doubled) FROM app.ownerless_view_variant") == 60U);
    exec_ok(db, "INSERT INTO app.ownerless_view_variant_base VALUES (3, 30)");
    assert(query_unsigned(db, "SELECT SUM(doubled) FROM app.ownerless_view_variant") == 120U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(
        exec_status(db, "SELECT SUM(doubled) FROM app.ownerless_view_variant", NULL) != MYLITE_OK
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_variant") == 2U);
    assert(query_unsigned(db, "SELECT SUM(adjusted) FROM app.ownerless_view_variant") == 60U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_variant") == 1U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_variant") == 30U);
    assert(query_unsigned(db, "SELECT SUM(adjusted) FROM app.ownerless_view_variant") == 29U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_variant'"
        ) == 0U
    );
    assert(
        exec_status(db, "SELECT SUM(adjusted) FROM app.ownerless_view_variant", NULL) != MYLITE_OK
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_variant_base") == 60U);
    assert(!path_exists(view_path));

    assert(mylite_close(db) == MYLITE_OK);
    close(view_ready_pipe[0]);
    close(view_release_pipe[1]);
    wait_for_child(view_child);

    assert_ownerless_view_ddl_variant_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_view_ddl_variant_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_view_ddl_variant_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_view_ddl_variant_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(view_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_view_check_option_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-view-check-option.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int view_ready_pipe[2];
    int view_release_pipe[2];
    pid_t view_child;
    char *datadir_path;
    char *app_path;
    char *view_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(view_ready_pipe) == 0);
    assert(pipe(view_release_pipe) == 0);

    view_child = fork();
    assert(view_child >= 0);
    if (view_child == 0) {
        close(view_ready_pipe[0]);
        close(view_release_pipe[1]);
        run_ownerless_view_check_option_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = view_ready_pipe[1],
                .release_read_fd = view_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    view_path = path_join(app_path, "ownerless_view_check.frm");

    close(view_ready_pipe[1]);
    close(view_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(path_exists(view_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_check' "
            "AND check_option = 'CASCADED' "
            "AND is_updatable = 'YES'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_check") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_check") == 30U);
    exec_ok(db, "INSERT INTO app.ownerless_view_check VALUES (3, 30, 'peer-cascaded')");
    expect_exec_mariadb_error(
        db,
        "INSERT INTO app.ownerless_view_check VALUES (90, 5, 'blocked-cascaded')",
        MYLITE_TEST_VIEW_CHECK_FAILED_ERRNO
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_check_base") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_check_base") == 60U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_check' "
            "AND check_option = 'LOCAL' "
            "AND is_updatable = 'YES'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_check") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_check") == 50U);
    exec_ok(db, "INSERT INTO app.ownerless_view_check VALUES (4, 25, 'peer-local')");
    expect_exec_mariadb_error(
        db,
        "INSERT INTO app.ownerless_view_check VALUES (91, 15, 'blocked-local')",
        MYLITE_TEST_VIEW_CHECK_FAILED_ERRNO
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_check_base") == 4U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_check_base") == 85U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_check' "
            "AND check_option = 'CASCADED' "
            "AND is_updatable = 'YES'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_check") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_check") == 55U);
    exec_ok(db, "INSERT INTO app.ownerless_view_check VALUES (5, 40, 'peer-final')");
    expect_exec_mariadb_error(
        db,
        "UPDATE app.ownerless_view_check SET value = 12 WHERE id = 5",
        MYLITE_TEST_VIEW_CHECK_FAILED_ERRNO
    );
    exec_ok(db, "UPDATE app.ownerless_view_check SET value = 27 WHERE id = 4");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_check") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_check") == 97U);

    signal_pipe_message(view_release_pipe[1]);
    wait_for_pipe_message(view_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_check'"
        ) == 0U
    );
    assert(exec_status(db, "SELECT COUNT(*) FROM app.ownerless_view_check", NULL) != MYLITE_OK);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_check_base") == 5U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_check_base") == 127U);
    assert(!path_exists(view_path));

    assert(mylite_close(db) == MYLITE_OK);
    close(view_ready_pipe[0]);
    close(view_release_pipe[1]);
    wait_for_child(view_child);

    assert_ownerless_view_check_option_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_view_check_option_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_view_check_option_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_view_check_option_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(view_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_trigger_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-trigger-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int trigger_ready_pipe[2];
    int trigger_release_pipe[2];
    pid_t trigger_child;
    char *datadir_path;
    char *app_path;
    char *trg_path;
    char *trn_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(trigger_ready_pipe) == 0);
    assert(pipe(trigger_release_pipe) == 0);

    trigger_child = fork();
    assert(trigger_child >= 0);
    if (trigger_child == 0) {
        close(trigger_ready_pipe[0]);
        close(trigger_release_pipe[1]);
        run_ownerless_trigger_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = trigger_ready_pipe[1],
                .release_read_fd = trigger_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    trg_path = path_join(app_path, "ownerless_trigger_base.TRG");
    trn_path = path_join(app_path, "ownerless_trigger_ai.TRN");

    close(trigger_ready_pipe[1]);
    close(trigger_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(path_exists(trg_path));
    assert(path_exists(trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_ai' "
            "AND event_object_table = 'ownerless_trigger_base'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_base") == 10U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_audit") == 10U);
    exec_ok(db, "INSERT INTO app.ownerless_trigger_base VALUES (2, 20)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_base") == 30U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_audit") == 30U);

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_ai'"
        ) == 0U
    );
    assert(!path_exists(trg_path));
    assert(!path_exists(trn_path));
    exec_ok(db, "INSERT INTO app.ownerless_trigger_base VALUES (3, 30)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_base") == 60U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_audit") == 30U);

    assert(mylite_close(db) == MYLITE_OK);
    close(trigger_ready_pipe[0]);
    close(trigger_release_pipe[1]);
    wait_for_child(trigger_child);

    assert_ownerless_trigger_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_trigger_ddl_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_trigger_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_trigger_ddl_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(trn_path);
    free(trg_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_trigger_ddl_variants_refresh_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-trigger-ddl-variants.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int trigger_ready_pipe[2];
    int trigger_release_pipe[2];
    pid_t trigger_child;
    char *datadir_path;
    char *app_path;
    char *trg_path;
    char *update_trn_path;
    char *delete_trn_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(trigger_ready_pipe) == 0);
    assert(pipe(trigger_release_pipe) == 0);

    trigger_child = fork();
    assert(trigger_child >= 0);
    if (trigger_child == 0) {
        close(trigger_ready_pipe[0]);
        close(trigger_release_pipe[1]);
        run_ownerless_trigger_ddl_variant_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = trigger_ready_pipe[1],
                .release_read_fd = trigger_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    trg_path = path_join(app_path, "ownerless_trigger_variant_base.TRG");
    update_trn_path = path_join(app_path, "ownerless_trigger_variant_bu.TRN");
    delete_trn_path = path_join(app_path, "ownerless_trigger_variant_ad.TRN");

    close(trigger_ready_pipe[1]);
    close(trigger_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(path_exists(trg_path));
    assert(path_exists(update_trn_path));
    assert(!path_exists(delete_trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_variant_bu' "
            "AND event_manipulation = 'UPDATE' "
            "AND action_timing = 'BEFORE'"
        ) == 1U
    );
    exec_ok(db, "UPDATE app.ownerless_trigger_variant_base SET value = 20 WHERE id = 1");
    assert(
        query_unsigned(db, "SELECT value FROM app.ownerless_trigger_variant_base WHERE id = 1") ==
        21U
    );

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(path_exists(trg_path));
    assert(path_exists(update_trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_variant_bu' "
            "AND event_manipulation = 'UPDATE' "
            "AND action_timing = 'BEFORE'"
        ) == 1U
    );
    exec_ok(db, "UPDATE app.ownerless_trigger_variant_base SET value = 30 WHERE id = 1");
    assert(
        query_unsigned(db, "SELECT value FROM app.ownerless_trigger_variant_base WHERE id = 1") ==
        32U
    );

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(path_exists(trg_path));
    assert(path_exists(update_trn_path));
    assert(path_exists(delete_trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND event_object_table = 'ownerless_trigger_variant_base'"
        ) == 2U
    );
    exec_ok(db, "DELETE FROM app.ownerless_trigger_variant_base WHERE id = 1");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_variant_base") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_variant_audit") == 1U);
    assert(
        query_unsigned(db, "SELECT SUM(old_value) FROM app.ownerless_trigger_variant_audit") == 32U
    );

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND event_object_table = 'ownerless_trigger_variant_base'"
        ) == 0U
    );
    assert(!path_exists(trg_path));
    assert(!path_exists(update_trn_path));
    assert(!path_exists(delete_trn_path));
    exec_ok(db, "INSERT INTO app.ownerless_trigger_variant_base VALUES (2, 40)");
    exec_ok(db, "UPDATE app.ownerless_trigger_variant_base SET value = 50 WHERE id = 2");
    assert(
        query_unsigned(db, "SELECT value FROM app.ownerless_trigger_variant_base WHERE id = 2") ==
        50U
    );
    exec_ok(db, "DELETE FROM app.ownerless_trigger_variant_base WHERE id = 2");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_variant_base") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_variant_audit") == 1U);
    assert(
        query_unsigned(db, "SELECT SUM(old_value) FROM app.ownerless_trigger_variant_audit") == 32U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(trigger_ready_pipe[0]);
    close(trigger_release_pipe[1]);
    wait_for_child(trigger_child);

    assert_ownerless_trigger_ddl_variant_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_trigger_ddl_variant_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_trigger_ddl_variant_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_trigger_ddl_variant_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(delete_trn_path);
    free(update_trn_path);
    free(trg_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_trigger_ordering_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-trigger-ordering.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int trigger_ready_pipe[2];
    int trigger_release_pipe[2];
    pid_t trigger_child;
    char *datadir_path;
    char *app_path;
    char *trg_path;
    char *first_trn_path;
    char *second_trn_path;
    char *third_trn_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(trigger_ready_pipe) == 0);
    assert(pipe(trigger_release_pipe) == 0);

    trigger_child = fork();
    assert(trigger_child >= 0);
    if (trigger_child == 0) {
        close(trigger_ready_pipe[0]);
        close(trigger_release_pipe[1]);
        run_ownerless_trigger_ordering_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = trigger_ready_pipe[1],
                .release_read_fd = trigger_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    trg_path = path_join(app_path, "ownerless_trigger_order_base.TRG");
    first_trn_path = path_join(app_path, "ownerless_trigger_order_first.TRN");
    second_trn_path = path_join(app_path, "ownerless_trigger_order_second.TRN");
    third_trn_path = path_join(app_path, "ownerless_trigger_order_third.TRN");

    close(trigger_ready_pipe[1]);
    close(trigger_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(path_exists(trg_path));
    assert(path_exists(first_trn_path));
    assert(path_exists(second_trn_path));
    assert(!path_exists(third_trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT action_order FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_order_first'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT action_order FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_order_second'"
        ) == 2U
    );
    assert_show_create_trigger_contains(
        db,
        "SHOW CREATE TRIGGER app.ownerless_trigger_order_second",
        "ownerless_trigger_order_second",
        "ownerless_trigger_order_audit"
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_order_base VALUES (1, 10)");
    assert(
        query_unsigned(
            db,
            "SELECT SUM((audit_id - ("
            "SELECT MIN(audit_id) FROM app.ownerless_trigger_order_audit WHERE base_id = 1"
            ") + 1) * marker) "
            "FROM app.ownerless_trigger_order_audit WHERE base_id = 1"
        ) == 5U
    );

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(path_exists(trg_path));
    assert(path_exists(first_trn_path));
    assert(path_exists(second_trn_path));
    assert(path_exists(third_trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT action_order FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_order_third'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT action_order FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_order_first'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT action_order FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_order_second'"
        ) == 3U
    );
    assert_show_create_trigger_contains(
        db,
        "SHOW CREATE TRIGGER app.ownerless_trigger_order_third",
        "ownerless_trigger_order_third",
        "ownerless_trigger_order_audit"
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_order_base VALUES (2, 20)");
    assert(
        query_unsigned(
            db,
            "SELECT SUM((audit_id - ("
            "SELECT MIN(audit_id) FROM app.ownerless_trigger_order_audit WHERE base_id = 2"
            ") + 1) * marker) "
            "FROM app.ownerless_trigger_order_audit WHERE base_id = 2"
        ) == 11U
    );

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND event_object_table = 'ownerless_trigger_order_base'"
        ) == 0U
    );
    assert(!path_exists(trg_path));
    assert(!path_exists(first_trn_path));
    assert(!path_exists(second_trn_path));
    assert(!path_exists(third_trn_path));
    exec_ok(db, "INSERT INTO app.ownerless_trigger_order_base VALUES (3, 30)");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_trigger_order_audit WHERE base_id = 3"
        ) == 0U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(trigger_ready_pipe[0]);
    close(trigger_release_pipe[1]);
    wait_for_child(trigger_child);

    assert_ownerless_trigger_ordering_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_trigger_ordering_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_trigger_ordering_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_trigger_ordering_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(third_trn_path);
    free(second_trn_path);
    free(first_trn_path);
    free(trg_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_trigger_idempotent_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-trigger-idempotent-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int trigger_ready_pipe[2];
    int trigger_release_pipe[2];
    pid_t trigger_child;
    char *datadir_path;
    char *app_path;
    char *trg_path;
    char *trn_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(trigger_ready_pipe) == 0);
    assert(pipe(trigger_release_pipe) == 0);

    trigger_child = fork();
    assert(trigger_child >= 0);
    if (trigger_child == 0) {
        close(trigger_ready_pipe[0]);
        close(trigger_release_pipe[1]);
        run_ownerless_trigger_idempotent_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = trigger_ready_pipe[1],
                .release_read_fd = trigger_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    trg_path = path_join(app_path, "ownerless_trigger_idempotent_base.TRG");
    trn_path = path_join(app_path, "ownerless_trigger_idempotent_ai.TRN");

    close(trigger_ready_pipe[1]);
    close(trigger_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(path_exists(trg_path));
    assert(path_exists(trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_idempotent_ai' "
            "AND event_object_table = 'ownerless_trigger_idempotent_base'"
        ) == 1U
    );
    expect_exec_mariadb_error(
        db,
        "CREATE TRIGGER app.ownerless_trigger_idempotent_ai "
        "AFTER INSERT ON app.ownerless_trigger_idempotent_base "
        "FOR EACH ROW "
        "INSERT INTO app.ownerless_trigger_idempotent_audit (base_id, value, marker) "
        "VALUES (NEW.id, NEW.value * 10, 9)",
        MYLITE_TEST_TRIGGER_ALREADY_EXISTS_ERRNO
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_idempotent_base VALUES (2, 20)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_audit") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_idempotent_audit") == 30U
    );
    assert(
        query_unsigned(db, "SELECT SUM(marker) FROM app.ownerless_trigger_idempotent_audit") == 2U
    );

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(path_exists(trg_path));
    assert(path_exists(trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_idempotent_ai'"
        ) == 1U
    );
    assert_show_create_trigger_contains(
        db,
        "SHOW CREATE TRIGGER app.ownerless_trigger_idempotent_ai",
        "ownerless_trigger_idempotent_ai",
        "VALUES (NEW.id, NEW.value, 1)"
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_idempotent_base VALUES (3, 30)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_audit") == 3U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_idempotent_audit") == 60U
    );
    assert(
        query_unsigned(db, "SELECT SUM(marker) FROM app.ownerless_trigger_idempotent_audit") == 3U
    );

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(path_exists(trg_path));
    assert(path_exists(trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_idempotent_ai'"
        ) == 1U
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_idempotent_base VALUES (4, 40)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_audit") == 4U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_idempotent_audit") == 100U
    );
    assert(
        query_unsigned(db, "SELECT SUM(marker) FROM app.ownerless_trigger_idempotent_audit") == 4U
    );

    signal_pipe_message(trigger_release_pipe[1]);
    wait_for_pipe_message(trigger_ready_pipe[0]);
    assert(!path_exists(trg_path));
    assert(!path_exists(trn_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_idempotent_ai'"
        ) == 0U
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_idempotent_base VALUES (5, 50)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_base") == 5U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_idempotent_base") == 150U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_audit") == 4U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_idempotent_audit") == 100U
    );
    assert(
        query_unsigned(db, "SELECT SUM(marker) FROM app.ownerless_trigger_idempotent_audit") == 4U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(trigger_ready_pipe[0]);
    close(trigger_release_pipe[1]);
    wait_for_child(trigger_child);

    assert_ownerless_trigger_idempotent_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_trigger_idempotent_ddl_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_trigger_idempotent_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_trigger_idempotent_ddl_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(trn_path);
    free(trg_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_stored_routine_ddl(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-routine-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    expect_exec_error(
        db,
        "CREATE FUNCTION app.ownerless_plus_five(input_value INT) "
        "RETURNS INT DETERMINISTIC "
        "RETURN input_value + 5"
    );
    expect_exec_error(
        db,
        "CREATE OR REPLACE FUNCTION app.ownerless_plus_five(input_value INT) "
        "RETURNS INT DETERMINISTIC "
        "RETURN input_value + 5"
    );
    expect_exec_error(
        db,
        "CREATE PROCEDURE app.ownerless_routine_policy_proc() "
        "BEGIN SELECT 1; END"
    );
    expect_exec_error(db, "DROP FUNCTION app.ownerless_plus_five");
    expect_exec_error(db, "DROP PROCEDURE app.ownerless_routine_policy_proc");
    expect_exec_error(db, "ALTER FUNCTION app.ownerless_plus_five COMMENT 'blocked'");
    expect_exec_error(db, "ALTER PROCEDURE app.ownerless_routine_policy_proc COMMENT 'blocked'");
    assert_ownerless_stored_routine_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );

    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_stored_routine_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_stored_routine_policy_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_stored_routine_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_stored_routine_policy_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_index_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-index-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int index_ready_pipe[2];
    int index_release_pipe[2];
    pid_t index_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(index_ready_pipe) == 0);
    assert(pipe(index_release_pipe) == 0);

    index_child = fork();
    assert(index_child >= 0);
    if (index_child == 0) {
        close(index_ready_pipe[0]);
        close(index_release_pipe[1]);
        run_ownerless_index_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = index_ready_pipe[1],
                .release_read_fd = index_release_pipe[0],
            }
        );
    }

    close(index_ready_pipe[1]);
    close(index_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(index_release_pipe[1]);
    wait_for_pipe_message(index_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_index_base' "
            "AND index_name = 'ownerless_index_value_idx'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_index_base "
            "FORCE INDEX (ownerless_index_value_idx) "
            "WHERE value >= 20"
        ) == 5U
    );
    exec_ok(db, "INSERT INTO app.ownerless_index_base VALUES (4, 40)");
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_index_base "
            "FORCE INDEX (ownerless_index_value_idx) "
            "WHERE value >= 20"
        ) == 9U
    );

    signal_pipe_message(index_release_pipe[1]);
    wait_for_pipe_message(index_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_index_base' "
            "AND index_name = 'ownerless_index_value_idx'"
        ) == 0U
    );
    assert(
        exec_status(
            db,
            "SELECT SUM(id) FROM app.ownerless_index_base "
            "FORCE INDEX (ownerless_index_value_idx) "
            "WHERE value >= 20",
            NULL
        ) != MYLITE_OK
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_index_base") == 100U);

    assert(mylite_close(db) == MYLITE_OK);
    close(index_ready_pipe[0]);
    close(index_release_pipe[1]);
    wait_for_child(index_child);

    assert_ownerless_index_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_index_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_index_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_index_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rename_index_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-rename-index-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int index_ready_pipe[2];
    int index_release_pipe[2];
    pid_t index_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(index_ready_pipe) == 0);
    assert(pipe(index_release_pipe) == 0);

    index_child = fork();
    assert(index_child >= 0);
    if (index_child == 0) {
        close(index_ready_pipe[0]);
        close(index_release_pipe[1]);
        run_ownerless_rename_index_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = index_ready_pipe[1],
                .release_read_fd = index_release_pipe[0],
            }
        );
    }

    close(index_ready_pipe[1]);
    close(index_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(index_release_pipe[1]);
    wait_for_pipe_message(index_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_index_base' "
            "AND index_name = 'ownerless_rename_old_idx'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_index_base' "
            "AND index_name = 'ownerless_rename_new_idx'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_rename_index_base "
            "FORCE INDEX (ownerless_rename_old_idx) "
            "WHERE value >= 20"
        ) == 5U
    );

    signal_pipe_message(index_release_pipe[1]);
    wait_for_pipe_message(index_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_index_base' "
            "AND index_name = 'ownerless_rename_old_idx'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_index_base' "
            "AND index_name = 'ownerless_rename_new_idx'"
        ) == 1U
    );
    assert(
        exec_status(
            db,
            "SELECT SUM(id) FROM app.ownerless_rename_index_base "
            "FORCE INDEX (ownerless_rename_old_idx) "
            "WHERE value >= 20",
            NULL
        ) != MYLITE_OK
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_rename_index_base "
            "FORCE INDEX (ownerless_rename_new_idx) "
            "WHERE value >= 20"
        ) == 5U
    );
    exec_ok(db, "INSERT INTO app.ownerless_rename_index_base VALUES (4, 40)");
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_rename_index_base "
            "FORCE INDEX (ownerless_rename_new_idx) "
            "WHERE value >= 20"
        ) == 9U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(index_ready_pipe[0]);
    close(index_release_pipe[1]);
    wait_for_child(index_child);

    assert_ownerless_rename_index_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_rename_index_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_rename_index_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_rename_index_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_ignored_index_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-ignored-index-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int index_ready_pipe[2];
    int index_release_pipe[2];
    pid_t index_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(index_ready_pipe) == 0);
    assert(pipe(index_release_pipe) == 0);

    index_child = fork();
    assert(index_child >= 0);
    if (index_child == 0) {
        close(index_ready_pipe[0]);
        close(index_release_pipe[1]);
        run_ownerless_ignored_index_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = index_ready_pipe[1],
                .release_read_fd = index_release_pipe[0],
            }
        );
    }

    close(index_ready_pipe[1]);
    close(index_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(index_release_pipe[1]);
    wait_for_pipe_message(index_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ignored_index_base' "
            "AND index_name = 'ownerless_ignored_value_idx' "
            "AND ignored = 'NO'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_ignored_index_base "
            "FORCE INDEX (ownerless_ignored_value_idx) "
            "WHERE value >= 20"
        ) == 5U
    );

    signal_pipe_message(index_release_pipe[1]);
    wait_for_pipe_message(index_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ignored_index_base' "
            "AND index_name = 'ownerless_ignored_value_idx' "
            "AND ignored = 'YES'"
        ) == 1U
    );
    exec_ok(db, "INSERT INTO app.ownerless_ignored_index_base VALUES (4, 40, 400)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_ignored_index_base") == 100U);

    signal_pipe_message(index_release_pipe[1]);
    wait_for_pipe_message(index_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ignored_index_base' "
            "AND index_name = 'ownerless_ignored_value_idx' "
            "AND ignored = 'NO'"
        ) == 1U
    );
    exec_ok(db, "INSERT INTO app.ownerless_ignored_index_base VALUES (5, 50, 500)");
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_ignored_index_base "
            "FORCE INDEX (ownerless_ignored_value_idx) "
            "WHERE value >= 20"
        ) == 14U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(index_ready_pipe[0]);
    close(index_release_pipe[1]);
    wait_for_child(index_child);

    assert_ownerless_ignored_index_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_ignored_index_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_ignored_index_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_ignored_index_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_unique_index_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-unique-index-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int index_ready_pipe[2];
    int index_release_pipe[2];
    pid_t index_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(index_ready_pipe) == 0);
    assert(pipe(index_release_pipe) == 0);

    index_child = fork();
    assert(index_child >= 0);
    if (index_child == 0) {
        close(index_ready_pipe[0]);
        close(index_release_pipe[1]);
        run_ownerless_unique_index_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = index_ready_pipe[1],
                .release_read_fd = index_release_pipe[0],
            }
        );
    }

    close(index_ready_pipe[1]);
    close(index_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(index_release_pipe[1]);
    wait_for_pipe_message(index_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_unique_index_base' "
            "AND index_name = 'ownerless_unique_tenant_slug' "
            "AND non_unique = 0"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(weight) FROM app.ownerless_unique_index_base "
            "FORCE INDEX (ownerless_unique_tenant_slug) "
            "WHERE tenant_id = 1 AND slug = 'alpha'"
        ) == 10U
    );
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_unique_index_base "
            "VALUES (4, 1, 'alpha', 40)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_DUPLICATE_KEY_ERRNO);
    exec_ok(db, "INSERT INTO app.ownerless_unique_index_base VALUES (4, 2, 'beta', 40)");

    signal_pipe_message(index_release_pipe[1]);
    wait_for_pipe_message(index_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_unique_index_base' "
            "AND index_name = 'ownerless_unique_tenant_slug'"
        ) == 0U
    );
    assert(
        exec_status(
            db,
            "SELECT SUM(weight) FROM app.ownerless_unique_index_base "
            "FORCE INDEX (ownerless_unique_tenant_slug) "
            "WHERE tenant_id = 1 AND slug = 'alpha'",
            NULL
        ) != MYLITE_OK
    );
    exec_ok(db, "INSERT INTO app.ownerless_unique_index_base VALUES (5, 1, 'alpha', 50)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_unique_index_base") == 5U);
    assert(query_unsigned(db, "SELECT SUM(weight) FROM app.ownerless_unique_index_base") == 150U);

    assert(mylite_close(db) == MYLITE_OK);
    close(index_ready_pipe[0]);
    close(index_release_pipe[1]);
    wait_for_child(index_child);

    assert_ownerless_unique_index_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_unique_index_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_unique_index_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_unique_index_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_primary_key_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-primary-key-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int primary_ready_pipe[2];
    int primary_release_pipe[2];
    pid_t primary_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(primary_ready_pipe) == 0);
    assert(pipe(primary_release_pipe) == 0);

    primary_child = fork();
    assert(primary_child >= 0);
    if (primary_child == 0) {
        close(primary_ready_pipe[0]);
        close(primary_release_pipe[1]);
        run_ownerless_primary_key_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = primary_ready_pipe[1],
                .release_read_fd = primary_release_pipe[0],
            }
        );
    }

    close(primary_ready_pipe[1]);
    close(primary_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(primary_release_pipe[1]);
    wait_for_pipe_message(primary_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_primary_key_base' "
            "AND index_name = 'PRIMARY' "
            "AND column_name = 'code' "
            "AND seq_in_index = 1 "
            "AND non_unique = 0"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM app.ownerless_primary_key_base "
            "FORCE INDEX (PRIMARY) "
            "WHERE code >= 20"
        ) == 500U
    );
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_primary_key_base "
            "VALUES (4, 20, 400)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_DUPLICATE_KEY_ERRNO);
    exec_ok(db, "INSERT INTO app.ownerless_primary_key_base VALUES (1, 40, 400)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_primary_key_base") == 4U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_primary_key_base") == 1000U);

    assert(mylite_close(db) == MYLITE_OK);
    close(primary_ready_pipe[0]);
    close(primary_release_pipe[1]);
    wait_for_child(primary_child);

    assert_ownerless_primary_key_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_primary_key_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_primary_key_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_primary_key_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_foreign_key_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_alter_child_parent' "
            "AND table_name = 'ownerless_fk_alter_child' "
            "AND referenced_table_name = 'ownerless_fk_alter_parent'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_alter_child") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child FORCE INDEX (PRIMARY)"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child "
            "FORCE INDEX (ownerless_fk_alter_parent_idx)"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_alter_child") == 100U);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_alter_child WHERE id = 1") == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child WHERE parent_id = 1"
        ) == 1U
    );
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_alter_child VALUES (2, 99, 990)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    exec_ok(db, "INSERT INTO app.ownerless_fk_alter_child VALUES (2, 2, 200)");
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_alter_child") == 2U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child FORCE INDEX (PRIMARY)"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child "
            "FORCE INDEX (ownerless_fk_alter_parent_idx)"
        ) == 2U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_alter_child") == 300U);
    exec_ok(db, "INSERT INTO app.ownerless_fk_alter_child VALUES (3, 99, 990)");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_alter_child_parent'"
        ) == 0U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_alter_child") == 3U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child FORCE INDEX (PRIMARY)"
        ) == 3U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child "
            "FORCE INDEX (ownerless_fk_alter_parent_idx)"
        ) == 3U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_alter_child") == 1290U);

    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_foreign_key_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_foreign_key_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_foreign_key_ddl_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_foreign_key_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_actions_cross_process(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-actions.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_foreign_key_action_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_parent") == 3U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_action_cascade' "
            "AND update_rule = 'CASCADE' "
            "AND delete_rule = 'CASCADE'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_action_set_null' "
            "AND update_rule = 'CASCADE' "
            "AND delete_rule = 'SET NULL'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_action_restrict' "
            "AND delete_rule = 'RESTRICT'"
        ) == 1U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_parent WHERE id = 1") == 0U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_parent WHERE id = 10") ==
        1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_action_cascade_child "
            "WHERE parent_id = 10"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_action_null_child "
            "WHERE parent_id = 10"
        ) == 1U
    );
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_action_cascade_child VALUES (3, 1, 150)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    exec_ok(db, "INSERT INTO app.ownerless_fk_action_cascade_child VALUES (3, 10, 150)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_action_null_child VALUES (3, 10, 350)");
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_parent WHERE id = 2") == 0U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_cascade_child") == 2U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_action_cascade_child "
            "WHERE parent_id = 10"
        ) == 2U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_null_child") == 3U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_action_null_child WHERE parent_id IS NULL"
        ) == 1U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_parent WHERE id = 3") == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_restrict_child") == 1U);

    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_foreign_key_action_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_foreign_key_action_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_foreign_key_action_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_foreign_key_action_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_composite_foreign_keys_cross_process(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-composite-foreign-key.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_composite_foreign_key_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_composite_parent") == 3U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_composite_child_parent' "
            "AND update_rule = 'CASCADE' "
            "AND delete_rule = 'RESTRICT'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_composite_child "
            "WHERE tenant_id = 1 AND parent_id = 10"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_composite_child "
            "WHERE tenant_id = 2 AND parent_id = 10"
        ) == 1U
    );
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_composite_child VALUES (3, 1, 99, 99)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    exec_ok(db, "INSERT INTO app.ownerless_composite_child VALUES (3, 2, 10, 33)");
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_composite_parent "
            "WHERE tenant_id = 1 AND id = 10"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_composite_parent "
            "WHERE tenant_id = 1 AND id = 11"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_composite_child "
            "WHERE tenant_id = 1 AND parent_id = 11"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_composite_child "
            "WHERE tenant_id = 2 AND parent_id = 10"
        ) == 2U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_composite_parent "
            "WHERE tenant_id = 2 AND id = 10"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_composite_child") == 3U);

    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_composite_foreign_key_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_composite_foreign_key_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_composite_foreign_key_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_composite_foreign_key_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_deep_cascade_cross_process(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-deep-cascade.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_foreign_key_deep_cascade_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_root") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level1") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level2") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level3") == 2U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_deep_l1' "
            "AND table_name = 'ownerless_fk_deep_level1' "
            "AND referenced_table_name = 'ownerless_fk_deep_root' "
            "AND update_rule = 'CASCADE' "
            "AND delete_rule = 'CASCADE'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_deep_l2' "
            "AND table_name = 'ownerless_fk_deep_level2' "
            "AND referenced_table_name = 'ownerless_fk_deep_level1' "
            "AND update_rule = 'CASCADE' "
            "AND delete_rule = 'CASCADE'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_deep_l3' "
            "AND table_name = 'ownerless_fk_deep_level3' "
            "AND referenced_table_name = 'ownerless_fk_deep_level2' "
            "AND update_rule = 'CASCADE' "
            "AND delete_rule = 'CASCADE'"
        ) == 1U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_root WHERE id = 1") == 0U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level1 WHERE id = 1") == 0U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level2 WHERE id = 1") == 0U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level3 WHERE id = 1") == 0U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_root WHERE id = 10") == 1U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level1 WHERE id = 10") == 1U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level2 WHERE id = 10") == 1U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level3 WHERE id = 10") == 1U
    );
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_deep_level3 VALUES (1, 9000)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    exec_ok(db, "INSERT INTO app.ownerless_fk_deep_root VALUES (3, 30)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_deep_level1 VALUES (3, 300)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_deep_level2 VALUES (3, 3000)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_deep_level3 VALUES (3, 30000)");
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_root WHERE id = 2") == 0U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level1 WHERE id = 2") == 0U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level2 WHERE id = 2") == 0U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level3 WHERE id = 2") == 0U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_root") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level1") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level2") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level3") == 2U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_deep_root") == 13U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_deep_level1") == 13U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_deep_level2") == 13U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_deep_level3") == 13U);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_root WHERE id IN (3, 10)") ==
        2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_deep_level1 WHERE id IN (3, 10)"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_deep_level2 WHERE id IN (3, 10)"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_deep_level3 WHERE id IN (3, 10)"
        ) == 2U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_deep_root") == 1030U);

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_foreign_key_deep_cascade_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_foreign_key_deep_cascade_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_foreign_key_deep_cascade_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_foreign_key_deep_cascade_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_generated_column_foreign_key_cross_process(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-generated-column-foreign-key.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_generated_column_foreign_key_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_parent") == 3U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_child") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_child") == 203U
    );
    assert(
        query_unsigned(db, "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_ref_parent") ==
        606U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_ref_child") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_ref_child") ==
        403U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_generated_child_parent' "
            "AND table_name = 'ownerless_fk_generated_child' "
            "AND referenced_table_name = 'ownerless_fk_generated_parent' "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'CASCADE'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_generated_ref_parent' "
            "AND table_name = 'ownerless_fk_generated_ref_child' "
            "AND referenced_table_name = 'ownerless_fk_generated_ref_parent' "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'CASCADE'"
        ) == 1U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_parent WHERE id = 101"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_parent WHERE id = 151"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_ref_parent "
            "WHERE parent_key = 201"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_ref_parent "
            "WHERE parent_key = 211"
        ) == 0U
    );
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_generated_child (id, raw_parent, value) "
            "VALUES (4, 99, 990)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    mariadb_errno = 0U;
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_generated_ref_child VALUES (4, 299, 990)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_parent WHERE id = 102"
        ) == 0U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_child") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_child WHERE parent_key = 102"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_ref_parent WHERE id = 2"
        ) == 0U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_ref_child") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_ref_child WHERE parent_key = 202"
        ) == 0U
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_child (id, raw_parent, value) "
        "VALUES (3, 3, 30)"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_generated_ref_child VALUES (3, 203, 300)");
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_generated_column_foreign_key_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_generated_column_foreign_key_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_generated_column_foreign_key_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_generated_column_foreign_key_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_generated_column_foreign_key_policy(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-generated-column-fk-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_generated_column_foreign_key_policy_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_policy_parent") == 2U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_policy_stored_alter") ==
        1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_policy_virtual_alter"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_policy_stored_alter"
        ) == 101U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_policy_virtual_alter"
        ) == 101U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fk_generated_policy_update_null', "
            "'ownerless_fk_generated_policy_update_cascade', "
            "'ownerless_fk_generated_policy_delete_null')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_generated_policy_virtual_create_fk', "
            "'ownerless_fk_generated_policy_virtual_alter_fk') "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'CASCADE'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_generated_policy_update_null_fk', "
            "'ownerless_fk_generated_policy_update_cascade_fk', "
            "'ownerless_fk_generated_policy_delete_null_fk', "
            "'ownerless_fk_generated_policy_alter_update_null', "
            "'ownerless_fk_generated_policy_alter_update_cascade', "
            "'ownerless_fk_generated_policy_alter_delete_null')"
        ) == 0U
    );
    unsigned mariadb_errno = 0U;
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_generated_policy_virtual_create "
            "(id, raw_parent, value) VALUES (4, 99, 990)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    mariadb_errno = 0U;
    assert(
        exec_status(
            db,
            "UPDATE app.ownerless_fk_generated_policy_parent SET id = 151 WHERE id = 101",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    exec_ok(db, "DELETE FROM app.ownerless_fk_generated_policy_parent WHERE id = 102");
    exec_ok(db, "INSERT INTO app.ownerless_fk_generated_policy_parent VALUES (103, 300)");
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_policy_stored_alter (id, raw_parent, value) "
        "VALUES (2, 3, 30)"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_policy_virtual_alter (id, raw_parent, value) "
        "VALUES (2, 3, 30)"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_policy_virtual_create (id, raw_parent, value) "
        "VALUES (3, 3, 30)"
    );
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_generated_column_foreign_key_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_generated_column_foreign_key_policy_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_generated_column_foreign_key_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_generated_column_foreign_key_policy_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_cyclic_foreign_key_cross_process(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-cyclic-foreign-key.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_cyclic_foreign_key_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(b_id) FROM app.ownerless_fk_cycle_a") == 1U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(a_id) FROM app.ownerless_fk_cycle_b") == 1U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_update_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle_update_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(b_key) FROM app.ownerless_fk_cycle_update_a") == 1U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_update_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(a_id) FROM app.ownerless_fk_cycle_update_b") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_cycle_a_b', "
            "'ownerless_fk_cycle_b_a') "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'CASCADE'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_cycle_update_a_b', "
            "'ownerless_fk_cycle_update_b_a') "
            "AND update_rule = 'CASCADE' "
            "AND delete_rule = 'CASCADE'"
        ) == 2U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_a WHERE id = 1") == 1U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_a WHERE id = 2") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_b WHERE id = 1") == 1U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_b WHERE id = 2") == 0U);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_update_a WHERE id = 1") ==
        1U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_update_a WHERE id = 2") ==
        0U
    );
    assert(query_unsigned(db, "SELECT SUM(a_id) FROM app.ownerless_fk_cycle_update_b") == 1U);
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_cycle_b VALUES (2, 999, 999)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    mariadb_errno = 0U;
    assert(
        exec_status(
            db,
            "UPDATE app.ownerless_fk_cycle_a SET b_id = 999 WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_a") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_b") == 0U);
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_a VALUES (10, NULL, 100)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_b VALUES (20, 10, 200)");
    exec_ok(db, "UPDATE app.ownerless_fk_cycle_a SET b_id = 20 WHERE id = 10");
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_cyclic_foreign_key_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_cyclic_foreign_key_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_cyclic_foreign_key_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_cyclic_foreign_key_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_cyclic_foreign_key_variants_cross_process(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-cyclic-foreign-key-variants.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_cyclic_foreign_key_variants_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle3_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(c_id) FROM app.ownerless_fk_cycle3_a") == 3U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle3_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(a_id) FROM app.ownerless_fk_cycle3_b") == 1U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle3_c") == 1U);
    assert(query_unsigned(db, "SELECT SUM(b_id) FROM app.ownerless_fk_cycle3_c") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(b_id) FROM app.ownerless_fk_cycle_null_a") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(a_id) FROM app.ownerless_fk_cycle_null_b") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_cycle3_a_c', "
            "'ownerless_fk_cycle3_b_a', "
            "'ownerless_fk_cycle3_c_b') "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'CASCADE'"
        ) == 3U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_cycle_null_a_b', "
            "'ownerless_fk_cycle_null_b_a') "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'SET NULL'"
        ) == 2U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle3_a") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle3_b") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle3_c") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_a") == 0U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_b "
            "WHERE id = 2 AND a_id IS NULL"
        ) == 1U
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle3_a VALUES (10, NULL, 100)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle3_b VALUES (20, 10, 200)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle3_c VALUES (30, 20, 300)");
    exec_ok(db, "UPDATE app.ownerless_fk_cycle3_a SET c_id = 30 WHERE id = 10");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_null_a VALUES (10, NULL, 10000)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_null_b VALUES (20, 10, 20000)");
    exec_ok(db, "UPDATE app.ownerless_fk_cycle_null_a SET b_id = 20 WHERE id = 10");
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_a "
            "WHERE id = 10 AND b_id IS NULL"
        ) == 1U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_b WHERE id = 20") == 0U
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_null_a VALUES (100, NULL, 100000)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_null_b VALUES (200, 100, 200000)");
    exec_ok(db, "UPDATE app.ownerless_fk_cycle_null_a SET b_id = 200 WHERE id = 100");
    exec_ok(db, "COMMIT");

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_cyclic_foreign_key_variants_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_cyclic_foreign_key_variants_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_cyclic_foreign_key_variants_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_cyclic_foreign_key_variants_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_rename_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-rename.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;
    char *datadir_path;
    char *app_path;
    char *parent_frm_path;
    char *parent_ibd_path;
    char *moved_frm_path;
    char *moved_ibd_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_foreign_key_rename_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    parent_frm_path = path_join(app_path, "ownerless_fk_rename_parent.frm");
    parent_ibd_path = path_join(app_path, "ownerless_fk_rename_parent.ibd");
    moved_frm_path = path_join(app_path, "ownerless_fk_rename_parent_moved.frm");
    moved_ibd_path = path_join(app_path, "ownerless_fk_rename_parent_moved.ibd");

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(path_exists(parent_frm_path));
    assert(path_exists(parent_ibd_path));
    assert(!path_exists(moved_frm_path));
    assert(!path_exists(moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_rename_child_parent' "
            "AND table_name = 'ownerless_fk_rename_child' "
            "AND referenced_table_name = 'ownerless_fk_rename_parent'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_rename_child") == 1U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_rename_child") == 100U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(!path_exists(parent_frm_path));
    assert(!path_exists(parent_ibd_path));
    assert(path_exists(moved_frm_path));
    assert(path_exists(moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_rename_parent'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_rename_parent_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_rename_child_parent' "
            "AND table_name = 'ownerless_fk_rename_child' "
            "AND referenced_table_name = 'ownerless_fk_rename_parent_moved'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_rename_parent", NULL) != MYLITE_OK
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_rename_parent_moved") == 30U
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_rename_child VALUES (2, 2, 200)");
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_rename_child VALUES (3, 99, 300)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        exec_status(
            db,
            "DELETE FROM app.ownerless_fk_rename_parent_moved WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_rename_child") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_rename_child") == 300U);

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_foreign_key_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_rename_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_foreign_key_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_rename_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(moved_ibd_path);
    free(moved_frm_path);
    free(parent_ibd_path);
    free(parent_frm_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_child_rename_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-child-rename.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;
    char *datadir_path;
    char *app_path;
    char *child_frm_path;
    char *child_ibd_path;
    char *moved_frm_path;
    char *moved_ibd_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_foreign_key_child_rename_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    child_frm_path = path_join(app_path, "ownerless_fk_child_rename_child.frm");
    child_ibd_path = path_join(app_path, "ownerless_fk_child_rename_child.ibd");
    moved_frm_path = path_join(app_path, "ownerless_fk_child_rename_child_moved.frm");
    moved_ibd_path = path_join(app_path, "ownerless_fk_child_rename_child_moved.ibd");

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(path_exists(child_frm_path));
    assert(path_exists(child_ibd_path));
    assert(!path_exists(moved_frm_path));
    assert(!path_exists(moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_child_rename_child_ibfk_1' "
            "AND table_name = 'ownerless_fk_child_rename_child' "
            "AND referenced_table_name = 'ownerless_fk_child_rename_parent'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_child_rename_parent") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_child_rename_child") == 1U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_child_rename_child") == 100U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(!path_exists(child_frm_path));
    assert(!path_exists(child_ibd_path));
    assert(path_exists(moved_frm_path));
    assert(path_exists(moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_child_rename_child'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_child_rename_child_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_child_rename_child_ibfk_1'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_child_rename_child_moved_ibfk_1' "
            "AND table_name = 'ownerless_fk_child_rename_child_moved' "
            "AND referenced_table_name = 'ownerless_fk_child_rename_parent'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_child_rename_child", NULL) !=
        MYLITE_OK
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_child_rename_child_moved") ==
        100U
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_child_rename_child_moved VALUES (2, 2, 200)");
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_child_rename_child_moved VALUES (3, 99, 300)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        exec_status(
            db,
            "DELETE FROM app.ownerless_fk_child_rename_parent WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_child_rename_child_moved") == 2U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_child_rename_child_moved") ==
        300U
    );

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_foreign_key_child_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_child_rename_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_foreign_key_child_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_child_rename_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(moved_ibd_path);
    free(moved_frm_path);
    free(child_ibd_path);
    free(child_frm_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_cross_schema_rename_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-cross-schema-rename.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;
    char *datadir_path;
    char *app_path;
    char *target_schema_path;
    char *parent_frm_path;
    char *parent_ibd_path;
    char *moved_frm_path;
    char *moved_ibd_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_foreign_key_cross_schema_rename_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    target_schema_path = path_join(datadir_path, "ownerless_fk_rename_schema");
    parent_frm_path = path_join(app_path, "ownerless_fk_cross_schema_parent.frm");
    parent_ibd_path = path_join(app_path, "ownerless_fk_cross_schema_parent.ibd");
    moved_frm_path = path_join(target_schema_path, "ownerless_fk_cross_schema_parent_moved.frm");
    moved_ibd_path = path_join(target_schema_path, "ownerless_fk_cross_schema_parent_moved.ibd");

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(path_exists(target_schema_path));
    assert(path_exists(parent_frm_path));
    assert(path_exists(parent_ibd_path));
    assert(!path_exists(moved_frm_path));
    assert(!path_exists(moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND unique_constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_cross_schema_child_parent' "
            "AND table_name = 'ownerless_fk_cross_schema_child' "
            "AND referenced_table_name = 'ownerless_fk_cross_schema_parent'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_child") == 1U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cross_schema_child") == 100U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(!path_exists(parent_frm_path));
    assert(!path_exists(parent_ibd_path));
    assert(path_exists(moved_frm_path));
    assert(path_exists(moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_cross_schema_parent'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_fk_rename_schema' "
            "AND table_name = 'ownerless_fk_cross_schema_parent_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND unique_constraint_schema = 'ownerless_fk_rename_schema' "
            "AND constraint_name = 'ownerless_fk_cross_schema_child_parent' "
            "AND table_name = 'ownerless_fk_cross_schema_child' "
            "AND referenced_table_name = 'ownerless_fk_cross_schema_parent_moved'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_parent", NULL) !=
        MYLITE_OK
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_rename_schema."
            "ownerless_fk_cross_schema_parent_moved"
        ) == 30U
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_cross_schema_child VALUES (2, 2, 200)");
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_cross_schema_child VALUES (3, 99, 300)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        exec_status(
            db,
            "DELETE FROM ownerless_fk_rename_schema.ownerless_fk_cross_schema_parent_moved "
            "WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_child") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cross_schema_child") == 300U
    );

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_foreign_key_cross_schema_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_cross_schema_rename_state(
        paths,
        MYLITE_OPEN_READWRITE,
        database_path
    );
    remove_concurrency_shm(database_path);
    assert_ownerless_foreign_key_cross_schema_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_cross_schema_rename_state(
        paths,
        MYLITE_OPEN_READWRITE,
        database_path
    );

    free(moved_ibd_path);
    free(moved_frm_path);
    free(parent_ibd_path);
    free(parent_frm_path);
    free(target_schema_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_cross_schema_child_rename_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-cross-schema-child-rename.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;
    char *datadir_path;
    char *app_path;
    char *target_schema_path;
    char *child_frm_path;
    char *child_ibd_path;
    char *moved_frm_path;
    char *moved_ibd_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_foreign_key_cross_schema_child_rename_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    target_schema_path = path_join(datadir_path, "ownerless_fk_child_rename_schema");
    child_frm_path = path_join(app_path, "ownerless_fk_cross_schema_child_child.frm");
    child_ibd_path = path_join(app_path, "ownerless_fk_cross_schema_child_child.ibd");
    moved_frm_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_child_child_moved.frm");
    moved_ibd_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_child_child_moved.ibd");

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(path_exists(target_schema_path));
    assert(path_exists(child_frm_path));
    assert(path_exists(child_ibd_path));
    assert(!path_exists(moved_frm_path));
    assert(!path_exists(moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND unique_constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_cross_schema_child_child_ibfk_1' "
            "AND table_name = 'ownerless_fk_cross_schema_child_child' "
            "AND referenced_table_name = 'ownerless_fk_cross_schema_child_parent'"
        ) == 1U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_child_parent") == 2U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_child_child") == 1U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cross_schema_child_child") ==
        100U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(!path_exists(child_frm_path));
    assert(!path_exists(child_ibd_path));
    assert(path_exists(moved_frm_path));
    assert(path_exists(moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_cross_schema_child_child'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_fk_child_rename_schema' "
            "AND table_name = 'ownerless_fk_cross_schema_child_child_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_cross_schema_child_child_ibfk_1'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'ownerless_fk_child_rename_schema' "
            "AND unique_constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_cross_schema_child_child_moved_ibfk_1' "
            "AND table_name = 'ownerless_fk_cross_schema_child_child_moved' "
            "AND referenced_table_name = 'ownerless_fk_cross_schema_child_parent'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_child_child", NULL) !=
        MYLITE_OK
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_child_rename_schema."
            "ownerless_fk_cross_schema_child_child_moved"
        ) == 100U
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_fk_child_rename_schema."
        "ownerless_fk_cross_schema_child_child_moved VALUES (2, 2, 200)"
    );
    assert(
        exec_status(
            db,
            "INSERT INTO ownerless_fk_child_rename_schema."
            "ownerless_fk_cross_schema_child_child_moved VALUES (3, 99, 300)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        exec_status(
            db,
            "DELETE FROM app.ownerless_fk_cross_schema_child_parent WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_fk_child_rename_schema."
            "ownerless_fk_cross_schema_child_child_moved"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_child_rename_schema."
            "ownerless_fk_cross_schema_child_child_moved"
        ) == 300U
    );

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_foreign_key_cross_schema_child_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_cross_schema_child_rename_state(
        paths,
        MYLITE_OPEN_READWRITE,
        database_path
    );
    remove_concurrency_shm(database_path);
    assert_ownerless_foreign_key_cross_schema_child_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_cross_schema_child_rename_state(
        paths,
        MYLITE_OPEN_READWRITE,
        database_path
    );

    free(moved_ibd_path);
    free(moved_frm_path);
    free(child_ibd_path);
    free(child_frm_path);
    free(target_schema_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_multi_rename_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-multi-rename.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;
    char *datadir_path;
    char *app_path;
    char *parent_frm_path;
    char *parent_ibd_path;
    char *parent_tmp_frm_path;
    char *parent_tmp_ibd_path;
    char *parent_moved_frm_path;
    char *parent_moved_ibd_path;
    char *child_frm_path;
    char *child_ibd_path;
    char *child_moved_frm_path;
    char *child_moved_ibd_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_foreign_key_multi_rename_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    parent_frm_path = path_join(app_path, "ownerless_fk_multi_rename_parent.frm");
    parent_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_parent.ibd");
    parent_tmp_frm_path = path_join(app_path, "ownerless_fk_multi_rename_parent_tmp.frm");
    parent_tmp_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_parent_tmp.ibd");
    parent_moved_frm_path = path_join(app_path, "ownerless_fk_multi_rename_parent_moved.frm");
    parent_moved_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_parent_moved.ibd");
    child_frm_path = path_join(app_path, "ownerless_fk_multi_rename_child.frm");
    child_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_child.ibd");
    child_moved_frm_path = path_join(app_path, "ownerless_fk_multi_rename_child_moved.frm");
    child_moved_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_child_moved.ibd");

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(path_exists(parent_frm_path));
    assert(path_exists(parent_ibd_path));
    assert(!path_exists(parent_tmp_frm_path));
    assert(!path_exists(parent_tmp_ibd_path));
    assert(!path_exists(parent_moved_frm_path));
    assert(!path_exists(parent_moved_ibd_path));
    assert(path_exists(child_frm_path));
    assert(path_exists(child_ibd_path));
    assert(!path_exists(child_moved_frm_path));
    assert(!path_exists(child_moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND unique_constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_multi_rename_child_ibfk_1' "
            "AND table_name = 'ownerless_fk_multi_rename_child' "
            "AND referenced_table_name = 'ownerless_fk_multi_rename_parent'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_parent") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_multi_rename_parent") == 30U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_child") == 1U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_multi_rename_child") == 100U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(!path_exists(parent_frm_path));
    assert(!path_exists(parent_ibd_path));
    assert(!path_exists(parent_tmp_frm_path));
    assert(!path_exists(parent_tmp_ibd_path));
    assert(path_exists(parent_moved_frm_path));
    assert(path_exists(parent_moved_ibd_path));
    assert(!path_exists(child_frm_path));
    assert(!path_exists(child_ibd_path));
    assert(path_exists(child_moved_frm_path));
    assert(path_exists(child_moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fk_multi_rename_parent', "
            "'ownerless_fk_multi_rename_parent_tmp', "
            "'ownerless_fk_multi_rename_child')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fk_multi_rename_parent_moved', "
            "'ownerless_fk_multi_rename_child_moved')"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_multi_rename_child_ibfk_1'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND unique_constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_multi_rename_child_moved_ibfk_1' "
            "AND table_name = 'ownerless_fk_multi_rename_child_moved' "
            "AND referenced_table_name = 'ownerless_fk_multi_rename_parent_moved'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_parent", NULL) !=
        MYLITE_OK
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_parent_tmp", NULL) !=
        MYLITE_OK
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_child", NULL) !=
        MYLITE_OK
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_multi_rename_parent_moved") ==
        30U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_multi_rename_child_moved") ==
        100U
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_multi_rename_child_moved VALUES (2, 2, 200)");
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_fk_multi_rename_child_moved VALUES (3, 99, 300)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        exec_status(
            db,
            "DELETE FROM app.ownerless_fk_multi_rename_parent_moved WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_child_moved") == 2U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_multi_rename_child_moved") ==
        300U
    );

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_foreign_key_multi_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_multi_rename_state(paths, MYLITE_OPEN_READWRITE, database_path);
    remove_concurrency_shm(database_path);
    assert_ownerless_foreign_key_multi_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_multi_rename_state(paths, MYLITE_OPEN_READWRITE, database_path);

    free(child_moved_ibd_path);
    free(child_moved_frm_path);
    free(child_ibd_path);
    free(child_frm_path);
    free(parent_moved_ibd_path);
    free(parent_moved_frm_path);
    free(parent_tmp_ibd_path);
    free(parent_tmp_frm_path);
    free(parent_ibd_path);
    free(parent_frm_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_foreign_key_cross_schema_multi_rename_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-foreign-key-cross-schema-multi-rename.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int fk_ready_pipe[2];
    int fk_release_pipe[2];
    pid_t fk_child;
    char *datadir_path;
    char *app_path;
    char *target_schema_path;
    char *parent_frm_path;
    char *parent_ibd_path;
    char *parent_moved_frm_path;
    char *parent_moved_ibd_path;
    char *child_frm_path;
    char *child_ibd_path;
    char *child_moved_frm_path;
    char *child_moved_ibd_path;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(fk_ready_pipe) == 0);
    assert(pipe(fk_release_pipe) == 0);

    fk_child = fork();
    assert(fk_child >= 0);
    if (fk_child == 0) {
        close(fk_ready_pipe[0]);
        close(fk_release_pipe[1]);
        run_ownerless_foreign_key_cross_schema_multi_rename_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = fk_ready_pipe[1],
                .release_read_fd = fk_release_pipe[0],
            }
        );
    }

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    target_schema_path = path_join(datadir_path, "ownerless_fk_cross_schema_multi_schema");
    parent_frm_path = path_join(app_path, "ownerless_fk_cross_schema_multi_parent.frm");
    parent_ibd_path = path_join(app_path, "ownerless_fk_cross_schema_multi_parent.ibd");
    parent_moved_frm_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_multi_parent_moved.frm");
    parent_moved_ibd_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_multi_parent_moved.ibd");
    child_frm_path = path_join(app_path, "ownerless_fk_cross_schema_multi_child.frm");
    child_ibd_path = path_join(app_path, "ownerless_fk_cross_schema_multi_child.ibd");
    child_moved_frm_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_multi_child_moved.frm");
    child_moved_ibd_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_multi_child_moved.ibd");

    close(fk_ready_pipe[1]);
    close(fk_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(path_exists(target_schema_path));
    assert(path_exists(parent_frm_path));
    assert(path_exists(parent_ibd_path));
    assert(!path_exists(parent_moved_frm_path));
    assert(!path_exists(parent_moved_ibd_path));
    assert(path_exists(child_frm_path));
    assert(path_exists(child_ibd_path));
    assert(!path_exists(child_moved_frm_path));
    assert(!path_exists(child_moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND unique_constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_cross_schema_multi_child_ibfk_1' "
            "AND table_name = 'ownerless_fk_cross_schema_multi_child' "
            "AND referenced_table_name = 'ownerless_fk_cross_schema_multi_parent'"
        ) == 1U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_multi_parent") == 2U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cross_schema_multi_parent") ==
        30U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_multi_child") == 1U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cross_schema_multi_child") ==
        100U
    );

    signal_pipe_message(fk_release_pipe[1]);
    wait_for_pipe_message(fk_ready_pipe[0]);
    assert(!path_exists(parent_frm_path));
    assert(!path_exists(parent_ibd_path));
    assert(path_exists(parent_moved_frm_path));
    assert(path_exists(parent_moved_ibd_path));
    assert(!path_exists(child_frm_path));
    assert(!path_exists(child_ibd_path));
    assert(path_exists(child_moved_frm_path));
    assert(path_exists(child_moved_ibd_path));
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fk_cross_schema_multi_parent', "
            "'ownerless_fk_cross_schema_multi_child')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_fk_cross_schema_multi_schema' "
            "AND table_name IN ("
            "'ownerless_fk_cross_schema_multi_parent_moved', "
            "'ownerless_fk_cross_schema_multi_child_moved')"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_cross_schema_multi_child_ibfk_1'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'ownerless_fk_cross_schema_multi_schema' "
            "AND unique_constraint_schema = 'ownerless_fk_cross_schema_multi_schema' "
            "AND constraint_name = 'ownerless_fk_cross_schema_multi_child_moved_ibfk_1' "
            "AND table_name = 'ownerless_fk_cross_schema_multi_child_moved' "
            "AND referenced_table_name = 'ownerless_fk_cross_schema_multi_parent_moved'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_multi_parent", NULL) !=
        MYLITE_OK
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_multi_child", NULL) !=
        MYLITE_OK
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_parent_moved"
        ) == 30U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_child_moved"
        ) == 100U
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_fk_cross_schema_multi_schema."
        "ownerless_fk_cross_schema_multi_child_moved VALUES (2, 2, 200)"
    );
    assert(
        exec_status(
            db,
            "INSERT INTO ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_child_moved VALUES (3, 99, 300)",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_NO_REFERENCED_ROW_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        exec_status(
            db,
            "DELETE FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_parent_moved WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_child_moved"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_child_moved"
        ) == 300U
    );

    signal_pipe_message(fk_release_pipe[1]);
    assert(mylite_close(db) == MYLITE_OK);
    close(fk_ready_pipe[0]);
    close(fk_release_pipe[1]);
    wait_for_child(fk_child);

    assert_ownerless_foreign_key_cross_schema_multi_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_cross_schema_multi_rename_state(
        paths,
        MYLITE_OPEN_READWRITE,
        database_path
    );
    remove_concurrency_shm(database_path);
    assert_ownerless_foreign_key_cross_schema_multi_rename_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_foreign_key_cross_schema_multi_rename_state(
        paths,
        MYLITE_OPEN_READWRITE,
        database_path
    );

    free(child_moved_ibd_path);
    free(child_moved_frm_path);
    free(child_ibd_path);
    free(child_frm_path);
    free(parent_moved_ibd_path);
    free(parent_moved_frm_path);
    free(parent_ibd_path);
    free(parent_frm_path);
    free(target_schema_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_check_constraint_ddl_refreshes_peer_dictionary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-check-constraint-ddl.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int check_ready_pipe[2];
    int check_release_pipe[2];
    pid_t check_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(check_ready_pipe) == 0);
    assert(pipe(check_release_pipe) == 0);

    check_child = fork();
    assert(check_child >= 0);
    if (check_child == 0) {
        close(check_ready_pipe[0]);
        close(check_release_pipe[1]);
        run_ownerless_check_constraint_ddl_sequence(
            paths,
            (child_pipes){
                .ready_write_fd = check_ready_pipe[1],
                .release_read_fd = check_release_pipe[0],
            }
        );
    }

    close(check_ready_pipe[1]);
    close(check_release_pipe[0]);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);

    signal_pipe_message(check_release_pipe[1]);
    wait_for_pipe_message(check_ready_pipe[0]);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_check_alter") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.check_constraints "
            "WHERE constraint_schema = 'app' "
            "AND table_name = 'ownerless_check_alter' "
            "AND constraint_name IN ('ownerless_check_positive', 'ownerless_check_label') "
            "AND level = 'Table'"
        ) == 2U
    );
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_check_alter VALUES (2, 0, 'zero')",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_CHECK_CONSTRAINT_ERRNO);
    exec_ok(db, "COMMIT");
    assert(
        exec_status(
            db,
            "INSERT INTO app.ownerless_check_alter VALUES (2, 5, 'xy')",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_CHECK_CONSTRAINT_ERRNO);
    exec_ok(db, "COMMIT");
    exec_ok(db, "INSERT INTO app.ownerless_check_alter VALUES (2, 25, 'valid')");
    exec_ok(db, "COMMIT");

    signal_pipe_message(check_release_pipe[1]);
    wait_for_pipe_message(check_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.check_constraints "
            "WHERE constraint_schema = 'app' "
            "AND table_name = 'ownerless_check_alter' "
            "AND constraint_name IN ('ownerless_check_positive', 'ownerless_check_label')"
        ) == 0U
    );
    exec_ok(db, "INSERT INTO app.ownerless_check_alter VALUES (3, 0, 'xy')");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_check_alter") == 3U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_check_alter "
            "WHERE id = 3 AND value = 0 AND label = 'xy'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_check_alter") == 35U);

    assert(mylite_close(db) == MYLITE_OK);
    close(check_ready_pipe[0]);
    close(check_release_pipe[1]);
    wait_for_child(check_child);

    assert_ownerless_check_constraint_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_check_constraint_ddl_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_check_constraint_ddl_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_check_constraint_ddl_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_table_admin_sql(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-table-admin-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_table_admin_policy ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "note VARCHAR(64) NOT NULL, "
        "INDEX ownerless_table_admin_value_idx (value)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_table_admin_policy VALUES "
        "(1, 10, 'alpha'), (2, 20, 'beta')"
    );
    expect_exec_error(db, "ANALYZE TABLE app.ownerless_table_admin_policy");
    expect_exec_error(
        db,
        "ANALYZE LOCAL TABLE app.ownerless_table_admin_policy PERSISTENT FOR ALL"
    );
    expect_exec_error(db, "CHECK TABLE app.ownerless_table_admin_policy FOR UPGRADE");
    expect_exec_error(db, "CHECKSUM TABLE app.ownerless_table_admin_policy");
    expect_exec_error(db, "CHECKSUM TABLE app.ownerless_table_admin_policy QUICK");
    expect_exec_error(db, "CHECKSUM TABLE app.ownerless_table_admin_policy EXTENDED");
    expect_exec_error(db, "OPTIMIZE TABLE app.ownerless_table_admin_policy");
    expect_exec_error(db, "OPTIMIZE NO_WRITE_TO_BINLOG TABLE app.ownerless_table_admin_policy");
    expect_exec_error(db, "REPAIR TABLE app.ownerless_table_admin_policy");
    expect_exec_error(db, "REPAIR LOCAL TABLE app.ownerless_table_admin_policy QUICK");
    exec_ok(db, "INSERT INTO app.ownerless_table_admin_policy VALUES (3, 30, 'gamma')");
    assert_ownerless_table_admin_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_table_admin_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_table_admin_policy_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_table_admin_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_table_admin_policy_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_lock_tables_sql(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-lock-tables-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_lock_tables_policy ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "INDEX ownerless_lock_tables_value_idx (value)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_lock_tables_policy VALUES "
        "(1, 10), (2, 20)"
    );
    expect_exec_error(db, "LOCK TABLES app.ownerless_lock_tables_policy READ");
    expect_exec_error(db, "LOCK TABLE app.ownerless_lock_tables_policy WRITE");
    expect_exec_error(
        db,
        "LOCK TABLES app.ownerless_lock_tables_policy AS locked_alias READ LOCAL"
    );
    expect_exec_error(db, "UNLOCK TABLES");
    expect_exec_error(db, "UNLOCK TABLE");
    exec_ok(db, "INSERT INTO app.ownerless_lock_tables_policy VALUES (3, 30)");
    assert_ownerless_lock_tables_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_lock_tables_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_lock_tables_policy_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_lock_tables_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_lock_tables_policy_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_flush_table_lock_sql(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-flush-table-lock-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_flush_table_lock_policy ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "INDEX ownerless_flush_table_lock_value_idx (value)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_flush_table_lock_policy VALUES "
        "(1, 10), (2, 20)"
    );
    exec_ok(db, "FLUSH TABLES");
    exec_ok(db, "FLUSH TABLE app.ownerless_flush_table_lock_policy");
    expect_exec_error(db, "FLUSH TABLES WITH READ LOCK");
    expect_exec_error(db, "FLUSH TABLES app.ownerless_flush_table_lock_policy WITH READ LOCK");
    expect_exec_error(
        db,
        "FLUSH TABLE app.ownerless_flush_table_lock_policy WITH READ LOCK AND DISABLE CHECKPOINT"
    );
    expect_exec_error(db, "FLUSH TABLES app.ownerless_flush_table_lock_policy FOR EXPORT");
    expect_exec_error(db, "FLUSH LOCAL TABLES app.ownerless_flush_table_lock_policy FOR EXPORT");
    expect_exec_error(
        db,
        "FLUSH NO_WRITE_TO_BINLOG TABLES app.ownerless_flush_table_lock_policy WITH READ LOCK"
    );
    exec_ok(db, "INSERT INTO app.ownerless_flush_table_lock_policy VALUES (3, 30)");
    assert_ownerless_flush_table_lock_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_flush_table_lock_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_flush_table_lock_policy_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_flush_table_lock_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_flush_table_lock_policy_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_read_uncommitted_isolation(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-read-uncommitted-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    expect_exec_error(db, "SET SESSION TRANSACTION ISOLATION LEVEL READ UNCOMMITTED");
    expect_exec_error(db, "SET LOCAL TRANSACTION ISOLATION LEVEL READ UNCOMMITTED");
    expect_exec_error(db, "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED");
    expect_exec_error(db, "SET SESSION tx_isolation = 'READ-UNCOMMITTED'");
    expect_exec_error(db, "SET SESSION tx_isolation = 'READ-COMMITTED'");
    expect_exec_error(db, "SET @@tx_isolation = 'READ-UNCOMMITTED'");
    expect_exec_error(db, "SET @@global.tx_isolation = 'READ-UNCOMMITTED'");
    expect_exec_error(db, "SET SESSION transaction_isolation = 'READ UNCOMMITTED'");
    expect_exec_error(db, "SET @@session.transaction_isolation = 'REPEATABLE-READ'");
    expect_exec_error(db, "SET @@session.transaction_isolation = 'READ-UNCOMMITTED'");
    expect_exec_error(db, "SET STATEMENT tx_isolation = 'READ-UNCOMMITTED' FOR SELECT 1");
    expect_exec_error(db, "SET STATEMENT tx_isolation = 'READ-COMMITTED' FOR SELECT 1");

    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
    assert(query_unsigned(db, "SELECT @@tx_isolation = 'READ-COMMITTED'") == 1U);
    exec_ok(db, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED");
    exec_ok(db, "START TRANSACTION");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);
    exec_ok(db, "ROLLBACK");
    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    assert(query_unsigned(db, "SELECT @@tx_isolation = 'REPEATABLE-READ'") == 1U);
    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    assert(query_unsigned(db, "SELECT @@tx_isolation = 'SERIALIZABLE'") == 1U);
    exec_ok(db, "INSERT INTO app.ownerless_sql VALUES (3, 30)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 60U);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL READ UNCOMMITTED");
    assert(query_unsigned(db, "SELECT @@tx_isolation = 'READ-UNCOMMITTED'") == 1U);
    exec_ok(db, "SET SESSION tx_isolation = 'READ-UNCOMMITTED'");
    assert(query_unsigned(db, "SELECT @@tx_isolation = 'READ-UNCOMMITTED'") == 1U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 60U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_sequence_sql(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-sequence-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    exec_ok(
        db,
        "CREATE SEQUENCE app.ownerless_existing_sequence "
        "START WITH 5 INCREMENT BY 5 NOCACHE"
    );
    assert(query_unsigned(db, "SELECT NEXT VALUE FOR app.ownerless_existing_sequence") == 5U);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    expect_exec_error(
        db,
        "CREATE SEQUENCE app.ownerless_sequence "
        "START WITH 7 INCREMENT BY 3 NOCACHE"
    );
    expect_exec_error(
        db,
        "CREATE OR REPLACE SEQUENCE app.ownerless_sequence "
        "START WITH 7 INCREMENT BY 3 NOCACHE"
    );
    expect_exec_error(db, "ALTER SEQUENCE app.ownerless_existing_sequence RESTART WITH 50");
    expect_exec_error(db, "DROP SEQUENCE app.ownerless_existing_sequence");
    expect_exec_error(db, "SELECT NEXT VALUE FOR app.ownerless_existing_sequence");
    expect_exec_error(db, "SELECT PREVIOUS VALUE FOR app.ownerless_existing_sequence");
    expect_exec_error(db, "SELECT NEXTVAL(app.ownerless_existing_sequence)");
    expect_exec_error(db, "SELECT LASTVAL(app.ownerless_existing_sequence)");
    expect_exec_error(db, "SELECT SETVAL(app.ownerless_existing_sequence, 20)");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_sequence'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_existing_sequence'"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT NEXT VALUE FOR app.ownerless_existing_sequence") == 10U);
    exec_ok(db, "DROP SEQUENCE app.ownerless_existing_sequence");
    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_sequence_policy_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_sequence_policy_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_sequence_policy_state(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_ownerless_sequence_policy_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_table_directory_options(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-table-directory-policy.mylite");
    char *external_data_path = path_join(root, "ownerless-external-data");
    char *external_index_path = path_join(root, "ownerless-external-index");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    char sql[1024];
    int written;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_table_directory_policy ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "KEY ownerless_table_directory_value_idx (value)"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_table_directory_policy VALUES (1, 10)");

    written = snprintf(
        sql,
        sizeof(sql),
        "CREATE TABLE app.ownerless_data_directory_policy ("
        "id INT NOT NULL PRIMARY KEY"
        ") ENGINE=InnoDB DATA DIRECTORY='%s'",
        external_data_path
    );
    assert(written > 0 && (size_t)written < sizeof(sql));
    expect_exec_error(db, sql);

    written = snprintf(
        sql,
        sizeof(sql),
        "CREATE TABLE app.ownerless_index_directory_policy ("
        "id INT NOT NULL, KEY ownerless_index_directory_idx (id)"
        ") ENGINE=InnoDB INDEX DIRECTORY='%s'",
        external_index_path
    );
    assert(written > 0 && (size_t)written < sizeof(sql));
    expect_exec_error(db, sql);

    written = snprintf(
        sql,
        sizeof(sql),
        "ALTER TABLE app.ownerless_table_directory_policy "
        "DATA DIRECTORY='%s'",
        external_data_path
    );
    assert(written > 0 && (size_t)written < sizeof(sql));
    expect_exec_error(db, sql);

    written = snprintf(
        sql,
        sizeof(sql),
        "ALTER TABLE app.ownerless_table_directory_policy "
        "INDEX DIRECTORY='%s'",
        external_index_path
    );
    assert(written > 0 && (size_t)written < sizeof(sql));
    expect_exec_error(db, sql);

    written = snprintf(
        sql,
        sizeof(sql),
        "CREATE TABLE app.ownerless_partition_directory_policy ("
        "id INT NOT NULL PRIMARY KEY"
        ") ENGINE=InnoDB PARTITION BY HASH(id) "
        "(PARTITION p0 DATA DIRECTORY='%s')",
        external_data_path
    );
    assert(written > 0 && (size_t)written < sizeof(sql));
    expect_exec_error(db, sql);

    exec_ok(db, "INSERT INTO app.ownerless_table_directory_policy VALUES (2, 20)");
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_table_directory_policy") == 30U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_data_directory_policy', "
            "'ownerless_index_directory_policy', "
            "'ownerless_partition_directory_policy'"
            ")"
        ) == 0U
    );
    assert(!path_exists(external_data_path));
    assert(!path_exists(external_index_path));
    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_table_directory_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        external_data_path,
        external_index_path
    );
    assert_ownerless_table_directory_policy_state(
        paths,
        MYLITE_OPEN_READWRITE,
        external_data_path,
        external_index_path
    );
    remove_concurrency_shm(database_path);
    assert_ownerless_table_directory_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        external_data_path,
        external_index_path
    );
    assert_ownerless_table_directory_policy_state(
        paths,
        MYLITE_OPEN_READWRITE,
        external_data_path,
        external_index_path
    );

    free(external_index_path);
    free(external_data_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_special_index_ddl(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-special-index-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_special_index_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "body TEXT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_special_index_base VALUES (1, 'alpha beta')");
    expect_exec_error(
        db,
        "CREATE FULLTEXT INDEX ownerless_fulltext_idx "
        "ON app.ownerless_special_index_base (body)"
    );
    expect_exec_error(
        db,
        "CREATE OR REPLACE FULLTEXT INDEX ownerless_fulltext_replace_idx "
        "ON app.ownerless_special_index_base (body)"
    );
    expect_exec_error(
        db,
        "ALTER TABLE app.ownerless_special_index_base "
        "ADD FULLTEXT INDEX ownerless_fulltext_alter_idx (body)"
    );
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_fulltext_inline ("
        "id INT NOT NULL PRIMARY KEY, "
        "body TEXT NOT NULL, "
        "FULLTEXT KEY ownerless_fulltext_inline_idx (body)"
        ") ENGINE=InnoDB"
    );
    expect_exec_error(
        db,
        "CREATE SPATIAL INDEX ownerless_spatial_idx "
        "ON app.ownerless_special_index_base (body)"
    );
    expect_exec_error(
        db,
        "ALTER TABLE app.ownerless_special_index_base "
        "ADD SPATIAL INDEX ownerless_spatial_alter_idx (body)"
    );
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_spatial_inline ("
        "id INT NOT NULL PRIMARY KEY, "
        "body TEXT NOT NULL, "
        "SPATIAL INDEX ownerless_spatial_inline_idx (body)"
        ") ENGINE=InnoDB"
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_special_index_base") == 1U);
    assert_ownerless_special_index_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_special_index_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_special_index_policy_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_special_index_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_special_index_policy_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_partition_ddl(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-partition-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_partition_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_partition_base VALUES (1, 10)");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_partition_keyword_column ("
        "`partition` INT NOT NULL PRIMARY KEY"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_partition_keyword_column VALUES (7)");
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_partitioned_range ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB "
        "PARTITION BY RANGE (id) ("
        "PARTITION p0 VALUES LESS THAN (10), "
        "PARTITION pmax VALUES LESS THAN MAXVALUE)"
    );
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_partitioned_hash ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB "
        "PARTITION BY HASH(id) PARTITIONS 2"
    );
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_partitioned_subpart ("
        "id INT NOT NULL, "
        "value INT NOT NULL, "
        "PRIMARY KEY (id, value)"
        ") ENGINE=InnoDB "
        "PARTITION BY RANGE (id) "
        "SUBPARTITION BY HASH(value) SUBPARTITIONS 2 ("
        "PARTITION p0 VALUES LESS THAN (10), "
        "PARTITION pmax VALUES LESS THAN MAXVALUE)"
    );
    expect_exec_error(
        db,
        "ALTER TABLE app.ownerless_partition_base "
        "PARTITION BY HASH(id) PARTITIONS 2"
    );
    expect_exec_error(
        db,
        "ALTER TABLE app.ownerless_partition_base "
        "ADD PARTITION (PARTITION pmax VALUES LESS THAN MAXVALUE)"
    );
    expect_exec_error(
        db,
        "ALTER TABLE app.ownerless_partition_base "
        "TRUNCATE PARTITION p0"
    );
    expect_exec_error(db, "ALTER TABLE app.ownerless_partition_base REMOVE PARTITIONING");
    assert_ownerless_partition_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_partition_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_partition_policy_state(paths, MYLITE_OPEN_READWRITE);
    remove_concurrency_shm(database_path);
    assert_ownerless_partition_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW
    );
    assert_ownerless_partition_policy_state(paths, MYLITE_OPEN_READWRITE);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_tablespace_management_ddl(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-tablespace-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    char *datadir_path;
    char *app_path;
    char *ibd_path;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    datadir_path = path_join(database_path, "datadir");
    app_path = path_join(datadir_path, "app");
    ibd_path = path_join(app_path, "ownerless_tablespace_policy.ibd");

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_tablespace_policy ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_tablespace_policy VALUES (1, 10)");
    assert(path_exists(ibd_path));
    expect_exec_error(db, "ALTER TABLE app.ownerless_tablespace_policy DISCARD TABLESPACE");
    expect_exec_error(db, "ALTER TABLE app.ownerless_tablespace_policy IMPORT TABLESPACE");
    assert_ownerless_tablespace_management_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert(mylite_close(db) == MYLITE_OK);

    assert_ownerless_tablespace_management_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_tablespace_management_policy_state(
        paths,
        MYLITE_OPEN_READWRITE,
        database_path
    );
    remove_concurrency_shm(database_path);
    assert_ownerless_tablespace_management_policy_state(
        paths,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
        database_path
    );
    assert_ownerless_tablespace_management_policy_state(
        paths,
        MYLITE_OPEN_READWRITE,
        database_path
    );

    free(ibd_path);
    free(app_path);
    free(datadir_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_temporary_tablespace_allows_peer_temp_tables(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-temporary-tables.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int first_ready_pipe[2];
    int first_release_pipe[2];
    int second_ready_pipe[2];
    int second_release_pipe[2];
    pid_t first_child;
    pid_t second_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(first_ready_pipe) == 0);
    assert(pipe(first_release_pipe) == 0);
    assert(pipe(second_ready_pipe) == 0);
    assert(pipe(second_release_pipe) == 0);

    first_child = fork();
    assert(first_child >= 0);
    if (first_child == 0) {
        close(first_ready_pipe[0]);
        close(first_release_pipe[1]);
        close(second_ready_pipe[0]);
        close(second_ready_pipe[1]);
        close(second_release_pipe[0]);
        close(second_release_pipe[1]);
        hold_ownerless_temporary_table_until_released(
            paths,
            11U,
            (child_pipes){
                .ready_write_fd = first_ready_pipe[1],
                .release_read_fd = first_release_pipe[0],
            }
        );
    }

    second_child = fork();
    assert(second_child >= 0);
    if (second_child == 0) {
        close(second_ready_pipe[0]);
        close(second_release_pipe[1]);
        close(first_ready_pipe[0]);
        close(first_ready_pipe[1]);
        close(first_release_pipe[0]);
        close(first_release_pipe[1]);
        hold_ownerless_temporary_table_until_released(
            paths,
            17U,
            (child_pipes){
                .ready_write_fd = second_ready_pipe[1],
                .release_read_fd = second_release_pipe[0],
            }
        );
    }

    close(first_ready_pipe[1]);
    close(first_release_pipe[0]);
    close(second_ready_pipe[1]);
    close(second_release_pipe[0]);
    wait_for_pipe(first_ready_pipe[0]);
    wait_for_pipe(second_ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);
    assert(mylite_close(db) == MYLITE_OK);

    signal_pipe(first_release_pipe[1]);
    signal_pipe(second_release_pipe[1]);
    wait_for_child(first_child);
    wait_for_child(second_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_temp_peer ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_temp_peer VALUES (1, 23)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_temp_peer") == 23U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_ownerless_temporary_table_peer_is_recovered(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-temporary-table-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int survivor_ready_pipe[2];
    int survivor_release_pipe[2];
    int crashed_ready_pipe[2];
    int crashed_release_pipe[2];
    pid_t survivor_child;
    pid_t crashed_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(survivor_ready_pipe) == 0);
    assert(pipe(survivor_release_pipe) == 0);
    assert(pipe(crashed_ready_pipe) == 0);
    assert(pipe(crashed_release_pipe) == 0);

    survivor_child = fork();
    assert(survivor_child >= 0);
    if (survivor_child == 0) {
        close(survivor_ready_pipe[0]);
        close(survivor_release_pipe[1]);
        close(crashed_ready_pipe[0]);
        close(crashed_ready_pipe[1]);
        close(crashed_release_pipe[0]);
        close(crashed_release_pipe[1]);
        hold_ownerless_temporary_table_until_released(
            paths,
            11U,
            (child_pipes){
                .ready_write_fd = survivor_ready_pipe[1],
                .release_read_fd = survivor_release_pipe[0],
            }
        );
    }

    crashed_child = fork();
    assert(crashed_child >= 0);
    if (crashed_child == 0) {
        close(crashed_ready_pipe[0]);
        close(crashed_release_pipe[1]);
        close(survivor_ready_pipe[0]);
        close(survivor_ready_pipe[1]);
        close(survivor_release_pipe[0]);
        close(survivor_release_pipe[1]);
        hold_ownerless_temporary_table_until_released(
            paths,
            17U,
            (child_pipes){
                .ready_write_fd = crashed_ready_pipe[1],
                .release_read_fd = crashed_release_pipe[0],
            }
        );
    }

    close(survivor_ready_pipe[1]);
    close(survivor_release_pipe[0]);
    close(crashed_ready_pipe[1]);
    close(crashed_release_pipe[0]);
    wait_for_pipe(survivor_ready_pipe[0]);
    wait_for_pipe(crashed_ready_pipe[0]);

    assert(kill(crashed_child, SIGKILL) == 0);
    wait_for_signaled_child(crashed_child, SIGKILL);
    assert(close(crashed_release_pipe[1]) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);
    exec_ok(
        db,
        "CREATE TEMPORARY TABLE app.ownerless_temp_peer ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_temp_peer VALUES (1, 23)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_temp_peer") == 23U);
    assert(mylite_close(db) == MYLITE_OK);

    signal_pipe(survivor_release_pipe[1]);
    wait_for_child(survivor_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_temp_peer ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_temp_peer VALUES (1, 29)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_temp_peer") == 29U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_rejects_non_innodb_engines(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-engine-policy.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    mylite_db *mixed_mode_db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(open_database_result(paths, MYLITE_OPEN_READWRITE, &mixed_mode_db) == MYLITE_BUSY);
    assert(mixed_mode_db == NULL);
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_myisam (id INT NOT NULL PRIMARY KEY) ENGINE=MyISAM"
    );
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_aria (id INT NOT NULL PRIMARY KEY) ENGINE=Aria"
    );
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_memory (id INT NOT NULL PRIMARY KEY) ENGINE=MEMORY"
    );
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_blackhole (id INT NOT NULL PRIMARY KEY) ENGINE=BLACKHOLE"
    );
    expect_exec_error(
        db,
        "CREATE TABLE app.ownerless_long_engine ("
        "id INT NOT NULL PRIMARY KEY, c01 INT, c02 INT, c03 INT, c04 INT, "
        "c05 INT, c06 INT, c07 INT, c08 INT, c09 INT, c10 INT, c11 INT, "
        "c12 INT, c13 INT, c14 INT, c15 INT, c16 INT, c17 INT, c18 INT"
        ") ENGINE=MyISAM"
    );
    expect_exec_error(db, "SET SESSION default_storage_engine = MyISAM");
    expect_exec_error(db, "SET SESSION storage_engine = MyISAM");
    expect_exec_error(db, "SET SESSION default_tmp_storage_engine = MyISAM");
    expect_exec_error(db, "SET SESSION enforce_storage_engine = MyISAM");
    expect_exec_error(db, "ALTER TABLE app.ownerless_sql ENGINE=MyISAM");
    exec_ok(db, "CREATE TABLE app.ownerless_innodb (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB");
    exec_ok(db, "CREATE TABLE app.ownerless_engine_column (engine INT NOT NULL PRIMARY KEY)");
    exec_ok(db, "SET SESSION default_storage_engine = DEFAULT");
    exec_ok(db, "CREATE TABLE app.ownerless_default (id INT NOT NULL PRIMARY KEY)");
    assert(mylite_close(db) == MYLITE_OK);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void test_crashed_page_publish_before_append_rebuilds_ownerless_state(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-page-publish-before-append-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    pid_t writer_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(ready_pipe[0]);
        update_first_row_until_page_publish_before_append_fault(paths, ready_pipe[1]);
    }

    close(ready_pipe[1]);
    wait_for_pipe(ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    assert_total_value_is_one_of(paths, 30U, 130U);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 2");
    assert(mylite_close(db) == MYLITE_OK);
    assert_total_value_is_one_of(paths, 31U, 131U);

    remove_concurrency_shm(database_path);
    assert_total_value_is_one_of(paths, 31U, 131U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_page_publish_rebuilds_ownerless_state(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-page-publish-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    pid_t writer_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(ready_pipe[0]);
        update_first_row_until_page_publish_fault(paths, ready_pipe[1]);
    }

    close(ready_pipe[1]);
    wait_for_pipe(ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    assert_total_value_is_one_of(paths, 30U, 130U);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 2");
    assert(mylite_close(db) == MYLITE_OK);
    assert_total_value_is_one_of(paths, 31U, 131U);

    remove_concurrency_shm(database_path);
    assert_total_value_is_one_of(paths, 31U, 131U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_checkpoint_rebuilds_ownerless_state(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-checkpoint-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db;
    char insert_sql[176];
    int ready_pipe[2];
    pid_t recovery_child;
    unsigned long long row_count;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_checkpoint_crash ("
        "id INT NOT NULL PRIMARY KEY, "
        "value VARBINARY(4000) NOT NULL"
        ") ENGINE=InnoDB"
    );
    for (unsigned id = 1U; id <= 64U; ++id) {
        assert(
            snprintf(
                insert_sql,
                sizeof(insert_sql),
                "INSERT INTO app.ownerless_checkpoint_crash VALUES (%u, REPEAT('y', 4000))",
                id
            ) > 0
        );
        exec_ok(db, insert_sql);
    }
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_checkpoint_crash") == 64U);
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    assert(pipe(ready_pipe) == 0);

    recovery_child = fork();
    assert(recovery_child >= 0);
    if (recovery_child == 0) {
        close(ready_pipe[0]);
        open_database_until_checkpoint_fault(paths, ready_pipe[1]);
    }

    close(ready_pipe[1]);
    wait_for_pipe(ready_pipe[0]);
    assert(kill(recovery_child, SIGKILL) == 0);
    wait_for_signaled_child(recovery_child, SIGKILL);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    row_count = query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_checkpoint_crash");
    assert(row_count == 64U);
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    row_count = query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_checkpoint_crash");
    assert(row_count == 64U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_visible_publish_without_checkpoint_preserves_committed_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-visible-publish-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int proceed_pipe[2];
    int fault_pipe[2];
    pid_t writer_child;
    mylite_db *db;
    uint64_t checkpoint_visible_before;
    uint64_t checkpoint_visible_after;
    uint64_t volatile_visible_after;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(proceed_pipe) == 0);
    assert(pipe(fault_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(ready_pipe[0]);
        close(proceed_pipe[1]);
        close(fault_pipe[0]);
        update_first_row_until_visible_publish_fault(
            paths,
            ready_pipe[1],
            proceed_pipe[0],
            fault_pipe[1]
        );
    }

    close(ready_pipe[1]);
    close(proceed_pipe[0]);
    close(fault_pipe[1]);
    wait_for_pipe(ready_pipe[0]);
    checkpoint_visible_before = read_concurrency_checkpoint_visible_lsn(database_path);
    signal_pipe(proceed_pipe[1]);
    wait_for_pipe(fault_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    checkpoint_visible_after = read_concurrency_checkpoint_visible_lsn(database_path);
    volatile_visible_after = read_concurrency_redo_visible_lsn(database_path);
    assert(checkpoint_visible_after == checkpoint_visible_before);
    assert(volatile_visible_after > checkpoint_visible_after);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 130U);
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 130U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_visible_checkpoint_preserves_committed_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-visible-checkpoint-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    pid_t writer_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(ready_pipe[0]);
        update_first_row_until_visible_checkpoint_fault(paths, ready_pipe[1]);
    }

    close(ready_pipe[1]);
    wait_for_pipe(ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    assert(read_concurrency_checkpoint_visible_lsn(database_path) > 0U);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 130U);
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 130U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_active_pin_reclaims_page_log_with_boundary(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-active-pin-reclaim-boundary.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int old_reader_ready_pipe[2];
    int old_reader_release_pipe[2];
    int second_old_reader_ready_pipe[2];
    int second_old_reader_release_pipe[2];
    int reader_ready_pipe[2];
    int reader_release_pipe[2];
    pid_t old_reader_child;
    pid_t second_old_reader_child;
    pid_t reader_child;
    mylite_db *db;
    uint64_t boundary_lsn;
    uint64_t checkpoint_visible_after;
    unsigned boundary_records_before;
    unsigned boundary_records_after;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_reclaim_boundary_aux ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "payload VARBINARY(4000) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_reclaim_boundary_aux VALUES (1, 10, REPEAT('a', 4000))");
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    assert(pipe(old_reader_ready_pipe) == 0);
    assert(pipe(old_reader_release_pipe) == 0);
    old_reader_child = fork();
    assert(old_reader_child >= 0);
    if (old_reader_child == 0) {
        close(old_reader_ready_pipe[0]);
        close(old_reader_release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = old_reader_ready_pipe[1],
                .release_read_fd = old_reader_release_pipe[0],
            }
        );
    }

    close(old_reader_ready_pipe[1]);
    close(old_reader_release_pipe[0]);
    wait_for_pipe(old_reader_ready_pipe[0]);

    assert(pipe(second_old_reader_ready_pipe) == 0);
    assert(pipe(second_old_reader_release_pipe) == 0);
    second_old_reader_child = fork();
    assert(second_old_reader_child >= 0);
    if (second_old_reader_child == 0) {
        close(second_old_reader_ready_pipe[0]);
        close(second_old_reader_release_pipe[1]);
        close(old_reader_ready_pipe[0]);
        close(old_reader_release_pipe[1]);
        hold_repeatable_read_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = second_old_reader_ready_pipe[1],
                .release_read_fd = second_old_reader_release_pipe[0],
            }
        );
    }

    close(second_old_reader_ready_pipe[1]);
    close(second_old_reader_release_pipe[0]);
    wait_for_pipe(second_old_reader_ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    exec_ok(
        db,
        "UPDATE app.ownerless_reclaim_boundary_aux "
        "SET value = value + 100, payload = REPEAT('b', 4000) WHERE id = 1"
    );
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    assert(!concurrency_wal_is_checkpointed(database_path));

    /* The later reader observes live latest/visible state; .ckpt visible may lag
     * while older pins are still active. */
    boundary_lsn = read_concurrency_checkpoint_latest_lsn(database_path);
    assert(boundary_lsn > 0U);
    boundary_records_before =
        count_concurrency_wal_records_at_or_before(database_path, boundary_lsn);
    assert(boundary_records_before > 0U);

    assert(pipe(reader_ready_pipe) == 0);
    assert(pipe(reader_release_pipe) == 0);
    reader_child = fork();
    assert(reader_child >= 0);
    if (reader_child == 0) {
        close(reader_ready_pipe[0]);
        close(reader_release_pipe[1]);
        hold_reclaim_boundary_snapshot_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = reader_ready_pipe[1],
                .release_read_fd = reader_release_pipe[0],
            }
        );
    }

    close(reader_ready_pipe[1]);
    close(reader_release_pipe[0]);
    wait_for_pipe(reader_ready_pipe[0]);

    assert(kill(old_reader_child, SIGKILL) == 0);
    wait_for_signaled_child(old_reader_child, SIGKILL);
    assert(close(old_reader_release_pipe[1]) == 0);
    assert(kill(second_old_reader_child, SIGKILL) == 0);
    wait_for_signaled_child(second_old_reader_child, SIGKILL);
    assert(close(second_old_reader_release_pipe[1]) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 5 WHERE id = 1");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 135U);
    assert(mylite_close(db) == MYLITE_OK);

    checkpoint_visible_after = read_concurrency_checkpoint_visible_lsn(database_path);
    boundary_records_after =
        count_concurrency_wal_records_at_or_before(database_path, boundary_lsn);
    if (checkpoint_visible_after > boundary_lsn) {
        const unsigned checkpointed_records_after =
            count_concurrency_wal_records_at_or_before(database_path, checkpoint_visible_after);
        assert(checkpointed_records_after == boundary_records_after);
    }
    assert(!concurrency_wal_is_checkpointed(database_path));

    signal_pipe(reader_release_pipe[1]);
    wait_for_child(reader_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 135U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_reclaim_boundary_aux") == 110U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 135U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_reclaim_boundary_aux") == 110U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_redo_reservation_blocks_peer_cleanup_until_reopen_rebuilds(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-redo-reserve-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        update_first_row_until_redo_reserve_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);
    assert_total_value(paths, 30U);

    remove_concurrency_shm(database_path);
    assert_total_value(paths, 30U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_redo_written_blocks_peer_cleanup_until_reopen_rebuilds(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-redo-written-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;
    uint64_t checkpoint_latest_before;
    uint64_t checkpoint_latest_after;
    uint64_t volatile_written_after;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);
    checkpoint_latest_before = read_concurrency_checkpoint_latest_lsn(database_path);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        update_first_row_until_redo_written_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    checkpoint_latest_after = read_concurrency_checkpoint_latest_lsn(database_path);
    volatile_written_after = read_concurrency_redo_written_lsn(database_path);
    assert(checkpoint_latest_after == checkpoint_latest_before);
    assert(volatile_written_after > checkpoint_latest_after);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);
    assert_total_value(paths, 30U);

    remove_concurrency_shm(database_path);
    assert_total_value(paths, 30U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_redo_latest_blocks_peer_cleanup_until_reopen_rebuilds(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-redo-latest-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;
    uint64_t checkpoint_latest_before;
    uint64_t checkpoint_latest_after;
    uint64_t volatile_latest_after;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);
    checkpoint_latest_before = read_concurrency_checkpoint_latest_lsn(database_path);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        update_first_row_until_redo_latest_checkpoint_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    checkpoint_latest_after = read_concurrency_checkpoint_latest_lsn(database_path);
    volatile_latest_after = read_concurrency_redo_latest_lsn(database_path);
    assert(checkpoint_latest_after == checkpoint_latest_before);
    assert(volatile_latest_after > checkpoint_latest_after);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);
    assert_total_value(paths, 30U);

    remove_concurrency_shm(database_path);
    assert_total_value(paths, 30U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_redo_latest_checkpoint_blocks_peer_cleanup_until_reopen_rebuilds(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-redo-latest-checkpoint-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;
    uint64_t checkpoint_latest_before;
    uint64_t checkpoint_latest_after;
    uint64_t checkpoint_visible_before;
    uint64_t checkpoint_visible_after;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);
    checkpoint_latest_before = read_concurrency_checkpoint_latest_lsn(database_path);
    checkpoint_visible_before = read_concurrency_checkpoint_visible_lsn(database_path);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        update_first_row_until_redo_after_checkpoint_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    checkpoint_latest_after = read_concurrency_checkpoint_latest_lsn(database_path);
    checkpoint_visible_after = read_concurrency_checkpoint_visible_lsn(database_path);
    assert(checkpoint_latest_after > checkpoint_latest_before);
    assert(checkpoint_visible_after == checkpoint_visible_before);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);
    assert_total_value(paths, 30U);

    remove_concurrency_shm(database_path);
    assert_total_value(paths, 30U);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_redo_gap_blocks_later_writer_until_rebuild(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-redo-gap-blocks-writer.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int gap_writer_ready_pipe[2];
    int blocked_writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t gap_writer_child;
    pid_t blocked_writer_child;
    pid_t peer_child;
    pid_t probe_child;
    int child_status = 0;
    pid_t wait_result;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_redo_gap_peer ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_redo_gap_peer VALUES (1, 17)");
    assert(mylite_close(db) == MYLITE_OK);

    assert(pipe(gap_writer_ready_pipe) == 0);
    assert(pipe(blocked_writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(gap_writer_ready_pipe[0]);
        close(gap_writer_ready_pipe[1]);
        close(blocked_writer_ready_pipe[0]);
        close(blocked_writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);

    gap_writer_child = fork();
    assert(gap_writer_child >= 0);
    if (gap_writer_child == 0) {
        close(gap_writer_ready_pipe[0]);
        close(blocked_writer_ready_pipe[0]);
        close(blocked_writer_ready_pipe[1]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        update_first_row_until_redo_reserve_fault(paths, gap_writer_ready_pipe[1]);
    }

    close(gap_writer_ready_pipe[1]);
    wait_for_pipe(gap_writer_ready_pipe[0]);

    blocked_writer_child = fork();
    assert(blocked_writer_child >= 0);
    if (blocked_writer_child == 0) {
        close(blocked_writer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        update_redo_gap_peer_table_after_signal(paths, blocked_writer_ready_pipe[1]);
    }
    close(blocked_writer_ready_pipe[1]);
    wait_for_pipe(blocked_writer_ready_pipe[0]);
    sleep_microseconds(1000000U);
    do {
        wait_result = waitpid(blocked_writer_child, &child_status, WNOHANG);
    } while (wait_result < 0 && errno == EINTR);
    assert(wait_result == 0);
    assert(kill(blocked_writer_child, SIGKILL) == 0);
    wait_for_signaled_child(blocked_writer_child, SIGKILL);

    assert(kill(gap_writer_child, SIGKILL) == 0);
    wait_for_signaled_child(gap_writer_child, SIGKILL);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);
    assert_redo_gap_blocked_values(paths);

    remove_concurrency_shm(database_path);
    assert_redo_gap_blocked_values(paths);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_native_checkpoint_reclaim_preserves_committed_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-native-reclaim-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    pid_t writer_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(ready_pipe[0]);
        update_first_row_until_native_checkpoint_reclaim_fault(paths, ready_pipe[1]);
    }

    close(ready_pipe[1]);
    wait_for_pipe(ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    assert(!concurrency_wal_is_checkpointed(database_path));
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 130U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 130U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_native_checkpoint_reclaim_race_preserves_newer_peer_commit(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-native-reclaim-race.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t writer_child;
    mylite_db *db;
    off_t wal_size_after_newer_peer_close;
    off_t wal_size_after_paused_closer;
    int newer_peer_reclaimed_all;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        update_first_row_until_native_checkpoint_reclaim_release(
            paths,
            ready_pipe[1],
            release_pipe[0]
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 130U);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 7 WHERE id = 2");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 137U);
    assert(mylite_close(db) == MYLITE_OK);
    wal_size_after_newer_peer_close = concurrency_wal_size(database_path);
    newer_peer_reclaimed_all = concurrency_wal_is_checkpointed(database_path);

    signal_pipe(release_pipe[1]);
    wait_for_child(writer_child);
    wal_size_after_paused_closer = concurrency_wal_size(database_path);
    assert(wal_size_after_paused_closer <= wal_size_after_newer_peer_close);
    if (newer_peer_reclaimed_all) {
        assert(concurrency_wal_is_checkpointed(database_path));
    } else {
        assert(wal_size_after_paused_closer < wal_size_after_newer_peer_close);
        assert(!concurrency_wal_is_checkpointed(database_path));
    }

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 137U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 137U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(concurrency_wal_is_checkpointed(database_path));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_dictionary_ddl_begin_rebuilds_ownerless_state(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-dictionary-ddl-begin-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        create_table_until_dictionary_begin_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' AND table_name = 'ownerless_ddl_begin_crash'"
        ) == 0U
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_ddl_begin_crash ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_ddl_begin_crash VALUES (1, 10)");
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_ddl_begin_crash") == 10U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_dictionary_ddl_blocks_peer_cleanup_until_reopen_rebuilds(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-dictionary-ddl-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        create_table_until_dictionary_finish_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' AND table_name = 'ownerless_ddl_crash'"
        ) == 1U
    );
    exec_ok(db, "INSERT INTO app.ownerless_ddl_crash VALUES (1, 10)");
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_ddl_crash") == 10U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_crashed_dictionary_ddl_finish_allows_peer_cleanup(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-dictionary-ddl-finish-crash.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t writer_child;
    pid_t peer_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        create_table_until_dictionary_after_finish_fault(paths, writer_ready_pipe[1]);
    }

    close(writer_ready_pipe[1]);
    wait_for_pipe(writer_ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' AND table_name = 'ownerless_ddl_after_finish_crash'"
        ) == 1U
    );
    exec_ok(db, "INSERT INTO app.ownerless_ddl_after_finish_crash VALUES (1, 10)");
    assert(mylite_close(db) == MYLITE_OK);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_ddl_after_finish_crash") == 10U
    );
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}
#endif

static void test_crashed_ownerless_writer_blocks_peer_cleanup_until_reopen_rebuilds(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-crash-recovery.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int writer_ready_pipe[2];
    int peer_ready_pipe[2];
    int peer_release_pipe[2];
    pid_t writer_child;
    pid_t peer_child;
    pid_t probe_child;
    mylite_db *db;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(writer_ready_pipe) == 0);
    assert(pipe(peer_ready_pipe) == 0);
    assert(pipe(peer_release_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(writer_ready_pipe[0]);
        close(peer_ready_pipe[0]);
        close(peer_ready_pipe[1]);
        close(peer_release_pipe[0]);
        close(peer_release_pipe[1]);
        update_first_row_without_commit_until_killed(paths, writer_ready_pipe[1]);
    }

    peer_child = fork();
    assert(peer_child >= 0);
    if (peer_child == 0) {
        close(peer_ready_pipe[0]);
        close(peer_release_pipe[1]);
        close(writer_ready_pipe[0]);
        close(writer_ready_pipe[1]);
        hold_ownerless_open_until_released(
            paths,
            (child_pipes){
                .ready_write_fd = peer_ready_pipe[1],
                .release_read_fd = peer_release_pipe[0],
            }
        );
    }

    close(writer_ready_pipe[1]);
    close(peer_ready_pipe[1]);
    close(peer_release_pipe[0]);
    wait_for_pipe(writer_ready_pipe[0]);
    wait_for_pipe(peer_ready_pipe[0]);

    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    probe_child = fork();
    assert(probe_child >= 0);
    if (probe_child == 0) {
        assert_ownerless_open_returns_busy(paths);
    }
    wait_for_child(probe_child);

    signal_pipe(peer_release_pipe[1]);
    wait_for_child(peer_child);

    assert_shared_readonly_open_returns_busy(paths);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(mylite_close(db) == MYLITE_OK);
    db = open_database(paths, MYLITE_OPEN_READONLY | MYLITE_OPEN_SHARED_READONLY);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void initialize_database(open_database_paths paths) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_sql ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_sql VALUES (1, 10), (2, 20)");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_a (id INT NOT NULL PRIMARY KEY, value INT NOT NULL) "
        "ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_b (id INT NOT NULL PRIMARY KEY, value INT NOT NULL) "
        "ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_a VALUES (1, 100)");
    exec_ok(db, "INSERT INTO app.ownerless_b VALUES (1, 200)");
    assert(mylite_close(db) == MYLITE_OK);
}

static void update_first_row_until_released(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void lock_first_row_for_update_until_released(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    assert(
        query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 1 FOR UPDATE") == 10U
    );
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "ROLLBACK");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}
#endif

static void update_first_row_without_commit_until_killed(open_database_paths paths, int ready_fd) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1");
    signal_pipe(ready_fd);
    for (;;) {
        pause();
    }
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void update_first_row_until_trx_register_fault(open_database_paths paths, int ready_fd) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "trx-after-register", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_record_lock_before_grant_fault(
    open_database_paths paths,
    int ready_fd
) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "record-lock-before-grant", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_record_lock_grant_fault(
    open_database_paths paths,
    int ready_fd
) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "record-lock-after-acquire", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}
#endif

static void hold_ownerless_open_until_released(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void assert_ownerless_open_returns_busy(open_database_paths paths) {
    mylite_db *db = NULL;
    const int result =
        open_database_result(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW, &db);

    if (result == MYLITE_BUSY && db == NULL) {
        _exit(0);
    }
    fprintf(
        stderr,
        "ownerless busy assertion failed: pid=%ld result=%d db=%p\n",
        (long)getpid(),
        result,
        (void *)db
    );
    fflush(stderr);
    if (db != NULL) {
        (void)mylite_close(db);
    }
    _exit(MYLITE_TEST_CHILD_OPEN_FAILED);
}

static void assert_shared_readonly_open_returns_busy(open_database_paths paths) {
    mylite_db *db = NULL;
    const int result =
        open_database_result(paths, MYLITE_OPEN_READONLY | MYLITE_OPEN_SHARED_READONLY, &db);

    assert(result == MYLITE_BUSY);
    assert(db == NULL);
}

static void update_second_row(open_database_paths paths) {
    mylite_db *db;

    db = open_database_allowing_failure(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    if (db == NULL) {
        fprintf(stderr, "update_second_row open returned NULL: pid=%ld\n", (long)getpid());
        fflush(stderr);
        _exit(2);
    }
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 2 WHERE id = 2");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void update_first_row_by_two(open_database_paths paths) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 10");
    assert(
        query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 1 FOR UPDATE") == 11U
    );
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 2 WHERE id = 1");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void update_first_table_until_released(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_a SET value = value + 1 WHERE id = 1");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void update_second_table_until_released(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_b SET value = value + 2 WHERE id = 1");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void commit_race_update_row_after_signal(
    open_database_paths paths,
    unsigned table_id,
    unsigned delta,
    child_pipes pipes
) {
    mylite_db *db;
    char sql[160];

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    assert(
        snprintf(
            sql,
            sizeof(sql),
            "UPDATE app.ownerless_commit_race_%u SET value = value + %u WHERE id = 1",
            table_id,
            delta
        ) > 0
    );
    exec_ok(db, sql);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void update_table_pair_after_signal(
    open_database_paths paths,
    const char *first_table,
    const char *second_table,
    unsigned increment,
    child_pipes pipes
) {
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    char first_update[128];
    char second_update[128];
    int result;

    db = open_database_allowing_failure(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    if (db == NULL) {
        fprintf(
            stderr,
            "update_table_pair_after_signal open returned NULL: pid=%ld first=%s second=%s\n",
            (long)getpid(),
            first_table,
            second_table
        );
        fflush(stderr);
        _exit(MYLITE_TEST_CHILD_OPEN_FAILED);
    }
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "START TRANSACTION");
    assert(
        snprintf(
            first_update,
            sizeof(first_update),
            "UPDATE app.%s SET value = value + %u WHERE id = 1",
            first_table,
            increment
        ) > 0
    );
    assert(
        snprintf(
            second_update,
            sizeof(second_update),
            "UPDATE app.%s SET value = value + %u WHERE id = 1",
            second_table,
            increment
        ) > 0
    );
    exec_ok(db, first_update);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    result = exec_status(db, second_update, &mariadb_errno);
    if (result == MYLITE_OK) {
        exec_ok(db, "COMMIT");
        if (mylite_close(db) != MYLITE_OK) {
            _exit(MYLITE_TEST_CHILD_CLOSE_FAILED);
        }
        _exit(MYLITE_TEST_CHILD_OK);
    }
    if (mariadb_errno == MYLITE_TEST_DEADLOCK_ERRNO) {
        exec_ok(db, "ROLLBACK");
        (void)mylite_close(db);
        _exit(MYLITE_TEST_CHILD_DEADLOCK);
    }
    if (mariadb_errno == MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO) {
        (void)mylite_close(db);
        _exit(MYLITE_TEST_CHILD_LOCK_WAIT_TIMEOUT);
    }

    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void hold_gap_lock_until_released(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(db, "START TRANSACTION");
    assert(query_unsigned(db, "SELECT @@tx_isolation = 'REPEATABLE-READ'") == 1U);
    exec_ok(
        db,
        "SELECT value FROM app.ownerless_gap FORCE INDEX (ownerless_gap_value_idx) "
        "WHERE value = 15 FOR UPDATE"
    );
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "ROLLBACK");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void insert_gap_row_expect_lock_timeout(open_database_paths paths) {
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int result;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 1");
    result = exec_status(db, "INSERT INTO app.ownerless_gap VALUES (15, 15)", &mariadb_errno);
    if (result != MYLITE_OK && mariadb_errno == MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO) {
        (void)mylite_close(db);
        _exit(MYLITE_TEST_CHILD_LOCK_WAIT_TIMEOUT);
    }
    if (result == MYLITE_OK) {
        (void)mylite_close(db);
        _exit(MYLITE_TEST_CHILD_OK);
    }
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_with_savepoint_rollback_until_released(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_sql SET value = 11 WHERE id = 1");
    exec_ok(db, "SAVEPOINT after_first_update");
    exec_ok(db, "UPDATE app.ownerless_sql SET value = 200 WHERE id = 2");
    exec_ok(db, "ROLLBACK TO SAVEPOINT after_first_update");
    exec_ok(db, "RELEASE SAVEPOINT after_first_update");
    assert(query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 11U);
    assert(query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 2") == 20U);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void hold_serializable_read_until_released(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    exec_ok(db, "START TRANSACTION");
    assert(query_unsigned(db, "SELECT @@tx_isolation = 'SERIALIZABLE'") == 1U);
    assert(query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "ROLLBACK");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void update_first_row_expect_lock_timeout(open_database_paths paths) {
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int result;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 1");
    result = exec_status(
        db,
        "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1",
        &mariadb_errno
    );
    if (result != MYLITE_OK && mariadb_errno == MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO) {
        (void)mylite_close(db);
        _exit(MYLITE_TEST_CHILD_LOCK_WAIT_TIMEOUT);
    }
    if (result == MYLITE_OK) {
        (void)mylite_close(db);
        _exit(MYLITE_TEST_CHILD_OK);
    }
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void run_serializable_write_skew_candidate_after_signal(
    open_database_paths paths,
    unsigned doctor_id,
    child_pipes pipes
) {
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int result;
    char sql[128];

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 2");
    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    exec_ok(db, "START TRANSACTION");
    assert(query_unsigned(db, "SELECT @@tx_isolation = 'SERIALIZABLE'") == 1U);
    assert(query_unsigned(db, "SELECT SUM(on_call) FROM app.ownerless_write_skew") == 2U);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    assert(
        snprintf(
            sql,
            sizeof(sql),
            "UPDATE app.ownerless_write_skew SET on_call = 0 WHERE doctor_id = %u",
            doctor_id
        ) > 0
    );
    result = exec_status(db, sql, &mariadb_errno);
    if (result == MYLITE_OK) {
        exec_ok(db, "COMMIT");
        if (mylite_close(db) != MYLITE_OK) {
            _exit(MYLITE_TEST_CHILD_CLOSE_FAILED);
        }
        _exit(MYLITE_TEST_CHILD_OK);
    }
    if (mariadb_errno == MYLITE_TEST_DEADLOCK_ERRNO) {
        exec_ok(db, "ROLLBACK");
        (void)mylite_close(db);
        _exit(MYLITE_TEST_CHILD_DEADLOCK);
    }
    if (mariadb_errno == MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO) {
        exec_ok(db, "ROLLBACK");
        (void)mylite_close(db);
        _exit(MYLITE_TEST_CHILD_LOCK_WAIT_TIMEOUT);
    }

    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void insert_auto_increment_rows_after_signal(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
) {
    mylite_db *db;
    char sql[128];

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned row = 0U; row < MYLITE_TEST_AUTO_INCREMENT_ROWS_PER_WORKER; ++row) {
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_auto_inc (value) VALUES (%u)",
                (worker_id * 1000U) + row
            ) > 0
        );
        exec_ok(db, sql);
    }
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void alter_auto_increment_after_signal(open_database_paths paths, child_pipes pipes) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);

    signal_pipe_message(pipes.ready_write_fd);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "ALTER TABLE app.ownerless_auto_inc_ddl AUTO_INCREMENT = 50");
    signal_pipe_message(pipes.ready_write_fd);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "ALTER TABLE app.ownerless_auto_inc_ddl AUTO_INCREMENT = 2");
    signal_pipe_message(pipes.ready_write_fd);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void increment_mix_row_after_signal(
    open_database_paths paths,
    unsigned row_id,
    child_pipes pipes
) {
    mylite_db *db;
    char update_sql[128];

    assert(
        snprintf(
            update_sql,
            sizeof(update_sql),
            "UPDATE app.ownerless_mix SET value = value + 1 WHERE id = %u",
            row_id
        ) > 0
    );

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    assert(query_unsigned(db, "SELECT @@innodb_lock_wait_timeout") == 30U);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    for (unsigned iteration = 0U; iteration < 16U; ++iteration) {
        exec_ok(db, update_sql);
    }
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void read_mix_total_after_signal(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;
    unsigned long long previous_total = 0U;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    for (unsigned iteration = 0U; iteration < 48U; ++iteration) {
        const unsigned long long total =
            query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_mix");
        assert(total >= previous_total);
        assert(total <= 48U);
        previous_total = total;
        sleep_microseconds(1000U);
    }
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_stress_writer(
    open_database_paths paths,
    unsigned table_id,
    child_pipes pipes
) {
    mylite_db *db;
    char update_sql[128];
    char select_sql[128];
    const unsigned stress_iterations = ownerless_stress_iterations();

    assert(
        snprintf(
            update_sql,
            sizeof(update_sql),
            "UPDATE app.ownerless_stress_%u SET value = value + 1 WHERE id = 1",
            table_id
        ) > 0
    );
    assert(
        snprintf(
            select_sql,
            sizeof(select_sql),
            "SELECT value FROM app.ownerless_stress_%u WHERE id = 1",
            table_id
        ) > 0
    );

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    for (unsigned iteration = 1U; iteration <= stress_iterations; ++iteration) {
        exec_ok(db, update_sql);
        if (iteration % 6U == 0U || iteration == stress_iterations) {
            const unsigned long long observed = query_unsigned(db, select_sql);
            if (observed != iteration) {
                fprintf(
                    stderr,
                    "ownerless stress writer mismatch: pid=%ld table=%u iteration=%u "
                    "observed=%llu\n",
                    (long)getpid(),
                    table_id,
                    iteration,
                    observed
                );
                fflush(stderr);
            }
            assert(observed == iteration);
        }
    }
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_stress_reader(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;
    unsigned long long previous_total = 0U;
    const unsigned stress_iterations = ownerless_stress_iterations();
    const unsigned stress_reader_polls = ownerless_stress_reader_polls();

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    for (unsigned iteration = 0U; iteration < stress_reader_polls; ++iteration) {
        const unsigned long long value = query_unsigned(
            db,
            "SELECT "
            "(SELECT value FROM app.ownerless_stress_1 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_2 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_3 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_4 WHERE id = 1)"
        );
        assert(value >= previous_total);
        assert(value <= MYLITE_TEST_STRESS_WRITER_COUNT * stress_iterations);
        previous_total = value;
        sleep_microseconds(1000U);
    }
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_ddl_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
) {
    mylite_db *db;
    char table_name[64];
    char renamed_name[72];
    char sql[512];
    const unsigned ddl_rounds = ownerless_ddl_stress_rounds();

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned round = 0U; round < ddl_rounds; ++round) {
        const unsigned note = (worker_id * 100U) + round;

        assert(
            snprintf(
                table_name,
                sizeof(table_name),
                "ownerless_ddl_stress_%u_%u",
                worker_id,
                round
            ) > 0
        );
        assert(
            snprintf(
                renamed_name,
                sizeof(renamed_name),
                "ownerless_ddl_stress_%u_%u_renamed",
                worker_id,
                round
            ) > 0
        );
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "CREATE TABLE app.%s ("
                "id INT NOT NULL PRIMARY KEY, "
                "value INT NOT NULL"
                ") ENGINE=InnoDB",
                table_name
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.%s VALUES "
                "(1, %u), (2, %u), (3, %u)",
                table_name,
                (worker_id * 1000U) + (round * 10U) + 1U,
                (worker_id * 1000U) + (round * 10U) + 2U,
                (worker_id * 1000U) + (round * 10U) + 3U
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "ALTER TABLE app.%s ADD COLUMN note INT NOT NULL DEFAULT %u",
                table_name,
                note
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "ALTER TABLE app.%s ADD INDEX value_idx (value), ALGORITHM=INPLACE, LOCK=NONE",
                table_name
            ) > 0
        );
        exec_ok(db, sql);
        assert(
            snprintf(sql, sizeof(sql), "RENAME TABLE app.%s TO app.%s", table_name, renamed_name) >
            0
        );
        exec_ok(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "SELECT COUNT(*) FROM app.%s WHERE note = %u",
                renamed_name,
                note
            ) > 0
        );
        assert(query_unsigned(db, sql) == 3U);
        assert(snprintf(sql, sizeof(sql), "TRUNCATE TABLE app.%s", renamed_name) > 0);
        exec_ok(db, sql);
        assert(snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM app.%s", renamed_name) > 0);
        assert(query_unsigned(db, sql) == 0U);
        assert(snprintf(sql, sizeof(sql), "DROP TABLE app.%s", renamed_name) > 0);
        exec_ok(db, sql);
    }

    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_ddl_stress_dml_worker(
    open_database_paths paths,
    unsigned row_id,
    child_pipes pipes
) {
    mylite_db *db;
    char update_sql[128];
    char select_sql[128];
    const unsigned ddl_rounds = ownerless_ddl_stress_rounds();
    const unsigned iterations = ddl_rounds * MYLITE_TEST_DDL_STRESS_DML_UPDATES_PER_ROUND;
    const unsigned long long initial_value = row_id == 1U ? 10U : 20U;

    assert(
        snprintf(
            update_sql,
            sizeof(update_sql),
            "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = %u",
            row_id
        ) > 0
    );
    assert(
        snprintf(
            select_sql,
            sizeof(select_sql),
            "SELECT value FROM app.ownerless_sql WHERE id = %u",
            row_id
        ) > 0
    );

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned iteration = 0U; iteration < iterations; ++iteration) {
        exec_ok(db, update_sql);
        if (iteration % 4U == 0U || iteration + 1U == iterations) {
            const unsigned long long expected = initial_value + iteration + 1U;
            const unsigned long long observed = query_unsigned(db, select_sql);
            if (observed != expected) {
                fprintf(
                    stderr,
                    "ownerless DDL stress DML mismatch: pid=%ld row=%u iteration=%u "
                    "expected=%llu observed=%llu\n",
                    (long)getpid(),
                    row_id,
                    iteration,
                    expected,
                    observed
                );
                fflush(stderr);
            }
            assert(observed == expected);
        }
    }

    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_ddl_stress_reader(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;
    unsigned long long previous_total = 30U;
    const unsigned ddl_rounds = ownerless_ddl_stress_rounds();
    const unsigned dml_iterations = ddl_rounds * MYLITE_TEST_DDL_STRESS_DML_UPDATES_PER_ROUND;
    const unsigned long long max_total =
        30U + (MYLITE_TEST_DDL_STRESS_DML_WORKER_COUNT * dml_iterations);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned poll = 0U; poll < ddl_rounds * 16U; ++poll) {
        const unsigned long long total =
            query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql");
        assert(total >= previous_total);
        assert(total <= max_total);
        previous_total = total;
        sleep_microseconds(1000U);
    }

    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_temp_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
) {
    mylite_db *db;
    char insert_sql[128];
    const unsigned temp_rounds = ownerless_temp_stress_rounds();

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned round = 0U; round < temp_rounds; ++round) {
        const unsigned value = (worker_id * 100000U) + round;

        exec_ok(
            db,
            "CREATE TEMPORARY TABLE app.ownerless_temp_stress ("
            "id INT NOT NULL PRIMARY KEY, "
            "value INT NOT NULL"
            ") ENGINE=InnoDB"
        );
        assert(
            snprintf(
                insert_sql,
                sizeof(insert_sql),
                "INSERT INTO app.ownerless_temp_stress VALUES (1, %u)",
                value
            ) > 0
        );
        exec_ok(db, insert_sql);
        assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_temp_stress") == value);
        exec_ok(db, "UPDATE app.ownerless_temp_stress SET value = value + 1 WHERE id = 1");
        assert(
            query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_temp_stress") == value + 1U
        );
        exec_ok(db, "DROP TEMPORARY TABLE app.ownerless_temp_stress");
        if (round % 3U == 0U) {
            assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_sql") == 2U);
        }
    }

    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_tx_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
) {
    mylite_db *db;
    char sql[256];
    const unsigned rounds = ownerless_tx_stress_rounds();

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned round = 1U; round <= rounds; ++round) {
        const unsigned long long delta = ownerless_tx_stress_delta(worker_id, round);

        exec_ok(db, "START TRANSACTION");
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "UPDATE app.ownerless_tx_stress_%u "
                "SET value = value + %llu, version = version + 1 "
                "WHERE id = 1",
                worker_id,
                delta
            ) > 0
        );
        exec_ok(db, sql);
        exec_ok(db, "SAVEPOINT ownerless_tx_stress_sp");
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "UPDATE app.ownerless_tx_stress_%u "
                "SET value = value + %llu, version = version + 1 "
                "WHERE id = 2",
                worker_id,
                delta * 101ULL
            ) > 0
        );
        exec_ok(db, sql);
        exec_ok(db, "ROLLBACK TO SAVEPOINT ownerless_tx_stress_sp");
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "SELECT value FROM app.ownerless_tx_stress_%u WHERE id = 2",
                worker_id
            ) > 0
        );
        {
            const unsigned long long observed_after_rollback = query_unsigned(db, sql);
            if (observed_after_rollback != 0U) {
                fprintf(
                    stderr,
                    "ownerless tx stress rollback mismatch after rollback: "
                    "worker=%u round=%u value=%llu\n",
                    worker_id,
                    round,
                    observed_after_rollback
                );
                fflush(stderr);
            }
            assert(observed_after_rollback == 0U);
        }
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "UPDATE app.ownerless_tx_stress_%u "
                "SET value = value + %llu, version = version + 1 "
                "WHERE id = 3",
                worker_id,
                delta * 2ULL
            ) > 0
        );
        exec_ok(db, sql);
        exec_ok(db, "RELEASE SAVEPOINT ownerless_tx_stress_sp");
        if (round % 5U == 0U || round == rounds) {
            unsigned long long observed_rolled_back_value;

            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "SELECT value FROM app.ownerless_tx_stress_%u WHERE id = 2",
                    worker_id
                ) > 0
            );
            observed_rolled_back_value = query_unsigned(db, sql);
            if (observed_rolled_back_value != 0U) {
                fprintf(
                    stderr,
                    "ownerless tx stress rollback mismatch before commit: "
                    "worker=%u round=%u value=%llu\n",
                    worker_id,
                    round,
                    observed_rolled_back_value
                );
                fflush(stderr);
            }
            assert(observed_rolled_back_value == 0U);
        }
        exec_ok(db, "COMMIT");
    }

    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_checksum_stress_writer(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
) {
    mylite_db *db;
    char sql[256];
    const unsigned rounds = ownerless_checksum_stress_rounds();

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned round = 1U; round <= rounds; ++round) {
        const unsigned row_id = ownerless_checksum_stress_row_id(worker_id, round);
        const unsigned long long delta = ownerless_checksum_stress_delta(worker_id, round);

        assert(
            snprintf(
                sql,
                sizeof(sql),
                "UPDATE app.ownerless_checksum_stress "
                "SET value = value + %llu, version = version + 1 "
                "WHERE id = %u",
                delta,
                row_id
            ) > 0
        );
        exec_ok(db, sql);

        if (round % 17U == 0U || round == rounds) {
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "SELECT COUNT(*) FROM app.ownerless_checksum_stress "
                    "WHERE worker = %u AND version > 0",
                    worker_id
                ) > 0
            );
            assert(query_unsigned(db, sql) >= 1U);
        }
    }

    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_prepared_checksum_stress_writer(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
) {
    mylite_db *db;
    mylite_stmt *stmt = NULL;
    const char *tail = NULL;
    char sql[256];
    const unsigned rounds = ownerless_checksum_stress_rounds();

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    assert(
        mylite_prepare(
            db,
            "UPDATE app.ownerless_checksum_stress "
            "SET value = value + ?, version = version + 1 "
            "WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            &tail
        ) == MYLITE_OK
    );
    assert(stmt != NULL);
    assert(tail != NULL && *tail == '\0');
    assert(mylite_bind_parameter_count(stmt) == 2U);

    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned round = 1U; round <= rounds; ++round) {
        const unsigned row_id = ownerless_checksum_stress_row_id(worker_id, round);
        const unsigned long long delta = ownerless_checksum_stress_delta(worker_id, round);

        assert(mylite_bind_uint64(stmt, 1, delta) == MYLITE_OK);
        assert(mylite_bind_int64(stmt, 2, (long long)row_id) == MYLITE_OK);
        assert(mylite_step(stmt) == MYLITE_DONE);
        assert(mylite_changes(db) == 1);
        assert(mylite_reset(stmt) == MYLITE_OK);

        if (round % 17U == 0U || round == rounds) {
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "SELECT COUNT(*) FROM app.ownerless_checksum_stress "
                    "WHERE worker = %u AND version > 0",
                    worker_id
                ) > 0
            );
            assert(query_unsigned(db, sql) >= 1U);
        }
    }

    assert(mylite_finalize(stmt) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_checksum_stress_reader(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;
    unsigned long long previous_sum = 0U;
    const unsigned rounds = ownerless_checksum_stress_rounds();
    unsigned long long expected_sum = 0U;
    unsigned long long expected_versions = 0U;
    unsigned long long expected_weighted_sum = 0U;

    ownerless_checksum_stress_expected(
        rounds,
        &expected_sum,
        &expected_versions,
        &expected_weighted_sum
    );

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned iteration = 0U; iteration < rounds * MYLITE_TEST_CHECKSUM_STRESS_WORKER_COUNT;
         ++iteration) {
        const unsigned long long sum =
            query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_checksum_stress");
        const unsigned long long versions =
            query_unsigned(db, "SELECT SUM(version) FROM app.ownerless_checksum_stress");

        assert(sum >= previous_sum);
        assert(sum <= expected_sum);
        assert(versions <= expected_versions);
        previous_sum = sum;
        sleep_microseconds(1000U);
    }

    (void)expected_weighted_sum;
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_random_tx_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
) {
    mylite_db *db;
    char sql[256];
    const unsigned rounds = ownerless_random_tx_stress_rounds();

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 1");
    exec_ok(db, "SET SESSION lock_wait_timeout = 1");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned round = 1U; round <= rounds; ++round) {
        int round_finished = 0;

        for (unsigned attempt = 1U; attempt <= MYLITE_TEST_RANDOM_TX_STRESS_MAX_ATTEMPTS;
             ++attempt) {
            unsigned rows[3];
            const int rollback_transaction =
                ownerless_random_tx_stress_rolls_back_transaction(worker_id, round);
            const int rollback_savepoint =
                ownerless_random_tx_stress_rolls_back_savepoint(worker_id, round);

            ownerless_random_tx_stress_rows(worker_id, round, rows);
            exec_ok(db, "START TRANSACTION");
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "UPDATE app.ownerless_random_tx_stress "
                    "SET value = value + %llu, version = version + 1 "
                    "WHERE id = %u",
                    ownerless_random_tx_stress_delta(worker_id, round, 0U),
                    rows[0]
                ) > 0
            );
            if (!ownerless_random_tx_stress_exec_retryable(db, sql, worker_id, round, attempt)) {
                exec_ok(db, "ROLLBACK");
                ownerless_random_tx_stress_retry_pause(worker_id, round, attempt);
                continue;
            }
            exec_ok(db, "SAVEPOINT ownerless_random_tx_sp");
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "UPDATE app.ownerless_random_tx_stress "
                    "SET value = value + %llu, version = version + 1 "
                    "WHERE id = %u",
                    ownerless_random_tx_stress_delta(worker_id, round, 1U),
                    rows[1]
                ) > 0
            );
            if (!ownerless_random_tx_stress_exec_retryable(db, sql, worker_id, round, attempt)) {
                exec_ok(db, "ROLLBACK");
                ownerless_random_tx_stress_retry_pause(worker_id, round, attempt);
                continue;
            }
            if (rollback_savepoint) {
                exec_ok(db, "ROLLBACK TO SAVEPOINT ownerless_random_tx_sp");
            }
            exec_ok(db, "RELEASE SAVEPOINT ownerless_random_tx_sp");
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "UPDATE app.ownerless_random_tx_stress "
                    "SET value = value + %llu, version = version + 1 "
                    "WHERE id = %u",
                    ownerless_random_tx_stress_delta(worker_id, round, 2U),
                    rows[2]
                ) > 0
            );
            if (!ownerless_random_tx_stress_exec_retryable(db, sql, worker_id, round, attempt)) {
                exec_ok(db, "ROLLBACK");
                ownerless_random_tx_stress_retry_pause(worker_id, round, attempt);
                continue;
            }
            if (round % 7U == 0U || round == rounds) {
                assert(
                    query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_random_tx_stress") ==
                    MYLITE_TEST_RANDOM_TX_STRESS_ROW_COUNT
                );
            }
            exec_ok(db, rollback_transaction ? "ROLLBACK" : "COMMIT");
            round_finished = 1;
            break;
        }

        if (!round_finished) {
            fprintf(
                stderr,
                "ownerless random tx stress exhausted retries: worker=%u round=%u\n",
                worker_id,
                round
            );
            fflush(stderr);
        }
        assert(round_finished);
    }

    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_random_tx_stress_reader(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;
    unsigned long long previous_sum = 0U;
    const unsigned rounds = ownerless_random_tx_stress_rounds();
    unsigned long long expected_sum = 0U;
    unsigned long long expected_versions = 0U;
    unsigned long long expected_weighted_sum = 0U;

    ownerless_random_tx_stress_expected(
        rounds,
        &expected_sum,
        &expected_versions,
        &expected_weighted_sum
    );

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned iteration = 0U; iteration < rounds * MYLITE_TEST_RANDOM_TX_STRESS_WORKER_COUNT;
         ++iteration) {
        const unsigned long long sum =
            query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_random_tx_stress");
        const unsigned long long versions =
            query_unsigned(db, "SELECT SUM(version) FROM app.ownerless_random_tx_stress");
        const unsigned long long weighted_sum =
            query_unsigned(db, "SELECT SUM(id * value) FROM app.ownerless_random_tx_stress");

        assert(sum >= previous_sum);
        assert(sum <= expected_sum);
        assert(versions <= expected_versions);
        assert(weighted_sum <= expected_weighted_sum);
        previous_sum = sum;
        sleep_microseconds(1000U);
    }

    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_fk_graph_stress_worker(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
) {
    mylite_db *db;
    char sql[512];
    const unsigned rounds = ownerless_fk_graph_stress_rounds();
    unsigned cascade_root = ownerless_fk_graph_stress_initial_id(worker_id, 1U);
    unsigned setnull_root = ownerless_fk_graph_stress_initial_id(worker_id, 2U);
    const unsigned restrict_root = ownerless_fk_graph_stress_initial_id(worker_id, 3U);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);

    for (unsigned round = 1U; round <= rounds; ++round) {
        const unsigned long long delta = ownerless_fk_graph_stress_delta(worker_id, round);
        int round_finished = 0;

        for (unsigned attempt = 1U; attempt <= MYLITE_TEST_FK_GRAPH_STRESS_MAX_ATTEMPTS;
             ++attempt) {
            const unsigned next_cascade_root = cascade_root + 1U;
            const unsigned next_setnull_root = setnull_root + 1U;

            exec_ok(db, "START TRANSACTION");
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "UPDATE app.ownerless_fk_graph_root "
                    "SET id = %u, value = value + %llu, version = version + 1 "
                    "WHERE id = %u",
                    next_cascade_root,
                    delta,
                    cascade_root
                ) > 0
            );
            if (!ownerless_fk_graph_stress_exec_retryable(db, sql, worker_id, round, attempt)) {
                exec_ok(db, "ROLLBACK");
                ownerless_fk_graph_stress_retry_pause(worker_id, round, attempt);
                continue;
            }
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "UPDATE app.ownerless_fk_graph_root "
                    "SET id = %u, value = value + %llu, version = version + 1 "
                    "WHERE id = %u",
                    next_setnull_root,
                    delta,
                    setnull_root
                ) > 0
            );
            if (!ownerless_fk_graph_stress_exec_retryable(db, sql, worker_id, round, attempt)) {
                exec_ok(db, "ROLLBACK");
                ownerless_fk_graph_stress_retry_pause(worker_id, round, attempt);
                continue;
            }
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "UPDATE app.ownerless_fk_graph_root "
                    "SET value = value + %llu, version = version + 1 "
                    "WHERE id = %u",
                    delta,
                    restrict_root
                ) > 0
            );
            if (!ownerless_fk_graph_stress_exec_retryable(db, sql, worker_id, round, attempt)) {
                exec_ok(db, "ROLLBACK");
                ownerless_fk_graph_stress_retry_pause(worker_id, round, attempt);
                continue;
            }
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "UPDATE app.ownerless_fk_graph_cascade_child "
                    "SET value = value + %llu, version = version + 1 "
                    "WHERE worker_id = %u",
                    delta,
                    worker_id
                ) > 0
            );
            if (!ownerless_fk_graph_stress_exec_retryable(db, sql, worker_id, round, attempt)) {
                exec_ok(db, "ROLLBACK");
                ownerless_fk_graph_stress_retry_pause(worker_id, round, attempt);
                continue;
            }
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "UPDATE app.ownerless_fk_graph_setnull_child "
                    "SET value = value + %llu, version = version + 1 "
                    "WHERE worker_id = %u",
                    delta,
                    worker_id
                ) > 0
            );
            if (!ownerless_fk_graph_stress_exec_retryable(db, sql, worker_id, round, attempt)) {
                exec_ok(db, "ROLLBACK");
                ownerless_fk_graph_stress_retry_pause(worker_id, round, attempt);
                continue;
            }
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "UPDATE app.ownerless_fk_graph_restrict_child "
                    "SET value = value + %llu, version = version + 1 "
                    "WHERE worker_id = %u",
                    delta,
                    worker_id
                ) > 0
            );
            if (!ownerless_fk_graph_stress_exec_retryable(db, sql, worker_id, round, attempt)) {
                exec_ok(db, "ROLLBACK");
                ownerless_fk_graph_stress_retry_pause(worker_id, round, attempt);
                continue;
            }
            exec_ok(db, "COMMIT");
            cascade_root = next_cascade_root;
            setnull_root = next_setnull_root;
            round_finished = 1;
            break;
        }

        if (!round_finished) {
            fprintf(
                stderr,
                "ownerless fk graph stress exhausted retries: worker=%u round=%u\n",
                worker_id,
                round
            );
            fflush(stderr);
        }
        assert(round_finished);

        if (round % 4U == 0U || round == rounds) {
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "DELETE FROM app.ownerless_fk_graph_root WHERE id = %u",
                    restrict_root
                ) > 0
            );
            ownerless_fk_graph_stress_expect_mariadb_error(
                db,
                sql,
                MYLITE_TEST_ROW_IS_REFERENCED_ERRNO,
                worker_id,
                round
            );
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "INSERT INTO app.ownerless_fk_graph_cascade_child "
                    "VALUES (%u, %u, %u, 0, 0)",
                    ownerless_fk_graph_stress_initial_id(worker_id, 1U) + 70000U + round,
                    worker_id,
                    ownerless_fk_graph_stress_initial_id(worker_id, 1U) + 90000U + round
                ) > 0
            );
            ownerless_fk_graph_stress_expect_mariadb_error(
                db,
                sql,
                MYLITE_TEST_NO_REFERENCED_ROW_ERRNO,
                worker_id,
                round
            );
        }
    }

    for (unsigned attempt = 1U; attempt <= MYLITE_TEST_FK_GRAPH_STRESS_MAX_ATTEMPTS; ++attempt) {
        exec_ok(db, "START TRANSACTION");
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "DELETE FROM app.ownerless_fk_graph_root WHERE id = %u",
                setnull_root
            ) > 0
        );
        if (!ownerless_fk_graph_stress_exec_retryable(db, sql, worker_id, rounds, attempt)) {
            exec_ok(db, "ROLLBACK");
            ownerless_fk_graph_stress_retry_pause(worker_id, rounds, attempt);
            continue;
        }
        exec_ok(db, "COMMIT");
        setnull_root = 0U;
        break;
    }
    if (setnull_root != 0U) {
        fprintf(
            stderr,
            "ownerless fk graph stress exhausted set-null delete retries: worker=%u\n",
            worker_id
        );
        fflush(stderr);
    }
    assert(setnull_root == 0U);

    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static unsigned ownerless_stress_iterations(void) {
    return ownerless_unsigned_env(
        "MYLITE_OWNERLESS_STRESS_ITERATIONS",
        MYLITE_TEST_STRESS_ITERATIONS,
        MYLITE_TEST_STRESS_ITERATIONS_MAX
    );
}

static unsigned ownerless_stress_reader_polls(void) {
    return ownerless_unsigned_env(
        "MYLITE_OWNERLESS_STRESS_READER_POLLS",
        MYLITE_TEST_STRESS_READER_POLLS,
        MYLITE_TEST_STRESS_READER_POLLS_MAX
    );
}

static unsigned ownerless_ddl_stress_rounds(void) {
    return ownerless_unsigned_env(
        "MYLITE_OWNERLESS_DDL_STRESS_ROUNDS",
        MYLITE_TEST_DDL_STRESS_ROUNDS,
        MYLITE_TEST_DDL_STRESS_ROUNDS_MAX
    );
}

static unsigned ownerless_temp_stress_rounds(void) {
    return ownerless_unsigned_env(
        "MYLITE_OWNERLESS_TEMP_STRESS_ROUNDS",
        MYLITE_TEST_TEMP_STRESS_ROUNDS,
        MYLITE_TEST_TEMP_STRESS_ROUNDS_MAX
    );
}

static unsigned ownerless_tx_stress_rounds(void) {
    return ownerless_unsigned_env(
        "MYLITE_OWNERLESS_TX_STRESS_ROUNDS",
        MYLITE_TEST_TX_STRESS_ROUNDS,
        MYLITE_TEST_TX_STRESS_ROUNDS_MAX
    );
}

static unsigned ownerless_checksum_stress_rounds(void) {
    return ownerless_unsigned_env(
        "MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS",
        MYLITE_TEST_CHECKSUM_STRESS_ROUNDS,
        MYLITE_TEST_CHECKSUM_STRESS_ROUNDS_MAX
    );
}

static unsigned ownerless_random_tx_stress_rounds(void) {
    return ownerless_unsigned_env(
        "MYLITE_OWNERLESS_RANDOM_TX_STRESS_ROUNDS",
        MYLITE_TEST_RANDOM_TX_STRESS_ROUNDS,
        MYLITE_TEST_RANDOM_TX_STRESS_ROUNDS_MAX
    );
}

static unsigned ownerless_fk_graph_stress_rounds(void) {
    return ownerless_unsigned_env(
        "MYLITE_OWNERLESS_FK_GRAPH_STRESS_ROUNDS",
        MYLITE_TEST_FK_GRAPH_STRESS_ROUNDS,
        MYLITE_TEST_FK_GRAPH_STRESS_ROUNDS_MAX
    );
}

static unsigned long long ownerless_tx_stress_delta(unsigned worker_id, unsigned round) {
    return (worker_id * 10000ULL) + (round * 17ULL);
}

static unsigned long long ownerless_tx_stress_delta_sum(unsigned worker_id, unsigned rounds) {
    unsigned long long sum = 0U;

    for (unsigned round = 1U; round <= rounds; ++round) {
        sum += ownerless_tx_stress_delta(worker_id, round);
    }
    return sum;
}

static unsigned ownerless_checksum_stress_row_id(unsigned worker_id, unsigned round) {
    const unsigned slot =
        ((round * 5U) + (worker_id * 3U)) % MYLITE_TEST_CHECKSUM_STRESS_ROWS_PER_WORKER;
    return (worker_id * 1000U) + slot + 1U;
}

static unsigned long long ownerless_checksum_stress_delta(unsigned worker_id, unsigned round) {
    return (worker_id * 100000ULL) + (round * 13ULL);
}

static void ownerless_random_tx_stress_rows(unsigned worker_id, unsigned round, unsigned rows[3]) {
    const unsigned rows_per_worker =
        MYLITE_TEST_RANDOM_TX_STRESS_ROW_COUNT / MYLITE_TEST_RANDOM_TX_STRESS_WORKER_COUNT;
    const unsigned worker_base = (worker_id - 1U) * rows_per_worker;
    const unsigned base = ((worker_id * 7U) + (round * 5U)) % rows_per_worker;

    assert(rows != NULL);
    assert(worker_id >= 1U && worker_id <= MYLITE_TEST_RANDOM_TX_STRESS_WORKER_COUNT);
    assert(rows_per_worker >= 4U);
    assert(
        rows_per_worker * MYLITE_TEST_RANDOM_TX_STRESS_WORKER_COUNT ==
        MYLITE_TEST_RANDOM_TX_STRESS_ROW_COUNT
    );
    rows[0] = worker_base + base + 1U;
    rows[1] = worker_base + ((base + 1U) % rows_per_worker) + 1U;
    rows[2] = worker_base + ((base + 3U) % rows_per_worker) + 1U;
    for (unsigned outer = 0U; outer < 3U; ++outer) {
        for (unsigned inner = outer + 1U; inner < 3U; ++inner) {
            if (rows[inner] < rows[outer]) {
                const unsigned swap = rows[outer];
                rows[outer] = rows[inner];
                rows[inner] = swap;
            }
        }
    }
}

static unsigned long long ownerless_random_tx_stress_delta(
    unsigned worker_id,
    unsigned round,
    unsigned phase
) {
    return (worker_id * 100000ULL) + (round * 100ULL) + ((phase + 1U) * 11ULL);
}

static int ownerless_random_tx_stress_exec_retryable(
    mylite_db *db,
    const char *sql,
    unsigned worker_id,
    unsigned round,
    unsigned attempt
) {
    unsigned mariadb_errno = 0U;
    const int result = exec_status(db, sql, &mariadb_errno);

    if (result == MYLITE_OK) {
        return 1;
    }
    if (mariadb_errno == MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO ||
        mariadb_errno == MYLITE_TEST_DEADLOCK_ERRNO) {
        return 0;
    }

    fprintf(
        stderr,
        "ownerless random tx stress unexpected error: worker=%u round=%u attempt=%u "
        "sql=%s errcode=%d mariadb_errno=%u\n",
        worker_id,
        round,
        attempt,
        sql,
        mylite_errcode(db),
        mariadb_errno
    );
    fflush(stderr);
    assert(0);
    return 0;
}

static void ownerless_random_tx_stress_retry_pause(
    unsigned worker_id,
    unsigned round,
    unsigned attempt
) {
    const unsigned delay = 1000U * (1U + ((worker_id * 17U + round * 13U + attempt * 7U) % 20U));

    sleep_microseconds(delay);
}

static int ownerless_random_tx_stress_rolls_back_transaction(unsigned worker_id, unsigned round) {
    return ((worker_id * 11U) + round) % 9U == 0U;
}

static int ownerless_random_tx_stress_rolls_back_savepoint(unsigned worker_id, unsigned round) {
    return ((worker_id * 3U) + round) % 4U == 0U;
}

static void ownerless_checksum_stress_expected(
    unsigned rounds,
    unsigned long long *out_sum,
    unsigned long long *out_versions,
    unsigned long long *out_weighted_sum
) {
    unsigned long long expected_sum = 0U;
    unsigned long long expected_versions = 0U;
    unsigned long long expected_weighted_sum = 0U;

    assert(out_sum != NULL);
    assert(out_versions != NULL);
    assert(out_weighted_sum != NULL);

    for (unsigned worker_id = 1U; worker_id <= MYLITE_TEST_CHECKSUM_STRESS_WORKER_COUNT;
         ++worker_id) {
        for (unsigned round = 1U; round <= rounds; ++round) {
            const unsigned row_id = ownerless_checksum_stress_row_id(worker_id, round);
            const unsigned long long delta = ownerless_checksum_stress_delta(worker_id, round);

            expected_sum += delta;
            ++expected_versions;
            expected_weighted_sum += ((unsigned long long)row_id) * delta;
        }
    }

    *out_sum = expected_sum;
    *out_versions = expected_versions;
    *out_weighted_sum = expected_weighted_sum;
}

static void ownerless_random_tx_stress_expected(
    unsigned rounds,
    unsigned long long *out_sum,
    unsigned long long *out_versions,
    unsigned long long *out_weighted_sum
) {
    unsigned long long expected_sum = 0U;
    unsigned long long expected_versions = 0U;
    unsigned long long expected_weighted_sum = 0U;

    assert(out_sum != NULL);
    assert(out_versions != NULL);
    assert(out_weighted_sum != NULL);

    for (unsigned worker_id = 1U; worker_id <= MYLITE_TEST_RANDOM_TX_STRESS_WORKER_COUNT;
         ++worker_id) {
        for (unsigned round = 1U; round <= rounds; ++round) {
            unsigned rows[3];
            const int rollback_transaction =
                ownerless_random_tx_stress_rolls_back_transaction(worker_id, round);
            const int rollback_savepoint =
                ownerless_random_tx_stress_rolls_back_savepoint(worker_id, round);

            if (rollback_transaction) {
                continue;
            }
            ownerless_random_tx_stress_rows(worker_id, round, rows);
            for (unsigned phase = 0U; phase < 3U; ++phase) {
                const unsigned long long delta =
                    ownerless_random_tx_stress_delta(worker_id, round, phase);

                if (phase == 1U && rollback_savepoint) {
                    continue;
                }
                expected_sum += delta;
                ++expected_versions;
                expected_weighted_sum += ((unsigned long long)rows[phase]) * delta;
            }
        }
    }

    *out_sum = expected_sum;
    *out_versions = expected_versions;
    *out_weighted_sum = expected_weighted_sum;
}

static unsigned ownerless_fk_graph_stress_initial_id(unsigned worker_id, unsigned kind) {
    assert(worker_id >= 1U && worker_id <= MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT);
    assert(kind >= 1U && kind <= 3U);
    return (worker_id * 100000U) + (kind * 10000U);
}

static unsigned long long ownerless_fk_graph_stress_delta(unsigned worker_id, unsigned round) {
    assert(worker_id >= 1U && worker_id <= MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT);
    assert(round >= 1U);
    return (worker_id * 1000ULL) + round;
}

static unsigned long long ownerless_fk_graph_stress_delta_sum(unsigned worker_id, unsigned rounds) {
    unsigned long long sum = 0U;

    for (unsigned round = 1U; round <= rounds; ++round) {
        sum += ownerless_fk_graph_stress_delta(worker_id, round);
    }
    return sum;
}

static unsigned long long ownerless_fk_graph_stress_total_delta_sum(unsigned rounds) {
    unsigned long long sum = 0U;

    for (unsigned worker_id = 1U; worker_id <= MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT;
         ++worker_id) {
        sum += ownerless_fk_graph_stress_delta_sum(worker_id, rounds);
    }
    return sum;
}

static int ownerless_fk_graph_stress_exec_retryable(
    mylite_db *db,
    const char *sql,
    unsigned worker_id,
    unsigned round,
    unsigned attempt
) {
    unsigned mariadb_errno = 0U;
    const int result = exec_status(db, sql, &mariadb_errno);

    if (result == MYLITE_OK) {
        return 1;
    }
    if (mariadb_errno == MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO ||
        mariadb_errno == MYLITE_TEST_DEADLOCK_ERRNO) {
        return 0;
    }

    fprintf(
        stderr,
        "ownerless fk graph stress unexpected error: worker=%u round=%u attempt=%u "
        "sql=%s errcode=%d mariadb_errno=%u\n",
        worker_id,
        round,
        attempt,
        sql,
        mylite_errcode(db),
        mariadb_errno
    );
    fflush(stderr);
    assert(0);
    return 0;
}

static void ownerless_fk_graph_stress_expect_mariadb_error(
    mylite_db *db,
    const char *sql,
    unsigned expected_errno,
    unsigned worker_id,
    unsigned round
) {
    for (unsigned attempt = 1U; attempt <= MYLITE_TEST_FK_GRAPH_STRESS_MAX_ATTEMPTS; ++attempt) {
        unsigned mariadb_errno = 0U;
        const int result = exec_status(db, "START TRANSACTION", NULL);
        int finished = 0;

        assert(result == MYLITE_OK);
        if (exec_status(db, sql, &mariadb_errno) != MYLITE_OK && mariadb_errno == expected_errno) {
            finished = 1;
        }
        exec_ok(db, "ROLLBACK");
        if (finished) {
            return;
        }
        if (mariadb_errno == MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO ||
            mariadb_errno == MYLITE_TEST_DEADLOCK_ERRNO) {
            ownerless_fk_graph_stress_retry_pause(worker_id, round, attempt);
            continue;
        }
        fprintf(
            stderr,
            "ownerless fk graph stress expected MariaDB error %u: "
            "worker=%u round=%u attempt=%u sql=%s errcode=%d mariadb_errno=%u\n",
            expected_errno,
            worker_id,
            round,
            attempt,
            sql,
            mylite_errcode(db),
            mariadb_errno
        );
        fflush(stderr);
        assert(0);
    }

    fprintf(
        stderr,
        "ownerless fk graph stress exhausted expected-error retries: "
        "worker=%u round=%u errno=%u sql=%s\n",
        worker_id,
        round,
        expected_errno,
        sql
    );
    fflush(stderr);
    assert(0);
}

static void ownerless_fk_graph_stress_retry_pause(
    unsigned worker_id,
    unsigned round,
    unsigned attempt
) {
    const unsigned delay = 1000U * (1U + ((worker_id * 19U + round * 11U + attempt * 5U) % 20U));

    sleep_microseconds(delay);
}

static unsigned ownerless_unsigned_env(
    const char *name,
    unsigned default_value,
    unsigned max_value
) {
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0U || parsed > max_value) {
        fprintf(stderr, "invalid %s=%s; expected 1..%u\n", name, value, max_value);
        fflush(stderr);
        assert(0);
    }
    return (unsigned)parsed;
}

static void hold_repeatable_read_snapshot_until_released(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(db, "START TRANSACTION WITH CONSISTENT SNAPSHOT");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void hold_expanding_page_snapshot_until_released(
    open_database_paths paths,
    child_pipes pipes
) {
    const unsigned rows =
        ownerless_unsigned_env("MYLITE_OWNERLESS_EXPANDING_PAGE_PRESSURE_ROWS", 12U, 128U);
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(db, "START TRANSACTION WITH CONSISTENT SNAPSHOT");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_expanding_pressure") == rows);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_expanding_pressure") == rows);
    assert(
        query_unsigned(db, "SELECT SUM(LENGTH(payload)) FROM app.ownerless_expanding_pressure") ==
        rows * 4000ULL
    );
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_expanding_pressure") == rows);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_expanding_pressure") == rows);
    assert(
        query_unsigned(db, "SELECT SUM(LENGTH(payload)) FROM app.ownerless_expanding_pressure") ==
        rows * 4000ULL
    );
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void start_consistent_snapshot_after_pin_fault(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;
    unsigned long long total;
    char ready_fd_value[32];
    char release_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", pipes.ready_write_fd) > 0);
    assert(snprintf(release_fd_value, sizeof(release_fd_value), "%d", pipes.release_read_fd) > 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "consistent-snapshot-after-pin", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_RELEASE_FD", release_fd_value, 1) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION WITH CONSISTENT SNAPSHOT");
    total = query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql");
    assert(total == 31U || total == 36U);
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void hold_reclaim_boundary_snapshot_until_released(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    exec_ok(db, "START TRANSACTION WITH CONSISTENT SNAPSHOT");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 130U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_reclaim_boundary_aux") == 110U);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 130U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_reclaim_boundary_aux") == 110U);
    exec_ok(db, "COMMIT");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}
#endif

static void churn_ownerless_history(open_database_paths paths) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    for (unsigned iteration = 0U; iteration < MYLITE_TEST_PURGE_HISTORY_UPDATES; ++iteration) {
        exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 1 WHERE id = 1");
    }
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void update_first_row_by_seven_after_signal(open_database_paths paths, int start_read_fd) {
    mylite_db *db;

    wait_for_pipe(start_read_fd);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 7 WHERE id = 1");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void hold_select_for_update_until_released(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    assert(
        query_unsigned(db, "SELECT value FROM app.ownerless_sql WHERE id = 1 FOR UPDATE") == 10U
    );
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "ROLLBACK");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void alter_ownerless_sql_expect_lock_timeout(open_database_paths paths) {
    mylite_db *db;
    unsigned mariadb_errno = 0U;
    int result;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION lock_wait_timeout = 1");
    result = exec_status(
        db,
        "ALTER TABLE app.ownerless_sql ADD COLUMN note VARCHAR(32)",
        &mariadb_errno
    );
    if (result != MYLITE_OK && mariadb_errno == MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO) {
        assert(mylite_close(db) == MYLITE_OK);
        _exit(0);
    }

    fprintf(
        stderr,
        "expected ownerless ALTER lock wait timeout, got result=%d errno=%u\n",
        result,
        mariadb_errno
    );
    fflush(stderr);
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXPECTED_ERROR);
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void ownerless_sql_expect_lock_timeout_with_table_wait_fault(
    open_database_paths paths,
    const ownerless_table_wait_negative_case *test_case,
    int ready_fd
) {
    mylite_db *db;
    char ready_fd_value[32];
    unsigned mariadb_errno = 0U;
    int result;

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION lock_wait_timeout = 1");
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "table-lock-wait", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    result = exec_status(db, test_case->sql, &mariadb_errno);
    if (result != MYLITE_OK && mariadb_errno == MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO) {
        assert(mylite_close(db) == MYLITE_OK);
        _exit(MYLITE_TEST_CHILD_OK);
    }

    fprintf(
        stderr,
        "expected ownerless SQL lock wait timeout with table-wait fault armed, "
        "case=%s got result=%d errno=%u\n",
        test_case->name,
        result,
        mariadb_errno
    );
    fflush(stderr);
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXPECTED_ERROR);
}
#endif

static void run_ownerless_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "ALTER TABLE app.ownerless_sql ADD COLUMN note INT NOT NULL DEFAULT 7");
    signal_pipe_message(pipes.ready_write_fd);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_created ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_created VALUES (1, 100)");
    signal_pipe_message(pipes.ready_write_fd);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "RENAME TABLE app.ownerless_created TO app.ownerless_renamed");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_renamed") == 100U);
    signal_pipe_message(pipes.ready_write_fd);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "TRUNCATE TABLE app.ownerless_renamed");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_renamed") == 0U);
    signal_pipe_message(pipes.ready_write_fd);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP TABLE app.ownerless_renamed");
    signal_pipe_message(pipes.ready_write_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void truncate_ownerless_large_table_after_signal(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    exec_ok(db, "TRUNCATE TABLE app.ownerless_truncate_bounds");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void create_ownerless_ddl_tables_after_signal(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
) {
    mylite_db *db;
    char sql[192];

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    signal_pipe_message(pipes.ready_write_fd);
    wait_for_pipe_message(pipes.release_read_fd);

    for (unsigned table_id = 0U; table_id < MYLITE_TEST_DDL_TABLES_PER_WORKER; ++table_id) {
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "CREATE TABLE app.ownerless_ddl_%u_%u ("
                "id INT NOT NULL PRIMARY KEY, "
                "value INT NOT NULL"
                ") ENGINE=InnoDB",
                worker_id,
                table_id
            ) > 0
        );
        exec_ok(db, sql);

        assert(
            snprintf(
                sql,
                sizeof(sql),
                "INSERT INTO app.ownerless_ddl_%u_%u VALUES (1, %u)",
                worker_id,
                table_id,
                (worker_id * 100U) + table_id
            ) > 0
        );
        exec_ok(db, sql);

        assert(
            snprintf(
                sql,
                sizeof(sql),
                "ALTER TABLE app.ownerless_ddl_%u_%u "
                "ADD COLUMN note INT NOT NULL DEFAULT %u",
                worker_id,
                table_id,
                (worker_id * 10U) + table_id
            ) > 0
        );
        exec_ok(db, sql);

        assert(
            snprintf(
                sql,
                sizeof(sql),
                "ALTER TABLE app.ownerless_ddl_%u_%u "
                "ADD INDEX ownerless_ddl_value_idx (value), "
                "ALGORITHM=INPLACE, LOCK=NONE",
                worker_id,
                table_id
            ) > 0
        );
        exec_ok(db, sql);

        assert(
            snprintf(
                sql,
                sizeof(sql),
                "ALTER TABLE app.ownerless_ddl_%u_%u "
                "DROP INDEX ownerless_ddl_value_idx, "
                "ADD INDEX ownerless_ddl_note_idx (note), "
                "ALGORITHM=INPLACE, LOCK=NONE",
                worker_id,
                table_id
            ) > 0
        );
        exec_ok(db, sql);
    }

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_broader_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "CONSTRAINT ownerless_fk_child_parent "
        "FOREIGN KEY (parent_id) REFERENCES app.ownerless_fk_parent (id) "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_parent VALUES (1, 10)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_child VALUES (1, 1, 20)");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_generated ("
        "id INT NOT NULL PRIMARY KEY, "
        "first_name VARCHAR(16) NOT NULL, "
        "last_name VARCHAR(16) NOT NULL, "
        "full_name VARCHAR(40) GENERATED ALWAYS AS "
        "(CONCAT(first_name, ' ', last_name)) STORED, "
        "name_length INT GENERATED ALWAYS AS "
        "(CHAR_LENGTH(CONCAT(first_name, ' ', last_name))) VIRTUAL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_generated (id, first_name, last_name) "
        "VALUES (1, 'Ada', 'Lovelace')"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_online ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "status VARCHAR(16) NOT NULL DEFAULT 'ready'"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_online (id, value) VALUES (1, 42)");
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_online "
        "ADD INDEX ownerless_online_status_idx (status), "
        "ALGORITHM=INPLACE, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "ALTER TABLE app.ownerless_online ADD COLUMN priority INT NOT NULL DEFAULT 7");
    exec_ok(db, "ALTER TABLE app.ownerless_online ADD COLUMN scratch VARCHAR(8) NULL");
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_online "
        "MODIFY COLUMN status VARCHAR(24) NOT NULL DEFAULT 'ready'"
    );
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_online "
        "CHANGE COLUMN status state VARCHAR(24) NOT NULL DEFAULT 'ready'"
    );
    exec_ok(db, "ALTER TABLE app.ownerless_online DROP COLUMN scratch");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "CREATE TABLE app.ownerless_like LIKE app.ownerless_online");
    exec_ok(db, "INSERT INTO app.ownerless_like SELECT * FROM app.ownerless_online");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_ctas ENGINE=InnoDB AS "
        "SELECT id, value, state, priority FROM app.ownerless_like"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_instant ("
        "id INT NOT NULL PRIMARY KEY, "
        "old_value INT NOT NULL, "
        "payload VARCHAR(32) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_instant VALUES (1, 10, 'base')");
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_instant "
        "ADD COLUMN instant_value INT NOT NULL DEFAULT 7, "
        "ALGORITHM=INSTANT, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_instant "
        "DROP COLUMN old_value, "
        "ALGORITHM=INSTANT, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_instant "
        "MODIFY COLUMN instant_value INT NOT NULL DEFAULT 7 AFTER id, "
        "ALGORITHM=INSTANT, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_online_ddl_options_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_ddl_options ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "status VARCHAR(16) NOT NULL DEFAULT 'ready', "
        "payload VARCHAR(24) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_ddl_options VALUES "
        "(1, 10, 'ready', 'alpha'), "
        "(2, 20, 'ready', 'beta')"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_ddl_options "
        "ADD INDEX ownerless_ddl_options_status_idx (status), "
        "ALGORITHM=NOCOPY, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_ddl_options "
        "ADD INDEX ownerless_ddl_options_value_idx (value), "
        "ALGORITHM=INPLACE, LOCK=SHARED"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_ddl_options "
        "MODIFY COLUMN payload VARCHAR(80) NOT NULL, "
        "ALGORITHM=COPY, LOCK=EXCLUSIVE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_ddl_options "
        "FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_generated_column_alter_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_generated_alter ("
        "id INT NOT NULL PRIMARY KEY, "
        "first_name VARCHAR(16) NOT NULL, "
        "last_name VARCHAR(16) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_generated_alter VALUES (1, 'Ada', 'Lovelace')");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_generated_alter "
        "ADD COLUMN full_name VARCHAR(40) GENERATED ALWAYS AS "
        "(CONCAT(first_name, ' ', last_name)) STORED, "
        "ADD COLUMN name_length INT GENERATED ALWAYS AS "
        "(CHAR_LENGTH(CONCAT(first_name, ' ', last_name))) VIRTUAL"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "ALTER TABLE app.ownerless_generated_alter DROP COLUMN name_length");
    exec_ok(db, "ALTER TABLE app.ownerless_generated_alter DROP COLUMN full_name");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_charset_convert_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_charset_convert_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "name VARCHAR(40) NOT NULL, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB "
        "DEFAULT CHARSET=latin1 COLLATE=latin1_swedish_ci"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_charset_convert_base VALUES "
        "(1, 'alpha', 10), "
        "(2, 'beta', 20)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_charset_convert_base "
        "CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci"
    );
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_row_format_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_row_format_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "note VARCHAR(16) NOT NULL, "
        "payload TEXT NOT NULL"
        ") ENGINE=InnoDB ROW_FORMAT=COMPACT"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_row_format_base VALUES "
        "(1, 10, 'before-a', REPEAT('a', 256)), "
        "(2, 20, 'before-b', REPEAT('b', 256))"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "ALTER TABLE app.ownerless_row_format_base ROW_FORMAT=DYNAMIC");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_table_comment_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_table_comment_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB COMMENT='ownerless initial comment'"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_table_comment_base VALUES "
        "(1, 10), "
        "(2, 20)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_table_comment_base "
        "COMMENT='ownerless updated comment'"
    );
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_force_rebuild_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_force_rebuild_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "note INT NOT NULL, "
        "KEY ownerless_force_rebuild_value_idx (value)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_force_rebuild_base VALUES "
        "(1, 10, 100), "
        "(2, 20, 200), "
        "(3, 30, 300)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "ALTER TABLE app.ownerless_force_rebuild_base FORCE");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_column_default_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_column_default_alter ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL DEFAULT 10, "
        "note VARCHAR(16) NOT NULL DEFAULT 'ready'"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_column_default_alter (id) VALUES (1)");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_column_default_alter "
        "ALTER COLUMN value SET DEFAULT 25"
    );
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_column_default_alter "
        "ALTER COLUMN note SET DEFAULT 'done'"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_column_default_alter "
        "ALTER COLUMN value DROP DEFAULT"
    );
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_instant_column_variant_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_instant_variants ("
        "id INT NOT NULL PRIMARY KEY, "
        "base_value INT NOT NULL, "
        "marker VARCHAR(16) NOT NULL DEFAULT 'base'"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_instant_variants VALUES (1, 10, 'base')");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_instant_variants "
        "ADD COLUMN first_note VARCHAR(16) NOT NULL DEFAULT 'first' FIRST, "
        "ALGORITHM=INSTANT, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_instant_variants "
        "ADD COLUMN side_value INT NOT NULL DEFAULT 5 AFTER base_value, "
        "ALGORITHM=INSTANT, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_instant_variants "
        "RENAME COLUMN marker TO renamed_marker, "
        "ALGORITHM=INSTANT, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_instant_variants "
        "ADD COLUMN value_double INT GENERATED ALWAYS AS (base_value * 2) VIRTUAL, "
        "ALGORITHM=INSTANT, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_instant_variants "
        "DROP COLUMN value_double, "
        "ALGORITHM=INSTANT, LOCK=NONE"
    );
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_schema_lifecycle_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "CREATE DATABASE ownerless_schema");
    exec_ok(
        db,
        "CREATE TABLE ownerless_schema.ownerless_schema_table ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO ownerless_schema.ownerless_schema_table VALUES (1, 10)");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP DATABASE ownerless_schema");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_schema_default_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE DATABASE ownerless_schema_defaults "
        "DEFAULT CHARACTER SET latin1 COLLATE latin1_swedish_ci"
    );
    exec_ok(
        db,
        "CREATE TABLE ownerless_schema_defaults.ownerless_schema_default_before ("
        "id INT NOT NULL PRIMARY KEY, "
        "name VARCHAR(32) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_schema_defaults.ownerless_schema_default_before "
        "VALUES (1, 'latin')"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER DATABASE ownerless_schema_defaults "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE ownerless_schema_defaults.ownerless_schema_default_after ("
        "id INT NOT NULL PRIMARY KEY, "
        "name VARCHAR(32) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_schema_defaults.ownerless_schema_default_after "
        "VALUES (1, 'utf8')"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP DATABASE ownerless_schema_defaults");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_schema_idempotent_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE SCHEMA IF NOT EXISTS ownerless_schema_idempotent "
        "DEFAULT CHARACTER SET latin1 COLLATE latin1_swedish_ci"
    );
    exec_ok(
        db,
        "CREATE TABLE ownerless_schema_idempotent.ownerless_schema_idempotent_table ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "note VARCHAR(32) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO ownerless_schema_idempotent.ownerless_schema_idempotent_table "
        "VALUES (1, 10, 'child-before')"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE DATABASE IF NOT EXISTS ownerless_schema_idempotent "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    exec_ok(db, "DROP SCHEMA IF EXISTS ownerless_schema_idempotent_missing");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP SCHEMA IF EXISTS ownerless_schema_idempotent");
    exec_ok(db, "DROP DATABASE IF EXISTS ownerless_schema_idempotent");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_cross_schema_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "CREATE DATABASE ownerless_rename_schema");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_cross_schema_source ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "note VARCHAR(16) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_cross_schema_source VALUES "
        "(1, 10, 'child'), (2, 20, 'child')"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "RENAME TABLE app.ownerless_cross_schema_source "
        "TO ownerless_rename_schema.ownerless_cross_schema_moved"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_multi_rename_cycle_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_rename_cycle_left ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "note VARCHAR(16) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_rename_cycle_right ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "note VARCHAR(16) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_rename_cycle_left VALUES "
        "(1, 10, 'left'), (2, 20, 'left')"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_rename_cycle_right VALUES "
        "(10, 100, 'right'), (20, 200, 'right')"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "RENAME TABLE "
        "app.ownerless_rename_cycle_left TO app.ownerless_rename_cycle_tmp, "
        "app.ownerless_rename_cycle_right TO app.ownerless_rename_cycle_left, "
        "app.ownerless_rename_cycle_tmp TO app.ownerless_rename_cycle_right"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_view_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_view_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_view_base VALUES (1, 10)");
    exec_ok(
        db,
        "CREATE VIEW app.ownerless_view AS "
        "SELECT id, value FROM app.ownerless_view_base WHERE value >= 10"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP VIEW app.ownerless_view");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_view_ddl_variant_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_view_variant_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_view_variant_base VALUES (1, 10), (2, 20)");
    exec_ok(
        db,
        "CREATE VIEW app.ownerless_view_variant AS "
        "SELECT id, value, value * 2 AS doubled "
        "FROM app.ownerless_view_variant_base "
        "WHERE value >= 10"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE OR REPLACE VIEW app.ownerless_view_variant AS "
        "SELECT id, value, value + 5 AS adjusted "
        "FROM app.ownerless_view_variant_base "
        "WHERE value >= 20"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER VIEW app.ownerless_view_variant AS "
        "SELECT id, value, value - 1 AS adjusted "
        "FROM app.ownerless_view_variant_base "
        "WHERE value >= 30"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP VIEW app.ownerless_view_variant");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_view_check_option_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_view_check_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "note VARCHAR(32) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_view_check_base VALUES "
        "(1, 10, 'child-ten'), (2, 20, 'child-twenty')"
    );
    exec_ok(
        db,
        "CREATE VIEW app.ownerless_view_check AS "
        "SELECT id, value, note FROM app.ownerless_view_check_base "
        "WHERE value >= 10 WITH CASCADED CHECK OPTION"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE OR REPLACE VIEW app.ownerless_view_check AS "
        "SELECT id, value, note FROM app.ownerless_view_check_base "
        "WHERE value >= 20 WITH LOCAL CHECK OPTION"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER VIEW app.ownerless_view_check AS "
        "SELECT id, value, note FROM app.ownerless_view_check_base "
        "WHERE value >= 25 WITH CASCADED CHECK OPTION"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP VIEW app.ownerless_view_check");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_trigger_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_trigger_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_trigger_audit ("
        "base_id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TRIGGER app.ownerless_trigger_ai "
        "AFTER INSERT ON app.ownerless_trigger_base "
        "FOR EACH ROW "
        "INSERT INTO app.ownerless_trigger_audit VALUES (NEW.id, NEW.value)"
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_base VALUES (1, 10)");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP TRIGGER app.ownerless_trigger_ai");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_trigger_ddl_variant_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_trigger_variant_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_trigger_variant_audit ("
        "base_id INT NOT NULL PRIMARY KEY, "
        "old_value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_variant_base VALUES (1, 10)");
    exec_ok(
        db,
        "CREATE TRIGGER app.ownerless_trigger_variant_bu "
        "BEFORE UPDATE ON app.ownerless_trigger_variant_base "
        "FOR EACH ROW "
        "SET NEW.value = NEW.value + 1"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE OR REPLACE TRIGGER app.ownerless_trigger_variant_bu "
        "BEFORE UPDATE ON app.ownerless_trigger_variant_base "
        "FOR EACH ROW "
        "SET NEW.value = NEW.value + 2"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TRIGGER app.ownerless_trigger_variant_ad "
        "AFTER DELETE ON app.ownerless_trigger_variant_base "
        "FOR EACH ROW "
        "INSERT INTO app.ownerless_trigger_variant_audit VALUES (OLD.id, OLD.value)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP TRIGGER app.ownerless_trigger_variant_bu");
    exec_ok(db, "DROP TRIGGER app.ownerless_trigger_variant_ad");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_trigger_ordering_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_trigger_order_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_trigger_order_audit ("
        "audit_id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, "
        "base_id INT NOT NULL, "
        "marker INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TRIGGER app.ownerless_trigger_order_first "
        "AFTER INSERT ON app.ownerless_trigger_order_base "
        "FOR EACH ROW "
        "INSERT INTO app.ownerless_trigger_order_audit (base_id, marker) "
        "VALUES (NEW.id, 1)"
    );
    exec_ok(
        db,
        "CREATE TRIGGER app.ownerless_trigger_order_second "
        "AFTER INSERT ON app.ownerless_trigger_order_base "
        "FOR EACH ROW FOLLOWS ownerless_trigger_order_first "
        "INSERT INTO app.ownerless_trigger_order_audit (base_id, marker) "
        "VALUES (NEW.id, 2)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TRIGGER app.ownerless_trigger_order_third "
        "AFTER INSERT ON app.ownerless_trigger_order_base "
        "FOR EACH ROW PRECEDES ownerless_trigger_order_first "
        "INSERT INTO app.ownerless_trigger_order_audit (base_id, marker) "
        "VALUES (NEW.id, 3)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP TRIGGER app.ownerless_trigger_order_first");
    exec_ok(db, "DROP TRIGGER app.ownerless_trigger_order_second");
    exec_ok(db, "DROP TRIGGER app.ownerless_trigger_order_third");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_trigger_idempotent_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_trigger_idempotent_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_trigger_idempotent_audit ("
        "audit_id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, "
        "base_id INT NOT NULL, "
        "value INT NOT NULL, "
        "marker INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TRIGGER IF NOT EXISTS app.ownerless_trigger_idempotent_ai "
        "AFTER INSERT ON app.ownerless_trigger_idempotent_base "
        "FOR EACH ROW "
        "INSERT INTO app.ownerless_trigger_idempotent_audit (base_id, value, marker) "
        "VALUES (NEW.id, NEW.value, 1)"
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_idempotent_base VALUES (1, 10)");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TRIGGER IF NOT EXISTS app.ownerless_trigger_idempotent_ai "
        "AFTER INSERT ON app.ownerless_trigger_idempotent_base "
        "FOR EACH ROW "
        "INSERT INTO app.ownerless_trigger_idempotent_audit (base_id, value, marker) "
        "VALUES (NEW.id, NEW.value * 10, 9)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP TRIGGER IF EXISTS app.ownerless_trigger_idempotent_missing");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP TRIGGER IF EXISTS app.ownerless_trigger_idempotent_ai");
    exec_ok(db, "DROP TRIGGER IF EXISTS app.ownerless_trigger_idempotent_ai");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_index_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_index_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_index_base VALUES (1, 10), (2, 20), (3, 30)");
    exec_ok(
        db,
        "CREATE INDEX ownerless_index_value_idx "
        "ON app.ownerless_index_base (value)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP INDEX ownerless_index_value_idx ON app.ownerless_index_base");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_rename_index_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_rename_index_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_rename_index_base VALUES (1, 10), (2, 20), (3, 30)");
    exec_ok(
        db,
        "CREATE INDEX ownerless_rename_old_idx "
        "ON app.ownerless_rename_index_base (value)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_rename_index_base "
        "RENAME INDEX ownerless_rename_old_idx TO ownerless_rename_new_idx"
    );
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_ignored_index_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_ignored_index_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "note INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_ignored_index_base VALUES "
        "(1, 10, 100), "
        "(2, 20, 200), "
        "(3, 30, 300)"
    );
    exec_ok(
        db,
        "CREATE INDEX ownerless_ignored_value_idx "
        "ON app.ownerless_ignored_index_base (value)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_ignored_index_base "
        "ALTER INDEX ownerless_ignored_value_idx IGNORED"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_ignored_index_base "
        "ALTER INDEX ownerless_ignored_value_idx NOT IGNORED"
    );
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void hold_ownerless_temporary_table_until_released(
    open_database_paths paths,
    unsigned value,
    child_pipes pipes
) {
    mylite_db *db;
    char sql[128];

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(
        db,
        "CREATE TEMPORARY TABLE app.ownerless_temp_peer ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert(
        snprintf(sql, sizeof(sql), "INSERT INTO app.ownerless_temp_peer VALUES (1, %u)", value) > 0
    );
    exec_ok(db, sql);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_temp_peer") == value);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_temp_peer") == value);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_unique_index_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_unique_index_base ("
        "id INT NOT NULL PRIMARY KEY, "
        "tenant_id INT NOT NULL, "
        "slug VARCHAR(32) NOT NULL, "
        "weight INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_unique_index_base VALUES "
        "(1, 1, 'alpha', 10), "
        "(2, 1, 'beta', 20), "
        "(3, 2, 'alpha', 30)"
    );
    exec_ok(
        db,
        "CREATE UNIQUE INDEX ownerless_unique_tenant_slug "
        "ON app.ownerless_unique_index_base (tenant_id, slug)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DROP INDEX ownerless_unique_tenant_slug ON app.ownerless_unique_index_base");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_primary_key_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_primary_key_base ("
        "id INT NOT NULL, "
        "code INT NOT NULL, "
        "value INT NOT NULL, "
        "PRIMARY KEY (id)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_primary_key_base VALUES "
        "(1, 10, 100), "
        "(2, 20, 200), "
        "(3, 30, 300)"
    );
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_primary_key_base "
        "DROP PRIMARY KEY, "
        "ADD PRIMARY KEY (code)"
    );
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_foreign_key_ddl_sequence(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_alter_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_alter_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_alter_parent_idx (parent_id)"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_alter_parent VALUES (1, 10), (2, 20)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_alter_child VALUES (1, 1, 100)");
    exec_ok(db, "COMMIT");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_alter_child") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child "
            "FORCE INDEX (ownerless_fk_alter_parent_idx)"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child WHERE parent_id = 1"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_alter_child") == 100U);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_fk_alter_child "
        "ADD CONSTRAINT ownerless_fk_alter_child_parent "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_alter_parent (id)"
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_alter_child") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child "
            "FORCE INDEX (ownerless_fk_alter_parent_idx)"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child WHERE parent_id = 1"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_alter_child") == 100U);
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_fk_alter_child "
        "DROP FOREIGN KEY ownerless_fk_alter_child_parent"
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_alter_child") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_alter_child") == 300U);
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_foreign_key_action_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;
    unsigned mariadb_errno = 0U;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_action_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_action_cascade_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_action_cascade_idx (parent_id), "
        "CONSTRAINT ownerless_fk_action_cascade "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_action_parent (id) "
        "ON UPDATE CASCADE "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_action_null_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_action_null_idx (parent_id), "
        "CONSTRAINT ownerless_fk_action_set_null "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_action_parent (id) "
        "ON UPDATE CASCADE "
        "ON DELETE SET NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_action_restrict_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_action_restrict_idx (parent_id), "
        "CONSTRAINT ownerless_fk_action_restrict "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_action_parent (id) "
        "ON DELETE RESTRICT"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_action_parent VALUES (1, 10), (2, 20), (3, 30)");
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_action_cascade_child VALUES (1, 1, 100), (2, 2, 200)"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_action_null_child VALUES (1, 1, 300), (2, 2, 400)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_action_restrict_child VALUES (1, 3, 500)");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "UPDATE app.ownerless_fk_action_parent SET id = 10, value = 1000 WHERE id = 1");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DELETE FROM app.ownerless_fk_action_parent WHERE id = 2");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(
        exec_status(
            db,
            "DELETE FROM app.ownerless_fk_action_parent WHERE id = 3",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_composite_foreign_key_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;
    unsigned mariadb_errno = 0U;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_composite_parent ("
        "tenant_id INT NOT NULL, "
        "id INT NOT NULL, "
        "value INT NOT NULL, "
        "PRIMARY KEY (tenant_id, id)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_composite_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "tenant_id INT NOT NULL, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_composite_parent_idx (tenant_id, parent_id), "
        "CONSTRAINT ownerless_composite_child_parent "
        "FOREIGN KEY (tenant_id, parent_id) "
        "REFERENCES app.ownerless_composite_parent (tenant_id, id) "
        "ON UPDATE CASCADE "
        "ON DELETE RESTRICT"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_composite_parent VALUES "
        "(1, 10, 100), (1, 20, 300), (2, 10, 200)"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_composite_child VALUES "
        "(1, 1, 10, 11), (2, 2, 10, 22)"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "UPDATE app.ownerless_composite_parent "
        "SET id = 11, value = 1010 "
        "WHERE tenant_id = 1 AND id = 10"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(
        exec_status(
            db,
            "DELETE FROM app.ownerless_composite_parent "
            "WHERE tenant_id = 2 AND id = 10",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_foreign_key_deep_cascade_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_deep_root ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_deep_level1 ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "CONSTRAINT ownerless_fk_deep_l1 "
        "FOREIGN KEY (id) "
        "REFERENCES app.ownerless_fk_deep_root (id) "
        "ON UPDATE CASCADE "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_deep_level2 ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "CONSTRAINT ownerless_fk_deep_l2 "
        "FOREIGN KEY (id) "
        "REFERENCES app.ownerless_fk_deep_level1 (id) "
        "ON UPDATE CASCADE "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_deep_level3 ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "CONSTRAINT ownerless_fk_deep_l3 "
        "FOREIGN KEY (id) "
        "REFERENCES app.ownerless_fk_deep_level2 (id) "
        "ON UPDATE CASCADE "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_deep_root VALUES (1, 10), (2, 20)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_deep_level1 VALUES (1, 100), (2, 200)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_deep_level2 VALUES (1, 1000), (2, 2000)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_deep_level3 VALUES (1, 10000), (2, 20000)");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "UPDATE app.ownerless_fk_deep_root SET id = 10, value = 1000 WHERE id = 1");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DELETE FROM app.ownerless_fk_deep_root WHERE id = 2");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_generated_column_foreign_key_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;
    unsigned mariadb_errno = 0U;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_generated_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_generated_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "raw_parent INT NOT NULL, "
        "parent_key INT GENERATED ALWAYS AS (raw_parent + 100) STORED, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_generated_child_parent_idx (parent_key), "
        "CONSTRAINT ownerless_fk_generated_child_parent "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_parent (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_generated_ref_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "base INT NOT NULL, "
        "parent_key INT GENERATED ALWAYS AS (base + 200) STORED, "
        "value INT NOT NULL, "
        "UNIQUE KEY ownerless_fk_generated_ref_parent_idx (parent_key)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_generated_ref_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_key INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_generated_ref_child_idx (parent_key), "
        "CONSTRAINT ownerless_fk_generated_ref_parent "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_ref_parent (parent_key) "
        "ON UPDATE RESTRICT "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_generated_parent VALUES (101, 1000)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_generated_parent VALUES (102, 2000)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_generated_parent VALUES (103, 3000)");
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_child (id, raw_parent, value) "
        "VALUES (1, 1, 10), (2, 2, 20)"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_ref_parent (id, base, value) "
        "VALUES (1, 1, 1000), (2, 2, 2000), (3, 3, 3000)"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_ref_child VALUES (1, 201, 100), (2, 202, 200)"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(
        exec_status(
            db,
            "UPDATE app.ownerless_fk_generated_parent SET id = 151 WHERE id = 101",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    mariadb_errno = 0U;
    assert(
        exec_status(
            db,
            "UPDATE app.ownerless_fk_generated_ref_parent SET base = 11 WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DELETE FROM app.ownerless_fk_generated_parent WHERE id = 102");
    exec_ok(db, "DELETE FROM app.ownerless_fk_generated_ref_parent WHERE id = 2");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_generated_column_foreign_key_policy_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_generated_policy_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_generated_policy_stored_alter ("
        "id INT NOT NULL PRIMARY KEY, "
        "raw_parent INT NOT NULL, "
        "parent_key INT GENERATED ALWAYS AS (raw_parent + 100) STORED, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_generated_policy_stored_idx (parent_key)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_generated_policy_virtual_alter ("
        "id INT NOT NULL PRIMARY KEY, "
        "raw_parent INT NOT NULL, "
        "parent_key INT GENERATED ALWAYS AS (raw_parent + 100) VIRTUAL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_generated_policy_virtual_idx (parent_key)"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_generated_policy_parent VALUES (101, 100)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_generated_policy_parent VALUES (102, 200)");
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_policy_stored_alter "
        "(id, raw_parent, value) VALUES (1, 1, 10)"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_policy_virtual_alter "
        "(id, raw_parent, value) VALUES (1, 1, 10)"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    expect_exec_mariadb_error(
        db,
        "CREATE TABLE app.ownerless_fk_generated_policy_update_null ("
        "id INT NOT NULL PRIMARY KEY, "
        "raw_parent INT NOT NULL, "
        "parent_key INT GENERATED ALWAYS AS (raw_parent + 100) STORED, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_generated_policy_update_null_idx (parent_key), "
        "CONSTRAINT ownerless_fk_generated_policy_update_null_fk "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_policy_parent (id) "
        "ON UPDATE SET NULL"
        ") ENGINE=InnoDB",
        MYLITE_TEST_WRONG_FK_OPTION_FOR_GENERATED_COLUMN_ERRNO
    );
    expect_exec_mariadb_error(
        db,
        "CREATE TABLE app.ownerless_fk_generated_policy_update_cascade ("
        "id INT NOT NULL PRIMARY KEY, "
        "raw_parent INT NOT NULL, "
        "parent_key INT GENERATED ALWAYS AS (raw_parent + 100) STORED, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_generated_policy_update_cascade_idx (parent_key), "
        "CONSTRAINT ownerless_fk_generated_policy_update_cascade_fk "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_policy_parent (id) "
        "ON UPDATE CASCADE"
        ") ENGINE=InnoDB",
        MYLITE_TEST_WRONG_FK_OPTION_FOR_GENERATED_COLUMN_ERRNO
    );
    expect_exec_mariadb_error(
        db,
        "CREATE TABLE app.ownerless_fk_generated_policy_delete_null ("
        "id INT NOT NULL PRIMARY KEY, "
        "raw_parent INT NOT NULL, "
        "parent_key INT GENERATED ALWAYS AS (raw_parent + 100) STORED, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_generated_policy_delete_null_idx (parent_key), "
        "CONSTRAINT ownerless_fk_generated_policy_delete_null_fk "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_policy_parent (id) "
        "ON DELETE SET NULL"
        ") ENGINE=InnoDB",
        MYLITE_TEST_WRONG_FK_OPTION_FOR_GENERATED_COLUMN_ERRNO
    );
    expect_exec_mariadb_error(
        db,
        "ALTER TABLE app.ownerless_fk_generated_policy_stored_alter "
        "ADD CONSTRAINT ownerless_fk_generated_policy_alter_update_null "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_policy_parent (id) "
        "ON UPDATE SET NULL",
        MYLITE_TEST_WRONG_FK_OPTION_FOR_GENERATED_COLUMN_ERRNO
    );
    expect_exec_mariadb_error(
        db,
        "ALTER TABLE app.ownerless_fk_generated_policy_stored_alter "
        "ADD CONSTRAINT ownerless_fk_generated_policy_alter_update_cascade "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_policy_parent (id) "
        "ON UPDATE CASCADE",
        MYLITE_TEST_WRONG_FK_OPTION_FOR_GENERATED_COLUMN_ERRNO
    );
    expect_exec_mariadb_error(
        db,
        "ALTER TABLE app.ownerless_fk_generated_policy_stored_alter "
        "ADD CONSTRAINT ownerless_fk_generated_policy_alter_delete_null "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_policy_parent (id) "
        "ON DELETE SET NULL",
        MYLITE_TEST_WRONG_FK_OPTION_FOR_GENERATED_COLUMN_ERRNO
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_generated_policy_virtual_create ("
        "id INT NOT NULL PRIMARY KEY, "
        "raw_parent INT NOT NULL, "
        "parent_key INT GENERATED ALWAYS AS (raw_parent + 100) VIRTUAL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_generated_policy_virtual_create_idx (parent_key), "
        "CONSTRAINT ownerless_fk_generated_policy_virtual_create_fk "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_policy_parent (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_fk_generated_policy_virtual_alter "
        "ADD CONSTRAINT ownerless_fk_generated_policy_virtual_alter_fk "
        "FOREIGN KEY (parent_key) "
        "REFERENCES app.ownerless_fk_generated_policy_parent (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE CASCADE"
    );
    exec_ok(
        db,
        "INSERT INTO app.ownerless_fk_generated_policy_virtual_create "
        "(id, raw_parent, value) VALUES (2, 2, 20)"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_cyclic_foreign_key_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;
    unsigned mariadb_errno = 0U;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cycle_a ("
        "id INT NOT NULL PRIMARY KEY, "
        "b_id INT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cycle_a_b_idx (b_id)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cycle_b ("
        "id INT NOT NULL PRIMARY KEY, "
        "a_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cycle_b_a_idx (a_id), "
        "CONSTRAINT ownerless_fk_cycle_b_a "
        "FOREIGN KEY (a_id) "
        "REFERENCES app.ownerless_fk_cycle_a (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_fk_cycle_a "
        "ADD CONSTRAINT ownerless_fk_cycle_a_b "
        "FOREIGN KEY (b_id) "
        "REFERENCES app.ownerless_fk_cycle_b (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE CASCADE"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cycle_update_a ("
        "id INT NOT NULL PRIMARY KEY, "
        "b_key INT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cycle_update_a_b_idx (b_key)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cycle_update_b ("
        "id INT NOT NULL PRIMARY KEY, "
        "a_id INT NOT NULL, "
        "value INT NOT NULL, "
        "UNIQUE KEY ownerless_fk_cycle_update_b_a_idx (a_id), "
        "CONSTRAINT ownerless_fk_cycle_update_b_a "
        "FOREIGN KEY (a_id) "
        "REFERENCES app.ownerless_fk_cycle_update_a (id) "
        "ON UPDATE CASCADE "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_fk_cycle_update_a "
        "ADD CONSTRAINT ownerless_fk_cycle_update_a_b "
        "FOREIGN KEY (b_key) "
        "REFERENCES app.ownerless_fk_cycle_update_b (a_id) "
        "ON UPDATE CASCADE "
        "ON DELETE CASCADE"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_a VALUES (1, NULL, 10)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_b VALUES (1, 1, 20)");
    exec_ok(db, "UPDATE app.ownerless_fk_cycle_a SET b_id = 1 WHERE id = 1");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_update_a VALUES (1, NULL, 100)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_update_b VALUES (1, 1, 200)");
    exec_ok(db, "UPDATE app.ownerless_fk_cycle_update_a SET b_key = 1 WHERE id = 1");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(
        exec_status(
            db,
            "UPDATE app.ownerless_fk_cycle_a SET id = 2 WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    mariadb_errno = 0U;
    assert(
        exec_status(
            db,
            "UPDATE app.ownerless_fk_cycle_b SET id = 2 WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    mariadb_errno = 0U;
    assert(
        exec_status(
            db,
            "UPDATE app.ownerless_fk_cycle_update_a SET id = 2 WHERE id = 1",
            &mariadb_errno
        ) != MYLITE_OK
    );
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == MYLITE_TEST_ROW_IS_REFERENCED_ERRNO);
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DELETE FROM app.ownerless_fk_cycle_a WHERE id = 1");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_cyclic_foreign_key_variants_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cycle3_a ("
        "id INT NOT NULL PRIMARY KEY, "
        "c_id INT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cycle3_a_c_idx (c_id)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cycle3_b ("
        "id INT NOT NULL PRIMARY KEY, "
        "a_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cycle3_b_a_idx (a_id), "
        "CONSTRAINT ownerless_fk_cycle3_b_a "
        "FOREIGN KEY (a_id) "
        "REFERENCES app.ownerless_fk_cycle3_a (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cycle3_c ("
        "id INT NOT NULL PRIMARY KEY, "
        "b_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cycle3_c_b_idx (b_id), "
        "CONSTRAINT ownerless_fk_cycle3_c_b "
        "FOREIGN KEY (b_id) "
        "REFERENCES app.ownerless_fk_cycle3_b (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_fk_cycle3_a "
        "ADD CONSTRAINT ownerless_fk_cycle3_a_c "
        "FOREIGN KEY (c_id) "
        "REFERENCES app.ownerless_fk_cycle3_c (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE CASCADE"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cycle_null_a ("
        "id INT NOT NULL PRIMARY KEY, "
        "b_id INT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cycle_null_a_b_idx (b_id)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cycle_null_b ("
        "id INT NOT NULL PRIMARY KEY, "
        "a_id INT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cycle_null_b_a_idx (a_id), "
        "CONSTRAINT ownerless_fk_cycle_null_b_a "
        "FOREIGN KEY (a_id) "
        "REFERENCES app.ownerless_fk_cycle_null_a (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE SET NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_fk_cycle_null_a "
        "ADD CONSTRAINT ownerless_fk_cycle_null_a_b "
        "FOREIGN KEY (b_id) "
        "REFERENCES app.ownerless_fk_cycle_null_b (id) "
        "ON UPDATE RESTRICT "
        "ON DELETE SET NULL"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle3_a VALUES (1, NULL, 10)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle3_b VALUES (2, 1, 20)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle3_c VALUES (3, 2, 30)");
    exec_ok(db, "UPDATE app.ownerless_fk_cycle3_a SET c_id = 3 WHERE id = 1");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_null_a VALUES (1, NULL, 1000)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cycle_null_b VALUES (2, 1, 2000)");
    exec_ok(db, "UPDATE app.ownerless_fk_cycle_null_a SET b_id = 2 WHERE id = 1");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DELETE FROM app.ownerless_fk_cycle3_a WHERE id = 1");
    exec_ok(db, "DELETE FROM app.ownerless_fk_cycle_null_a WHERE id = 1");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "DELETE FROM app.ownerless_fk_cycle_null_b WHERE id = 20");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_foreign_key_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_rename_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_rename_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_rename_parent_idx (parent_id), "
        "CONSTRAINT ownerless_fk_rename_child_parent "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_rename_parent (id) "
        "ON DELETE RESTRICT"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_rename_parent VALUES (1, 10), (2, 20)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_rename_child VALUES (1, 1, 100)");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "RENAME TABLE app.ownerless_fk_rename_parent "
        "TO app.ownerless_fk_rename_parent_moved"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_foreign_key_child_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_child_rename_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_child_rename_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_child_rename_parent_idx (parent_id), "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_child_rename_parent (id) "
        "ON DELETE RESTRICT"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_child_rename_parent VALUES (1, 10), (2, 20)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_child_rename_child VALUES (1, 1, 100)");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "RENAME TABLE app.ownerless_fk_child_rename_child "
        "TO app.ownerless_fk_child_rename_child_moved"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_foreign_key_cross_schema_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "CREATE DATABASE ownerless_fk_rename_schema");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cross_schema_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cross_schema_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cross_schema_parent_idx (parent_id), "
        "CONSTRAINT ownerless_fk_cross_schema_child_parent "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_cross_schema_parent (id) "
        "ON DELETE RESTRICT"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_cross_schema_parent VALUES (1, 10), (2, 20)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cross_schema_child VALUES (1, 1, 100)");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "RENAME TABLE app.ownerless_fk_cross_schema_parent "
        "TO ownerless_fk_rename_schema.ownerless_fk_cross_schema_parent_moved"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_foreign_key_cross_schema_child_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "CREATE DATABASE ownerless_fk_child_rename_schema");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cross_schema_child_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cross_schema_child_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cross_schema_child_parent_idx (parent_id), "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_cross_schema_child_parent (id) "
        "ON DELETE RESTRICT"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_cross_schema_child_parent VALUES (1, 10), (2, 20)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cross_schema_child_child VALUES (1, 1, 100)");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "RENAME TABLE app.ownerless_fk_cross_schema_child_child "
        "TO ownerless_fk_child_rename_schema.ownerless_fk_cross_schema_child_child_moved"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_foreign_key_multi_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_multi_rename_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_multi_rename_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_multi_rename_parent_idx (parent_id), "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_multi_rename_parent (id) "
        "ON DELETE RESTRICT"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_multi_rename_parent VALUES (1, 10), (2, 20)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_multi_rename_child VALUES (1, 1, 100)");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "RENAME TABLE "
        "app.ownerless_fk_multi_rename_parent "
        "TO app.ownerless_fk_multi_rename_parent_tmp, "
        "app.ownerless_fk_multi_rename_child "
        "TO app.ownerless_fk_multi_rename_child_moved, "
        "app.ownerless_fk_multi_rename_parent_tmp "
        "TO app.ownerless_fk_multi_rename_parent_moved"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_foreign_key_cross_schema_multi_rename_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(db, "CREATE DATABASE ownerless_fk_cross_schema_multi_schema");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cross_schema_multi_parent ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_fk_cross_schema_multi_child ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "value INT NOT NULL, "
        "INDEX ownerless_fk_cross_schema_multi_parent_idx (parent_id), "
        "FOREIGN KEY (parent_id) "
        "REFERENCES app.ownerless_fk_cross_schema_multi_parent (id) "
        "ON DELETE RESTRICT"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_fk_cross_schema_multi_parent VALUES (1, 10), (2, 20)");
    exec_ok(db, "INSERT INTO app.ownerless_fk_cross_schema_multi_child VALUES (1, 1, 100)");
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "RENAME TABLE "
        "app.ownerless_fk_cross_schema_multi_parent "
        "TO ownerless_fk_cross_schema_multi_schema."
        "ownerless_fk_cross_schema_multi_parent_moved, "
        "app.ownerless_fk_cross_schema_multi_child "
        "TO ownerless_fk_cross_schema_multi_schema."
        "ownerless_fk_cross_schema_multi_child_moved"
    );
    exec_ok(db, "COMMIT");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_check_constraint_ddl_sequence(
    open_database_paths paths,
    child_pipes pipes
) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_check_alter ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "label VARCHAR(16) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_check_alter VALUES (1, 10, 'start')");
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_check_alter "
        "ADD CONSTRAINT ownerless_check_positive CHECK (value > 0), "
        "ADD CONSTRAINT ownerless_check_label CHECK (CHAR_LENGTH(label) >= 3)"
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_check_alter") == 1U);
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_check_alter") == 2U);
    exec_ok(
        db,
        "ALTER TABLE app.ownerless_check_alter "
        "DROP CONSTRAINT ownerless_check_positive, "
        "DROP CONSTRAINT ownerless_check_label"
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_check_alter") == 35U);
    signal_pipe_message(pipes.ready_write_fd);

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void update_first_row_until_page_publish_before_append_fault(
    open_database_paths paths,
    int ready_fd
) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "page-publish-before-append", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_page_publish_fault(open_database_paths paths, int ready_fd) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "page-publish-after-append", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void open_database_until_checkpoint_fault(open_database_paths paths, int ready_fd) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "checkpoint-before-truncate", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_visible_publish_fault(
    open_database_paths paths,
    int ready_fd,
    int proceed_fd,
    int fault_ready_fd
) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", fault_ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    signal_pipe(ready_fd);
    wait_for_pipe(proceed_fd);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "pages-visible-before-checkpoint", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    exec_ok(db, "COMMIT");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_visible_checkpoint_fault(
    open_database_paths paths,
    int ready_fd
) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "pages-visible-after-checkpoint", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    exec_ok(db, "COMMIT");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_redo_reserve_fault(open_database_paths paths, int ready_fd) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "redo-after-reserve", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_redo_written_fault(open_database_paths paths, int ready_fd) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "redo-after-written", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_redo_latest_checkpoint_fault(
    open_database_paths paths,
    int ready_fd
) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "redo-before-checkpoint", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_redo_after_checkpoint_fault(
    open_database_paths paths,
    int ready_fd
) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "redo-after-checkpoint", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_native_checkpoint_reclaim_fault(
    open_database_paths paths,
    int ready_fd
) {
    mylite_db *db;
    char ready_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "native-checkpoint-before-reclaim", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}

static void update_first_row_until_native_checkpoint_reclaim_release(
    open_database_paths paths,
    int ready_fd,
    int release_fd
) {
    mylite_db *db;
    char ready_fd_value[32];
    char release_fd_value[32];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    assert(snprintf(release_fd_value, sizeof(release_fd_value), "%d", release_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 100 WHERE id = 1");
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "native-checkpoint-before-reclaim", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_RELEASE_FD", release_fd_value, 1) == 0);
    assert(mylite_close(db) == MYLITE_OK);
    _exit(MYLITE_TEST_CHILD_OK);
}

static void update_redo_gap_peer_table_after_signal(open_database_paths paths, int ready_fd) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);

    signal_pipe(ready_fd);
    exec_ok(db, "UPDATE app.ownerless_redo_gap_peer SET value = value + 7 WHERE id = 1");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(MYLITE_TEST_CHILD_OK);
}

static void assert_redo_gap_blocked_values(open_database_paths paths) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);

    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 30U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_redo_gap_peer") == 17U);
    assert(mylite_close(db) == MYLITE_OK);
}

static void create_table_until_dictionary_begin_fault(open_database_paths paths, int ready_fd) {
    create_table_until_dictionary_fault(
        paths,
        ready_fd,
        "dictionary-after-begin",
        "ownerless_ddl_begin_crash"
    );
}

static void create_table_until_dictionary_finish_fault(open_database_paths paths, int ready_fd) {
    create_table_until_dictionary_fault(
        paths,
        ready_fd,
        "dictionary-before-finish",
        "ownerless_ddl_crash"
    );
}

static void create_table_until_dictionary_after_finish_fault(
    open_database_paths paths,
    int ready_fd
) {
    create_table_until_dictionary_fault(
        paths,
        ready_fd,
        "dictionary-after-finish",
        "ownerless_ddl_after_finish_crash"
    );
}

static void create_table_until_dictionary_fault(
    open_database_paths paths,
    int ready_fd,
    const char *fault_name,
    const char *table_name
) {
    mylite_db *db;
    char ready_fd_value[32];
    char create_sql[256];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", fault_name, 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
    assert(
        snprintf(
            create_sql,
            sizeof(create_sql),
            "CREATE TABLE app.%s ("
            "id INT NOT NULL PRIMARY KEY, "
            "value INT NOT NULL"
            ") ENGINE=InnoDB",
            table_name
        ) > 0
    );
    exec_ok(db, create_sql);
    (void)mylite_close(db);
    _exit(MYLITE_TEST_CHILD_EXEC_FAILED);
}
#endif

static mylite_db *open_database(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database_allowing_failure(paths, flags);

    assert(db != NULL);
    return db;
}

static mylite_db *open_database_with_page_log_limit(
    open_database_paths paths,
    unsigned flags,
    unsigned long long limit_bytes
) {
    mylite_db *db = NULL;
    const int result = open_database_with_page_log_limit_result(paths, flags, limit_bytes, &db);

    if (result != MYLITE_OK || db == NULL) {
        fprintf(
            stderr,
            "mylite_open failed: pid=%ld path=%s flags=%u result=%d db=%p\n",
            (long)getpid(),
            paths.database_path,
            flags,
            result,
            (void *)db
        );
        fflush(stderr);
        assert(0);
    }
    return db;
}

static mylite_db *open_database_allowing_failure(open_database_paths paths, unsigned flags) {
    mylite_db *db = NULL;
    const int result = open_database_result(paths, flags, &db);

    if (result != MYLITE_OK || db == NULL) {
        fprintf(
            stderr,
            "mylite_open failed: pid=%ld path=%s flags=%u result=%d db=%p\n",
            (long)getpid(),
            paths.database_path,
            flags,
            result,
            (void *)db
        );
        fflush(stderr);
        return NULL;
    }
    return db;
}

static int open_database_result(open_database_paths paths, unsigned flags, mylite_db **out_db) {
    return open_database_with_page_log_limit_result(paths, flags, 0U, out_db);
}

static int open_database_with_page_log_limit_result(
    open_database_paths paths,
    unsigned flags,
    unsigned long long limit_bytes,
    mylite_db **out_db
) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = paths.runtime_root,
        .ownerless_page_log_limit_bytes = limit_bytes,
    };

    assert(out_db != NULL);
    *out_db = NULL;
    return mylite_open(paths.database_path, out_db, flags, &config);
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    if (mylite_exec(db, sql, NULL, NULL, &errmsg) != MYLITE_OK) {
        fprintf(
            stderr,
            "mylite_exec failed: pid=%ld sql=%s errcode=%d mariadb_errno=%u message=%s\n",
            (long)getpid(),
            sql,
            mylite_errcode(db),
            mylite_mariadb_errno(db),
            errmsg != NULL ? errmsg : mylite_errmsg(db)
        );
        mylite_free(errmsg);
        assert(0);
    }
    assert(errmsg == NULL);
}

static int exec_status(mylite_db *db, const char *sql, unsigned *mariadb_errno) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);

    if (mariadb_errno != NULL) {
        *mariadb_errno = mylite_mariadb_errno(db);
    }
    if (errmsg != NULL) {
        mylite_free(errmsg);
    }
    return result;
}

static void expect_exec_error(mylite_db *db, const char *sql) {
    unsigned mariadb_errno = 0U;

    assert(exec_status(db, sql, &mariadb_errno) != MYLITE_OK);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mariadb_errno == 0U);
}

static void expect_exec_mariadb_error(mylite_db *db, const char *sql, unsigned expected_errno) {
    unsigned mariadb_errno = 0U;
    const int result = exec_status(db, sql, &mariadb_errno);

    if (result == MYLITE_OK || mylite_errcode(db) != MYLITE_ERROR ||
        mariadb_errno != expected_errno) {
        fprintf(
            stderr,
            "expected MariaDB error %u, got result=%d errcode=%d mariadb_errno=%u sql=%s\n",
            expected_errno,
            result,
            mylite_errcode(db),
            mariadb_errno,
            sql
        );
        assert(0);
    }
}

static void expect_exec_busy(mylite_db *db, const char *sql, const char *message_part) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_BUSY);
    assert(mylite_errcode(db) == MYLITE_BUSY);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(errmsg != NULL);
    assert(strstr(errmsg, message_part) != NULL);
    mylite_free(errmsg);
}

static void expect_readonly_exec_error(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_READONLY);
    assert(mylite_errcode(db) == MYLITE_READONLY);
    assert(errmsg != NULL);
    mylite_free(errmsg);
}

static unsigned long long query_unsigned(mylite_db *db, const char *sql) {
    query_result result = {0U};
    char *errmsg = NULL;

    if (mylite_exec(db, sql, capture_first_column, &result, &errmsg) != MYLITE_OK) {
        fprintf(
            stderr,
            "mylite query failed: pid=%ld sql=%s errcode=%d mariadb_errno=%u message=%s\n",
            (long)getpid(),
            sql,
            mylite_errcode(db),
            mylite_mariadb_errno(db),
            errmsg != NULL ? errmsg : mylite_errmsg(db)
        );
        fflush(stderr);
        if (errmsg != NULL) {
            mylite_free(errmsg);
        }
        assert(0);
    }
    assert(errmsg == NULL);
    return result.value;
}

static void assert_ownerless_pressure_write_policy_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql") == 31U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_pressure_policy") == 2U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_pressure_policy") == 52U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_pressure_policy' "
            "AND column_name = 'note'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_pressure_policy "
            "WHERE note = 'ok'"
        ) == 2U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_pressure_created") == 70U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_pressure_drop'"
        ) == 0U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_ddl_stress_state(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_total
) {
    mylite_db *db = open_database(paths, flags);
    const unsigned long long observed_total =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_sql");
    const unsigned long long remaining_stress_tables = query_unsigned(
        db,
        "SELECT COUNT(*) FROM information_schema.tables "
        "WHERE table_schema = 'app' "
        "AND table_name >= 'ownerless_ddl_stress_' "
        "AND table_name < 'ownerless_ddl_stress`'"
    );

    if (observed_total != expected_total || remaining_stress_tables != 0U) {
        fprintf(
            stderr,
            "ownerless ddl stress mismatch: flags=%u total=%llu/%llu tables=%llu/0\n",
            flags,
            observed_total,
            expected_total,
            remaining_stress_tables
        );
    }
    assert(observed_total == expected_total);
    assert(remaining_stress_tables == 0U);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_auto_increment_ddl_state(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_count,
    unsigned long long expected_id_sum,
    unsigned long long expected_value_sum,
    unsigned long long expected_max_id
) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_auto_inc_ddl") == expected_count);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_auto_inc_ddl") == expected_id_sum);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_auto_inc_ddl") ==
        expected_value_sum
    );
    assert(query_unsigned(db, "SELECT MIN(id) FROM app.ownerless_auto_inc_ddl") == 1U);
    assert(query_unsigned(db, "SELECT MAX(id) FROM app.ownerless_auto_inc_ddl") == expected_max_id);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_broader_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_parent") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_child") == 0U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated "
            "WHERE full_name = 'Ada Byron' AND name_length = 9"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_online "
            "WHERE id = 1 AND value = 42 AND state = 'archived' AND priority = 8"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_online' "
            "AND column_name = 'status'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_online' "
            "AND column_name = 'state'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_online' "
            "AND column_name = 'scratch'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_like "
            "WHERE id = 1 AND value = 42 AND state = 'archived' AND priority = 8"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_ctas "
            "WHERE id = 1 AND value = 42 AND state = 'archived' AND priority = 8"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(instant_value) FROM app.ownerless_instant") == 24U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant' "
            "AND column_name = 'old_value'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant' "
            "AND column_name = 'instant_value'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant' "
            "AND column_name = 'payload'"
        ) == 3U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_instant "
            "WHERE id = 2 AND instant_value = 13 AND payload = 'done'"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_online_ddl_options_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_ddl_options") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_ddl_options") == 65U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ddl_options' "
            "AND index_name = 'ownerless_ddl_options_status_idx'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ddl_options' "
            "AND index_name = 'ownerless_ddl_options_value_idx'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT character_maximum_length FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ddl_options' "
            "AND column_name = 'payload'"
        ) == 80U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_ddl_options "
            "FORCE INDEX (ownerless_ddl_options_status_idx) "
            "WHERE status = 'done'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_ddl_options "
            "FORCE INDEX (ownerless_ddl_options_value_idx) "
            "WHERE value >= 20"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_ddl_options "
            "WHERE id = 3 AND payload = 'rebuilt'"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_generated_column_alter_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_generated_alter' "
            "AND column_name IN ('full_name', 'name_length')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated_alter "
            "WHERE id = 1 AND first_name = 'Ada' AND last_name = 'Byron'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_generated_alter "
            "WHERE id = 2 AND first_name = 'Rear' AND last_name = 'Hopper'"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_charset_convert_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_charset_convert_base' "
            "AND column_name = 'name' "
            "AND character_set_name = 'utf8mb4' "
            "AND collation_name = 'utf8mb4_general_ci'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_charset_convert_base") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_charset_convert_base") == 60U);
    assert(
        query_unsigned(
            db,
            "SELECT SUM(CHAR_LENGTH(name)) FROM app.ownerless_charset_convert_base"
        ) == 14U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_row_format_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_row_format_base' "
            "AND ROW_FORMAT = 'Dynamic'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_row_format_base' "
            "AND row_format = 'Dynamic'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_row_format_base") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_row_format_base") == 60U);
    assert(
        query_unsigned(db, "SELECT SUM(CHAR_LENGTH(payload)) FROM app.ownerless_row_format_base") ==
        768U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_row_format_base WHERE note LIKE 'before-%'"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_table_comment_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_table_comment_base' "
            "AND table_comment LIKE 'ownerless updated comment%'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_table_comment_base") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_table_comment_base") == 60U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_table_comment_base WHERE id = 3 AND value = 30"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_force_rebuild_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_force_rebuild_base' "
            "AND TABLE_ID > 0 "
            "AND SPACE > 0"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_force_rebuild_base' "
            "AND index_name = 'ownerless_force_rebuild_value_idx'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_force_rebuild_base") == 4U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_force_rebuild_base") == 100U);
    assert(query_unsigned(db, "SELECT SUM(note) FROM app.ownerless_force_rebuild_base") == 1000U);
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_force_rebuild_base "
            "FORCE INDEX (ownerless_force_rebuild_value_idx) "
            "WHERE value >= 20"
        ) == 9U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_column_default_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_column_default_alter' "
            "AND column_name = 'value' "
            "AND column_default IS NULL"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_column_default_alter' "
            "AND column_name = 'note' "
            "AND column_default = '''done'''"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_column_default_alter") == 4U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_column_default_alter") == 85U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_column_default_alter WHERE note = 'ready'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_column_default_alter WHERE note = 'done'"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_instant_column_variant_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'first_note'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'id'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT ordinal_position FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'side_value'"
        ) == 4U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name IN ('marker', 'value_double')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_instant_variants' "
            "AND column_name = 'renamed_marker'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_instant_variants") == 3U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_instant_variants") == 6U);
    assert(query_unsigned(db, "SELECT SUM(base_value) FROM app.ownerless_instant_variants") == 65U);
    assert(query_unsigned(db, "SELECT SUM(side_value) FROM app.ownerless_instant_variants") == 21U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_instant_variants "
            "WHERE id = 1 AND first_note = 'first' AND base_value = 10 "
            "AND side_value = 9 AND renamed_marker = 'renamed'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_instant_variants "
            "WHERE id = 2 AND first_note = 'peer' AND base_value = 25 "
            "AND side_value = 5 AND renamed_marker = 'peer'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_instant_variants "
            "WHERE id = 3 AND first_note = 'final' AND base_value = 30 "
            "AND side_value = 7 AND renamed_marker = 'final'"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_schema_lifecycle_absent(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *schema_path = path_join(datadir_path, "ownerless_schema");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_schema'"
        ) == 0U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM ownerless_schema.ownerless_schema_table", NULL) !=
        MYLITE_OK
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(schema_path));

    free(schema_path);
    free(datadir_path);
}

static void assert_ownerless_schema_default_ddl_absent(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *schema_path = path_join(datadir_path, "ownerless_schema_defaults");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema_defaults'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_schema_defaults'"
        ) == 0U
    );
    assert(
        exec_status(
            db,
            "SELECT COUNT(*) FROM ownerless_schema_defaults.ownerless_schema_default_before",
            NULL
        ) != MYLITE_OK
    );
    assert(
        exec_status(
            db,
            "SELECT COUNT(*) FROM ownerless_schema_defaults.ownerless_schema_default_after",
            NULL
        ) != MYLITE_OK
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(schema_path));

    free(schema_path);
    free(datadir_path);
}

static void assert_ownerless_schema_idempotent_ddl_absent(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *schema_path = path_join(datadir_path, "ownerless_schema_idempotent");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_schema_idempotent'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_schema_idempotent'"
        ) == 0U
    );
    assert(
        exec_status(
            db,
            "SELECT COUNT(*) FROM "
            "ownerless_schema_idempotent.ownerless_schema_idempotent_table",
            NULL
        ) != MYLITE_OK
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(schema_path));

    free(schema_path);
    free(datadir_path);
}

static void assert_ownerless_cross_schema_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *target_schema_path = path_join(datadir_path, "ownerless_rename_schema");
    char *source_frm_path = path_join(app_path, "ownerless_cross_schema_source.frm");
    char *source_ibd_path = path_join(app_path, "ownerless_cross_schema_source.ibd");
    char *target_frm_path = path_join(target_schema_path, "ownerless_cross_schema_moved.frm");
    char *target_ibd_path = path_join(target_schema_path, "ownerless_cross_schema_moved.ibd");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_rename_schema'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_cross_schema_source'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_rename_schema' "
            "AND table_name = 'ownerless_cross_schema_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'ownerless_rename_schema/ownerless_cross_schema_moved'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_cross_schema_source", NULL) != MYLITE_OK
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_rename_schema.ownerless_cross_schema_moved"
        ) == 3U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM ownerless_rename_schema.ownerless_cross_schema_moved"
        ) == 6U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_rename_schema.ownerless_cross_schema_moved"
        ) == 61U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_rename_schema.ownerless_cross_schema_moved "
            "WHERE note = 'child'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_rename_schema.ownerless_cross_schema_moved "
            "WHERE note = 'peer'"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(path_exists(target_schema_path));
    assert(!path_exists(source_frm_path));
    assert(!path_exists(source_ibd_path));
    assert(path_exists(target_frm_path));
    assert(path_exists(target_ibd_path));

    free(target_ibd_path);
    free(target_frm_path);
    free(source_ibd_path);
    free(source_frm_path);
    free(target_schema_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_multi_rename_cycle_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *left_frm_path = path_join(app_path, "ownerless_rename_cycle_left.frm");
    char *left_ibd_path = path_join(app_path, "ownerless_rename_cycle_left.ibd");
    char *right_frm_path = path_join(app_path, "ownerless_rename_cycle_right.frm");
    char *right_ibd_path = path_join(app_path, "ownerless_rename_cycle_right.ibd");
    char *tmp_frm_path = path_join(app_path, "ownerless_rename_cycle_tmp.frm");
    char *tmp_ibd_path = path_join(app_path, "ownerless_rename_cycle_tmp.ibd");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_cycle_left'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_cycle_right'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_cycle_tmp'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_rename_cycle_tmp'"
        ) == 0U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_rename_cycle_left") == 3U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_rename_cycle_left") == 60U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_cycle_left") == 600U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_rename_cycle_left WHERE note = 'right'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_rename_cycle_left WHERE note = 'peer-left'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_rename_cycle_right") == 3U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_rename_cycle_right") == 6U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_cycle_right") == 61U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_rename_cycle_right WHERE note = 'left'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_rename_cycle_right WHERE note = 'peer-right'"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(path_exists(left_frm_path));
    assert(path_exists(left_ibd_path));
    assert(path_exists(right_frm_path));
    assert(path_exists(right_ibd_path));
    assert(!path_exists(tmp_frm_path));
    assert(!path_exists(tmp_ibd_path));

    free(tmp_ibd_path);
    free(tmp_frm_path);
    free(right_ibd_path);
    free(right_frm_path);
    free(left_ibd_path);
    free(left_frm_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_view_ddl_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *view_path = path_join(app_path, "ownerless_view.frm");
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_base") == 30U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view'"
        ) == 0U
    );
    assert(exec_status(db, "SELECT SUM(value) FROM app.ownerless_view", NULL) != MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(view_path));

    free(view_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_view_ddl_variant_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *view_path = path_join(app_path, "ownerless_view_variant.frm");
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_variant_base") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_variant_base") == 60U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_variant'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_variant'"
        ) == 0U
    );
    assert(
        exec_status(db, "SELECT SUM(adjusted) FROM app.ownerless_view_variant", NULL) != MYLITE_OK
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(view_path));

    free(view_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_view_check_option_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *view_path = path_join(app_path, "ownerless_view_check.frm");
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_view_check_base") == 5U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_view_check_base") == 127U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_view_check_base "
            "WHERE id = 4 AND value = 27"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_view_check_base "
            "WHERE id = 5 AND value = 40"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.views "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_check'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_view_check'"
        ) == 0U
    );
    assert(exec_status(db, "SELECT COUNT(*) FROM app.ownerless_view_check", NULL) != MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(view_path));

    free(view_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_trigger_ddl_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *trg_path = path_join(app_path, "ownerless_trigger_base.TRG");
    char *trn_path = path_join(app_path, "ownerless_trigger_ai.TRN");
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_base") == 60U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_audit") == 30U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_ai'"
        ) == 0U
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_base VALUES (4, 40)");
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_base") == 100U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_audit") == 30U);
    exec_ok(db, "DELETE FROM app.ownerless_trigger_base WHERE id = 4");
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(trg_path));
    assert(!path_exists(trn_path));

    free(trn_path);
    free(trg_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_trigger_ddl_variant_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *trg_path = path_join(app_path, "ownerless_trigger_variant_base.TRG");
    char *update_trn_path = path_join(app_path, "ownerless_trigger_variant_bu.TRN");
    char *delete_trn_path = path_join(app_path, "ownerless_trigger_variant_ad.TRN");
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_variant_base") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_variant_audit") == 1U);
    assert(
        query_unsigned(db, "SELECT SUM(old_value) FROM app.ownerless_trigger_variant_audit") == 32U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND event_object_table = 'ownerless_trigger_variant_base'"
        ) == 0U
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_variant_base VALUES (3, 60)");
    exec_ok(db, "UPDATE app.ownerless_trigger_variant_base SET value = 70 WHERE id = 3");
    assert(
        query_unsigned(db, "SELECT value FROM app.ownerless_trigger_variant_base WHERE id = 3") ==
        70U
    );
    exec_ok(db, "DELETE FROM app.ownerless_trigger_variant_base WHERE id = 3");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_variant_base") == 0U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_variant_audit") == 1U);
    assert(
        query_unsigned(db, "SELECT SUM(old_value) FROM app.ownerless_trigger_variant_audit") == 32U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(trg_path));
    assert(!path_exists(update_trn_path));
    assert(!path_exists(delete_trn_path));

    free(delete_trn_path);
    free(update_trn_path);
    free(trg_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_trigger_ordering_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *trg_path = path_join(app_path, "ownerless_trigger_order_base.TRG");
    char *first_trn_path = path_join(app_path, "ownerless_trigger_order_first.TRN");
    char *second_trn_path = path_join(app_path, "ownerless_trigger_order_second.TRN");
    char *third_trn_path = path_join(app_path, "ownerless_trigger_order_third.TRN");
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_order_base") == 3U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_order_audit") == 5U);
    assert(query_unsigned(db, "SELECT SUM(marker) FROM app.ownerless_trigger_order_audit") == 9U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND event_object_table = 'ownerless_trigger_order_base'"
        ) == 0U
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_order_base VALUES (4, 40)");
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_trigger_order_audit WHERE base_id = 4"
        ) == 0U
    );
    exec_ok(db, "DELETE FROM app.ownerless_trigger_order_base WHERE id = 4");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_order_base") == 3U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_order_audit") == 5U);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(trg_path));
    assert(!path_exists(first_trn_path));
    assert(!path_exists(second_trn_path));
    assert(!path_exists(third_trn_path));

    free(third_trn_path);
    free(second_trn_path);
    free(first_trn_path);
    free(trg_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_trigger_idempotent_ddl_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *trg_path = path_join(app_path, "ownerless_trigger_idempotent_base.TRG");
    char *trn_path = path_join(app_path, "ownerless_trigger_idempotent_ai.TRN");
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_base") == 5U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_idempotent_base") == 150U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_audit") == 4U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_idempotent_audit") == 100U
    );
    assert(
        query_unsigned(db, "SELECT SUM(marker) FROM app.ownerless_trigger_idempotent_audit") == 4U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.triggers "
            "WHERE trigger_schema = 'app' "
            "AND trigger_name = 'ownerless_trigger_idempotent_ai'"
        ) == 0U
    );
    exec_ok(db, "INSERT INTO app.ownerless_trigger_idempotent_base VALUES (6, 60)");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_base") == 6U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_idempotent_base") == 210U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_audit") == 4U);
    exec_ok(db, "DELETE FROM app.ownerless_trigger_idempotent_base WHERE id = 6");
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_trigger_idempotent_base") == 5U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_trigger_idempotent_base") == 150U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(trg_path));
    assert(!path_exists(trn_path));

    free(trn_path);
    free(trg_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_stored_routine_policy_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.routines "
            "WHERE routine_schema = 'app' "
            "AND routine_name IN ("
            "'ownerless_plus_five', "
            "'ownerless_routine_policy_proc'"
            ")"
        ) == 0U
    );
    assert(exec_status(db, "SELECT app.ownerless_plus_five(7)", NULL) != MYLITE_OK);
    assert(exec_status(db, "CALL app.ownerless_routine_policy_proc()", NULL) != MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_index_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_index_base' "
            "AND index_name = 'ownerless_index_value_idx'"
        ) == 0U
    );
    assert(
        exec_status(
            db,
            "SELECT SUM(id) FROM app.ownerless_index_base "
            "FORCE INDEX (ownerless_index_value_idx) "
            "WHERE value >= 20",
            NULL
        ) != MYLITE_OK
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_index_base") == 4U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_index_base") == 100U);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_rename_index_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_index_base' "
            "AND index_name = 'ownerless_rename_old_idx'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_rename_index_base' "
            "AND index_name = 'ownerless_rename_new_idx'"
        ) == 1U
    );
    assert(
        exec_status(
            db,
            "SELECT SUM(id) FROM app.ownerless_rename_index_base "
            "FORCE INDEX (ownerless_rename_old_idx) "
            "WHERE value >= 20",
            NULL
        ) != MYLITE_OK
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_rename_index_base "
            "FORCE INDEX (ownerless_rename_new_idx) "
            "WHERE value >= 20"
        ) == 9U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_rename_index_base") == 4U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_rename_index_base") == 100U);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_ignored_index_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_ignored_index_base' "
            "AND index_name = 'ownerless_ignored_value_idx' "
            "AND ignored = 'NO'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_ignored_index_base "
            "FORCE INDEX (ownerless_ignored_value_idx) "
            "WHERE value >= 20"
        ) == 14U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_ignored_index_base") == 5U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_ignored_index_base") == 150U);
    assert(query_unsigned(db, "SELECT SUM(note) FROM app.ownerless_ignored_index_base") == 1500U);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_unique_index_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_unique_index_base' "
            "AND index_name = 'ownerless_unique_tenant_slug'"
        ) == 0U
    );
    assert(
        exec_status(
            db,
            "SELECT SUM(weight) FROM app.ownerless_unique_index_base "
            "FORCE INDEX (ownerless_unique_tenant_slug) "
            "WHERE tenant_id = 1 AND slug = 'alpha'",
            NULL
        ) != MYLITE_OK
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_unique_index_base") == 5U);
    assert(query_unsigned(db, "SELECT SUM(weight) FROM app.ownerless_unique_index_base") == 150U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_unique_index_base "
            "WHERE tenant_id = 1 AND slug = 'alpha'"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_primary_key_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_primary_key_base' "
            "AND index_name = 'PRIMARY' "
            "AND column_name = 'code' "
            "AND seq_in_index = 1 "
            "AND non_unique = 0"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_primary_key_base' "
            "AND index_name = 'PRIMARY' "
            "AND column_name = 'id'"
        ) == 0U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_primary_key_base") == 4U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_primary_key_base") == 1000U);
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM app.ownerless_primary_key_base "
            "FORCE INDEX (PRIMARY) "
            "WHERE code >= 20"
        ) == 900U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_primary_key_base "
            "WHERE id = 1"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_foreign_key_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_alter_child_parent'"
        ) == 0U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_alter_child") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_alter_child") == 1290U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_child "
            "WHERE parent_id = 99"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_alter_parent "
            "WHERE id IN (1, 2)"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_foreign_key_action_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_parent") == 2U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_action_parent") == 13U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_action_parent") == 1030U);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_parent WHERE id = 3") == 1U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_parent WHERE id = 10") ==
        1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_cascade_child") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(parent_id) FROM app.ownerless_fk_action_cascade_child") ==
        20U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_action_cascade_child") == 250U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_null_child") == 3U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_action_null_child WHERE parent_id IS NULL"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(COALESCE(parent_id, 0)) FROM app.ownerless_fk_action_null_child"
        ) == 20U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_action_null_child") == 1050U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_action_restrict_child") == 1U);
    assert(
        query_unsigned(db, "SELECT SUM(parent_id) FROM app.ownerless_fk_action_restrict_child") ==
        3U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_action_restrict_child") == 500U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_action_cascade', "
            "'ownerless_fk_action_set_null', "
            "'ownerless_fk_action_restrict'"
            ")"
        ) == 3U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_composite_foreign_key_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_composite_parent") == 3U);
    assert(query_unsigned(db, "SELECT SUM(tenant_id) FROM app.ownerless_composite_parent") == 4U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_composite_parent") == 41U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_composite_parent") == 1510U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_composite_child") == 3U);
    assert(query_unsigned(db, "SELECT SUM(tenant_id) FROM app.ownerless_composite_child") == 5U);
    assert(query_unsigned(db, "SELECT SUM(parent_id) FROM app.ownerless_composite_child") == 31U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_composite_child") == 66U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_composite_child "
            "WHERE tenant_id = 1 AND parent_id = 11"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_composite_child "
            "WHERE tenant_id = 2 AND parent_id = 10"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_composite_child_parent'"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_foreign_key_deep_cascade_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_root") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level1") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level2") == 2U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_level3") == 2U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_deep_root") == 13U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_deep_level1") == 13U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_deep_level2") == 13U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_deep_level3") == 13U);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_deep_root WHERE id IN (3, 10)") ==
        2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_deep_level1 WHERE id IN (3, 10)"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_deep_level2 WHERE id IN (3, 10)"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_deep_level3 WHERE id IN (3, 10)"
        ) == 2U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_deep_root") == 1030U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_deep_level1") == 400U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_deep_level2") == 4000U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_deep_level3") == 40000U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_deep_l1', "
            "'ownerless_fk_deep_l2', "
            "'ownerless_fk_deep_l3') "
            "AND update_rule = 'CASCADE' "
            "AND delete_rule = 'CASCADE'"
        ) == 3U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_generated_column_foreign_key_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_parent") == 2U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_generated_parent") == 204U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_generated_parent") == 4000U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_child") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(raw_parent) FROM app.ownerless_fk_generated_child") == 4U
    );
    assert(
        query_unsigned(db, "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_child") == 204U
    );
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_generated_child") == 40U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_ref_parent") == 2U);
    assert(query_unsigned(db, "SELECT SUM(base) FROM app.ownerless_fk_generated_ref_parent") == 4U);
    assert(
        query_unsigned(db, "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_ref_parent") ==
        404U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_generated_ref_parent") == 4000U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_ref_child") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_ref_child") ==
        404U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_generated_ref_child") == 400U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fk_generated_child', "
            "'ownerless_fk_generated_ref_parent') "
            "AND column_name = 'parent_key'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_generated_child_parent', "
            "'ownerless_fk_generated_ref_parent') "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'CASCADE'"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_generated_column_foreign_key_policy_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_policy_parent") == 2U
    );
    assert(
        query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_generated_policy_parent") == 204U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_generated_policy_parent") ==
        400U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_generated_policy_stored_alter") ==
        2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_policy_stored_alter"
        ) == 204U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM app.ownerless_fk_generated_policy_stored_alter"
        ) == 40U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_policy_virtual_alter"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_policy_virtual_alter"
        ) == 204U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM app.ownerless_fk_generated_policy_virtual_alter"
        ) == 40U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_generated_policy_virtual_create"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(parent_key) FROM app.ownerless_fk_generated_policy_virtual_create"
        ) == 103U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM app.ownerless_fk_generated_policy_virtual_create"
        ) == 30U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fk_generated_policy_update_null', "
            "'ownerless_fk_generated_policy_update_cascade', "
            "'ownerless_fk_generated_policy_delete_null')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_generated_policy_virtual_create_fk', "
            "'ownerless_fk_generated_policy_virtual_alter_fk') "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'CASCADE'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_generated_policy_update_null_fk', "
            "'ownerless_fk_generated_policy_update_cascade_fk', "
            "'ownerless_fk_generated_policy_delete_null_fk', "
            "'ownerless_fk_generated_policy_alter_update_null', "
            "'ownerless_fk_generated_policy_alter_update_cascade', "
            "'ownerless_fk_generated_policy_alter_delete_null')"
        ) == 0U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_cyclic_foreign_key_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle_a") == 10U);
    assert(query_unsigned(db, "SELECT SUM(b_id) FROM app.ownerless_fk_cycle_a") == 20U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cycle_a") == 100U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle_b") == 20U);
    assert(query_unsigned(db, "SELECT SUM(a_id) FROM app.ownerless_fk_cycle_b") == 10U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cycle_b") == 200U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_update_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle_update_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(b_key) FROM app.ownerless_fk_cycle_update_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cycle_update_a") == 100U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_update_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle_update_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(a_id) FROM app.ownerless_fk_cycle_update_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cycle_update_b") == 200U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_cycle_a_b', "
            "'ownerless_fk_cycle_b_a') "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'CASCADE'"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_cycle_update_a_b', "
            "'ownerless_fk_cycle_update_b_a') "
            "AND update_rule = 'CASCADE' "
            "AND delete_rule = 'CASCADE'"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_cyclic_foreign_key_variants_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle3_a") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle3_a") == 10U);
    assert(query_unsigned(db, "SELECT SUM(c_id) FROM app.ownerless_fk_cycle3_a") == 30U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cycle3_a") == 100U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle3_b") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle3_b") == 20U);
    assert(query_unsigned(db, "SELECT SUM(a_id) FROM app.ownerless_fk_cycle3_b") == 10U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cycle3_b") == 200U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle3_c") == 1U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle3_c") == 30U);
    assert(query_unsigned(db, "SELECT SUM(b_id) FROM app.ownerless_fk_cycle3_c") == 20U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cycle3_c") == 300U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_a") == 2U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle_null_a") == 110U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_a WHERE b_id IS NULL"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(b_id) FROM app.ownerless_fk_cycle_null_a") == 200U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cycle_null_a") == 110000U);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_b") == 2U);
    assert(query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_cycle_null_b") == 202U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_cycle_null_b WHERE a_id IS NULL"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT SUM(a_id) FROM app.ownerless_fk_cycle_null_b") == 100U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cycle_null_b") == 202000U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_cycle3_a_c', "
            "'ownerless_fk_cycle3_b_a', "
            "'ownerless_fk_cycle3_c_b') "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'CASCADE'"
        ) == 3U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name IN ("
            "'ownerless_fk_cycle_null_a_b', "
            "'ownerless_fk_cycle_null_b_a') "
            "AND update_rule = 'RESTRICT' "
            "AND delete_rule = 'SET NULL'"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_foreign_key_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *parent_frm_path = path_join(app_path, "ownerless_fk_rename_parent.frm");
    char *parent_ibd_path = path_join(app_path, "ownerless_fk_rename_parent.ibd");
    char *moved_frm_path = path_join(app_path, "ownerless_fk_rename_parent_moved.frm");
    char *moved_ibd_path = path_join(app_path, "ownerless_fk_rename_parent_moved.ibd");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_rename_parent'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_rename_parent_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_rename_child'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_fk_rename_parent'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_fk_rename_parent_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_rename_child_parent' "
            "AND table_name = 'ownerless_fk_rename_child' "
            "AND referenced_table_name = 'ownerless_fk_rename_parent_moved' "
            "AND delete_rule = 'RESTRICT'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_rename_child' "
            "AND index_name = 'ownerless_fk_rename_parent_idx' "
            "AND column_name = 'parent_id'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_rename_parent", NULL) != MYLITE_OK
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_rename_parent_moved") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_rename_parent_moved") == 30U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_rename_child") == 2U);
    assert(query_unsigned(db, "SELECT SUM(parent_id) FROM app.ownerless_fk_rename_child") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_rename_child") == 300U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_rename_child "
            "WHERE id = 2 AND parent_id = 2"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(parent_frm_path));
    assert(!path_exists(parent_ibd_path));
    assert(path_exists(moved_frm_path));
    assert(path_exists(moved_ibd_path));

    free(moved_ibd_path);
    free(moved_frm_path);
    free(parent_ibd_path);
    free(parent_frm_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_foreign_key_child_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *child_frm_path = path_join(app_path, "ownerless_fk_child_rename_child.frm");
    char *child_ibd_path = path_join(app_path, "ownerless_fk_child_rename_child.ibd");
    char *moved_frm_path = path_join(app_path, "ownerless_fk_child_rename_child_moved.frm");
    char *moved_ibd_path = path_join(app_path, "ownerless_fk_child_rename_child_moved.ibd");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_child_rename_parent'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_child_rename_child'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_child_rename_child_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_fk_child_rename_child'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_fk_child_rename_child_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_child_rename_child_ibfk_1'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_child_rename_child_moved_ibfk_1' "
            "AND table_name = 'ownerless_fk_child_rename_child_moved' "
            "AND referenced_table_name = 'ownerless_fk_child_rename_parent' "
            "AND delete_rule = 'RESTRICT'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_child_rename_child_moved' "
            "AND index_name = 'ownerless_fk_child_rename_parent_idx' "
            "AND column_name = 'parent_id'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_child_rename_child", NULL) !=
        MYLITE_OK
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_child_rename_parent") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_child_rename_parent") == 30U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_child_rename_child_moved") == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(parent_id) FROM app.ownerless_fk_child_rename_child_moved"
        ) == 3U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_child_rename_child_moved") ==
        300U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_child_rename_child_moved "
            "FORCE INDEX (ownerless_fk_child_rename_parent_idx)"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(child_frm_path));
    assert(!path_exists(child_ibd_path));
    assert(path_exists(moved_frm_path));
    assert(path_exists(moved_ibd_path));

    free(moved_ibd_path);
    free(moved_frm_path);
    free(child_ibd_path);
    free(child_frm_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_foreign_key_cross_schema_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *target_schema_path = path_join(datadir_path, "ownerless_fk_rename_schema");
    char *parent_frm_path = path_join(app_path, "ownerless_fk_cross_schema_parent.frm");
    char *parent_ibd_path = path_join(app_path, "ownerless_fk_cross_schema_parent.ibd");
    char *moved_frm_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_parent_moved.frm");
    char *moved_ibd_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_parent_moved.ibd");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_fk_rename_schema'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_cross_schema_parent'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_fk_rename_schema' "
            "AND table_name = 'ownerless_fk_cross_schema_parent_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_cross_schema_child'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_fk_cross_schema_parent'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'ownerless_fk_rename_schema/ownerless_fk_cross_schema_parent_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND unique_constraint_schema = 'ownerless_fk_rename_schema' "
            "AND constraint_name = 'ownerless_fk_cross_schema_child_parent' "
            "AND table_name = 'ownerless_fk_cross_schema_child' "
            "AND referenced_table_name = 'ownerless_fk_cross_schema_parent_moved' "
            "AND delete_rule = 'RESTRICT'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_parent", NULL) !=
        MYLITE_OK
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_fk_rename_schema."
            "ownerless_fk_cross_schema_parent_moved"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_rename_schema."
            "ownerless_fk_cross_schema_parent_moved"
        ) == 30U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_child") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(parent_id) FROM app.ownerless_fk_cross_schema_child") == 3U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cross_schema_child") == 300U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(path_exists(target_schema_path));
    assert(!path_exists(parent_frm_path));
    assert(!path_exists(parent_ibd_path));
    assert(path_exists(moved_frm_path));
    assert(path_exists(moved_ibd_path));

    free(moved_ibd_path);
    free(moved_frm_path);
    free(parent_ibd_path);
    free(parent_frm_path);
    free(target_schema_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_foreign_key_cross_schema_child_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *target_schema_path = path_join(datadir_path, "ownerless_fk_child_rename_schema");
    char *child_frm_path = path_join(app_path, "ownerless_fk_cross_schema_child_child.frm");
    char *child_ibd_path = path_join(app_path, "ownerless_fk_cross_schema_child_child.ibd");
    char *moved_frm_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_child_child_moved.frm");
    char *moved_ibd_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_child_child_moved.ibd");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_fk_child_rename_schema'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_cross_schema_child_parent'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_cross_schema_child_child'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_fk_child_rename_schema' "
            "AND table_name = 'ownerless_fk_cross_schema_child_child_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'app/ownerless_fk_cross_schema_child_child'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME = 'ownerless_fk_child_rename_schema/"
            "ownerless_fk_cross_schema_child_child_moved'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_cross_schema_child_child_ibfk_1'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'ownerless_fk_child_rename_schema' "
            "AND unique_constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_cross_schema_child_child_moved_ibfk_1' "
            "AND table_name = 'ownerless_fk_cross_schema_child_child_moved' "
            "AND referenced_table_name = 'ownerless_fk_cross_schema_child_parent' "
            "AND delete_rule = 'RESTRICT'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'ownerless_fk_child_rename_schema' "
            "AND table_name = 'ownerless_fk_cross_schema_child_child_moved' "
            "AND index_name = 'ownerless_fk_cross_schema_child_parent_idx' "
            "AND column_name = 'parent_id'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_child_child", NULL) !=
        MYLITE_OK
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_child_parent") == 2U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_cross_schema_child_parent") ==
        30U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_fk_child_rename_schema."
            "ownerless_fk_cross_schema_child_child_moved"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(parent_id) FROM ownerless_fk_child_rename_schema."
            "ownerless_fk_cross_schema_child_child_moved"
        ) == 3U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_child_rename_schema."
            "ownerless_fk_cross_schema_child_child_moved"
        ) == 300U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_fk_child_rename_schema."
            "ownerless_fk_cross_schema_child_child_moved "
            "FORCE INDEX (ownerless_fk_cross_schema_child_parent_idx)"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(path_exists(target_schema_path));
    assert(!path_exists(child_frm_path));
    assert(!path_exists(child_ibd_path));
    assert(path_exists(moved_frm_path));
    assert(path_exists(moved_ibd_path));

    free(moved_ibd_path);
    free(moved_frm_path);
    free(child_ibd_path);
    free(child_frm_path);
    free(target_schema_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_foreign_key_multi_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *parent_frm_path = path_join(app_path, "ownerless_fk_multi_rename_parent.frm");
    char *parent_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_parent.ibd");
    char *parent_tmp_frm_path = path_join(app_path, "ownerless_fk_multi_rename_parent_tmp.frm");
    char *parent_tmp_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_parent_tmp.ibd");
    char *parent_moved_frm_path = path_join(app_path, "ownerless_fk_multi_rename_parent_moved.frm");
    char *parent_moved_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_parent_moved.ibd");
    char *child_frm_path = path_join(app_path, "ownerless_fk_multi_rename_child.frm");
    char *child_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_child.ibd");
    char *child_moved_frm_path = path_join(app_path, "ownerless_fk_multi_rename_child_moved.frm");
    char *child_moved_ibd_path = path_join(app_path, "ownerless_fk_multi_rename_child_moved.ibd");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fk_multi_rename_parent', "
            "'ownerless_fk_multi_rename_parent_tmp', "
            "'ownerless_fk_multi_rename_child')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fk_multi_rename_parent_moved', "
            "'ownerless_fk_multi_rename_child_moved')"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME IN ("
            "'app/ownerless_fk_multi_rename_parent', "
            "'app/ownerless_fk_multi_rename_parent_tmp', "
            "'app/ownerless_fk_multi_rename_child')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME IN ("
            "'app/ownerless_fk_multi_rename_parent_moved', "
            "'app/ownerless_fk_multi_rename_child_moved')"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_multi_rename_child_ibfk_1'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND unique_constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_multi_rename_child_moved_ibfk_1' "
            "AND table_name = 'ownerless_fk_multi_rename_child_moved' "
            "AND referenced_table_name = 'ownerless_fk_multi_rename_parent_moved' "
            "AND delete_rule = 'RESTRICT'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_fk_multi_rename_child_moved' "
            "AND index_name = 'ownerless_fk_multi_rename_parent_idx' "
            "AND column_name = 'parent_id'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_parent", NULL) !=
        MYLITE_OK
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_parent_tmp", NULL) !=
        MYLITE_OK
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_child", NULL) !=
        MYLITE_OK
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_parent_moved") == 2U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_multi_rename_parent_moved") ==
        30U
    );
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_child_moved") == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(parent_id) FROM app.ownerless_fk_multi_rename_child_moved"
        ) == 3U
    );
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_multi_rename_child_moved") ==
        300U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_fk_multi_rename_child_moved "
            "FORCE INDEX (ownerless_fk_multi_rename_parent_idx)"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(parent_frm_path));
    assert(!path_exists(parent_ibd_path));
    assert(!path_exists(parent_tmp_frm_path));
    assert(!path_exists(parent_tmp_ibd_path));
    assert(path_exists(parent_moved_frm_path));
    assert(path_exists(parent_moved_ibd_path));
    assert(!path_exists(child_frm_path));
    assert(!path_exists(child_ibd_path));
    assert(path_exists(child_moved_frm_path));
    assert(path_exists(child_moved_ibd_path));

    free(child_moved_ibd_path);
    free(child_moved_frm_path);
    free(child_ibd_path);
    free(child_frm_path);
    free(parent_moved_ibd_path);
    free(parent_moved_frm_path);
    free(parent_tmp_ibd_path);
    free(parent_tmp_frm_path);
    free(parent_ibd_path);
    free(parent_frm_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_foreign_key_cross_schema_multi_rename_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *target_schema_path = path_join(datadir_path, "ownerless_fk_cross_schema_multi_schema");
    char *parent_frm_path = path_join(app_path, "ownerless_fk_cross_schema_multi_parent.frm");
    char *parent_ibd_path = path_join(app_path, "ownerless_fk_cross_schema_multi_parent.ibd");
    char *parent_moved_frm_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_multi_parent_moved.frm");
    char *parent_moved_ibd_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_multi_parent_moved.ibd");
    char *child_frm_path = path_join(app_path, "ownerless_fk_cross_schema_multi_child.frm");
    char *child_ibd_path = path_join(app_path, "ownerless_fk_cross_schema_multi_child.ibd");
    char *child_moved_frm_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_multi_child_moved.frm");
    char *child_moved_ibd_path =
        path_join(target_schema_path, "ownerless_fk_cross_schema_multi_child_moved.ibd");
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name = 'ownerless_fk_cross_schema_multi_schema'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fk_cross_schema_multi_parent', "
            "'ownerless_fk_cross_schema_multi_child')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'ownerless_fk_cross_schema_multi_schema' "
            "AND table_name IN ("
            "'ownerless_fk_cross_schema_multi_parent_moved', "
            "'ownerless_fk_cross_schema_multi_child_moved')"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME IN ("
            "'app/ownerless_fk_cross_schema_multi_parent', "
            "'app/ownerless_fk_cross_schema_multi_child')"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME IN ("
            "'ownerless_fk_cross_schema_multi_schema/"
            "ownerless_fk_cross_schema_multi_parent_moved', "
            "'ownerless_fk_cross_schema_multi_schema/"
            "ownerless_fk_cross_schema_multi_child_moved')"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'app' "
            "AND constraint_name = 'ownerless_fk_cross_schema_multi_child_ibfk_1'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.referential_constraints "
            "WHERE constraint_schema = 'ownerless_fk_cross_schema_multi_schema' "
            "AND unique_constraint_schema = 'ownerless_fk_cross_schema_multi_schema' "
            "AND constraint_name = 'ownerless_fk_cross_schema_multi_child_moved_ibfk_1' "
            "AND table_name = 'ownerless_fk_cross_schema_multi_child_moved' "
            "AND referenced_table_name = 'ownerless_fk_cross_schema_multi_parent_moved' "
            "AND delete_rule = 'RESTRICT'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'ownerless_fk_cross_schema_multi_schema' "
            "AND table_name = 'ownerless_fk_cross_schema_multi_child_moved' "
            "AND index_name = 'ownerless_fk_cross_schema_multi_parent_idx' "
            "AND column_name = 'parent_id'"
        ) == 1U
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_multi_parent", NULL) !=
        MYLITE_OK
    );
    assert(
        exec_status(db, "SELECT COUNT(*) FROM app.ownerless_fk_cross_schema_multi_child", NULL) !=
        MYLITE_OK
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_parent_moved"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_parent_moved"
        ) == 30U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_child_moved"
        ) == 2U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(parent_id) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_child_moved"
        ) == 3U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(value) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_child_moved"
        ) == 300U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM ownerless_fk_cross_schema_multi_schema."
            "ownerless_fk_cross_schema_multi_child_moved "
            "FORCE INDEX (ownerless_fk_cross_schema_multi_parent_idx)"
        ) == 2U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(path_exists(target_schema_path));
    assert(!path_exists(parent_frm_path));
    assert(!path_exists(parent_ibd_path));
    assert(path_exists(parent_moved_frm_path));
    assert(path_exists(parent_moved_ibd_path));
    assert(!path_exists(child_frm_path));
    assert(!path_exists(child_ibd_path));
    assert(path_exists(child_moved_frm_path));
    assert(path_exists(child_moved_ibd_path));

    free(child_moved_ibd_path);
    free(child_moved_frm_path);
    free(child_ibd_path);
    free(child_frm_path);
    free(parent_moved_ibd_path);
    free(parent_moved_frm_path);
    free(parent_ibd_path);
    free(parent_frm_path);
    free(target_schema_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_check_constraint_ddl_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.check_constraints "
            "WHERE constraint_schema = 'app' "
            "AND table_name = 'ownerless_check_alter' "
            "AND constraint_name IN ('ownerless_check_positive', 'ownerless_check_label')"
        ) == 0U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_check_alter") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_check_alter") == 35U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_check_alter "
            "WHERE id = 1 AND value = 10 AND label = 'start'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_check_alter "
            "WHERE id = 2 AND value = 25 AND label = 'valid'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_check_alter "
            "WHERE id = 3 AND value = 0 AND label = 'xy'"
        ) == 1U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_table_admin_policy_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_table_admin_policy'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_table_admin_policy' "
            "AND index_name = 'ownerless_table_admin_value_idx'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_table_admin_policy") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_table_admin_policy") == 60U);
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_table_admin_policy "
            "FORCE INDEX (ownerless_table_admin_value_idx) "
            "WHERE value >= 20"
        ) == 5U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_lock_tables_policy_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_lock_tables_policy'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_lock_tables_policy' "
            "AND index_name = 'ownerless_lock_tables_value_idx'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_lock_tables_policy") == 3U);
    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_lock_tables_policy") == 60U);
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_lock_tables_policy "
            "FORCE INDEX (ownerless_lock_tables_value_idx) "
            "WHERE value >= 20"
        ) == 5U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_flush_table_lock_policy_state(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_flush_table_lock_policy'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_flush_table_lock_policy' "
            "AND index_name = 'ownerless_flush_table_lock_value_idx'"
        ) == 1U
    );
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_flush_table_lock_policy") == 3U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_flush_table_lock_policy") == 60U
    );
    assert(
        query_unsigned(
            db,
            "SELECT SUM(id) FROM app.ownerless_flush_table_lock_policy "
            "FORCE INDEX (ownerless_flush_table_lock_value_idx) "
            "WHERE value >= 20"
        ) == 5U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_sequence_policy_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_sequence', "
            "'ownerless_existing_sequence'"
            ")"
        ) == 0U
    );
    assert(
        exec_status(db, "SELECT NEXT VALUE FOR app.ownerless_existing_sequence", NULL) != MYLITE_OK
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_table_directory_policy_state(
    open_database_paths paths,
    unsigned flags,
    const char *external_data_path,
    const char *external_index_path
) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_table_directory_policy") == 2U);
    assert(
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_table_directory_policy") == 30U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_table_directory_policy' "
            "AND index_name = 'ownerless_table_directory_value_idx'"
        ) == 1U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_data_directory_policy', "
            "'ownerless_index_directory_policy', "
            "'ownerless_partition_directory_policy'"
            ")"
        ) == 0U
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(external_data_path));
    assert(!path_exists(external_index_path));
}

static void assert_ownerless_special_index_policy_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_special_index_base") == 1U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_special_index_base' "
            "AND index_name IN ("
            "'ownerless_fulltext_idx', "
            "'ownerless_fulltext_replace_idx', "
            "'ownerless_fulltext_alter_idx', "
            "'ownerless_spatial_idx', "
            "'ownerless_spatial_alter_idx'"
            ")"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_fulltext_inline', "
            "'ownerless_spatial_inline'"
            ")"
        ) == 0U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_partition_policy_state(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);

    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_partition_base") == 10U);
    assert(
        query_unsigned(db, "SELECT SUM(`partition`) FROM app.ownerless_partition_keyword_column") ==
        7U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.partitions "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_partition_base' "
            "AND partition_name IS NOT NULL"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name IN ("
            "'ownerless_partitioned_range', "
            "'ownerless_partitioned_hash', "
            "'ownerless_partitioned_subpart'"
            ")"
        ) == 0U
    );
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_tablespace_management_policy_state(
    open_database_paths paths,
    unsigned flags,
    const char *database_path
) {
    mylite_db *db = open_database(paths, flags);
    char *datadir_path = path_join(database_path, "datadir");
    char *app_path = path_join(datadir_path, "app");
    char *ibd_path = path_join(app_path, "ownerless_tablespace_policy.ibd");

    assert(query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_tablespace_policy") == 10U);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name = 'ownerless_tablespace_policy'"
        ) == 1U
    );
    assert(path_exists(ibd_path));
    assert(mylite_close(db) == MYLITE_OK);

    free(ibd_path);
    free(app_path);
    free(datadir_path);
}

static void assert_ownerless_temp_stress_permanent_table(
    open_database_paths paths,
    unsigned flags
) {
    mylite_db *db = open_database(paths, flags);
    const unsigned long long observed_total =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_temp_stress");

    if (observed_total != 41U) {
        fprintf(
            stderr,
            "ownerless temporary stress mismatch: flags=%u total=%llu/41\n",
            flags,
            observed_total
        );
    }
    assert(observed_total == 41U);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_ddl_tables(mylite_db *db) {
    const unsigned long long expected_table_count =
        MYLITE_TEST_DDL_WORKER_COUNT * MYLITE_TEST_DDL_TABLES_PER_WORKER;
    char sql[256];

    for (unsigned worker_id = 0U; worker_id < MYLITE_TEST_DDL_WORKER_COUNT; ++worker_id) {
        for (unsigned table_id = 0U; table_id < MYLITE_TEST_DDL_TABLES_PER_WORKER; ++table_id) {
            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "SELECT SUM(value) FROM app.ownerless_ddl_%u_%u",
                    worker_id,
                    table_id
                ) > 0
            );
            assert(query_unsigned(db, sql) == (worker_id * 100U) + table_id);

            assert(
                snprintf(
                    sql,
                    sizeof(sql),
                    "SELECT SUM(note) FROM app.ownerless_ddl_%u_%u",
                    worker_id,
                    table_id
                ) > 0
            );
            assert(query_unsigned(db, sql) == (worker_id * 10U) + table_id);
        }
    }

    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'app' "
            "AND table_name >= 'ownerless_ddl_' "
            "AND table_name < 'ownerless_ddm'"
        ) == expected_table_count
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME >= 'app/ownerless_ddl_' "
            "AND NAME < 'app/ownerless_ddm'"
        ) == expected_table_count
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(DISTINCT TABLE_ID) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME >= 'app/ownerless_ddl_' "
            "AND NAME < 'app/ownerless_ddm'"
        ) == expected_table_count
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(DISTINCT SPACE) FROM information_schema.INNODB_SYS_TABLES "
            "WHERE NAME >= 'app/ownerless_ddl_' "
            "AND NAME < 'app/ownerless_ddm'"
        ) == expected_table_count
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name >= 'ownerless_ddl_' "
            "AND table_name < 'ownerless_ddm' "
            "AND index_name = 'ownerless_ddl_value_idx'"
        ) == 0U
    );
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM information_schema.statistics "
            "WHERE table_schema = 'app' "
            "AND table_name >= 'ownerless_ddl_' "
            "AND table_name < 'ownerless_ddm' "
            "AND index_name = 'ownerless_ddl_note_idx'"
        ) == expected_table_count
    );
}

static void assert_total_value(open_database_paths paths, unsigned long long expected) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE);
    query_result result = {0U};
    char *errmsg = NULL;

    assert(
        mylite_exec(
            db,
            "SELECT SUM(value) FROM app.ownerless_sql",
            capture_first_column,
            &result,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    if (result.value != expected) {
        fprintf(stderr, "expected total value %llu, got %llu\n", expected, result.value);
    }
    assert(result.value == expected);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_total_value(open_database_paths paths, unsigned long long expected) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    query_result result = {0U};
    char *errmsg = NULL;

    assert(
        mylite_exec(
            db,
            "SELECT SUM(value) FROM app.ownerless_sql",
            capture_first_column,
            &result,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    if (result.value != expected) {
        fprintf(stderr, "expected ownerless total value %llu, got %llu\n", expected, result.value);
    }
    assert(result.value == expected);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_commit_race_total(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_sum
) {
    mylite_db *db = open_database(paths, flags);
    char sql[192];
    unsigned long long actual_sum = 0U;

    for (unsigned table_id = 1U; table_id <= MYLITE_TEST_COMMIT_RACE_WORKER_COUNT; ++table_id) {
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "SELECT SUM(value) FROM app.ownerless_commit_race_%u",
                table_id
            ) > 0
        );
        actual_sum += query_unsigned(db, sql);
    }
    if (actual_sum != expected_sum) {
        fprintf(
            stderr,
            "expected ownerless commit-race total %llu with flags %u, got %llu\n",
            expected_sum,
            flags,
            actual_sum
        );
    }
    assert(actual_sum == expected_sum);
    assert(mylite_close(db) == MYLITE_OK);
}

static uint64_t assert_commit_race_recovery_anchors(
    const char *database_path,
    uint64_t minimum_visible_lsn
) {
    assert_concurrency_wal_has_page_versions_or_checkpoint(database_path);
    if (!concurrency_wal_is_checkpointed(database_path)) {
        assert_concurrency_page_index_has_entries(database_path);
    }

    const uint64_t checkpoint_visible_lsn = read_concurrency_checkpoint_visible_lsn(database_path);
    if (checkpoint_visible_lsn < minimum_visible_lsn || checkpoint_visible_lsn == 0U) {
        fprintf(
            stderr,
            "expected ownerless commit-race checkpoint visible LSN >= %llu, got %llu\n",
            (unsigned long long)minimum_visible_lsn,
            (unsigned long long)checkpoint_visible_lsn
        );
    }
    assert(checkpoint_visible_lsn >= minimum_visible_lsn);
    assert(checkpoint_visible_lsn > 0U);
    assert(read_concurrency_redo_visible_lsn(database_path) >= checkpoint_visible_lsn);
    return checkpoint_visible_lsn;
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void assert_total_value_is_one_of(
    open_database_paths paths,
    unsigned long long first_expected,
    unsigned long long second_expected
) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    query_result result = {0U};
    char *errmsg = NULL;

    assert(
        mylite_exec(
            db,
            "SELECT SUM(value) FROM app.ownerless_sql",
            capture_first_column,
            &result,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    if (result.value != first_expected && result.value != second_expected) {
        fprintf(
            stderr,
            "expected total value %llu or %llu, got %llu\n",
            first_expected,
            second_expected,
            result.value
        );
    }
    assert(result.value == first_expected || result.value == second_expected);
    assert(mylite_close(db) == MYLITE_OK);
}
#endif

static void assert_table_total_value_is_one_of(
    open_database_paths paths,
    unsigned long long first_expected,
    unsigned long long second_expected
) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE);
    query_result result = {0U};
    char *errmsg = NULL;

    assert(
        mylite_exec(
            db,
            "SELECT "
            "(SELECT value FROM app.ownerless_a WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_b WHERE id = 1)",
            capture_first_column,
            &result,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    if (result.value != first_expected && result.value != second_expected) {
        fprintf(
            stderr,
            "expected table value total %llu or %llu, got %llu\n",
            first_expected,
            second_expected,
            result.value
        );
    }
    assert(result.value == first_expected || result.value == second_expected);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_table_values(open_database_paths paths) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE);
    query_result result = {0U};
    char *errmsg = NULL;

    assert(
        mylite_exec(
            db,
            "SELECT "
            "(SELECT value FROM app.ownerless_a WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_b WHERE id = 1)",
            capture_first_column,
            &result,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    if (result.value != 303U) {
        fprintf(stderr, "expected table value total 303, got %llu\n", result.value);
    }
    assert(result.value == 303U);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_stress_total(open_database_paths paths, unsigned long long expected) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE);
    query_result result = {0U};
    char *errmsg = NULL;

    assert(
        mylite_exec(
            db,
            "SELECT "
            "(SELECT value FROM app.ownerless_stress_1 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_2 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_3 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_4 WHERE id = 1)",
            capture_first_column,
            &result,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    if (result.value != expected) {
        fprintf(stderr, "expected ownerless stress total %llu, got %llu\n", expected, result.value);
    }
    assert(result.value == expected);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    result.value = 0U;
    assert(
        mylite_exec(
            db,
            "SELECT "
            "(SELECT value FROM app.ownerless_stress_1 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_2 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_3 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_4 WHERE id = 1)",
            capture_first_column,
            &result,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    if (result.value != expected) {
        fprintf(stderr, "expected ownerless stress total %llu, got %llu\n", expected, result.value);
    }
    assert(result.value == expected);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_checksum_stress_totals(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_sum,
    unsigned long long expected_versions,
    unsigned long long expected_weighted_sum
) {
    mylite_db *db = open_database(paths, flags);
    const unsigned long long row_count =
        MYLITE_TEST_CHECKSUM_STRESS_WORKER_COUNT * MYLITE_TEST_CHECKSUM_STRESS_ROWS_PER_WORKER;
    const unsigned long long observed_count =
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_checksum_stress");
    const unsigned long long observed_sum =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_checksum_stress");
    const unsigned long long observed_versions =
        query_unsigned(db, "SELECT SUM(version) FROM app.ownerless_checksum_stress");
    const unsigned long long observed_weighted_sum =
        query_unsigned(db, "SELECT SUM(id * value) FROM app.ownerless_checksum_stress");

    if (observed_count != row_count || observed_sum != expected_sum ||
        observed_versions != expected_versions || observed_weighted_sum != expected_weighted_sum) {
        fprintf(
            stderr,
            "ownerless checksum stress mismatch: flags=%u "
            "count=%llu/%llu sum=%llu/%llu versions=%llu/%llu weighted=%llu/%llu\n",
            flags,
            observed_count,
            row_count,
            observed_sum,
            expected_sum,
            observed_versions,
            expected_versions,
            observed_weighted_sum,
            expected_weighted_sum
        );
    }
    assert(observed_count == row_count);
    assert(observed_sum == expected_sum);
    assert(observed_versions == expected_versions);
    assert(observed_weighted_sum == expected_weighted_sum);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_random_tx_stress_totals(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_sum,
    unsigned long long expected_versions,
    unsigned long long expected_weighted_sum
) {
    mylite_db *db = open_database(paths, flags);
    const unsigned long long observed_count =
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_random_tx_stress");
    const unsigned long long observed_sum =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_random_tx_stress");
    const unsigned long long observed_versions =
        query_unsigned(db, "SELECT SUM(version) FROM app.ownerless_random_tx_stress");
    const unsigned long long observed_weighted_sum =
        query_unsigned(db, "SELECT SUM(id * value) FROM app.ownerless_random_tx_stress");

    if (observed_count != MYLITE_TEST_RANDOM_TX_STRESS_ROW_COUNT || observed_sum != expected_sum ||
        observed_versions != expected_versions || observed_weighted_sum != expected_weighted_sum) {
        fprintf(
            stderr,
            "ownerless random tx stress mismatch: flags=%u "
            "count=%llu/%u sum=%llu/%llu versions=%llu/%llu weighted=%llu/%llu\n",
            flags,
            observed_count,
            MYLITE_TEST_RANDOM_TX_STRESS_ROW_COUNT,
            observed_sum,
            expected_sum,
            observed_versions,
            expected_versions,
            observed_weighted_sum,
            expected_weighted_sum
        );
    }
    assert(observed_count == MYLITE_TEST_RANDOM_TX_STRESS_ROW_COUNT);
    assert(observed_sum == expected_sum);
    assert(observed_versions == expected_versions);
    assert(observed_weighted_sum == expected_weighted_sum);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_fk_graph_stress_state(
    open_database_paths paths,
    unsigned flags,
    unsigned rounds
) {
    mylite_db *db = open_database(paths, flags);
    const unsigned long long expected_count = MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT;
    const unsigned long long total_delta = ownerless_fk_graph_stress_total_delta_sum(rounds);
    const unsigned long long root_versions = expected_count * rounds * 2ULL;
    const unsigned long long child_versions = expected_count * rounds;
    unsigned long long expected_cascade_ref_sum = 0U;
    unsigned long long expected_restrict_ref_sum = 0U;
    const unsigned long long root_count =
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_graph_root");
    const unsigned long long root_id_sum =
        query_unsigned(db, "SELECT SUM(id) FROM app.ownerless_fk_graph_root");
    const unsigned long long root_value_sum =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_graph_root");
    const unsigned long long root_version_sum =
        query_unsigned(db, "SELECT SUM(version) FROM app.ownerless_fk_graph_root");
    const unsigned long long deleted_setnull_roots =
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_graph_root WHERE kind = 2");
    const unsigned long long cascade_count =
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_graph_cascade_child");
    const unsigned long long cascade_ref_sum =
        query_unsigned(db, "SELECT SUM(root_id) FROM app.ownerless_fk_graph_cascade_child");
    const unsigned long long cascade_value_sum =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_graph_cascade_child");
    const unsigned long long cascade_version_sum =
        query_unsigned(db, "SELECT SUM(version) FROM app.ownerless_fk_graph_cascade_child");
    const unsigned long long setnull_count =
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_graph_setnull_child");
    const unsigned long long setnull_null_count = query_unsigned(
        db,
        "SELECT COUNT(*) FROM app.ownerless_fk_graph_setnull_child WHERE root_id IS NULL"
    );
    const unsigned long long setnull_ref_sum = query_unsigned(
        db,
        "SELECT SUM(COALESCE(root_id, 0)) FROM app.ownerless_fk_graph_setnull_child"
    );
    const unsigned long long setnull_value_sum =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_graph_setnull_child");
    const unsigned long long setnull_version_sum =
        query_unsigned(db, "SELECT SUM(version) FROM app.ownerless_fk_graph_setnull_child");
    const unsigned long long restrict_count =
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_fk_graph_restrict_child");
    const unsigned long long restrict_ref_sum =
        query_unsigned(db, "SELECT SUM(root_id) FROM app.ownerless_fk_graph_restrict_child");
    const unsigned long long restrict_value_sum =
        query_unsigned(db, "SELECT SUM(value) FROM app.ownerless_fk_graph_restrict_child");
    const unsigned long long restrict_version_sum =
        query_unsigned(db, "SELECT SUM(version) FROM app.ownerless_fk_graph_restrict_child");
    const unsigned long long constraint_count = query_unsigned(
        db,
        "SELECT COUNT(*) FROM information_schema.referential_constraints "
        "WHERE constraint_schema = 'app' "
        "AND ("
        "(constraint_name = 'ownerless_fk_graph_cascade_fk' "
        "AND update_rule = 'CASCADE' "
        "AND delete_rule = 'CASCADE') "
        "OR (constraint_name = 'ownerless_fk_graph_setnull_fk' "
        "AND update_rule = 'CASCADE' "
        "AND delete_rule = 'SET NULL') "
        "OR (constraint_name = 'ownerless_fk_graph_restrict_fk' "
        "AND update_rule = 'RESTRICT' "
        "AND delete_rule = 'RESTRICT'))"
    );

    for (unsigned worker_id = 1U; worker_id <= MYLITE_TEST_FK_GRAPH_STRESS_WORKER_COUNT;
         ++worker_id) {
        expected_cascade_ref_sum += ownerless_fk_graph_stress_initial_id(worker_id, 1U) + rounds;
        expected_restrict_ref_sum += ownerless_fk_graph_stress_initial_id(worker_id, 3U);
    }

    if (root_count != expected_count * 2ULL ||
        root_id_sum != expected_cascade_ref_sum + expected_restrict_ref_sum ||
        root_value_sum != total_delta * 2ULL || root_version_sum != root_versions ||
        deleted_setnull_roots != 0U || cascade_count != expected_count ||
        cascade_ref_sum != expected_cascade_ref_sum || cascade_value_sum != total_delta ||
        cascade_version_sum != child_versions || setnull_count != expected_count ||
        setnull_null_count != expected_count || setnull_ref_sum != 0U ||
        setnull_value_sum != total_delta || setnull_version_sum != child_versions ||
        restrict_count != expected_count || restrict_ref_sum != expected_restrict_ref_sum ||
        restrict_value_sum != total_delta || restrict_version_sum != child_versions ||
        constraint_count != 3U) {
        fprintf(
            stderr,
            "ownerless fk graph stress mismatch: flags=%u "
            "root=count:%llu/%llu ids:%llu/%llu value:%llu/%llu version:%llu/%llu "
            "setnull_roots:%llu/0 "
            "cascade=count:%llu/%llu ref:%llu/%llu value:%llu/%llu version:%llu/%llu "
            "setnull=count:%llu/%llu null:%llu/%llu ref:%llu/0 value:%llu/%llu "
            "version:%llu/%llu "
            "restrict=count:%llu/%llu ref:%llu/%llu value:%llu/%llu version:%llu/%llu "
            "constraints:%llu/3\n",
            flags,
            root_count,
            expected_count * 2ULL,
            root_id_sum,
            expected_cascade_ref_sum + expected_restrict_ref_sum,
            root_value_sum,
            total_delta * 2ULL,
            root_version_sum,
            root_versions,
            deleted_setnull_roots,
            cascade_count,
            expected_count,
            cascade_ref_sum,
            expected_cascade_ref_sum,
            cascade_value_sum,
            total_delta,
            cascade_version_sum,
            child_versions,
            setnull_count,
            expected_count,
            setnull_null_count,
            expected_count,
            setnull_ref_sum,
            setnull_value_sum,
            total_delta,
            setnull_version_sum,
            child_versions,
            restrict_count,
            expected_count,
            restrict_ref_sum,
            expected_restrict_ref_sum,
            restrict_value_sum,
            total_delta,
            restrict_version_sum,
            child_versions,
            constraint_count
        );
    }
    assert(root_count == expected_count * 2ULL);
    assert(root_id_sum == expected_cascade_ref_sum + expected_restrict_ref_sum);
    assert(root_value_sum == total_delta * 2ULL);
    assert(root_version_sum == root_versions);
    assert(deleted_setnull_roots == 0U);
    assert(cascade_count == expected_count);
    assert(cascade_ref_sum == expected_cascade_ref_sum);
    assert(cascade_value_sum == total_delta);
    assert(cascade_version_sum == child_versions);
    assert(setnull_count == expected_count);
    assert(setnull_null_count == expected_count);
    assert(setnull_ref_sum == 0U);
    assert(setnull_value_sum == total_delta);
    assert(setnull_version_sum == child_versions);
    assert(restrict_count == expected_count);
    assert(restrict_ref_sum == expected_restrict_ref_sum);
    assert(restrict_value_sum == total_delta);
    assert(restrict_version_sum == child_versions);
    assert(constraint_count == 3U);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_tx_stress_totals(
    open_database_paths paths,
    unsigned flags,
    unsigned rounds
) {
    mylite_db *db = open_database(paths, flags);
    char sql[256];

    for (unsigned worker_id = 1U; worker_id <= MYLITE_TEST_TX_STRESS_WORKER_COUNT; ++worker_id) {
        const unsigned long long delta_sum = ownerless_tx_stress_delta_sum(worker_id, rounds);
        const unsigned long long expected_value_sum = delta_sum * 3ULL;
        const unsigned long long expected_weighted_sum = delta_sum * 7ULL;
        const unsigned long long expected_versions = rounds * 2ULL;
        unsigned long long observed_value_sum;
        unsigned long long observed_weighted_sum;
        unsigned long long observed_versions;
        unsigned long long observed_rolled_back_value;
        unsigned long long observed_rolled_back_versions;

        assert(
            snprintf(
                sql,
                sizeof(sql),
                "SELECT SUM(value) FROM app.ownerless_tx_stress_%u",
                worker_id
            ) > 0
        );
        observed_value_sum = query_unsigned(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "SELECT SUM(id * value) FROM app.ownerless_tx_stress_%u",
                worker_id
            ) > 0
        );
        observed_weighted_sum = query_unsigned(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "SELECT SUM(version) FROM app.ownerless_tx_stress_%u",
                worker_id
            ) > 0
        );
        observed_versions = query_unsigned(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "SELECT value FROM app.ownerless_tx_stress_%u WHERE id = 2",
                worker_id
            ) > 0
        );
        observed_rolled_back_value = query_unsigned(db, sql);
        assert(
            snprintf(
                sql,
                sizeof(sql),
                "SELECT version FROM app.ownerless_tx_stress_%u WHERE id = 2",
                worker_id
            ) > 0
        );
        observed_rolled_back_versions = query_unsigned(db, sql);

        if (observed_value_sum != expected_value_sum ||
            observed_weighted_sum != expected_weighted_sum ||
            observed_versions != expected_versions || observed_rolled_back_value != 0U ||
            observed_rolled_back_versions != 0U) {
            fprintf(
                stderr,
                "ownerless tx stress mismatch: flags=%u worker=%u "
                "sum=%llu/%llu weighted=%llu/%llu versions=%llu/%llu "
                "rolled_back=%llu/%llu\n",
                flags,
                worker_id,
                observed_value_sum,
                expected_value_sum,
                observed_weighted_sum,
                expected_weighted_sum,
                observed_versions,
                expected_versions,
                observed_rolled_back_value,
                observed_rolled_back_versions
            );
        }
        assert(observed_value_sum == expected_value_sum);
        assert(observed_weighted_sum == expected_weighted_sum);
        assert(observed_versions == expected_versions);
        assert(observed_rolled_back_value == 0U);
        assert(observed_rolled_back_versions == 0U);
    }

    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_ownerless_write_skew_invariant(open_database_paths paths, unsigned flags) {
    mylite_db *db = open_database(paths, flags);
    const unsigned long long row_count =
        query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_write_skew");
    const unsigned long long on_call =
        query_unsigned(db, "SELECT SUM(on_call) FROM app.ownerless_write_skew");

    if (row_count != 2U || (on_call != 1U && on_call != 2U)) {
        fprintf(
            stderr,
            "ownerless write-skew invariant failed: flags=%u count=%llu on_call=%llu\n",
            flags,
            row_count,
            on_call
        );
    }
    assert(row_count == 2U);
    assert(on_call == 1U || on_call == 2U);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_concurrency_wal_has_page_versions_or_checkpoint(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *wal_path = path_join(concurrency_path, "mylite-concurrency.wal");
    struct stat wal_stat;
    const off_t empty_log_end =
        MYLITE_TEST_CONCURRENCY_RECOVERY_HEADER_SIZE + MYLITE_TEST_PAGE_LOG_HEADER_SIZE;
    const off_t first_record_end = MYLITE_TEST_CONCURRENCY_RECOVERY_HEADER_SIZE +
                                   MYLITE_TEST_PAGE_LOG_HEADER_SIZE +
                                   MYLITE_TEST_PAGE_LOG_RECORD_HEADER_SIZE;

    assert(stat(wal_path, &wal_stat) == 0);
    if (wal_stat.st_size != empty_log_end && wal_stat.st_size <= first_record_end) {
        fprintf(
            stderr,
            "expected ownerless WAL page records or checkpointed log, got size %lld\n",
            (long long)wal_stat.st_size
        );
    }
    assert(wal_stat.st_size == empty_log_end || wal_stat.st_size > first_record_end);

    free(wal_path);
    free(concurrency_path);
}

static void assert_concurrency_page_index_has_entries(const char *database_path) {
    const uint64_t active_count = read_concurrency_page_index_active_count(database_path);

    if (active_count == 0U) {
        fprintf(stderr, "expected ownerless page-index entries, got 0\n");
    }
    assert(active_count > 0U);
}

static unsigned count_concurrency_wal_records_at_or_before(
    const char *database_path,
    uint64_t commit_lsn
) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *wal_path = path_join(concurrency_path, "mylite-concurrency.wal");
    struct stat wal_stat;
    unsigned char bytes[8];
    unsigned count = 0U;
    off_t record_offset =
        MYLITE_TEST_CONCURRENCY_RECOVERY_HEADER_SIZE + MYLITE_TEST_PAGE_LOG_HEADER_SIZE;
    int fd = open(wal_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    assert(fstat(fd, &wal_stat) == 0);
    while (record_offset + MYLITE_TEST_PAGE_LOG_RECORD_HEADER_SIZE <= wal_stat.st_size) {
        uint64_t record_commit_lsn;
        uint64_t payload_size;
        off_t payload_offset;
        off_t next_record_offset;

        read_exact_at(
            fd,
            bytes,
            sizeof(bytes),
            record_offset + MYLITE_TEST_PAGE_LOG_RECORD_COMMIT_LSN_OFFSET
        );
        record_commit_lsn = read_le64(bytes);
        read_exact_at(
            fd,
            bytes,
            sizeof(bytes),
            record_offset + MYLITE_TEST_PAGE_LOG_RECORD_PAYLOAD_SIZE_OFFSET
        );
        payload_size = read_le64(bytes);
        payload_offset = record_offset + MYLITE_TEST_PAGE_LOG_RECORD_HEADER_SIZE;
        assert(payload_size <= (uint64_t)(wal_stat.st_size - payload_offset));
        next_record_offset = payload_offset + (off_t)payload_size;
        if (record_commit_lsn <= commit_lsn) {
            ++count;
        }
        record_offset = next_record_offset;
    }

    assert(close(fd) == 0);
    free(wal_path);
    free(concurrency_path);
    return count;
}

static off_t concurrency_wal_size(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *wal_path = path_join(concurrency_path, "mylite-concurrency.wal");
    struct stat wal_stat;

    assert(stat(wal_path, &wal_stat) == 0);
    free(wal_path);
    free(concurrency_path);
    return wal_stat.st_size;
}

static int concurrency_wal_is_checkpointed(const char *database_path) {
    const off_t empty_log_end =
        MYLITE_TEST_CONCURRENCY_RECOVERY_HEADER_SIZE + MYLITE_TEST_PAGE_LOG_HEADER_SIZE;
    return concurrency_wal_size(database_path) == empty_log_end;
}

static void assert_concurrency_wal_checkpointed(const char *database_path) {
    if (!concurrency_wal_is_checkpointed(database_path)) {
        fprintf(stderr, "ownerless page-version WAL was not checkpointed: %s\n", database_path);
        fflush(stderr);
        assert(0);
    }
}

static void remove_concurrency_shm(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");

    assert(unlink(shm_path) == 0);
    free(shm_path);
    free(concurrency_path);
}

static int capture_first_column(void *ctx, int column_count, char **values, char **columns) {
    query_result *result = ctx;

    assert(column_count == 1);
    if (values[0] == NULL) {
        fprintf(
            stderr,
            "mylite query returned NULL first column: pid=%ld column=%s\n",
            (long)getpid(),
            columns[0] != NULL ? columns[0] : "(unknown)"
        );
        fflush(stderr);
    }
    assert(values[0] != NULL);
    result->value = strtoull(values[0], NULL, 10);
    return 0;
}

static void assert_show_create_trigger_contains(
    mylite_db *db,
    const char *sql,
    const char *trigger_name,
    const char *statement_substring
) {
    show_create_trigger_expectation expectation = {
        .trigger_name = trigger_name,
        .statement_substring = statement_substring,
        .seen_rows = 0U,
    };
    char *errmsg = NULL;

    if (mylite_exec(db, sql, capture_show_create_trigger, &expectation, &errmsg) != MYLITE_OK) {
        fprintf(
            stderr,
            "mylite show trigger failed: pid=%ld sql=%s errcode=%d mariadb_errno=%u "
            "message=%s\n",
            (long)getpid(),
            sql,
            mylite_errcode(db),
            mylite_mariadb_errno(db),
            errmsg != NULL ? errmsg : mylite_errmsg(db)
        );
        fflush(stderr);
        if (errmsg != NULL) {
            mylite_free(errmsg);
        }
        assert(0);
    }
    assert(errmsg == NULL);
    assert(expectation.seen_rows == 1U);
}

static int capture_show_create_trigger(void *ctx, int column_count, char **values, char **columns) {
    show_create_trigger_expectation *expectation = ctx;

    assert(column_count >= 3);
    assert(strcmp(columns[0], "Trigger") == 0);
    assert(strcmp(columns[2], "SQL Original Statement") == 0);
    assert(values[0] != NULL);
    assert(values[2] != NULL);
    assert(strcmp(values[0], expectation->trigger_name) == 0);
    if (strstr(values[2], expectation->statement_substring) == NULL) {
        fprintf(
            stderr,
            "SHOW CREATE TRIGGER statement missing substring: trigger=%s statement=%s "
            "expected=%s\n",
            expectation->trigger_name,
            values[2],
            expectation->statement_substring
        );
        fflush(stderr);
    }
    assert(strstr(values[2], expectation->statement_substring) != NULL);
    ++expectation->seen_rows;
    return 0;
}

static uint64_t wait_for_concurrency_ownerless_write_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
) {
    const unsigned iterations = timeout_ms * 1000U / MYLITE_TEST_WAIT_POLL_INTERVAL_US;

    for (unsigned iteration = 0U; iteration <= iterations; ++iteration) {
        const uint64_t waiting_count =
            read_concurrency_innodb_lock_waiting_count(database_path) +
            read_concurrency_page_write_lock_waiting_count(database_path);
        if (expected_minimum == 0U) {
            if (waiting_count == 0U) {
                return waiting_count;
            }
        } else if (waiting_count >= expected_minimum) {
            return waiting_count;
        }
        sleep_microseconds(MYLITE_TEST_WAIT_POLL_INTERVAL_US);
    }
    return read_concurrency_innodb_lock_waiting_count(database_path) +
           read_concurrency_page_write_lock_waiting_count(database_path);
}

static uint64_t read_concurrency_innodb_lock_waiting_count(const char *database_path) {
    return read_concurrency_lock_waiting_count(
        database_path,
        MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SEGMENT_TYPE
    );
}

static uint64_t read_concurrency_page_write_lock_waiting_count(const char *database_path) {
    return read_concurrency_lock_waiting_count(
        database_path,
        MYLITE_TEST_CONCURRENCY_PAGE_WRITE_LOCK_SEGMENT_TYPE
    );
}

static uint64_t read_concurrency_lock_waiting_count(
    const char *database_path,
    uint32_t segment_type
) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    uint64_t lock_offset;
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    lock_offset = read_concurrency_shm_segment_offset(fd, segment_type);
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(lock_offset + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_le64(bytes);
}

static uint64_t read_concurrency_redo_visible_lsn(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    uint64_t redo_state_offset;
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    redo_state_offset =
        read_concurrency_shm_segment_offset(fd, MYLITE_TEST_CONCURRENCY_REDO_STATE_SEGMENT_TYPE);
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(redo_state_offset + MYLITE_TEST_CONCURRENCY_REDO_STATE_VISIBLE_LSN_OFFSET)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_native64(bytes);
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static uint64_t read_concurrency_redo_latest_lsn(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    uint64_t redo_state_offset;
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    redo_state_offset =
        read_concurrency_shm_segment_offset(fd, MYLITE_TEST_CONCURRENCY_REDO_STATE_SEGMENT_TYPE);
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(redo_state_offset + MYLITE_TEST_CONCURRENCY_REDO_STATE_LATEST_LSN_OFFSET)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_native64(bytes);
}

static uint64_t read_concurrency_redo_written_lsn(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    uint64_t redo_state_offset;
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    redo_state_offset =
        read_concurrency_shm_segment_offset(fd, MYLITE_TEST_CONCURRENCY_REDO_STATE_SEGMENT_TYPE);
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(redo_state_offset + MYLITE_TEST_CONCURRENCY_REDO_STATE_WRITTEN_LSN_OFFSET)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_native64(bytes);
}

#endif

static uint64_t read_concurrency_checkpoint_latest_lsn(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *checkpoint_path = path_join(concurrency_path, "mylite-concurrency.ckpt");
    unsigned char bytes[8];
    int fd = open(checkpoint_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    read_exact_at(fd, bytes, sizeof(bytes), MYLITE_TEST_CONCURRENCY_CHECKPOINT_LATEST_LSN_OFFSET);
    assert(close(fd) == 0);
    free(checkpoint_path);
    free(concurrency_path);
    return read_le64(bytes);
}

static uint64_t read_concurrency_checkpoint_visible_lsn(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *checkpoint_path = path_join(concurrency_path, "mylite-concurrency.ckpt");
    unsigned char bytes[8];
    int fd = open(checkpoint_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    read_exact_at(fd, bytes, sizeof(bytes), MYLITE_TEST_CONCURRENCY_CHECKPOINT_VISIBLE_LSN_OFFSET);
    assert(close(fd) == 0);
    free(checkpoint_path);
    free(concurrency_path);
    return read_le64(bytes);
}

static void write_concurrency_checkpoint_visible_lsn(
    const char *database_path,
    uint64_t visible_lsn
) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *checkpoint_path = path_join(concurrency_path, "mylite-concurrency.ckpt");
    unsigned char bytes[8];
    int fd;

    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        bytes[index] = (unsigned char)((visible_lsn >> (index * 8U)) & 0xffU);
    }

    fd = open(checkpoint_path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(
        pwrite(fd, bytes, sizeof(bytes), MYLITE_TEST_CONCURRENCY_CHECKPOINT_VISIBLE_LSN_OFFSET) ==
        (ssize_t)sizeof(bytes)
    );
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
    free(checkpoint_path);
    free(concurrency_path);
}

static uint64_t read_concurrency_page_index_active_count(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    uint64_t page_index_offset;
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    page_index_offset =
        read_concurrency_shm_segment_offset(fd, MYLITE_TEST_CONCURRENCY_PAGE_INDEX_SEGMENT_TYPE);
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(page_index_offset + MYLITE_TEST_CONCURRENCY_PAGE_INDEX_ACTIVE_COUNT_OFFSET)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_native64(bytes);
}

static uint64_t read_concurrency_shm_segment_offset(int fd, uint32_t segment_type) {
    unsigned char bytes[8];
    uint64_t segment_table_offset;
    uint32_t segment_count;

    read_exact_at(fd, bytes, 4U, MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TABLE_OFFSET);
    segment_table_offset = read_le32(bytes);
    read_exact_at(fd, bytes, 4U, MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_COUNT_OFFSET);
    segment_count = read_le32(bytes);

    for (uint32_t index = 0U; index < segment_count; ++index) {
        const off_t descriptor_offset =
            (off_t)(segment_table_offset +
                    (index * MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_DESCRIPTOR_SIZE));
        read_exact_at(
            fd,
            bytes,
            4U,
            descriptor_offset + MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TYPE_OFFSET
        );
        if (read_le32(bytes) != segment_type) {
            continue;
        }
        read_exact_at(
            fd,
            bytes,
            sizeof(bytes),
            descriptor_offset + MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_DATA_OFFSET
        );
        return read_le64(bytes);
    }

    assert(0);
    return 0U;
}

static void read_exact_at(int fd, void *buffer, size_t size, off_t offset) {
    assert(pread(fd, buffer, size, offset) == (ssize_t)size);
}

static uint64_t read_native64(const unsigned char *bytes) {
    uint64_t value;

    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t read_le32(const unsigned char *bytes) {
    uint32_t value = 0U;

    for (size_t index = 0U; index < sizeof(value); ++index) {
        value |= ((uint32_t)bytes[index]) << (index * 8U);
    }
    return value;
}

static uint64_t read_le64(const unsigned char *bytes) {
    uint64_t value = 0U;

    for (size_t index = 0U; index < sizeof(value); ++index) {
        value |= ((uint64_t)bytes[index]) << (index * 8U);
    }
    return value;
}

static void sleep_microseconds(unsigned microseconds) {
    struct timespec remaining = {
        .tv_sec = (time_t)(microseconds / 1000000U),
        .tv_nsec = (long)((microseconds % 1000000U) * 1000U),
    };

    while (nanosleep(&remaining, &remaining) != 0) {
        assert(errno == EINTR);
    }
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-ownerless-sql.XXXXXX";
    char *root = mkdtemp(template_path);

    assert(root != NULL);
    return strdup(root);
}

static char *path_join(const char *directory, const char *name) {
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    char *path = malloc(directory_length + name_length + 2);

    assert(path != NULL);
    assert(sprintf(path, "%s/%s", directory, name) > 0);
    return path;
}

static void signal_pipe_message(int pipe_fd) {
    const char value = 'x';

    assert(write(pipe_fd, &value, sizeof(value)) == sizeof(value));
}

static void wait_for_pipe_message(int pipe_fd) {
    char value = '\0';

    assert(read(pipe_fd, &value, sizeof(value)) == sizeof(value));
    assert(value == 'x');
}

static void signal_pipe(int pipe_fd) {
    const char value = 'x';

    assert(write(pipe_fd, &value, sizeof(value)) == sizeof(value));
    assert(close(pipe_fd) == 0);
}

static void wait_for_pipe(int pipe_fd) {
    char value = '\0';

    assert(read(pipe_fd, &value, sizeof(value)) == sizeof(value));
    assert(value == 'x');
    assert(close(pipe_fd) == 0);
}

static void wait_for_child(pid_t child) {
    int child_status = 0;

    assert(waitpid(child, &child_status, 0) == child);
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(
            stderr,
            "child %ld status=%d exited=%d exit=%d signaled=%d signal=%d\n",
            (long)child,
            child_status,
            WIFEXITED(child_status),
            WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1,
            WIFSIGNALED(child_status),
            WIFSIGNALED(child_status) ? WTERMSIG(child_status) : -1
        );
    }
    assert(WIFEXITED(child_status));
    assert(WEXITSTATUS(child_status) == 0);
}

static void wait_for_signaled_child(pid_t child, int expected_signal) {
    int child_status = 0;

    assert(waitpid(child, &child_status, 0) == child);
    if (!WIFSIGNALED(child_status) || WTERMSIG(child_status) != expected_signal) {
        fprintf(
            stderr,
            "child %ld status=%d signaled=%d signal=%d\n",
            (long)child,
            child_status,
            WIFSIGNALED(child_status),
            WIFSIGNALED(child_status) ? WTERMSIG(child_status) : -1
        );
    }
    assert(WIFSIGNALED(child_status));
    assert(WTERMSIG(child_status) == expected_signal);
}

static int wait_for_child_result(pid_t child) {
    int child_status = 0;

    assert(waitpid(child, &child_status, 0) == child);
    if (WIFEXITED(child_status)) {
        return WEXITSTATUS(child_status);
    }
    return MYLITE_TEST_CHILD_EXEC_FAILED;
}

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void kill_or_reap_child(pid_t child) {
    int child_status = 0;
    pid_t wait_result;

    do {
        wait_result = waitpid(child, &child_status, WNOHANG);
    } while (wait_result < 0 && errno == EINTR);

    if (wait_result == child) {
        return;
    }
    assert(wait_result == 0);
    assert(kill(child, SIGKILL) == 0);
    wait_for_signaled_child(child, SIGKILL);
}

static wait_child_or_pipe_result wait_for_child_result_or_pipe_message(
    pid_t child,
    int pipe_fd,
    unsigned timeout_ms
) {
    const unsigned poll_count = (timeout_ms * 1000U + MYLITE_TEST_WAIT_POLL_INTERVAL_US - 1U) /
                                MYLITE_TEST_WAIT_POLL_INTERVAL_US;
    wait_child_or_pipe_result result = {
        .child_result = MYLITE_TEST_CHILD_EXEC_FAILED,
        .pipe_message = 0,
        .timed_out = 0,
    };
    int pipe_flags = fcntl(pipe_fd, F_GETFL, 0);

    assert(pipe_flags >= 0);
    assert(fcntl(pipe_fd, F_SETFL, pipe_flags | O_NONBLOCK) == 0);

    for (unsigned poll = 0U; poll <= poll_count; ++poll) {
        int child_status = 0;
        pid_t wait_result;

        do {
            wait_result = waitpid(child, &child_status, WNOHANG);
        } while (wait_result < 0 && errno == EINTR);

        if (wait_result == child) {
            if (pipe_fd >= 0) {
                assert(close(pipe_fd) == 0);
            }
            if (WIFEXITED(child_status)) {
                result.child_result = WEXITSTATUS(child_status);
            }
            return result;
        }
        assert(wait_result == 0);

        if (pipe_fd >= 0) {
            char value = '\0';
            const ssize_t bytes_read = read(pipe_fd, &value, sizeof(value));
            if (bytes_read == (ssize_t)sizeof(value)) {
                assert(value == 'x');
                assert(close(pipe_fd) == 0);
                result.pipe_message = 1;
                kill_or_reap_child(child);
                return result;
            }
            if (bytes_read == 0) {
                assert(close(pipe_fd) == 0);
                pipe_fd = -1;
            } else if (bytes_read < 0) {
                assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
            }
        }
        sleep_microseconds(MYLITE_TEST_WAIT_POLL_INTERVAL_US);
    }

    if (pipe_fd >= 0) {
        assert(close(pipe_fd) == 0);
    }
    result.timed_out = 1;
    kill_or_reap_child(child);
    return result;
}
#endif

static int path_exists(const char *path) {
    struct stat path_stat;

    return stat(path, &path_stat) == 0;
}

static void remove_tree(const char *path) {
    if (!path_exists(path)) {
        return;
    }
    assert(
        nftw(path, remove_tree_entry, MYLITE_TEST_REMOVE_TREE_MAX_FDS, FTW_DEPTH | FTW_PHYS) == 0
    );
}

static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
) {
    (void)path_stat;
    (void)walk;

    if (type_flag == FTW_DP || type_flag == FTW_D) {
        return rmdir(path);
    }
    return unlink(path);
}

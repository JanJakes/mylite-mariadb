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
#define MYLITE_TEST_AUTO_INCREMENT_WORKER_COUNT 2U
#define MYLITE_TEST_AUTO_INCREMENT_ROWS_PER_WORKER 12U
#define MYLITE_TEST_PURGE_HISTORY_UPDATES 64U
#define MYLITE_TEST_DDL_WORKER_COUNT 3U
#define MYLITE_TEST_DDL_TABLES_PER_WORKER 4U
#ifndef MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
#  define MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS 0
#endif

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
static void test_ownerless_auto_increment_assigns_distinct_ids(void);
static void test_four_processes_mix_ownerless_reads_and_writes(void);
static void test_ownerless_independent_table_stress(void);
static void test_ownerless_concurrent_ddl_stress(void);
static void test_ownerless_temporary_table_stress(void);
static void test_ownerless_transaction_mix_stress(void);
static void test_ownerless_checksum_stress(void);
static void test_ownerless_random_transaction_stress(void);
static void test_ownerless_purge_preserves_cross_process_snapshot(void);
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
static void test_ownerless_temporary_tablespace_allows_peer_temp_tables(void);
static void test_crashed_ownerless_temporary_table_peer_is_recovered(void);
static void test_ownerless_rejects_non_innodb_engines(void);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void test_crashed_page_publish_rebuilds_ownerless_state(void);
static void test_crashed_checkpoint_rebuilds_ownerless_state(void);
static void test_crashed_visible_publish_without_checkpoint_preserves_committed_update(void);
static void test_crashed_visible_checkpoint_preserves_committed_update(void);
static void test_crashed_redo_reservation_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_redo_written_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_redo_latest_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_redo_latest_checkpoint_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_redo_gap_blocks_later_writer_until_rebuild(void);
static void test_crashed_dictionary_ddl_begin_rebuilds_ownerless_state(void);
static void test_crashed_dictionary_ddl_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void test_crashed_dictionary_ddl_finish_allows_peer_cleanup(void);
#endif
static void test_crashed_ownerless_writer_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void initialize_database(open_database_paths paths);
static void update_first_row_until_released(open_database_paths paths, child_pipes pipes);
static void update_first_row_without_commit_until_killed(open_database_paths paths, int ready_fd);
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
static void insert_auto_increment_rows_after_signal(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
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
static void hold_repeatable_read_snapshot_until_released(
    open_database_paths paths,
    child_pipes pipes
);
static void churn_ownerless_history(open_database_paths paths);
static void update_first_row_by_seven_after_signal(open_database_paths paths, int start_read_fd);
static void hold_select_for_update_until_released(open_database_paths paths, child_pipes pipes);
static void alter_ownerless_sql_expect_lock_timeout(open_database_paths paths);
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
static void hold_ownerless_temporary_table_until_released(
    open_database_paths paths,
    unsigned value,
    child_pipes pipes
);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
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
static mylite_db *open_database_allowing_failure(open_database_paths paths, unsigned flags);
static int open_database_result(open_database_paths paths, unsigned flags, mylite_db **out_db);
static void exec_ok(mylite_db *db, const char *sql);
static int exec_status(mylite_db *db, const char *sql, unsigned *mariadb_errno);
static void expect_exec_error(mylite_db *db, const char *sql);
static void expect_readonly_exec_error(mylite_db *db, const char *sql);
static unsigned long long query_unsigned(mylite_db *db, const char *sql);
static void assert_ownerless_ddl_tables(mylite_db *db);
static void assert_total_value(open_database_paths paths, unsigned long long expected);
static void assert_ownerless_total_value(open_database_paths paths, unsigned long long expected);
static void assert_commit_race_total(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_sum
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
static void assert_ownerless_ddl_stress_state(
    open_database_paths paths,
    unsigned flags,
    unsigned long long expected_total
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
static void assert_ownerless_tx_stress_totals(
    open_database_paths paths,
    unsigned flags,
    unsigned rounds
);
static unsigned ownerless_unsigned_env(
    const char *name,
    unsigned default_value,
    unsigned max_value
);
static void assert_concurrency_wal_has_page_versions_or_checkpoint(const char *database_path);
static void assert_concurrency_page_index_has_entries(const char *database_path);
static int concurrency_wal_is_checkpointed(const char *database_path);
static void remove_concurrency_shm(const char *database_path);
static int capture_first_column(void *ctx, int column_count, char **values, char **columns);
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
static uint64_t read_concurrency_checkpoint_latest_lsn(const char *database_path);
#endif
static uint64_t read_concurrency_checkpoint_visible_lsn(const char *database_path);
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
    if (argc == 2 && strcmp(argv[1], "auto-inc") == 0) {
        test_ownerless_auto_increment_assigns_distinct_ids();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "crash-writer") == 0) {
        test_crashed_ownerless_writer_blocks_peer_cleanup_until_reopen_rebuilds();
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
    if (argc == 2 && strcmp(argv[1], "crash-tail") == 0) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        test_crashed_page_publish_rebuilds_ownerless_state();
        test_crashed_checkpoint_rebuilds_ownerless_state();
        test_crashed_visible_publish_without_checkpoint_preserves_committed_update();
        test_crashed_visible_checkpoint_preserves_committed_update();
        test_crashed_redo_reservation_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_redo_written_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_redo_latest_blocks_peer_cleanup_until_reopen_rebuilds();
        test_crashed_redo_latest_checkpoint_blocks_peer_cleanup_until_reopen_rebuilds();
        test_redo_gap_blocks_later_writer_until_rebuild();
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
            "tx-stress|random-tx-stress|"
            "ddl-refresh|ddl-allocation|ddl-truncate-refresh|ddl-broader|"
            "prepared-committed-read|local-write-first-read|isolation|"
            "shared-readonly|visibility-prefix|different-rows|same-row|different-tables|"
            "commit-race|deadlock-rows|gap-lock|savepoint|serializable|"
            "auto-inc|engine-policy|engine-policy-page-publish|crash-writer|"
            "visible-publish-crash|visible-checkpoint-crash|redo-written-crash|"
            "redo-latest-crash|redo-latest-checkpoint-crash|"
            "redo-gap-blocks-writer|crash-tail]\n",
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
    run_ownerless_sql_test_case(test_ownerless_auto_increment_assigns_distinct_ids);
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
    run_ownerless_sql_test_case(test_rebuild_checkpoints_committed_page_versions);
    run_ownerless_sql_test_case(test_ownerless_alter_waits_for_active_transaction);
    run_ownerless_sql_test_case(test_ownerless_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_large_truncate_refreshes_peer_allocation);
    run_ownerless_sql_test_case(test_ownerless_local_ddl_survives_dictionary_flush);
    run_ownerless_sql_test_case(test_concurrent_ownerless_ddl_allocates_unique_metadata);
    run_ownerless_sql_test_case(test_ownerless_broader_ddl_refreshes_peer_dictionary);
    run_ownerless_sql_test_case(test_ownerless_temporary_tablespace_allows_peer_temp_tables);
    run_ownerless_sql_test_case(test_crashed_ownerless_temporary_table_peer_is_recovered);
    run_ownerless_sql_test_case(test_ownerless_rejects_non_innodb_engines);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
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

    remove_concurrency_shm(database_path);
    assert_commit_race_total(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW, expected_sum);
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
    assert(!concurrency_wal_is_checkpointed(database_path));
    assert_concurrency_page_index_has_entries(database_path);
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
            "SELECT COUNT(*) FROM app.ownerless_like "
            "WHERE id = 1 AND value = 42 AND status = 'done'"
        ) == 1U
    );

    signal_pipe_message(ddl_release_pipe[1]);
    wait_for_pipe_message(ddl_ready_pipe[0]);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.ownerless_ctas "
            "WHERE id = 1 AND value = 42 AND status = 'done'"
        ) == 1U
    );

    assert(mylite_close(db) == MYLITE_OK);
    close(ddl_ready_pipe[0]);
    close(ddl_release_pipe[1]);
    wait_for_child(ddl_child);

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
    exec_ok(db, "CREATE TABLE app.ownerless_like LIKE app.ownerless_online");
    exec_ok(db, "INSERT INTO app.ownerless_like SELECT * FROM app.ownerless_online");
    signal_pipe_message(pipes.ready_write_fd);

    wait_for_pipe_message(pipes.release_read_fd);
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_ctas ENGINE=InnoDB AS "
        "SELECT id, value, status FROM app.ownerless_like"
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

#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
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
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = paths.runtime_root,
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

static int concurrency_wal_is_checkpointed(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *wal_path = path_join(concurrency_path, "mylite-concurrency.wal");
    const off_t empty_log_end =
        MYLITE_TEST_CONCURRENCY_RECOVERY_HEADER_SIZE + MYLITE_TEST_PAGE_LOG_HEADER_SIZE;
    struct stat wal_stat;

    assert(stat(wal_path, &wal_stat) == 0);
    free(wal_path);
    free(concurrency_path);
    return wal_stat.st_size == empty_log_end;
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
#endif

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

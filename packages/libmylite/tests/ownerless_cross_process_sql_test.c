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
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET 64
#define MYLITE_TEST_CONCURRENCY_REDO_STATE_SEGMENT_TYPE 7U
#define MYLITE_TEST_CONCURRENCY_REDO_STATE_VISIBLE_LSN_OFFSET 40
#define MYLITE_TEST_CONCURRENCY_PAGE_INDEX_SEGMENT_TYPE 8U
#define MYLITE_TEST_CONCURRENCY_PAGE_INDEX_ACTIVE_COUNT_OFFSET 40
#define MYLITE_TEST_CONCURRENCY_RECOVERY_HEADER_SIZE 128
#define MYLITE_TEST_CONCURRENCY_CHECKPOINT_VISIBLE_LSN_OFFSET 136
#define MYLITE_TEST_PAGE_LOG_HEADER_SIZE 64
#define MYLITE_TEST_PAGE_LOG_RECORD_HEADER_SIZE 64
#define MYLITE_TEST_STRESS_WRITER_COUNT 4U
#define MYLITE_TEST_STRESS_ITERATIONS 24U
#define MYLITE_TEST_STRESS_READER_POLLS 48U
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

static void test_two_processes_update_different_innodb_rows(void);
static void test_two_processes_update_same_innodb_row(void);
static void test_two_processes_update_different_innodb_tables(void);
static void test_two_processes_deadlock_on_innodb_rows(void);
static void test_four_processes_mix_ownerless_reads_and_writes(void);
static void test_ownerless_independent_table_stress(void);
static void test_ownerless_purge_preserves_cross_process_snapshot(void);
static void test_process_reads_committed_external_update(void);
static void test_shared_readonly_process_reads_committed_external_update(void);
static void test_process_checkpoints_committed_page_versions(void);
static void test_ownerless_alter_waits_for_active_transaction(void);
static void test_ownerless_ddl_refreshes_peer_dictionary(void);
static void test_concurrent_ownerless_ddl_allocates_unique_metadata(void);
static void test_ownerless_rejects_non_innodb_engines(void);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void test_crashed_page_publish_rebuilds_ownerless_state(void);
static void test_crashed_checkpoint_rebuilds_ownerless_state(void);
static void test_crashed_redo_reservation_blocks_peer_cleanup_until_reopen_rebuilds(void);
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
static void update_table_pair_after_signal(
    open_database_paths paths,
    const char *first_table,
    const char *second_table,
    unsigned increment,
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
static void create_ownerless_ddl_tables_after_signal(
    open_database_paths paths,
    unsigned worker_id,
    child_pipes pipes
);
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
static void update_first_row_until_page_publish_fault(open_database_paths paths, int ready_fd);
static void insert_checkpoint_rows_until_fault(open_database_paths paths, int ready_fd);
static void update_first_row_until_redo_reserve_fault(open_database_paths paths, int ready_fd);
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
static void assert_concurrency_wal_has_page_versions_or_checkpoint(const char *database_path);
static void assert_concurrency_page_index_has_entries(const char *database_path);
static int concurrency_wal_is_checkpointed(const char *database_path);
static void remove_concurrency_shm(const char *database_path);
static int capture_first_column(void *ctx, int column_count, char **values, char **columns);
static uint64_t wait_for_concurrency_innodb_lock_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
);
static uint64_t read_concurrency_innodb_lock_waiting_count(const char *database_path);
static uint64_t read_concurrency_redo_visible_lsn(const char *database_path);
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

int main(void) {
    test_two_processes_update_different_innodb_rows();
    test_two_processes_update_same_innodb_row();
    test_two_processes_update_different_innodb_tables();
    test_two_processes_deadlock_on_innodb_rows();
    test_four_processes_mix_ownerless_reads_and_writes();
    test_ownerless_independent_table_stress();
    test_ownerless_purge_preserves_cross_process_snapshot();
    test_process_reads_committed_external_update();
    test_shared_readonly_process_reads_committed_external_update();
    test_process_checkpoints_committed_page_versions();
    test_ownerless_alter_waits_for_active_transaction();
    test_ownerless_ddl_refreshes_peer_dictionary();
    test_concurrent_ownerless_ddl_allocates_unique_metadata();
    test_ownerless_rejects_non_innodb_engines();
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    test_crashed_page_publish_rebuilds_ownerless_state();
    test_crashed_checkpoint_rebuilds_ownerless_state();
    test_crashed_redo_reservation_blocks_peer_cleanup_until_reopen_rebuilds();
#endif
    test_crashed_ownerless_writer_blocks_peer_cleanup_until_reopen_rebuilds();
    return 0;
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

    assert(wait_for_concurrency_innodb_lock_waiting_count(database_path, 1U, 5000U) >= 1U);
    signal_pipe(release_pipe[1]);
    wait_for_child(first_child);
    wait_for_child(second_child);
    assert(wait_for_concurrency_innodb_lock_waiting_count(database_path, 0U, 5000U) == 0U);
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
    assert(wait_for_concurrency_innodb_lock_waiting_count(database_path, 0U, 5000U) == 0U);
    assert_table_total_value_is_one_of(paths, 302U, 304U);

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

    assert_ownerless_stress_total(
        paths,
        MYLITE_TEST_STRESS_WRITER_COUNT * MYLITE_TEST_STRESS_ITERATIONS
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

static void test_shared_readonly_process_reads_committed_external_update(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-shared-readonly.mylite");
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
    reader = open_database(paths, MYLITE_OPEN_READONLY | MYLITE_OPEN_SHARED_READONLY);
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 10U);
    expect_readonly_exec_error(reader, "UPDATE app.ownerless_sql SET value = 11 WHERE id = 1");
    signal_pipe(start_pipe[1]);
    wait_for_child(writer_child);

    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 17U);
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_process_checkpoints_committed_page_versions(void) {
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
        "CREATE TABLE app.ownerless_after_peer_ddl ("
        "id INT NOT NULL PRIMARY KEY"
        ") ENGINE=InnoDB"
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
    int ready_pipe[2];
    pid_t writer_child;
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
    assert(mylite_close(db) == MYLITE_OK);
    assert(pipe(ready_pipe) == 0);

    writer_child = fork();
    assert(writer_child >= 0);
    if (writer_child == 0) {
        close(ready_pipe[0]);
        insert_checkpoint_rows_until_fault(paths, ready_pipe[1]);
    }

    close(ready_pipe[1]);
    wait_for_pipe(ready_pipe[0]);
    assert(kill(writer_child, SIGKILL) == 0);
    wait_for_signaled_child(writer_child, SIGKILL);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    row_count = query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_checkpoint_crash");
    assert(row_count > 0U);
    assert(row_count <= 64U);
    assert(mylite_close(db) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    row_count = query_unsigned(db, "SELECT COUNT(*) FROM app.ownerless_checkpoint_crash");
    assert(row_count > 0U);
    assert(row_count <= 64U);
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
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 3");
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
    for (unsigned iteration = 1U; iteration <= MYLITE_TEST_STRESS_ITERATIONS; ++iteration) {
        exec_ok(db, update_sql);
        if (iteration % 6U == 0U) {
            assert(query_unsigned(db, select_sql) == iteration);
        }
    }
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void run_ownerless_stress_reader(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;
    unsigned long long previous_total = 0U;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 30");
    exec_ok(db, "SET SESSION lock_wait_timeout = 30");
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    for (unsigned iteration = 0U; iteration < MYLITE_TEST_STRESS_READER_POLLS; ++iteration) {
        const unsigned long long value = query_unsigned(
            db,
            "SELECT "
            "(SELECT value FROM app.ownerless_stress_1 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_2 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_3 WHERE id = 1) + "
            "(SELECT value FROM app.ownerless_stress_4 WHERE id = 1)"
        );
        assert(value >= previous_total);
        assert(value <= MYLITE_TEST_STRESS_WRITER_COUNT * MYLITE_TEST_STRESS_ITERATIONS);
        previous_total = value;
        sleep_microseconds(1000U);
    }
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
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
    }

    assert(close(pipes.ready_write_fd) == 0);
    assert(close(pipes.release_read_fd) == 0);
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

static void insert_checkpoint_rows_until_fault(open_database_paths paths, int ready_fd) {
    mylite_db *db;
    char ready_fd_value[32];
    char insert_sql[168];

    assert(snprintf(ready_fd_value, sizeof(ready_fd_value), "%d", ready_fd) > 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT", "checkpoint-before-truncate", 1) == 0);
    assert(setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd_value, 1) == 0);
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

static uint64_t wait_for_concurrency_innodb_lock_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
) {
    const unsigned iterations = timeout_ms * 1000U / MYLITE_TEST_WAIT_POLL_INTERVAL_US;

    for (unsigned iteration = 0U; iteration <= iterations; ++iteration) {
        const uint64_t waiting_count = read_concurrency_innodb_lock_waiting_count(database_path);
        if (expected_minimum == 0U) {
            if (waiting_count == 0U) {
                return waiting_count;
            }
        } else if (waiting_count >= expected_minimum) {
            return waiting_count;
        }
        sleep_microseconds(MYLITE_TEST_WAIT_POLL_INTERVAL_US);
    }
    return read_concurrency_innodb_lock_waiting_count(database_path);
}

static uint64_t read_concurrency_innodb_lock_waiting_count(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    uint64_t innodb_lock_offset;
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    innodb_lock_offset =
        read_concurrency_shm_segment_offset(fd, MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SEGMENT_TYPE);
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(innodb_lock_offset + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET)
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

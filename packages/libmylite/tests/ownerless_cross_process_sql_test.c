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
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TABLE_OFFSET 56
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_COUNT_OFFSET 60
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_DESCRIPTOR_SIZE 32
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TYPE_OFFSET 0
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_DATA_OFFSET 8
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SEGMENT_TYPE 6U
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET 64
#define MYLITE_TEST_CONCURRENCY_PAGE_INDEX_SEGMENT_TYPE 8U
#define MYLITE_TEST_CONCURRENCY_PAGE_INDEX_ACTIVE_COUNT_OFFSET 40
#define MYLITE_TEST_CONCURRENCY_RECOVERY_HEADER_SIZE 128
#define MYLITE_TEST_PAGE_LOG_HEADER_SIZE 64
#define MYLITE_TEST_PAGE_LOG_RECORD_HEADER_SIZE 64

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
static void test_process_reads_committed_external_update(void);
static void test_crashed_ownerless_writer_blocks_peer_cleanup_until_reopen_rebuilds(void);
static void initialize_database(open_database_paths paths);
static void update_first_row_until_released(open_database_paths paths, child_pipes pipes);
static void update_first_row_without_commit_until_killed(open_database_paths paths, int ready_fd);
static void hold_ownerless_open_until_released(open_database_paths paths, child_pipes pipes);
static void assert_ownerless_open_returns_busy(open_database_paths paths);
static void update_second_row(open_database_paths paths);
static void update_first_row_by_two(open_database_paths paths);
static void update_first_table_until_released(open_database_paths paths, child_pipes pipes);
static void update_second_table(open_database_paths paths);
static void update_row_pair_after_signal(
    open_database_paths paths,
    unsigned first_id,
    unsigned second_id,
    child_pipes pipes
);
static void update_first_row_by_seven_after_signal(open_database_paths paths, int start_read_fd);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static mylite_db *open_database_allowing_failure(open_database_paths paths, unsigned flags);
static int open_database_result(open_database_paths paths, unsigned flags, mylite_db **out_db);
static void exec_ok(mylite_db *db, const char *sql);
static int exec_status(mylite_db *db, const char *sql, unsigned *mariadb_errno);
static unsigned long long query_unsigned(mylite_db *db, const char *sql);
static void assert_total_value(open_database_paths paths, unsigned long long expected);
static void assert_total_value_is_one_of(
    open_database_paths paths,
    unsigned long long first_expected,
    unsigned long long second_expected
);
static void assert_table_values(open_database_paths paths);
static void assert_concurrency_wal_has_page_versions(const char *database_path);
static void assert_concurrency_page_index_has_entries(const char *database_path);
static void remove_concurrency_shm(const char *database_path);
static int capture_first_column(void *ctx, int column_count, char **values, char **columns);
static uint64_t wait_for_concurrency_innodb_lock_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
);
static uint64_t read_concurrency_innodb_lock_waiting_count(const char *database_path);
static uint64_t read_concurrency_page_index_active_count(const char *database_path);
static uint64_t read_concurrency_shm_segment_offset(int fd, uint32_t segment_type);
static void read_exact_at(int fd, void *buffer, size_t size, off_t offset);
static uint64_t read_native64(const unsigned char *bytes);
static uint32_t read_le32(const unsigned char *bytes);
static uint64_t read_le64(const unsigned char *bytes);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
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
    test_process_reads_committed_external_update();
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
        update_first_table_until_released(
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
        update_second_table(paths);
    }

    wait_for_child(second_child);
    signal_pipe(release_pipe[1]);
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
        update_row_pair_after_signal(
            paths,
            1U,
            2U,
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
        update_row_pair_after_signal(
            paths,
            2U,
            1U,
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
    assert(
        (first_result == MYLITE_TEST_CHILD_OK &&
         second_result == MYLITE_TEST_CHILD_DEADLOCK) ||
        (first_result == MYLITE_TEST_CHILD_DEADLOCK &&
         second_result == MYLITE_TEST_CHILD_OK)
    );
    assert(wait_for_concurrency_innodb_lock_waiting_count(database_path, 0U, 5000U) == 0U);
    assert_total_value_is_one_of(paths, 32U, 34U);

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
    assert_concurrency_wal_has_page_versions(database_path);
    assert_concurrency_page_index_has_entries(database_path);
    assert(mylite_close(reader) == MYLITE_OK);

    remove_concurrency_shm(database_path);
    reader = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert_concurrency_page_index_has_entries(database_path);
    assert(query_unsigned(reader, "SELECT value FROM app.ownerless_sql WHERE id = 1") == 17U);
    assert(mylite_close(reader) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

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
    assert_total_value(paths, 30U);

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
    exec_ok(db, "CREATE TABLE app.ownerless_a (id INT NOT NULL PRIMARY KEY, value INT NOT NULL) ENGINE=InnoDB");
    exec_ok(db, "CREATE TABLE app.ownerless_b (id INT NOT NULL PRIMARY KEY, value INT NOT NULL) ENGINE=InnoDB");
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

static void update_second_table(open_database_paths paths) {
    mylite_db *db;

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_b SET value = value + 2 WHERE id = 1");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

static void update_row_pair_after_signal(
    open_database_paths paths,
    unsigned first_id,
    unsigned second_id,
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
            "update_row_pair_after_signal open returned NULL: pid=%ld first=%u second=%u\n",
            (long)getpid(),
            first_id,
            second_id
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
            "UPDATE app.ownerless_sql SET value = value + %u WHERE id = %u",
            first_id,
            first_id
        ) > 0
    );
    assert(
        snprintf(
            second_update,
            sizeof(second_update),
            "UPDATE app.ownerless_sql SET value = value + %u WHERE id = %u",
            first_id,
            second_id
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

static void update_first_row_by_seven_after_signal(open_database_paths paths, int start_read_fd) {
    mylite_db *db;

    wait_for_pipe(start_read_fd);
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "UPDATE app.ownerless_sql SET value = value + 7 WHERE id = 1");
    assert(mylite_close(db) == MYLITE_OK);
    _exit(0);
}

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
            "mylite_exec failed: sql=%s errcode=%d mariadb_errno=%u message=%s\n",
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

static unsigned long long query_unsigned(mylite_db *db, const char *sql) {
    query_result result = {0U};
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, capture_first_column, &result, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    return result.value;
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
        fprintf(
            stderr,
            "expected total value %llu, got %llu\n",
            expected,
            result.value
        );
    }
    assert(result.value == expected);
    assert(mylite_close(db) == MYLITE_OK);
}

static void assert_total_value_is_one_of(
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

static void assert_concurrency_wal_has_page_versions(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *wal_path = path_join(concurrency_path, "mylite-concurrency.wal");
    struct stat wal_stat;
    const off_t first_record_end =
        MYLITE_TEST_CONCURRENCY_RECOVERY_HEADER_SIZE +
        MYLITE_TEST_PAGE_LOG_HEADER_SIZE +
        MYLITE_TEST_PAGE_LOG_RECORD_HEADER_SIZE;

    assert(stat(wal_path, &wal_stat) == 0);
    if (wal_stat.st_size <= first_record_end) {
        fprintf(
            stderr,
            "expected ownerless WAL page records, got size %lld\n",
            (long long)wal_stat.st_size
        );
    }
    assert(wal_stat.st_size > first_record_end);

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

static void remove_concurrency_shm(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");

    assert(unlink(shm_path) == 0);
    free(shm_path);
    free(concurrency_path);
}

static int capture_first_column(void *ctx, int column_count, char **values, char **columns) {
    query_result *result = ctx;

    (void)columns;
    assert(column_count == 1);
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
        const uint64_t waiting_count =
            read_concurrency_innodb_lock_waiting_count(database_path);
        if (expected_minimum == 0U) {
            if (waiting_count == 0U) {
                return waiting_count;
            }
        } else if (waiting_count >= expected_minimum) {
            return waiting_count;
        }
        usleep(MYLITE_TEST_WAIT_POLL_INTERVAL_US);
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
        const off_t descriptor_offset = (off_t)(
            segment_table_offset + (index * MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_DESCRIPTOR_SIZE)
        );
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
        fprintf(stderr, "child %ld status=%d exited=%d exit=%d signaled=%d signal=%d\n",
                (long)child,
                child_status,
                WIFEXITED(child_status),
                WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1,
                WIFSIGNALED(child_status),
                WIFSIGNALED(child_status) ? WTERMSIG(child_status) : -1);
    }
    assert(WIFEXITED(child_status));
    assert(WEXITSTATUS(child_status) == 0);
}

static void wait_for_signaled_child(pid_t child, int expected_signal) {
    int child_status = 0;

    assert(waitpid(child, &child_status, 0) == child);
    if (!WIFSIGNALED(child_status) || WTERMSIG(child_status) != expected_signal) {
        fprintf(stderr, "child %ld status=%d signaled=%d signal=%d\n",
                (long)child,
                child_status,
                WIFSIGNALED(child_status),
                WIFSIGNALED(child_status) ? WTERMSIG(child_status) : -1);
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

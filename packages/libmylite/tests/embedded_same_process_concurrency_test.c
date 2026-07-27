#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TABLE_OFFSET 56
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_COUNT_OFFSET 60
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_DESCRIPTOR_SIZE 32
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TYPE_OFFSET 0
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_DATA_OFFSET 8
#define MYLITE_TEST_CONCURRENCY_MDL_SEGMENT_TYPE 3U
#define MYLITE_TEST_CONCURRENCY_MDL_ACTIVE_COUNT_OFFSET 16
#define MYLITE_TEST_CONCURRENCY_MDL_WAITING_COUNT_OFFSET 56
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SEGMENT_TYPE 6U
#define MYLITE_TEST_CONCURRENCY_PAGE_WRITE_LOCK_SEGMENT_TYPE 10U
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET 64
#define MYLITE_TEST_WAIT_POLL_INTERVAL_US 10000U

typedef struct expected_query {
    const char *sql;
    int column_count;
    int row_count;
    const char *const *column_names;
    const char *const *values;
} expected_query;

typedef struct expected_result {
    int column_count;
    int row_count;
    const char *const *column_names;
    const char *const *values;
    int seen_rows;
} expected_result;

typedef struct mdl_lock_counts {
    uint64_t active;
    uint64_t waiting;
} mdl_lock_counts;

typedef struct open_database_paths {
    const char *database_path;
    const char *runtime_root;
} open_database_paths;

typedef struct exec_thread_args {
    open_database_paths paths;
    const char *sql;
    int result;
    unsigned mariadb_errno;
    int close_result;
} exec_thread_args;

typedef struct overlap_barrier {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned arrived;
} overlap_barrier;

typedef struct overlap_thread_args {
    open_database_paths paths;
    overlap_barrier *barrier;
    int row_id;
    int update_result;
    unsigned mariadb_errno;
    int transaction_result;
    int close_result;
} overlap_thread_args;

static void test_committed_rows_are_visible_across_handles(void);
static void test_active_transactions_can_write_different_rows(void);
static void test_ownerless_different_leaf_pages_overlap(void);
static void test_lock_wait_timeout_between_handles(void);
static void test_innodb_wait_registry_tracks_local_waits(void);
static void test_metadata_lock_timeout_between_handles(void);
static void test_ownerless_metadata_lock_timeout_between_handles(void);
static void test_savepoints_and_foreign_keys_across_handles(void);
static void create_database_schema(mylite_db *db);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void exec_ok(mylite_db *db, const char *sql);
static void expect_exec_error(mylite_db *db, const char *sql, unsigned mariadb_errno);
static void *execute_sql_in_thread(void *ctx);
static void *execute_overlapping_update_in_thread(void *ctx);
static void populate_wide_items(mylite_db *db, unsigned rows);
static uint64_t wait_for_ownerless_write_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
);
static void sleep_microseconds(unsigned microseconds);
static uint64_t read_ownerless_write_waiting_count(const char *database_path);
static uint64_t read_lock_waiting_count(const char *database_path, uint32_t segment_type);
static mdl_lock_counts read_mdl_lock_counts(const char *database_path);
static uint64_t read_concurrency_shm_segment_offset(int fd, uint32_t segment_type);
static void read_exact_at(int fd, void *buffer, size_t size, off_t offset);
static uint64_t monotonic_milliseconds(void);
static void query_expect(mylite_db *db, expected_query query);
static int expected_result_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static int is_directory_empty(const char *path);
static int path_exists(const char *path);
static uint32_t read_le32(const unsigned char *bytes);
static uint64_t read_le64(const unsigned char *bytes);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

int main(void) {
    test_committed_rows_are_visible_across_handles();
    test_active_transactions_can_write_different_rows();
    test_ownerless_different_leaf_pages_overlap();
    test_lock_wait_timeout_between_handles();
    test_innodb_wait_registry_tracks_local_waits();
    test_metadata_lock_timeout_between_handles();
    test_ownerless_metadata_lock_timeout_between_handles();
    test_savepoints_and_foreign_keys_across_handles();
    return 0;
}

static void test_committed_rows_are_visible_across_handles(void) {
    static const char *const columns[] = {"row_count", "value_sum"};
    static const char *const initial_values[] = {"2", "30"};
    static const char *const final_values[] = {"3", "60"};
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "committed-rows.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *first = NULL;
    mylite_db *second = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    first = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    create_database_schema(first);
    exec_ok(first, "INSERT INTO app.items VALUES (1, 10), (2, 20)");

    second = open_database(paths, MYLITE_OPEN_READWRITE);
    query_expect(
        second,
        (expected_query){
            .sql = "SELECT COUNT(*) AS row_count, SUM(value) AS value_sum FROM app.items",
            .column_count = 2,
            .row_count = 1,
            .column_names = columns,
            .values = initial_values,
        }
    );

    exec_ok(second, "INSERT INTO app.items VALUES (3, 30)");
    query_expect(
        first,
        (expected_query){
            .sql = "SELECT COUNT(*) AS row_count, SUM(value) AS value_sum FROM app.items",
            .column_count = 2,
            .row_count = 1,
            .column_names = columns,
            .values = final_values,
        }
    );

    assert(mylite_close(second) == MYLITE_OK);
    assert(mylite_close(first) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_active_transactions_can_write_different_rows(void) {
    static const char *const columns[] = {"id", "value"};
    static const char *const values[] = {"1", "15", "2", "35"};
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "different-rows.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *first = NULL;
    mylite_db *second = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    first = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    create_database_schema(first);
    exec_ok(first, "INSERT INTO app.items VALUES (1, 10), (2, 20)");
    second = open_database(paths, MYLITE_OPEN_READWRITE);

    exec_ok(first, "START TRANSACTION");
    exec_ok(second, "START TRANSACTION");
    exec_ok(first, "UPDATE app.items SET value = value + 5 WHERE id = 1");
    exec_ok(second, "UPDATE app.items SET value = value + 15 WHERE id = 2");
    exec_ok(second, "COMMIT");
    exec_ok(first, "COMMIT");

    query_expect(
        second,
        (expected_query){
            .sql = "SELECT id, value FROM app.items ORDER BY id",
            .column_count = 2,
            .row_count = 2,
            .column_names = columns,
            .values = values,
        }
    );

    assert(mylite_close(second) == MYLITE_OK);
    assert(mylite_close(first) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_different_leaf_pages_overlap(void) {
    static const char *const columns[] = {"id", "value"};
    static const char *const values[] = {"1", "1", "400", "1"};
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "different-leaf-overlap.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db = NULL;
    pthread_t first_thread;
    pthread_t second_thread;
    overlap_barrier barrier = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .arrived = 0U,
    };
    overlap_thread_args first = {
        .paths = paths,
        .barrier = &barrier,
        .row_id = 1,
        .update_result = MYLITE_ERROR,
        .mariadb_errno = 0U,
        .transaction_result = MYLITE_ERROR,
        .close_result = MYLITE_ERROR,
    };
    overlap_thread_args second = {
        .paths = paths,
        .barrier = &barrier,
        .row_id = 400,
        .update_result = MYLITE_ERROR,
        .mariadb_errno = 0U,
        .transaction_result = MYLITE_ERROR,
        .close_result = MYLITE_ERROR,
    };

    assert(mkdir(runtime_root, 0700) == 0);
    db =
        open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.wide_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL, "
        "padding VARCHAR(3000) NOT NULL"
        ") ENGINE=InnoDB ROW_FORMAT=COMPACT"
    );
    populate_wide_items(db, 400U);

    assert(pthread_create(&first_thread, NULL, execute_overlapping_update_in_thread, &first) == 0);
    assert(
        pthread_create(&second_thread, NULL, execute_overlapping_update_in_thread, &second) == 0
    );
    assert(pthread_join(first_thread, NULL) == 0);
    assert(pthread_join(second_thread, NULL) == 0);

    assert(first.update_result == MYLITE_OK);
    assert(first.mariadb_errno == 0U);
    assert(first.transaction_result == MYLITE_OK);
    assert(first.close_result == MYLITE_OK);
    assert(second.update_result == MYLITE_OK);
    assert(second.mariadb_errno == 0U);
    assert(second.transaction_result == MYLITE_OK);
    assert(second.close_result == MYLITE_OK);
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, value FROM app.wide_items WHERE id IN (1, 400) ORDER BY id",
            .column_count = 2,
            .row_count = 2,
            .column_names = columns,
            .values = values,
        }
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert(pthread_cond_destroy(&barrier.condition) == 0);
    assert(pthread_mutex_destroy(&barrier.mutex) == 0);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_lock_wait_timeout_between_handles(void) {
    static const char *const columns[] = {"value"};
    static const char *const old_value[] = {"10"};
    static const char *const final_value[] = {"11"};
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "lock-wait.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *first = NULL;
    mylite_db *second = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    first = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    create_database_schema(first);
    exec_ok(first, "INSERT INTO app.items VALUES (1, 10)");
    second = open_database(paths, MYLITE_OPEN_READWRITE);

    exec_ok(second, "SET SESSION innodb_lock_wait_timeout = 1");
    exec_ok(first, "START TRANSACTION");
    exec_ok(first, "UPDATE app.items SET value = value + 100 WHERE id = 1");
    query_expect(
        second,
        (expected_query){
            .sql = "SELECT value FROM app.items WHERE id = 1",
            .column_count = 1,
            .row_count = 1,
            .column_names = columns,
            .values = old_value,
        }
    );
    expect_exec_error(second, "UPDATE app.items SET value = value + 1 WHERE id = 1", 1205U);
    exec_ok(first, "ROLLBACK");
    exec_ok(second, "UPDATE app.items SET value = value + 1 WHERE id = 1");
    query_expect(
        first,
        (expected_query){
            .sql = "SELECT value FROM app.items WHERE id = 1",
            .column_count = 1,
            .row_count = 1,
            .column_names = columns,
            .values = final_value,
        }
    );

    assert(mylite_close(second) == MYLITE_OK);
    assert(mylite_close(first) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_innodb_wait_registry_tracks_local_waits(void) {
    static const char *const columns[] = {"value"};
    static const char *const values[] = {"11"};
    static const char *const ordinary_reopen_values[] = {"12"};
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "innodb-wait-registry.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *first = NULL;
    pthread_t update_thread;
    exec_thread_args args = {
        .paths = paths,
        .sql = "UPDATE app.items SET value = value + 1 WHERE id = 1",
        .result = MYLITE_ERROR,
        .mariadb_errno = 0U,
        .close_result = MYLITE_ERROR,
    };

    assert(mkdir(runtime_root, 0700) == 0);
    first =
        open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE | MYLITE_OPEN_OWNERLESS_RW);
    create_database_schema(first);
    exec_ok(first, "INSERT INTO app.items VALUES (1, 10)");

    exec_ok(first, "START TRANSACTION");
    exec_ok(first, "UPDATE app.items SET value = value + 100 WHERE id = 1");
    assert(pthread_create(&update_thread, NULL, execute_sql_in_thread, &args) == 0);
    assert(wait_for_ownerless_write_waiting_count(database_path, 1U, 5000U) >= 1U);

    exec_ok(first, "ROLLBACK");
    assert(pthread_join(update_thread, NULL) == 0);
    assert(args.result == MYLITE_OK);
    assert(args.mariadb_errno == 0U);
    assert(args.close_result == MYLITE_OK);
    assert(wait_for_ownerless_write_waiting_count(database_path, 0U, 5000U) == 0U);
    query_expect(
        first,
        (expected_query){
            .sql = "SELECT value FROM app.items WHERE id = 1",
            .column_count = 1,
            .row_count = 1,
            .column_names = columns,
            .values = values,
        }
    );

    assert(mylite_close(first) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));
    first = open_database(paths, MYLITE_OPEN_READWRITE);
    exec_ok(first, "UPDATE app.items SET value = value + 1 WHERE id = 1");
    query_expect(
        first,
        (expected_query){
            .sql = "SELECT value FROM app.items WHERE id = 1",
            .column_count = 1,
            .row_count = 1,
            .column_names = columns,
            .values = ordinary_reopen_values,
        }
    );
    assert(mylite_close(first) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_metadata_lock_timeout_between_handles(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "metadata-lock.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *first = NULL;
    mylite_db *second = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    first = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    create_database_schema(first);
    exec_ok(first, "INSERT INTO app.items VALUES (1, 10)");
    second = open_database(paths, MYLITE_OPEN_READWRITE);

    exec_ok(second, "SET SESSION lock_wait_timeout = 1");
    exec_ok(first, "START TRANSACTION");
    exec_ok(first, "SELECT * FROM app.items WHERE id = 1 FOR UPDATE");
    expect_exec_error(second, "ALTER TABLE app.items ADD COLUMN note VARCHAR(32)", 1205U);
    exec_ok(first, "ROLLBACK");
    exec_ok(second, "ALTER TABLE app.items ADD COLUMN note VARCHAR(32)");
    exec_ok(second, "UPDATE app.items SET note = 'ok' WHERE id = 1");

    assert(mylite_close(second) == MYLITE_OK);
    assert(mylite_close(first) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_metadata_lock_timeout_between_handles(void) {
    static const char *const columns[] = {"value", "note"};
    static const char *const ownerless_values[] = {"11", "ownerless"};
    static const char *const native_values[] = {"12", "native"};
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-metadata-lock.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *first = NULL;
    mylite_db *second = NULL;
    mdl_lock_counts counts;
    uint64_t start_ms;
    uint64_t elapsed_ms;

    assert(mkdir(runtime_root, 0700) == 0);
    first =
        open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE | MYLITE_OPEN_OWNERLESS_RW);
    create_database_schema(first);
    exec_ok(first, "INSERT INTO app.items VALUES (1, 10)");
    second = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);

    counts = read_mdl_lock_counts(database_path);
    assert(counts.active == 0U);
    assert(counts.waiting == 0U);
    exec_ok(second, "SET SESSION lock_wait_timeout = 1");
    exec_ok(first, "START TRANSACTION");
    exec_ok(first, "SELECT * FROM app.items WHERE id = 1 FOR UPDATE");
    counts = read_mdl_lock_counts(database_path);
    assert(counts.active > 0U);
    assert(counts.waiting == 0U);

    start_ms = monotonic_milliseconds();
    expect_exec_error(second, "ALTER TABLE app.items ADD COLUMN note VARCHAR(32)", 1205U);
    elapsed_ms = monotonic_milliseconds() - start_ms;
    assert(elapsed_ms >= 500U);
    assert(elapsed_ms <= 5000U);
    counts = read_mdl_lock_counts(database_path);
    assert(counts.active > 0U);
    assert(counts.waiting == 0U);

    exec_ok(first, "ROLLBACK");
    counts = read_mdl_lock_counts(database_path);
    assert(counts.active == 0U);
    assert(counts.waiting == 0U);
    exec_ok(second, "ALTER TABLE app.items ADD COLUMN note VARCHAR(32)");
    exec_ok(second, "UPDATE app.items SET value = value + 1, note = 'ownerless' WHERE id = 1");
    query_expect(
        first,
        (expected_query){
            .sql = "SELECT value, note FROM app.items WHERE id = 1",
            .column_count = 2,
            .row_count = 1,
            .column_names = columns,
            .values = ownerless_values,
        }
    );
    query_expect(
        second,
        (expected_query){
            .sql = "SELECT value, note FROM app.items WHERE id = 1",
            .column_count = 2,
            .row_count = 1,
            .column_names = columns,
            .values = ownerless_values,
        }
    );
    counts = read_mdl_lock_counts(database_path);
    assert(counts.active == 0U);
    assert(counts.waiting == 0U);

    assert(mylite_close(second) == MYLITE_OK);
    counts = read_mdl_lock_counts(database_path);
    assert(counts.active == 0U);
    assert(counts.waiting == 0U);
    assert(mylite_close(first) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    first = open_database(paths, MYLITE_OPEN_READWRITE);
    query_expect(
        first,
        (expected_query){
            .sql = "SELECT value, note FROM app.items WHERE id = 1",
            .column_count = 2,
            .row_count = 1,
            .column_names = columns,
            .values = ownerless_values,
        }
    );
    exec_ok(first, "UPDATE app.items SET value = value + 1, note = 'native' WHERE id = 1");
    query_expect(
        first,
        (expected_query){
            .sql = "SELECT value, note FROM app.items WHERE id = 1",
            .column_count = 2,
            .row_count = 1,
            .column_names = columns,
            .values = native_values,
        }
    );
    assert(mylite_close(first) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_savepoints_and_foreign_keys_across_handles(void) {
    static const char *const columns[] = {"parent_count", "child_count"};
    static const char *const after_rollback_values[] = {"1", "0"};
    static const char *const final_values[] = {"1", "1"};
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "savepoint-fk.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *first = NULL;
    mylite_db *second = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    first = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    exec_ok(first, "CREATE DATABASE app");
    exec_ok(first, "CREATE TABLE app.parents (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB");
    exec_ok(
        first,
        "CREATE TABLE app.children ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "CONSTRAINT children_parent_fk FOREIGN KEY (parent_id) REFERENCES app.parents(id)"
        ") ENGINE=InnoDB"
    );
    second = open_database(paths, MYLITE_OPEN_READWRITE);

    exec_ok(first, "START TRANSACTION");
    exec_ok(first, "INSERT INTO app.parents VALUES (1)");
    exec_ok(first, "SAVEPOINT before_child");
    exec_ok(first, "INSERT INTO app.children VALUES (1, 1)");
    exec_ok(first, "ROLLBACK TO SAVEPOINT before_child");
    exec_ok(first, "RELEASE SAVEPOINT before_child");
    exec_ok(first, "COMMIT");
    query_expect(
        second,
        (expected_query){
            .sql = "SELECT "
                   "(SELECT COUNT(*) FROM app.parents) AS parent_count, "
                   "(SELECT COUNT(*) FROM app.children) AS child_count",
            .column_count = 2,
            .row_count = 1,
            .column_names = columns,
            .values = after_rollback_values,
        }
    );

    expect_exec_error(second, "INSERT INTO app.children VALUES (2, 2)", 1452U);
    exec_ok(second, "INSERT INTO app.children VALUES (2, 1)");
    query_expect(
        first,
        (expected_query){
            .sql = "SELECT "
                   "(SELECT COUNT(*) FROM app.parents) AS parent_count, "
                   "(SELECT COUNT(*) FROM app.children) AS child_count",
            .column_count = 2,
            .row_count = 1,
            .column_names = columns,
            .values = final_values,
        }
    );

    assert(mylite_close(second) == MYLITE_OK);
    assert(mylite_close(first) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void create_database_schema(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.items ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
}

static mylite_db *open_database(open_database_paths paths, unsigned flags) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = paths.runtime_root,
    };
    mylite_db *db = NULL;

    assert(mylite_open(paths.database_path, &db, flags, &config) == MYLITE_OK);
    assert(db != NULL);
    return db;
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);

    if (result != MYLITE_OK) {
        fprintf(
            stderr,
            "SQL failed: %s\nresult=%d errcode=%d mariadb_errno=%u sqlstate=%s message=%s\n",
            sql,
            result,
            mylite_errcode(db),
            mylite_mariadb_errno(db),
            mylite_sqlstate(db),
            errmsg != NULL ? errmsg : mylite_errmsg(db)
        );
        mylite_free(errmsg);
        abort();
    }
    assert(errmsg == NULL);
}

static void expect_exec_error(mylite_db *db, const char *sql, unsigned mariadb_errno) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == mariadb_errno);
    assert(errmsg != NULL);
    mylite_free(errmsg);
}

static void *execute_sql_in_thread(void *ctx) {
    exec_thread_args *args = ctx;
    char *errmsg = NULL;
    mylite_db *db = open_database(args->paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);

    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 10");
    args->result = mylite_exec(db, args->sql, NULL, NULL, &errmsg);
    args->mariadb_errno = mylite_mariadb_errno(db);
    if (errmsg != NULL) {
        mylite_free(errmsg);
    }
    args->close_result = mylite_close(db);
    return NULL;
}

static void *execute_overlapping_update_in_thread(void *ctx) {
    overlap_thread_args *args = ctx;
    char sql[160];
    char *errmsg = NULL;
    mylite_db *db = open_database(args->paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);

    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 2");
    exec_ok(db, "START TRANSACTION");
    assert(
        snprintf(
            sql,
            sizeof(sql),
            "UPDATE app.wide_items SET value = value + 1 WHERE id = %d",
            args->row_id
        ) > 0
    );
    args->update_result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    args->mariadb_errno = mylite_mariadb_errno(db);
    if (errmsg != NULL) {
        mylite_free(errmsg);
        errmsg = NULL;
    }

    assert(pthread_mutex_lock(&args->barrier->mutex) == 0);
    ++args->barrier->arrived;
    assert(pthread_cond_broadcast(&args->barrier->condition) == 0);
    while (args->barrier->arrived < 2U) {
        assert(pthread_cond_wait(&args->barrier->condition, &args->barrier->mutex) == 0);
    }
    assert(pthread_mutex_unlock(&args->barrier->mutex) == 0);

    args->transaction_result = mylite_exec(
        db,
        args->update_result == MYLITE_OK ? "COMMIT" : "ROLLBACK",
        NULL,
        NULL,
        &errmsg
    );
    if (errmsg != NULL) {
        mylite_free(errmsg);
    }
    args->close_result = mylite_close(db);
    return NULL;
}

static void populate_wide_items(mylite_db *db, unsigned rows) {
    const size_t capacity = 96U + ((size_t)rows * 40U);
    char *sql = malloc(capacity);
    size_t offset;
    int written;

    assert(sql != NULL);
    written = snprintf(sql, capacity, "INSERT INTO app.wide_items VALUES ");
    assert(written > 0 && (size_t)written < capacity);
    offset = (size_t)written;
    for (unsigned row = 1U; row <= rows; ++row) {
        written = snprintf(
            sql + offset,
            capacity - offset,
            "%s(%u, 0, REPEAT('x', 3000))",
            row == 1U ? "" : ",",
            row
        );
        assert(written > 0 && (size_t)written < capacity - offset);
        offset += (size_t)written;
    }
    exec_ok(db, sql);
    free(sql);
}

static uint64_t wait_for_ownerless_write_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
) {
    const unsigned iterations = timeout_ms * 1000U / MYLITE_TEST_WAIT_POLL_INTERVAL_US;

    for (unsigned iteration = 0U; iteration <= iterations; ++iteration) {
        const uint64_t waiting_count = read_ownerless_write_waiting_count(database_path);
        if (expected_minimum == 0U) {
            if (waiting_count == 0U) {
                return waiting_count;
            }
        } else if (waiting_count >= expected_minimum) {
            return waiting_count;
        }
        sleep_microseconds(MYLITE_TEST_WAIT_POLL_INTERVAL_US);
    }
    return read_ownerless_write_waiting_count(database_path);
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

static uint64_t read_ownerless_write_waiting_count(const char *database_path) {
    return read_lock_waiting_count(
               database_path,
               MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SEGMENT_TYPE
           ) +
           read_lock_waiting_count(
               database_path,
               MYLITE_TEST_CONCURRENCY_PAGE_WRITE_LOCK_SEGMENT_TYPE
           );
}

static uint64_t read_lock_waiting_count(const char *database_path, uint32_t segment_type) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);
    uint64_t registry_offset;

    assert(fd >= 0);
    registry_offset = read_concurrency_shm_segment_offset(fd, segment_type);
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(registry_offset + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_le64(bytes);
}

static mdl_lock_counts read_mdl_lock_counts(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);
    uint64_t mdl_offset;
    mdl_lock_counts counts;

    assert(fd >= 0);
    mdl_offset = read_concurrency_shm_segment_offset(fd, MYLITE_TEST_CONCURRENCY_MDL_SEGMENT_TYPE);
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(mdl_offset + MYLITE_TEST_CONCURRENCY_MDL_ACTIVE_COUNT_OFFSET)
    );
    counts.active = read_le64(bytes);
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(mdl_offset + MYLITE_TEST_CONCURRENCY_MDL_WAITING_COUNT_OFFSET)
    );
    counts.waiting = read_le64(bytes);
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return counts;
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
    unsigned char *cursor = buffer;
    size_t remaining = size;

    while (remaining > 0U) {
        const ssize_t read_size = pread(fd, cursor, remaining, offset);

        if (read_size < 0 && errno == EINTR) {
            continue;
        }
        assert(read_size > 0);
        cursor += (size_t)read_size;
        remaining -= (size_t)read_size;
        offset += read_size;
    }
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec now;

    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return ((uint64_t)now.tv_sec * 1000U) + ((uint64_t)now.tv_nsec / 1000000U);
}

static void query_expect(mylite_db *db, expected_query query) {
    expected_result result = {
        .column_count = query.column_count,
        .row_count = query.row_count,
        .column_names = query.column_names,
        .values = query.values,
        .seen_rows = 0,
    };
    char *errmsg = NULL;

    assert(mylite_exec(db, query.sql, expected_result_callback, &result, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(result.seen_rows == query.row_count);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): required callback signature.
static int expected_result_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    expected_result *result = ctx;

    assert(column_count == result->column_count);
    assert(result->seen_rows < result->row_count);
    for (int column = 0; column < column_count; ++column) {
        const int offset = result->seen_rows * column_count + column;
        assert(strcmp(column_names[column], result->column_names[column]) == 0);
        assert(values[column] != NULL);
        assert(strcmp(values[column], result->values[offset]) == 0);
    }
    ++result->seen_rows;
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-same-process.XXXXXX";
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

static int is_directory_empty(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;

    assert(directory != NULL);
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            assert(closedir(directory) == 0);
            return 0;
        }
    }
    assert(errno == 0);
    assert(closedir(directory) == 0);
    return 1;
}

static int path_exists(const char *path) {
    struct stat path_stat;

    return stat(path, &path_stat) == 0;
}

static uint32_t read_le32(const unsigned char *bytes) {
    uint32_t value = 0U;

    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        value |= (uint32_t)*bytes++ << shift;
    }
    return value;
}

static uint64_t read_le64(const unsigned char *bytes) {
    uint64_t value = 0U;

    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        value |= (uint64_t)*bytes++ << shift;
    }
    return value;
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

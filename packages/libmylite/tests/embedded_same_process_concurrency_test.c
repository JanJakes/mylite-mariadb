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
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE 262144
#define MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_OFFSET 512
#define MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_HEADER_SIZE 96
#define MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_COUNT 16
#define MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_SIZE 128
#define MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_SIZE                                              \
    (MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_HEADER_SIZE +                                        \
     (MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_COUNT * MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_SIZE))
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_HEADER_SIZE 64
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_COUNT 16
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SIZE 64
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SEGMENT_SIZE                                          \
    (MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_HEADER_SIZE +                                            \
     (MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_COUNT * MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SIZE))
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_OFFSET                                                \
    (MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_OFFSET +                                             \
     MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_SIZE)
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_HEADER_SIZE 96
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_COUNT 6
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_SIZE 64
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_SEGMENT_SIZE                                        \
    (MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_HEADER_SIZE +                                          \
     (MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_COUNT *                                         \
      MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_SIZE))
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_OFFSET                                              \
    (MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_OFFSET +                                                 \
     MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SEGMENT_SIZE)
#define MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_HEADER_SIZE 96
#define MYLITE_TEST_CONCURRENCY_TRX_SLOT_COUNT 64
#define MYLITE_TEST_CONCURRENCY_TRX_SLOT_SIZE 64
#define MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_SIZE                                                  \
    (MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_HEADER_SIZE +                                            \
     (MYLITE_TEST_CONCURRENCY_TRX_SLOT_COUNT * MYLITE_TEST_CONCURRENCY_TRX_SLOT_SIZE))
#define MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_OFFSET                                                \
    (MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_OFFSET +                                               \
     MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_SEGMENT_SIZE)
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_OFFSET                                          \
    (MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_OFFSET + MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_SIZE)
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_HEADER_SIZE 96
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_COUNT 64
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_SIZE 576
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_SIZE                                            \
    (MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_HEADER_SIZE +                                      \
     (MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_COUNT * MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_SIZE))
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET                                        \
    (MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_OFFSET +                                           \
     MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_SIZE)
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

static void test_committed_rows_are_visible_across_handles(void);
static void test_active_transactions_can_write_different_rows(void);
static void test_lock_wait_timeout_between_handles(void);
static void test_innodb_wait_registry_tracks_local_waits(void);
static void test_metadata_lock_timeout_between_handles(void);
static void test_savepoints_and_foreign_keys_across_handles(void);
static void create_database_schema(mylite_db *db);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void exec_ok(mylite_db *db, const char *sql);
static void expect_exec_error(mylite_db *db, const char *sql, unsigned mariadb_errno);
static void *execute_sql_in_thread(void *ctx);
static uint64_t wait_for_innodb_lock_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
);
static uint64_t read_innodb_lock_waiting_count(const char *database_path);
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
    test_lock_wait_timeout_between_handles();
    test_innodb_wait_registry_tracks_local_waits();
    test_metadata_lock_timeout_between_handles();
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
    first = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    create_database_schema(first);
    exec_ok(first, "INSERT INTO app.items VALUES (1, 10)");

    exec_ok(first, "START TRANSACTION");
    exec_ok(first, "UPDATE app.items SET value = value + 100 WHERE id = 1");
    assert(pthread_create(&update_thread, NULL, execute_sql_in_thread, &args) == 0);
    assert(wait_for_innodb_lock_waiting_count(database_path, 1U, 5000U) >= 1U);

    exec_ok(first, "ROLLBACK");
    assert(pthread_join(update_thread, NULL) == 0);
    assert(args.result == MYLITE_OK);
    assert(args.mariadb_errno == 0U);
    assert(args.close_result == MYLITE_OK);
    assert(wait_for_innodb_lock_waiting_count(database_path, 0U, 5000U) == 0U);
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

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_OK);
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
    mylite_db *db = open_database(args->paths, MYLITE_OPEN_READWRITE);

    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 10");
    args->result = mylite_exec(db, args->sql, NULL, NULL, &errmsg);
    args->mariadb_errno = mylite_mariadb_errno(db);
    if (errmsg != NULL) {
        mylite_free(errmsg);
    }
    args->close_result = mylite_close(db);
    return NULL;
}

static uint64_t wait_for_innodb_lock_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
) {
    const unsigned iterations = timeout_ms * 1000U / MYLITE_TEST_WAIT_POLL_INTERVAL_US;

    for (unsigned iteration = 0U; iteration <= iterations; ++iteration) {
        const uint64_t waiting_count = read_innodb_lock_waiting_count(database_path);
        if (expected_minimum == 0U) {
            if (waiting_count == 0U) {
                return waiting_count;
            }
        } else if (waiting_count >= expected_minimum) {
            return waiting_count;
        }
        usleep(MYLITE_TEST_WAIT_POLL_INTERVAL_US);
    }
    return read_innodb_lock_waiting_count(database_path);
}

static uint64_t read_innodb_lock_waiting_count(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);
    const unsigned char *page;
    uint64_t waiting_count;

    assert(fd >= 0);
    page = mmap(NULL, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    assert(page != MAP_FAILED);
    waiting_count = read_le64(
        page + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET +
        MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET
    );
    assert(munmap((void *)page, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return waiting_count;
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

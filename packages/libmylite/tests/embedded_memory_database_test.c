#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32

enum { MYLITE_TEST_MEMORY_TOTAL_COLUMN_COUNT = 5 };

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

static void test_memory_database_sql_prepared_transactions_and_cleanup(void);
static mylite_db *open_memory_database(const char *runtime_root);
static void create_memory_schema(mylite_db *db);
static void assert_memory_engines(mylite_db *db);
static void insert_memory_rows_with_transaction(mylite_db *db);
static void insert_memory_prepared_row(mylite_db *db);
static void assert_memory_rows_visible(mylite_db *db);
static void assert_memory_database_is_fresh(mylite_db *db);
static mylite_stmt *prepare_statement(mylite_db *db, const char *sql);
static void exec_ok(mylite_db *db, const char *sql);
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
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

int main(void) {
    test_memory_database_sql_prepared_transactions_and_cleanup();
    return 0;
}

static void test_memory_database_sql_prepared_transactions_and_cleanup(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    mylite_db *db = NULL;
    mylite_db *shared = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_memory_database(runtime_root);
    assert(!is_directory_empty(runtime_root));
    create_memory_schema(db);
    assert_memory_engines(db);
    insert_memory_rows_with_transaction(db);
    insert_memory_prepared_row(db);
    assert_memory_rows_visible(db);

    shared = open_memory_database(runtime_root);
    assert_memory_rows_visible(shared);
    assert(mylite_close(db) == MYLITE_OK);
    assert(!is_directory_empty(runtime_root));
    assert_memory_rows_visible(shared);
    assert(mylite_close(shared) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    db = open_memory_database(runtime_root);
    assert_memory_database_is_fresh(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    free(runtime_root);
    remove_tree(root);
    free(root);
}

static mylite_db *open_memory_database(const char *runtime_root) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = runtime_root,
    };
    mylite_db *db = NULL;

    assert(
        mylite_open(":memory:", &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(db != NULL);
    return db;
}

static void create_memory_schema(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.default_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL, "
        "qty INT NOT NULL, "
        "KEY idx_label (label)"
        ")"
    );
    exec_ok(
        db,
        "CREATE TABLE app.myisam_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL"
        ") ENGINE=MyISAM"
    );
    exec_ok(
        db,
        "CREATE TABLE app.aria_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL"
        ") ENGINE=Aria"
    );
    exec_ok(
        db,
        "CREATE TABLE app.memory_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL"
        ") ENGINE=MEMORY"
    );
}

static void assert_memory_engines(mylite_db *db) {
    static const char *const default_engine_columns[] = {"default_engine"};
    static const char *const default_engine_values[] = {"InnoDB"};
    static const char *const table_engine_columns[] = {"TABLE_NAME", "ENGINE"};
    static const char *const table_engine_values[] = {
        "aria_items",
        "Aria",
        "default_items",
        "InnoDB",
        "memory_items",
        "MEMORY",
        "myisam_items",
        "MyISAM",
    };

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT @@default_storage_engine AS default_engine",
            .column_count = 1,
            .row_count = 1,
            .column_names = default_engine_columns,
            .values = default_engine_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT TABLE_NAME, ENGINE "
                   "FROM information_schema.TABLES "
                   "WHERE TABLE_SCHEMA = 'app' "
                   "ORDER BY TABLE_NAME",
            .column_count = 2,
            .row_count = 4,
            .column_names = table_engine_columns,
            .values = table_engine_values,
        }
    );
}

static void insert_memory_rows_with_transaction(mylite_db *db) {
    static const char *const totals_columns[] = {
        "default_rows",
        "default_qty",
        "myisam_rows",
        "aria_rows",
        "memory_rows",
    };
    static const char *const totals_values[] = {"1", "11", "1", "1", "1"};

    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "INSERT INTO app.default_items VALUES (1, 'alpha', 11)");
    exec_ok(db, "SAVEPOINT before_beta");
    exec_ok(db, "INSERT INTO app.default_items VALUES (2, 'beta', 22)");
    exec_ok(db, "ROLLBACK TO SAVEPOINT before_beta");
    exec_ok(db, "RELEASE SAVEPOINT before_beta");
    exec_ok(db, "COMMIT");

    exec_ok(db, "INSERT INTO app.myisam_items VALUES (1, 'myisam')");
    exec_ok(db, "INSERT INTO app.aria_items VALUES (1, 'aria')");
    exec_ok(db, "INSERT INTO app.memory_items VALUES (1, 'memory')");

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "(SELECT COUNT(*) FROM app.default_items) AS default_rows, "
                   "(SELECT SUM(qty) FROM app.default_items) AS default_qty, "
                   "(SELECT COUNT(*) FROM app.myisam_items) AS myisam_rows, "
                   "(SELECT COUNT(*) FROM app.aria_items) AS aria_rows, "
                   "(SELECT COUNT(*) FROM app.memory_items) AS memory_rows",
            .column_count = MYLITE_TEST_MEMORY_TOTAL_COLUMN_COUNT,
            .row_count = 1,
            .column_names = totals_columns,
            .values = totals_values,
        }
    );
}

static void insert_memory_prepared_row(mylite_db *db) {
    mylite_stmt *insert_stmt =
        prepare_statement(db, "INSERT INTO app.default_items (id, label, qty) VALUES (?, ?, ?)");
    mylite_stmt *select_stmt = NULL;

    assert(mylite_bind_parameter_count(insert_stmt) == 3U);
    assert(mylite_bind_int64(insert_stmt, 1, 3) == MYLITE_OK);
    assert(
        mylite_bind_text(insert_stmt, 2, "gamma", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK
    );
    assert(mylite_bind_int64(insert_stmt, 3, 33) == MYLITE_OK);
    assert(mylite_step(insert_stmt) == MYLITE_DONE);
    assert(mylite_changes(db) == 1);
    assert(mylite_finalize(insert_stmt) == MYLITE_OK);

    select_stmt = prepare_statement(db, "SELECT label, qty FROM app.default_items WHERE id = ?");
    assert(mylite_bind_int64(select_stmt, 1, 3) == MYLITE_OK);
    assert(mylite_step(select_stmt) == MYLITE_ROW);
    assert(mylite_column_count(select_stmt) == 2U);
    assert(strcmp(mylite_column_name(select_stmt, 0), "label") == 0);
    assert(strcmp(mylite_column_name(select_stmt, 1), "qty") == 0);
    assert(strcmp(mylite_column_text(select_stmt, 0), "gamma") == 0);
    assert(mylite_column_int64(select_stmt, 1) == 33);
    assert(mylite_step(select_stmt) == MYLITE_DONE);
    assert(mylite_finalize(select_stmt) == MYLITE_OK);
}

static void assert_memory_rows_visible(mylite_db *db) {
    static const char *const row_columns[] = {"id", "label", "qty"};
    static const char *const row_values[] = {
        "1",
        "alpha",
        "11",
        "3",
        "gamma",
        "33",
    };

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, label, qty FROM app.default_items ORDER BY id",
            .column_count = 3,
            .row_count = 2,
            .column_names = row_columns,
            .values = row_values,
        }
    );
}

static void assert_memory_database_is_fresh(mylite_db *db) {
    static const char *const columns[] = {"app_schema_count"};
    static const char *const values[] = {"0"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT COUNT(*) AS app_schema_count "
                   "FROM information_schema.SCHEMATA "
                   "WHERE SCHEMA_NAME = 'app'",
            .column_count = 1,
            .row_count = 1,
            .column_names = columns,
            .values = values,
        }
    );
}

static mylite_stmt *prepare_statement(mylite_db *db, const char *sql) {
    mylite_stmt *stmt = NULL;
    const char *tail = NULL;

    assert(mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, &tail) == MYLITE_OK);
    assert(stmt != NULL);
    assert(tail == sql + strlen(sql));
    return stmt;
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);

    if (result != MYLITE_OK) {
        fprintf(stderr, "SQL failed: %s\n%s\n", sql, errmsg != NULL ? errmsg : mylite_errmsg(db));
    }
    assert(result == MYLITE_OK);
    assert(errmsg == NULL);
}

static void query_expect(mylite_db *db, expected_query query) {
    expected_result result = {
        .column_count = query.column_count,
        .row_count = query.row_count,
        .column_names = query.column_names,
        .values = query.values,
        .seen_rows = 0,
    };

    assert(mylite_exec(db, query.sql, expected_result_callback, &result, NULL) == MYLITE_OK);
    assert(result.seen_rows == query.row_count);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): required callback signature.
static int expected_result_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    expected_result *result = (expected_result *)ctx;
    assert(column_count == result->column_count);
    assert(result->seen_rows < result->row_count);

    for (int column = 0; column < column_count; ++column) {
        const int value_index = (result->seen_rows * result->column_count) + column;
        const char *expected_value = result->values[value_index];

        assert(strcmp(column_names[column], result->column_names[column]) == 0);
        if (expected_value == NULL) {
            assert(values[column] == NULL);
        } else {
            assert(values[column] != NULL);
            if (strcmp(values[column], expected_value) != 0) {
                fprintf(
                    stderr,
                    "Unexpected value for %s: expected '%s', got '%s'\n",
                    column_names[column],
                    expected_value,
                    values[column]
                );
            }
            assert(strcmp(values[column], expected_value) == 0);
        }
    }

    ++result->seen_rows;
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-memory.XXXXXX";
    char *root = mkdtemp(template_path);
    char *copy = NULL;

    assert(root != NULL);
    copy = strdup(root);
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

static void remove_tree(const char *path) {
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

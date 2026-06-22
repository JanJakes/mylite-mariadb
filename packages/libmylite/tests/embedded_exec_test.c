#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32

typedef struct select_context {
    int rows;
    const char *expected_label;
} select_context;

typedef struct scalar_context {
    int rows;
    const char *expected_value;
} scalar_context;

typedef struct metadata_context {
    int rows;
} metadata_context;

enum exec_result_perf_stat_index {
    EXEC_RESULT_PERF_CALLS = 0,
    EXEC_RESULT_PERF_MYSQL_QUERY_NS,
    EXEC_RESULT_PERF_MYSQL_QUERY_ERRORS,
    EXEC_RESULT_PERF_AFFECTED_ROWS_NS,
    EXEC_RESULT_PERF_STORE_RESULT_NS,
    EXEC_RESULT_PERF_RESULT_SETS,
    EXEC_RESULT_PERF_NO_RESULT_SETS,
    EXEC_RESULT_PERF_CURRENT_SCHEMA_NS,
    EXEC_RESULT_PERF_STATUS_UPDATE_NS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_CALLS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_NS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_ERRORS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_AUTOCOMMIT_NOOPS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_START_TRANSACTION_CALLS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_COMMIT_CALLS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_ROLLBACK_CALLS,
    EXEC_RESULT_PERF_STAT_COUNT
};

typedef struct metadata_only_context {
    int metadata_calls;
    int rows;
} metadata_only_context;

static void test_select_callback(void);
static void test_result_metadata_callback(void);
static void test_empty_result_metadata_callback(void);
static void test_innodb_temporary_table_dml(void);
static void test_ordinary_write_does_not_publish_ownerless_page_log(void);
static void test_stored_procedure_call_callback(void);
static void test_callback_abort(void);
static void test_syntax_error_diagnostics(void);
static void test_native_control_fast_path(void);
static int select_callback(void *ctx, int column_count, char **values, char **column_names);
static int scalar_callback(void *ctx, int column_count, char **values, char **column_names);
static int metadata_callback(
    void *ctx,
    int column_count,
    char **values,
    const size_t *value_lengths,
    const mylite_exec_column *columns
);
static int empty_result_metadata_callback(
    void *ctx,
    int column_count,
    const mylite_exec_column *columns
);
static int unexpected_empty_result_row_callback(
    void *ctx,
    int column_count,
    char **values,
    const size_t *value_lengths,
    const mylite_exec_column *columns
);
static int stored_procedure_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);
static int abort_callback(void *ctx, int column_count, char **values, char **column_names);
static mylite_db *open_database(const char *root, char **database_path);
static void exec_ok(mylite_db *db, const char *sql);
static void expect_scalar(mylite_db *db, const char *sql, const char *expected_value);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static int is_directory(const char *path);
static int is_directory_empty(const char *path);
static int path_exists(const char *path);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);
void mylite_exec_result_perf_set_enabled(int enabled);
void mylite_exec_result_perf_reset(void);
void mylite_exec_result_perf_read(uint64_t *out_values, size_t value_count);

int main(void) {
    test_select_callback();
    test_result_metadata_callback();
    test_empty_result_metadata_callback();
    test_innodb_temporary_table_dml();
    test_ordinary_write_does_not_publish_ownerless_page_log();
    test_stored_procedure_call_callback();
    test_callback_abort();
    test_syntax_error_diagnostics();
    test_native_control_fast_path();
    return 0;
}

static void test_select_callback(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mylite_db *db = open_database(root, &database_path);
    select_context ctx = {.rows = 0};

    assert(
        mylite_exec(db, "SELECT 1 AS one, NULL AS empty", select_callback, &ctx, NULL) == MYLITE_OK
    );
    assert(ctx.rows == 1);
    assert(mylite_changes(db) == 0);
    assert(mylite_last_insert_id(db) == 0U);

    assert(mylite_close(db) == MYLITE_OK);
    free(database_path);
    remove_tree(root);
    free(root);
}

static void test_result_metadata_callback(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mylite_db *db = open_database(root, &database_path);
    metadata_context ctx = {.rows = 0};

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.metadata_probe ("
        "id INT NOT NULL PRIMARY KEY, "
        "payload VARBINARY(8) NOT NULL"
        ") ENGINE=MyISAM"
    );
    exec_ok(db, "INSERT INTO app.metadata_probe VALUES (1, 0x410042)");
    assert(
        mylite_exec_result(
            db,
            "SELECT id AS alias_id, payload FROM app.metadata_probe WHERE id = 1",
            metadata_callback,
            &ctx,
            NULL
        ) == MYLITE_OK
    );
    assert(ctx.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    free(database_path);
    remove_tree(root);
    free(root);
}

static void test_empty_result_metadata_callback(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mylite_db *db = open_database(root, &database_path);
    metadata_only_context ctx = {.metadata_calls = 0, .rows = 0};

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.empty_metadata_probe ("
        "id INT NOT NULL PRIMARY KEY, "
        "payload VARBINARY(8) NOT NULL"
        ") ENGINE=MyISAM"
    );
    exec_ok(db, "INSERT INTO app.empty_metadata_probe VALUES (1, 0x410042)");
    assert(
        mylite_exec_result_with_metadata(
            db,
            "SELECT id AS alias_id, payload FROM app.empty_metadata_probe WHERE id = 99",
            empty_result_metadata_callback,
            unexpected_empty_result_row_callback,
            &ctx,
            NULL
        ) == MYLITE_OK
    );
    assert(ctx.metadata_calls == 1);
    assert(ctx.rows == 0);

    assert(mylite_close(db) == MYLITE_OK);
    free(database_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_temporary_table_dml(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mylite_db *db = open_database(root, &database_path);

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TEMPORARY TABLE app.temp_undo_probe ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.temp_undo_probe VALUES (1, 10), (2, 20)");
    exec_ok(db, "UPDATE app.temp_undo_probe SET value = value + 1 WHERE id = 2");
    expect_scalar(db, "SELECT SUM(value) FROM app.temp_undo_probe", "31");
    exec_ok(db, "DROP TEMPORARY TABLE app.temp_undo_probe");

    assert(mylite_close(db) == MYLITE_OK);
    free(database_path);
    remove_tree(root);
    free(root);
}

static void test_ordinary_write_does_not_publish_ownerless_page_log(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mylite_db *db = open_database(root, &database_path);
    char *concurrency_path = path_join(database_path, "concurrency");
    char *wal_path = path_join(concurrency_path, "mylite-concurrency.wal");

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.ordinary_page_log ("
        "id INT NOT NULL PRIMARY KEY, "
        "value VARCHAR(128) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ordinary_page_log VALUES (1, REPEAT('a', 128))");
    exec_ok(db, "UPDATE app.ordinary_page_log SET value = REPEAT('b', 128) WHERE id = 1");
    assert(mylite_close(db) == MYLITE_OK);
    assert(!path_exists(wal_path));

    free(wal_path);
    free(concurrency_path);
    free(database_path);
    remove_tree(root);
    free(root);
}

static void test_stored_procedure_call_callback(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mylite_db *db = open_database(root, &database_path);
    select_context ctx = {.rows = 0};

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.stored_values ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL"
        ") ENGINE=MyISAM"
    );
    exec_ok(db, "INSERT INTO app.stored_values VALUES (7, 'stored')");
    exec_ok(db, "DROP PROCEDURE IF EXISTS app.select_stored_value");
    exec_ok(
        db,
        "CREATE PROCEDURE app.select_stored_value() "
        "BEGIN SELECT id, label FROM app.stored_values ORDER BY id LIMIT 1; END"
    );
    assert(
        mylite_exec(db, "CALL app.select_stored_value()", stored_procedure_callback, &ctx, NULL) ==
        MYLITE_OK
    );
    assert(ctx.rows == 1);

    exec_ok(db, "UPDATE app.stored_values SET label = 'stored-after-call' WHERE id = 7");
    assert(mylite_changes(db) == 1);

    ctx.rows = 0;
    ctx.expected_label = "stored-after-call";
    assert(
        mylite_exec(
            db,
            "SELECT id, label FROM app.stored_values WHERE id = 7",
            stored_procedure_callback,
            &ctx,
            NULL
        ) == MYLITE_OK
    );
    assert(ctx.rows == 1);

    exec_ok(db, "DROP PROCEDURE IF EXISTS app.select_stored_value");
    assert(mylite_close(db) == MYLITE_OK);
    free(database_path);
    remove_tree(root);
    free(root);
}

static void test_callback_abort(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mylite_db *db = open_database(root, &database_path);
    int callback_count = 0;
    char *errmsg = NULL;

    assert(mylite_exec(db, "SELECT 1", abort_callback, &callback_count, &errmsg) == MYLITE_ERROR);
    assert(callback_count == 1);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "callback") != NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    mylite_free(errmsg);

    assert(mylite_close(db) == MYLITE_OK);
    free(database_path);
    remove_tree(root);
    free(root);
}

static void test_syntax_error_diagnostics(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mylite_db *db = open_database(root, &database_path);
    char *errmsg = NULL;

    assert(mylite_exec(db, "SELEC broken", NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "syntax") != NULL || strstr(errmsg, "SQL") != NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) != 0U);
    assert(strcmp(mylite_sqlstate(db), "00000") != 0);
    mylite_free(errmsg);

    assert(mylite_close(db) == MYLITE_OK);
    free(database_path);
    remove_tree(root);
    free(root);
}

static void test_native_control_fast_path(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mylite_db *db = open_database(root, &database_path);
    uint64_t perf[EXEC_RESULT_PERF_STAT_COUNT] = {0};
    uint64_t native_control_calls_before_rollback_to = 0;
    uint64_t native_control_calls_before_start_options = 0;
    uint64_t native_start_calls_before_start_options = 0;

    mylite_exec_result_perf_reset();
    mylite_exec_result_perf_set_enabled(1);

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, "CREATE TABLE app.native_control_probe (id INT PRIMARY KEY) ENGINE=InnoDB");
    exec_ok(db, "SET autocommit = 0");
    exec_ok(db, "SET autocommit = 0");
    assert(mylite_changes(db) == 0);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "INSERT INTO app.native_control_probe VALUES (1)");
    exec_ok(db, "ROLLBACK");
    assert(mylite_changes(db) == 0);
    expect_scalar(db, "SELECT COUNT(*) FROM app.native_control_probe", "0");

    mylite_exec_result_perf_read(perf, EXEC_RESULT_PERF_STAT_COUNT);
    native_control_calls_before_start_options = perf[EXEC_RESULT_PERF_NATIVE_CONTROL_CALLS];
    native_start_calls_before_start_options =
        perf[EXEC_RESULT_PERF_NATIVE_CONTROL_START_TRANSACTION_CALLS];
    exec_ok(db, "START TRANSACTION READ ONLY");
    mylite_exec_result_perf_read(perf, EXEC_RESULT_PERF_STAT_COUNT);
    assert(
        perf[EXEC_RESULT_PERF_NATIVE_CONTROL_CALLS] == native_control_calls_before_start_options
    );
    assert(
        perf[EXEC_RESULT_PERF_NATIVE_CONTROL_START_TRANSACTION_CALLS] ==
        native_start_calls_before_start_options
    );
    exec_ok(db, "ROLLBACK");

    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "INSERT INTO app.native_control_probe VALUES (1)");
    exec_ok(db, "SAVEPOINT before_extra");
    exec_ok(db, "INSERT INTO app.native_control_probe VALUES (2)");
    mylite_exec_result_perf_read(perf, EXEC_RESULT_PERF_STAT_COUNT);
    native_control_calls_before_rollback_to = perf[EXEC_RESULT_PERF_NATIVE_CONTROL_CALLS];
    exec_ok(db, "ROLLBACK TO SAVEPOINT before_extra");
    mylite_exec_result_perf_read(perf, EXEC_RESULT_PERF_STAT_COUNT);
    assert(perf[EXEC_RESULT_PERF_NATIVE_CONTROL_CALLS] == native_control_calls_before_rollback_to);
    exec_ok(db, "COMMIT");
    assert(mylite_changes(db) == 0);
    expect_scalar(db, "SELECT COUNT(*) FROM app.native_control_probe", "1");

    exec_ok(db, "SET autocommit = 1;");
    exec_ok(db, "SET autocommit = 1;");
    assert(mylite_changes(db) == 0);

    exec_ok(db, "SET autocommit = 0");
    exec_ok(db, "SET autocommit = 0;");
    exec_ok(db, "INSERT INTO app.native_control_probe VALUES (2)");
    exec_ok(db, "SET autocommit = 1;");
    exec_ok(db, "ROLLBACK");
    expect_scalar(db, "SELECT COUNT(*) FROM app.native_control_probe", "2");

    mylite_exec_result_perf_set_enabled(0);
    mylite_exec_result_perf_read(perf, EXEC_RESULT_PERF_STAT_COUNT);
    assert(perf[EXEC_RESULT_PERF_NATIVE_CONTROL_CALLS] == 13U);
    assert(perf[EXEC_RESULT_PERF_NATIVE_CONTROL_NS] > 0U);
    assert(perf[EXEC_RESULT_PERF_NATIVE_CONTROL_ERRORS] == 0U);
    assert(perf[EXEC_RESULT_PERF_NATIVE_CONTROL_AUTOCOMMIT_NOOPS] == 3U);
    assert(perf[EXEC_RESULT_PERF_NATIVE_CONTROL_START_TRANSACTION_CALLS] == 2U);
    assert(perf[EXEC_RESULT_PERF_NATIVE_CONTROL_COMMIT_CALLS] == 1U);
    assert(perf[EXEC_RESULT_PERF_NATIVE_CONTROL_ROLLBACK_CALLS] == 3U);

    assert(mylite_close(db) == MYLITE_OK);
    free(database_path);
    remove_tree(root);
    free(root);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): required callback signature.
static int select_callback(void *ctx, int column_count, char **values, char **column_names) {
    select_context *select_ctx = (select_context *)ctx;
    assert(column_count == 2);
    assert(strcmp(column_names[0], "one") == 0);
    assert(strcmp(column_names[1], "empty") == 0);
    assert(values[0] != NULL);
    assert(strcmp(values[0], "1") == 0);
    assert(values[1] == NULL);
    ++select_ctx->rows;
    return 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): required callback signature.
static int scalar_callback(void *ctx, int column_count, char **values, char **column_names) {
    scalar_context *scalar_ctx = (scalar_context *)ctx;
    (void)column_names;

    assert(column_count == 1);
    assert(values[0] != NULL);
    assert(strcmp(values[0], scalar_ctx->expected_value) == 0);
    ++scalar_ctx->rows;
    return 0;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): required callback signature.
static int metadata_callback(
    void *ctx,
    int column_count,
    char **values,
    const size_t *value_lengths,
    const mylite_exec_column *columns
) {
    static const char expected_payload[] = {'A', '\0', 'B'};
    metadata_context *metadata_ctx = (metadata_context *)ctx;

    assert(column_count == 2);
    assert(strcmp(columns[0].name, "alias_id") == 0);
    assert(strcmp(columns[0].org_name, "id") == 0);
    assert(strcmp(columns[0].table, "metadata_probe") == 0);
    assert(strcmp(columns[0].org_table, "metadata_probe") == 0);
    assert(strcmp(columns[1].name, "payload") == 0);
    assert(strcmp(columns[1].org_name, "payload") == 0);
    assert(strcmp(columns[1].table, "metadata_probe") == 0);
    assert(strcmp(columns[1].org_table, "metadata_probe") == 0);
    assert(values[0] != NULL);
    assert(value_lengths[0] == 1U);
    assert(memcmp(values[0], "1", value_lengths[0]) == 0);
    assert(values[1] != NULL);
    assert(value_lengths[1] == sizeof(expected_payload));
    assert(memcmp(values[1], expected_payload, sizeof(expected_payload)) == 0);
    ++metadata_ctx->rows;
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static int empty_result_metadata_callback(
    void *ctx,
    int column_count,
    const mylite_exec_column *columns
) {
    metadata_only_context *metadata_ctx = (metadata_only_context *)ctx;

    assert(column_count == 2);
    assert(strcmp(columns[0].name, "alias_id") == 0);
    assert(strcmp(columns[0].org_name, "id") == 0);
    assert(strcmp(columns[0].table, "empty_metadata_probe") == 0);
    assert(strcmp(columns[0].org_table, "empty_metadata_probe") == 0);
    assert(strcmp(columns[1].name, "payload") == 0);
    assert(strcmp(columns[1].org_name, "payload") == 0);
    assert(strcmp(columns[1].table, "empty_metadata_probe") == 0);
    assert(strcmp(columns[1].org_table, "empty_metadata_probe") == 0);
    ++metadata_ctx->metadata_calls;
    return 0;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): required callback signature.
static int unexpected_empty_result_row_callback(
    void *ctx,
    int column_count,
    char **values,
    const size_t *value_lengths,
    const mylite_exec_column *columns
) {
    metadata_only_context *metadata_ctx = (metadata_only_context *)ctx;
    (void)column_count;
    (void)values;
    (void)value_lengths;
    (void)columns;
    ++metadata_ctx->rows;
    assert(0);
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

// NOLINTBEGIN(bugprone-easily-swappable-parameters): required callback signature.
static int stored_procedure_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    select_context *select_ctx = (select_context *)ctx;
    assert(column_count == 2);
    assert(strcmp(column_names[0], "id") == 0);
    assert(strcmp(column_names[1], "label") == 0);
    assert(values[0] != NULL);
    assert(strcmp(values[0], "7") == 0);
    assert(values[1] != NULL);
    assert(
        strcmp(
            values[1],
            select_ctx->expected_label != NULL ? select_ctx->expected_label : "stored"
        ) == 0
    );
    ++select_ctx->rows;
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): required callback signature.
static int abort_callback(void *ctx, int column_count, char **values, char **column_names) {
    int *callback_count = (int *)ctx;
    (void)column_count;
    (void)values;
    (void)column_names;
    ++*callback_count;
    return 1;
}

static mylite_db *open_database(const char *root, char **database_path) {
    char *runtime_root = path_join(root, "runtime");
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = runtime_root,
    };
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    *database_path = path_join(root, "exec.mylite");
    assert(
        mylite_open(*database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(is_directory(*database_path));
    free(runtime_root);
    return db;
}

static void exec_ok(mylite_db *db, const char *sql) {
    assert(mylite_exec(db, sql, NULL, NULL, NULL) == MYLITE_OK);
}

static void expect_scalar(mylite_db *db, const char *sql, const char *expected_value) {
    scalar_context ctx = {.rows = 0, .expected_value = expected_value};
    assert(mylite_exec(db, sql, scalar_callback, &ctx, NULL) == MYLITE_OK);
    assert(ctx.rows == 1);
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-exec.XXXXXX";
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

static int is_directory(const char *path) {
    struct stat path_stat;
    assert(lstat(path, &path_stat) == 0);
    return S_ISDIR(path_stat.st_mode);
}

static int is_directory_empty(const char *path) {
    DIR *directory = opendir(path);
    assert(directory != NULL);

    int count = 0;
    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }

    assert(closedir(directory) == 0);
    return count == 0;
}

static int path_exists(const char *path) {
    struct stat path_stat;
    return lstat(path, &path_stat) == 0;
}

static void remove_tree(const char *path) {
    char *runtime_root = path_join(path, "runtime");
    assert(is_directory_empty(runtime_root));
    free(runtime_root);
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
    return type_flag == FTW_DP ? rmdir(path) : unlink(path);
}

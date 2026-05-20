#include <mylite/mylite.h>

#include <assert.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_LARGE_PAYLOAD_SIZE 6000

static const double k_first_ratio = 2.5;
static const double k_second_ratio = 3.75;
static const double k_large_ratio = 1.5;
static const unsigned k_payload_pattern_modulus = 251U;

typedef struct open_database_paths {
    const char *database_path;
    const char *runtime_root;
} open_database_paths;

static void test_prepared_statement_bindings_and_columns(void);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void create_prepared_schema(mylite_db *db);
static mylite_stmt *prepare_statement(mylite_db *db, const char *sql);
static void insert_prepared_rows(mylite_db *db, mylite_stmt *stmt);
static void insert_first_prepared_row(mylite_db *db, mylite_stmt *stmt);
static void insert_second_prepared_row(mylite_db *db, mylite_stmt *stmt);
static void insert_large_prepared_row(mylite_db *db, mylite_stmt *stmt);
static void insert_empty_prepared_row(mylite_db *db, mylite_stmt *stmt);
static void assert_prepared_row(mylite_stmt *stmt);
static void assert_large_payload_row(mylite_stmt *stmt);
static void assert_empty_payload_row(mylite_stmt *stmt);
static void assert_warning_access(mylite_db *db);
static void fill_large_payload(unsigned char *payload, size_t payload_size);
static void exec_ok(mylite_db *db, const char *sql);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

int main(void) {
    test_prepared_statement_bindings_and_columns();
    return 0;
}

static void test_prepared_statement_bindings_and_columns(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "prepared.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db = NULL;
    mylite_stmt *insert_stmt = NULL;
    mylite_stmt *select_stmt = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    create_prepared_schema(db);

    insert_stmt = prepare_statement(
        db,
        "INSERT INTO app.prepared_values "
        "(id, unsigned_value, ratio, label, payload, optional_text) "
        "VALUES (?, ?, ?, ?, ?, ?)"
    );
    insert_prepared_rows(db, insert_stmt);

    select_stmt = prepare_statement(
        db,
        "SELECT id, unsigned_value, ratio, label, payload, optional_text "
        "FROM app.prepared_values WHERE id = ?"
    );
    assert(mylite_bind_int64(select_stmt, 1, 1) == MYLITE_OK);
    assert(mylite_step(select_stmt) == MYLITE_ROW);
    assert_prepared_row(select_stmt);
    assert(mylite_step(select_stmt) == MYLITE_DONE);
    assert(mylite_reset(select_stmt) == MYLITE_OK);

    assert(mylite_bind_int64(select_stmt, 1, 3) == MYLITE_OK);
    assert(mylite_step(select_stmt) == MYLITE_ROW);
    assert_large_payload_row(select_stmt);
    assert(mylite_step(select_stmt) == MYLITE_DONE);
    assert(mylite_reset(select_stmt) == MYLITE_OK);

    assert(mylite_bind_int64(select_stmt, 1, 4) == MYLITE_OK);
    assert(mylite_step(select_stmt) == MYLITE_ROW);
    assert_empty_payload_row(select_stmt);
    assert(mylite_step(select_stmt) == MYLITE_DONE);
    assert(mylite_reset(select_stmt) == MYLITE_OK);

    assert_warning_access(db);
    assert(mylite_close(db) == MYLITE_BUSY);
    assert(mylite_finalize(select_stmt) == MYLITE_OK);
    assert(mylite_finalize(insert_stmt) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
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

static void create_prepared_schema(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.prepared_values ("
        "id BIGINT NOT NULL PRIMARY KEY, "
        "unsigned_value BIGINT UNSIGNED NOT NULL, "
        "ratio DOUBLE NOT NULL, "
        "label VARCHAR(64) NOT NULL, "
        "payload LONGBLOB NOT NULL, "
        "optional_text VARCHAR(64) NULL"
        ") ENGINE=InnoDB"
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

static void insert_prepared_rows(mylite_db *db, mylite_stmt *stmt) {
    assert(mylite_bind_parameter_count(stmt) == 6U);
    insert_first_prepared_row(db, stmt);
    insert_second_prepared_row(db, stmt);
    insert_large_prepared_row(db, stmt);
    insert_empty_prepared_row(db, stmt);
}

static void insert_first_prepared_row(mylite_db *db, mylite_stmt *stmt) {
    static const unsigned char first_payload[] = {'A', '\0', 'Z'};

    assert(mylite_bind_int64(stmt, 1, 1) == MYLITE_OK);
    assert(mylite_bind_uint64(stmt, 2, 9223372036854775810ULL) == MYLITE_OK);
    assert(mylite_bind_double(stmt, 3, k_first_ratio) == MYLITE_OK);
    assert(mylite_bind_text(stmt, 4, "alpha", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK);
    assert(
        mylite_bind_blob(stmt, 5, first_payload, sizeof(first_payload), MYLITE_STATIC) == MYLITE_OK
    );
    assert(mylite_bind_null(stmt, 6) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_changes(db) == 1);
    assert(mylite_reset(stmt) == MYLITE_OK);
}

static void insert_second_prepared_row(mylite_db *db, mylite_stmt *stmt) {
    static const unsigned char second_payload[] = {'B', '2'};

    assert(mylite_clear_bindings(stmt) == MYLITE_OK);
    assert(mylite_bind_int64(stmt, 1, 2) == MYLITE_OK);
    assert(mylite_bind_uint64(stmt, 2, 42U) == MYLITE_OK);
    assert(mylite_bind_double(stmt, 3, k_second_ratio) == MYLITE_OK);
    assert(mylite_bind_text(stmt, 4, "beta", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK);
    assert(
        mylite_bind_blob(stmt, 5, second_payload, sizeof(second_payload), MYLITE_STATIC) ==
        MYLITE_OK
    );
    assert(mylite_bind_text(stmt, 6, "present", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_changes(db) == 1);
    assert(mylite_reset(stmt) == MYLITE_OK);
}

static void insert_large_prepared_row(mylite_db *db, mylite_stmt *stmt) {
    unsigned char *large_payload = malloc(MYLITE_TEST_LARGE_PAYLOAD_SIZE);

    assert(large_payload != NULL);
    fill_large_payload(large_payload, MYLITE_TEST_LARGE_PAYLOAD_SIZE);
    assert(mylite_clear_bindings(stmt) == MYLITE_OK);
    assert(mylite_bind_int64(stmt, 1, 3) == MYLITE_OK);
    assert(mylite_bind_uint64(stmt, 2, 7U) == MYLITE_OK);
    assert(mylite_bind_double(stmt, 3, k_large_ratio) == MYLITE_OK);
    assert(mylite_bind_text(stmt, 4, "large", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK);
    assert(
        mylite_bind_blob(stmt, 5, large_payload, MYLITE_TEST_LARGE_PAYLOAD_SIZE, MYLITE_STATIC) ==
        MYLITE_OK
    );
    assert(mylite_bind_text(stmt, 6, "long", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_changes(db) == 1);
    assert(mylite_reset(stmt) == MYLITE_OK);
    free(large_payload);
}

static void insert_empty_prepared_row(mylite_db *db, mylite_stmt *stmt) {
    assert(mylite_clear_bindings(stmt) == MYLITE_OK);
    assert(mylite_bind_int64(stmt, 1, 4) == MYLITE_OK);
    assert(mylite_bind_uint64(stmt, 2, 0U) == MYLITE_OK);
    assert(mylite_bind_double(stmt, 3, 0.0) == MYLITE_OK);
    assert(mylite_bind_text(stmt, 4, NULL, 0, MYLITE_STATIC) == MYLITE_OK);
    assert(mylite_bind_blob(stmt, 5, NULL, 0, MYLITE_STATIC) == MYLITE_OK);
    assert(mylite_bind_text(stmt, 6, "", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_changes(db) == 1);
    assert(mylite_reset(stmt) == MYLITE_OK);
}

static void assert_prepared_row(mylite_stmt *stmt) {
    const unsigned char expected_payload[] = {'A', '\0', 'Z'};

    assert(mylite_column_count(stmt) == 6U);
    assert(strcmp(mylite_column_name(stmt, 0), "id") == 0);
    assert(strcmp(mylite_column_name(stmt, 1), "unsigned_value") == 0);
    assert(strcmp(mylite_column_name(stmt, 2), "ratio") == 0);
    assert(strcmp(mylite_column_name(stmt, 3), "label") == 0);
    assert(strcmp(mylite_column_name(stmt, 4), "payload") == 0);
    assert(strcmp(mylite_column_name(stmt, 5), "optional_text") == 0);
    assert(mylite_column_type(stmt, 0) == MYLITE_TYPE_INT64);
    assert(mylite_column_type(stmt, 1) == MYLITE_TYPE_UINT64);
    assert(mylite_column_type(stmt, 2) == MYLITE_TYPE_DOUBLE);
    assert(mylite_column_type(stmt, 3) == MYLITE_TYPE_TEXT);
    assert(mylite_column_type(stmt, 4) == MYLITE_TYPE_BLOB);
    assert(mylite_column_type(stmt, 5) == MYLITE_TYPE_NULL);
    assert(mylite_column_int64(stmt, 0) == 1);
    assert(mylite_column_uint64(stmt, 1) == 9223372036854775810ULL);
    assert(mylite_column_double(stmt, 2) == k_first_ratio);
    assert(strcmp(mylite_column_text(stmt, 3), "alpha") == 0);
    assert(mylite_column_bytes(stmt, 3) == 5U);
    assert(mylite_column_bytes(stmt, 4) == sizeof(expected_payload));
    assert(memcmp(mylite_column_blob(stmt, 4), expected_payload, sizeof(expected_payload)) == 0);
    assert(mylite_column_text(stmt, 5) == NULL);
    assert(mylite_column_blob(stmt, 5) == NULL);
    assert(mylite_column_bytes(stmt, 5) == 0U);
}

static void assert_large_payload_row(mylite_stmt *stmt) {
    const unsigned char *payload = mylite_column_blob(stmt, 4);

    assert(mylite_column_int64(stmt, 0) == 3);
    assert(strcmp(mylite_column_text(stmt, 3), "large") == 0);
    assert(mylite_column_type(stmt, 4) == MYLITE_TYPE_BLOB);
    assert(mylite_column_bytes(stmt, 4) == MYLITE_TEST_LARGE_PAYLOAD_SIZE);
    assert(payload != NULL);
    for (size_t index = 0; index < MYLITE_TEST_LARGE_PAYLOAD_SIZE; ++index) {
        assert(payload[index] == (unsigned char)(index % k_payload_pattern_modulus));
    }
}

static void assert_empty_payload_row(mylite_stmt *stmt) {
    assert(mylite_column_int64(stmt, 0) == 4);
    assert(mylite_column_type(stmt, 3) == MYLITE_TYPE_TEXT);
    assert(mylite_column_text(stmt, 3) != NULL);
    assert(strcmp(mylite_column_text(stmt, 3), "") == 0);
    assert(mylite_column_bytes(stmt, 3) == 0U);
    assert(mylite_column_type(stmt, 4) == MYLITE_TYPE_BLOB);
    assert(mylite_column_blob(stmt, 4) != NULL);
    assert(mylite_column_bytes(stmt, 4) == 0U);
    assert(mylite_column_type(stmt, 5) == MYLITE_TYPE_TEXT);
    assert(mylite_column_text(stmt, 5) != NULL);
    assert(strcmp(mylite_column_text(stmt, 5), "") == 0);
    assert(mylite_column_bytes(stmt, 5) == 0U);
}

static void assert_warning_access(mylite_db *db) {
    mylite_warning_level level = MYLITE_WARNING_NOTE;
    unsigned code = 0;
    const char *message = NULL;

    exec_ok(db, "SELECT CAST('not-a-number' AS UNSIGNED) AS coerced");
    assert(mylite_warning_count(db) > 0U);
    assert(mylite_warning(db, 0, &level, &code, &message) == MYLITE_OK);
    assert(level == MYLITE_WARNING_WARNING);
    assert(code > 0U);
    assert(message != NULL);
    assert(message[0] != '\0');
    assert(mylite_warning(db, mylite_warning_count(db), NULL, NULL, NULL) == MYLITE_NOTFOUND);
}

static void fill_large_payload(unsigned char *payload, size_t payload_size) {
    for (size_t index = 0; index < payload_size; ++index) {
        payload[index] = (unsigned char)(index % k_payload_pattern_modulus);
    }
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-prepared.XXXXXX";
    char *root = mkdtemp(template_path);
    char *copy = NULL;

    assert(root != NULL);
    copy = strdup(root);
    assert(copy != NULL);
    return copy;
}

static char *path_join(const char *directory, const char *name) {
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    char *path = malloc(directory_length + name_length + 2);

    assert(path != NULL);
    assert(sprintf(path, "%s/%s", directory, name) > 0);
    return path;
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

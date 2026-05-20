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
#define MYLITE_TEST_DUPLICATE_KEY_ERRNO 1062U

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

static void test_myisam_native_table_operations_survive_reopen(void);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void create_native_table(mylite_db *db);
static void insert_initial_rows(mylite_db *db);
static void assert_duplicate_unique_key_fails(mylite_db *db);
static void update_and_delete_rows(mylite_db *db);
static void assert_pre_alter_queries(mylite_db *db);
static void alter_table_with_copy_rebuild(mylite_db *db);
static void assert_post_alter_queries(mylite_db *db);
static void assert_reopened_table(mylite_db *db);
static void drop_native_table(mylite_db *db);
static void exec_ok(mylite_db *db, const char *sql);
static void exec_duplicate_key_error(mylite_db *db, const char *sql);
static void query_expect(mylite_db *db, expected_query query);
static int expected_result_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void assert_database_open_layout(const char *database_path);
static void assert_database_closed_layout(const char *database_path);
static void assert_native_table_files(const char *database_path);
static void assert_native_table_files_removed(const char *database_path);
static void assert_test_root_contains_only_database_and_runtime(const char *root);
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

int main(void) {
    test_myisam_native_table_operations_survive_reopen();
    return 0;
}

static void test_myisam_native_table_operations_survive_reopen(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "native-ops.mylite");
    mylite_db *db = NULL;
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    assert_database_open_layout(database_path);
    assert(is_directory_empty(runtime_root));

    create_native_table(db);
    insert_initial_rows(db);
    assert_duplicate_unique_key_fails(db);
    update_and_delete_rows(db);
    assert_pre_alter_queries(db);
    alter_table_with_copy_rebuild(db);
    assert_post_alter_queries(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert_native_table_files(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(root);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert_database_open_layout(database_path);
    assert_reopened_table(db);
    drop_native_table(db);
    assert_native_table_files_removed(database_path);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(root);

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

static void create_native_table(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.native_ops ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "name VARCHAR(64) NOT NULL, "
        "email VARCHAR(128) NULL, "
        "note TEXT NOT NULL, "
        "payload BLOB NOT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY uq_email (email), "
        "KEY idx_name (name)"
        ") ENGINE=MyISAM"
    );
}

static void insert_initial_rows(mylite_db *db) {
    exec_ok(
        db,
        "INSERT INTO app.native_ops (name, email, note, payload) "
        "VALUES ('alpha', 'alpha@example.test', REPEAT('a', 4096), REPEAT('A', 4096))"
    );
    assert(mylite_changes(db) == 1);
    assert(mylite_last_insert_id(db) == 1U);

    exec_ok(
        db,
        "INSERT INTO app.native_ops (name, email, note, payload) "
        "VALUES ('beta', 'beta@example.test', REPEAT('b', 8192), REPEAT('B', 8192))"
    );
    assert(mylite_changes(db) == 1);
    assert(mylite_last_insert_id(db) == 2U);

    exec_ok(
        db,
        "INSERT INTO app.native_ops (name, email, note, payload) "
        "VALUES ('gamma', NULL, REPEAT('g', 2048), REPEAT('G', 2048))"
    );
    assert(mylite_changes(db) == 1);
    assert(mylite_last_insert_id(db) == 3U);

    exec_ok(
        db,
        "INSERT INTO app.native_ops (name, email, note, payload) "
        "VALUES ('delta', NULL, REPEAT('d', 1024), REPEAT('D', 1024))"
    );
    assert(mylite_changes(db) == 1);
    assert(mylite_last_insert_id(db) == 4U);
}

static void assert_duplicate_unique_key_fails(mylite_db *db) {
    exec_duplicate_key_error(
        db,
        "INSERT INTO app.native_ops (name, email, note, payload) "
        "VALUES ('duplicate', 'alpha@example.test', 'dupe', 'dupe')"
    );
}

static void update_and_delete_rows(mylite_db *db) {
    exec_ok(
        db,
        "UPDATE app.native_ops "
        "SET name = 'alpha2', note = CONCAT(note, '-tail'), payload = CONCAT(payload, 'Z') "
        "WHERE email = 'alpha@example.test'"
    );
    assert(mylite_changes(db) == 1);

    exec_ok(db, "DELETE FROM app.native_ops WHERE name = 'gamma'");
    assert(mylite_changes(db) == 1);
}

static void assert_pre_alter_queries(mylite_db *db) {
    static const char *const scan_columns[] = {"id", "name"};
    static const char *const scan_values[] = {"1", "alpha2", "2", "beta", "4", "delta"};
    static const char *const unique_columns[] = {"id", "name"};
    static const char *const unique_values[] = {"2", "beta"};
    static const char *const secondary_columns[] = {"id", "email"};
    static const char *const secondary_values[] = {"1", "alpha@example.test"};
    static const char *const length_columns[] = {
        "note_len",
        "payload_len",
        "last_payload_hex",
    };
    static const char *const length_values[] = {"4101", "4097", "5A"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, name FROM app.native_ops ORDER BY id",
            .column_count = 2,
            .row_count = 3,
            .column_names = scan_columns,
            .values = scan_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, name FROM app.native_ops WHERE email = 'beta@example.test'",
            .column_count = 2,
            .row_count = 1,
            .column_names = unique_columns,
            .values = unique_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, email FROM app.native_ops WHERE name = 'alpha2'",
            .column_count = 2,
            .row_count = 1,
            .column_names = secondary_columns,
            .values = secondary_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "OCTET_LENGTH(note) AS note_len, "
                   "OCTET_LENGTH(payload) AS payload_len, "
                   "HEX(SUBSTRING(payload, OCTET_LENGTH(payload), 1)) AS last_payload_hex "
                   "FROM app.native_ops WHERE id = 1",
            .column_count = 3,
            .row_count = 1,
            .column_names = length_columns,
            .values = length_values,
        }
    );
}

static void alter_table_with_copy_rebuild(mylite_db *db) {
    exec_ok(
        db,
        "ALTER TABLE app.native_ops "
        "ADD COLUMN status VARCHAR(16) NOT NULL DEFAULT 'ready', "
        "ADD KEY idx_status (status), "
        "ALGORITHM=COPY"
    );
    exec_ok(db, "UPDATE app.native_ops SET status = 'archived' WHERE email = 'beta@example.test'");
    assert(mylite_changes(db) == 1);
}

static void assert_post_alter_queries(mylite_db *db) {
    static const char *const status_columns[] = {"id", "status"};
    static const char *const status_values[] = {"2", "archived"};
    static const char *const default_columns[] = {"id", "status"};
    static const char *const default_values[] = {"1", "ready", "4", "ready"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, status FROM app.native_ops WHERE status = 'archived'",
            .column_count = 2,
            .row_count = 1,
            .column_names = status_columns,
            .values = status_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, status FROM app.native_ops "
                   "WHERE status = 'ready' ORDER BY id",
            .column_count = 2,
            .row_count = 2,
            .column_names = default_columns,
            .values = default_values,
        }
    );
}

static void assert_reopened_table(mylite_db *db) {
    static const char *const scan_columns[] = {"id", "name", "status"};
    static const char *const scan_values[] = {
        "1",
        "alpha2",
        "ready",
        "2",
        "beta",
        "archived",
        "4",
        "delta",
        "ready",
        "5",
        "epsilon",
        "ready",
    };

    exec_ok(
        db,
        "INSERT INTO app.native_ops (name, email, note, payload) "
        "VALUES ('epsilon', 'epsilon@example.test', REPEAT('e', 512), REPEAT('E', 512))"
    );
    assert(mylite_changes(db) == 1);
    assert(mylite_last_insert_id(db) == 5U);
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, name, status FROM app.native_ops ORDER BY id",
            .column_count = 3,
            .row_count = 4,
            .column_names = scan_columns,
            .values = scan_values,
        }
    );
}

static void drop_native_table(mylite_db *db) {
    exec_ok(db, "DROP TABLE app.native_ops");
    exec_ok(db, "DROP DATABASE app");
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
}

static void exec_duplicate_key_error(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(errmsg != NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == MYLITE_TEST_DUPLICATE_KEY_ERRNO);
    assert(strcmp(mylite_sqlstate(db), "23000") == 0);
    assert(strstr(mylite_errmsg(db), "Duplicate entry") != NULL);
    mylite_free(errmsg);
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

// Required callback signature.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
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
            continue;
        }
        assert(values[column] != NULL);
        assert(strcmp(values[column], expected_value) == 0);
    }

    ++result->seen_rows;
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-native-ops.XXXXXX";
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

static void assert_database_open_layout(const char *database_path) {
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");
    char *plugin_path = path_join(run_path, "plugins");

    assert(path_exists(metadata_path));
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(is_directory(run_path));
    assert(is_directory(plugin_path));

    free(plugin_path);
    free(run_path);
    free(tmp_path);
    free(data_path);
    free(metadata_path);
}

static void assert_database_closed_layout(const char *database_path) {
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");

    assert(path_exists(metadata_path));
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(!path_exists(run_path));

    free(run_path);
    free(tmp_path);
    free(data_path);
    free(metadata_path);
}

static void assert_native_table_files(const char *database_path) {
    char *data_path = path_join(database_path, "datadir");
    char *app_schema_path = path_join(data_path, "app");
    char *definition_path = path_join(app_schema_path, "native_ops.frm");
    char *data_file_path = path_join(app_schema_path, "native_ops.MYD");
    char *index_path = path_join(app_schema_path, "native_ops.MYI");

    assert(path_exists(definition_path));
    assert(path_exists(data_file_path));
    assert(path_exists(index_path));

    free(index_path);
    free(data_file_path);
    free(definition_path);
    free(app_schema_path);
    free(data_path);
}

static void assert_native_table_files_removed(const char *database_path) {
    char *data_path = path_join(database_path, "datadir");
    char *app_schema_path = path_join(data_path, "app");
    char *definition_path = path_join(app_schema_path, "native_ops.frm");
    char *data_file_path = path_join(app_schema_path, "native_ops.MYD");
    char *index_path = path_join(app_schema_path, "native_ops.MYI");

    assert(!path_exists(definition_path));
    assert(!path_exists(data_file_path));
    assert(!path_exists(index_path));
    assert(!path_exists(app_schema_path));

    free(index_path);
    free(data_file_path);
    free(definition_path);
    free(app_schema_path);
    free(data_path);
}

static void assert_test_root_contains_only_database_and_runtime(const char *root) {
    DIR *directory = opendir(root);
    int saw_database = 0;
    int saw_runtime = 0;
    assert(directory != NULL);

    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (strcmp(entry->d_name, "native-ops.mylite") == 0) {
            ++saw_database;
            continue;
        }
        if (strcmp(entry->d_name, "runtime") == 0) {
            ++saw_runtime;
            continue;
        }
        assert(0 && "unexpected file outside the MyLite database directory");
    }

    assert(closedir(directory) == 0);
    assert(saw_database == 1);
    assert(saw_runtime == 1);
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
    if (lstat(path, &path_stat) == 0) {
        return 1;
    }
    assert(errno == ENOENT);
    return 0;
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
    return type_flag == FTW_DP ? rmdir(path) : unlink(path);
}

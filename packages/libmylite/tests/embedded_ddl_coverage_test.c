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

typedef struct root_entries {
    const char *root;
    const char *database_name;
} root_entries;

static void test_broader_ddl_survives_reopen(void);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void assert_default_engine(mylite_db *db);
static void create_base_schema(mylite_db *db);
static void exercise_table_copy_ddl(mylite_db *db);
static void exercise_index_ddl(mylite_db *db);
static void exercise_alter_table_ddl(mylite_db *db);
static void exercise_check_constraint_ddl(mylite_db *db);
static void exercise_foreign_key_ddl(mylite_db *db);
static void exercise_generated_column_ddl(mylite_db *db);
static void assert_broader_ddl_state(mylite_db *db);
static void exec_ok(mylite_db *db, const char *sql);
static void expect_error(mylite_db *db, const char *sql);
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
static void assert_test_root_contains_only_database_and_runtime(root_entries entries);
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
    test_broader_ddl_survives_reopen();
    return 0;
}

static void test_broader_ddl_survives_reopen(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ddl-coverage.mylite");
    const root_entries expected_root = {
        .root = root,
        .database_name = "ddl-coverage.mylite",
    };
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    assert_database_open_layout(database_path);
    assert_default_engine(db);
    create_base_schema(db);
    exercise_table_copy_ddl(db);
    exercise_index_ddl(db);
    exercise_alter_table_ddl(db);
    exercise_check_constraint_ddl(db);
    exercise_foreign_key_ddl(db);
    exercise_generated_column_ddl(db);
    assert_broader_ddl_state(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(expected_root);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert_database_open_layout(database_path);
    assert_default_engine(db);
    assert_broader_ddl_state(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(expected_root);

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

static void assert_default_engine(mylite_db *db) {
    static const char *const columns[] = {"default_engine"};
    static const char *const values[] = {"InnoDB"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT @@default_storage_engine AS default_engine",
            .column_count = 1,
            .row_count = 1,
            .column_names = columns,
            .values = values,
        }
    );
}

static void create_base_schema(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.base_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL, "
        "qty INT NOT NULL, "
        "KEY idx_qty (qty)"
        ")"
    );
    exec_ok(db, "INSERT INTO app.base_items VALUES (1, 'alpha', 10), (2, 'beta', 20)");
}

static void exercise_table_copy_ddl(mylite_db *db) {
    exec_ok(db, "CREATE TABLE app.like_items LIKE app.base_items");
    exec_ok(db, "INSERT INTO app.like_items SELECT * FROM app.base_items WHERE id = 1");
    exec_ok(
        db,
        "CREATE TABLE app.selected_items AS "
        "SELECT id, label, qty FROM app.base_items WHERE qty >= 20"
    );
}

static void exercise_index_ddl(mylite_db *db) {
    static const char *const columns[] = {"index_count"};
    static const char *const created_values[] = {"1"};
    static const char *const dropped_values[] = {"0"};

    exec_ok(db, "CREATE INDEX idx_base_label ON app.base_items (label)");
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT COUNT(*) AS index_count "
                   "FROM information_schema.STATISTICS "
                   "WHERE TABLE_SCHEMA = 'app' "
                   "AND TABLE_NAME = 'base_items' "
                   "AND INDEX_NAME = 'idx_base_label'",
            .column_count = 1,
            .row_count = 1,
            .column_names = columns,
            .values = created_values,
        }
    );
    exec_ok(db, "DROP INDEX idx_base_label ON app.base_items");
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT COUNT(*) AS index_count "
                   "FROM information_schema.STATISTICS "
                   "WHERE TABLE_SCHEMA = 'app' "
                   "AND TABLE_NAME = 'base_items' "
                   "AND INDEX_NAME = 'idx_base_label'",
            .column_count = 1,
            .row_count = 1,
            .column_names = columns,
            .values = dropped_values,
        }
    );
}

static void exercise_alter_table_ddl(mylite_db *db) {
    exec_ok(
        db,
        "ALTER TABLE app.base_items "
        "ADD COLUMN state VARCHAR(16) NOT NULL DEFAULT 'active', "
        "MODIFY COLUMN label VARCHAR(64) NOT NULL, "
        "ADD KEY idx_state (state)"
    );
    exec_ok(
        db,
        "ALTER TABLE app.base_items "
        "CHANGE COLUMN state item_state VARCHAR(16) NOT NULL DEFAULT 'active'"
    );
    exec_ok(db, "UPDATE app.base_items SET item_state = 'archived' WHERE id = 2");
    exec_ok(db, "ALTER TABLE app.base_items DROP KEY idx_state");
}

static void exercise_check_constraint_ddl(mylite_db *db) {
    exec_ok(
        db,
        "CREATE TABLE app.checked_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "qty INT NOT NULL, "
        "CONSTRAINT chk_positive_qty CHECK (qty > 0)"
        ")"
    );
    exec_ok(db, "INSERT INTO app.checked_items VALUES (1, 5)");
    expect_error(db, "INSERT INTO app.checked_items VALUES (2, 0)");
}

static void exercise_foreign_key_ddl(mylite_db *db) {
    exec_ok(
        db,
        "CREATE TABLE app.parent_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL"
        ")"
    );
    exec_ok(
        db,
        "CREATE TABLE app.child_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "parent_id INT NOT NULL, "
        "label VARCHAR(32) NOT NULL, "
        "CONSTRAINT fk_child_parent "
        "FOREIGN KEY (parent_id) REFERENCES app.parent_items (id) "
        "ON DELETE CASCADE"
        ")"
    );
    exec_ok(db, "INSERT INTO app.parent_items VALUES (1, 'parent')");
    exec_ok(db, "INSERT INTO app.child_items VALUES (1, 1, 'child')");
    expect_error(db, "INSERT INTO app.child_items VALUES (2, 99, 'orphan')");
    exec_ok(db, "DELETE FROM app.parent_items WHERE id = 1");
}

static void exercise_generated_column_ddl(mylite_db *db) {
    exec_ok(
        db,
        "CREATE TABLE app.generated_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "first_name VARCHAR(16) NOT NULL, "
        "last_name VARCHAR(16) NOT NULL, "
        "full_name VARCHAR(40) GENERATED ALWAYS AS "
        "(CONCAT(first_name, ' ', last_name)) STORED, "
        "name_length INT GENERATED ALWAYS AS "
        "(CHAR_LENGTH(CONCAT(first_name, ' ', last_name))) VIRTUAL"
        ")"
    );
    exec_ok(
        db,
        "INSERT INTO app.generated_items (id, first_name, last_name) VALUES (1, 'Ada', 'Lovelace')"
    );
    exec_ok(db, "UPDATE app.generated_items SET last_name = 'Byron' WHERE id = 1");
}

static void assert_broader_ddl_state(mylite_db *db) {
    static const char *const engine_columns[] = {"non_innodb_tables"};
    static const char *const engine_values[] = {"0"};
    static const char *const index_columns[] = {
        "base_label_indexes",
        "base_state_indexes",
        "like_qty_indexes",
    };
    static const char *const index_values[] = {"0", "0", "1"};
    static const char *const base_columns[] = {"id", "label", "qty", "item_state"};
    static const char *const base_values[] = {
        "1",
        "alpha",
        "10",
        "active",
        "2",
        "beta",
        "20",
        "archived",
    };
    static const char *const copy_columns[] = {"like_rows", "selected_rows", "selected_label"};
    static const char *const copy_values[] = {"1", "1", "beta"};
    static const char *const constraint_columns[] = {
        "checked_rows",
        "parents",
        "children",
    };
    static const char *const constraint_values[] = {"1", "0", "0"};
    static const char *const generated_columns[] = {"full_name", "name_length"};
    static const char *const generated_values[] = {"Ada Byron", "9"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT COUNT(*) AS non_innodb_tables "
                   "FROM information_schema.TABLES "
                   "WHERE TABLE_SCHEMA = 'app' "
                   "AND TABLE_TYPE = 'BASE TABLE' "
                   "AND ENGINE <> 'InnoDB'",
            .column_count = 1,
            .row_count = 1,
            .column_names = engine_columns,
            .values = engine_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "(SELECT COUNT(*) FROM information_schema.STATISTICS "
                   "WHERE TABLE_SCHEMA = 'app' "
                   "AND TABLE_NAME = 'base_items' "
                   "AND INDEX_NAME = 'idx_base_label') AS base_label_indexes, "
                   "(SELECT COUNT(*) FROM information_schema.STATISTICS "
                   "WHERE TABLE_SCHEMA = 'app' "
                   "AND TABLE_NAME = 'base_items' "
                   "AND INDEX_NAME = 'idx_state') AS base_state_indexes, "
                   "(SELECT COUNT(*) FROM information_schema.STATISTICS "
                   "WHERE TABLE_SCHEMA = 'app' "
                   "AND TABLE_NAME = 'like_items' "
                   "AND INDEX_NAME = 'idx_qty') AS like_qty_indexes",
            .column_count = 3,
            .row_count = 1,
            .column_names = index_columns,
            .values = index_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, label, qty, item_state "
                   "FROM app.base_items ORDER BY id",
            .column_count = 4,
            .row_count = 2,
            .column_names = base_columns,
            .values = base_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "(SELECT COUNT(*) FROM app.like_items) AS like_rows, "
                   "(SELECT COUNT(*) FROM app.selected_items) AS selected_rows, "
                   "(SELECT label FROM app.selected_items ORDER BY id LIMIT 1) "
                   "AS selected_label",
            .column_count = 3,
            .row_count = 1,
            .column_names = copy_columns,
            .values = copy_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "(SELECT COUNT(*) FROM app.checked_items) AS checked_rows, "
                   "(SELECT COUNT(*) FROM app.parent_items) AS parents, "
                   "(SELECT COUNT(*) FROM app.child_items) AS children",
            .column_count = 3,
            .row_count = 1,
            .column_names = constraint_columns,
            .values = constraint_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT full_name, name_length FROM app.generated_items WHERE id = 1",
            .column_count = 2,
            .row_count = 1,
            .column_names = generated_columns,
            .values = generated_values,
        }
    );
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

static void expect_error(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);

    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(strcmp(mylite_sqlstate(db), "00000") != 0);
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
    char template_path[] = "/tmp/mylite-ddl-coverage.XXXXXX";
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

static void assert_database_open_layout(const char *database_path) {
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *lock_path = path_join(database_path, "mylite.lock");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");
    char *plugin_path = path_join(run_path, "plugins");

    assert(path_exists(metadata_path));
    assert(path_exists(lock_path));
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(is_directory(run_path));
    assert(is_directory(plugin_path));

    free(plugin_path);
    free(run_path);
    free(tmp_path);
    free(data_path);
    free(lock_path);
    free(metadata_path);
}

static void assert_database_closed_layout(const char *database_path) {
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *lock_path = path_join(database_path, "mylite.lock");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");

    assert(path_exists(metadata_path));
    assert(path_exists(lock_path));
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(is_directory_empty(tmp_path));
    assert(!path_exists(run_path));

    free(run_path);
    free(tmp_path);
    free(data_path);
    free(lock_path);
    free(metadata_path);
}

static void assert_test_root_contains_only_database_and_runtime(root_entries entries) {
    DIR *directory = opendir(entries.root);
    int saw_database = 0;
    int saw_runtime = 0;

    assert(directory != NULL);
    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (strcmp(entry->d_name, entries.database_name) == 0) {
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

    return stat(path, &path_stat) == 0 && S_ISDIR(path_stat.st_mode);
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

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

typedef enum compatibility_group {
    COMPATIBILITY_GROUP_MARIADB_COMPARISON = 1,
    COMPATIBILITY_GROUP_APPLICATION_QUERIES = 2,
} compatibility_group;

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

static compatibility_group parse_compatibility_group(const char *group_name);
static void run_mariadb_comparison_group(void);
static void run_application_queries_group(void);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void create_application_schema(mylite_db *db);
static void insert_application_rows(mylite_db *db);
static void assert_application_queries(mylite_db *db);
static void archive_application_user(mylite_db *db);
static void assert_application_after_reopen(mylite_db *db);
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

int main(int argc, char **argv) {
    assert(argc == 2);

    switch (parse_compatibility_group(argv[1])) {
    case COMPATIBILITY_GROUP_MARIADB_COMPARISON:
        run_mariadb_comparison_group();
        break;
    case COMPATIBILITY_GROUP_APPLICATION_QUERIES:
        run_application_queries_group();
        break;
    }
    return 0;
}

static compatibility_group parse_compatibility_group(const char *group_name) {
    if (strcmp(group_name, "mariadb-comparison") == 0) {
        return COMPATIBILITY_GROUP_MARIADB_COMPARISON;
    }
    if (strcmp(group_name, "application-queries") == 0) {
        return COMPATIBILITY_GROUP_APPLICATION_QUERIES;
    }
    assert(0 && "unknown compatibility group");
    return COMPATIBILITY_GROUP_MARIADB_COMPARISON;
}

static void run_mariadb_comparison_group(void) {
    static const char *const arithmetic_columns[] = {"sum_value"};
    static const char *const arithmetic_values[] = {"3"};
    static const char *const string_columns[] = {"label"};
    static const char *const string_values[] = {"mylite"};
    static const char *const null_columns[] = {"value_text"};
    static const char *const null_values[] = {"fallback"};
    static const char *const date_columns[] = {"leap_day"};
    static const char *const date_values[] = {"2024-02-29"};
    static const char *const null_result_columns[] = {"missing_value"};
    static const char *const null_result_values[] = {NULL};

    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "compat-mariadb.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    assert_database_open_layout(database_path);
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT 1 + 2 AS sum_value",
            .column_count = 1,
            .row_count = 1,
            .column_names = arithmetic_columns,
            .values = arithmetic_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT CONCAT('my', 'lite') AS label",
            .column_count = 1,
            .row_count = 1,
            .column_names = string_columns,
            .values = string_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT IFNULL(NULL, 'fallback') AS value_text",
            .column_count = 1,
            .row_count = 1,
            .column_names = null_columns,
            .values = null_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT DATE_ADD('2024-02-28', INTERVAL 1 DAY) AS leap_day",
            .column_count = 1,
            .row_count = 1,
            .column_names = date_columns,
            .values = date_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT NULL AS missing_value",
            .column_count = 1,
            .row_count = 1,
            .column_names = null_result_columns,
            .values = null_result_values,
        }
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(
        (root_entries){.root = root, .database_name = "compat-mariadb.mylite"}
    );

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void run_application_queries_group(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "compat-app.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    assert_database_open_layout(database_path);
    create_application_schema(db);
    insert_application_rows(db);
    assert_application_queries(db);
    archive_application_user(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(
        (root_entries){.root = root, .database_name = "compat-app.mylite"}
    );

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert_database_open_layout(database_path);
    assert_application_after_reopen(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(
        (root_entries){.root = root, .database_name = "compat-app.mylite"}
    );

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

static void create_application_schema(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.users ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "email VARCHAR(128) NOT NULL, "
        "status VARCHAR(16) NOT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY uq_email (email), "
        "KEY idx_status (status)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.orders ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "user_id INT NOT NULL, "
        "total_cents INT NOT NULL, "
        "state VARCHAR(16) NOT NULL, "
        "created_at DATETIME NOT NULL, "
        "PRIMARY KEY (id), "
        "KEY idx_user_state (user_id, state), "
        "KEY idx_created (created_at)"
        ") ENGINE=InnoDB"
    );
}

static void insert_application_rows(mylite_db *db) {
    exec_ok(
        db,
        "INSERT INTO app.users (email, status) VALUES "
        "('alpha@example.test', 'active'), "
        "('beta@example.test', 'active'), "
        "('gamma@example.test', 'inactive'), "
        "('delta@example.test', 'active')"
    );
    exec_ok(
        db,
        "INSERT INTO app.orders (user_id, total_cents, state, created_at) VALUES "
        "(1, 1000, 'paid', '2026-01-01 10:00:00'), "
        "(1, 500, 'pending', '2026-01-02 10:00:00'), "
        "(2, 2500, 'paid', '2026-01-03 10:00:00'), "
        "(2, 750, 'paid', '2026-01-04 10:00:00'), "
        "(3, 999, 'paid', '2026-01-05 10:00:00'), "
        "(4, 100, 'pending', '2026-01-06 10:00:00')"
    );
}

static void assert_application_queries(mylite_db *db) {
    static const char *const summary_columns[] = {"email", "order_count", "total_cents"};
    static const char *const summary_values[] = {
        "beta@example.test",
        "2",
        "3250",
        "alpha@example.test",
        "1",
        "1000",
    };
    static const char *const page_columns[] = {"id", "email"};
    static const char *const page_values[] = {"2", "beta@example.test", "4", "delta@example.test"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "u.email AS email, "
                   "COUNT(o.id) AS order_count, "
                   "COALESCE(SUM(o.total_cents), 0) AS total_cents "
                   "FROM app.users u "
                   "LEFT JOIN app.orders o ON o.user_id = u.id AND o.state = 'paid' "
                   "WHERE u.status = 'active' "
                   "GROUP BY u.id, u.email "
                   "ORDER BY total_cents DESC, u.email "
                   "LIMIT 2",
            .column_count = 3,
            .row_count = 2,
            .column_names = summary_columns,
            .values = summary_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, email "
                   "FROM app.users "
                   "WHERE status = 'active' "
                   "ORDER BY id "
                   "LIMIT 2 OFFSET 1",
            .column_count = 2,
            .row_count = 2,
            .column_names = page_columns,
            .values = page_values,
        }
    );
}

static void archive_application_user(mylite_db *db) {
    exec_ok(db, "UPDATE app.users SET status = 'archived' WHERE email = 'delta@example.test'");
    assert(mylite_changes(db) == 1);
}

static void assert_application_after_reopen(mylite_db *db) {
    static const char *const active_columns[] = {"email"};
    static const char *const active_values[] = {"alpha@example.test", "beta@example.test"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT email FROM app.users WHERE status = 'active' ORDER BY id",
            .column_count = 1,
            .row_count = 2,
            .column_names = active_columns,
            .values = active_values,
        }
    );
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_OK);
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
            assert(strcmp(values[column], expected_value) == 0);
        }
    }

    ++result->seen_rows;
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-compat.XXXXXX";
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

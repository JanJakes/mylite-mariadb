#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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

static void test_innodb_transactions_survive_reopen(void);
static void test_innodb_recovers_after_process_exit(void);
static void create_accounts_table(mylite_db *db);
static void assert_accounts_after_commit_and_rollback(mylite_db *db);
static void assert_accounts_after_reopen(mylite_db *db);
static void create_events_table(mylite_db *db);
static void write_recovery_rows_in_child(open_database_paths paths);
static void assert_recovered_events(mylite_db *db);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
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
static void assert_innodb_files(const char *database_path);
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
    test_innodb_transactions_survive_reopen();
    test_innodb_recovers_after_process_exit();
    return 0;
}

static void test_innodb_transactions_survive_reopen(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "transactions.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    assert_database_open_layout(database_path);
    create_accounts_table(db);
    assert_accounts_after_commit_and_rollback(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert_innodb_files(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(root);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert_database_open_layout(database_path);
    assert_accounts_after_reopen(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert_innodb_files(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(root);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_innodb_recovers_after_process_exit(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "recovery.mylite");
    char *run_path = path_join(database_path, "run");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db = NULL;
    pid_t child;
    int child_status = 0;

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    create_events_table(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert_innodb_files(database_path);
    assert(is_directory_empty(runtime_root));

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        write_recovery_rows_in_child(paths);
    }

    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status));
    assert(WEXITSTATUS(child_status) == 0);
    assert(is_directory(run_path));
    assert(is_directory_empty(runtime_root));

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert_database_open_layout(database_path);
    assert_recovered_events(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert_innodb_files(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(root);

    free(run_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void create_accounts_table(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.accounts ("
        "id INT NOT NULL PRIMARY KEY, "
        "balance INT NOT NULL, "
        "note VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "INSERT INTO app.accounts (id, balance, note) "
        "VALUES (1, 100, 'checking'), (2, 50, 'savings')"
    );
}

static void assert_accounts_after_commit_and_rollback(mylite_db *db) {
    static const char *const balance_columns[] = {"id", "balance"};
    static const char *const balance_values[] = {"1", "70", "2", "50"};

    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.accounts SET balance = balance - 30 WHERE id = 1");
    exec_ok(db, "SAVEPOINT after_debit");
    exec_ok(db, "UPDATE app.accounts SET balance = balance + 30 WHERE id = 2");
    exec_ok(db, "ROLLBACK TO SAVEPOINT after_debit");
    exec_ok(db, "RELEASE SAVEPOINT after_debit");
    exec_ok(db, "COMMIT");

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, balance FROM app.accounts ORDER BY id",
            .column_count = 2,
            .row_count = 2,
            .column_names = balance_columns,
            .values = balance_values,
        }
    );

    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "INSERT INTO app.accounts VALUES (3, 500, 'temporary')");
    exec_ok(db, "UPDATE app.accounts SET balance = 1 WHERE id = 1");
    exec_ok(db, "ROLLBACK");

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, balance FROM app.accounts ORDER BY id",
            .column_count = 2,
            .row_count = 2,
            .column_names = balance_columns,
            .values = balance_values,
        }
    );
}

static void assert_accounts_after_reopen(mylite_db *db) {
    static const char *const columns[] = {"id", "balance", "note"};
    static const char *const values[] = {"1", "70", "checking", "2", "50", "savings"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, balance, note FROM app.accounts ORDER BY id",
            .column_count = 3,
            .row_count = 2,
            .column_names = columns,
            .values = values,
        }
    );
}

static void create_events_table(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.events ("
        "id INT NOT NULL PRIMARY KEY, "
        "message VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
}

static void write_recovery_rows_in_child(open_database_paths paths) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE);

    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "INSERT INTO app.events VALUES (1, 'committed')");
    exec_ok(db, "COMMIT");
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "INSERT INTO app.events VALUES (2, 'uncommitted')");
    _exit(0);
}

static void assert_recovered_events(mylite_db *db) {
    static const char *const columns[] = {"id", "message"};
    static const char *const values[] = {"1", "committed"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT id, message FROM app.events ORDER BY id",
            .column_count = 2,
            .row_count = 1,
            .column_names = columns,
            .values = values,
        }
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
    assert(mylite_errcode(db) == MYLITE_OK);
    assert(mylite_extended_errcode(db) == MYLITE_OK);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "00000") == 0);
    assert(strcmp(mylite_errmsg(db), "not an error") == 0);
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
    int row = result->seen_rows;

    assert(row < result->row_count);
    assert(column_count == result->column_count);
    for (int column = 0; column < column_count; ++column) {
        int index = (row * column_count) + column;
        assert(strcmp(column_names[column], result->column_names[column]) == 0);
        assert(values[column] != NULL);
        assert(strcmp(values[column], result->values[index]) == 0);
    }
    result->seen_rows += 1;
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static char *make_temp_root(void) {
    char template[] = "/tmp/mylite-test.XXXXXX";
    char *root = mkdtemp(template);

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

static void assert_database_open_layout(const char *database_path) {
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");

    assert(is_directory(database_path));
    assert(path_exists(metadata_path));
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(is_directory(run_path));

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

    assert(is_directory(database_path));
    assert(path_exists(metadata_path));
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(!path_exists(run_path));

    free(run_path);
    free(tmp_path);
    free(data_path);
    free(metadata_path);
}

static void assert_innodb_files(const char *database_path) {
    char *data_path = path_join(database_path, "datadir");
    char *system_path = path_join(data_path, "ibdata1");
    char *redo_path = path_join(data_path, "ib_logfile0");
    char *undo_path = path_join(data_path, "undo001");

    assert(path_exists(system_path));
    assert(path_exists(redo_path));
    assert(path_exists(undo_path));

    free(undo_path);
    free(redo_path);
    free(system_path);
    free(data_path);
}

static void assert_test_root_contains_only_database_and_runtime(const char *root) {
    DIR *directory = opendir(root);
    struct dirent *entry;
    int allowed_entries = 0;

    assert(directory != NULL);
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        assert(strstr(entry->d_name, ".mylite") != NULL || strcmp(entry->d_name, "runtime") == 0);
        allowed_entries += 1;
    }
    assert(errno == 0);
    assert(closedir(directory) == 0);
    assert(allowed_entries == 2);
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

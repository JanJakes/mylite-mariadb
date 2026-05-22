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

typedef struct altered_row_context {
    int rows;
} altered_row_context;

typedef struct open_database_paths {
    const char *database_path;
    const char *runtime_root;
} open_database_paths;

static void test_myisam_ddl_lifecycle_stays_in_database_directory(void);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void exec_ok(mylite_db *db, const char *sql);
static void trace_step(const char *step);
static int altered_row_callback(void *ctx, int column_count, char **values, char **column_names);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void assert_database_open_layout(const char *database_path);
static void assert_database_closed_layout(const char *database_path);
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
    test_myisam_ddl_lifecycle_stays_in_database_directory();
    return 0;
}

static void test_myisam_ddl_lifecycle_stays_in_database_directory(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ddl.mylite");
    mylite_db *db = NULL;
    char *data_path = NULL;
    char *app_schema_path = NULL;
    char *schema_options_path = NULL;
    char *old_definition_path = NULL;
    char *old_data_path = NULL;
    char *old_index_path = NULL;
    char *renamed_definition_path = NULL;
    char *renamed_data_path = NULL;
    char *renamed_index_path = NULL;
    altered_row_context ctx = {.rows = 0};
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};

    assert(mkdir(runtime_root, 0700) == 0);

    trace_step("open create");
    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    assert_database_open_layout(database_path);
    assert(is_directory_empty(runtime_root));
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.notes ("
        "id INT NOT NULL PRIMARY KEY, "
        "body VARCHAR(32) NOT NULL"
        ") ENGINE=MyISAM"
    );
    exec_ok(db, "INSERT INTO app.notes VALUES (1, 'alpha')");
    exec_ok(db, "ALTER TABLE app.notes ADD COLUMN revision INT NOT NULL DEFAULT 0");
    exec_ok(db, "UPDATE app.notes SET revision = 7 WHERE id = 1");
    exec_ok(db, "RENAME TABLE app.notes TO app.notes_archive");
    trace_step("close after rename");
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(root);

    data_path = path_join(database_path, "datadir");
    app_schema_path = path_join(data_path, "app");
    schema_options_path = path_join(app_schema_path, "db.opt");
    old_definition_path = path_join(app_schema_path, "notes.frm");
    old_data_path = path_join(app_schema_path, "notes.MYD");
    old_index_path = path_join(app_schema_path, "notes.MYI");
    renamed_definition_path = path_join(app_schema_path, "notes_archive.frm");
    renamed_data_path = path_join(app_schema_path, "notes_archive.MYD");
    renamed_index_path = path_join(app_schema_path, "notes_archive.MYI");

    assert(path_exists(schema_options_path));
    assert(!path_exists(old_definition_path));
    assert(!path_exists(old_data_path));
    assert(!path_exists(old_index_path));
    assert(path_exists(renamed_definition_path));
    assert(path_exists(renamed_data_path));
    assert(path_exists(renamed_index_path));

    trace_step("reopen");
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert_database_open_layout(database_path);
    trace_step("select renamed table");
    assert(
        mylite_exec(
            db,
            "SELECT body, revision FROM app.notes_archive WHERE id = 1",
            altered_row_callback,
            &ctx,
            NULL
        ) == MYLITE_OK
    );
    assert(ctx.rows == 1);
    exec_ok(db, "DROP TABLE app.notes_archive");
    assert(!path_exists(renamed_definition_path));
    assert(!path_exists(renamed_data_path));
    assert(!path_exists(renamed_index_path));
    exec_ok(db, "DROP DATABASE app");
    assert(!path_exists(app_schema_path));
    trace_step("close after drop");
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(root);

    free(renamed_index_path);
    free(renamed_data_path);
    free(renamed_definition_path);
    free(old_index_path);
    free(old_data_path);
    free(old_definition_path);
    free(schema_options_path);
    free(app_schema_path);
    free(data_path);
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

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    fprintf(stderr, "sql: %s\n", sql);
    fflush(stderr);
    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
}

static void trace_step(const char *step) {
    fprintf(stderr, "step: %s\n", step);
    fflush(stderr);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): required callback signature.
static int altered_row_callback(void *ctx, int column_count, char **values, char **column_names) {
    altered_row_context *altered_ctx = (altered_row_context *)ctx;
    assert(column_count == 2);
    assert(strcmp(column_names[0], "body") == 0);
    assert(strcmp(column_names[1], "revision") == 0);
    assert(values[0] != NULL);
    assert(values[1] != NULL);
    assert(strcmp(values[0], "alpha") == 0);
    assert(strcmp(values[1], "7") == 0);
    ++altered_ctx->rows;
    return 0;
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-ddl.XXXXXX";
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

static void assert_test_root_contains_only_database_and_runtime(const char *root) {
    DIR *directory = opendir(root);
    int saw_database = 0;
    int saw_runtime = 0;
    assert(directory != NULL);

    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (strcmp(entry->d_name, "ddl.mylite") == 0) {
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

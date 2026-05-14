#include <mylite/mylite.h>
#include <mylite/storage.h>

#include <assert.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct engine_context {
    int rows;
    int found_mylite;
    int supported_mylite;
} engine_context;

typedef struct table_context {
    int rows;
    int found_posts;
} table_context;

typedef struct catalog_table_context {
    const char *expected_schema_name;
    const char *expected_table_name;
    unsigned count;
    int found_expected_table;
} catalog_table_context;

static void test_show_engines_reports_mylite(void);
static void test_memory_database_has_empty_mylite_discovery(void);
static void test_create_table_persists_catalog_metadata(void);
static void assert_exec_fails(mylite_db *db, const char *sql);
static void assert_catalog_table_count(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned count
);
static int engine_callback(void *ctx, int column_count, char **values, char **column_names);
static int table_callback(void *ctx, int column_count, char **values, char **column_names);
static int row_callback(void *ctx, int column_count, char **values, char **column_names);
static int catalog_table_callback(void *ctx, const char *schema_name, const char *table_name);
static mylite_db *open_database(const char *root, char **filename);
static mylite_db *open_database_with_filename(const char *root, const char *filename);
static mylite_open_config open_config(const char *runtime_root);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void assert_no_durable_sidecars(const char *root, const char *primary_name);
static void assert_no_forbidden_sidecars(const char *path);
static void assert_only_primary_and_runtime_root(const char *root, const char *primary_name);
static int is_forbidden_sidecar_name(const char *name);
static int has_prefix(const char *value, const char *prefix);
static int has_suffix(const char *value, const char *suffix);
static int is_directory_empty(const char *path);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

int main(void) {
    test_show_engines_reports_mylite();
    test_memory_database_has_empty_mylite_discovery();
    test_create_table_persists_catalog_metadata();
    return 0;
}

static void test_show_engines_reports_mylite(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    engine_context ctx = {0};
    char *errmsg = NULL;

    assert(mylite_exec(db, "SHOW ENGINES", engine_callback, &ctx, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(ctx.rows > 0);
    assert(ctx.found_mylite);
    assert(ctx.supported_mylite);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_memory_database_has_empty_mylite_discovery(void) {
    char *root = make_temp_root();
    mylite_db *db = open_database_with_filename(root, ":memory:");
    table_context tables = {0};
    char *errmsg = NULL;

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(mylite_exec(db, "USE app", NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 0);
    assert_exec_fails(
        db,
        "CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=MYLITE"
    );

    assert(mylite_close(db) == MYLITE_OK);
    remove_tree(root);
    free(root);
}

static void test_create_table_persists_catalog_metadata(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context tables = {0};
    table_context rows = {0};
    char *errmsg = NULL;

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(mylite_exec(db, "USE app", NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(
        mylite_exec(
            db,
            "CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=MYLITE",
            NULL,
            NULL,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert_catalog_table_count(filename, "app", "posts", 1U);
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.found_posts);
    assert_exec_fails(
        db,
        "CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=MYLITE"
    );
    assert_catalog_table_count(filename, "app", "posts", 1U);
    assert(
        mylite_exec(db, "INSERT INTO posts VALUES (1, 'draft')", NULL, NULL, &errmsg) != MYLITE_OK
    );
    assert(errmsg != NULL);
    mylite_free(errmsg);
    errmsg = NULL;
    assert_exec_fails(db, "DROP TABLE posts");
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert_catalog_table_count(filename, "app", "posts", 1U);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    tables = (table_context){0};
    db = open_database_with_filename(root, filename);
    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(mylite_exec(db, "USE app", NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.found_posts);
    assert(mylite_exec(db, "SELECT * FROM posts", row_callback, &rows, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(rows.rows == 0);
    assert_catalog_table_count(filename, "app", "posts", 1U);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void assert_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) != MYLITE_OK);
    assert(errmsg != NULL);
    mylite_free(errmsg);
}

static void assert_catalog_table_count(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned count
) {
    catalog_table_context ctx = {
        .expected_schema_name = schema_name,
        .expected_table_name = table_name,
    };

    assert(
        mylite_storage_list_tables(filename, schema_name, catalog_table_callback, &ctx) ==
        MYLITE_STORAGE_OK
    );
    assert(ctx.count == count);
    assert(count == 0U || ctx.found_expected_table);
}

static int engine_callback(void *ctx, int column_count, char **values, char **column_names) {
    engine_context *engine_ctx = (engine_context *)ctx;
    (void)column_names;

    assert(column_count >= 2);
    ++engine_ctx->rows;

    if (values[0] == NULL || strcmp(values[0], "MYLITE") != 0) {
        return 0;
    }

    engine_ctx->found_mylite = 1;
    if (values[1] != NULL && strcmp(values[1], "YES") == 0) {
        engine_ctx->supported_mylite = 1;
    }

    return 0;
}

static int table_callback(void *ctx, int column_count, char **values, char **column_names) {
    table_context *table_ctx = (table_context *)ctx;
    (void)column_names;

    assert(column_count >= 1);
    ++table_ctx->rows;
    if (values[0] != NULL && strcmp(values[0], "posts") == 0) {
        table_ctx->found_posts = 1;
    }
    return 0;
}

static int row_callback(void *ctx, int column_count, char **values, char **column_names) {
    table_context *table_ctx = (table_context *)ctx;
    (void)column_count;
    (void)values;
    (void)column_names;
    ++table_ctx->rows;
    return 0;
}

static int catalog_table_callback(void *ctx, const char *schema_name, const char *table_name) {
    catalog_table_context *catalog_ctx = (catalog_table_context *)ctx;

    assert(schema_name != NULL);
    assert(table_name != NULL);
    ++catalog_ctx->count;
    if (strcmp(schema_name, catalog_ctx->expected_schema_name) == 0 &&
        strcmp(table_name, catalog_ctx->expected_table_name) == 0) {
        catalog_ctx->found_expected_table = 1;
    }
    return 0;
}

static mylite_db *open_database(const char *root, char **filename) {
    *filename = path_join(root, "storage-engine.mylite");
    return open_database_with_filename(root, *filename);
}

static mylite_db *open_database_with_filename(const char *root, const char *filename) {
    char *runtime_root = path_join(root, "runtime");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    if (mkdir(runtime_root, 0700) != 0) {
        assert(is_directory_empty(runtime_root));
    }
    assert(
        mylite_open_v2(filename, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    free(runtime_root);
    return db;
}

static mylite_open_config open_config(const char *runtime_root) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = runtime_root,
    };
    return config;
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-storage-engine.XXXXXX";
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

static void assert_no_durable_sidecars(const char *root, const char *primary_name) {
    char *runtime_root = path_join(root, "runtime");
    assert_no_forbidden_sidecars(root);
    assert_only_primary_and_runtime_root(root, primary_name);
    assert(is_directory_empty(runtime_root));
    free(runtime_root);
}

static void assert_no_forbidden_sidecars(const char *path) {
    struct stat path_stat;
    assert(lstat(path, &path_stat) == 0);

    if (!S_ISDIR(path_stat.st_mode)) {
        return;
    }

    DIR *directory = opendir(path);
    assert(directory != NULL);

    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        assert(!is_forbidden_sidecar_name(entry->d_name));

        char *child = path_join(path, entry->d_name);
        assert_no_forbidden_sidecars(child);
        free(child);
    }

    assert(closedir(directory) == 0);
}

static void assert_only_primary_and_runtime_root(const char *root, const char *primary_name) {
    DIR *directory = opendir(root);
    assert(directory != NULL);

    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, primary_name) == 0 || strcmp(entry->d_name, "runtime") == 0) {
            continue;
        }
        assert(0);
    }

    assert(closedir(directory) == 0);
}

static int is_forbidden_sidecar_name(const char *name) {
    return has_suffix(name, ".frm") || has_suffix(name, ".par") || has_suffix(name, ".ibd") ||
           has_suffix(name, ".MYD") || has_suffix(name, ".MYI") || has_suffix(name, ".MAI") ||
           has_suffix(name, ".MAD") || has_prefix(name, "ibdata") ||
           has_prefix(name, "ib_logfile") || has_prefix(name, "undo") ||
           has_prefix(name, "aria_log") || has_prefix(name, "mysql-bin") ||
           has_prefix(name, "relay-log");
}

static int has_prefix(const char *value, const char *prefix) {
    const size_t prefix_len = strlen(prefix);
    return strncmp(value, prefix, prefix_len) == 0;
}

static int has_suffix(const char *value, const char *suffix) {
    const size_t value_len = strlen(value);
    const size_t suffix_len = strlen(suffix);
    if (value_len < suffix_len) {
        return 0;
    }
    return strcmp(value + value_len - suffix_len, suffix) == 0;
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

static void remove_tree(const char *path) {
    char *runtime_root = path_join(path, "runtime");
    assert(is_directory_empty(runtime_root));
    free(runtime_root);
    remove_tree_entry(path);
}

static void remove_tree_entry(const char *path) {
    struct stat path_stat;
    assert(lstat(path, &path_stat) == 0);

    if (S_ISDIR(path_stat.st_mode)) {
        DIR *directory = opendir(path);
        assert(directory != NULL);

        for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char *child = path_join(path, entry->d_name);
            remove_tree_entry(child);
            free(child);
        }

        assert(closedir(directory) == 0);
        assert(rmdir(path) == 0);
        return;
    }

    assert(unlink(path) == 0);
}

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

enum { MYLITE_TEST_ENGINE_TABLE_COUNT = 5 };

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

static void test_engine_clauses_and_wordpress_schema_survive_reopen(void);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void create_engine_schema(mylite_db *db);
static void assert_unsupported_engine_rejected(mylite_db *db);
static void insert_engine_rows(mylite_db *db);
static void assert_engine_rows_before_reopen(mylite_db *db);
static void create_wordpress_schema(mylite_db *db);
static void insert_wordpress_rows(mylite_db *db);
static void assert_wordpress_queries(mylite_db *db);
static void assert_after_reopen(mylite_db *db);
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
static void assert_engine_files(const char *database_path);
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
    test_engine_clauses_and_wordpress_schema_survive_reopen();
    return 0;
}

static void test_engine_clauses_and_wordpress_schema_survive_reopen(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "engine-schema.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    assert_database_open_layout(database_path);
    create_engine_schema(db);
    assert_unsupported_engine_rejected(db);
    insert_engine_rows(db);
    assert_engine_rows_before_reopen(db);
    create_wordpress_schema(db);
    insert_wordpress_rows(db);
    assert_wordpress_queries(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert_engine_files(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(
        (root_entries){.root = root, .database_name = "engine-schema.mylite"}
    );

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert_database_open_layout(database_path);
    assert_after_reopen(db);
    assert_wordpress_queries(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert_engine_files(database_path);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(
        (root_entries){.root = root, .database_name = "engine-schema.mylite"}
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

static void create_engine_schema(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.innodb_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL, "
        "qty INT NOT NULL, "
        "KEY idx_label (label)"
        ") ENGINE=InnoDB"
    );
    exec_ok(
        db,
        "CREATE TABLE app.myisam_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL, "
        "qty INT NOT NULL, "
        "KEY idx_label (label)"
        ") ENGINE=MyISAM"
    );
    exec_ok(
        db,
        "CREATE TABLE app.aria_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL, "
        "qty INT NOT NULL, "
        "KEY idx_label (label)"
        ") ENGINE=Aria"
    );
    exec_ok(
        db,
        "CREATE TABLE app.memory_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL, "
        "qty INT NOT NULL, "
        "KEY idx_label (label)"
        ") ENGINE=MEMORY"
    );
    exec_ok(
        db,
        "CREATE TABLE app.default_items ("
        "id INT NOT NULL PRIMARY KEY, "
        "label VARCHAR(32) NOT NULL"
        ")"
    );
}

static void assert_unsupported_engine_rejected(mylite_db *db) {
    expect_error(
        db,
        "CREATE TABLE app.blackhole_items ("
        "id INT NOT NULL PRIMARY KEY"
        ") ENGINE=BLACKHOLE"
    );
    expect_error(
        db,
        "CREATE TABLE app.archive_items ("
        "id INT NOT NULL PRIMARY KEY"
        ") ENGINE=ARCHIVE"
    );
}

static void insert_engine_rows(mylite_db *db) {
    exec_ok(db, "INSERT INTO app.innodb_items VALUES (1, 'alpha', 10), (2, 'beta', 20)");
    exec_ok(db, "INSERT INTO app.myisam_items VALUES (1, 'alpha', 30), (2, 'beta', 40)");
    exec_ok(db, "INSERT INTO app.aria_items VALUES (1, 'alpha', 50), (2, 'beta', 60)");
    exec_ok(db, "INSERT INTO app.memory_items VALUES (1, 'alpha', 70), (2, 'beta', 80)");
    exec_ok(db, "INSERT INTO app.default_items VALUES (1, 'default-alpha')");
}

static void assert_engine_rows_before_reopen(mylite_db *db) {
    static const char *const engine_columns[] = {"TABLE_NAME", "ENGINE"};
    static const char *const engine_values[] = {
        "aria_items",
        "Aria",
        "default_items",
        "MyISAM",
        "innodb_items",
        "InnoDB",
        "memory_items",
        "MEMORY",
        "myisam_items",
        "MyISAM",
    };
    static const char *const totals_columns[] = {
        "innodb_total",
        "myisam_total",
        "aria_total",
        "memory_total",
    };
    static const char *const totals_values[] = {"30", "70", "110", "150"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT TABLE_NAME, ENGINE "
                   "FROM information_schema.TABLES "
                   "WHERE TABLE_SCHEMA = 'app' "
                   "ORDER BY TABLE_NAME",
            .column_count = 2,
            .row_count = MYLITE_TEST_ENGINE_TABLE_COUNT,
            .column_names = engine_columns,
            .values = engine_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "(SELECT SUM(qty) FROM app.innodb_items) AS innodb_total, "
                   "(SELECT SUM(qty) FROM app.myisam_items) AS myisam_total, "
                   "(SELECT SUM(qty) FROM app.aria_items) AS aria_total, "
                   "(SELECT SUM(qty) FROM app.memory_items) AS memory_total",
            .column_count = 4,
            .row_count = 1,
            .column_names = totals_columns,
            .values = totals_values,
        }
    );
}

static void create_wordpress_schema(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE wp");
    exec_ok(
        db,
        "CREATE TABLE wp.wp_options ("
        "option_id BIGINT(20) UNSIGNED NOT NULL AUTO_INCREMENT, "
        "option_name VARCHAR(191) NOT NULL DEFAULT '', "
        "option_value LONGTEXT NOT NULL, "
        "autoload VARCHAR(20) NOT NULL DEFAULT 'yes', "
        "PRIMARY KEY (option_id), "
        "UNIQUE KEY option_name (option_name), "
        "KEY autoload (autoload)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin"
    );
    exec_ok(
        db,
        "CREATE TABLE wp.wp_posts ("
        "ID BIGINT(20) UNSIGNED NOT NULL AUTO_INCREMENT, "
        "post_author BIGINT(20) UNSIGNED NOT NULL DEFAULT 0, "
        "post_date DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "post_date_gmt DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "post_content LONGTEXT NOT NULL, "
        "post_title TEXT NOT NULL, "
        "post_excerpt TEXT NOT NULL, "
        "post_status VARCHAR(20) NOT NULL DEFAULT 'publish', "
        "comment_status VARCHAR(20) NOT NULL DEFAULT 'open', "
        "ping_status VARCHAR(20) NOT NULL DEFAULT 'open', "
        "post_password VARCHAR(255) NOT NULL DEFAULT '', "
        "post_name VARCHAR(200) NOT NULL DEFAULT '', "
        "to_ping TEXT NOT NULL, "
        "pinged TEXT NOT NULL, "
        "post_modified DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "post_modified_gmt DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "post_content_filtered LONGTEXT NOT NULL, "
        "post_parent BIGINT(20) UNSIGNED NOT NULL DEFAULT 0, "
        "guid VARCHAR(255) NOT NULL DEFAULT '', "
        "menu_order INT(11) NOT NULL DEFAULT 0, "
        "post_type VARCHAR(20) NOT NULL DEFAULT 'post', "
        "post_mime_type VARCHAR(100) NOT NULL DEFAULT '', "
        "comment_count BIGINT(20) NOT NULL DEFAULT 0, "
        "PRIMARY KEY (ID), "
        "KEY post_name (post_name(191)), "
        "KEY type_status_date (post_type, post_status, post_date, ID), "
        "KEY post_parent (post_parent), "
        "KEY post_author (post_author)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin"
    );
    exec_ok(
        db,
        "CREATE TABLE wp.wp_postmeta ("
        "meta_id BIGINT(20) UNSIGNED NOT NULL AUTO_INCREMENT, "
        "post_id BIGINT(20) UNSIGNED NOT NULL DEFAULT 0, "
        "meta_key VARCHAR(255) DEFAULT NULL, "
        "meta_value LONGTEXT, "
        "PRIMARY KEY (meta_id), "
        "KEY post_id (post_id), "
        "KEY meta_key (meta_key(191))"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin"
    );
}

static void insert_wordpress_rows(mylite_db *db) {
    exec_ok(
        db,
        "INSERT INTO wp.wp_options (option_name, option_value, autoload) VALUES "
        "('siteurl', 'https://example.test', 'yes'), "
        "('blogname', 'MyLite', 'yes')"
    );
    exec_ok(
        db,
        "INSERT INTO wp.wp_posts "
        "(post_author, post_date, post_date_gmt, post_content, post_title, post_excerpt, "
        "post_name, to_ping, pinged, post_modified, post_modified_gmt, "
        "post_content_filtered, post_type, guid) "
        "VALUES "
        "(1, '2026-01-01 10:00:00', '2026-01-01 10:00:00', "
        "'Hello from MyLite', 'Hello MyLite', '', 'hello-mylite', "
        "'', '', '2026-01-01 10:00:00', '2026-01-01 10:00:00', '', 'post', "
        "'https://example.test/?p=1')"
    );
    exec_ok(
        db,
        "INSERT INTO wp.wp_postmeta (post_id, meta_key, meta_value) VALUES "
        "(1, '_mylite_marker', 'featured')"
    );
}

static void assert_wordpress_queries(mylite_db *db) {
    static const char *const option_columns[] = {"option_value"};
    static const char *const option_values[] = {"MyLite"};
    static const char *const post_columns[] = {"post_title", "meta_value"};
    static const char *const post_values[] = {"Hello MyLite", "featured"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT option_value "
                   "FROM wp.wp_options "
                   "WHERE option_name = 'blogname'",
            .column_count = 1,
            .row_count = 1,
            .column_names = option_columns,
            .values = option_values,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT p.post_title, m.meta_value "
                   "FROM wp.wp_posts p "
                   "JOIN wp.wp_postmeta m ON m.post_id = p.ID "
                   "WHERE p.post_type = 'post' AND m.meta_key = '_mylite_marker'",
            .column_count = 2,
            .row_count = 1,
            .column_names = post_columns,
            .values = post_values,
        }
    );
}

static void assert_after_reopen(mylite_db *db) {
    static const char *const row_count_columns[] = {
        "innodb_rows",
        "myisam_rows",
        "aria_rows",
        "memory_rows",
        "default_rows",
    };
    static const char *const row_count_values[] = {"2", "2", "2", "0", "1"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "(SELECT COUNT(*) FROM app.innodb_items) AS innodb_rows, "
                   "(SELECT COUNT(*) FROM app.myisam_items) AS myisam_rows, "
                   "(SELECT COUNT(*) FROM app.aria_items) AS aria_rows, "
                   "(SELECT COUNT(*) FROM app.memory_items) AS memory_rows, "
                   "(SELECT COUNT(*) FROM app.default_items) AS default_rows",
            .column_count = MYLITE_TEST_ENGINE_TABLE_COUNT,
            .row_count = 1,
            .column_names = row_count_columns,
            .values = row_count_values,
        }
    );
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    int result = mylite_exec(db, sql, NULL, NULL, &errmsg);

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
            assert(strcmp(values[column], expected_value) == 0);
        }
    }

    ++result->seen_rows;
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-engine-schema.XXXXXX";
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

static void assert_engine_files(const char *database_path) {
    char *data_path = path_join(database_path, "datadir");
    char *app_schema_path = path_join(data_path, "app");
    char *wp_schema_path = path_join(data_path, "wp");
    char *myisam_data_path = path_join(app_schema_path, "myisam_items.MYD");
    char *myisam_index_path = path_join(app_schema_path, "myisam_items.MYI");
    char *aria_data_path = path_join(app_schema_path, "aria_items.MAD");
    char *aria_index_path = path_join(app_schema_path, "aria_items.MAI");
    char *memory_definition_path = path_join(app_schema_path, "memory_items.frm");
    char *default_data_path = path_join(app_schema_path, "default_items.MYD");
    char *default_index_path = path_join(app_schema_path, "default_items.MYI");
    char *options_definition_path = path_join(wp_schema_path, "wp_options.frm");
    char *posts_definition_path = path_join(wp_schema_path, "wp_posts.frm");
    char *postmeta_definition_path = path_join(wp_schema_path, "wp_postmeta.frm");

    assert(path_exists(myisam_data_path));
    assert(path_exists(myisam_index_path));
    assert(path_exists(aria_data_path));
    assert(path_exists(aria_index_path));
    assert(path_exists(memory_definition_path));
    assert(path_exists(default_data_path));
    assert(path_exists(default_index_path));
    assert(path_exists(options_definition_path));
    assert(path_exists(posts_definition_path));
    assert(path_exists(postmeta_definition_path));

    free(postmeta_definition_path);
    free(posts_definition_path);
    free(options_definition_path);
    free(default_index_path);
    free(default_data_path);
    free(memory_definition_path);
    free(aria_index_path);
    free(aria_data_path);
    free(myisam_index_path);
    free(myisam_data_path);
    free(wp_schema_path);
    free(app_schema_path);
    free(data_path);
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

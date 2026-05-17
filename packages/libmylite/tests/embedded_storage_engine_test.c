#include <mylite/mylite.h>
#include <mylite/storage.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MYLITE_TEST_FIXTURE_DIR
#  define MYLITE_TEST_FIXTURE_DIR "."
#endif

typedef struct timed_lock_request {
    int operation;
    unsigned milliseconds;
} timed_lock_request;

static const unsigned k_busy_timeout_release_ms = 50U;
static const unsigned k_busy_timeout_wait_ms = 1000U;
static const useconds_t k_microseconds_per_millisecond = 1000U;
static const char *g_test_program_path = NULL;

typedef struct engine_context {
    int rows;
    int found_mylite;
    int found_sequence;
    int supported_mylite;
} engine_context;

typedef struct table_context {
    int rows;
} table_context;

typedef struct schema_context {
    int rows;
    int found_app;
    int found_empty_blog;
} schema_context;

typedef struct schema_option_context {
    int rows;
    const char *expected_character_set;
    const char *expected_collation;
    const char *expected_comment;
} schema_option_context;

typedef struct show_create_schema_context {
    int rows;
    const char *expected_schema_name;
    const char *expected_character_set;
    const char *expected_collation;
} show_create_schema_context;

typedef struct show_create_table_context {
    int rows;
    const char *expected_table_name;
    char *create_sql;
} show_create_table_context;

typedef struct post_row_context {
    int rows;
    int found_draft;
    int found_published;
} post_row_context;

typedef struct nullable_row_context {
    int rows;
    int found_null_row;
    int found_value_row;
} nullable_row_context;

typedef struct blob_row_context {
    int rows;
    int found_text_binary;
    int found_nulls;
    int found_empty;
    int found_large;
} blob_row_context;

typedef struct mutable_row_context {
    int rows;
    int found_updated;
    int found_untouched;
} mutable_row_context;

typedef struct alter_row_context {
    int rows;
    int found_first;
    int found_large;
} alter_row_context;

typedef struct auto_row_context {
    int rows;
    int found_first;
    int found_manual;
    int found_after_manual;
    int found_after_alter;
    int found_after_low_alter;
    int found_reopened;
    int found_after_reopened_alter;
} auto_row_context;

typedef struct id_sequence_context {
    int rows;
    int expected_count;
    const char **expected_ids;
} id_sequence_context;

typedef struct single_value_context {
    int rows;
    const char *expected_value;
} single_value_context;

typedef struct wordpress_post_context {
    int rows;
    const char *expected_status;
} wordpress_post_context;

typedef struct wordpress_join_context {
    int rows;
} wordpress_join_context;

typedef struct collation_restart_case {
    const char *table_name;
    const char *character_set_name;
    const char *collation_name;
} collation_restart_case;

typedef struct catalog_table_context {
    unsigned count;
    const char *expected_schema_name;
} catalog_table_context;

static void test_show_engines_reports_mylite(void);
static void test_unsupported_engine_request_policy(void);
static void test_blackhole_engine_routes_to_mylite(void);
static void test_memory_engine_routes_to_mylite(void);
static void test_memory_database_has_empty_mylite_discovery(void);
static void test_schema_namespaces(void);
static void test_prepared_schema_namespaces(void);
static void test_schema_options(void);
static void test_create_database_existence_options(void);
static void test_directory_free_create_database(void);
static void test_utf8mb4_unicode_ci_survives_restart(void);
static void test_collation_restart_matrix(void);
static void test_non_table_object_policy(void);
static void test_transaction_and_foreign_key_policies(void);
static void test_row_dml_transactions(void);
static void test_create_table_persists_catalog_metadata(void);
static void test_check_constraint_if_exists(void);
static void test_non_check_constraint_ddl(void);
static void test_primary_key_alter_ddl(void);
static void test_failed_add_unique_constraint_rollback(void);
static void test_create_table_if_not_exists(void);
static void test_alter_table_rebuilds_keyless_rows(void);
static void test_column_alter_if_exists(void);
static void test_generated_column_alter_ddl(void);
static void test_generated_primary_key_policy(void);
static void test_autoincrement_key_policy(void);
static void test_indexed_rows(void);
static void test_standalone_index_ddl(void);
static void test_index_ddl_if_exists(void);
static void test_index_ignorability(void);
static void test_blob_text_prefix_indexes(void);
static void test_create_table_like(void);
static void test_create_table_select(void);
static void test_create_table_select_duplicate_modes(void);
static void test_temporary_table_catalog_isolation(void);
static void test_create_or_replace_table(void);
static void test_failed_create_or_replace_rollback(void);
static void test_failed_table_ddl_rollback(void);
static void test_table_ddl_if_exists(void);
static void test_constraint_generated_dump_fixture(void);
static void test_show_create_table_round_trip(void);
static void test_constraint_generated_expression_matrix(void);
static void test_truncate_table_lifecycle(void);
static void test_wordpress_shaped_schema(void);
static void test_wordpress_installer_schema_fixture(void);
static void test_wordpress_multisite_global_schema_fixture(void);
static void test_wordpress_multisite_blog_schema_fixture(void);
static void test_buddypress_component_schema_fixture(void);
static void test_laravel_default_schema_fixture(void);
static void assert_exec_succeeds(mylite_db *db, const char *sql);
static void assert_exec_fails(mylite_db *db, const char *sql);
static void assert_exec_fails_with_message(mylite_db *db, const char *sql, const char *message);
static void assert_non_table_object_exec_fails(mylite_db *db, const char *sql);
static void assert_transaction_control_exec_fails(mylite_db *db, const char *sql);
static void assert_transaction_crash_recovery(const char *root, const char *filename);
static void assert_locking_sql_exec_fails(mylite_db *db, const char *sql);
static void assert_online_alter_exec_fails(mylite_db *db, const char *sql);
static void assert_csv_engine_exec_fails(mylite_db *db, const char *sql);
static void assert_unsupported_engine_exec_fails(mylite_db *db, const char *sql);
static void assert_partition_exec_fails(mylite_db *db, const char *sql);
static void assert_foreign_key_exec_fails(mylite_db *db, const char *sql);
static void assert_prepared_succeeds(mylite_db *db, const char *sql);
static void assert_prepared_fails(mylite_db *db, const char *sql);
static void assert_prepared_fails_with_message(mylite_db *db, const char *sql, const char *message);
static void assert_prepared_policy_fails_with_message(
    mylite_db *db,
    mylite_stmt *stmt,
    const char *message
);
static void assert_schema_options(
    mylite_db *db,
    const char *schema_name,
    const char *expected_character_set,
    const char *expected_collation,
    const char *expected_comment
);
static void assert_show_create_schema(
    mylite_db *db,
    const char *schema_name,
    const char *expected_character_set,
    const char *expected_collation
);
static char *capture_show_create_table(mylite_db *db, const char *table_name);
static void assert_catalog_table_count(
    const char *filename,
    const char *schema_name,
    unsigned count
);
static void assert_catalog_table_metadata(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const char *requested_engine_name,
    const char *effective_engine_name
);
static void assert_collation_matrix_catalog_metadata(
    const char *filename,
    const collation_restart_case *cases,
    size_t case_count
);
static void create_collation_matrix_table(mylite_db *db, const collation_restart_case *test_case);
static void assert_collation_matrix_table(mylite_db *db, const collation_restart_case *test_case);
static void assert_wordpress_catalog_metadata(const char *filename);
static void assert_wordpress_installer_catalog_metadata(const char *filename);
static void assert_wordpress_installer_seed_data(mylite_db *db);
static void assert_wordpress_multisite_global_catalog_metadata(const char *filename);
static void assert_wordpress_multisite_global_rows(mylite_db *db);
static void assert_wordpress_multisite_blog_catalog_metadata(const char *filename);
static void assert_wordpress_multisite_global_table_metadata(
    const char *filename,
    const char *schema_name
);
static void assert_wordpress_multisite_blog_rows(mylite_db *db);
static void assert_buddypress_catalog_metadata(const char *filename);
static void assert_buddypress_table_metadata(
    const char *filename,
    const char *schema_name,
    const char *table_name
);
static void assert_buddypress_rows(mylite_db *db);
static void assert_laravel_catalog_metadata(const char *filename);
static void assert_laravel_table_metadata(
    const char *filename,
    const char *schema_name,
    const char *table_name
);
static void assert_laravel_rows(mylite_db *db);
static void assert_table_collation(
    mylite_db *db,
    const char *schema_name,
    const char *table_name,
    const char *expected_collation
);
static void assert_query_single_value(mylite_db *db, const char *sql, const char *expected_value);
static void assert_check_constraint_count(
    mylite_db *db,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name,
    unsigned expected_count
);
static void assert_index_ignored(
    mylite_db *db,
    const char *schema_name,
    const char *table_name,
    const char *index_name,
    const char *expected_ignored
);
static void assert_warning_message_contains(mylite_db *db, const char *expected_message);
static void exec_sql_fixture(mylite_db *db, const char *fixture_name);
static int sql_fixture_cursor_is_separator(
    char **cursor,
    int *quote,
    int *line_comment,
    int *escaped
);
static void exec_sql_statement_if_present(mylite_db *db, const char *statement);
static int is_sql_statement_empty(const char *statement);
static char *read_text_file(const char *path);
static int engine_callback(void *ctx, int column_count, char **values, char **column_names);
static int schema_callback(void *ctx, int column_count, char **values, char **column_names);
static int schema_option_callback(void *ctx, int column_count, char **values, char **column_names);
static int show_create_schema_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);
static int show_create_table_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);
static int table_callback(void *ctx, int column_count, char **values, char **column_names);
static int row_callback(void *ctx, int column_count, char **values, char **column_names);
static int post_row_callback(void *ctx, int column_count, char **values, char **column_names);
static int nullable_row_callback(void *ctx, int column_count, char **values, char **column_names);
static int blob_row_callback(void *ctx, int column_count, char **values, char **column_names);
static int mutable_row_callback(void *ctx, int column_count, char **values, char **column_names);
static int alter_row_callback(void *ctx, int column_count, char **values, char **column_names);
static int auto_row_callback(void *ctx, int column_count, char **values, char **column_names);
static int id_sequence_callback(void *ctx, int column_count, char **values, char **column_names);
static int single_value_callback(void *ctx, int column_count, char **values, char **column_names);
static int wordpress_post_callback(void *ctx, int column_count, char **values, char **column_names);
static int wordpress_join_callback(void *ctx, int column_count, char **values, char **column_names);
static int catalog_table_callback(void *ctx, const char *schema_name, const char *table_name);
static mylite_db *open_database(const char *root, char **filename);
static mylite_db *open_database_with_filename(const char *root, const char *filename);
static mylite_db *open_database_with_runtime_name(
    const char *root,
    const char *filename,
    const char *runtime_name
);
static mylite_open_config open_config(const char *runtime_root);
static void run_transaction_crash_child(const char *root, const char *filename);
static pid_t hold_test_lock_for(const char *filename, timed_lock_request request);
static void wait_test_lock_child(pid_t pid);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static char *transaction_journal_path(const char *filename);
static void assert_no_durable_sidecars(const char *root, const char *primary_name);
static void assert_no_runtime_schema_directory(const char *root, const char *schema_name);
static void assert_no_forbidden_sidecars(const char *path);
static void assert_only_primary_and_runtime_root(const char *root, const char *primary_name);
static int is_forbidden_sidecar_name(const char *name);
static int has_prefix(const char *value, const char *prefix);
static int has_suffix(const char *value, const char *suffix);
static int is_directory_empty(const char *path);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--transaction-crash-child") == 0) {
        run_transaction_crash_child(argv[2], argv[3]);
    }

    assert(argc == 1);
    g_test_program_path = argv[0];

    test_show_engines_reports_mylite();
    test_unsupported_engine_request_policy();
    test_blackhole_engine_routes_to_mylite();
    test_memory_engine_routes_to_mylite();
    test_memory_database_has_empty_mylite_discovery();
    test_schema_namespaces();
    test_prepared_schema_namespaces();
    test_schema_options();
    test_create_database_existence_options();
    test_directory_free_create_database();
    test_utf8mb4_unicode_ci_survives_restart();
    test_collation_restart_matrix();
    test_non_table_object_policy();
    test_transaction_and_foreign_key_policies();
    test_row_dml_transactions();
    test_create_table_persists_catalog_metadata();
    test_check_constraint_if_exists();
    test_non_check_constraint_ddl();
    test_primary_key_alter_ddl();
    test_failed_add_unique_constraint_rollback();
    test_create_table_if_not_exists();
    test_alter_table_rebuilds_keyless_rows();
    test_column_alter_if_exists();
    test_generated_column_alter_ddl();
    test_generated_primary_key_policy();
    test_autoincrement_key_policy();
    test_indexed_rows();
    test_standalone_index_ddl();
    test_index_ddl_if_exists();
    test_index_ignorability();
    test_blob_text_prefix_indexes();
    test_create_table_like();
    test_create_table_select();
    test_create_table_select_duplicate_modes();
    test_temporary_table_catalog_isolation();
    test_create_or_replace_table();
    test_failed_create_or_replace_rollback();
    test_failed_table_ddl_rollback();
    test_table_ddl_if_exists();
    test_constraint_generated_dump_fixture();
    test_show_create_table_round_trip();
    test_constraint_generated_expression_matrix();
    test_truncate_table_lifecycle();
    test_wordpress_shaped_schema();
    test_wordpress_installer_schema_fixture();
    test_wordpress_multisite_global_schema_fixture();
    test_wordpress_multisite_blog_schema_fixture();
    test_buddypress_component_schema_fixture();
    test_laravel_default_schema_fixture();
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
    assert(!ctx.found_sequence);

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(db, "CREATE TABLE seq_1_to_10 (seq INT NOT NULL PRIMARY KEY)");
    assert_catalog_table_metadata(filename, "app", "seq_1_to_10", "DEFAULT", "MYLITE");
    assert_exec_succeeds(db, "INSERT INTO seq_1_to_10 VALUES (42)");
    assert_query_single_value(db, "SELECT seq FROM seq_1_to_10", "42");

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_unsupported_engine_request_policy(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE csv_comment ("
        "id INT NOT NULL PRIMARY KEY COMMENT 'ENGINE=CSV ENGINE=ARCHIVE', "
        "engine_label VARCHAR(16)"
        ") ENGINE InnoDB"
    );
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "csv_comment", "InnoDB", "MYLITE");

    assert_csv_engine_exec_fails(
        db,
        "CREATE TABLE csv_posts (id INT NOT NULL PRIMARY KEY) ENGINE=CSV"
    );
    assert(mylite_storage_table_exists(filename, "app", "csv_posts") == MYLITE_STORAGE_NOTFOUND);
    assert_catalog_table_count(filename, "app", 1U);
    assert_csv_engine_exec_fails(
        db,
        "CREATE TEMPORARY TABLE csv_temp_posts (id INT NOT NULL PRIMARY KEY) ENGINE = CSV"
    );
    assert_csv_engine_exec_fails(
        db,
        "CREATE TABLE csv_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE CSV"
    );
    assert_csv_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE=CSV");
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "csv_comment", "InnoDB", "MYLITE");
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE archive_posts (id INT NOT NULL PRIMARY KEY) ENGINE=ARCHIVE"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "archive_posts") == MYLITE_STORAGE_NOTFOUND
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE archive_quoted_posts (id INT NOT NULL PRIMARY KEY) ENGINE='ARCHIVE'"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "archive_quoted_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE archive_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE ARCHIVE"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "archive_no_equal_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE connect_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE CONNECT"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "connect_no_equal_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE federated_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE FEDERATED"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "federated_no_equal_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE mrg_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE MRG_MyISAM"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "mrg_no_equal_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE sequence_posts (id INT NOT NULL PRIMARY KEY) ENGINE=SEQUENCE"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "sequence_posts") == MYLITE_STORAGE_NOTFOUND
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE sequence_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE SEQUENCE"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "sequence_no_equal_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE=ARCHIVE");
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE ARCHIVE");
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE ROCKSDB");
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE=SEQUENCE");
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE SEQUENCE");
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "csv_comment", "InnoDB", "MYLITE");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_blackhole_engine_routes_to_mylite(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    unsigned long long row_count = 0ULL;
    char *show_create = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE blackhole_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL, "
        "KEY title_key (title)"
        ") ENGINE=BLACKHOLE"
    );
    assert(mylite_storage_table_exists(filename, "app", "blackhole_posts") == MYLITE_STORAGE_OK);
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "blackhole_posts", "BLACKHOLE", "MYLITE");
    show_create = capture_show_create_table(db, "blackhole_posts");
    assert(strstr(show_create, "ENGINE=BLACKHOLE") != NULL);
    free(show_create);
    assert_exec_succeeds(db, "INSERT INTO blackhole_posts VALUES (1, 'first'), (2, 'second')");
    assert(
        mylite_storage_count_rows(filename, "app", "blackhole_posts", &row_count) ==
        MYLITE_STORAGE_OK
    );
    assert(row_count == 0ULL);
    assert_query_single_value(db, "SELECT COUNT(*) FROM blackhole_posts", "0");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM blackhole_posts FORCE INDEX (title_key) WHERE title = 'first'",
        "0"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE app");
    assert_catalog_table_metadata(filename, "app", "blackhole_posts", "BLACKHOLE", "MYLITE");
    show_create = capture_show_create_table(db, "blackhole_posts");
    assert(strstr(show_create, "ENGINE=BLACKHOLE") != NULL);
    free(show_create);
    row_count = 0ULL;
    assert(
        mylite_storage_count_rows(filename, "app", "blackhole_posts", &row_count) ==
        MYLITE_STORAGE_OK
    );
    assert(row_count == 0ULL);
    assert_query_single_value(db, "SELECT COUNT(*) FROM blackhole_posts", "0");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM blackhole_posts FORCE INDEX (title_key) WHERE title = 'second'",
        "0"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_memory_engine_routes_to_mylite(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    unsigned long long durable_row_count = 0ULL;
    char *show_create = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE memory_posts ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "title VARCHAR(64) NOT NULL, "
        "PRIMARY KEY USING BTREE (id), "
        "UNIQUE KEY title_key USING BTREE (title)"
        ") ENGINE=MEMORY AUTO_INCREMENT=10"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE heap_posts (id INT NOT NULL, title VARCHAR(64) NOT NULL) ENGINE=HEAP"
    );
    assert_catalog_table_count(filename, "app", 2U);
    assert_catalog_table_metadata(filename, "app", "memory_posts", "MEMORY", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "heap_posts", "HEAP", "MYLITE");
    show_create = capture_show_create_table(db, "memory_posts");
    assert(strstr(show_create, "ENGINE=MEMORY") != NULL);
    free(show_create);
    show_create = capture_show_create_table(db, "heap_posts");
    assert(strstr(show_create, "ENGINE=HEAP") != NULL);
    free(show_create);

    assert_exec_succeeds(
        db,
        "CREATE TABLE memory_rename_source ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=MEMORY"
    );
    assert_exec_succeeds(db, "INSERT INTO memory_rename_source VALUES (1, 'rename-row')");
    assert_exec_succeeds(db, "RENAME TABLE memory_rename_source TO memory_rename_target");
    assert_query_single_value(
        db,
        "SELECT title FROM memory_rename_target WHERE id = 1",
        "rename-row"
    );
    assert_exec_succeeds(db, "DROP TABLE memory_rename_target");
    assert(
        mylite_storage_table_exists(filename, "app", "memory_rename_target") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "app", 2U);

    assert_exec_succeeds(db, "INSERT INTO memory_posts (title) VALUES ('first'), ('second')");
    assert_exec_succeeds(db, "INSERT INTO heap_posts VALUES (1, 'heap-row')");
    assert(
        mylite_storage_count_rows(filename, "app", "memory_posts", &durable_row_count) ==
        MYLITE_STORAGE_OK
    );
    assert(durable_row_count == 0ULL);
    assert_query_single_value(db, "SELECT COUNT(*) FROM memory_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM memory_posts FORCE INDEX (title_key) WHERE title = 'first'",
        "10"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM heap_posts", "1");
    assert_exec_fails(db, "INSERT INTO memory_posts (title) VALUES ('first')");
    assert_query_single_value(db, "SELECT COUNT(*) FROM memory_posts", "2");
    assert_exec_succeeds(db, "UPDATE memory_posts SET title = 'updated' WHERE id = 11");
    assert_query_single_value(
        db,
        "SELECT title FROM memory_posts FORCE INDEX (title_key) WHERE title = 'updated'",
        "updated"
    );
    assert_exec_succeeds(db, "DELETE FROM memory_posts WHERE id = 10");
    assert_query_single_value(db, "SELECT COUNT(*) FROM memory_posts", "1");
    assert_exec_succeeds(db, "TRUNCATE TABLE memory_posts");
    assert_query_single_value(db, "SELECT COUNT(*) FROM memory_posts", "0");
    assert_exec_succeeds(db, "INSERT INTO memory_posts (title) VALUES ('after-truncate')");
    assert_query_single_value(
        db,
        "SELECT id FROM memory_posts FORCE INDEX (title_key) WHERE title = 'after-truncate'",
        "1"
    );
    assert_exec_fails(db, "CREATE TABLE memory_blob_posts (body TEXT) ENGINE=MEMORY");
    assert(
        mylite_storage_table_exists(filename, "app", "memory_blob_posts") == MYLITE_STORAGE_NOTFOUND
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE app");
    assert_catalog_table_count(filename, "app", 2U);
    assert_catalog_table_metadata(filename, "app", "memory_posts", "MEMORY", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "heap_posts", "HEAP", "MYLITE");
    assert_query_single_value(db, "SELECT COUNT(*) FROM memory_posts", "0");
    assert_query_single_value(db, "SELECT COUNT(*) FROM heap_posts", "0");
    assert_exec_succeeds(db, "INSERT INTO memory_posts (title) VALUES ('after-reopen')");
    assert_query_single_value(
        db,
        "SELECT id FROM memory_posts FORCE INDEX (title_key) WHERE title = 'after-reopen'",
        "1"
    );
    assert(
        mylite_storage_count_rows(filename, "app", "memory_posts", &durable_row_count) ==
        MYLITE_STORAGE_OK
    );
    assert(durable_row_count == 0ULL);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_memory_database_has_empty_mylite_discovery(void) {
    char *root = make_temp_root();
    mylite_db *db = open_database_with_filename(root, ":memory:");
    table_context tables = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
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

static void test_schema_namespaces(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    schema_context schemas = {0};
    table_context tables = {0};
    table_context rows = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "CREATE DATABASE empty_blog");
    assert(mylite_storage_schema_exists(filename, "app") == MYLITE_STORAGE_OK);
    assert(mylite_storage_schema_exists(filename, "empty_blog") == MYLITE_STORAGE_OK);
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(db, "CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR(64))");
    assert_exec_succeeds(db, "INSERT INTO posts VALUES (1, 'first')");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "app");
    assert_no_runtime_schema_directory(root, "empty_blog");
    assert_exec_succeeds(db, "USE app");
    assert(mylite_exec(db, "SHOW DATABASES", schema_callback, &schemas, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(schemas.found_app);
    assert(schemas.found_empty_blog);
    assert(schemas.rows == 2);
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 1);
    assert(mylite_exec(db, "SELECT id FROM posts", row_callback, &rows, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(rows.rows == 1);
    assert_exec_succeeds(db, "USE empty_blog");
    tables = (table_context){0};
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 0);

    assert_exec_succeeds(db, "DROP DATABASE empty_blog");
    assert(mylite_storage_schema_exists(filename, "empty_blog") == MYLITE_STORAGE_NOTFOUND);
    assert_exec_fails(db, "USE empty_blog");
    assert_exec_succeeds(db, "DROP DATABASE app");
    assert(mylite_storage_schema_exists(filename, "app") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_prepared_schema_namespaces(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_prepared_succeeds(db, "CREATE SCHEMA prepared_app");
    assert(mylite_storage_schema_exists(filename, "prepared_app") == MYLITE_STORAGE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE prepared_app");
    assert_prepared_succeeds(db, "DROP SCHEMA prepared_app");
    assert(mylite_storage_schema_exists(filename, "prepared_app") == MYLITE_STORAGE_NOTFOUND);
    assert_exec_fails(db, "USE prepared_app");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_schema_options(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_storage_schema_metadata metadata = {0};

    assert_exec_succeeds(
        db,
        "CREATE DATABASE option_app DEFAULT CHARACTER SET latin1 COLLATE latin1_bin "
        "COMMENT 'first comment'"
    );
    assert_schema_options(db, "option_app", "latin1", "latin1_bin", "first comment");
    assert_show_create_schema(db, "option_app", "latin1", "latin1_bin");
    assert(
        mylite_storage_read_schema_definition(filename, "option_app", &metadata) ==
        MYLITE_STORAGE_OK
    );
    assert(strcmp(metadata.default_character_set_name, "latin1") == 0);
    assert(strcmp(metadata.default_collation_name, "latin1_bin") == 0);
    assert(strcmp(metadata.schema_comment, "first comment") == 0);
    mylite_storage_free(metadata.default_character_set_name);
    mylite_storage_free(metadata.default_collation_name);
    mylite_storage_free(metadata.schema_comment);

    assert_exec_succeeds(
        db,
        "ALTER DATABASE option_app DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin "
        "COMMENT 'updated ''comment'"
    );
    assert_schema_options(db, "option_app", "utf8mb4", "utf8mb4_bin", "updated 'comment");
    assert_show_create_schema(db, "option_app", "utf8mb4", "utf8mb4_bin");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "option_app");
    assert_exec_succeeds(db, "USE option_app");
    assert_schema_options(db, "option_app", "utf8mb4", "utf8mb4_bin", "updated 'comment");
    assert_show_create_schema(db, "option_app", "utf8mb4", "utf8mb4_bin");
    assert_exec_succeeds(
        db,
        "ALTER DATABASE option_app DEFAULT CHARACTER SET latin1 COLLATE latin1_bin "
        "COMMENT 'reopened comment'"
    );
    assert_schema_options(db, "option_app", "latin1", "latin1_bin", "reopened comment");
    assert_show_create_schema(db, "option_app", "latin1", "latin1_bin");
    assert_exec_succeeds(db, "DROP DATABASE option_app");
    assert(mylite_storage_schema_exists(filename, "option_app") == MYLITE_STORAGE_NOTFOUND);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_create_database_existence_options(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context tables = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(
        db,
        "CREATE DATABASE replace_schema DEFAULT CHARACTER SET latin1 COLLATE latin1_bin "
        "COMMENT 'old comment'"
    );
    assert_exec_succeeds(db, "USE replace_schema");
    assert_exec_succeeds(
        db,
        "CREATE TABLE posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(64) NOT NULL, "
        "UNIQUE KEY slug_key (slug)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "INSERT INTO posts VALUES (1, 'old-post')");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "replace_schema");
    assert_exec_fails(db, "CREATE DATABASE replace_schema");
    assert_no_runtime_schema_directory(root, "replace_schema");
    assert_catalog_table_count(filename, "replace_schema", 1U);
    assert_exec_succeeds(db, "USE replace_schema");
    assert_query_single_value(db, "SELECT slug FROM posts WHERE id = 1", "old-post");

    assert_exec_succeeds(
        db,
        "CREATE DATABASE IF NOT EXISTS replace_schema "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin COMMENT 'ignored comment'"
    );
    assert_warning_message_contains(db, "replace_schema");
    assert_no_runtime_schema_directory(root, "replace_schema");
    assert_schema_options(db, "replace_schema", "latin1", "latin1_bin", "old comment");
    assert_query_single_value(db, "SELECT slug FROM posts FORCE INDEX (slug_key)", "old-post");

    assert_exec_succeeds(
        db,
        "CREATE OR REPLACE DATABASE replace_schema "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin COMMENT 'replacement comment'"
    );
    assert_no_runtime_schema_directory(root, "replace_schema");
    assert(mylite_storage_schema_exists(filename, "replace_schema") == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_table_exists(filename, "replace_schema", "posts") == MYLITE_STORAGE_NOTFOUND
    );
    assert_query_single_value(db, "SELECT COALESCE(DATABASE(), '')", "");
    assert_schema_options(db, "replace_schema", "utf8mb4", "utf8mb4_bin", "replacement comment");
    assert_exec_succeeds(db, "USE replace_schema");
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 0);
    assert_exec_fails(db, "SELECT COUNT(*) FROM posts");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "replace_schema");
    assert_exec_succeeds(db, "USE replace_schema");
    assert_schema_options(db, "replace_schema", "utf8mb4", "utf8mb4_bin", "replacement comment");
    tables = (table_context){0};
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 0);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_directory_free_create_database(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context tables = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(
        db,
        "CREATE DATABASE direct_schema DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin "
        "COMMENT 'direct comment'"
    );
    assert(mylite_changes(db) == 1);
    assert(mylite_storage_schema_exists(filename, "direct_schema") == MYLITE_STORAGE_OK);
    assert_no_runtime_schema_directory(root, "direct_schema");
    assert_schema_options(db, "direct_schema", "utf8mb4", "utf8mb4_bin", "direct comment");
    assert_show_create_schema(db, "direct_schema", "utf8mb4", "utf8mb4_bin");
    assert_query_single_value(
        db,
        "SELECT SCHEMA_NAME FROM INFORMATION_SCHEMA.SCHEMATA "
        "WHERE SCHEMA_NAME='direct_schema'",
        "direct_schema"
    );
    assert_exec_succeeds(db, "USE direct_schema");
    assert_no_runtime_schema_directory(root, "direct_schema");
    assert_exec_succeeds(
        db,
        "CREATE TABLE posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(64) NOT NULL, "
        "UNIQUE KEY slug_key (slug)"
        ") ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "direct_schema", 1U);
    assert_catalog_table_metadata(filename, "direct_schema", "posts", "InnoDB", "MYLITE");
    assert_no_runtime_schema_directory(root, "direct_schema");
    assert_exec_succeeds(db, "INSERT INTO posts VALUES (1, 'catalog-only')");
    assert_query_single_value(db, "SELECT slug FROM posts FORCE INDEX (slug_key)", "catalog-only");

    assert_exec_succeeds(
        db,
        "CREATE SCHEMA alias_schema DEFAULT CHARACTER SET latin1 COLLATE latin1_bin "
        "COMMENT 'alias comment'"
    );
    assert(mylite_storage_schema_exists(filename, "alias_schema") == MYLITE_STORAGE_OK);
    assert_no_runtime_schema_directory(root, "alias_schema");
    assert_schema_options(db, "alias_schema", "latin1", "latin1_bin", "alias comment");

    assert_exec_succeeds(
        db,
        "CREATE DATABASE IF NOT EXISTS optional_schema "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci COMMENT 'optional comment'"
    );
    assert(mylite_warning_count(db) == 0U);
    assert_no_runtime_schema_directory(root, "optional_schema");
    assert_schema_options(
        db,
        "optional_schema",
        "utf8mb4",
        "utf8mb4_unicode_ci",
        "optional comment"
    );

    assert_exec_succeeds(
        db,
        "CREATE OR REPLACE DATABASE replace_missing "
        "DEFAULT CHARACTER SET latin1 COLLATE latin1_swedish_ci COMMENT 'replace missing'"
    );
    assert(mylite_changes(db) == 1);
    assert_no_runtime_schema_directory(root, "replace_missing");
    assert_schema_options(db, "replace_missing", "latin1", "latin1_swedish_ci", "replace missing");

    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "direct_schema");
    assert_no_runtime_schema_directory(root, "alias_schema");
    assert_no_runtime_schema_directory(root, "optional_schema");
    assert_no_runtime_schema_directory(root, "replace_missing");
    assert_exec_succeeds(db, "USE direct_schema");
    assert_schema_options(db, "direct_schema", "utf8mb4", "utf8mb4_bin", "direct comment");
    assert_show_create_schema(db, "direct_schema", "utf8mb4", "utf8mb4_bin");
    tables = (table_context){0};
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 1);
    assert_catalog_table_count(filename, "direct_schema", 1U);
    assert_catalog_table_metadata(filename, "direct_schema", "posts", "InnoDB", "MYLITE");
    assert_query_single_value(db, "SELECT slug FROM posts FORCE INDEX (slug_key)", "catalog-only");
    assert_exec_succeeds(db, "USE alias_schema");
    assert_schema_options(db, "alias_schema", "latin1", "latin1_bin", "alias comment");
    assert_exec_succeeds(db, "USE optional_schema");
    assert_schema_options(
        db,
        "optional_schema",
        "utf8mb4",
        "utf8mb4_unicode_ci",
        "optional comment"
    );
    assert_exec_succeeds(db, "USE replace_missing");
    assert_schema_options(db, "replace_missing", "latin1", "latin1_swedish_ci", "replace missing");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_utf8mb4_unicode_ci_survives_restart(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    char *errmsg = NULL;

    assert_exec_succeeds(
        db,
        "CREATE DATABASE unicode_app DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(db, "USE unicode_app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE terms ("
        "id BIGINT NOT NULL AUTO_INCREMENT, "
        "name VARCHAR(191) NOT NULL, "
        "slug VARCHAR(191) NOT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY name_key (name), "
        "KEY slug_key (slug)"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_table_collation(db, "unicode_app", "terms", "utf8mb4_unicode_ci");
    assert_exec_succeeds(
        db,
        "INSERT INTO terms (name, slug) VALUES ('Cafe', 'cafe'), ('Resume', 'resume')"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    for (unsigned i = 0; i < 3; ++i) {
        single_value_context slug = {
            .expected_value = "resume",
        };

        db = open_database_with_filename(root, filename);
        assert_exec_succeeds(db, "USE unicode_app");
        assert_table_collation(db, "unicode_app", "terms", "utf8mb4_unicode_ci");
        assert_exec_fails(db, "INSERT INTO terms (name, slug) VALUES ('Resume', 'duplicate')");
        assert(
            mylite_exec(
                db,
                "SELECT slug FROM terms FORCE INDEX (name_key) WHERE name = 'Resume'",
                single_value_callback,
                &slug,
                &errmsg
            ) == MYLITE_OK
        );
        assert(errmsg == NULL);
        assert(slug.rows == 1);
        assert(mylite_close(db) == MYLITE_OK);
        assert_no_durable_sidecars(root, "storage-engine.mylite");
    }

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_collation_restart_matrix(void) {
    const collation_restart_case cases[] = {
        {
            .table_name = "collation_utf8mb4_general_ci",
            .character_set_name = "utf8mb4",
            .collation_name = "utf8mb4_general_ci",
        },
        {
            .table_name = "collation_utf8mb4_bin",
            .character_set_name = "utf8mb4",
            .collation_name = "utf8mb4_bin",
        },
        {
            .table_name = "collation_utf8mb4_unicode_ci",
            .character_set_name = "utf8mb4",
            .collation_name = "utf8mb4_unicode_ci",
        },
        {
            .table_name = "collation_utf8mb4_unicode_520_ci",
            .character_set_name = "utf8mb4",
            .collation_name = "utf8mb4_unicode_520_ci",
        },
        {
            .table_name = "collation_latin1_swedish_ci",
            .character_set_name = "latin1",
            .collation_name = "latin1_swedish_ci",
        },
    };
    const size_t case_count = sizeof(cases) / sizeof(cases[0]);
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE collation_matrix");
    assert_exec_succeeds(db, "USE collation_matrix");
    for (size_t i = 0; i < case_count; ++i) {
        create_collation_matrix_table(db, cases + i);
    }
    assert_collation_matrix_catalog_metadata(filename, cases, case_count);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    for (unsigned round = 0; round < 2; ++round) {
        db = open_database_with_filename(root, filename);
        assert_exec_succeeds(db, "USE collation_matrix");
        assert_collation_matrix_catalog_metadata(filename, cases, case_count);
        for (size_t i = 0; i < case_count; ++i) {
            assert_collation_matrix_table(db, cases + i);
        }
        assert(mylite_close(db) == MYLITE_OK);
        assert_no_durable_sidecars(root, "storage-engine.mylite");
    }

    free(filename);
    remove_tree(root);
    free(root);
}

static void assert_collation_matrix_catalog_metadata(
    const char *filename,
    const collation_restart_case *cases,
    size_t case_count
) {
    assert_catalog_table_count(filename, "collation_matrix", (unsigned)case_count);
    for (size_t i = 0; i < case_count; ++i) {
        assert_catalog_table_metadata(
            filename,
            "collation_matrix",
            cases[i].table_name,
            "InnoDB",
            "MYLITE"
        );
    }
}

static void create_collation_matrix_table(mylite_db *db, const collation_restart_case *test_case) {
    char sql[640];
    int written = snprintf(
        sql,
        sizeof(sql),
        "CREATE TABLE %s ("
        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "name VARCHAR(191) NOT NULL, "
        "slug VARCHAR(191) NOT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY name_key (name), "
        "KEY slug_key (slug)"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET %s COLLATE %s",
        test_case->table_name,
        test_case->character_set_name,
        test_case->collation_name
    );
    assert(written > 0);
    assert((size_t)written < sizeof(sql));
    assert_exec_succeeds(db, sql);

    written = snprintf(
        sql,
        sizeof(sql),
        "INSERT INTO %s (name, slug) VALUES ('Cafe', 'cafe'), ('Resume', 'resume')",
        test_case->table_name
    );
    assert(written > 0);
    assert((size_t)written < sizeof(sql));
    assert_exec_succeeds(db, sql);
}

static void assert_collation_matrix_table(mylite_db *db, const collation_restart_case *test_case) {
    char sql[320];
    single_value_context slug = {
        .expected_value = "resume",
    };
    char *errmsg = NULL;

    assert_table_collation(
        db,
        "collation_matrix",
        test_case->table_name,
        test_case->collation_name
    );

    int written = snprintf(
        sql,
        sizeof(sql),
        "INSERT INTO %s (name, slug) VALUES ('Resume', 'duplicate')",
        test_case->table_name
    );
    assert(written > 0);
    assert((size_t)written < sizeof(sql));
    assert_exec_fails(db, sql);

    written = snprintf(
        sql,
        sizeof(sql),
        "SELECT slug FROM %s FORCE INDEX (name_key) WHERE name = 'Resume'",
        test_case->table_name
    );
    assert(written > 0);
    assert((size_t)written < sizeof(sql));
    assert(mylite_exec(db, sql, single_value_callback, &slug, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(slug.rows == 1);
}

static void test_non_table_object_policy(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(db, "CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR(64))");

    assert_non_table_object_exec_fails(db, "CREATE VIEW blocked_view AS SELECT id FROM posts");
    assert_non_table_object_exec_fails(
        db,
        "CREATE TRIGGER blocked_trigger BEFORE INSERT ON posts "
        "FOR EACH ROW SET @mylite_blocked = 1"
    );
    assert_non_table_object_exec_fails(db, "CREATE PROCEDURE blocked_proc() SELECT 1");
    assert_non_table_object_exec_fails(db, "CREATE FUNCTION blocked_func() RETURNS INT RETURN 1");
    assert_non_table_object_exec_fails(db, "CALL blocked_proc()");
    assert_non_table_object_exec_fails(db, "CREATE SEQUENCE blocked_seq");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_transaction_and_foreign_key_policies(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    single_value_context count = {.expected_value = "1"};
    single_value_context child_count = {.expected_value = "1"};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR(64)) ENGINE=InnoDB"
    );

    assert_transaction_control_exec_fails(db, "SAVEPOINT mylite_probe");
    assert_exec_succeeds(db, "START TRANSACTION READ WRITE");
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "COMMIT AND CHAIN");
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "START TRANSACTION READ ONLY");
    assert_exec_succeeds(db, "ROLLBACK");
    assert_transaction_control_exec_fails(db, "COMMIT RELEASE");
    assert_exec_succeeds(db, "SET completion_type=CHAIN");
    assert_exec_succeeds(db, "SET completion_type=DEFAULT");
    assert_exec_succeeds(db, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED");
    assert_exec_succeeds(db, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    assert_exec_succeeds(db, "SET LOCAL TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    assert_exec_succeeds(db, "SET transaction_isolation='READ-COMMITTED'");
    assert_exec_succeeds(db, "SET SESSION tx_isolation='SERIALIZABLE'");
    assert_exec_succeeds(db, "SET @@transaction_isolation='READ-UNCOMMITTED'");
    assert_exec_succeeds(db, "SET transaction_read_only=0");
    assert_exec_succeeds(db, "SET @@transaction_read_only=0");
    assert_transaction_control_exec_fails(db, "SET GLOBAL transaction_isolation='READ-COMMITTED'");
    assert_transaction_control_exec_fails(db, "SET @@global.transaction_read_only=1");
    assert_transaction_control_exec_fails(
        db,
        "SET GLOBAL TRANSACTION ISOLATION LEVEL READ COMMITTED"
    );
    assert_transaction_control_exec_fails(db, "SET GLOBAL autocommit=0");
    assert_exec_succeeds(db, "SET autocommit=DEFAULT");
    assert_locking_sql_exec_fails(db, "LOCK TABLES posts WRITE");
    assert_locking_sql_exec_fails(db, "UNLOCK TABLES");
    assert_locking_sql_exec_fails(db, "SELECT id FROM posts FOR UPDATE");
    assert_locking_sql_exec_fails(db, "SELECT id FROM posts LOCK IN SHARE MODE");
    assert_locking_sql_exec_fails(db, "SELECT GET_LOCK('mylite-lock', 1)");
    assert_online_alter_exec_fails(db, "ALTER ONLINE TABLE posts ADD COLUMN blocked_online INT");
    assert_online_alter_exec_fails(
        db,
        "ALTER TABLE posts ADD COLUMN blocked_inplace INT, ALGORITHM=INPLACE"
    );
    assert_online_alter_exec_fails(
        db,
        "ALTER TABLE posts ADD COLUMN blocked_instant INT, ALGORITHM=INSTANT"
    );
    assert_online_alter_exec_fails(
        db,
        "ALTER TABLE posts ADD COLUMN blocked_nocopy INT, ALGORITHM=NOCOPY"
    );
    assert_online_alter_exec_fails(
        db,
        "ALTER OFFLINE TABLE posts ADD COLUMN blocked_offline_inplace INT, ALGORITHM=INPLACE"
    );
    assert_online_alter_exec_fails(db, "ALTER TABLE posts ADD COLUMN blocked_lock INT, LOCK=NONE");
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "posts", "InnoDB", "MYLITE");
    assert_exec_succeeds(db, "SELECT 'FOR UPDATE' AS quoted_text");
    assert_partition_exec_fails(
        db,
        "CREATE TABLE partitioned_posts (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB "
        "PARTITION BY HASH (id) PARTITIONS 2"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "partitioned_posts") == MYLITE_STORAGE_NOTFOUND
    );
    assert_partition_exec_fails(db, "ALTER TABLE posts PARTITION BY HASH (id) PARTITIONS 2");
    assert_partition_exec_fails(db, "ALTER TABLE posts REMOVE PARTITIONING");
    assert_catalog_table_count(filename, "app", 1U);
    assert_exec_succeeds(db, "ALTER TABLE posts ADD COLUMN copy_ok INT NULL, ALGORITHM=COPY");
    assert_exec_succeeds(
        db,
        "ALTER TABLE posts ADD COLUMN algorithm_note VARCHAR(32) DEFAULT 'ALGORITHM=INPLACE', "
        "ALGORITHM=COPY"
    );
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "posts", "InnoDB", "MYLITE");
    assert_exec_succeeds(db, "INSERT INTO posts (id, title) VALUES (1, 'autocommit')");
    assert(
        mylite_exec(db, "SELECT COUNT(*) FROM posts", single_value_callback, &count, NULL) ==
        MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "CREATE TABLE fk_parent (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB");
    assert_catalog_table_count(filename, "app", 2U);
    assert_catalog_table_metadata(filename, "app", "fk_parent", "InnoDB", "MYLITE");

    assert_foreign_key_exec_fails(
        db,
        "CREATE TABLE fk_blocked_table ("
        "id INT NOT NULL PRIMARY KEY, parent_id INT, "
        "CONSTRAINT fk_parent FOREIGN KEY (parent_id) REFERENCES fk_parent(id)"
        ") ENGINE=InnoDB"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "fk_blocked_table") == MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "app", 2U);

    assert_foreign_key_exec_fails(
        db,
        "CREATE TABLE fk_blocked_column ("
        "id INT NOT NULL PRIMARY KEY, parent_id INT REFERENCES fk_parent(id)"
        ") ENGINE=InnoDB"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "fk_blocked_column") == MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "app", 2U);

    assert_exec_succeeds(
        db,
        "CREATE TABLE fk_child (id INT NOT NULL PRIMARY KEY, parent_id INT) ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "fk_child", "InnoDB", "MYLITE");

    assert_exec_succeeds(db, "SET foreign_key_checks=0");
    assert_foreign_key_exec_fails(
        db,
        "ALTER TABLE fk_child ADD CONSTRAINT fk_child_parent "
        "FOREIGN KEY (parent_id) REFERENCES fk_parent(id)"
    );
    assert_foreign_key_exec_fails(db, "ALTER TABLE fk_child DROP FOREIGN KEY fk_child_parent");
    assert_exec_succeeds(db, "SET foreign_key_checks=1");

    assert_exec_succeeds(db, "INSERT INTO fk_parent VALUES (1)");
    assert_exec_succeeds(db, "INSERT INTO fk_child VALUES (1, 99)");
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM fk_child WHERE parent_id = 99",
            single_value_callback,
            &child_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(child_count.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_row_dml_transactions(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *rollback_update = NULL;
    mylite_stmt *read_only_insert = NULL;
    mylite_stmt *prepared_tx_ddl = NULL;
    mylite_stmt *prepared_savepoint = NULL;
    mylite_stmt *prepared_rollback = NULL;
    mylite_stmt *prepared_release = NULL;
    single_value_context count = {.expected_value = "1"};
    single_value_context zero_count = {.expected_value = "0"};

    assert_exec_succeeds(db, "CREATE DATABASE tx_app");
    assert_exec_succeeds(db, "USE tx_app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE tx_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL, "
        "UNIQUE KEY title_key (title)"
        ") ENGINE=InnoDB"
    );

    assert_exec_succeeds(
        db,
        "CREATE TEMPORARY TABLE tx_temp ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "INSERT INTO tx_temp VALUES (1, 'temporary-before')");
    assert_exec_succeeds(db, "START TRANSACTION READ ONLY");
    assert_exec_fails_with_message(
        db,
        "UPDATE tx_temp JOIN tx_posts ON tx_posts.id = tx_temp.id "
        "SET tx_posts.title = 'read-only-join'",
        "read-only transaction"
    );
    assert_exec_fails_with_message(
        db,
        "DELETE tx_posts FROM tx_temp JOIN tx_posts ON tx_posts.id = tx_temp.id",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "INSERT INTO tx_temp VALUES (2, 'temporary-insert')");
    assert_exec_succeeds(db, "UPDATE tx_temp SET title = 'temporary-updated' WHERE id = 1");
    assert_exec_succeeds(db, "REPLACE INTO tx_temp VALUES (2, 'temporary-replaced')");
    assert_exec_succeeds(db, "DELETE FROM tx_temp WHERE id = 1");
    assert_prepared_succeeds(db, "INSERT INTO tx_temp VALUES (3, 'prepared-temporary')");
    assert_exec_succeeds(db, "COMMIT");
    assert_query_single_value(db, "SELECT COUNT(*) FROM tx_temp", "2");
    assert_query_single_value(db, "SELECT title FROM tx_temp WHERE id = 2", "temporary-replaced");
    assert_query_single_value(db, "SELECT title FROM tx_temp WHERE id = 3", "prepared-temporary");
    assert_exec_succeeds(db, "DROP TEMPORARY TABLE tx_temp");
    assert_exec_succeeds(
        db,
        "CREATE TEMPORARY TABLE tx_temp ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "DROP TEMPORARY TABLE tx_app.tx_temp");
    assert_exec_succeeds(
        db,
        "CREATE TABLE tx_temp ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "START TRANSACTION READ ONLY");
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_temp VALUES (4, 'durable-after-temporary')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");

    assert_prepared_succeeds(
        db,
        "CREATE TEMPORARY TABLE tx_temp ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "START TRANSACTION READ ONLY");
    assert_prepared_succeeds(db, "INSERT INTO tx_temp VALUES (5, 'prepared-lifecycle')");
    assert_exec_succeeds(db, "COMMIT");
    assert_query_single_value(db, "SELECT title FROM tx_temp WHERE id = 5", "prepared-lifecycle");
    assert_prepared_succeeds(db, "DROP TEMPORARY TABLE tx_temp");
    assert_exec_succeeds(db, "START TRANSACTION READ ONLY");
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_temp VALUES (6, 'durable-after-prepared-temporary')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "DROP TABLE tx_temp");

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (40, 'active-temp-source')");
    assert_exec_succeeds(
        db,
        "CREATE TEMPORARY TABLE tx_active_temp ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "INSERT INTO tx_active_temp VALUES (1, 'active-temp-row')");
    assert_prepared_succeeds(
        db,
        "CREATE TEMPORARY TABLE tx_active_prepared ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert_prepared_succeeds(
        db,
        "INSERT INTO tx_active_prepared VALUES (2, 'active-prepared-temp-row')"
    );
    assert_exec_succeeds(
        db,
        "CREATE TEMPORARY TABLE tx_active_ctas AS "
        "SELECT id, title FROM tx_posts WHERE id = 40"
    );
    assert_exec_succeeds(db, "ROLLBACK");
    assert_query_single_value(db, "SELECT COUNT(*) FROM tx_posts WHERE id = 40", "0");
    assert_query_single_value(
        db,
        "SELECT title FROM tx_active_temp WHERE id = 1",
        "active-temp-row"
    );
    assert_query_single_value(
        db,
        "SELECT title FROM tx_active_prepared WHERE id = 2",
        "active-prepared-temp-row"
    );
    assert_query_single_value(
        db,
        "SELECT title FROM tx_active_ctas WHERE id = 40",
        "active-temp-source"
    );

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "DROP TEMPORARY TABLE tx_active_temp");
    assert_prepared_succeeds(db, "DROP TEMPORARY TABLE tx_active_prepared");
    assert_exec_succeeds(db, "DROP TEMPORARY TABLE tx_active_ctas");
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_fails_with_message(db, "SELECT COUNT(*) FROM tx_active_temp", "doesn't exist");
    assert_exec_fails_with_message(db, "SELECT COUNT(*) FROM tx_active_prepared", "doesn't exist");
    assert_exec_fails_with_message(db, "SELECT COUNT(*) FROM tx_active_ctas", "doesn't exist");

    assert_exec_succeeds(db, "START TRANSACTION READ ONLY");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_posts VALUES (46, 'read-only-direct')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "SET TRANSACTION READ ONLY");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_fails_with_message(
        db,
        "UPDATE tx_posts SET title = 'read-only-update' WHERE id = 1",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (46, 'one-shot-reset-write')");
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "SET SESSION TRANSACTION READ ONLY");
    assert_exec_succeeds(db, "START TRANSACTION READ WRITE");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (46, 'read-write-override')");
    assert_exec_succeeds(db, "COMMIT");
    assert_exec_succeeds(db, "SET SESSION TRANSACTION READ WRITE");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 46 AND title = 'read-write-override'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "SET SESSION TRANSACTION READ ONLY");
    assert_exec_succeeds(db, "SET autocommit=0");
    assert_exec_fails_with_message(
        db,
        "DELETE FROM tx_posts WHERE id = 46",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "SET autocommit=1");
    assert_exec_succeeds(db, "SET SESSION TRANSACTION READ WRITE");

    assert_exec_succeeds(db, "START TRANSACTION READ ONLY");
    assert_exec_succeeds(db, "COMMIT AND CHAIN");
    assert_exec_fails_with_message(
        db,
        "REPLACE INTO tx_posts VALUES (47, 'read-only-chain')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK AND NO CHAIN");

    assert(
        mylite_prepare(
            db,
            "INSERT INTO tx_posts VALUES (48, 'prepared-read-only')",
            MYLITE_NUL_TERMINATED,
            &read_only_insert,
            NULL
        ) == MYLITE_OK
    );
    assert(read_only_insert != NULL);
    assert_exec_succeeds(db, "START TRANSACTION READ ONLY");
    assert(mylite_step(read_only_insert) == MYLITE_ERROR);
    assert(strstr(mylite_errmsg(db), "read-only transaction") != NULL);
    assert(mylite_finalize(read_only_insert) == MYLITE_OK);
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (1, 'committed')");
    assert_exec_succeeds(db, "COMMIT");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 1 AND title = 'committed'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "BEGIN WORK");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (2, 'rolled-back')");
    assert_exec_succeeds(db, "ROLLBACK WORK");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'rolled-back'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);

    assert_exec_succeeds(db, "START TRANSACTION");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (2, 'second')");
    assert_exec_fails(db, "INSERT INTO tx_posts VALUES (3, 'duplicate'), (4, 'duplicate')");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'duplicate'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 2 AND title = 'second'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    assert_exec_succeeds(db, "COMMIT");

    assert_exec_succeeds(db, "START TRANSACTION");
    assert(
        mylite_prepare(
            db,
            "UPDATE tx_posts SET title=? WHERE id IN (1, 2) ORDER BY id",
            MYLITE_NUL_TERMINATED,
            &rollback_update,
            NULL
        ) == MYLITE_OK
    );
    assert(rollback_update != NULL);
    assert(
        mylite_bind_text(
            rollback_update,
            1U,
            "prepared-duplicate",
            MYLITE_NUL_TERMINATED,
            MYLITE_STATIC
        ) == MYLITE_OK
    );
    assert(mylite_step(rollback_update) == MYLITE_ERROR);
    assert(mylite_finalize(rollback_update) == MYLITE_OK);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'prepared-duplicate'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 1 AND title = 'committed'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "SET autocommit=0");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (3, 'autocommit-rollback')");
    assert_exec_succeeds(db, "ROLLBACK");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'autocommit-rollback'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);

    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (3, 'autocommit-commit')");
    assert_exec_succeeds(db, "COMMIT");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 3 AND title = 'autocommit-commit'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (4, 'autocommit-on-commit')");
    assert_exec_succeeds(db, "SET @@session.autocommit=1");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 4 AND title = 'autocommit-on-commit'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "SET autocommit=0");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (22, 'autocommit-default-commit')");
    assert_exec_succeeds(db, "SET autocommit=DEFAULT");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts "
            "WHERE id = 22 AND title = 'autocommit-default-commit'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "SET autocommit=0, sql_mode='ANSI'");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (36, 'autocommit-list-rollback')");
    assert_exec_succeeds(db, "ROLLBACK");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'autocommit-list-rollback'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);

    assert_exec_succeeds(db, "SET sql_mode='', autocommit=0");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (36, 'autocommit-list-commit')");
    assert_exec_succeeds(db, "SET @mylite_autocommit_label='commit', autocommit=1");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 36 AND title = 'autocommit-list-commit'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "SET autocommit=0, @mylite_autocommit_label='default'");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (37, 'autocommit-list-default')");
    assert_exec_succeeds(db, "SET sql_mode='', autocommit=DEFAULT");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 37 AND title = 'autocommit-list-default'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "SET TRANSACTION READ WRITE");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (38, 'set-transaction-read-write')");
    assert_exec_succeeds(db, "COMMIT");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts "
            "WHERE id = 38 AND title = 'set-transaction-read-write'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "SET completion_type=NO_CHAIN");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (39, 'completion-no-chain')");
    assert_exec_succeeds(db, "COMMIT");
    assert_exec_succeeds(db, "DROP TABLE IF EXISTS no_chain_probe");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 39 AND title = 'completion-no-chain'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "SET completion_type=NO_CHAIN, autocommit=0");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (40, 'completion-autocommit-list')");
    assert_exec_succeeds(db, "COMMIT");
    assert_exec_succeeds(db, "SET autocommit=1");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 40 AND title = 'completion-autocommit-list'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "SET completion_type=CHAIN");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (41, 'completion-chain-before')");
    assert_exec_succeeds(db, "COMMIT");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (42, 'completion-chain-after')");
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "ROLLBACK AND NO CHAIN");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 41 AND title = 'completion-chain-before'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'completion-chain-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (43, 'completion-no-chain-override')");
    assert_exec_succeeds(db, "COMMIT AND NO CHAIN");
    assert_exec_succeeds(db, "DROP TABLE IF EXISTS no_chain_override_probe");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts "
            "WHERE id = 43 AND title = 'completion-no-chain-override'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    assert_exec_succeeds(db, "SET completion_type=DEFAULT");

    assert_exec_succeeds(db, "SET @@session.completion_type=1");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (44, 'completion-one-before')");
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (45, 'completion-one-after')");
    assert_exec_succeeds(db, "ROLLBACK AND NO CHAIN");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 44 AND title = 'completion-one-before'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 45 AND title = 'completion-one-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "SET completion_type=DEFAULT");

    assert_exec_succeeds(db, "SET completion_type=CHAIN, completion_type=NO_CHAIN");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (57, 'completion-duplicate-no-chain')");
    assert_exec_succeeds(db, "COMMIT");
    assert_exec_succeeds(db, "DROP TABLE IF EXISTS duplicate_no_chain_probe");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts "
            "WHERE id = 57 AND title = 'completion-duplicate-no-chain'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    assert_exec_succeeds(db, "DELETE FROM tx_posts WHERE id = 57");

    assert_exec_succeeds(db, "SET completion_type=NO_CHAIN, completion_type=CHAIN");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(
        db,
        "INSERT INTO tx_posts VALUES (57, 'completion-duplicate-chain-before')"
    );
    assert_exec_succeeds(db, "COMMIT");
    assert_exec_succeeds(
        db,
        "INSERT INTO tx_posts VALUES (58, 'completion-duplicate-chain-after')"
    );
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "ROLLBACK AND NO CHAIN");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts "
            "WHERE id = 57 AND title = 'completion-duplicate-chain-before'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts "
            "WHERE id = 58 AND title = 'completion-duplicate-chain-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "DELETE FROM tx_posts WHERE id = 57");
    assert_exec_succeeds(db, "SET completion_type=DEFAULT");

    assert_exec_succeeds(db, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED, READ ONLY");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_posts VALUES (50, 'isolation-read-only')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "SET TRANSACTION READ WRITE, ISOLATION LEVEL REPEATABLE READ");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (50, 'isolation-read-write')");
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "SET transaction_read_only=1");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_posts VALUES (51, 'variable-read-only')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "SET transaction_read_only=0");

    assert_exec_succeeds(
        db,
        "SET transaction_isolation='READ-COMMITTED', tx_isolation='SERIALIZABLE'"
    );
    assert_exec_succeeds(db, "SET transaction_read_only=1, tx_read_only=0");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (59, 'duplicate-variable-read-write')");
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "SET tx_read_only=0, transaction_read_only=1");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_posts VALUES (59, 'duplicate-variable-read-only')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "SET transaction_read_only=0");

    assert_prepared_succeeds(db, "SET autocommit=0");
    assert_prepared_succeeds(
        db,
        "INSERT INTO tx_posts VALUES (60, 'prepared-autocommit-rollback')"
    );
    assert_exec_succeeds(db, "ROLLBACK");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM tx_posts WHERE title = 'prepared-autocommit-rollback'",
        "0"
    );

    assert_prepared_succeeds(db, "SET sql_mode='', autocommit=0");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (60, 'prepared-autocommit-commit')");
    assert_prepared_succeeds(db, "SET autocommit=1");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM tx_posts "
        "WHERE id = 60 AND title = 'prepared-autocommit-commit'",
        "1"
    );
    assert_exec_succeeds(db, "DELETE FROM tx_posts WHERE id = 60");

    assert_prepared_succeeds(db, "SET autocommit=0, @mylite_prepared_autocommit='default'");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (61, 'prepared-autocommit-default')");
    assert_prepared_succeeds(db, "SET autocommit=DEFAULT");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM tx_posts "
        "WHERE id = 61 AND title = 'prepared-autocommit-default'",
        "1"
    );
    assert_exec_succeeds(db, "DELETE FROM tx_posts WHERE id = 61");

    assert_prepared_succeeds(db, "SET completion_type=NO_CHAIN, completion_type=CHAIN");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (62, 'prepared-completion-before')");
    assert_exec_succeeds(db, "COMMIT");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (63, 'prepared-completion-after')");
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "ROLLBACK AND NO CHAIN");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM tx_posts "
        "WHERE id = 62 AND title = 'prepared-completion-before'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM tx_posts "
        "WHERE id = 63 AND title = 'prepared-completion-after'",
        "0"
    );
    assert_exec_succeeds(db, "DELETE FROM tx_posts WHERE id = 62");
    assert_prepared_succeeds(db, "SET completion_type=DEFAULT");

    assert_prepared_succeeds(db, "SET transaction_read_only=1, tx_read_only=0");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (64, 'prepared-variable-read-write')");
    assert_exec_succeeds(db, "ROLLBACK");

    assert_prepared_succeeds(db, "SET tx_read_only=0, transaction_read_only=1");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_posts VALUES (64, 'prepared-variable-read-only')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");
    assert_prepared_succeeds(db, "SET transaction_read_only=0");

    assert_prepared_succeeds(db, "SET TRANSACTION READ ONLY");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_posts VALUES (65, 'prepared-set-transaction-read-only')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");

    assert_prepared_succeeds(db, "SET TRANSACTION READ WRITE, ISOLATION LEVEL READ COMMITTED");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (65, 'prepared-set-transaction-write')");
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "SET @@transaction_read_only=1");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_posts VALUES (51, 'one-shot-variable-read-only')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (51, 'one-shot-variable-read-write')");
    assert_exec_succeeds(db, "ROLLBACK");

    assert(
        mylite_prepare(
            db,
            "ALTER TABLE tx_posts ADD COLUMN blocked_prepared_tx_ddl INT",
            MYLITE_NUL_TERMINATED,
            &prepared_tx_ddl,
            NULL
        ) == MYLITE_OK
    );
    assert(prepared_tx_ddl != NULL);
    assert_exec_succeeds(db, "BEGIN");
    assert_prepared_policy_fails_with_message(db, prepared_tx_ddl, "transactional DDL");
    assert(mylite_finalize(prepared_tx_ddl) == MYLITE_OK);
    prepared_tx_ddl = NULL;
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "SET SESSION autocommit=OFF");
    assert_exec_fails_with_message(
        db,
        "ALTER TABLE tx_posts ADD COLUMN blocked_autocommit_ddl INT",
        "transactional DDL"
    );
    assert_transaction_control_exec_fails(db, "SET GLOBAL autocommit=0");
    assert_exec_succeeds(db, "SET autocommit=0, completion_type=CHAIN");
    assert_exec_succeeds(db, "SET completion_type=DEFAULT");
    assert_exec_succeeds(db, "SET TRANSACTION READ ONLY");
    assert_exec_fails_with_message(
        db,
        "INSERT INTO tx_posts VALUES (49, 'autocommit-read-only')",
        "read-only transaction"
    );
    assert_exec_succeeds(db, "SET autocommit=ON");

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_fails_with_message(
        db,
        "ALTER TABLE tx_posts ADD COLUMN blocked_tx_ddl INT",
        "transactional DDL"
    );
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "SAVEPOINT mylite_probe");
    assert_exec_fails_with_message(
        db,
        "ALTER TABLE tx_posts ADD COLUMN blocked_savepoint_ddl INT",
        "transactional DDL"
    );
    assert_exec_succeeds(db, "ROLLBACK TO SAVEPOINT mylite_probe");
    assert_exec_succeeds(db, "RELEASE SAVEPOINT mylite_probe");
    assert_transaction_control_exec_fails(db, "XA START 'mylite-xid'");
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (5, 'restart-committed')");
    assert_exec_succeeds(db, "START TRANSACTION");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (6, 'restart-rolled-back')");
    assert_exec_succeeds(db, "ROLLBACK");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 5 AND title = 'restart-committed'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'restart-rolled-back'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);

    assert_exec_succeeds(db, "START TRANSACTION READ WRITE");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (30, 'read-write-committed')");
    assert_exec_succeeds(db, "COMMIT AND NO CHAIN NO RELEASE");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 30 AND title = 'read-write-committed'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (31, 'rollback-no-chain')");
    assert_exec_succeeds(db, "ROLLBACK AND NO CHAIN NO RELEASE");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'rollback-no-chain'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (32, 'commit-chain-before')");
    assert_exec_succeeds(db, "COMMIT AND CHAIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (33, 'commit-chain-after')");
    assert_exec_succeeds(db, "ROLLBACK");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 32 AND title = 'commit-chain-before'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'commit-chain-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (34, 'rollback-chain-before')");
    assert_exec_succeeds(db, "ROLLBACK AND CHAIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (35, 'rollback-chain-after')");
    assert_exec_succeeds(db, "COMMIT");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'rollback-chain-before'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 35 AND title = 'rollback-chain-after'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (6, 'savepoint-before')");
    assert_exec_succeeds(db, "SAVEPOINT mylite_sp");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (7, 'savepoint-after')");
    assert_exec_succeeds(db, "ROLLBACK TO SAVEPOINT mylite_sp");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'savepoint-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (7, 'savepoint-after-second')");
    assert_exec_succeeds(db, "ROLLBACK TO mylite_sp");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'savepoint-after-second'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (7, 'savepoint-release')");
    assert_exec_succeeds(db, "RELEASE SAVEPOINT mylite_sp");
    assert_exec_succeeds(db, "COMMIT");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 6 AND title = 'savepoint-before'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 7 AND title = 'savepoint-release'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "SAVEPOINT `quoted sp`");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (23, 'quoted-savepoint-after')");
    assert_exec_succeeds(db, "ROLLBACK TO SAVEPOINT `quoted sp`");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'quoted-savepoint-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (23, 'quoted-savepoint-second')");
    assert_exec_succeeds(db, "ROLLBACK TO `quoted sp`");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'quoted-savepoint-second'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "RELEASE SAVEPOINT `quoted sp`");
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "SET sql_mode='ANSI_QUOTES'");
    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "SAVEPOINT \"ansi \"\"sp\"");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (52, 'ansi-savepoint-after')");
    assert_exec_succeeds(db, "ROLLBACK TO SAVEPOINT \"ansi \"\"sp\"");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'ansi-savepoint-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "RELEASE SAVEPOINT \"ansi \"\"sp\"");
    assert_exec_succeeds(db, "ROLLBACK");
    assert_exec_succeeds(db, "SET sql_mode=''");

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "SAVEPOINT CaseDup");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (54, 'case-dup-before')");
    assert_exec_succeeds(db, "SAVEPOINT casedup");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (55, 'case-dup-after')");
    assert_exec_succeeds(db, "ROLLBACK TO SAVEPOINT CASEDUP");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 54 AND title = 'case-dup-before'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 55 AND title = 'case-dup-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "ROLLBACK");

    assert(
        mylite_prepare(
            db,
            "SAVEPOINT prepared_sp",
            MYLITE_NUL_TERMINATED,
            &prepared_savepoint,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_prepare(
            db,
            "ROLLBACK TO SAVEPOINT prepared_sp",
            MYLITE_NUL_TERMINATED,
            &prepared_rollback,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_prepare(
            db,
            "RELEASE SAVEPOINT prepared_sp",
            MYLITE_NUL_TERMINATED,
            &prepared_release,
            NULL
        ) == MYLITE_OK
    );
    assert(mylite_bind_parameter_count(prepared_savepoint) == 0U);
    assert(mylite_column_count(prepared_savepoint) == 0U);
    assert(mylite_step(prepared_savepoint) == MYLITE_ERROR);
    assert(strstr(mylite_errmsg(db), "transaction control") != NULL);
    assert(mylite_reset(prepared_savepoint) == MYLITE_OK);

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (20, 'prepared-savepoint-before')");
    assert(mylite_step(prepared_savepoint) == MYLITE_DONE);
    assert(mylite_step(prepared_savepoint) == MYLITE_DONE);
    assert(mylite_reset(prepared_savepoint) == MYLITE_OK);
    assert(mylite_step(prepared_savepoint) == MYLITE_DONE);
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (21, 'prepared-savepoint-after')");
    assert(mylite_step(prepared_rollback) == MYLITE_DONE);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'prepared-savepoint-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (21, 'prepared-savepoint-second')");
    assert(mylite_reset(prepared_rollback) == MYLITE_OK);
    assert(mylite_step(prepared_rollback) == MYLITE_DONE);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'prepared-savepoint-second'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (21, 'prepared-savepoint-release')");
    assert(mylite_step(prepared_release) == MYLITE_DONE);
    assert_exec_succeeds(db, "COMMIT");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts "
            "WHERE id = 20 AND title = 'prepared-savepoint-before'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts "
            "WHERE id = 21 AND title = 'prepared-savepoint-release'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    assert(mylite_finalize(prepared_savepoint) == MYLITE_OK);
    assert(mylite_finalize(prepared_rollback) == MYLITE_OK);
    assert(mylite_finalize(prepared_release) == MYLITE_OK);
    prepared_savepoint = NULL;
    prepared_rollback = NULL;
    prepared_release = NULL;

    assert(
        mylite_prepare(
            db,
            "SAVEPOINT `prepared quoted ``sp`",
            MYLITE_NUL_TERMINATED,
            &prepared_savepoint,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_prepare(
            db,
            "ROLLBACK TO `prepared quoted ``sp`",
            MYLITE_NUL_TERMINATED,
            &prepared_rollback,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_prepare(
            db,
            "RELEASE SAVEPOINT `prepared quoted ``sp`",
            MYLITE_NUL_TERMINATED,
            &prepared_release,
            NULL
        ) == MYLITE_OK
    );
    assert_exec_succeeds(db, "BEGIN");
    assert(mylite_step(prepared_savepoint) == MYLITE_DONE);
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (24, 'prepared-quoted-after')");
    assert(mylite_step(prepared_rollback) == MYLITE_DONE);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'prepared-quoted-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert(mylite_step(prepared_release) == MYLITE_DONE);
    assert_exec_succeeds(db, "ROLLBACK");
    assert(mylite_finalize(prepared_savepoint) == MYLITE_OK);
    assert(mylite_finalize(prepared_rollback) == MYLITE_OK);
    assert(mylite_finalize(prepared_release) == MYLITE_OK);
    prepared_savepoint = NULL;
    prepared_rollback = NULL;
    prepared_release = NULL;

    assert_exec_succeeds(db, "SET sql_mode='ANSI_QUOTES'");
    assert(
        mylite_prepare(
            db,
            "SAVEPOINT \"prepared ansi \"\"sp\"",
            MYLITE_NUL_TERMINATED,
            &prepared_savepoint,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_prepare(
            db,
            "ROLLBACK TO \"prepared ansi \"\"sp\"",
            MYLITE_NUL_TERMINATED,
            &prepared_rollback,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_prepare(
            db,
            "RELEASE SAVEPOINT \"prepared ansi \"\"sp\"",
            MYLITE_NUL_TERMINATED,
            &prepared_release,
            NULL
        ) == MYLITE_OK
    );
    assert_exec_succeeds(db, "SET sql_mode=''");
    assert_exec_succeeds(db, "BEGIN");
    assert(mylite_step(prepared_savepoint) == MYLITE_DONE);
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (53, 'prepared-ansi-after')");
    assert(mylite_step(prepared_rollback) == MYLITE_DONE);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'prepared-ansi-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert(mylite_step(prepared_release) == MYLITE_DONE);
    assert_exec_succeeds(db, "ROLLBACK");
    assert(mylite_finalize(prepared_savepoint) == MYLITE_OK);
    assert(mylite_finalize(prepared_rollback) == MYLITE_OK);
    assert(mylite_finalize(prepared_release) == MYLITE_OK);
    prepared_savepoint = NULL;
    prepared_rollback = NULL;
    prepared_release = NULL;

    assert(
        mylite_prepare(
            db,
            "SAVEPOINT PreparedCase",
            MYLITE_NUL_TERMINATED,
            &prepared_savepoint,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_prepare(
            db,
            "ROLLBACK TO preparedcase",
            MYLITE_NUL_TERMINATED,
            &prepared_rollback,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_prepare(
            db,
            "RELEASE SAVEPOINT PREPAREDCASE",
            MYLITE_NUL_TERMINATED,
            &prepared_release,
            NULL
        ) == MYLITE_OK
    );
    assert_exec_succeeds(db, "BEGIN");
    assert(mylite_step(prepared_savepoint) == MYLITE_DONE);
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (56, 'prepared-case-after')");
    assert(mylite_step(prepared_rollback) == MYLITE_DONE);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'prepared-case-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert(mylite_step(prepared_release) == MYLITE_DONE);
    assert_exec_succeeds(db, "ROLLBACK");
    assert(mylite_finalize(prepared_savepoint) == MYLITE_OK);
    assert(mylite_finalize(prepared_rollback) == MYLITE_OK);
    assert(mylite_finalize(prepared_release) == MYLITE_OK);
    prepared_savepoint = NULL;
    prepared_rollback = NULL;
    prepared_release = NULL;

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "SAVEPOINT duplicate_sp");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (8, 'duplicate-before')");
    assert_exec_succeeds(db, "SAVEPOINT duplicate_sp");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (9, 'duplicate-after')");
    assert_exec_succeeds(db, "ROLLBACK TO SAVEPOINT duplicate_sp");
    assert_exec_succeeds(db, "COMMIT");
    count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE id = 8 AND title = 'duplicate-before'",
            single_value_callback,
            &count,
            NULL
        ) == MYLITE_OK
    );
    assert(count.rows == 1);
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'duplicate-after'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "SAVEPOINT missing_guard");
    assert_transaction_control_exec_fails(db, "ROLLBACK TO SAVEPOINT missing_sp");
    assert_transaction_control_exec_fails(db, "RELEASE SAVEPOINT missing_sp");
    assert_exec_fails_with_message(
        db,
        "ALTER TABLE tx_posts ADD COLUMN blocked_missing_savepoint_ddl INT",
        "transactional DDL"
    );
    assert_exec_succeeds(db, "ROLLBACK");

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (9, 'savepoint-full-rollback-before')");
    assert_exec_succeeds(db, "SAVEPOINT full_rollback_sp");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (10, 'savepoint-full-rollback-after')");
    assert_exec_succeeds(db, "ROLLBACK");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title LIKE 'savepoint-full-rollback%'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);

    assert_exec_succeeds(db, "BEGIN");
    assert_exec_succeeds(db, "SAVEPOINT close_sp");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (9, 'close-rollback')");
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE tx_app");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'close-rollback'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    count = (single_value_context){.expected_value = "22"};
    assert(
        mylite_exec(db, "SELECT COUNT(*) FROM tx_posts", single_value_callback, &count, NULL) ==
        MYLITE_OK
    );
    assert(count.rows == 1);

    assert_exec_succeeds(db, "SET autocommit=0");
    assert_exec_succeeds(db, "SAVEPOINT autocommit_close_sp");
    assert_exec_succeeds(db, "INSERT INTO tx_posts VALUES (9, 'autocommit-close-rollback')");
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE tx_app");
    zero_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'autocommit-close-rollback'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    count = (single_value_context){.expected_value = "22"};
    assert(
        mylite_exec(db, "SELECT COUNT(*) FROM tx_posts", single_value_callback, &count, NULL) ==
        MYLITE_OK
    );
    assert(count.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    assert_transaction_crash_recovery(root, filename);
    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE tx_app");
    count = (single_value_context){.expected_value = "22"};
    assert(
        mylite_exec(db, "SELECT COUNT(*) FROM tx_posts", single_value_callback, &count, NULL) ==
        MYLITE_OK
    );
    assert(count.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_create_table_persists_catalog_metadata(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context tables = {0};
    table_context rows = {0};
    table_context drop_rows = {0};
    post_row_context row_posts = {0};
    post_row_context renamed_row_posts = {0};
    nullable_row_context nullable_rows = {0};
    blob_row_context blob_rows = {0};
    mutable_row_context mutable_rows = {0};
    auto_row_context auto_rows = {0};
    single_value_context checked_rating = {.expected_value = "4"};
    single_value_context checked_disabled_rating = {.expected_value = "7"};
    single_value_context checked_row_count = {.expected_value = "5"};
    single_value_context generated_title_len = {.expected_value = "5"};
    single_value_context generated_label = {.expected_value = "draft-1"};
    single_value_context generated_title_len_index = {.expected_value = "1"};
    single_value_context generated_label_index = {.expected_value = "1"};
    single_value_context generated_old_label_index = {.expected_value = "1"};
    single_value_context generated_reused_title_key = {.expected_value = "3"};
    single_value_context generated_deleted_title_key = {.expected_value = "4"};
    single_value_context rollback_count = {.expected_value = "1"};
    mylite_stmt *rollback_update = NULL;
    mylite_stmt *rollback_replace = NULL;
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(db, "CREATE TABLE default_posts (id INT PRIMARY KEY, title VARCHAR(255))");
    assert_exec_succeeds(
        db,
        "CREATE TABLE explicit_posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=MYLITE"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE innodb_posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE myisam_posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=MyISAM"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE aria_posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=Aria"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE row_posts (id INT NOT NULL, title VARCHAR(255) NOT NULL) ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE nullable_posts (id INT NOT NULL, title VARCHAR(255) NULL, score INT NULL) "
        "ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE auto_posts (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, "
        "title TEXT NOT NULL) ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE content_posts (id INT NOT NULL, body TEXT NULL, data BLOB NULL) "
        "ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE mutable_posts (id INT NOT NULL, title VARCHAR(255) NOT NULL, body TEXT NULL) "
        "ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE drop_posts (id INT NOT NULL, title VARCHAR(255) NOT NULL) ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE checked_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "rating INT CHECK (rating >= 0), "
        "CONSTRAINT rating_max CHECK (rating <= 5)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE generated_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL, "
        "title_key VARCHAR(80) AS (CONCAT(title, '-key')) VIRTUAL, "
        "title_len INT AS (CHAR_LENGTH(title)) VIRTUAL, "
        "label VARCHAR(80) AS (CONCAT(title, '-', id)) STORED, "
        "UNIQUE KEY title_key_unique (title_key), "
        "KEY title_len_key (title_len), "
        "KEY label_key (label)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE rollback_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(32) NOT NULL UNIQUE, "
        "rating INT CHECK (rating <= 5)"
        ") ENGINE=InnoDB"
    );
    assert(mylite_storage_table_exists(filename, "app", "default_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "explicit_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "innodb_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "myisam_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "aria_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "row_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "nullable_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "auto_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "content_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "mutable_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "drop_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "checked_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "generated_posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "rollback_posts") == MYLITE_STORAGE_OK);
    assert_catalog_table_count(filename, "app", 14U);
    assert_catalog_table_metadata(filename, "app", "default_posts", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "explicit_posts", "MYLITE", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "innodb_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "myisam_posts", "MyISAM", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "aria_posts", "Aria", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "row_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "nullable_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "auto_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "content_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "mutable_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "drop_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "checked_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "generated_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "rollback_posts", "InnoDB", "MYLITE");
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 14);
    assert_exec_fails(
        db,
        "CREATE TABLE archive_posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=ARCHIVE"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "archive_posts") == MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "app", 14U);
    assert_exec_fails(
        db,
        "CREATE TABLE innodb_posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "app", 14U);
    assert_exec_fails(
        db,
        "CREATE TABLE fulltext_index_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "body TEXT NOT NULL, "
        "FULLTEXT KEY body_fulltext (body)"
        ") ENGINE=InnoDB"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "fulltext_index_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "app", 14U);
    assert_exec_fails(
        db,
        "CREATE TABLE spatial_index_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "location POINT NOT NULL, "
        "SPATIAL KEY location_spatial (location)"
        ") ENGINE=InnoDB"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "spatial_index_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "app", 14U);
    assert_exec_fails(
        db,
        "CREATE TABLE vector_index_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "embedding VECTOR(3) NOT NULL, "
        "VECTOR KEY embedding_vector (embedding)"
        ") ENGINE=InnoDB"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "vector_index_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "app", 14U);
    assert(mylite_busy_timeout(db, k_busy_timeout_wait_ms) == MYLITE_OK);
    pid_t child = hold_test_lock_for(
        filename,
        (timed_lock_request){
            .operation = LOCK_EX,
            .milliseconds = k_busy_timeout_release_ms,
        }
    );
    assert_exec_succeeds(db, "INSERT INTO row_posts VALUES (1, 'draft')");
    wait_test_lock_child(child);
    assert(mylite_busy_timeout(db, 0U) == MYLITE_OK);
    assert_exec_succeeds(db, "INSERT INTO row_posts VALUES (2, 'published')");
    assert_exec_succeeds(db, "INSERT INTO nullable_posts VALUES (1, NULL, NULL)");
    assert_exec_succeeds(db, "INSERT INTO nullable_posts VALUES (2, 'filled', 42)");
    assert_exec_succeeds(db, "INSERT INTO content_posts VALUES (1, 'short text', X'000102FF')");
    assert_exec_succeeds(db, "INSERT INTO content_posts VALUES (2, NULL, NULL)");
    assert_exec_succeeds(db, "INSERT INTO content_posts VALUES (3, '', X'')");
    assert_exec_succeeds(db, "INSERT INTO content_posts VALUES (4, REPEAT('large-', 900), NULL)");
    assert_exec_succeeds(db, "INSERT INTO mutable_posts VALUES (1, 'delete a', 'delete me')");
    assert_exec_succeeds(db, "INSERT INTO mutable_posts VALUES (2, 'published', 'short')");
    assert_exec_succeeds(db, "INSERT INTO mutable_posts VALUES (3, 'untouched', NULL)");
    assert_exec_succeeds(db, "INSERT INTO mutable_posts VALUES (4, 'delete z', 'delete me too')");
    assert_exec_succeeds(db, "INSERT INTO auto_posts (title) VALUES ('first')");
    assert_exec_succeeds(db, "INSERT INTO auto_posts (id, title) VALUES (7, 'manual')");
    assert_exec_succeeds(db, "INSERT INTO auto_posts (title) VALUES ('after manual')");
    assert_exec_fails(db, "INSERT INTO auto_posts (id, title) VALUES (7, 'duplicate')");
    assert_exec_succeeds(db, "ALTER TABLE auto_posts AUTO_INCREMENT = 50");
    assert_exec_succeeds(db, "ALTER TABLE auto_posts ADD COLUMN summary VARCHAR(32) NULL");
    assert_exec_succeeds(db, "INSERT INTO auto_posts (title) VALUES ('after alter')");
    assert_exec_succeeds(db, "ALTER TABLE auto_posts AUTO_INCREMENT = 4");
    assert_exec_succeeds(db, "INSERT INTO auto_posts (title) VALUES ('after low alter')");
    assert_exec_succeeds(db, "INSERT INTO checked_posts VALUES (1, 5)");
    assert_exec_fails(db, "INSERT INTO checked_posts VALUES (2, -1)");
    assert_exec_fails(db, "INSERT INTO checked_posts VALUES (3, 6)");
    assert_exec_fails(db, "UPDATE checked_posts SET rating = 6 WHERE id = 1");
    assert_exec_succeeds(db, "UPDATE checked_posts SET rating = 4 WHERE id = 1");
    assert_exec_succeeds(db, "SET check_constraint_checks=OFF");
    assert_exec_succeeds(db, "INSERT INTO checked_posts VALUES (2, 7)");
    assert_exec_succeeds(db, "SET check_constraint_checks=ON");
    assert_exec_succeeds(db, "ALTER TABLE checked_posts DROP CONSTRAINT rating_max");
    assert_exec_succeeds(db, "INSERT INTO checked_posts VALUES (3, 8)");
    assert_exec_succeeds(
        db,
        "ALTER TABLE checked_posts ADD CONSTRAINT rating_cap CHECK (rating <= 8)"
    );
    assert_exec_succeeds(db, "ALTER TABLE checked_posts DROP CONSTRAINT rating_cap");
    assert_exec_succeeds(db, "INSERT INTO checked_posts VALUES (4, 9)");
    assert_exec_succeeds(
        db,
        "ALTER TABLE checked_posts ADD CONSTRAINT rating_reopen_cap CHECK (rating <= 9)"
    );
    assert_exec_fails_with_message(
        db,
        "ALTER TABLE checked_posts ADD CONSTRAINT impossible_rating CHECK (rating < 9)",
        "CONSTRAINT"
    );
    assert_catalog_table_count(filename, "app", 14U);
    assert_exec_succeeds(db, "INSERT INTO checked_posts VALUES (6, 9)");
    assert_prepared_fails_with_message(
        db,
        "INSERT INTO checked_posts VALUES (8, 10)",
        "CONSTRAINT"
    );
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM checked_posts",
            single_value_callback,
            &checked_row_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(checked_row_count.rows == 1);
    assert_exec_succeeds(db, "INSERT INTO generated_posts (id, title) VALUES (1, 'draft')");
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM generated_posts WHERE id = 1",
            single_value_callback,
            &generated_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT label FROM generated_posts WHERE id = 1",
            single_value_callback,
            &generated_label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_label.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (title_len_key) "
            "WHERE title_len = 5 AND id = 1",
            single_value_callback,
            &generated_title_len_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_title_len_index.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (label_key) WHERE label = 'draft-1'",
            single_value_callback,
            &generated_label_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_label_index.rows == 1);
    assert_prepared_fails_with_message(
        db,
        "INSERT INTO generated_posts (id, title) VALUES (2, 'draft')",
        "Duplicate"
    );
    assert_exec_succeeds(db, "UPDATE generated_posts SET title = 'published' WHERE id = 1");
    generated_title_len = (single_value_context){.expected_value = "9"};
    generated_label = (single_value_context){.expected_value = "published-1"};
    generated_title_len_index = (single_value_context){.expected_value = "1"};
    generated_label_index = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM generated_posts WHERE id = 1",
            single_value_callback,
            &generated_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT label FROM generated_posts WHERE id = 1",
            single_value_callback,
            &generated_label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_label.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (title_len_key) "
            "WHERE title_len = 9 AND id = 1",
            single_value_callback,
            &generated_title_len_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_title_len_index.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (label_key) WHERE label = 'published-1'",
            single_value_callback,
            &generated_label_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_label_index.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (label_key) WHERE label = 'draft-1'",
            single_value_callback,
            &generated_old_label_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_old_label_index.rows == 0);
    assert_exec_succeeds(db, "INSERT INTO generated_posts (id, title) VALUES (3, 'draft')");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (title_key_unique) "
            "WHERE title_key = 'draft-key'",
            single_value_callback,
            &generated_reused_title_key,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_reused_title_key.rows == 1);
    assert_exec_succeeds(db, "INSERT INTO rollback_posts VALUES (1, 'first', 1)");
    assert_exec_fails(db, "INSERT INTO rollback_posts VALUES (2, 'second', 2), (3, 'second', 3)");
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE id IN (2, 3)",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    assert_exec_fails(db, "INSERT INTO rollback_posts VALUES (2, 'second', 2), (3, 'third', 6)");
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE id IN (2, 3)",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    assert_exec_succeeds(db, "INSERT INTO rollback_posts VALUES (2, 'second', 2), (3, 'third', 3)");
    assert_prepared_fails(
        db,
        "INSERT INTO rollback_posts VALUES (4, 'fourth', 4), (5, 'fourth', 5)"
    );
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE id IN (4, 5)",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    assert_exec_fails(
        db,
        "UPDATE rollback_posts SET title = 'duplicate' WHERE id IN (2, 3) ORDER BY id"
    );
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE title = 'duplicate'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE id = 2 AND title = 'second'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    assert(
        mylite_prepare(
            db,
            "UPDATE rollback_posts SET title=? WHERE id IN (2, 3) ORDER BY id",
            MYLITE_NUL_TERMINATED,
            &rollback_update,
            NULL
        ) == MYLITE_OK
    );
    assert(rollback_update != NULL);
    assert(
        mylite_bind_text(
            rollback_update,
            1U,
            "prepared-duplicate",
            MYLITE_NUL_TERMINATED,
            MYLITE_STATIC
        ) == MYLITE_OK
    );
    assert(mylite_step(rollback_update) == MYLITE_ERROR);
    assert(strstr(mylite_errmsg(db), "Duplicate") != NULL);
    assert(mylite_finalize(rollback_update) == MYLITE_OK);
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE title = 'prepared-duplicate'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE id = 2 AND title = 'second'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE id = 2 AND title = 'second'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE id = 3 AND title = 'third'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    assert_exec_fails(
        db,
        "REPLACE INTO rollback_posts VALUES "
        "(2, 'direct-replaced', 2), "
        "(4, 'direct-bad', 6)"
    );
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE title = 'direct-replaced'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE id = 2 AND title = 'second'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    assert(
        mylite_prepare(
            db,
            "REPLACE INTO rollback_posts VALUES (3, ?, 3), (5, ?, 6)",
            MYLITE_NUL_TERMINATED,
            &rollback_replace,
            NULL
        ) == MYLITE_OK
    );
    assert(rollback_replace != NULL);
    assert(
        mylite_bind_text(
            rollback_replace,
            1U,
            "prepared-replaced",
            MYLITE_NUL_TERMINATED,
            MYLITE_STATIC
        ) == MYLITE_OK
    );
    assert(
        mylite_bind_text(
            rollback_replace,
            2U,
            "prepared-bad",
            MYLITE_NUL_TERMINATED,
            MYLITE_STATIC
        ) == MYLITE_OK
    );
    assert(mylite_step(rollback_replace) == MYLITE_ERROR);
    assert(strstr(mylite_errmsg(db), "CONSTRAINT") != NULL);
    assert(mylite_finalize(rollback_replace) == MYLITE_OK);
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE title = 'prepared-replaced'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE id = 3 AND title = 'third'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    assert_exec_fails(
        db,
        "REPLACE INTO rollback_posts "
        "SELECT id, title, rating FROM ("
        "SELECT 2 AS id, 'select-replaced' AS title, 2 AS rating "
        "UNION ALL SELECT 6, 'select-bad', 6"
        ") AS replace_rows ORDER BY id"
    );
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE title = 'select-replaced'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE id = 2 AND title = 'second'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT rating FROM checked_posts WHERE id = 1",
            single_value_callback,
            &checked_rating,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(checked_rating.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT rating FROM checked_posts WHERE id = 2",
            single_value_callback,
            &checked_disabled_rating,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(checked_disabled_rating.rows == 1);
    assert_exec_succeeds(
        db,
        "UPDATE mutable_posts SET title = 'published edited', body = REPEAT('changed-', 700) "
        "WHERE id IN (2, 3) ORDER BY title ASC LIMIT 1"
    );
    assert_exec_succeeds(
        db,
        "DELETE FROM mutable_posts WHERE id IN (1, 4) ORDER BY title DESC LIMIT 1"
    );
    assert_exec_succeeds(db, "DELETE FROM mutable_posts WHERE id = 1");
    assert_exec_succeeds(db, "INSERT INTO drop_posts VALUES (7, 'old')");
    assert(
        mylite_exec(
            db,
            "SELECT id, title FROM row_posts",
            post_row_callback,
            &row_posts,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(row_posts.rows == 2);
    assert(row_posts.found_draft);
    assert(row_posts.found_published);
    assert(
        mylite_exec(
            db,
            "SELECT id, title, score FROM nullable_posts",
            nullable_row_callback,
            &nullable_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(nullable_rows.rows == 2);
    assert(nullable_rows.found_null_row);
    assert(nullable_rows.found_value_row);
    assert(
        mylite_exec(
            db,
            "SELECT id, body IS NULL, LENGTH(body), body, data IS NULL, LENGTH(data), HEX(data) "
            "FROM content_posts",
            blob_row_callback,
            &blob_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(blob_rows.rows == 4);
    assert(blob_rows.found_text_binary);
    assert(blob_rows.found_nulls);
    assert(blob_rows.found_empty);
    assert(blob_rows.found_large);
    assert(
        mylite_exec(
            db,
            "SELECT id, title, LENGTH(body), body IS NULL FROM mutable_posts",
            mutable_row_callback,
            &mutable_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(mutable_rows.rows == 2);
    assert(mutable_rows.found_updated);
    assert(mutable_rows.found_untouched);
    assert(
        mylite_exec(
            db,
            "SELECT id, title FROM auto_posts",
            auto_row_callback,
            &auto_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(auto_rows.rows == 5);
    assert(auto_rows.found_first);
    assert(auto_rows.found_manual);
    assert(auto_rows.found_after_manual);
    assert(auto_rows.found_after_alter);
    assert(auto_rows.found_after_low_alter);
    assert_exec_succeeds(db, "INSERT INTO innodb_posts VALUES (1, 'draft')");
    assert_exec_succeeds(db, "DROP TABLE drop_posts");
    assert(mylite_storage_table_exists(filename, "app", "drop_posts") == MYLITE_STORAGE_NOTFOUND);
    assert_catalog_table_count(filename, "app", 13U);
    assert_exec_succeeds(
        db,
        "CREATE TABLE drop_posts (id INT NOT NULL, title VARCHAR(255) NOT NULL) ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "app", 14U);
    assert(
        mylite_exec(db, "SELECT id, title FROM drop_posts", row_callback, &drop_rows, &errmsg) ==
        MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(drop_rows.rows == 0);
    assert_exec_succeeds(db, "DROP TABLE aria_posts");
    assert(mylite_storage_table_exists(filename, "app", "aria_posts") == MYLITE_STORAGE_NOTFOUND);
    assert_catalog_table_count(filename, "app", 13U);
    assert_exec_succeeds(db, "RENAME TABLE row_posts TO renamed_row_posts");
    assert(mylite_storage_table_exists(filename, "app", "row_posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_table_exists(filename, "app", "renamed_row_posts") == MYLITE_STORAGE_OK);
    assert_exec_fails(db, "SELECT id, title FROM row_posts");
    assert(
        mylite_exec(
            db,
            "SELECT id, title FROM renamed_row_posts",
            post_row_callback,
            &renamed_row_posts,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(renamed_row_posts.rows == 2);
    assert(renamed_row_posts.found_draft);
    assert(renamed_row_posts.found_published);
    assert_exec_succeeds(db, "RENAME TABLE myisam_posts TO renamed_posts");
    assert(mylite_storage_table_exists(filename, "app", "myisam_posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_table_exists(filename, "app", "renamed_posts") == MYLITE_STORAGE_OK);
    assert_catalog_table_count(filename, "app", 13U);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    tables = (table_context){0};
    drop_rows = (table_context){0};
    renamed_row_posts = (post_row_context){0};
    nullable_rows = (nullable_row_context){0};
    blob_rows = (blob_row_context){0};
    mutable_rows = (mutable_row_context){0};
    auto_rows = (auto_row_context){0};
    checked_rating = (single_value_context){.expected_value = "4"};
    checked_disabled_rating = (single_value_context){.expected_value = "7"};
    checked_row_count = (single_value_context){.expected_value = "5"};
    generated_title_len = (single_value_context){.expected_value = "9"};
    generated_label = (single_value_context){.expected_value = "published-1"};
    generated_title_len_index = (single_value_context){.expected_value = "1"};
    generated_label_index = (single_value_context){.expected_value = "1"};
    generated_reused_title_key = (single_value_context){.expected_value = "3"};
    generated_deleted_title_key = (single_value_context){.expected_value = "4"};
    rollback_count = (single_value_context){.expected_value = "3"};
    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "app");
    assert_exec_succeeds(db, "USE app");
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 13);
    assert(
        mylite_exec(db, "SELECT * FROM innodb_posts", row_callback, &rows, &errmsg) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id, title FROM renamed_row_posts",
            post_row_callback,
            &renamed_row_posts,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(renamed_row_posts.rows == 2);
    assert(renamed_row_posts.found_draft);
    assert(renamed_row_posts.found_published);
    assert(
        mylite_exec(
            db,
            "SELECT id, title, score FROM nullable_posts",
            nullable_row_callback,
            &nullable_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(nullable_rows.rows == 2);
    assert(nullable_rows.found_null_row);
    assert(nullable_rows.found_value_row);
    assert(
        mylite_exec(
            db,
            "SELECT id, body IS NULL, LENGTH(body), body, data IS NULL, LENGTH(data), HEX(data) "
            "FROM content_posts",
            blob_row_callback,
            &blob_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(blob_rows.rows == 4);
    assert(blob_rows.found_text_binary);
    assert(blob_rows.found_nulls);
    assert(blob_rows.found_empty);
    assert(blob_rows.found_large);
    assert(
        mylite_exec(
            db,
            "SELECT id, title, LENGTH(body), body IS NULL FROM mutable_posts",
            mutable_row_callback,
            &mutable_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(mutable_rows.rows == 2);
    assert(mutable_rows.found_updated);
    assert(mutable_rows.found_untouched);
    assert_exec_succeeds(db, "INSERT INTO auto_posts (title) VALUES ('reopened')");
    assert_exec_succeeds(db, "ALTER TABLE auto_posts AUTO_INCREMENT = 80");
    assert_exec_succeeds(db, "INSERT INTO auto_posts (title) VALUES ('after reopened alter')");
    assert(
        mylite_exec(
            db,
            "SELECT id, title FROM auto_posts",
            auto_row_callback,
            &auto_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(auto_rows.rows == 7);
    assert(auto_rows.found_first);
    assert(auto_rows.found_manual);
    assert(auto_rows.found_after_manual);
    assert(auto_rows.found_after_alter);
    assert(auto_rows.found_after_low_alter);
    assert(auto_rows.found_reopened);
    assert(auto_rows.found_after_reopened_alter);
    assert_exec_fails_with_message(db, "INSERT INTO checked_posts VALUES (5, 10)", "CONSTRAINT");
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM checked_posts",
            single_value_callback,
            &checked_row_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(checked_row_count.rows == 1);
    assert_exec_succeeds(db, "INSERT INTO checked_posts VALUES (7, 9)");
    assert_exec_succeeds(db, "ALTER TABLE checked_posts DROP CONSTRAINT rating_reopen_cap");
    assert_exec_succeeds(db, "INSERT INTO checked_posts VALUES (5, 10)");
    assert(
        mylite_exec(
            db,
            "SELECT rating FROM checked_posts WHERE id = 1",
            single_value_callback,
            &checked_rating,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(checked_rating.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT rating FROM checked_posts WHERE id = 2",
            single_value_callback,
            &checked_disabled_rating,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(checked_disabled_rating.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM generated_posts WHERE id = 1",
            single_value_callback,
            &generated_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT label FROM generated_posts WHERE id = 1",
            single_value_callback,
            &generated_label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_label.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (title_len_key) "
            "WHERE title_len = 9 AND id = 1",
            single_value_callback,
            &generated_title_len_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_title_len_index.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (label_key) WHERE label = 'published-1'",
            single_value_callback,
            &generated_label_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_label_index.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (title_key_unique) "
            "WHERE title_key = 'draft-key'",
            single_value_callback,
            &generated_reused_title_key,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_reused_title_key.rows == 1);
    assert_exec_succeeds(db, "DELETE FROM generated_posts WHERE id = 3");
    assert_exec_succeeds(db, "INSERT INTO generated_posts (id, title) VALUES (4, 'draft')");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_posts FORCE INDEX (title_key_unique) "
            "WHERE title_key = 'draft-key'",
            single_value_callback,
            &generated_deleted_title_key,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_deleted_title_key.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE title = 'duplicate'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE title = 'prepared-duplicate'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts WHERE id = 3 AND title = 'third'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE title = 'direct-replaced'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE title = 'prepared-replaced'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "1"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE id = 2 AND title = 'second'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    rollback_count = (single_value_context){.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM rollback_posts FORCE INDEX (title) "
            "WHERE title = 'select-replaced'",
            single_value_callback,
            &rollback_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rollback_count.rows == 1);
    assert(
        mylite_exec(db, "SELECT id, title FROM drop_posts", row_callback, &drop_rows, &errmsg) ==
        MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(drop_rows.rows == 0);
    assert_catalog_table_count(filename, "app", 13U);
    assert_catalog_table_metadata(filename, "app", "innodb_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "renamed_row_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "renamed_posts", "MyISAM", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "nullable_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "auto_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "content_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "mutable_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "drop_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "checked_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "generated_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "rollback_posts", "InnoDB", "MYLITE");
    assert(mylite_storage_table_exists(filename, "app", "row_posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_table_exists(filename, "app", "myisam_posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_table_exists(filename, "app", "aria_posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_check_constraint_if_exists(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE constraint_if_exists");
    assert_exec_succeeds(db, "USE constraint_if_exists");
    assert_exec_succeeds(
        db,
        "CREATE TABLE constraint_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "rating INT NOT NULL, "
        "CONSTRAINT rating_max CHECK (rating <= 10)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "INSERT INTO constraint_posts VALUES (1, 10)");
    assert_catalog_table_count(filename, "constraint_if_exists", 1U);
    assert_catalog_table_metadata(
        filename,
        "constraint_if_exists",
        "constraint_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_check_constraint_count(db, "constraint_if_exists", "constraint_posts", "rating_max", 1U);

    assert_exec_succeeds(
        db,
        "ALTER TABLE constraint_posts "
        "ADD CONSTRAINT IF NOT EXISTS rating_max CHECK (rating <= 10), "
        "ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "rating_max");
    assert_check_constraint_count(db, "constraint_if_exists", "constraint_posts", "rating_max", 1U);
    assert_exec_fails_with_message(db, "INSERT INTO constraint_posts VALUES (2, 11)", "CONSTRAINT");

    assert_exec_succeeds(
        db,
        "ALTER TABLE constraint_posts "
        "ADD CONSTRAINT IF NOT EXISTS rating_min CHECK (rating >= 0), "
        "ALGORITHM=COPY"
    );
    assert_check_constraint_count(db, "constraint_if_exists", "constraint_posts", "rating_min", 1U);
    assert_exec_fails_with_message(db, "INSERT INTO constraint_posts VALUES (3, -1)", "CONSTRAINT");

    assert_exec_succeeds(
        db,
        "ALTER TABLE constraint_posts "
        "DROP CONSTRAINT IF EXISTS missing_rating, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_rating");
    assert_check_constraint_count(db, "constraint_if_exists", "constraint_posts", "rating_min", 1U);
    assert_exec_fails_with_message(db, "INSERT INTO constraint_posts VALUES (4, -1)", "CONSTRAINT");

    assert_exec_succeeds(
        db,
        "ALTER TABLE constraint_posts "
        "DROP CONSTRAINT IF EXISTS rating_min, ALGORITHM=COPY"
    );
    assert_check_constraint_count(db, "constraint_if_exists", "constraint_posts", "rating_min", 0U);
    assert_exec_succeeds(db, "INSERT INTO constraint_posts VALUES (5, -1)");
    assert_exec_fails_with_message(db, "INSERT INTO constraint_posts VALUES (6, 11)", "CONSTRAINT");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE constraint_if_exists");
    assert_check_constraint_count(db, "constraint_if_exists", "constraint_posts", "rating_max", 1U);
    assert_check_constraint_count(db, "constraint_if_exists", "constraint_posts", "rating_min", 0U);
    assert_exec_succeeds(db, "INSERT INTO constraint_posts VALUES (7, -2)");
    assert_exec_fails_with_message(db, "INSERT INTO constraint_posts VALUES (8, 11)", "CONSTRAINT");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_non_check_constraint_ddl(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context primary_index_rows = {0};
    table_context slug_index_rows = {0};
    table_context author_index_rows = {0};
    table_context reopened_primary_index_rows = {0};
    table_context reopened_slug_index_rows = {0};
    table_context reopened_author_index_rows = {0};
    table_context missing_author_index_rows = {0};
    table_context category_index_rows = {0};
    table_context reopened_missing_author_index_rows = {0};
    table_context reopened_category_index_rows = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE non_check_constraints");
    assert_exec_succeeds(db, "USE non_check_constraints");
    assert_exec_succeeds(
        db,
        "CREATE TABLE constraint_posts ("
        "id INT NOT NULL, "
        "slug VARCHAR(64) NOT NULL, "
        "author VARCHAR(64) NOT NULL, "
        "category VARCHAR(64) NOT NULL, "
        "CONSTRAINT posts_pk PRIMARY KEY (id), "
        "CONSTRAINT slug_unique UNIQUE (slug)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "INSERT INTO constraint_posts VALUES (1, 'alpha', 'jan', 'news')");
    assert_exec_succeeds(db, "INSERT INTO constraint_posts VALUES (2, 'beta', 'jane', 'tech')");
    assert_catalog_table_count(filename, "non_check_constraints", 1U);
    assert_catalog_table_metadata(
        filename,
        "non_check_constraints",
        "constraint_posts",
        "InnoDB",
        "MYLITE"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'PRIMARY'",
            table_callback,
            &primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(primary_index_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'slug_unique'",
            table_callback,
            &slug_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(slug_index_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO constraint_posts VALUES (1, 'gamma', 'jim', 'docs')");
    assert_exec_fails(db, "INSERT INTO constraint_posts VALUES (3, 'beta', 'jim', 'docs')");
    assert_query_single_value(
        db,
        "SELECT id FROM constraint_posts FORCE INDEX (slug_unique) WHERE slug = 'beta'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE constraint_posts "
        "ADD CONSTRAINT author_unique UNIQUE (author), ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'author_unique'",
            table_callback,
            &author_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(author_index_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO constraint_posts VALUES (3, 'gamma', 'jan', 'docs')");
    assert_query_single_value(
        db,
        "SELECT id FROM constraint_posts FORCE INDEX (author_unique) WHERE author = 'jan'",
        "1"
    );
    assert_exec_succeeds(
        db,
        "ALTER TABLE constraint_posts "
        "ADD CONSTRAINT author_unique UNIQUE IF NOT EXISTS (author), ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "author_unique");
    assert_catalog_table_count(filename, "non_check_constraints", 1U);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE non_check_constraints");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'PRIMARY'",
            table_callback,
            &reopened_primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_primary_index_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'slug_unique'",
            table_callback,
            &reopened_slug_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_slug_index_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'author_unique'",
            table_callback,
            &reopened_author_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_author_index_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO constraint_posts VALUES (3, 'gamma', 'jan', 'docs')");
    assert_query_single_value(
        db,
        "SELECT id FROM constraint_posts FORCE INDEX (author_unique) WHERE author = 'jane'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE constraint_posts "
        "DROP CONSTRAINT IF EXISTS missing_unique, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_unique");
    assert_exec_succeeds(
        db,
        "ALTER TABLE constraint_posts DROP CONSTRAINT author_unique, ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'author_unique'",
            table_callback,
            &missing_author_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(missing_author_index_rows.rows == 0);
    assert_exec_fails(
        db,
        "SELECT id FROM constraint_posts FORCE INDEX (author_unique) WHERE author = 'jan'"
    );
    assert_exec_succeeds(db, "INSERT INTO constraint_posts VALUES (3, 'gamma', 'jan', 'docs')");
    assert_exec_succeeds(
        db,
        "ALTER TABLE constraint_posts "
        "ADD CONSTRAINT category_unique UNIQUE (category), ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'category_unique'",
            table_callback,
            &category_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(category_index_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO constraint_posts VALUES (4, 'delta', 'jill', 'news')");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE non_check_constraints");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'author_unique'",
            table_callback,
            &reopened_missing_author_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_missing_author_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM constraint_posts WHERE Key_name = 'category_unique'",
            table_callback,
            &reopened_category_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_category_index_rows.rows == 1);
    assert_query_single_value(
        db,
        "SELECT id FROM constraint_posts FORCE INDEX (category_unique) WHERE category = 'docs'",
        "3"
    );
    assert_exec_succeeds(db, "INSERT INTO constraint_posts VALUES (4, 'delta', 'jan', 'notes')");
    assert_exec_fails(db, "INSERT INTO constraint_posts VALUES (5, 'epsilon', 'jim', 'news')");
    assert_catalog_table_count(filename, "non_check_constraints", 1U);
    assert_catalog_table_metadata(
        filename,
        "non_check_constraints",
        "constraint_posts",
        "InnoDB",
        "MYLITE"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_primary_key_alter_ddl(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context primary_index_rows = {0};
    table_context duplicate_primary_index_rows = {0};
    table_context reopened_primary_index_rows = {0};
    table_context dropped_primary_index_rows = {0};
    table_context failed_readd_primary_index_rows = {0};
    table_context readded_primary_index_rows = {0};
    table_context final_primary_index_rows = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE primary_key_alter");
    assert_exec_succeeds(db, "USE primary_key_alter");
    assert_exec_succeeds(
        db,
        "CREATE TABLE pk_posts ("
        "id INT NOT NULL, "
        "slug VARCHAR(64) NOT NULL, "
        "category VARCHAR(64) NOT NULL, "
        "KEY category_key (category)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO pk_posts VALUES "
        "(1, 'alpha', 'news'), "
        "(2, 'beta', 'tech')"
    );
    assert_catalog_table_count(filename, "primary_key_alter", 1U);
    assert_catalog_table_metadata(filename, "primary_key_alter", "pk_posts", "InnoDB", "MYLITE");

    assert_exec_succeeds(
        db,
        "ALTER TABLE pk_posts ADD CONSTRAINT posts_pk PRIMARY KEY (id), ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM pk_posts WHERE Key_name = 'PRIMARY'",
            table_callback,
            &primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(primary_index_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO pk_posts VALUES (1, 'duplicate-id', 'news')");
    assert_query_single_value(
        db,
        "SELECT slug FROM pk_posts FORCE INDEX (PRIMARY) WHERE id = 2",
        "beta"
    );
    assert_exec_succeeds(
        db,
        "ALTER TABLE pk_posts ADD CONSTRAINT posts_pk PRIMARY KEY IF NOT EXISTS (id), "
        "ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "primary key");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM pk_posts WHERE Key_name = 'PRIMARY'",
            table_callback,
            &duplicate_primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(duplicate_primary_index_rows.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE primary_key_alter");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM pk_posts WHERE Key_name = 'PRIMARY'",
            table_callback,
            &reopened_primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_primary_index_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO pk_posts VALUES (1, 'duplicate-id', 'news')");
    assert_query_single_value(
        db,
        "SELECT id FROM pk_posts FORCE INDEX (category_key) WHERE category = 'news'",
        "1"
    );

    assert_exec_succeeds(db, "ALTER TABLE pk_posts DROP PRIMARY KEY, ALGORITHM=COPY");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM pk_posts WHERE Key_name = 'PRIMARY'",
            table_callback,
            &dropped_primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(dropped_primary_index_rows.rows == 0);
    assert_exec_fails(db, "SELECT slug FROM pk_posts FORCE INDEX (PRIMARY) WHERE id = 1");
    assert_exec_succeeds(db, "INSERT INTO pk_posts VALUES (1, 'alpha-copy', 'news')");
    assert_query_single_value(db, "SELECT COUNT(*) FROM pk_posts WHERE id = 1", "2");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM pk_posts FORCE INDEX (category_key) WHERE category = 'news'",
        "2"
    );

    assert_exec_fails(db, "ALTER TABLE pk_posts ADD PRIMARY KEY (id), ALGORITHM=COPY");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM pk_posts WHERE Key_name = 'PRIMARY'",
            table_callback,
            &failed_readd_primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(failed_readd_primary_index_rows.rows == 0);
    assert_query_single_value(db, "SELECT COUNT(*) FROM pk_posts WHERE id = 1", "2");

    assert_exec_succeeds(db, "DELETE FROM pk_posts WHERE slug = 'alpha-copy'");
    assert_exec_succeeds(db, "ALTER TABLE pk_posts ADD PRIMARY KEY (id), ALGORITHM=COPY");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM pk_posts WHERE Key_name = 'PRIMARY'",
            table_callback,
            &readded_primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(readded_primary_index_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO pk_posts VALUES (2, 'duplicate-beta', 'tech')");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE primary_key_alter");
    assert_catalog_table_count(filename, "primary_key_alter", 1U);
    assert_catalog_table_metadata(filename, "primary_key_alter", "pk_posts", "InnoDB", "MYLITE");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM pk_posts WHERE Key_name = 'PRIMARY'",
            table_callback,
            &final_primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(final_primary_index_rows.rows == 1);
    assert_query_single_value(db, "SELECT COUNT(*) FROM pk_posts", "2");
    assert_query_single_value(
        db,
        "SELECT slug FROM pk_posts FORCE INDEX (PRIMARY) WHERE id = 1",
        "alpha"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM pk_posts FORCE INDEX (category_key) WHERE category = 'tech'",
        "2"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_failed_add_unique_constraint_rollback(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context missing_unique_index_rows = {0};
    table_context reopened_missing_unique_index_rows = {0};
    table_context added_unique_index_rows = {0};
    table_context final_unique_index_rows = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE unique_constraint_rollback");
    assert_exec_succeeds(db, "USE unique_constraint_rollback");
    assert_exec_succeeds(
        db,
        "CREATE TABLE unique_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "author VARCHAR(64) NOT NULL, "
        "category VARCHAR(64) NOT NULL, "
        "KEY category_key (category)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO unique_posts VALUES "
        "(1, 'jan', 'news'), "
        "(2, 'jan', 'tech'), "
        "(3, 'jane', 'news')"
    );
    assert_catalog_table_count(filename, "unique_constraint_rollback", 1U);
    assert_catalog_table_metadata(
        filename,
        "unique_constraint_rollback",
        "unique_posts",
        "InnoDB",
        "MYLITE"
    );

    assert_exec_fails_with_message(
        db,
        "ALTER TABLE unique_posts ADD CONSTRAINT author_unique UNIQUE (author), "
        "ALGORITHM=COPY",
        "Duplicate"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM unique_posts WHERE Key_name = 'author_unique'",
            table_callback,
            &missing_unique_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(missing_unique_index_rows.rows == 0);
    assert_exec_fails(
        db,
        "SELECT id FROM unique_posts FORCE INDEX (author_unique) WHERE author = 'jan'"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM unique_posts WHERE author = 'jan'", "2");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM unique_posts FORCE INDEX (category_key) WHERE category = 'news'",
        "2"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE unique_constraint_rollback");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM unique_posts WHERE Key_name = 'author_unique'",
            table_callback,
            &reopened_missing_unique_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_missing_unique_index_rows.rows == 0);
    assert_query_single_value(db, "SELECT COUNT(*) FROM unique_posts WHERE author = 'jan'", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM unique_posts FORCE INDEX (category_key) WHERE category = 'tech'",
        "2"
    );

    assert_exec_succeeds(db, "DELETE FROM unique_posts WHERE id = 2");
    assert_exec_succeeds(
        db,
        "ALTER TABLE unique_posts ADD CONSTRAINT author_unique UNIQUE (author), "
        "ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM unique_posts WHERE Key_name = 'author_unique'",
            table_callback,
            &added_unique_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(added_unique_index_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO unique_posts VALUES (4, 'jan', 'docs')");
    assert_query_single_value(
        db,
        "SELECT id FROM unique_posts FORCE INDEX (author_unique) WHERE author = 'jane'",
        "3"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE unique_constraint_rollback");
    assert_catalog_table_count(filename, "unique_constraint_rollback", 1U);
    assert_catalog_table_metadata(
        filename,
        "unique_constraint_rollback",
        "unique_posts",
        "InnoDB",
        "MYLITE"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM unique_posts WHERE Key_name = 'author_unique'",
            table_callback,
            &final_unique_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(final_unique_index_rows.rows == 1);
    assert_query_single_value(db, "SELECT COUNT(*) FROM unique_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM unique_posts FORCE INDEX (author_unique) WHERE author = 'jan'",
        "1"
    );
    assert_exec_fails(db, "INSERT INTO unique_posts VALUES (5, 'jane', 'docs')");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_create_table_if_not_exists(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE create_if_not_exists");
    assert_exec_succeeds(db, "USE create_if_not_exists");
    assert_exec_succeeds(
        db,
        "CREATE TABLE existing_posts ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO existing_posts (slug, body) VALUES "
        "('existing-alpha', 'existing body one'), "
        "('existing-beta', 'existing body two')"
    );
    assert_catalog_table_count(filename, "create_if_not_exists", 1U);
    assert_catalog_table_metadata(
        filename,
        "create_if_not_exists",
        "existing_posts",
        "InnoDB",
        "MYLITE"
    );

    assert_exec_succeeds(
        db,
        "CREATE TABLE IF NOT EXISTS existing_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=MyISAM"
    );
    assert_warning_message_contains(db, "existing_posts");
    assert_catalog_table_count(filename, "create_if_not_exists", 1U);
    assert_catalog_table_metadata(
        filename,
        "create_if_not_exists",
        "existing_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM existing_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM existing_posts FORCE INDEX (slug_key) "
        "WHERE slug = 'existing-beta'",
        "2"
    );
    assert_exec_fails(
        db,
        "INSERT INTO existing_posts (slug, body) VALUES "
        "('existing-beta', 'duplicate existing body')"
    );

    assert_exec_succeeds(
        db,
        "CREATE TABLE IF NOT EXISTS fresh_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=Aria"
    );
    assert_catalog_table_count(filename, "create_if_not_exists", 2U);
    assert_catalog_table_metadata(
        filename,
        "create_if_not_exists",
        "fresh_posts",
        "Aria",
        "MYLITE"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO fresh_posts VALUES "
        "(1, 'fresh-alpha', 'fresh body one'), "
        "(2, 'fresh-beta', 'fresh body two')"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM fresh_posts FORCE INDEX (body_prefix) WHERE body = 'fresh body one'",
        "1"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "create_if_not_exists");
    assert_exec_succeeds(db, "USE create_if_not_exists");
    assert_catalog_table_count(filename, "create_if_not_exists", 2U);
    assert_catalog_table_metadata(
        filename,
        "create_if_not_exists",
        "existing_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "create_if_not_exists",
        "fresh_posts",
        "Aria",
        "MYLITE"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM existing_posts", "2");
    assert_query_single_value(db, "SELECT COUNT(*) FROM fresh_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM existing_posts FORCE INDEX (body_prefix) "
        "WHERE body = 'existing body one'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM fresh_posts FORCE INDEX (slug_key) WHERE slug = 'fresh-beta'",
        "2"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_alter_table_rebuilds_keyless_rows(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    alter_row_context rows = {0};
    table_context tables = {0};
    table_context reopened_headline_rows = {0};
    table_context reopened_status_rows = {0};
    table_context dropped_headline_index_rows = {0};
    table_context dropped_status_index_rows = {0};
    single_value_context reopened_status_count = {.expected_value = "2"};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE alter_posts (id INT NOT NULL, title VARCHAR(32) NOT NULL, notes TEXT NULL) "
        "ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "INSERT INTO alter_posts VALUES (1, 'first', 'alpha')");
    assert_exec_succeeds(db, "INSERT INTO alter_posts VALUES (2, 'second', REPEAT('large-', 700))");
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "alter_posts", "InnoDB", "MYLITE");

    assert_exec_succeeds(
        db,
        "ALTER TABLE alter_posts ADD COLUMN status VARCHAR(16) NOT NULL DEFAULT 'draft', "
        "ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "ALTER TABLE alter_posts ADD COLUMN drop_me INT NOT NULL DEFAULT 7, ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "ALTER TABLE alter_posts CHANGE COLUMN title headline VARCHAR(64) NOT NULL, "
        "ALGORITHM=COPY"
    );
    assert_exec_succeeds(db, "ALTER TABLE alter_posts DROP COLUMN drop_me, ALGORITHM=COPY");
    assert_exec_succeeds(db, "ALTER TABLE alter_posts ENGINE=InnoDB, ALGORITHM=COPY");
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "alter_posts", "InnoDB", "MYLITE");

    assert_online_alter_exec_fails(
        db,
        "ALTER TABLE alter_posts ADD COLUMN blocked INT NULL, ALGORITHM=COPY, LOCK=NONE"
    );
    assert_exec_succeeds(db, "ALTER TABLE alter_posts ADD PRIMARY KEY (id), ALGORITHM=COPY");
    assert_exec_fails(
        db,
        "INSERT INTO alter_posts (id, headline, status, notes) VALUES (1, 'duplicate', 'draft', "
        "NULL)"
    );
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "alter_posts", "InnoDB", "MYLITE");

    assert(
        mylite_exec(
            db,
            "SELECT id, headline, status, LENGTH(notes), notes IS NULL FROM alter_posts",
            alter_row_callback,
            &rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rows.rows == 2);
    assert(rows.found_first);
    assert(rows.found_large);
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    rows = (alter_row_context){0};
    tables = (table_context){0};
    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "app");
    assert_exec_succeeds(db, "USE app");
    assert(
        mylite_exec(
            db,
            "SELECT id, headline, status, LENGTH(notes), notes IS NULL FROM alter_posts",
            alter_row_callback,
            &rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rows.rows == 2);
    assert(rows.found_first);
    assert(rows.found_large);
    assert(mylite_exec(db, "SHOW TABLES", table_callback, &tables, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(tables.rows == 1);
    assert_exec_succeeds(
        db,
        "ALTER TABLE alter_posts ADD COLUMN reopened_status VARCHAR(16) NOT NULL DEFAULT 'open'"
    );
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM alter_posts WHERE reopened_status = 'open'",
            single_value_callback,
            &reopened_status_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_status_count.rows == 1);
    assert_exec_succeeds(db, "ALTER TABLE alter_posts ADD KEY reopened_headline_key (headline)");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM alter_posts FORCE INDEX (reopened_headline_key) "
            "WHERE headline = 'second'",
            row_callback,
            &reopened_headline_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_headline_rows.rows == 1);
    assert_exec_succeeds(db, "CREATE INDEX reopened_status_key ON alter_posts (status)");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM alter_posts FORCE INDEX (reopened_status_key) WHERE status = 'draft'",
            row_callback,
            &reopened_status_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_status_rows.rows == 2);
    assert_exec_succeeds(db, "DROP INDEX reopened_status_key ON alter_posts");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM alter_posts WHERE Key_name = 'reopened_status_key'",
            table_callback,
            &dropped_status_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(dropped_status_index_rows.rows == 0);
    assert_exec_succeeds(db, "ALTER TABLE alter_posts DROP INDEX reopened_headline_key");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM alter_posts WHERE Key_name = 'reopened_headline_key'",
            table_callback,
            &dropped_headline_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(dropped_headline_index_rows.rows == 0);
    assert_exec_succeeds(
        db,
        "ALTER TABLE alter_posts CHANGE COLUMN headline reopened_headline VARCHAR(64) NOT NULL"
    );
    rows = (alter_row_context){0};
    assert(
        mylite_exec(
            db,
            "SELECT id, reopened_headline, status, LENGTH(notes), notes IS NULL FROM alter_posts",
            alter_row_callback,
            &rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rows.rows == 2);
    assert(rows.found_first);
    assert(rows.found_large);
    assert_exec_succeeds(db, "ALTER TABLE alter_posts DROP COLUMN reopened_status");
    assert_exec_fails(db, "SELECT reopened_status FROM alter_posts");
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "alter_posts", "InnoDB", "MYLITE");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_column_alter_if_exists(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE column_if_exists");
    assert_exec_succeeds(db, "USE column_if_exists");
    assert_exec_succeeds(
        db,
        "CREATE TABLE column_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "title VARCHAR(64) NOT NULL, "
        "body LONGTEXT NULL, "
        "UNIQUE KEY slug_key (slug), "
        "KEY title_key (title), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO column_posts VALUES "
        "(1, 'alpha', 'Alpha title', 'alpha body'), "
        "(2, 'beta', 'Beta title', 'beta body')"
    );
    assert_catalog_table_count(filename, "column_if_exists", 1U);
    assert_catalog_table_metadata(filename, "column_if_exists", "column_posts", "InnoDB", "MYLITE");

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "ADD COLUMN IF NOT EXISTS title VARCHAR(128) NOT NULL, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "title");
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (title_key) WHERE title = 'Beta title'",
        "2"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM column_posts", "2");

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "ADD COLUMN IF NOT EXISTS subtitle VARCHAR(64) NOT NULL DEFAULT 'untitled', "
        "ALGORITHM=COPY"
    );
    assert_catalog_table_count(filename, "column_if_exists", 1U);
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM column_posts WHERE subtitle = 'untitled'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts DROP COLUMN IF EXISTS missing_subtitle, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_subtitle");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM column_posts WHERE subtitle = 'untitled'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (body_prefix) WHERE body = 'alpha body'",
        "1"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts DROP COLUMN IF EXISTS subtitle, ALGORITHM=COPY"
    );
    assert_exec_fails(db, "SELECT subtitle FROM column_posts");
    assert_query_single_value(db, "SELECT COUNT(*) FROM column_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (slug_key) WHERE slug = 'beta'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "MODIFY COLUMN IF EXISTS missing_title VARCHAR(96) NOT NULL, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_title");
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (title_key) WHERE title = 'Beta title'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "MODIFY COLUMN IF EXISTS title VARCHAR(96) NOT NULL, ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "UPDATE column_posts SET title = "
        "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789plus' WHERE id = 1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (title_key) WHERE title = "
        "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789plus'",
        "1"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "CHANGE COLUMN IF EXISTS missing_headline headline VARCHAR(96) NOT NULL, "
        "ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_headline");
    assert_exec_fails(db, "SELECT headline FROM column_posts");

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "CHANGE COLUMN IF EXISTS title headline VARCHAR(96) NOT NULL, ALGORITHM=COPY"
    );
    assert_exec_fails(db, "SELECT title FROM column_posts");
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (title_key) WHERE headline = "
        "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789plus'",
        "1"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "MODIFY COLUMN IF EXISTS title VARCHAR(128) NOT NULL, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "title");
    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "MODIFY COLUMN IF EXISTS headline VARCHAR(128) NOT NULL, ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "UPDATE column_posts SET headline = "
        "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzA"
        "BCDEFGHIJKL' "
        "WHERE id = 2"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (title_key) WHERE headline = "
        "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzA"
        "BCDEFGHIJKL'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "RENAME COLUMN IF EXISTS missing_final_headline TO final_headline, "
        "ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_final_headline");
    assert_exec_fails(db, "SELECT final_headline FROM column_posts");

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "RENAME COLUMN IF EXISTS headline TO final_headline, ALGORITHM=COPY"
    );
    assert_exec_fails(db, "SELECT headline FROM column_posts");
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (title_key) WHERE final_headline = "
        "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789plus'",
        "1"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "ALTER COLUMN IF EXISTS missing_slug SET DEFAULT 'missing', ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_slug");
    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "ALTER COLUMN IF EXISTS slug SET DEFAULT 'gamma', ALGORITHM=COPY"
    );
    char *create_sql = capture_show_create_table(db, "column_posts");
    assert(strstr(create_sql, "DEFAULT 'gamma'") != NULL);
    free(create_sql);
    assert_exec_succeeds(
        db,
        "INSERT INTO column_posts (id, final_headline, body) "
        "VALUES (3, 'Gamma title', 'gamma body')"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (slug_key) WHERE slug = 'gamma'",
        "3"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts "
        "ALTER COLUMN IF EXISTS missing_slug DROP DEFAULT, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_slug");
    assert_exec_succeeds(
        db,
        "ALTER TABLE column_posts ALTER COLUMN IF EXISTS slug DROP DEFAULT, ALGORITHM=COPY"
    );
    create_sql = capture_show_create_table(db, "column_posts");
    assert(strstr(create_sql, "DEFAULT 'gamma'") == NULL);
    free(create_sql);
    assert_query_single_value(db, "SELECT COUNT(*) FROM column_posts", "3");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "column_if_exists");
    assert_exec_succeeds(db, "USE column_if_exists");
    assert_catalog_table_count(filename, "column_if_exists", 1U);
    assert_catalog_table_metadata(filename, "column_if_exists", "column_posts", "InnoDB", "MYLITE");
    assert_exec_fails(db, "SELECT subtitle FROM column_posts");
    assert_exec_fails(db, "SELECT title FROM column_posts");
    assert_exec_fails(db, "SELECT headline FROM column_posts");
    assert_query_single_value(db, "SELECT COUNT(*) FROM column_posts", "3");
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (title_key) WHERE final_headline = "
        "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789plus'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (title_key) WHERE final_headline = "
        "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzA"
        "BCDEFGHIJKL'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (slug_key) WHERE slug = 'gamma'",
        "3"
    );
    create_sql = capture_show_create_table(db, "column_posts");
    assert(strstr(create_sql, "DEFAULT 'gamma'") == NULL);
    free(create_sql);
    assert_query_single_value(
        db,
        "SELECT id FROM column_posts FORCE INDEX (body_prefix) WHERE body = 'beta body'",
        "2"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_generated_column_alter_ddl(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    single_value_context title_len = {.expected_value = "5"};
    single_value_context label = {.expected_value = "draft-1"};
    single_value_context modified_title_len = {.expected_value = "6"};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE generated_alter_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "INSERT INTO generated_alter_posts VALUES (1, 'draft')");
    assert_exec_succeeds(
        db,
        "ALTER TABLE generated_alter_posts "
        "ADD COLUMN title_len INT AS (CHAR_LENGTH(title)) VIRTUAL, "
        "ADD COLUMN label VARCHAR(80) AS (CONCAT(title, '-', id)) STORED, "
        "ALGORITHM=COPY"
    );
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "generated_alter_posts", "InnoDB", "MYLITE");
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM generated_alter_posts WHERE id = 1",
            single_value_callback,
            &title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT label FROM generated_alter_posts WHERE id = 1",
            single_value_callback,
            &label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(label.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    title_len = (single_value_context){.expected_value = "5"};
    label = (single_value_context){.expected_value = "draft-1"};
    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "app");
    assert_exec_succeeds(db, "USE app");
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM generated_alter_posts WHERE id = 1",
            single_value_callback,
            &title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT label FROM generated_alter_posts WHERE id = 1",
            single_value_callback,
            &label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(label.rows == 1);
    assert_exec_succeeds(
        db,
        "ALTER TABLE generated_alter_posts "
        "MODIFY COLUMN title_len INT AS (CHAR_LENGTH(title) + 1) VIRTUAL, "
        "ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM generated_alter_posts WHERE id = 1",
            single_value_callback,
            &modified_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(modified_title_len.rows == 1);
    assert_exec_succeeds(db, "ALTER TABLE generated_alter_posts DROP COLUMN label, ALGORITHM=COPY");
    assert_exec_fails(db, "SELECT label FROM generated_alter_posts");
    modified_title_len = (single_value_context){.expected_value = "6"};
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM generated_alter_posts WHERE id = 1",
            single_value_callback,
            &modified_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(modified_title_len.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    modified_title_len = (single_value_context){.expected_value = "6"};
    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_fails(db, "SELECT label FROM generated_alter_posts");
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM generated_alter_posts WHERE id = 1",
            single_value_callback,
            &modified_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(modified_title_len.rows == 1);
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "generated_alter_posts", "InnoDB", "MYLITE");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_generated_primary_key_policy(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    single_value_context title = {.expected_value = "draft"};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_fails_with_message(
        db,
        "CREATE TABLE generated_primary_create_posts ("
        "title VARCHAR(64) NOT NULL, "
        "slug VARCHAR(80) AS (CONCAT(title, '-slug')) STORED, "
        "PRIMARY KEY (slug)"
        ") ENGINE=InnoDB",
        "Primary key cannot be defined upon a generated column"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "generated_primary_create_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "app", 0U);

    assert_exec_succeeds(
        db,
        "CREATE TABLE generated_primary_alter_posts ("
        "id INT NOT NULL, "
        "title VARCHAR(64) NOT NULL, "
        "slug VARCHAR(80) AS (CONCAT(title, '-slug')) STORED"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO generated_primary_alter_posts (id, title) VALUES (1, 'draft')"
    );
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(
        filename,
        "app",
        "generated_primary_alter_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_exec_fails_with_message(
        db,
        "ALTER TABLE generated_primary_alter_posts ADD PRIMARY KEY (slug), ALGORITHM=COPY",
        "Primary key cannot be defined upon a generated column"
    );
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(
        filename,
        "app",
        "generated_primary_alter_posts",
        "InnoDB",
        "MYLITE"
    );
    assert(
        mylite_exec(
            db,
            "SELECT title FROM generated_primary_alter_posts WHERE slug = 'draft-slug'",
            single_value_callback,
            &title,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    title = (single_value_context){.expected_value = "draft"};
    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "app");
    assert_exec_succeeds(db, "USE app");
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(
        filename,
        "app",
        "generated_primary_alter_posts",
        "InnoDB",
        "MYLITE"
    );
    assert(
        mylite_exec(
            db,
            "SELECT title FROM generated_primary_alter_posts WHERE slug = 'draft-slug'",
            single_value_callback,
            &title,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_autoincrement_key_policy(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE auto_policy");
    assert_exec_succeeds(db, "USE auto_policy");
    assert_exec_succeeds(
        db,
        "CREATE TABLE single_auto_posts ("
        "id INT NOT NULL AUTO_INCREMENT,"
        "category INT NOT NULL,"
        "title VARCHAR(32) NOT NULL,"
        "PRIMARY KEY (id),"
        "KEY category_id (category, id)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "INSERT INTO single_auto_posts (category, title) VALUES (7, 'first')");
    assert_query_single_value(db, "SELECT id FROM single_auto_posts WHERE category = 7", "1");
    assert_catalog_table_count(filename, "auto_policy", 1U);
    assert_catalog_table_metadata(filename, "auto_policy", "single_auto_posts", "InnoDB", "MYLITE");

    assert_exec_fails(
        db,
        "CREATE TABLE compound_auto_first ("
        "id INT NOT NULL AUTO_INCREMENT,"
        "category INT NOT NULL,"
        "PRIMARY KEY (id, category)"
        ") ENGINE=InnoDB"
    );
    assert(
        mylite_storage_table_exists(filename, "auto_policy", "compound_auto_first") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "auto_policy", 1U);

    assert_exec_fails(
        db,
        "CREATE TABLE grouped_auto_posts ("
        "category INT NOT NULL,"
        "id INT NOT NULL AUTO_INCREMENT,"
        "title VARCHAR(32) NOT NULL,"
        "PRIMARY KEY (category, id)"
        ") ENGINE=MyISAM"
    );
    assert(
        mylite_storage_table_exists(filename, "auto_policy", "grouped_auto_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "auto_policy", 1U);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_indexed_rows(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context rows = {0};
    table_context news_rows = {0};
    table_context alter_rows = {0};
    table_context renamed_slug_rows = {0};
    table_context renamed_category_rows = {0};
    table_context reopened_rows = {0};
    table_context reopened_renamed_slug_rows = {0};
    const char *score_desc_ids[] = {"3", "2", "1"};
    id_sequence_context score_desc = {
        .expected_count = 3,
        .expected_ids = score_desc_ids,
    };
    const char *nullable_ids[] = {"1", "2"};
    id_sequence_context nullable_sequence = {
        .expected_count = 2,
        .expected_ids = nullable_ids,
    };
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE indexed_posts ("
        "id INT NOT NULL, "
        "slug VARCHAR(32) NOT NULL, "
        "category VARCHAR(32) NULL, "
        "score INT NOT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY category_key (category), "
        "KEY score_key (score)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE nullable_unique_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "code INT NULL UNIQUE"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE alter_index_posts (id INT NOT NULL, slug VARCHAR(32) NOT NULL) "
        "ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE rename_index_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "category VARCHAR(32) NOT NULL, "
        "UNIQUE KEY slug_key (slug), "
        "KEY category_key (category)"
        ") ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "app", 4U);

    assert_exec_succeeds(db, "INSERT INTO indexed_posts VALUES (1, 'alpha', 'news', 10)");
    assert_exec_succeeds(db, "INSERT INTO indexed_posts VALUES (2, 'beta', NULL, 20)");
    assert_exec_succeeds(db, "INSERT INTO indexed_posts VALUES (3, 'gamma', 'news', 30)");
    assert_exec_fails(db, "INSERT INTO indexed_posts VALUES (1, 'duplicate-id', 'news', 40)");
    assert_exec_fails(db, "INSERT INTO indexed_posts VALUES (4, 'beta', 'tech', 40)");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM indexed_posts FORCE INDEX (PRIMARY) WHERE id = 2 AND slug = 'beta'",
            row_callback,
            &rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM indexed_posts FORCE INDEX (category_key) WHERE category = 'news'",
            row_callback,
            &news_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(news_rows.rows == 2);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM indexed_posts FORCE INDEX (score_key) ORDER BY score DESC",
            id_sequence_callback,
            &score_desc,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(score_desc.rows == 3);

    assert_exec_succeeds(
        db,
        "UPDATE indexed_posts SET slug = 'beta-updated', category = 'tech', score = 25 "
        "WHERE slug = 'beta'"
    );
    rows = (table_context){0};
    assert(
        mylite_exec(
            db,
            "SELECT id FROM indexed_posts FORCE INDEX (slug_key) "
            "WHERE slug = 'beta-updated' AND category = 'tech' AND score = 25",
            row_callback,
            &rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(rows.rows == 1);
    assert_exec_succeeds(db, "DELETE FROM indexed_posts WHERE category = 'news' AND id = 1");
    news_rows = (table_context){0};
    assert(
        mylite_exec(
            db,
            "SELECT id FROM indexed_posts FORCE INDEX (category_key) WHERE category = 'news'",
            row_callback,
            &news_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(news_rows.rows == 1);

    assert_exec_succeeds(db, "INSERT INTO nullable_unique_posts VALUES (1, NULL)");
    assert_exec_succeeds(db, "INSERT INTO nullable_unique_posts VALUES (2, NULL)");
    assert_exec_succeeds(db, "INSERT INTO nullable_unique_posts VALUES (3, 7)");
    assert_exec_fails(db, "INSERT INTO nullable_unique_posts VALUES (4, 7)");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM nullable_unique_posts WHERE code IS NULL ORDER BY id",
            id_sequence_callback,
            &nullable_sequence,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(nullable_sequence.rows == 2);

    assert_exec_succeeds(db, "INSERT INTO alter_index_posts VALUES (1, 'first')");
    assert_exec_succeeds(db, "INSERT INTO alter_index_posts VALUES (2, 'second')");
    assert_exec_succeeds(db, "INSERT INTO rename_index_posts VALUES (1, 'alpha', 'news')");
    assert_exec_succeeds(db, "INSERT INTO rename_index_posts VALUES (2, 'beta', 'tech')");
    assert_exec_succeeds(
        db,
        "ALTER TABLE alter_index_posts ADD PRIMARY KEY (id), "
        "ADD UNIQUE KEY slug_key (slug), ALGORITHM=COPY"
    );
    assert_exec_fails(db, "INSERT INTO alter_index_posts VALUES (2, 'duplicate-id')");
    assert_exec_fails(db, "INSERT INTO alter_index_posts VALUES (3, 'second')");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM alter_index_posts FORCE INDEX (PRIMARY) "
            "WHERE id = 2 AND slug = 'second'",
            row_callback,
            &alter_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(alter_rows.rows == 1);
    assert_exec_succeeds(db, "RENAME TABLE rename_index_posts TO renamed_index_posts");
    assert(
        mylite_storage_table_exists(filename, "app", "rename_index_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_table_exists(filename, "app", "renamed_index_posts") == MYLITE_STORAGE_OK
    );
    assert_exec_fails(db, "SELECT id FROM rename_index_posts");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM renamed_index_posts FORCE INDEX (slug_key) WHERE slug = 'beta'",
            row_callback,
            &renamed_slug_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(renamed_slug_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM renamed_index_posts FORCE INDEX (category_key) WHERE category = 'news'",
            row_callback,
            &renamed_category_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(renamed_category_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO renamed_index_posts VALUES (3, 'beta', 'duplicate')");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE app");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM indexed_posts FORCE INDEX (slug_key) "
            "WHERE slug = 'beta-updated'",
            row_callback,
            &reopened_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM renamed_index_posts FORCE INDEX (slug_key) WHERE slug = 'beta'",
            row_callback,
            &reopened_renamed_slug_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_renamed_slug_rows.rows == 1);
    assert_exec_fails(db, "SELECT id FROM rename_index_posts");
    assert_catalog_table_count(filename, "app", 4U);
    assert_catalog_table_metadata(filename, "app", "indexed_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "nullable_unique_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "alter_index_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "renamed_index_posts", "InnoDB", "MYLITE");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_standalone_index_ddl(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context slug_rows = {0};
    table_context renamed_slug_rows = {0};
    table_context old_renamed_index_rows = {0};
    table_context new_renamed_index_rows = {0};
    table_context dropped_index_rows = {0};
    table_context reopened_slug_rows = {0};
    table_context reopened_old_renamed_index_rows = {0};
    table_context reopened_new_renamed_index_rows = {0};
    table_context reopened_dropped_index_rows = {0};
    table_context generated_slug_rows = {0};
    table_context generated_label_rows = {0};
    table_context generated_old_renamed_index_rows = {0};
    table_context generated_new_renamed_index_rows = {0};
    table_context generated_dropped_index_rows = {0};
    table_context generated_post_drop_rows = {0};
    table_context generated_blob_body_rows = {0};
    table_context generated_blob_payload_rows = {0};
    table_context generated_blob_dropped_index_rows = {0};
    table_context generated_blob_post_drop_rows = {0};
    table_context reopened_generated_slug_rows = {0};
    table_context reopened_generated_old_renamed_index_rows = {0};
    table_context reopened_generated_new_renamed_index_rows = {0};
    table_context reopened_generated_dropped_index_rows = {0};
    table_context reopened_generated_label_rows = {0};
    table_context reopened_generated_label_dropped_rows = {0};
    table_context reopened_generated_blob_payload_rows = {0};
    table_context reopened_generated_blob_unique_rows = {0};
    table_context reopened_generated_blob_dropped_index_rows = {0};
    const char *category_ids[] = {"1", "3"};
    id_sequence_context category_sequence = {
        .expected_count = 2,
        .expected_ids = category_ids,
    };
    id_sequence_context post_drop_sequence = {
        .expected_count = 2,
        .expected_ids = category_ids,
    };
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE standalone_index_posts ("
        "id INT NOT NULL, "
        "slug VARCHAR(32) NOT NULL, "
        "category VARCHAR(32) NULL, "
        "score INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE generated_index_ddl_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(32) NOT NULL, "
        "slug VARCHAR(48) AS (CONCAT(title, '-slug')) VIRTUAL, "
        "label VARCHAR(48) AS (CONCAT(title, '-', id)) STORED"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE generated_blob_index_ddl_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL, "
        "generated_body TEXT AS (CONCAT(title, '-body')) STORED, "
        "generated_label TEXT AS (CONCAT(title, '-label')) VIRTUAL, "
        "generated_payload BLOB AS (UNHEX(HEX(CONCAT(title, '-bin')))) STORED"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(db, "INSERT INTO standalone_index_posts VALUES (1, 'alpha', 'news', 10)");
    assert_exec_succeeds(db, "INSERT INTO standalone_index_posts VALUES (2, 'beta', 'tech', 20)");
    assert_exec_succeeds(db, "INSERT INTO standalone_index_posts VALUES (3, 'gamma', 'news', 30)");
    assert_exec_succeeds(
        db,
        "INSERT INTO generated_index_ddl_posts (id, title) VALUES (1, 'alpha')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO generated_index_ddl_posts (id, title) VALUES (2, 'beta')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO generated_index_ddl_posts (id, title) VALUES (3, 'gamma')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO generated_blob_index_ddl_posts (id, title) VALUES "
        "(1, 'alpha'), (2, 'beta'), (3, 'gamma')"
    );

    assert_exec_succeeds(
        db,
        "CREATE INDEX category_lookup ON standalone_index_posts (category) ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "CREATE UNIQUE INDEX slug_lookup ON standalone_index_posts (slug) ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "ALTER TABLE generated_index_ddl_posts "
        "ADD UNIQUE KEY generated_slug_key (slug), ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "CREATE INDEX generated_label_key ON generated_index_ddl_posts (label) ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "CREATE INDEX generated_body_prefix_ddl "
        "ON generated_blob_index_ddl_posts (generated_body(12)) ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "CREATE UNIQUE INDEX generated_label_prefix_ddl "
        "ON generated_blob_index_ddl_posts (generated_label(10)) ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "CREATE INDEX generated_payload_prefix_ddl "
        "ON generated_blob_index_ddl_posts (generated_payload(6)) ALGORITHM=COPY"
    );
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "standalone_index_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "generated_index_ddl_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "app",
        "generated_blob_index_ddl_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_exec_fails(db, "INSERT INTO standalone_index_posts VALUES (4, 'beta', 'docs', 40)");
    assert_exec_fails(db, "INSERT INTO generated_index_ddl_posts (id, title) VALUES (4, 'alpha')");
    assert_exec_fails(
        db,
        "INSERT INTO generated_blob_index_ddl_posts (id, title) VALUES (4, 'alpha')"
    );

    assert(
        mylite_exec(
            db,
            "SELECT id FROM standalone_index_posts FORCE INDEX (slug_lookup) WHERE slug = 'beta'",
            row_callback,
            &slug_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(slug_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM standalone_index_posts FORCE INDEX (category_lookup) "
            "WHERE category = 'news' ORDER BY id",
            id_sequence_callback,
            &category_sequence,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(category_sequence.rows == 2);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_index_ddl_posts FORCE INDEX (generated_slug_key) "
            "WHERE slug = 'beta-slug'",
            row_callback,
            &generated_slug_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_slug_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_index_ddl_posts FORCE INDEX (generated_label_key) "
            "WHERE label = 'gamma-3'",
            row_callback,
            &generated_label_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_label_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_index_ddl_posts "
            "FORCE INDEX (generated_body_prefix_ddl) WHERE generated_body = 'alpha-body'",
            row_callback,
            &generated_blob_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_blob_body_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_index_ddl_posts "
            "FORCE INDEX (generated_payload_prefix_ddl) "
            "WHERE generated_payload = UNHEX('626574612D62696E')",
            row_callback,
            &generated_blob_payload_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_blob_payload_rows.rows == 1);

    assert_exec_succeeds(
        db,
        "ALTER TABLE standalone_index_posts "
        "RENAME INDEX slug_lookup TO renamed_slug_lookup, ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM standalone_index_posts WHERE Key_name = 'slug_lookup'",
            table_callback,
            &old_renamed_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(old_renamed_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM standalone_index_posts WHERE Key_name = 'renamed_slug_lookup'",
            table_callback,
            &new_renamed_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(new_renamed_index_rows.rows == 1);
    assert_exec_fails(
        db,
        "SELECT id FROM standalone_index_posts FORCE INDEX (slug_lookup) WHERE slug = 'beta'"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM standalone_index_posts FORCE INDEX (renamed_slug_lookup) "
            "WHERE slug = 'beta'",
            row_callback,
            &renamed_slug_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(renamed_slug_rows.rows == 1);
    assert_exec_succeeds(
        db,
        "ALTER TABLE generated_index_ddl_posts "
        "RENAME INDEX generated_slug_key TO renamed_generated_slug_key, ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_index_ddl_posts WHERE Key_name = 'generated_slug_key'",
            table_callback,
            &generated_old_renamed_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_old_renamed_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_index_ddl_posts WHERE Key_name = "
            "'renamed_generated_slug_key'",
            table_callback,
            &generated_new_renamed_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_new_renamed_index_rows.rows == 1);
    assert_exec_fails(
        db,
        "SELECT id FROM generated_index_ddl_posts FORCE INDEX (generated_slug_key) "
        "WHERE slug = 'beta-slug'"
    );
    generated_slug_rows = (table_context){0};
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_index_ddl_posts FORCE INDEX (renamed_generated_slug_key) "
            "WHERE slug = 'beta-slug'",
            row_callback,
            &generated_slug_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_slug_rows.rows == 1);
    assert_exec_fails(db, "INSERT INTO generated_index_ddl_posts (id, title) VALUES (4, 'beta')");
    assert_exec_fails(db, "INSERT INTO standalone_index_posts VALUES (4, 'beta', 'docs', 40)");
    assert_exec_fails(
        db,
        "INSERT INTO generated_blob_index_ddl_posts (id, title) VALUES (4, 'beta')"
    );
    assert_catalog_table_count(filename, "app", 3U);

    assert_exec_succeeds(db, "DROP INDEX category_lookup ON standalone_index_posts");
    assert_exec_succeeds(db, "DROP INDEX generated_label_key ON generated_index_ddl_posts");
    assert_exec_succeeds(
        db,
        "DROP INDEX generated_body_prefix_ddl ON generated_blob_index_ddl_posts"
    );
    assert_catalog_table_count(filename, "app", 3U);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM standalone_index_posts WHERE Key_name = 'category_lookup'",
            table_callback,
            &dropped_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(dropped_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_index_ddl_posts WHERE Key_name = 'generated_label_key'",
            table_callback,
            &generated_dropped_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_dropped_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_blob_index_ddl_posts "
            "WHERE Key_name = 'generated_body_prefix_ddl'",
            table_callback,
            &generated_blob_dropped_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_blob_dropped_index_rows.rows == 0);
    assert_exec_fails(
        db,
        "SELECT id FROM standalone_index_posts FORCE INDEX (category_lookup) "
        "WHERE category = 'news'"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM standalone_index_posts WHERE category = 'news' ORDER BY id",
            id_sequence_callback,
            &post_drop_sequence,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(post_drop_sequence.rows == 2);
    assert_exec_fails(
        db,
        "SELECT id FROM generated_index_ddl_posts FORCE INDEX (generated_label_key) "
        "WHERE label = 'gamma-3'"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_index_ddl_posts WHERE label = 'gamma-3'",
            row_callback,
            &generated_post_drop_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_post_drop_rows.rows == 1);
    assert_exec_fails(
        db,
        "SELECT id FROM generated_blob_index_ddl_posts "
        "FORCE INDEX (generated_body_prefix_ddl) WHERE generated_body = 'alpha-body'"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_index_ddl_posts WHERE generated_body = 'alpha-body'",
            row_callback,
            &generated_blob_post_drop_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_blob_post_drop_rows.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE app");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM standalone_index_posts FORCE INDEX (renamed_slug_lookup) "
            "WHERE slug = 'gamma'",
            row_callback,
            &reopened_slug_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_slug_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_index_ddl_posts FORCE INDEX (renamed_generated_slug_key) "
            "WHERE slug = 'gamma-slug'",
            row_callback,
            &reopened_generated_slug_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_slug_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM standalone_index_posts WHERE Key_name = 'slug_lookup'",
            table_callback,
            &reopened_old_renamed_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_old_renamed_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM standalone_index_posts WHERE Key_name = 'renamed_slug_lookup'",
            table_callback,
            &reopened_new_renamed_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_new_renamed_index_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_index_ddl_posts WHERE Key_name = 'generated_slug_key'",
            table_callback,
            &reopened_generated_old_renamed_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_old_renamed_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_index_ddl_posts WHERE Key_name = "
            "'renamed_generated_slug_key'",
            table_callback,
            &reopened_generated_new_renamed_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_new_renamed_index_rows.rows == 1);
    assert_exec_fails(
        db,
        "SELECT id FROM standalone_index_posts FORCE INDEX (slug_lookup) WHERE slug = 'gamma'"
    );
    assert_exec_fails(
        db,
        "SELECT id FROM generated_index_ddl_posts FORCE INDEX (generated_slug_key) "
        "WHERE slug = 'gamma-slug'"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_index_ddl_posts "
            "FORCE INDEX (generated_payload_prefix_ddl) "
            "WHERE generated_payload = UNHEX('67616D6D612D62696E')",
            row_callback,
            &reopened_generated_blob_payload_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_blob_payload_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_index_ddl_posts "
            "FORCE INDEX (generated_label_prefix_ddl) WHERE generated_label = 'beta-label'",
            row_callback,
            &reopened_generated_blob_unique_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_blob_unique_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM standalone_index_posts WHERE Key_name = 'category_lookup'",
            table_callback,
            &reopened_dropped_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_dropped_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_index_ddl_posts WHERE Key_name = 'generated_label_key'",
            table_callback,
            &reopened_generated_dropped_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_dropped_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_blob_index_ddl_posts "
            "WHERE Key_name = 'generated_body_prefix_ddl'",
            table_callback,
            &reopened_generated_blob_dropped_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_blob_dropped_index_rows.rows == 0);
    assert_exec_succeeds(
        db,
        "CREATE INDEX reopened_generated_label_key ON generated_index_ddl_posts (label)"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_index_ddl_posts FORCE INDEX (reopened_generated_label_key) "
            "WHERE label = 'alpha-1'",
            row_callback,
            &reopened_generated_label_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_label_rows.rows == 1);
    assert_exec_succeeds(
        db,
        "DROP INDEX reopened_generated_label_key ON generated_index_ddl_posts"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_index_ddl_posts "
            "WHERE Key_name = 'reopened_generated_label_key'",
            table_callback,
            &reopened_generated_label_dropped_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_label_dropped_rows.rows == 0);
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "standalone_index_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "generated_index_ddl_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "app",
        "generated_blob_index_ddl_posts",
        "InnoDB",
        "MYLITE"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_index_ddl_if_exists(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE index_if_exists");
    assert_exec_succeeds(db, "USE index_if_exists");
    assert_exec_succeeds(
        db,
        "CREATE TABLE index_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "category VARCHAR(32) NOT NULL, "
        "status VARCHAR(16) NOT NULL, "
        "body LONGTEXT NULL, "
        "UNIQUE KEY slug_key (slug), "
        "KEY category_key (category), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO index_posts VALUES "
        "(1, 'alpha', 'news', 'open', 'alpha body'), "
        "(2, 'beta', 'tech', 'closed', 'beta body'), "
        "(3, 'gamma', 'news', 'open', 'gamma body')"
    );
    assert_catalog_table_count(filename, "index_if_exists", 1U);
    assert_catalog_table_metadata(filename, "index_if_exists", "index_posts", "InnoDB", "MYLITE");

    assert_exec_succeeds(
        db,
        "CREATE INDEX IF NOT EXISTS category_key "
        "ON index_posts (category) ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "category_key");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (category_key) WHERE category = 'news'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE index_posts "
        "ADD INDEX IF NOT EXISTS category_key (category), ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "category_key");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (category_key) WHERE category = 'news'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "CREATE INDEX IF NOT EXISTS status_key ON index_posts (status) ALGORITHM=COPY"
    );
    assert_catalog_table_count(filename, "index_if_exists", 1U);
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (status_key) WHERE status = 'open'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE index_posts "
        "RENAME INDEX IF EXISTS missing_status_key TO renamed_status_key, "
        "ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_status_key");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (status_key) WHERE status = 'open'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE index_posts "
        "RENAME INDEX IF EXISTS status_key TO renamed_status_key, ALGORITHM=COPY"
    );
    assert_exec_fails(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (status_key) WHERE status = 'open'"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (renamed_status_key) WHERE status = 'open'",
        "2"
    );

    assert_exec_succeeds(db, "DROP INDEX IF EXISTS missing_key ON index_posts");
    assert_warning_message_contains(db, "missing_key");
    assert_query_single_value(
        db,
        "SELECT id FROM index_posts FORCE INDEX (slug_key) WHERE slug = 'beta'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (renamed_status_key) WHERE status = 'open'",
        "2"
    );

    assert_exec_succeeds(db, "DROP INDEX IF EXISTS renamed_status_key ON index_posts");
    assert_catalog_table_count(filename, "index_if_exists", 1U);
    assert_exec_fails(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (renamed_status_key) WHERE status = 'open'"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM index_posts", "3");
    assert_query_single_value(
        db,
        "SELECT id FROM index_posts FORCE INDEX (body_prefix) WHERE body = 'gamma body'",
        "3"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE index_posts "
        "ADD INDEX IF NOT EXISTS alter_status_key (status), ALGORITHM=COPY"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (alter_status_key) WHERE status = 'open'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE index_posts DROP INDEX IF EXISTS missing_alter_key, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_alter_key");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (alter_status_key) WHERE status = 'open'",
        "2"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "index_if_exists");
    assert_exec_succeeds(db, "USE index_if_exists");
    assert_catalog_table_count(filename, "index_if_exists", 1U);
    assert_catalog_table_metadata(filename, "index_if_exists", "index_posts", "InnoDB", "MYLITE");
    assert_query_single_value(db, "SELECT COUNT(*) FROM index_posts", "3");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (category_key) WHERE category = 'news'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM index_posts FORCE INDEX (slug_key) WHERE slug = 'alpha'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (alter_status_key) WHERE status = 'open'",
        "2"
    );
    assert_exec_succeeds(
        db,
        "ALTER TABLE index_posts DROP INDEX IF EXISTS alter_status_key, ALGORITHM=COPY"
    );
    assert_exec_fails(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (alter_status_key) WHERE status = 'open'"
    );
    assert_exec_fails(
        db,
        "SELECT COUNT(*) FROM index_posts FORCE INDEX (renamed_status_key) WHERE status = 'open'"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_index_ignorability(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE index_ignorability");
    assert_exec_succeeds(db, "USE index_ignorability");
    assert_exec_succeeds(
        db,
        "CREATE TABLE ignored_index_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "status VARCHAR(16) NOT NULL, "
        "category VARCHAR(16) NOT NULL, "
        "KEY status_key (status) IGNORED, "
        "KEY category_key (category) NOT IGNORED"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO ignored_index_posts VALUES "
        "(1, 'alpha', 'open', 'news'), "
        "(2, 'beta', 'closed', 'docs'), "
        "(3, 'gamma', 'open', 'news')"
    );
    assert_catalog_table_count(filename, "index_ignorability", 1U);
    assert_catalog_table_metadata(
        filename,
        "index_ignorability",
        "ignored_index_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_index_ignored(db, "index_ignorability", "ignored_index_posts", "status_key", "YES");
    assert_index_ignored(db, "index_ignorability", "ignored_index_posts", "category_key", "NO");
    assert_exec_fails_with_message(
        db,
        "SELECT COUNT(*) FROM ignored_index_posts "
        "FORCE INDEX (status_key) WHERE status = 'open'",
        "status_key"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM ignored_index_posts "
        "FORCE INDEX (category_key) WHERE category = 'news'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "CREATE INDEX created_ignored_key ON ignored_index_posts (slug) IGNORED "
        "ALGORITHM=COPY"
    );
    assert_index_ignored(
        db,
        "index_ignorability",
        "ignored_index_posts",
        "created_ignored_key",
        "YES"
    );
    assert_exec_fails_with_message(
        db,
        "SELECT id FROM ignored_index_posts "
        "FORCE INDEX (created_ignored_key) WHERE slug = 'beta'",
        "created_ignored_key"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE ignored_index_posts "
        "ADD INDEX alter_category_key (category) NOT IGNORED, ALGORITHM=COPY"
    );
    assert_index_ignored(
        db,
        "index_ignorability",
        "ignored_index_posts",
        "alter_category_key",
        "NO"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM ignored_index_posts "
        "FORCE INDEX (alter_category_key) WHERE category = 'news'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE ignored_index_posts "
        "ALTER INDEX IF EXISTS missing_ignored_key IGNORED, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_ignored_key");
    assert_index_ignored(db, "index_ignorability", "ignored_index_posts", "category_key", "NO");

    assert_exec_succeeds(
        db,
        "ALTER TABLE ignored_index_posts "
        "ALTER INDEX IF EXISTS category_key IGNORED, ALGORITHM=COPY"
    );
    assert_index_ignored(db, "index_ignorability", "ignored_index_posts", "category_key", "YES");
    assert_exec_fails_with_message(
        db,
        "SELECT COUNT(*) FROM ignored_index_posts "
        "FORCE INDEX (category_key) WHERE category = 'news'",
        "category_key"
    );

    assert_exec_succeeds(
        db,
        "ALTER TABLE ignored_index_posts "
        "ALTER INDEX IF EXISTS missing_ignored_key NOT IGNORED, ALGORITHM=COPY"
    );
    assert_warning_message_contains(db, "missing_ignored_key");
    assert_index_ignored(db, "index_ignorability", "ignored_index_posts", "category_key", "YES");

    assert_exec_succeeds(
        db,
        "ALTER TABLE ignored_index_posts "
        "ALTER INDEX IF EXISTS category_key NOT IGNORED, ALGORITHM=COPY"
    );
    assert_index_ignored(db, "index_ignorability", "ignored_index_posts", "category_key", "NO");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM ignored_index_posts "
        "FORCE INDEX (category_key) WHERE category = 'news'",
        "2"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE index_ignorability");
    assert_index_ignored(db, "index_ignorability", "ignored_index_posts", "status_key", "YES");
    assert_index_ignored(
        db,
        "index_ignorability",
        "ignored_index_posts",
        "created_ignored_key",
        "YES"
    );
    assert_index_ignored(db, "index_ignorability", "ignored_index_posts", "category_key", "NO");
    assert_index_ignored(
        db,
        "index_ignorability",
        "ignored_index_posts",
        "alter_category_key",
        "NO"
    );
    assert_exec_fails_with_message(
        db,
        "SELECT COUNT(*) FROM ignored_index_posts "
        "FORCE INDEX (status_key) WHERE status = 'open'",
        "status_key"
    );
    assert_exec_fails_with_message(
        db,
        "SELECT id FROM ignored_index_posts "
        "FORCE INDEX (created_ignored_key) WHERE slug = 'beta'",
        "created_ignored_key"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM ignored_index_posts "
        "FORCE INDEX (category_key) WHERE category = 'news'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM ignored_index_posts "
        "FORCE INDEX (alter_category_key) WHERE category = 'news'",
        "2"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_blob_text_prefix_indexes(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context body_rows = {0};
    table_context old_body_rows = {0};
    table_context updated_body_rows = {0};
    table_context payload_rows = {0};
    table_context deleted_payload_rows = {0};
    table_context standalone_body_rows = {0};
    table_context standalone_unbounded_index_rows = {0};
    table_context standalone_unbounded_body_rows = {0};
    table_context generated_body_rows = {0};
    table_context generated_payload_rows = {0};
    table_context generated_old_label_rows = {0};
    table_context generated_updated_label_rows = {0};
    table_context generated_reused_label_rows = {0};
    table_context generated_deleted_body_rows = {0};
    table_context generated_unbounded_index_rows = {0};
    table_context generated_unbounded_payload_rows = {0};
    table_context reopened_body_rows = {0};
    table_context reopened_payload_rows = {0};
    table_context reopened_standalone_body_rows = {0};
    table_context reopened_generated_body_rows = {0};
    table_context reopened_generated_payload_rows = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE blob_prefix_posts ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "body TEXT NULL, "
        "payload BLOB NULL, "
        "PRIMARY KEY (id), "
        "KEY body_prefix (body(8)), "
        "KEY payload_prefix (payload(3)), "
        "UNIQUE KEY unique_body_prefix (body(12))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE standalone_blob_prefix_posts ("
        "id INT NOT NULL, "
        "body LONGTEXT NULL, "
        "payload LONGBLOB NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE generated_blob_prefix_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL, "
        "generated_body TEXT AS (CONCAT(title, '-body')) STORED, "
        "generated_label TEXT AS (CONCAT(title, '-label')) VIRTUAL, "
        "generated_payload BLOB AS (UNHEX(HEX(CONCAT(title, '-bin')))) STORED, "
        "KEY generated_body_prefix (generated_body(12)), "
        "UNIQUE KEY generated_label_prefix (generated_label(10)), "
        "KEY generated_payload_prefix (generated_payload(6))"
        ") ENGINE=InnoDB"
    );
    assert_exec_fails(
        db,
        "CREATE TABLE full_blob_unique_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "body TEXT NOT NULL, "
        "UNIQUE KEY body_unique (body)"
        ") ENGINE=InnoDB"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "full_blob_unique_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_exec_fails(
        db,
        "CREATE TABLE generated_full_blob_unique_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL, "
        "generated_body TEXT AS (CONCAT(title, '-body')) STORED, "
        "UNIQUE KEY generated_body_unique (generated_body)"
        ") ENGINE=InnoDB"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "generated_full_blob_unique_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "app", 3U);

    assert_exec_succeeds(
        db,
        "INSERT INTO blob_prefix_posts (body, payload) VALUES "
        "('alpha body one', UNHEX('010203AA')), "
        "('beta body two', UNHEX('010204BB')), "
        "('alphabet soup', UNHEX('FF0001'))"
    );
    assert_exec_fails(
        db,
        "INSERT INTO blob_prefix_posts (body, payload) VALUES "
        "('alpha body other', UNHEX('ABCDEF'))"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM blob_prefix_posts FORCE INDEX (body_prefix) "
            "WHERE body = 'alpha body one'",
            row_callback,
            &body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(body_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM blob_prefix_posts FORCE INDEX (payload_prefix) "
            "WHERE payload = UNHEX('010203AA')",
            row_callback,
            &payload_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(payload_rows.rows == 1);

    assert_exec_succeeds(
        db,
        "UPDATE blob_prefix_posts SET body = 'gamma body one', payload = UNHEX('0A0B0C') "
        "WHERE id = 1"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM blob_prefix_posts FORCE INDEX (body_prefix) "
            "WHERE body = 'alpha body one'",
            row_callback,
            &old_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(old_body_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM blob_prefix_posts FORCE INDEX (body_prefix) "
            "WHERE body = 'gamma body one'",
            row_callback,
            &updated_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(updated_body_rows.rows == 1);

    assert_exec_succeeds(db, "DELETE FROM blob_prefix_posts WHERE id = 2");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM blob_prefix_posts FORCE INDEX (payload_prefix) "
            "WHERE payload = UNHEX('010204BB')",
            row_callback,
            &deleted_payload_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(deleted_payload_rows.rows == 0);

    assert_exec_succeeds(
        db,
        "INSERT INTO generated_blob_prefix_posts (id, title) VALUES "
        "(1, 'alpha'), (2, 'beta')"
    );
    assert_exec_fails(
        db,
        "INSERT INTO generated_blob_prefix_posts (id, title) VALUES (3, 'alpha')"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_prefix_posts FORCE INDEX (generated_body_prefix) "
            "WHERE generated_body = 'alpha-body'",
            row_callback,
            &generated_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_body_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_prefix_posts FORCE INDEX (generated_payload_prefix) "
            "WHERE generated_payload = UNHEX('616C7068612D62696E')",
            row_callback,
            &generated_payload_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_payload_rows.rows == 1);
    assert_exec_succeeds(db, "UPDATE generated_blob_prefix_posts SET title = 'gamma' WHERE id = 1");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_prefix_posts FORCE INDEX (generated_label_prefix) "
            "WHERE generated_label = 'alpha-label'",
            row_callback,
            &generated_old_label_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_old_label_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_prefix_posts FORCE INDEX (generated_label_prefix) "
            "WHERE generated_label = 'gamma-label'",
            row_callback,
            &generated_updated_label_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_updated_label_rows.rows == 1);
    assert_exec_succeeds(
        db,
        "INSERT INTO generated_blob_prefix_posts (id, title) VALUES (3, 'alpha')"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_prefix_posts FORCE INDEX (generated_label_prefix) "
            "WHERE generated_label = 'alpha-label'",
            row_callback,
            &generated_reused_label_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_reused_label_rows.rows == 1);
    assert_exec_succeeds(db, "DELETE FROM generated_blob_prefix_posts WHERE id = 2");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_prefix_posts FORCE INDEX (generated_body_prefix) "
            "WHERE generated_body = 'beta-body'",
            row_callback,
            &generated_deleted_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_deleted_body_rows.rows == 0);

    assert_exec_succeeds(
        db,
        "INSERT INTO standalone_blob_prefix_posts VALUES "
        "(1, 'standalone alpha body', UNHEX('AABBCC01')), "
        "(2, 'standalone beta body', UNHEX('DDEEFF02'))"
    );
    assert_exec_succeeds(
        db,
        "CREATE INDEX standalone_body_prefix "
        "ON standalone_blob_prefix_posts (body(10)) ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "CREATE UNIQUE INDEX standalone_payload_prefix "
        "ON standalone_blob_prefix_posts (payload(3)) ALGORITHM=COPY"
    );
    assert_exec_fails(
        db,
        "INSERT INTO standalone_blob_prefix_posts VALUES "
        "(3, 'standalone gamma body', UNHEX('AABBCCFF'))"
    );
    assert_exec_fails(
        db,
        "CREATE UNIQUE INDEX standalone_body_unique "
        "ON standalone_blob_prefix_posts (body) ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM standalone_blob_prefix_posts "
            "WHERE Key_name = 'standalone_body_unique'",
            table_callback,
            &standalone_unbounded_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(standalone_unbounded_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM standalone_blob_prefix_posts WHERE body = 'standalone alpha body'",
            row_callback,
            &standalone_unbounded_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(standalone_unbounded_body_rows.rows == 1);
    assert_exec_fails(
        db,
        "ALTER TABLE generated_blob_prefix_posts "
        "ADD UNIQUE KEY generated_payload_unique (generated_payload), ALGORITHM=COPY"
    );
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM generated_blob_prefix_posts "
            "WHERE Key_name = 'generated_payload_unique'",
            table_callback,
            &generated_unbounded_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_unbounded_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_prefix_posts "
            "WHERE generated_payload = UNHEX('616C7068612D62696E')",
            row_callback,
            &generated_unbounded_payload_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_unbounded_payload_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM standalone_blob_prefix_posts FORCE INDEX (standalone_body_prefix) "
            "WHERE body = 'standalone alpha body'",
            row_callback,
            &standalone_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(standalone_body_rows.rows == 1);
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "blob_prefix_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "app",
        "standalone_blob_prefix_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "app",
        "generated_blob_prefix_posts",
        "InnoDB",
        "MYLITE"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "app");
    assert_exec_succeeds(db, "USE app");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM blob_prefix_posts FORCE INDEX (body_prefix) "
            "WHERE body = 'gamma body one'",
            row_callback,
            &reopened_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_body_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM blob_prefix_posts FORCE INDEX (payload_prefix) "
            "WHERE payload = UNHEX('0A0B0C')",
            row_callback,
            &reopened_payload_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_payload_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM standalone_blob_prefix_posts FORCE INDEX (standalone_body_prefix) "
            "WHERE body = 'standalone beta body'",
            row_callback,
            &reopened_standalone_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_standalone_body_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_prefix_posts FORCE INDEX (generated_body_prefix) "
            "WHERE generated_body = 'gamma-body'",
            row_callback,
            &reopened_generated_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_body_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM generated_blob_prefix_posts FORCE INDEX (generated_payload_prefix) "
            "WHERE generated_payload = UNHEX('616C7068612D62696E')",
            row_callback,
            &reopened_generated_payload_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_generated_payload_rows.rows == 1);
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "blob_prefix_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "app",
        "standalone_blob_prefix_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "app",
        "generated_blob_prefix_posts",
        "InnoDB",
        "MYLITE"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_create_table_like(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    single_value_context empty_clone_count = {
        .expected_value = "0",
    };
    single_value_context clone_first_id = {
        .expected_value = "1",
    };
    single_value_context source_count = {
        .expected_value = "2",
    };
    single_value_context clone_count = {
        .expected_value = "1",
    };
    table_context slug_index_rows = {0};
    table_context body_index_rows = {0};
    table_context payload_rows = {0};
    table_context reopened_body_rows = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE like_source_posts ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "payload BLOB NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8)), "
        "KEY payload_prefix (payload(2))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO like_source_posts (slug, body, payload) VALUES "
        "('source-alpha', 'source body one', UNHEX('010203')), "
        "('source-beta', 'source body two', UNHEX('040506'))"
    );
    assert_exec_succeeds(db, "CREATE TABLE like_clone_posts LIKE like_source_posts");
    assert_catalog_table_count(filename, "app", 2U);
    assert_catalog_table_metadata(filename, "app", "like_source_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "like_clone_posts", "InnoDB", "MYLITE");

    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM like_clone_posts",
            single_value_callback,
            &empty_clone_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(empty_clone_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM like_clone_posts WHERE Key_name = 'body_prefix'",
            row_callback,
            &body_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(body_index_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM like_clone_posts WHERE Key_name = 'slug_key'",
            row_callback,
            &slug_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(slug_index_rows.rows == 1);

    assert_exec_succeeds(
        db,
        "INSERT INTO like_clone_posts (slug, body, payload) VALUES "
        "('clone-alpha', 'clone body one', UNHEX('BEEF01'))"
    );
    assert_exec_fails(
        db,
        "INSERT INTO like_clone_posts (slug, body, payload) VALUES "
        "('clone-alpha', 'duplicate slug', UNHEX('CAFE01'))"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM like_clone_posts FORCE INDEX (slug_key) "
            "WHERE slug = 'clone-alpha'",
            single_value_callback,
            &clone_first_id,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(clone_first_id.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM like_clone_posts FORCE INDEX (payload_prefix) "
            "WHERE payload = UNHEX('BEEF01')",
            row_callback,
            &payload_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(payload_rows.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE app");
    assert_catalog_table_count(filename, "app", 2U);
    assert_catalog_table_metadata(filename, "app", "like_source_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "like_clone_posts", "InnoDB", "MYLITE");
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM like_source_posts",
            single_value_callback,
            &source_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(source_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM like_clone_posts",
            single_value_callback,
            &clone_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(clone_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM like_clone_posts FORCE INDEX (body_prefix) "
            "WHERE body = 'clone body one'",
            row_callback,
            &reopened_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_body_rows.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_create_table_select(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    single_value_context default_label = {
        .expected_value = "constant row",
    };
    single_value_context copied_count = {
        .expected_value = "2",
    };
    single_value_context copied_payload = {
        .expected_value = "010203",
    };
    single_value_context next_id = {
        .expected_value = "3",
    };
    single_value_context reopened_count = {
        .expected_value = "3",
    };
    single_value_context generated_title_len = {
        .expected_value = "5",
    };
    single_value_context generated_label = {
        .expected_value = "draft-1",
    };
    single_value_context target_title_len = {
        .expected_value = "6",
    };
    single_value_context target_label = {
        .expected_value = "target-2",
    };
    single_value_context checked_rating = {
        .expected_value = "4",
    };
    table_context failed_check_tables = {0};
    table_context failed_tables = {0};
    table_context body_rows = {0};
    table_context reopened_body_rows = {0};
    table_context reopened_checked_rows = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_source_posts ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "payload BLOB NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO ctas_source_posts (slug, body, payload) VALUES "
        "('source-alpha', 'source body one', UNHEX('010203')), "
        "('source-beta', 'source body two', UNHEX('040506'))"
    );
    assert_exec_fails(
        db,
        "CREATE TABLE ctas_failed_posts ("
        "id INT NOT NULL, "
        "slug VARCHAR(32) NOT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug)"
        ") ENGINE=InnoDB "
        "SELECT 1 AS id, 'duplicate' AS slug "
        "UNION ALL SELECT 2 AS id, 'duplicate' AS slug"
    );
    assert(
        mylite_exec(
            db,
            "SHOW TABLES LIKE 'ctas_failed_posts'",
            table_callback,
            &failed_tables,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(failed_tables.rows == 0);
    assert_catalog_table_count(filename, "app", 1U);
    assert_exec_fails(
        db,
        "CREATE TABLE ctas_failed_check_posts ("
        "id INT NOT NULL, "
        "rating INT NOT NULL CHECK (rating >= 0), "
        "CONSTRAINT rating_limit CHECK (rating <= 5)"
        ") ENGINE=InnoDB "
        "SELECT 1 AS id, 6 AS rating"
    );
    assert(
        mylite_exec(
            db,
            "SHOW TABLES LIKE 'ctas_failed_check_posts'",
            table_callback,
            &failed_check_tables,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(failed_check_tables.rows == 0);
    assert_catalog_table_count(filename, "app", 1U);

    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_default_constants AS "
        "SELECT 10 AS id, 'constant row' AS label"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_indexed_posts ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "payload BLOB NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB "
        "SELECT id, slug, body, payload FROM ctas_source_posts"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_generated_source ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL, "
        "title_len INT AS (CHAR_LENGTH(title)) VIRTUAL, "
        "label VARCHAR(80) AS (CONCAT(title, '-', id)) STORED"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO ctas_generated_source (id, title) VALUES "
        "(1, 'draft'), (2, 'published')"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_generated_projection ENGINE=InnoDB AS "
        "SELECT id, title_len, label FROM ctas_generated_source WHERE id = 1"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_generated_target_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL, "
        "title_len INT AS (CHAR_LENGTH(title)) VIRTUAL, "
        "label VARCHAR(80) AS (CONCAT(title, '-', id)) STORED"
        ") ENGINE=InnoDB "
        "SELECT 2 AS id, 'target' AS title"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_checked_posts ("
        "id INT NOT NULL, "
        "rating INT NOT NULL CHECK (rating >= 0), "
        "CONSTRAINT rating_limit CHECK (rating <= 5)"
        ") ENGINE=InnoDB "
        "SELECT 1 AS id, 4 AS rating"
    );
    assert_catalog_table_count(filename, "app", 7U);
    assert_catalog_table_metadata(filename, "app", "ctas_source_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_default_constants", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_indexed_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_generated_source", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_generated_projection", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "app",
        "ctas_generated_target_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(filename, "app", "ctas_checked_posts", "InnoDB", "MYLITE");

    assert(
        mylite_exec(
            db,
            "SELECT label FROM ctas_default_constants WHERE id = 10",
            single_value_callback,
            &default_label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(default_label.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM ctas_indexed_posts",
            single_value_callback,
            &copied_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(copied_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT HEX(payload) FROM ctas_indexed_posts FORCE INDEX (slug_key) "
            "WHERE slug = 'source-alpha'",
            single_value_callback,
            &copied_payload,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(copied_payload.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM ctas_indexed_posts FORCE INDEX (body_prefix) "
            "WHERE body = 'source body two'",
            row_callback,
            &body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(body_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM ctas_generated_projection WHERE id = 1",
            single_value_callback,
            &generated_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT label FROM ctas_generated_projection WHERE id = 1",
            single_value_callback,
            &generated_label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_label.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM ctas_generated_target_posts WHERE id = 2",
            single_value_callback,
            &target_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(target_title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT label FROM ctas_generated_target_posts WHERE id = 2",
            single_value_callback,
            &target_label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(target_label.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT rating FROM ctas_checked_posts WHERE id = 1",
            single_value_callback,
            &checked_rating,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(checked_rating.rows == 1);

    assert_exec_fails(
        db,
        "INSERT INTO ctas_indexed_posts (slug, body, payload) VALUES "
        "('source-alpha', 'duplicate slug', UNHEX('AA'))"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO ctas_indexed_posts (slug, body, payload) VALUES "
        "('source-gamma', 'source body three', UNHEX('070809'))"
    );
    assert_exec_fails(db, "INSERT INTO ctas_checked_posts VALUES (2, -1)");
    assert_exec_fails(db, "INSERT INTO ctas_checked_posts VALUES (3, 6)");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM ctas_indexed_posts FORCE INDEX (slug_key) "
            "WHERE slug = 'source-gamma'",
            single_value_callback,
            &next_id,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(next_id.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "app");
    assert_exec_succeeds(db, "USE app");
    generated_title_len = (single_value_context){.expected_value = "5"};
    generated_label = (single_value_context){.expected_value = "draft-1"};
    target_title_len = (single_value_context){.expected_value = "6"};
    target_label = (single_value_context){.expected_value = "target-2"};
    assert_catalog_table_count(filename, "app", 7U);
    assert_catalog_table_metadata(filename, "app", "ctas_source_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_default_constants", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_indexed_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_generated_source", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_generated_projection", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "app",
        "ctas_generated_target_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(filename, "app", "ctas_checked_posts", "InnoDB", "MYLITE");
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM ctas_indexed_posts",
            single_value_callback,
            &reopened_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM ctas_indexed_posts FORCE INDEX (body_prefix) "
            "WHERE body = 'source body three'",
            row_callback,
            &reopened_body_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_body_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM ctas_generated_projection WHERE id = 1",
            single_value_callback,
            &generated_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT label FROM ctas_generated_projection WHERE id = 1",
            single_value_callback,
            &generated_label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(generated_label.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM ctas_generated_target_posts WHERE id = 2",
            single_value_callback,
            &target_title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(target_title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT label FROM ctas_generated_target_posts WHERE id = 2",
            single_value_callback,
            &target_label,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(target_label.rows == 1);
    assert_exec_fails(db, "INSERT INTO ctas_checked_posts VALUES (4, 7)");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM ctas_checked_posts WHERE rating = 4",
            row_callback,
            &reopened_checked_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_checked_rows.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_create_table_select_duplicate_modes(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE ctas_duplicate_modes");
    assert_exec_succeeds(db, "USE ctas_duplicate_modes");
    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_duplicate_source ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO ctas_duplicate_source VALUES "
        "(1, 'alpha', 'body one'), "
        "(2, 'alpha', 'body two'), "
        "(3, 'beta', 'body three')"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_ignore_posts ("
        "id INT NOT NULL, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB IGNORE "
        "SELECT id, slug, body FROM ctas_duplicate_source ORDER BY id"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE ctas_replace_posts ("
        "id INT NOT NULL, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB REPLACE "
        "SELECT id, slug, body FROM ctas_duplicate_source ORDER BY id"
    );
    assert_catalog_table_count(filename, "ctas_duplicate_modes", 3U);
    assert_catalog_table_metadata(
        filename,
        "ctas_duplicate_modes",
        "ctas_duplicate_source",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "ctas_duplicate_modes",
        "ctas_ignore_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "ctas_duplicate_modes",
        "ctas_replace_posts",
        "InnoDB",
        "MYLITE"
    );

    assert_query_single_value(db, "SELECT COUNT(*) FROM ctas_ignore_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM ctas_ignore_posts FORCE INDEX (slug_key) WHERE slug = 'alpha'",
        "1"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM ctas_ignore_posts WHERE id = 2", "0");
    assert_query_single_value(
        db,
        "SELECT id FROM ctas_ignore_posts FORCE INDEX (body_prefix) "
        "WHERE body = 'body three'",
        "3"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM ctas_replace_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM ctas_replace_posts FORCE INDEX (slug_key) WHERE slug = 'alpha'",
        "2"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM ctas_replace_posts WHERE id = 1", "0");
    assert_query_single_value(
        db,
        "SELECT id FROM ctas_replace_posts FORCE INDEX (body_prefix) "
        "WHERE body = 'body two'",
        "2"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "ctas_duplicate_modes");
    assert_exec_succeeds(db, "USE ctas_duplicate_modes");
    assert_catalog_table_count(filename, "ctas_duplicate_modes", 3U);
    assert_catalog_table_metadata(
        filename,
        "ctas_duplicate_modes",
        "ctas_ignore_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "ctas_duplicate_modes",
        "ctas_replace_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM ctas_ignore_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM ctas_ignore_posts FORCE INDEX (slug_key) WHERE slug = 'alpha'",
        "1"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM ctas_replace_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM ctas_replace_posts FORCE INDEX (slug_key) WHERE slug = 'alpha'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM ctas_replace_posts FORCE INDEX (body_prefix) "
        "WHERE body = 'body three'",
        "3"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_temporary_table_catalog_isolation(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE temp_isolation");
    assert_exec_succeeds(db, "USE temp_isolation");
    assert_exec_succeeds(
        db,
        "CREATE TABLE source_posts ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO source_posts (slug, body) VALUES "
        "('source-alpha', 'source body one'), "
        "('source-beta', 'source body two')"
    );
    assert_catalog_table_count(filename, "temp_isolation", 1U);
    assert_catalog_table_count(filename, "tmp", 0U);

    assert_exec_succeeds(db, "CREATE TEMPORARY TABLE temp_like_posts LIKE source_posts");
    assert_exec_succeeds(
        db,
        "INSERT INTO temp_like_posts (slug, body) VALUES "
        "('temp-alpha', 'temporary like body')"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM temp_like_posts", "1");
    assert_query_single_value(
        db,
        "SELECT id FROM temp_like_posts FORCE INDEX (slug_key) WHERE slug = 'temp-alpha'",
        "1"
    );
    assert(
        mylite_storage_table_exists(filename, "temp_isolation", "temp_like_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "temp_isolation", 1U);

    assert_exec_succeeds(
        db,
        "CREATE TEMPORARY TABLE temp_select_posts AS "
        "SELECT id, slug, body FROM source_posts WHERE id = 2"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM temp_select_posts", "1");
    assert_query_single_value(db, "SELECT slug FROM temp_select_posts WHERE id = 2", "source-beta");
    assert(
        mylite_storage_table_exists(filename, "temp_isolation", "temp_select_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "temp_isolation", 1U);

    assert_exec_succeeds(db, "DROP TEMPORARY TABLE temp_like_posts, temp_select_posts");
    assert_catalog_table_count(filename, "temp_isolation", 1U);
    assert_catalog_table_count(filename, "tmp", 0U);

    assert_exec_succeeds(db, "CREATE TABLE shadow_like_posts LIKE source_posts");
    assert_exec_succeeds(
        db,
        "INSERT INTO shadow_like_posts (slug, body) VALUES "
        "('durable-shadow-like', 'durable shadow like body')"
    );
    assert_catalog_table_count(filename, "temp_isolation", 2U);
    assert_catalog_table_metadata(
        filename,
        "temp_isolation",
        "shadow_like_posts",
        "InnoDB",
        "MYLITE"
    );

    assert_exec_succeeds(db, "CREATE TEMPORARY TABLE shadow_like_posts LIKE source_posts");
    assert_exec_succeeds(
        db,
        "INSERT INTO shadow_like_posts (slug, body) VALUES "
        "('temp-shadow-like', 'temporary shadow like body')"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM shadow_like_posts", "1");
    assert_query_single_value(
        db,
        "SELECT slug FROM shadow_like_posts WHERE id = 1",
        "temp-shadow-like"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM shadow_like_posts FORCE INDEX (body_prefix) "
        "WHERE body = 'temporary shadow like body'",
        "1"
    );
    assert_catalog_table_count(filename, "temp_isolation", 2U);

    assert_exec_succeeds(
        db,
        "CREATE TEMPORARY TABLE source_posts AS "
        "SELECT id, slug, body FROM source_posts WHERE id = 2"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM source_posts", "1");
    assert_query_single_value(db, "SELECT slug FROM source_posts WHERE id = 2", "source-beta");
    assert_catalog_table_count(filename, "temp_isolation", 2U);

    assert_exec_succeeds(db, "DROP TEMPORARY TABLE shadow_like_posts, source_posts");
    assert_catalog_table_count(filename, "temp_isolation", 2U);
    assert_catalog_table_count(filename, "tmp", 0U);
    assert_query_single_value(db, "SELECT COUNT(*) FROM source_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM shadow_like_posts FORCE INDEX (slug_key) "
        "WHERE slug = 'durable-shadow-like'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM shadow_like_posts FORCE INDEX (body_prefix) "
        "WHERE body = 'durable shadow like body'",
        "1"
    );

    assert_exec_succeeds(db, "CREATE TEMPORARY TABLE shadow_like_posts LIKE source_posts");
    assert_exec_succeeds(
        db,
        "INSERT INTO shadow_like_posts (slug, body) VALUES "
        "('temp-before-replace-like', 'temporary before replace like body')"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM shadow_like_posts", "1");
    assert_exec_succeeds(
        db,
        "CREATE OR REPLACE TEMPORARY TABLE shadow_like_posts LIKE source_posts"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM shadow_like_posts", "0");
    assert_exec_succeeds(
        db,
        "INSERT INTO shadow_like_posts (slug, body) VALUES "
        "('temp-replaced-like', 'temporary replaced like body')"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM shadow_like_posts FORCE INDEX (body_prefix) "
        "WHERE body = 'temporary replaced like body'",
        "1"
    );
    assert_catalog_table_count(filename, "temp_isolation", 2U);

    assert_exec_succeeds(
        db,
        "CREATE TEMPORARY TABLE replace_select_posts AS "
        "SELECT id, slug, body FROM source_posts WHERE id = 1"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM replace_select_posts", "1");
    assert_query_single_value(
        db,
        "SELECT slug FROM replace_select_posts WHERE id = 1",
        "source-alpha"
    );
    assert_exec_succeeds(
        db,
        "CREATE OR REPLACE TEMPORARY TABLE replace_select_posts AS "
        "SELECT id, slug, body FROM source_posts WHERE id = 2"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM replace_select_posts", "1");
    assert_query_single_value(db, "SELECT COUNT(*) FROM replace_select_posts WHERE id = 1", "0");
    assert_query_single_value(
        db,
        "SELECT slug FROM replace_select_posts WHERE id = 2",
        "source-beta"
    );
    assert(
        mylite_storage_table_exists(filename, "temp_isolation", "replace_select_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "temp_isolation", 2U);

    assert_exec_succeeds(
        db,
        "CREATE OR REPLACE TEMPORARY TABLE source_posts AS "
        "SELECT id, slug, body FROM source_posts WHERE id = 1"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM source_posts", "1");
    assert_query_single_value(db, "SELECT slug FROM source_posts WHERE id = 1", "source-alpha");
    assert_exec_fails_with_message(
        db,
        "CREATE OR REPLACE TEMPORARY TABLE source_posts AS "
        "SELECT id, slug, body FROM source_posts WHERE id = 2",
        "Can't reopen table"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM source_posts", "1");
    assert_query_single_value(db, "SELECT slug FROM source_posts WHERE id = 1", "source-alpha");
    assert_catalog_table_count(filename, "temp_isolation", 2U);

    assert_exec_succeeds(
        db,
        "DROP TEMPORARY TABLE shadow_like_posts, replace_select_posts, source_posts"
    );
    assert_catalog_table_count(filename, "temp_isolation", 2U);
    assert_catalog_table_count(filename, "tmp", 0U);
    assert_query_single_value(db, "SELECT COUNT(*) FROM source_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM shadow_like_posts FORCE INDEX (slug_key) "
        "WHERE slug = 'durable-shadow-like'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM shadow_like_posts FORCE INDEX (body_prefix) "
        "WHERE body = 'durable shadow like body'",
        "1"
    );

    assert_exec_succeeds(db, "CREATE TEMPORARY TABLE temp_close_posts LIKE source_posts");
    assert_exec_succeeds(
        db,
        "INSERT INTO temp_close_posts (slug, body) VALUES "
        "('temp-close', 'temporary close body')"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM temp_close_posts", "1");
    assert(
        mylite_storage_table_exists(filename, "temp_isolation", "temp_close_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_count(filename, "temp_isolation", 2U);

    assert(mylite_close(db) == MYLITE_OK);
    assert_catalog_table_count(filename, "tmp", 0U);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "temp_isolation");
    assert_exec_succeeds(db, "USE temp_isolation");
    assert_catalog_table_count(filename, "temp_isolation", 2U);
    assert_catalog_table_metadata(filename, "temp_isolation", "source_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "temp_isolation",
        "shadow_like_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM source_posts", "2");
    assert_query_single_value(
        db,
        "SELECT slug FROM shadow_like_posts WHERE id = 1",
        "durable-shadow-like"
    );
    assert_exec_fails(db, "SELECT COUNT(*) FROM temp_like_posts");
    assert_exec_fails(db, "SELECT COUNT(*) FROM temp_select_posts");
    assert_exec_fails(db, "SELECT COUNT(*) FROM temp_close_posts");
    assert_catalog_table_count(filename, "tmp", 0U);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_create_or_replace_table(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context old_plain_index_rows = {0};
    table_context plain_primary_index_rows = {0};
    table_context reopened_old_plain_index_rows = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE replace_app");
    assert_exec_succeeds(db, "USE replace_app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE replace_plain_target ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "old_slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY old_slug_key (old_slug), "
        "KEY old_body_prefix (body(8))"
        ") ENGINE=MyISAM"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO replace_plain_target (old_slug, body) VALUES "
        "('old-plain-alpha', 'old plain body one'), "
        "('old-plain-beta', 'old plain body two')"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE replace_like_source ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE replace_like_target ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL"
        ") ENGINE=MyISAM"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO replace_like_target VALUES "
        "(99, 'old-like-target', 'old target body')"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE replace_ctas_source ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO replace_ctas_source VALUES "
        "(1, 'ctas-alpha', 'ctas body one'), "
        "(2, 'ctas-beta', 'ctas body two'), "
        "(3, 'ctas-gamma', 'ctas body three')"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE replace_ctas_target ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL"
        ") ENGINE=MyISAM"
    );
    assert_exec_succeeds(db, "INSERT INTO replace_ctas_target VALUES (99, 'old-ctas-target')");
    assert_catalog_table_count(filename, "replace_app", 5U);
    assert_catalog_table_metadata(
        filename,
        "replace_app",
        "replace_plain_target",
        "MyISAM",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "replace_app",
        "replace_like_target",
        "MyISAM",
        "MYLITE"
    );

    assert_exec_succeeds(
        db,
        "CREATE OR REPLACE TABLE replace_plain_target ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "replace_app", 5U);
    assert_catalog_table_metadata(
        filename,
        "replace_app",
        "replace_plain_target",
        "InnoDB",
        "MYLITE"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM replace_plain_target", "0");
    assert_exec_fails(db, "SELECT old_slug FROM replace_plain_target");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM replace_plain_target WHERE Key_name = 'old_slug_key'",
            table_callback,
            &old_plain_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(old_plain_index_rows.rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM replace_plain_target WHERE Key_name = 'PRIMARY'",
            table_callback,
            &plain_primary_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(plain_primary_index_rows.rows == 1);
    assert_exec_succeeds(
        db,
        "INSERT INTO replace_plain_target (slug, body) VALUES "
        "('new-plain-alpha', 'new plain body one'), "
        "('new-plain-beta', 'new plain body two')"
    );
    assert_exec_fails(
        db,
        "INSERT INTO replace_plain_target (slug, body) VALUES "
        "('new-plain-alpha', 'duplicate plain body')"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM replace_plain_target FORCE INDEX (slug_key) "
        "WHERE slug = 'new-plain-alpha'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM replace_plain_target FORCE INDEX (body_prefix) "
        "WHERE body = 'new plain body two'",
        "2"
    );

    assert_exec_succeeds(
        db,
        "CREATE OR REPLACE TABLE replace_like_target LIKE replace_like_source"
    );
    assert_catalog_table_count(filename, "replace_app", 5U);
    assert_catalog_table_metadata(
        filename,
        "replace_app",
        "replace_like_source",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "replace_app",
        "replace_like_target",
        "InnoDB",
        "MYLITE"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM replace_like_target", "0");
    assert_exec_succeeds(
        db,
        "INSERT INTO replace_like_target (slug, body) VALUES "
        "('new-like-target', 'new target body')"
    );
    assert_exec_fails(
        db,
        "INSERT INTO replace_like_target (slug, body) VALUES "
        "('new-like-target', 'duplicate target body')"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM replace_like_target FORCE INDEX (slug_key) "
        "WHERE slug = 'new-like-target'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM replace_like_target FORCE INDEX (body_prefix) "
        "WHERE body = 'new target body'",
        "1"
    );

    assert_exec_succeeds(
        db,
        "CREATE OR REPLACE TABLE replace_ctas_target ("
        "id INT NOT NULL, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB "
        "SELECT id, slug, body FROM replace_ctas_source WHERE id <= 2"
    );
    assert_catalog_table_count(filename, "replace_app", 5U);
    assert_catalog_table_metadata(
        filename,
        "replace_app",
        "replace_ctas_target",
        "InnoDB",
        "MYLITE"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM replace_ctas_target", "2");
    assert_exec_fails(
        db,
        "INSERT INTO replace_ctas_target VALUES "
        "(4, 'ctas-alpha', 'duplicate slug')"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM replace_ctas_target FORCE INDEX (slug_key) WHERE slug = 'ctas-beta'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM replace_ctas_target FORCE INDEX (body_prefix) "
        "WHERE body = 'ctas body one'",
        "1"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "replace_app");
    assert_exec_succeeds(db, "USE replace_app");
    assert_catalog_table_count(filename, "replace_app", 5U);
    assert_catalog_table_metadata(
        filename,
        "replace_app",
        "replace_plain_target",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "replace_app",
        "replace_like_target",
        "InnoDB",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "replace_app",
        "replace_ctas_target",
        "InnoDB",
        "MYLITE"
    );
    assert_query_single_value(db, "SELECT COUNT(*) FROM replace_plain_target", "2");
    assert_query_single_value(db, "SELECT COUNT(*) FROM replace_like_target", "1");
    assert_query_single_value(db, "SELECT COUNT(*) FROM replace_ctas_target", "2");
    assert_exec_fails(db, "SELECT old_slug FROM replace_plain_target");
    assert(
        mylite_exec(
            db,
            "SHOW INDEX FROM replace_plain_target WHERE Key_name = 'old_slug_key'",
            table_callback,
            &reopened_old_plain_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_old_plain_index_rows.rows == 0);
    assert_query_single_value(
        db,
        "SELECT id FROM replace_plain_target FORCE INDEX (slug_key) "
        "WHERE slug = 'new-plain-beta'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM replace_like_target FORCE INDEX (slug_key) "
        "WHERE slug = 'new-like-target'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM replace_ctas_target FORCE INDEX (body_prefix) "
        "WHERE body = 'ctas body two'",
        "2"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_failed_create_or_replace_rollback(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE replace_rollback");
    assert_exec_succeeds(db, "USE replace_rollback");
    assert_exec_succeeds(
        db,
        "CREATE TABLE target_posts ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=MyISAM"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO target_posts (slug, body) VALUES "
        "('old-alpha', 'old body one'), "
        "('old-beta', 'old body two')"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE duplicate_source_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO duplicate_source_posts VALUES "
        "(10, 'duplicate', 'replacement body one'), "
        "(11, 'duplicate', 'replacement body two')"
    );
    assert_catalog_table_count(filename, "replace_rollback", 2U);
    assert_catalog_table_metadata(filename, "replace_rollback", "target_posts", "MyISAM", "MYLITE");

    assert_exec_fails(db, "CREATE OR REPLACE TABLE target_posts LIKE target_posts");
    assert_catalog_table_count(filename, "replace_rollback", 2U);
    assert_query_single_value(db, "SELECT COUNT(*) FROM target_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM target_posts FORCE INDEX (slug_key) WHERE slug = 'old-beta'",
        "2"
    );
    assert_catalog_table_metadata(filename, "replace_rollback", "target_posts", "MyISAM", "MYLITE");

    assert_exec_fails(
        db,
        "CREATE OR REPLACE TABLE target_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "body TEXT NOT NULL, "
        "FULLTEXT KEY body_fulltext (body)"
        ") ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "replace_rollback", 2U);
    assert_query_single_value(db, "SELECT COUNT(*) FROM target_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM target_posts FORCE INDEX (body_prefix) WHERE body = 'old body one'",
        "1"
    );
    assert_catalog_table_metadata(filename, "replace_rollback", "target_posts", "MyISAM", "MYLITE");

    assert_exec_fails(
        db,
        "CREATE OR REPLACE TABLE target_posts ("
        "id INT NOT NULL, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB "
        "SELECT id, slug, body FROM duplicate_source_posts"
    );
    assert_catalog_table_count(filename, "replace_rollback", 2U);
    assert_catalog_table_metadata(filename, "replace_rollback", "target_posts", "MyISAM", "MYLITE");
    assert_query_single_value(db, "SELECT COUNT(*) FROM target_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM target_posts FORCE INDEX (slug_key) WHERE slug = 'old-alpha'",
        "1"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO target_posts (slug, body) VALUES ('old-gamma', 'old body three')"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM target_posts FORCE INDEX (slug_key) WHERE slug = 'old-gamma'",
        "3"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "replace_rollback");
    assert_exec_succeeds(db, "USE replace_rollback");
    assert_catalog_table_count(filename, "replace_rollback", 2U);
    assert_catalog_table_metadata(filename, "replace_rollback", "target_posts", "MyISAM", "MYLITE");
    assert_query_single_value(db, "SELECT COUNT(*) FROM target_posts", "3");
    assert_query_single_value(
        db,
        "SELECT id FROM target_posts FORCE INDEX (body_prefix) WHERE body = 'old body three'",
        "3"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_failed_table_ddl_rollback(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE ddl_rollback");
    assert_exec_succeeds(db, "USE ddl_rollback");
    assert_exec_succeeds(
        db,
        "CREATE TABLE drop_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO drop_posts VALUES "
        "(1, 'drop-alpha', 'old drop body one'), "
        "(2, 'drop-beta', 'old drop body two')"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE rename_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO rename_posts VALUES "
        "(1, 'rename-alpha', 'old rename body one'), "
        "(2, 'rename-beta', 'old rename body two')"
    );
    assert_catalog_table_count(filename, "ddl_rollback", 2U);

    assert_exec_fails(db, "DROP TABLE drop_posts, missing_drop_posts");
    assert_catalog_table_count(filename, "ddl_rollback", 2U);
    assert_catalog_table_metadata(filename, "ddl_rollback", "drop_posts", "InnoDB", "MYLITE");
    assert_query_single_value(db, "SELECT COUNT(*) FROM drop_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM drop_posts FORCE INDEX (slug_key) WHERE slug = 'drop-beta'",
        "2"
    );

    assert_exec_fails(
        db,
        "RENAME TABLE rename_posts TO renamed_posts, "
        "missing_rename_posts TO missing_renamed_posts"
    );
    assert_catalog_table_count(filename, "ddl_rollback", 2U);
    assert_catalog_table_metadata(filename, "ddl_rollback", "rename_posts", "InnoDB", "MYLITE");
    assert(
        mylite_storage_table_exists(filename, "ddl_rollback", "renamed_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_exec_fails(db, "SELECT COUNT(*) FROM renamed_posts");
    assert_query_single_value(db, "SELECT COUNT(*) FROM rename_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM rename_posts FORCE INDEX (body_prefix) WHERE body = 'old rename body one'",
        "1"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "ddl_rollback");
    assert_exec_succeeds(db, "USE ddl_rollback");
    assert_catalog_table_count(filename, "ddl_rollback", 2U);
    assert_catalog_table_metadata(filename, "ddl_rollback", "drop_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "ddl_rollback", "rename_posts", "InnoDB", "MYLITE");
    assert_query_single_value(db, "SELECT COUNT(*) FROM drop_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM rename_posts FORCE INDEX (slug_key) WHERE slug = 'rename-beta'",
        "2"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_table_ddl_if_exists(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE ddl_if_exists");
    assert_exec_succeeds(db, "USE ddl_if_exists");
    assert_exec_succeeds(
        db,
        "CREATE TABLE drop_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO drop_posts VALUES "
        "(1, 'drop-alpha', 'drop body one'), "
        "(2, 'drop-beta', 'drop body two')"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE rename_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "slug VARCHAR(32) NOT NULL, "
        "body LONGTEXT NULL, "
        "UNIQUE KEY slug_key (slug), "
        "KEY body_prefix (body(8))"
        ") ENGINE=MyISAM"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO rename_posts VALUES "
        "(1, 'rename-alpha', 'rename body one'), "
        "(2, 'rename-beta', 'rename body two')"
    );
    assert_catalog_table_count(filename, "ddl_if_exists", 2U);

    assert_exec_succeeds(db, "DROP TABLE IF EXISTS drop_posts, missing_drop_posts");
    assert_warning_message_contains(db, "missing_drop_posts");
    assert_catalog_table_count(filename, "ddl_if_exists", 1U);
    assert(
        mylite_storage_table_exists(filename, "ddl_if_exists", "drop_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_exec_fails(db, "SELECT COUNT(*) FROM drop_posts");
    assert_catalog_table_metadata(filename, "ddl_if_exists", "rename_posts", "MyISAM", "MYLITE");

    assert_exec_succeeds(db, "DROP TABLE IF EXISTS missing_only_posts");
    assert_warning_message_contains(db, "missing_only_posts");
    assert_catalog_table_count(filename, "ddl_if_exists", 1U);
    assert_query_single_value(db, "SELECT COUNT(*) FROM rename_posts", "2");

    assert_exec_succeeds(
        db,
        "RENAME TABLE IF EXISTS missing_rename_posts TO skipped_rename_posts, "
        "rename_posts TO renamed_posts"
    );
    assert_warning_message_contains(db, "missing_rename_posts");
    assert_catalog_table_count(filename, "ddl_if_exists", 1U);
    assert(
        mylite_storage_table_exists(filename, "ddl_if_exists", "missing_rename_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_table_exists(filename, "ddl_if_exists", "skipped_rename_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_table_exists(filename, "ddl_if_exists", "rename_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_metadata(filename, "ddl_if_exists", "renamed_posts", "MyISAM", "MYLITE");
    assert_query_single_value(db, "SELECT COUNT(*) FROM renamed_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM renamed_posts FORCE INDEX (slug_key) WHERE slug = 'rename-beta'",
        "2"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "ddl_if_exists");
    assert_exec_succeeds(db, "USE ddl_if_exists");
    assert_catalog_table_count(filename, "ddl_if_exists", 1U);
    assert(
        mylite_storage_table_exists(filename, "ddl_if_exists", "drop_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_table_exists(filename, "ddl_if_exists", "skipped_rename_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_table_exists(filename, "ddl_if_exists", "rename_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert_catalog_table_metadata(filename, "ddl_if_exists", "renamed_posts", "MyISAM", "MYLITE");
    assert_query_single_value(db, "SELECT COUNT(*) FROM renamed_posts", "2");
    assert_query_single_value(
        db,
        "SELECT id FROM renamed_posts FORCE INDEX (body_prefix) WHERE body = 'rename body one'",
        "1"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_constraint_generated_dump_fixture(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    single_value_context title_len = {
        .expected_value = "5",
    };
    single_value_context title_key = {
        .expected_value = "alpha",
    };
    single_value_context title_len_index = {
        .expected_value = "1",
    };
    single_value_context title_key_index = {
        .expected_value = "1",
    };
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE dump_import");
    assert_exec_succeeds(db, "USE dump_import");
    exec_sql_fixture(db, "constraint-generated-dump.sql");
    assert_catalog_table_count(filename, "dump_import", 1U);
    assert_catalog_table_metadata(filename, "dump_import", "dump_posts", "InnoDB", "MYLITE");
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM dump_posts WHERE id = 1",
            single_value_callback,
            &title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT title_key FROM dump_posts WHERE id = 1",
            single_value_callback,
            &title_key,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_key.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM dump_posts FORCE INDEX (title_len_key) WHERE title_len = 5",
            single_value_callback,
            &title_len_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_len_index.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM dump_posts FORCE INDEX (title_key_unique) WHERE title_key = 'alpha'",
            single_value_callback,
            &title_key_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_key_index.rows == 1);
    assert_exec_fails_with_message(
        db,
        "INSERT INTO dump_posts (id, title, rating) VALUES (3, 'Gamma', 11)",
        "CONSTRAINT"
    );
    assert_prepared_fails_with_message(
        db,
        "INSERT INTO dump_posts (id, title, rating) VALUES (4, 'ALPHA', 5)",
        "Duplicate"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    title_len = (single_value_context){.expected_value = "4"};
    title_key = (single_value_context){.expected_value = "beta"};
    title_len_index = (single_value_context){.expected_value = "2"};
    title_key_index = (single_value_context){.expected_value = "2"};
    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE dump_import");
    assert_catalog_table_count(filename, "dump_import", 1U);
    assert_catalog_table_metadata(filename, "dump_import", "dump_posts", "InnoDB", "MYLITE");
    assert(
        mylite_exec(
            db,
            "SELECT title_len FROM dump_posts WHERE id = 2",
            single_value_callback,
            &title_len,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_len.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT title_key FROM dump_posts WHERE id = 2",
            single_value_callback,
            &title_key,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_key.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM dump_posts FORCE INDEX (title_len_key) WHERE title_len = 4",
            single_value_callback,
            &title_len_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_len_index.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM dump_posts FORCE INDEX (title_key_unique) WHERE title_key = 'beta'",
            single_value_callback,
            &title_key_index,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(title_key_index.rows == 1);
    assert_exec_fails_with_message(
        db,
        "INSERT INTO dump_posts (id, title, rating) VALUES (5, 'Delta', -1)",
        "CONSTRAINT"
    );
    assert_prepared_fails_with_message(
        db,
        "INSERT INTO dump_posts (id, title, rating) VALUES (6, 'beta', 6)",
        "Duplicate"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_show_create_table_round_trip(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE export_source");
    assert_exec_succeeds(db, "USE export_source");
    assert_exec_succeeds(
        db,
        "CREATE TABLE export_posts ("
        "id int NOT NULL AUTO_INCREMENT,"
        "title varchar(64) NOT NULL,"
        "status varchar(16) NOT NULL,"
        "rating int NOT NULL,"
        "body text NOT NULL,"
        "slug varchar(96) AS (LOWER(REPLACE(title, ' ', '-'))) STORED,"
        "PRIMARY KEY (id),"
        "UNIQUE KEY slug_unique (slug),"
        "KEY status_rating (status, rating),"
        "KEY body_prefix (body(12)),"
        "CONSTRAINT rating_window CHECK (rating BETWEEN 0 AND 10),"
        "CONSTRAINT status_choice CHECK (status IN ('draft', 'published'))"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO export_posts (title, status, rating, body) "
        "VALUES ('Source Post', 'draft', 4, 'source-body')"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE export_source");
    char *create_sql = capture_show_create_table(db, "export_posts");
    assert(strstr(create_sql, "ENGINE=InnoDB") != NULL);
    assert(strstr(create_sql, "AUTO_INCREMENT=2") != NULL);

    assert_exec_succeeds(db, "CREATE DATABASE export_roundtrip");
    assert_exec_succeeds(db, "USE export_roundtrip");
    assert_exec_succeeds(db, create_sql);
    assert_catalog_table_count(filename, "export_roundtrip", 1U);
    assert_catalog_table_metadata(filename, "export_roundtrip", "export_posts", "InnoDB", "MYLITE");

    assert_exec_succeeds(
        db,
        "INSERT INTO export_posts (title, status, rating, body) "
        "VALUES ('Round Trip', 'published', 8, 'roundtrip-body')"
    );
    assert_query_single_value(db, "SELECT id FROM export_posts WHERE title = 'Round Trip'", "2");
    assert_query_single_value(db, "SELECT slug FROM export_posts WHERE id = 2", "round-trip");
    assert_query_single_value(
        db,
        "SELECT id FROM export_posts FORCE INDEX (slug_unique) WHERE slug = 'round-trip'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM export_posts FORCE INDEX (status_rating) "
        "WHERE status = 'published' AND rating = 8",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM export_posts FORCE INDEX (body_prefix) WHERE body = 'roundtrip-body'",
        "2"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO export_posts (title, status, rating, body) "
        "VALUES ('Bad Status', 'archived', 5, 'bad-status')",
        "CONSTRAINT"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO export_posts (title, status, rating, body) "
        "VALUES ('Round Trip', 'draft', 3, 'duplicate-slug')",
        "Duplicate"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE export_roundtrip");
    assert_catalog_table_count(filename, "export_roundtrip", 1U);
    assert_catalog_table_metadata(filename, "export_roundtrip", "export_posts", "InnoDB", "MYLITE");
    assert_query_single_value(db, "SELECT slug FROM export_posts WHERE id = 2", "round-trip");
    assert_query_single_value(
        db,
        "SELECT id FROM export_posts FORCE INDEX (body_prefix) WHERE body = 'roundtrip-body'",
        "2"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO export_posts (title, status, rating, body) "
        "VALUES ('Too High', 'draft', 11, 'too-high')",
        "CONSTRAINT"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(create_sql);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_constraint_generated_expression_matrix(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(db, "CREATE DATABASE expression_matrix");
    assert_exec_succeeds(db, "USE expression_matrix");
    assert_exec_succeeds(
        db,
        "CREATE TABLE expression_posts ("
        "id int NOT NULL,"
        "title varchar(64) NOT NULL,"
        "subtitle varchar(64) DEFAULT NULL,"
        "status varchar(16) NOT NULL,"
        "rating int NOT NULL,"
        "bonus int NOT NULL DEFAULT 0,"
        "slug varchar(96) AS (LOWER(REPLACE(title, ' ', '-'))) VIRTUAL,"
        "display_title varchar(129) AS "
        "(CONCAT(title, COALESCE(CONCAT(': ', subtitle), ''))) STORED,"
        "score_total int AS (rating + bonus) STORED,"
        "status_rank int AS "
        "(CASE WHEN status = 'published' THEN 1 ELSE 0 END) VIRTUAL,"
        "PRIMARY KEY (id),"
        "UNIQUE KEY slug_unique (slug),"
        "KEY score_total_key (score_total),"
        "CONSTRAINT rating_window CHECK (rating BETWEEN 0 AND 10),"
        "CONSTRAINT score_window CHECK (rating + bonus BETWEEN 0 AND 15),"
        "CONSTRAINT status_choice CHECK (status IN ('draft', 'published')),"
        "CONSTRAINT trimmed_title CHECK (title = TRIM(title) AND CHAR_LENGTH(title) > 0),"
        "CONSTRAINT subtitle_window CHECK "
        "(subtitle IS NULL OR CHAR_LENGTH(subtitle) <= 32)"
        ") ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "expression_matrix", 1U);
    assert_catalog_table_metadata(
        filename,
        "expression_matrix",
        "expression_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO expression_posts (id, title, subtitle, status, rating, bonus) VALUES "
        "(1, 'Alpha Post', NULL, 'draft', 4, 1),"
        "(2, 'Beta Post', 'Launch', 'published', 9, 3)"
    );
    assert_query_single_value(db, "SELECT slug FROM expression_posts WHERE id = 1", "alpha-post");
    assert_query_single_value(
        db,
        "SELECT display_title FROM expression_posts WHERE id = 2",
        "Beta Post: Launch"
    );
    assert_query_single_value(db, "SELECT score_total FROM expression_posts WHERE id = 2", "12");
    assert_query_single_value(db, "SELECT status_rank FROM expression_posts WHERE id = 1", "0");
    assert_query_single_value(
        db,
        "SELECT id FROM expression_posts FORCE INDEX (slug_unique) WHERE slug = 'alpha-post'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM expression_posts FORCE INDEX (score_total_key) WHERE score_total = 12",
        "2"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO expression_posts (id, title, subtitle, status, rating, bonus) "
        "VALUES (3, 'Bad Rating', NULL, 'draft', 11, 0)",
        "CONSTRAINT"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO expression_posts (id, title, subtitle, status, rating, bonus) "
        "VALUES (3, 'Bad Score', NULL, 'draft', 9, 9)",
        "CONSTRAINT"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO expression_posts (id, title, subtitle, status, rating, bonus) "
        "VALUES (3, 'Bad Status', NULL, 'archived', 4, 1)",
        "CONSTRAINT"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO expression_posts (id, title, subtitle, status, rating, bonus) "
        "VALUES (3, ' Bad Title ', NULL, 'draft', 4, 1)",
        "CONSTRAINT"
    );
    assert_exec_fails_with_message(
        db,
        "UPDATE expression_posts SET bonus = 20 WHERE id = 2",
        "CONSTRAINT"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO expression_posts (id, title, subtitle, status, rating, bonus) "
        "VALUES (3, 'Alpha Post', NULL, 'draft', 5, 1)",
        "Duplicate"
    );

    assert_exec_succeeds(
        db,
        "UPDATE expression_posts "
        "SET title = 'Gamma Post', subtitle = 'Review', status = 'published', rating = 5, "
        "bonus = 2 "
        "WHERE id = 1"
    );
    assert_query_single_value(db, "SELECT slug FROM expression_posts WHERE id = 1", "gamma-post");
    assert_query_single_value(
        db,
        "SELECT display_title FROM expression_posts WHERE id = 1",
        "Gamma Post: Review"
    );
    assert_query_single_value(db, "SELECT score_total FROM expression_posts WHERE id = 1", "7");
    assert_query_single_value(db, "SELECT status_rank FROM expression_posts WHERE id = 1", "1");
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM expression_posts FORCE INDEX (slug_unique) "
        "WHERE slug = 'alpha-post'",
        "0"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM expression_posts FORCE INDEX (slug_unique) WHERE slug = 'gamma-post'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM expression_posts FORCE INDEX (score_total_key) WHERE score_total = 7",
        "1"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE expression_matrix");
    assert_catalog_table_count(filename, "expression_matrix", 1U);
    assert_catalog_table_metadata(
        filename,
        "expression_matrix",
        "expression_posts",
        "InnoDB",
        "MYLITE"
    );
    assert_query_single_value(db, "SELECT slug FROM expression_posts WHERE id = 1", "gamma-post");
    assert_query_single_value(
        db,
        "SELECT display_title FROM expression_posts WHERE id = 1",
        "Gamma Post: Review"
    );
    assert_query_single_value(db, "SELECT score_total FROM expression_posts WHERE id = 2", "12");
    assert_query_single_value(db, "SELECT status_rank FROM expression_posts WHERE id = 2", "1");
    assert_query_single_value(
        db,
        "SELECT id FROM expression_posts FORCE INDEX (slug_unique) WHERE slug = 'gamma-post'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM expression_posts FORCE INDEX (score_total_key) WHERE score_total = 12",
        "2"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO expression_posts (id, title, subtitle, status, rating, bonus) "
        "VALUES (4, 'Delta Post', 'This subtitle is intentionally too long', "
        "'draft', 4, 1)",
        "CONSTRAINT"
    );
    assert_exec_fails_with_message(
        db,
        "INSERT INTO expression_posts (id, title, subtitle, status, rating, bonus) "
        "VALUES (4, 'Gamma Post', NULL, 'draft', 5, 1)",
        "Duplicate"
    );

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void assert_check_constraint_count(
    mylite_db *db,
    const char *schema_name,
    const char *table_name,
    const char *constraint_name,
    unsigned expected_count
) {
    char sql[512];
    char expected_value[32];
    const int sql_written = snprintf(
        sql,
        sizeof(sql),
        "SELECT COUNT(*) FROM INFORMATION_SCHEMA.CHECK_CONSTRAINTS "
        "WHERE CONSTRAINT_SCHEMA = '%s' "
        "AND TABLE_NAME = '%s' "
        "AND CONSTRAINT_NAME = '%s'",
        schema_name,
        table_name,
        constraint_name
    );
    assert(sql_written > 0);
    assert((size_t)sql_written < sizeof(sql));

    const int expected_written =
        snprintf(expected_value, sizeof(expected_value), "%u", expected_count);
    assert(expected_written > 0);
    assert((size_t)expected_written < sizeof(expected_value));
    assert_query_single_value(db, sql, expected_value);
}

static void assert_index_ignored(
    mylite_db *db,
    const char *schema_name,
    const char *table_name,
    const char *index_name,
    const char *expected_ignored
) {
    char sql[512];
    const int written = snprintf(
        sql,
        sizeof(sql),
        "SELECT IGNORED FROM INFORMATION_SCHEMA.STATISTICS "
        "WHERE TABLE_SCHEMA = '%s' "
        "AND TABLE_NAME = '%s' "
        "AND INDEX_NAME = '%s' "
        "AND SEQ_IN_INDEX = 1",
        schema_name,
        table_name,
        index_name
    );
    assert(written > 0);
    assert((size_t)written < sizeof(sql));
    assert_query_single_value(db, sql, expected_ignored);
}

static void assert_query_single_value(mylite_db *db, const char *sql, const char *expected_value) {
    single_value_context value = {
        .expected_value = expected_value,
    };
    char *errmsg = NULL;

    const int result = mylite_exec(db, sql, single_value_callback, &value, &errmsg);
    if (result != MYLITE_OK) {
        fprintf(stderr, "Query failed: %s\n%s\n", sql, errmsg != NULL ? errmsg : "(no error)");
    }
    assert(result == MYLITE_OK);
    assert(errmsg == NULL);
    assert(value.rows == 1);
}

static void assert_warning_message_contains(mylite_db *db, const char *expected_message) {
    const unsigned warning_count = mylite_warning_count(db);
    assert(warning_count >= 1U);

    for (unsigned index = 0U; index < warning_count; ++index) {
        mylite_warning_level level = MYLITE_WARNING_NOTE;
        unsigned code = 0U;
        const char *message = NULL;
        assert(mylite_warning(db, index, &level, &code, &message) == MYLITE_OK);
        (void)level;
        (void)code;

        if (message != NULL && strstr(message, expected_message) != NULL) {
            return;
        }
    }

    fprintf(stderr, "Missing warning containing: %s\n", expected_message);
    for (unsigned index = 0U; index < warning_count; ++index) {
        mylite_warning_level level = MYLITE_WARNING_NOTE;
        unsigned code = 0U;
        const char *message = NULL;
        assert(mylite_warning(db, index, &level, &code, &message) == MYLITE_OK);
        fprintf(
            stderr,
            "Warning %u: level=%u code=%u message=%s\n",
            index,
            (unsigned)level,
            code,
            message != NULL ? message : "(null)"
        );
    }
    assert(0);
}

static void test_truncate_table_lifecycle(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    table_context old_slug_rows = {0};
    table_context primary_rows = {0};
    single_value_context empty_count = {
        .expected_value = "0",
    };
    single_value_context reset_id = {
        .expected_value = "1",
    };
    const char *news_ids[] = {"1", "4"};
    id_sequence_context news_sequence = {
        .expected_count = 2,
        .expected_ids = news_ids,
    };
    id_sequence_context reopened_news_sequence = {
        .expected_count = 2,
        .expected_ids = news_ids,
    };
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE truncate_posts ("
        "id INT NOT NULL AUTO_INCREMENT, "
        "slug VARCHAR(32) NOT NULL, "
        "category VARCHAR(32) NOT NULL, "
        "body TEXT NULL, "
        "PRIMARY KEY (id), "
        "UNIQUE KEY slug_key (slug), "
        "KEY category_key (category)"
        ") ENGINE=InnoDB"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO truncate_posts (slug, category, body) VALUES "
        "('alpha', 'news', 'first'), "
        "('beta', 'tech', REPEAT('before-', 400))"
    );
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "truncate_posts", "InnoDB", "MYLITE");

    assert_exec_succeeds(db, "TRUNCATE TABLE truncate_posts");
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM truncate_posts",
            single_value_callback,
            &empty_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(empty_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM truncate_posts FORCE INDEX (slug_key) WHERE slug = 'alpha'",
            row_callback,
            &old_slug_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(old_slug_rows.rows == 0);

    assert_exec_succeeds(
        db,
        "INSERT INTO truncate_posts (slug, category, body) VALUES "
        "('alpha', 'news', 'after truncate')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO truncate_posts (id, slug, category, body) VALUES "
        "(4, 'beta', 'news', REPEAT('after-', 300))"
    );
    assert_exec_fails(
        db,
        "INSERT INTO truncate_posts (slug, category, body) VALUES "
        "('alpha', 'dupe', NULL)"
    );
    assert(
        mylite_exec(
            db,
            "SELECT id FROM truncate_posts FORCE INDEX (slug_key) WHERE slug = 'alpha'",
            single_value_callback,
            &reset_id,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reset_id.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM truncate_posts FORCE INDEX (PRIMARY) WHERE id = 4",
            row_callback,
            &primary_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(primary_rows.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT id FROM truncate_posts FORCE INDEX (category_key) "
            "WHERE category = 'news' ORDER BY id",
            id_sequence_callback,
            &news_sequence,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(news_sequence.rows == 2);
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "truncate_posts", "InnoDB", "MYLITE");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE app");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM truncate_posts FORCE INDEX (category_key) "
            "WHERE category = 'news' ORDER BY id",
            id_sequence_callback,
            &reopened_news_sequence,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_news_sequence.rows == 2);
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "truncate_posts", "InnoDB", "MYLITE");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_wordpress_shaped_schema(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    single_value_context option_value = {
        .expected_value = "MyLite Site",
    };
    single_value_context postmeta_value = {
        .expected_value = "42",
    };
    single_value_context post_content_length = {
        .expected_value = "1800",
    };
    single_value_context post_title = {
        .expected_value = "Hello world",
    };
    single_value_context user_display_name = {
        .expected_value = "admin",
    };
    single_value_context user_capabilities = {
        .expected_value = "a:1:{s:13:\"administrator\";b:1;}",
    };
    single_value_context term_slug = {
        .expected_value = "uncategorized",
    };
    single_value_context comment_content_length = {
        .expected_value = "160",
    };
    single_value_context commentmeta_value = {
        .expected_value = "5",
    };
    single_value_context link_name = {
        .expected_value = "MyLite",
    };
    single_value_context deleted_commentmeta_count = {
        .expected_value = "0",
    };
    wordpress_post_context published_post = {
        .expected_status = "publish",
    };
    wordpress_join_context joined_postmeta = {0};
    table_context deleted_meta = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(
        db,
        "CREATE DATABASE app DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(db, "USE app");
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_options ("
        "option_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "option_name VARCHAR(191) NOT NULL DEFAULT '', "
        "option_value LONGTEXT NOT NULL, "
        "autoload VARCHAR(20) NOT NULL DEFAULT 'yes', "
        "PRIMARY KEY (option_id), "
        "UNIQUE KEY option_name (option_name), "
        "KEY autoload (autoload)"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_posts ("
        "ID BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "post_author BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "post_date DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "post_content LONGTEXT NOT NULL, "
        "post_title TEXT NOT NULL, "
        "post_status VARCHAR(20) NOT NULL DEFAULT 'publish', "
        "post_name VARCHAR(200) NOT NULL DEFAULT '', "
        "post_modified DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "post_parent BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "guid VARCHAR(255) NOT NULL DEFAULT '', "
        "post_type VARCHAR(20) NOT NULL DEFAULT 'post', "
        "comment_count BIGINT NOT NULL DEFAULT 0, "
        "PRIMARY KEY (ID), "
        "KEY post_name (post_name), "
        "KEY post_title_prefix (post_title(16)), "
        "KEY type_status_date (post_type, post_status, post_date, ID), "
        "KEY post_parent (post_parent), "
        "KEY post_author (post_author)"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_postmeta ("
        "meta_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "post_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "meta_key VARCHAR(255) NULL, "
        "meta_value LONGTEXT NULL, "
        "PRIMARY KEY (meta_id), "
        "KEY post_id (post_id), "
        "KEY meta_key (meta_key(191))"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_users ("
        "ID BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "user_login VARCHAR(60) NOT NULL DEFAULT '', "
        "user_pass VARCHAR(255) NOT NULL DEFAULT '', "
        "user_nicename VARCHAR(50) NOT NULL DEFAULT '', "
        "user_email VARCHAR(100) NOT NULL DEFAULT '', "
        "user_url VARCHAR(100) NOT NULL DEFAULT '', "
        "user_registered DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "user_activation_key VARCHAR(255) NOT NULL DEFAULT '', "
        "user_status INT NOT NULL DEFAULT 0, "
        "display_name VARCHAR(250) NOT NULL DEFAULT '', "
        "PRIMARY KEY (ID), "
        "KEY user_login_key (user_login), "
        "KEY user_nicename (user_nicename), "
        "KEY user_email (user_email)"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_usermeta ("
        "umeta_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "user_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "meta_key VARCHAR(255) NULL, "
        "meta_value LONGTEXT NULL, "
        "PRIMARY KEY (umeta_id), "
        "KEY user_id (user_id), "
        "KEY meta_key (meta_key(191))"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_terms ("
        "term_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "name VARCHAR(200) NOT NULL DEFAULT '', "
        "slug VARCHAR(200) NOT NULL DEFAULT '', "
        "term_group BIGINT NOT NULL DEFAULT 0, "
        "PRIMARY KEY (term_id), "
        "KEY slug (slug(191)), "
        "KEY name (name(191))"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_term_taxonomy ("
        "term_taxonomy_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "term_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "taxonomy VARCHAR(32) NOT NULL DEFAULT '', "
        "description LONGTEXT NOT NULL, "
        "parent BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "count BIGINT NOT NULL DEFAULT 0, "
        "PRIMARY KEY (term_taxonomy_id), "
        "UNIQUE KEY term_id_taxonomy (term_id, taxonomy), "
        "KEY taxonomy (taxonomy)"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_term_relationships ("
        "object_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "term_taxonomy_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "term_order INT NOT NULL DEFAULT 0, "
        "PRIMARY KEY (object_id, term_taxonomy_id), "
        "KEY term_taxonomy_id (term_taxonomy_id)"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_comments ("
        "comment_ID BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "comment_post_ID BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "comment_author TINYTEXT NOT NULL, "
        "comment_author_email VARCHAR(100) NOT NULL DEFAULT '', "
        "comment_author_url VARCHAR(200) NOT NULL DEFAULT '', "
        "comment_author_IP VARCHAR(100) NOT NULL DEFAULT '', "
        "comment_date DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "comment_date_gmt DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "comment_content TEXT NOT NULL, "
        "comment_karma INT NOT NULL DEFAULT 0, "
        "comment_approved VARCHAR(20) NOT NULL DEFAULT '1', "
        "comment_agent VARCHAR(255) NOT NULL DEFAULT '', "
        "comment_type VARCHAR(20) NOT NULL DEFAULT 'comment', "
        "comment_parent BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "user_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "PRIMARY KEY (comment_ID), "
        "KEY comment_post_ID (comment_post_ID), "
        "KEY comment_approved_date_gmt (comment_approved, comment_date_gmt), "
        "KEY comment_date_gmt (comment_date_gmt), "
        "KEY comment_parent (comment_parent), "
        "KEY comment_author_email (comment_author_email(10))"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_commentmeta ("
        "meta_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "comment_id BIGINT UNSIGNED NOT NULL DEFAULT 0, "
        "meta_key VARCHAR(255) NULL, "
        "meta_value LONGTEXT NULL, "
        "PRIMARY KEY (meta_id), "
        "KEY comment_id (comment_id), "
        "KEY meta_key (meta_key(191))"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(
        db,
        "CREATE TABLE wp_links ("
        "link_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "link_url VARCHAR(255) NOT NULL DEFAULT '', "
        "link_name VARCHAR(255) NOT NULL DEFAULT '', "
        "link_image VARCHAR(255) NOT NULL DEFAULT '', "
        "link_target VARCHAR(25) NOT NULL DEFAULT '', "
        "link_description VARCHAR(255) NOT NULL DEFAULT '', "
        "link_visible VARCHAR(20) NOT NULL DEFAULT 'Y', "
        "link_owner BIGINT UNSIGNED NOT NULL DEFAULT 1, "
        "link_rating INT NOT NULL DEFAULT 0, "
        "link_updated DATETIME NOT NULL DEFAULT '0000-00-00 00:00:00', "
        "link_rel VARCHAR(255) NOT NULL DEFAULT '', "
        "link_notes MEDIUMTEXT NOT NULL, "
        "link_rss VARCHAR(255) NOT NULL DEFAULT '', "
        "PRIMARY KEY (link_id), "
        "KEY link_visible (link_visible)"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_wordpress_catalog_metadata(filename);
    assert_table_collation(db, "app", "wp_posts", "utf8mb4_unicode_ci");
    assert_table_collation(db, "app", "wp_comments", "utf8mb4_unicode_ci");

    assert_exec_succeeds(
        db,
        "INSERT INTO wp_options (option_name, option_value, autoload) VALUES "
        "('siteurl', 'https://example.test', 'yes'), "
        "('blogname', 'MyLite Site', 'yes')"
    );
    assert_exec_fails(
        db,
        "INSERT INTO wp_options (option_name, option_value, autoload) VALUES "
        "('blogname', 'Duplicate', 'yes')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_posts ("
        "post_author, post_date, post_content, post_title, post_status, post_name, "
        "post_modified, post_parent, guid, post_type, comment_count"
        ") VALUES ("
        "1, '2026-05-14 12:00:00', REPEAT('hello ', 300), 'Hello world', "
        "'publish', 'hello-world', '2026-05-14 12:01:00', 0, "
        "'https://example.test/?p=1', 'post', 0"
        ")"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_postmeta (post_id, meta_key, meta_value) VALUES "
        "(1, '_thumbnail_id', '42'), "
        "(1, '_edit_lock', '1760000000:1')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_users ("
        "user_login, user_pass, user_nicename, user_email, user_url, user_registered, "
        "user_activation_key, user_status, display_name"
        ") VALUES ("
        "'admin', '$P$hash', 'admin', 'admin@example.test', 'https://example.test', "
        "'2026-05-14 11:00:00', '', 0, 'admin'"
        ")"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_usermeta (user_id, meta_key, meta_value) VALUES "
        "(1, 'wp_capabilities', 'a:1:{s:13:\"administrator\";b:1;}'), "
        "(1, 'wp_user_level', '10')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_terms (name, slug, term_group) VALUES "
        "('Uncategorized', 'uncategorized', 0)"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_term_taxonomy (term_id, taxonomy, description, parent, count) VALUES "
        "(1, 'category', 'Default category', 0, 1)"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_term_relationships (object_id, term_taxonomy_id, term_order) VALUES "
        "(1, 1, 0)"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_comments ("
        "comment_post_ID, comment_author, comment_author_email, comment_author_url, "
        "comment_author_IP, comment_date, comment_date_gmt, comment_content, "
        "comment_karma, comment_approved, comment_agent, comment_type, comment_parent, user_id"
        ") VALUES ("
        "1, 'Jan', 'jan@example.test', '', '127.0.0.1', "
        "'2026-05-14 12:10:00', '2026-05-14 10:10:00', REPEAT('comment ', 20), "
        "0, '1', 'mylite-test', 'comment', 0, 1"
        ")"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_commentmeta (comment_id, meta_key, meta_value) VALUES "
        "(1, '_rating', '5')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_links ("
        "link_url, link_name, link_image, link_target, link_description, link_visible, "
        "link_owner, link_rating, link_updated, link_rel, link_notes, link_rss"
        ") VALUES ("
        "'https://mylite.example', 'MyLite', '', '', 'Embedded database', 'Y', "
        "1, 0, '2026-05-14 12:20:00', '', 'link notes', ''"
        ")"
    );

    assert(
        mylite_exec(
            db,
            "SELECT option_value FROM wp_options FORCE INDEX (option_name) "
            "WHERE option_name = 'blogname'",
            single_value_callback,
            &option_value,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(option_value.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT ID, post_name, post_status FROM wp_posts FORCE INDEX (post_name) "
            "WHERE post_name = 'hello-world'",
            wordpress_post_callback,
            &published_post,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(published_post.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT LENGTH(post_content) FROM wp_posts WHERE ID = 1",
            single_value_callback,
            &post_content_length,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(post_content_length.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT post_title FROM wp_posts FORCE INDEX (post_title_prefix) "
            "WHERE post_title = 'Hello world'",
            single_value_callback,
            &post_title,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(post_title.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT meta_value FROM wp_postmeta FORCE INDEX (post_id) "
            "WHERE post_id = 1 AND meta_key = '_thumbnail_id'",
            single_value_callback,
            &postmeta_value,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(postmeta_value.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT p.ID, LENGTH(p.post_title), p.post_title, m.meta_value "
            "FROM wp_posts p JOIN wp_postmeta m ON m.post_id = p.ID "
            "WHERE p.post_type = 'post' AND p.post_status = 'publish' "
            "AND m.meta_key = '_thumbnail_id'",
            wordpress_join_callback,
            &joined_postmeta,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(joined_postmeta.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT display_name FROM wp_users FORCE INDEX (user_login_key) "
            "WHERE user_login = 'admin'",
            single_value_callback,
            &user_display_name,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(user_display_name.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT meta_value FROM wp_usermeta FORCE INDEX (user_id) "
            "WHERE user_id = 1 AND meta_key = 'wp_capabilities'",
            single_value_callback,
            &user_capabilities,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(user_capabilities.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT t.slug FROM wp_terms t "
            "JOIN wp_term_taxonomy tt ON tt.term_id = t.term_id "
            "JOIN wp_term_relationships tr ON tr.term_taxonomy_id = tt.term_taxonomy_id "
            "WHERE tr.object_id = 1 AND tt.taxonomy = 'category'",
            single_value_callback,
            &term_slug,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(term_slug.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT LENGTH(comment_content) FROM wp_comments FORCE INDEX (comment_post_ID) "
            "WHERE comment_post_ID = 1",
            single_value_callback,
            &comment_content_length,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(comment_content_length.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT meta_value FROM wp_commentmeta FORCE INDEX (comment_id) "
            "WHERE comment_id = 1 AND meta_key = '_rating'",
            single_value_callback,
            &commentmeta_value,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(commentmeta_value.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT link_name FROM wp_links FORCE INDEX (link_visible) "
            "WHERE link_visible = 'Y'",
            single_value_callback,
            &link_name,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(link_name.rows == 1);

    assert_exec_succeeds(
        db,
        "UPDATE wp_options SET option_value = 'Updated MyLite Site' "
        "WHERE option_name = 'blogname'"
    );
    assert_exec_succeeds(
        db,
        "UPDATE wp_posts SET post_status = 'draft' WHERE post_name = 'hello-world'"
    );
    assert_exec_succeeds(db, "UPDATE wp_users SET display_name = 'Jan Admin' WHERE ID = 1");
    assert_exec_succeeds(db, "DELETE FROM wp_postmeta WHERE meta_key = '_edit_lock'");
    assert_exec_succeeds(db, "DELETE FROM wp_commentmeta WHERE meta_key = '_rating'");
    assert(
        mylite_exec(
            db,
            "SELECT meta_id FROM wp_postmeta WHERE meta_key = '_edit_lock'",
            row_callback,
            &deleted_meta,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(deleted_meta.rows == 0);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM wp_commentmeta WHERE meta_key = '_rating'",
            single_value_callback,
            &deleted_commentmeta_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(deleted_commentmeta_count.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    option_value = (single_value_context){
        .expected_value = "Updated MyLite Site",
    };
    post_content_length = (single_value_context){
        .expected_value = "1800",
    };
    post_title = (single_value_context){
        .expected_value = "Hello world",
    };
    published_post = (wordpress_post_context){
        .expected_status = "draft",
    };
    postmeta_value = (single_value_context){
        .expected_value = "42",
    };
    user_display_name = (single_value_context){
        .expected_value = "Jan Admin",
    };
    user_capabilities = (single_value_context){
        .expected_value = "a:1:{s:13:\"administrator\";b:1;}",
    };
    term_slug = (single_value_context){
        .expected_value = "uncategorized",
    };
    comment_content_length = (single_value_context){
        .expected_value = "160",
    };
    link_name = (single_value_context){
        .expected_value = "MyLite",
    };
    deleted_commentmeta_count = (single_value_context){
        .expected_value = "0",
    };
    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE app");
    assert_wordpress_catalog_metadata(filename);
    assert_table_collation(db, "app", "wp_posts", "utf8mb4_unicode_ci");
    assert_table_collation(db, "app", "wp_comments", "utf8mb4_unicode_ci");
    assert(
        mylite_exec(
            db,
            "SELECT option_value FROM wp_options FORCE INDEX (option_name) "
            "WHERE option_name = 'blogname'",
            single_value_callback,
            &option_value,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(option_value.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT ID, post_name, post_status FROM wp_posts FORCE INDEX (post_name) "
            "WHERE post_name = 'hello-world'",
            wordpress_post_callback,
            &published_post,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(published_post.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT LENGTH(post_content) FROM wp_posts WHERE ID = 1",
            single_value_callback,
            &post_content_length,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(post_content_length.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT post_title FROM wp_posts FORCE INDEX (post_title_prefix) "
            "WHERE post_title = 'Hello world'",
            single_value_callback,
            &post_title,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(post_title.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT meta_value FROM wp_postmeta FORCE INDEX (meta_key) "
            "WHERE meta_key = '_thumbnail_id'",
            single_value_callback,
            &postmeta_value,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(postmeta_value.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT display_name FROM wp_users FORCE INDEX (user_login_key) "
            "WHERE user_login = 'admin'",
            single_value_callback,
            &user_display_name,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(user_display_name.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT meta_value FROM wp_usermeta FORCE INDEX (meta_key) "
            "WHERE meta_key = 'wp_capabilities'",
            single_value_callback,
            &user_capabilities,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(user_capabilities.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT t.slug FROM wp_terms t "
            "JOIN wp_term_taxonomy tt ON tt.term_id = t.term_id "
            "JOIN wp_term_relationships tr ON tr.term_taxonomy_id = tt.term_taxonomy_id "
            "WHERE tr.object_id = 1 AND tt.taxonomy = 'category'",
            single_value_callback,
            &term_slug,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(term_slug.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT LENGTH(comment_content) FROM wp_comments FORCE INDEX (comment_post_ID) "
            "WHERE comment_post_ID = 1",
            single_value_callback,
            &comment_content_length,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(comment_content_length.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT link_name FROM wp_links FORCE INDEX (link_visible) "
            "WHERE link_visible = 'Y'",
            single_value_callback,
            &link_name,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(link_name.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM wp_commentmeta WHERE meta_key = '_rating'",
            single_value_callback,
            &deleted_commentmeta_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(deleted_commentmeta_count.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_wordpress_installer_schema_fixture(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(
        db,
        "CREATE DATABASE wordpress_install "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(db, "USE wordpress_install");
    exec_sql_fixture(db, "wordpress-6.9.4-single-site-schema.sql");
    exec_sql_fixture(db, "wordpress-6.9.4-single-site-seed.sql");
    assert_wordpress_installer_catalog_metadata(filename);
    assert_wordpress_installer_seed_data(db);
    assert_table_collation(db, "wordpress_install", "wp_posts", "utf8mb4_unicode_ci");
    assert_table_collation(db, "wordpress_install", "wp_termmeta", "utf8mb4_unicode_ci");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE wordpress_install");
    assert_wordpress_installer_catalog_metadata(filename);
    assert_wordpress_installer_seed_data(db);
    assert_table_collation(db, "wordpress_install", "wp_posts", "utf8mb4_unicode_ci");
    assert_table_collation(db, "wordpress_install", "wp_termmeta", "utf8mb4_unicode_ci");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_wordpress_multisite_global_schema_fixture(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(
        db,
        "CREATE DATABASE wordpress_multisite "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(db, "USE wordpress_multisite");
    exec_sql_fixture(db, "wordpress-6.9.4-multisite-global-schema.sql");
    assert_wordpress_multisite_global_catalog_metadata(filename);
    assert_table_collation(db, "wordpress_multisite", "wp_users", "utf8mb4_unicode_ci");
    assert_table_collation(db, "wordpress_multisite", "wp_signups", "utf8mb4_unicode_ci");

    assert_exec_succeeds(db, "INSERT INTO wp_site (domain, path) VALUES ('example.test', '/')");
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_blogs "
        "(site_id, domain, path, registered, last_updated) VALUES "
        "(1, 'example.test', '/', '2026-05-15 12:00:00', '2026-05-15 12:00:00')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_blogmeta (blog_id, meta_key, meta_value) VALUES "
        "(1, 'public', '1')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_sitemeta (site_id, meta_key, meta_value) VALUES "
        "(1, 'site_name', 'MyLite Network')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_users "
        "(user_login, user_pass, user_nicename, user_email, user_registered, display_name) "
        "VALUES "
        "('admin', '$P$Bfixturehash', 'admin', 'admin@example.test', "
        "'2026-05-15 10:00:00', 'admin')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_usermeta (user_id, meta_key, meta_value) VALUES "
        "(1, 'wp_capabilities', 'a:1:{s:13:\"administrator\";b:1;}')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_registration_log (email, IP, blog_id, date_registered) VALUES "
        "('admin@example.test', '127.0.0.1', 1, '2026-05-15 12:00:00')"
    );
    assert_exec_succeeds(
        db,
        "INSERT INTO wp_signups "
        "(domain, path, title, user_login, user_email, registered, activated, active, "
        "activation_key, meta) VALUES "
        "('example.test', '/', 'MyLite Network', 'admin', 'admin@example.test', "
        "'2026-05-15 12:00:00', '2026-05-15 12:00:00', 1, 'activation', "
        "'a:1:{s:6:\"public\";i:1;}')"
    );
    assert_wordpress_multisite_global_rows(db);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE wordpress_multisite");
    assert_wordpress_multisite_global_catalog_metadata(filename);
    assert_wordpress_multisite_global_rows(db);
    assert_table_collation(db, "wordpress_multisite", "wp_users", "utf8mb4_unicode_ci");
    assert_table_collation(db, "wordpress_multisite", "wp_signups", "utf8mb4_unicode_ci");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_wordpress_multisite_blog_schema_fixture(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(
        db,
        "CREATE DATABASE wordpress_multisite_blog "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(db, "USE wordpress_multisite_blog");
    exec_sql_fixture(db, "wordpress-6.9.4-multisite-global-schema.sql");
    exec_sql_fixture(db, "wordpress-6.9.4-multisite-blog-2-schema.sql");
    assert_wordpress_multisite_blog_catalog_metadata(filename);
    assert_table_collation(db, "wordpress_multisite_blog", "wp_2_posts", "utf8mb4_unicode_ci");
    assert_table_collation(db, "wordpress_multisite_blog", "wp_2_options", "utf8mb4_unicode_ci");
    exec_sql_fixture(db, "wordpress-6.9.4-multisite-network-seed.sql");
    assert_wordpress_multisite_blog_rows(db);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE wordpress_multisite_blog");
    assert_wordpress_multisite_blog_catalog_metadata(filename);
    assert_wordpress_multisite_blog_rows(db);
    assert_table_collation(db, "wordpress_multisite_blog", "wp_2_posts", "utf8mb4_unicode_ci");
    assert_table_collation(db, "wordpress_multisite_blog", "wp_2_options", "utf8mb4_unicode_ci");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_buddypress_component_schema_fixture(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(
        db,
        "CREATE DATABASE buddypress_install "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(db, "USE buddypress_install");
    exec_sql_fixture(db, "buddypress-14.4.0-component-schema.sql");
    assert_buddypress_catalog_metadata(filename);
    assert_table_collation(db, "buddypress_install", "wp_bp_activity", "utf8mb4_unicode_ci");
    assert_table_collation(db, "buddypress_install", "wp_bp_xprofile_fields", "utf8mb4_unicode_ci");
    exec_sql_fixture(db, "buddypress-14.4.0-component-seed.sql");
    assert_buddypress_rows(db);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE buddypress_install");
    assert_buddypress_catalog_metadata(filename);
    assert_buddypress_rows(db);
    assert_table_collation(db, "buddypress_install", "wp_bp_activity", "utf8mb4_unicode_ci");
    assert_table_collation(db, "buddypress_install", "wp_bp_xprofile_fields", "utf8mb4_unicode_ci");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void test_laravel_default_schema_fixture(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_exec_succeeds(
        db,
        "CREATE DATABASE laravel_install "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
    );
    assert_exec_succeeds(db, "USE laravel_install");
    exec_sql_fixture(db, "laravel-13.6.0-default-schema.sql");
    assert_laravel_catalog_metadata(filename);
    assert_table_collation(db, "laravel_install", "users", "utf8mb4_unicode_ci");
    assert_table_collation(db, "laravel_install", "failed_jobs", "utf8mb4_unicode_ci");
    exec_sql_fixture(db, "laravel-13.6.0-default-seed.sql");
    assert_laravel_rows(db);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE laravel_install");
    assert_laravel_catalog_metadata(filename);
    assert_laravel_rows(db);
    assert_table_collation(db, "laravel_install", "users", "utf8mb4_unicode_ci");
    assert_table_collation(db, "laravel_install", "failed_jobs", "utf8mb4_unicode_ci");
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
}

static void assert_wordpress_catalog_metadata(const char *filename) {
    assert_catalog_table_count(filename, "app", 11U);
    assert_catalog_table_metadata(filename, "app", "wp_options", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_postmeta", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_users", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_usermeta", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_terms", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_term_taxonomy", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_term_relationships", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_comments", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_commentmeta", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_links", "InnoDB", "MYLITE");
}

static void assert_wordpress_installer_catalog_metadata(const char *filename) {
    assert_catalog_table_count(filename, "wordpress_install", 12U);
    assert_catalog_table_metadata(filename, "wordpress_install", "wp_users", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "wordpress_install",
        "wp_usermeta",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_install",
        "wp_termmeta",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(filename, "wordpress_install", "wp_terms", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "wordpress_install",
        "wp_term_taxonomy",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_install",
        "wp_term_relationships",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_install",
        "wp_commentmeta",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_install",
        "wp_comments",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(filename, "wordpress_install", "wp_links", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, "wordpress_install", "wp_options", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "wordpress_install",
        "wp_postmeta",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(filename, "wordpress_install", "wp_posts", "DEFAULT", "MYLITE");
}

static void assert_wordpress_installer_seed_data(mylite_db *db) {
    single_value_context blogname = {
        .expected_value = "MyLite Fixture",
    };
    single_value_context db_version = {
        .expected_value = "60717",
    };
    single_value_context admin_email = {
        .expected_value = "admin@example.test",
    };
    single_value_context default_role = {
        .expected_value = "subscriber",
    };
    single_value_context default_options_count = {
        .expected_value = "9",
    };
    single_value_context role_payload_count = {
        .expected_value = "1",
    };
    single_value_context admin_capabilities = {
        .expected_value = "a:1:{s:13:\"administrator\";b:1;}",
    };
    single_value_context widget_block = {
        .expected_value = "a:1:{i:2;a:1:{s:7:\"content\";s:19:\"<!-- wp:search /-->\";}}",
    };
    single_value_context term_slug = {
        .expected_value = "uncategorized",
    };
    single_value_context post_title = {
        .expected_value = "Hello world!",
    };
    single_value_context comment_author = {
        .expected_value = "A WordPress Commenter",
    };
    single_value_context page_template = {
        .expected_value = "default",
    };
    char *errmsg = NULL;

    assert(
        mylite_exec(
            db,
            "SELECT option_value FROM wp_options FORCE INDEX (option_name) "
            "WHERE option_name = 'blogname'",
            single_value_callback,
            &blogname,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(blogname.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT option_value FROM wp_options FORCE INDEX (option_name) "
            "WHERE option_name = 'db_version'",
            single_value_callback,
            &db_version,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(db_version.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT user_email FROM wp_users FORCE INDEX (user_login_key) "
            "WHERE user_login = 'admin'",
            single_value_callback,
            &admin_email,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(admin_email.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT option_value FROM wp_options FORCE INDEX (option_name) "
            "WHERE option_name = 'default_role'",
            single_value_callback,
            &default_role,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(default_role.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM wp_options WHERE "
            "(option_name = 'users_can_register' AND option_value = '0') OR "
            "(option_name = 'start_of_week' AND option_value = '1') OR "
            "(option_name = 'use_smilies' AND option_value = '1') OR "
            "(option_name = 'comments_notify' AND option_value = '1') OR "
            "(option_name = 'posts_per_page' AND option_value = '10') OR "
            "(option_name = 'active_plugins' AND option_value = 'a:0:{}') OR "
            "(option_name = 'sticky_posts' AND option_value = 'a:0:{}') OR "
            "(option_name = 'comment_previously_approved' AND option_value = '1') OR "
            "(option_name = 'auto_update_core_major' AND option_value = 'enabled')",
            single_value_callback,
            &default_options_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(default_options_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM wp_options FORCE INDEX (option_name) "
            "WHERE option_name = 'wp_user_roles' AND LENGTH(option_value) > 3000 "
            "AND LOCATE('s:13:\"administrator\"', option_value) > 0 "
            "AND LOCATE('s:6:\"editor\"', option_value) > 0 "
            "AND LOCATE('s:6:\"author\"', option_value) > 0 "
            "AND LOCATE('s:11:\"contributor\"', option_value) > 0 "
            "AND LOCATE('s:10:\"subscriber\"', option_value) > 0 "
            "AND LOCATE('s:14:\"update_plugins\";b:1;', option_value) > 0",
            single_value_callback,
            &role_payload_count,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(role_payload_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT meta_value FROM wp_usermeta FORCE INDEX (meta_key) "
            "WHERE meta_key = 'wp_capabilities'",
            single_value_callback,
            &admin_capabilities,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(admin_capabilities.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT option_value FROM wp_options FORCE INDEX (option_name) "
            "WHERE option_name = 'widget_block'",
            single_value_callback,
            &widget_block,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(widget_block.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT t.slug FROM wp_terms t "
            "JOIN wp_term_taxonomy tt ON tt.term_id = t.term_id "
            "JOIN wp_term_relationships tr ON tr.term_taxonomy_id = tt.term_taxonomy_id "
            "WHERE tr.object_id = 1 AND tt.taxonomy = 'category'",
            single_value_callback,
            &term_slug,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(term_slug.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT post_title FROM wp_posts FORCE INDEX (post_name) "
            "WHERE post_name = 'hello-world'",
            single_value_callback,
            &post_title,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(post_title.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT comment_author FROM wp_comments FORCE INDEX (comment_post_ID) "
            "WHERE comment_post_ID = 1",
            single_value_callback,
            &comment_author,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(comment_author.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT meta_value FROM wp_postmeta FORCE INDEX (post_id) "
            "WHERE post_id = 2 AND meta_key = '_wp_page_template'",
            single_value_callback,
            &page_template,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(page_template.rows == 1);
}

static void assert_wordpress_multisite_global_catalog_metadata(const char *filename) {
    assert_catalog_table_count(filename, "wordpress_multisite", 8U);
    assert_wordpress_multisite_global_table_metadata(filename, "wordpress_multisite");
}

static void assert_wordpress_multisite_global_rows(mylite_db *db) {
    assert_query_single_value(
        db,
        "SELECT id FROM wp_site FORCE INDEX (domain) "
        "WHERE domain = 'example.test' AND path = '/'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT blog_id FROM wp_blogs FORCE INDEX (domain) "
        "WHERE domain = 'example.test' AND path = '/'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_blogmeta FORCE INDEX (meta_key) WHERE meta_key = 'public'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_sitemeta FORCE INDEX (meta_key) WHERE meta_key = 'site_name'",
        "MyLite Network"
    );
    assert_query_single_value(
        db,
        "SELECT user_email FROM wp_users FORCE INDEX (user_login_key) WHERE user_login = 'admin'",
        "admin@example.test"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_usermeta FORCE INDEX (meta_key) "
        "WHERE meta_key = 'wp_capabilities'",
        "a:1:{s:13:\"administrator\";b:1;}"
    );
    assert_query_single_value(
        db,
        "SELECT ID FROM wp_registration_log FORCE INDEX (IP) WHERE IP = '127.0.0.1'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT signup_id FROM wp_signups FORCE INDEX (user_login_email) "
        "WHERE user_login = 'admin' AND user_email = 'admin@example.test'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM wp_users WHERE spam = 0 AND deleted = 0",
        "1"
    );
}

static void assert_wordpress_multisite_blog_catalog_metadata(const char *filename) {
    assert_catalog_table_count(filename, "wordpress_multisite_blog", 18U);
    assert_wordpress_multisite_global_table_metadata(filename, "wordpress_multisite_blog");
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_termmeta",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_terms",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_term_taxonomy",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_term_relationships",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_commentmeta",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_comments",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_links",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_options",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_postmeta",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(
        filename,
        "wordpress_multisite_blog",
        "wp_2_posts",
        "DEFAULT",
        "MYLITE"
    );
}

static void assert_wordpress_multisite_global_table_metadata(
    const char *filename,
    const char *schema_name
) {
    assert_catalog_table_metadata(filename, schema_name, "wp_users", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, schema_name, "wp_usermeta", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, schema_name, "wp_blogs", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, schema_name, "wp_blogmeta", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        schema_name,
        "wp_registration_log",
        "DEFAULT",
        "MYLITE"
    );
    assert_catalog_table_metadata(filename, schema_name, "wp_site", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, schema_name, "wp_sitemeta", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, schema_name, "wp_signups", "DEFAULT", "MYLITE");
}

static void assert_wordpress_multisite_blog_rows(mylite_db *db) {
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_sitemeta FORCE INDEX (meta_key) WHERE meta_key = 'site_name'",
        "MyLite Network"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_sitemeta FORCE INDEX (meta_key) WHERE meta_key = 'site_admins'",
        "a:1:{i:0;s:13:\"network-admin\";}"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_usermeta FORCE INDEX (meta_key) WHERE meta_key = 'primary_blog'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT blog_id FROM wp_blogs FORCE INDEX (domain) "
        "WHERE domain = 'example.test' AND path = '/second/'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT option_value FROM wp_2_options FORCE INDEX (option_name) "
        "WHERE option_name = 'blogname'",
        "Second Site"
    );
    assert_query_single_value(
        db,
        "SELECT post_title FROM wp_2_posts FORCE INDEX (post_name) "
        "WHERE post_name = 'second-site-post'",
        "Second Site Post"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_2_postmeta FORCE INDEX (post_id) "
        "WHERE post_id = 1 AND meta_key = '_thumbnail_id'",
        "84"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_2_termmeta FORCE INDEX (term_id) "
        "WHERE term_id = 1 AND meta_key = 'display'",
        "featured"
    );
    assert_query_single_value(
        db,
        "SELECT t.slug FROM wp_2_terms t "
        "JOIN wp_2_term_taxonomy tt ON tt.term_id = t.term_id "
        "JOIN wp_2_term_relationships tr ON tr.term_taxonomy_id = tt.term_taxonomy_id "
        "WHERE tr.object_id = 1 AND tt.taxonomy = 'category'",
        "network-news"
    );
    assert_query_single_value(
        db,
        "SELECT comment_author FROM wp_2_comments FORCE INDEX (comment_post_ID) "
        "WHERE comment_post_ID = 1",
        "Jan"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_2_commentmeta FORCE INDEX (comment_id) "
        "WHERE comment_id = 1 AND meta_key = '_rating'",
        "5"
    );
    assert_query_single_value(
        db,
        "SELECT link_name FROM wp_2_links FORCE INDEX (link_visible) WHERE link_visible = 'Y'",
        "Second Site Link"
    );
    assert_query_single_value(
        db,
        "SELECT COUNT(*) FROM wp_2_terms FORCE INDEX (slug) WHERE slug = 'network-news'",
        "1"
    );
}

static void assert_buddypress_catalog_metadata(const char *filename) {
    static const char *const table_names[] = {
        "wp_bp_notifications",     "wp_bp_notifications_meta",
        "wp_bp_activity",          "wp_bp_activity_meta",
        "wp_bp_friends",           "wp_bp_groups",
        "wp_bp_groups_members",    "wp_bp_groups_groupmeta",
        "wp_bp_messages_messages", "wp_bp_messages_recipients",
        "wp_bp_messages_notices",  "wp_bp_messages_meta",
        "wp_bp_xprofile_groups",   "wp_bp_xprofile_fields",
        "wp_bp_xprofile_data",     "wp_bp_xprofile_meta",
        "wp_bp_user_blogs",        "wp_bp_user_blogs_blogmeta",
        "wp_bp_invitations",       "wp_bp_optouts",
    };

    assert_catalog_table_count(filename, "buddypress_install", 20U);
    for (size_t i = 0; i < sizeof(table_names) / sizeof(table_names[0]); ++i) {
        assert_buddypress_table_metadata(filename, "buddypress_install", table_names[i]);
    }
}

static void assert_buddypress_table_metadata(
    const char *filename,
    const char *schema_name,
    const char *table_name
) {
    assert_catalog_table_metadata(filename, schema_name, table_name, "DEFAULT", "MYLITE");
}

static void assert_buddypress_rows(mylite_db *db) {
    assert_query_single_value(
        db,
        "SELECT component_action FROM wp_bp_notifications FORCE INDEX (useritem) "
        "WHERE user_id = 10 AND is_new = 1",
        "membership_request"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_bp_notifications_meta FORCE INDEX (notification_id) "
        "WHERE notification_id = 1 AND meta_key = 'source'",
        "fixture"
    );
    assert_query_single_value(
        db,
        "SELECT content FROM wp_bp_activity FORCE INDEX (user_id) "
        "WHERE user_id = 10 AND component = 'activity'",
        "BuddyPress fixture activity"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_bp_activity_meta FORCE INDEX (activity_id) "
        "WHERE activity_id = 1 AND meta_key = 'favorite_count'",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT friend_user_id FROM wp_bp_friends FORCE INDEX (initiator_user_id) "
        "WHERE initiator_user_id = 10",
        "11"
    );
    assert_query_single_value(
        db,
        "SELECT name FROM wp_bp_groups FORCE INDEX (status) WHERE status = 'public'",
        "Fixture Group"
    );
    assert_query_single_value(
        db,
        "SELECT user_title FROM wp_bp_groups_members FORCE INDEX (group_id) "
        "WHERE group_id = 1 AND user_id = 10",
        "Admin"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_bp_groups_groupmeta FORCE INDEX (meta_key) "
        "WHERE meta_key = 'last_activity'",
        "2026-05-15 12:17:00"
    );
    assert_query_single_value(
        db,
        "SELECT subject FROM wp_bp_messages_messages FORCE INDEX (thread_id) "
        "WHERE thread_id = 501",
        "Fixture message"
    );
    assert_query_single_value(
        db,
        "SELECT unread_count FROM wp_bp_messages_recipients FORCE INDEX (user_id) "
        "WHERE user_id = 11",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT subject FROM wp_bp_messages_notices FORCE INDEX (is_active) WHERE is_active = 1",
        "Fixture notice"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_bp_messages_meta FORCE INDEX (message_id) "
        "WHERE message_id = 1 AND meta_key = 'priority'",
        "high"
    );
    assert_query_single_value(
        db,
        "SELECT name FROM wp_bp_xprofile_groups FORCE INDEX (can_delete) WHERE can_delete = 0",
        "General"
    );
    assert_query_single_value(
        db,
        "SELECT name FROM wp_bp_xprofile_fields FORCE INDEX (group_id) WHERE group_id = 1",
        "Display Name"
    );
    assert_query_single_value(
        db,
        "SELECT value FROM wp_bp_xprofile_data FORCE INDEX (user_id) WHERE user_id = 10",
        "Jan Fixture"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_bp_xprofile_meta FORCE INDEX (meta_key) "
        "WHERE meta_key = 'allow_custom_visibility'",
        "disabled"
    );
    assert_query_single_value(
        db,
        "SELECT blog_id FROM wp_bp_user_blogs FORCE INDEX (user_id) WHERE user_id = 10",
        "2"
    );
    assert_query_single_value(
        db,
        "SELECT meta_value FROM wp_bp_user_blogs_blogmeta FORCE INDEX (meta_key) "
        "WHERE meta_key = 'last_activity'",
        "2026-05-15 12:35:00"
    );
    assert_query_single_value(
        db,
        "SELECT invitee_email FROM wp_bp_invitations FORCE INDEX (inviter_id) "
        "WHERE inviter_id = 10",
        "invitee@example.test"
    );
    assert_query_single_value(
        db,
        "SELECT email_address_hash FROM wp_bp_optouts FORCE INDEX (email_type) "
        "WHERE email_type = 'members-invitation'",
        "hash-fixture"
    );
}

static void assert_laravel_catalog_metadata(const char *filename) {
    static const char *const table_names[] = {
        "users",
        "password_reset_tokens",
        "sessions",
        "cache",
        "cache_locks",
        "jobs",
        "job_batches",
        "failed_jobs",
    };

    assert_catalog_table_count(filename, "laravel_install", 8U);
    for (size_t i = 0; i < sizeof(table_names) / sizeof(table_names[0]); ++i) {
        assert_laravel_table_metadata(filename, "laravel_install", table_names[i]);
    }
}

static void assert_laravel_table_metadata(
    const char *filename,
    const char *schema_name,
    const char *table_name
) {
    assert_catalog_table_metadata(filename, schema_name, table_name, "DEFAULT", "MYLITE");
}

static void assert_laravel_rows(mylite_db *db) {
    assert_query_single_value(
        db,
        "SELECT name FROM users FORCE INDEX (users_email_unique) "
        "WHERE email = 'jan@example.test'",
        "Jan Fixture"
    );
    assert_query_single_value(
        db,
        "SELECT token FROM password_reset_tokens FORCE INDEX (PRIMARY) "
        "WHERE email = 'jan@example.test'",
        "reset-token-fixture"
    );
    assert_query_single_value(
        db,
        "SELECT id FROM sessions FORCE INDEX (sessions_user_id_index) WHERE user_id = 1",
        "session-fixture"
    );
    assert_query_single_value(
        db,
        "SELECT value FROM `cache` FORCE INDEX (PRIMARY) "
        "WHERE `key` = 'laravel_cache_fixture'",
        "fixture-cache-value"
    );
    assert_query_single_value(
        db,
        "SELECT owner FROM cache_locks FORCE INDEX (PRIMARY) "
        "WHERE `key` = 'laravel_lock_fixture'",
        "fixture-owner"
    );
    assert_query_single_value(
        db,
        "SELECT attempts FROM jobs FORCE INDEX (jobs_queue_index) WHERE queue = 'default'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT pending_jobs FROM job_batches FORCE INDEX (PRIMARY) WHERE id = 'batch-fixture'",
        "1"
    );
    assert_query_single_value(
        db,
        "SELECT connection FROM failed_jobs FORCE INDEX (failed_jobs_uuid_unique) "
        "WHERE uuid = 'failed-job-fixture'",
        "database"
    );
    assert_query_single_value(
        db,
        "SELECT uuid FROM failed_jobs FORCE INDEX (failed_jobs_connection_queue_failed_at_index) "
        "WHERE connection = 'database' AND queue = 'default'",
        "failed-job-fixture"
    );
}

static void assert_table_collation(
    mylite_db *db,
    const char *schema_name,
    const char *table_name,
    const char *expected_collation
) {
    char sql[320];
    const int written = snprintf(
        sql,
        sizeof(sql),
        "SELECT TABLE_COLLATION FROM INFORMATION_SCHEMA.TABLES "
        "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s'",
        schema_name,
        table_name
    );
    assert(written > 0);
    assert((size_t)written < sizeof(sql));

    single_value_context table_collation = {
        .expected_value = expected_collation,
    };
    char *errmsg = NULL;
    assert(mylite_exec(db, sql, single_value_callback, &table_collation, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(table_collation.rows == 1);
}

static void exec_sql_fixture(mylite_db *db, const char *fixture_name) {
    char *path = path_join(MYLITE_TEST_FIXTURE_DIR, fixture_name);
    char *sql = read_text_file(path);
    char *statement_start = sql;
    int quote = 0;
    int line_comment = 0;
    int escaped = 0;

    for (char *cursor = sql; *cursor != '\0'; ++cursor) {
        if (!sql_fixture_cursor_is_separator(&cursor, &quote, &line_comment, &escaped)) {
            continue;
        }

        *cursor = '\0';
        exec_sql_statement_if_present(db, statement_start);
        statement_start = cursor + 1;
    }
    assert(is_sql_statement_empty(statement_start));

    free(sql);
    free(path);
}

static int sql_fixture_cursor_is_separator(
    char **cursor,
    int *quote,
    int *line_comment,
    int *escaped
) {
    assert(cursor != NULL);
    assert(*cursor != NULL);
    assert(quote != NULL);
    assert(line_comment != NULL);
    assert(escaped != NULL);

    char *current = *cursor;

    if (*line_comment) {
        if (*current == '\n' || *current == '\r') {
            *line_comment = 0;
        }
        return 0;
    }

    if (*quote != 0) {
        if (*escaped) {
            *escaped = 0;
            return 0;
        }
        if (*current == '\\' && current[1] != '\0') {
            *escaped = 1;
            return 0;
        }
        if (*current == *quote) {
            if (current[1] == *quote) {
                *cursor = current + 1;
                return 0;
            }
            *quote = 0;
        }
        return 0;
    }

    if (*current == '-' && current[1] == '-' && isspace((unsigned char)current[2])) {
        *line_comment = 1;
        return 0;
    }
    if (*current == '#') {
        *line_comment = 1;
        return 0;
    }
    if (*current == '\'' || *current == '"') {
        *quote = *current;
        *escaped = 0;
        return 0;
    }

    return *current == ';';
}

static void exec_sql_statement_if_present(mylite_db *db, const char *statement) {
    if (is_sql_statement_empty(statement)) {
        return;
    }

    assert_exec_succeeds(db, statement);
}

static int is_sql_statement_empty(const char *statement) {
    while (*statement != '\0') {
        if (!isspace((unsigned char)*statement)) {
            return 0;
        }
        ++statement;
    }
    return 1;
}

static char *read_text_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    const long file_size = ftell(file);
    assert(file_size >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);

    char *contents = (char *)malloc((size_t)file_size + 1U);
    assert(contents != NULL);
    assert(fread(contents, 1U, (size_t)file_size, file) == (size_t)file_size);
    contents[file_size] = '\0';
    assert(fclose(file) == 0);
    return contents;
}

static void assert_exec_succeeds(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result != MYLITE_OK) {
        fprintf(stderr, "SQL failed: %s\n%s\n", sql, errmsg != NULL ? errmsg : "(no error)");
    }
    assert(result == MYLITE_OK);
    assert(errmsg == NULL);
}

static void assert_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result != MYLITE_OK);
    assert(errmsg != NULL);
    mylite_free(errmsg);
}

static void assert_exec_fails_with_message(mylite_db *db, const char *sql, const char *message) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result != MYLITE_OK);
    assert(errmsg != NULL);
    assert(strstr(errmsg, message) != NULL);
    mylite_free(errmsg);
}

static void assert_non_table_object_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "non-table database object") != NULL);
    mylite_free(errmsg);
}

static void assert_transaction_control_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "transaction control") != NULL);
    mylite_free(errmsg);
}

static void assert_transaction_crash_recovery(const char *root, const char *filename) {
    char *transaction_journal_filename = transaction_journal_path(filename);
    char *child_runtime_root = path_join(root, "crash-runtime");
    const pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        execl(
            g_test_program_path,
            g_test_program_path,
            "--transaction-crash-child",
            root,
            filename,
            (char *)NULL
        );
        _exit(127);
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(access(transaction_journal_filename, F_OK) == 0);

    mylite_db *db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE tx_app");
    single_value_context zero_count = {.expected_value = "0"};
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) FROM tx_posts WHERE title = 'crash-rolled-back'",
            single_value_callback,
            &zero_count,
            NULL
        ) == MYLITE_OK
    );
    assert(zero_count.rows == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert(access(transaction_journal_filename, F_OK) != 0);
    assert(errno == ENOENT);

    remove_tree_entry(child_runtime_root);
    free(child_runtime_root);
    free(transaction_journal_filename);
}

static void assert_locking_sql_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "SQL locking") != NULL);
    mylite_free(errmsg);
}

static void assert_online_alter_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "online ALTER") != NULL);
    mylite_free(errmsg);
}

static void assert_csv_engine_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "CSV storage engine") != NULL);
    mylite_free(errmsg);
}

static void assert_unsupported_engine_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "storage engine request") != NULL);
    mylite_free(errmsg);
}

static void assert_partition_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "partition") != NULL);
    mylite_free(errmsg);
}

static void assert_foreign_key_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "foreign-key") != NULL);
    mylite_free(errmsg);
}

static void assert_prepared_succeeds(mylite_db *db, const char *sql) {
    mylite_stmt *stmt = NULL;
    const int prepare_result = mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, NULL);
    if (prepare_result != MYLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n%s\n", sql, mylite_errmsg(db));
    }
    assert(prepare_result == MYLITE_OK);
    assert(stmt != NULL);

    const int step_result = mylite_step(stmt);
    if (step_result != MYLITE_DONE) {
        fprintf(stderr, "Prepared SQL failed: %s\n%s\n", sql, mylite_errmsg(db));
    }
    assert(step_result == MYLITE_DONE);
    assert(mylite_finalize(stmt) == MYLITE_OK);
}

static void assert_prepared_fails(mylite_db *db, const char *sql) {
    mylite_stmt *stmt = NULL;
    const int prepare_result = mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, NULL);
    if (prepare_result != MYLITE_OK) {
        fprintf(stderr, "Prepare failed before execution: %s\n%s\n", sql, mylite_errmsg(db));
    }
    assert(prepare_result == MYLITE_OK);
    assert(stmt != NULL);

    const int step_result = mylite_step(stmt);
    if (step_result != MYLITE_ERROR) {
        fprintf(stderr, "Prepared SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(step_result == MYLITE_ERROR);
    assert(mylite_finalize(stmt) == MYLITE_OK);
}

static void assert_prepared_fails_with_message(
    mylite_db *db,
    const char *sql,
    const char *message
) {
    mylite_stmt *stmt = NULL;
    mylite_warning_level warning_level = MYLITE_WARNING_NOTE;
    unsigned warning_code = 0U;
    const char *warning_message = NULL;
    const int prepare_result = mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, NULL);
    if (prepare_result != MYLITE_OK) {
        fprintf(stderr, "Prepare failed before execution: %s\n%s\n", sql, mylite_errmsg(db));
    }
    assert(prepare_result == MYLITE_OK);
    assert(stmt != NULL);

    const int step_result = mylite_step(stmt);
    if (step_result != MYLITE_ERROR) {
        fprintf(stderr, "Prepared SQL unexpectedly succeeded: %s\n", sql);
    }
    assert(step_result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    const unsigned mariadb_errno = mylite_mariadb_errno(db);
    assert(mariadb_errno != 0U);
    assert(strcmp(mylite_sqlstate(db), "00000") != 0);
    assert(strstr(mylite_errmsg(db), message) != NULL);
    assert(mylite_warning_count(db) >= 1U);
    assert(mylite_warning(db, 0U, &warning_level, &warning_code, &warning_message) == MYLITE_OK);
    assert(warning_level == MYLITE_WARNING_ERROR);
    assert(warning_code == mariadb_errno);
    assert(warning_message != NULL);
    assert(strstr(warning_message, message) != NULL);
    assert(mylite_finalize(stmt) == MYLITE_OK);
}

static void assert_prepared_policy_fails_with_message(
    mylite_db *db,
    mylite_stmt *stmt,
    const char *message
) {
    const int step_result = mylite_step(stmt);
    if (step_result != MYLITE_ERROR) {
        fprintf(stderr, "Prepared SQL unexpectedly succeeded\n");
    }
    assert(step_result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), message) != NULL);
}

static void assert_schema_options(
    mylite_db *db,
    const char *schema_name,
    const char *expected_character_set,
    const char *expected_collation,
    const char *expected_comment
) {
    schema_option_context options = {
        .expected_character_set = expected_character_set,
        .expected_collation = expected_collation,
        .expected_comment = expected_comment,
    };
    char sql[512];
    char *errmsg = NULL;
    const int written = snprintf(
        sql,
        sizeof(sql),
        "SELECT DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME, SCHEMA_COMMENT "
        "FROM INFORMATION_SCHEMA.SCHEMATA WHERE SCHEMA_NAME='%s'",
        schema_name
    );
    assert(written > 0);
    assert((size_t)written < sizeof(sql));

    assert(mylite_exec(db, sql, schema_option_callback, &options, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(options.rows == 1);
}

static void assert_show_create_schema(
    mylite_db *db,
    const char *schema_name,
    const char *expected_character_set,
    const char *expected_collation
) {
    show_create_schema_context show_create = {
        .expected_schema_name = schema_name,
        .expected_character_set = expected_character_set,
        .expected_collation = expected_collation,
    };
    char sql[256];
    char *errmsg = NULL;
    const int written = snprintf(sql, sizeof(sql), "SHOW CREATE DATABASE %s", schema_name);
    assert(written > 0);
    assert((size_t)written < sizeof(sql));

    assert(mylite_exec(db, sql, show_create_schema_callback, &show_create, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(show_create.rows == 1);
}

static char *capture_show_create_table(mylite_db *db, const char *table_name) {
    char sql[256];
    const int written = snprintf(sql, sizeof(sql), "SHOW CREATE TABLE %s", table_name);
    assert(written > 0);
    assert((size_t)written < sizeof(sql));

    show_create_table_context ctx = {
        .expected_table_name = table_name,
    };
    char *errmsg = NULL;
    assert(mylite_exec(db, sql, show_create_table_callback, &ctx, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(ctx.rows == 1);
    assert(ctx.create_sql != NULL);
    return ctx.create_sql;
}

static void assert_catalog_table_count(
    const char *filename,
    const char *schema_name,
    unsigned count
) {
    catalog_table_context ctx = {
        .expected_schema_name = schema_name,
    };

    assert(
        mylite_storage_list_tables(filename, schema_name, catalog_table_callback, &ctx) ==
        MYLITE_STORAGE_OK
    );
    if (ctx.count != count) {
        fprintf(
            stderr,
            "Catalog table count mismatch for schema %s: expected %u, got %u\n",
            schema_name,
            count,
            ctx.count
        );
    }
    assert(ctx.count == count);
}

static void assert_catalog_table_metadata(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const char *requested_engine_name,
    const char *effective_engine_name
) {
    mylite_storage_table_metadata metadata = {
        .size = sizeof(metadata),
    };

    assert(
        mylite_storage_read_table_metadata(filename, schema_name, table_name, &metadata) ==
        MYLITE_STORAGE_OK
    );
    assert(strcmp(metadata.requested_engine_name, requested_engine_name) == 0);
    assert(strcmp(metadata.effective_engine_name, effective_engine_name) == 0);
    mylite_storage_free(metadata.requested_engine_name);
    mylite_storage_free(metadata.effective_engine_name);
}

static int engine_callback(void *ctx, int column_count, char **values, char **column_names) {
    engine_context *engine_ctx = (engine_context *)ctx;
    (void)column_names;

    assert(column_count >= 2);
    ++engine_ctx->rows;

    if (values[0] == NULL) {
        return 0;
    }

    if (strcmp(values[0], "SEQUENCE") == 0) {
        engine_ctx->found_sequence = 1;
        return 0;
    }

    if (strcmp(values[0], "MYLITE") != 0) {
        return 0;
    }

    engine_ctx->found_mylite = 1;
    if (values[1] != NULL && (strcmp(values[1], "YES") == 0 || strcmp(values[1], "DEFAULT") == 0)) {
        engine_ctx->supported_mylite = 1;
    }

    return 0;
}

static int schema_callback(void *ctx, int column_count, char **values, char **column_names) {
    schema_context *schema_ctx = (schema_context *)ctx;
    (void)column_names;

    assert(column_count == 1);
    if (values[0] == NULL) {
        return 0;
    }
    if (strcmp(values[0], "app") == 0) {
        schema_ctx->found_app = 1;
        ++schema_ctx->rows;
    } else if (strcmp(values[0], "empty_blog") == 0) {
        schema_ctx->found_empty_blog = 1;
        ++schema_ctx->rows;
    }
    return 0;
}

static int schema_option_callback(void *ctx, int column_count, char **values, char **column_names) {
    schema_option_context *option_ctx = (schema_option_context *)ctx;
    (void)column_names;

    assert(column_count == 3);
    assert(values[0] != NULL);
    assert(values[1] != NULL);
    assert(values[2] != NULL);
    if (strcmp(values[0], option_ctx->expected_character_set) != 0 ||
        strcmp(values[1], option_ctx->expected_collation) != 0 ||
        strcmp(values[2], option_ctx->expected_comment) != 0) {
        fprintf(
            stderr,
            "unexpected schema options: charset='%s' collation='%s' comment='%s'\n",
            values[0],
            values[1],
            values[2]
        );
    }
    assert(strcmp(values[0], option_ctx->expected_character_set) == 0);
    assert(strcmp(values[1], option_ctx->expected_collation) == 0);
    assert(strcmp(values[2], option_ctx->expected_comment) == 0);
    ++option_ctx->rows;
    return 0;
}

static int show_create_schema_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    show_create_schema_context *show_create_ctx = (show_create_schema_context *)ctx;
    (void)column_names;

    assert(column_count == 2);
    assert(show_create_ctx->expected_schema_name != NULL);
    assert(values[0] != NULL && strcmp(values[0], show_create_ctx->expected_schema_name) == 0);
    assert(values[1] != NULL);
    assert(strstr(values[1], show_create_ctx->expected_character_set) != NULL);
    assert(strstr(values[1], show_create_ctx->expected_collation) != NULL);
    ++show_create_ctx->rows;
    return 0;
}

static int show_create_table_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    show_create_table_context *show_ctx = (show_create_table_context *)ctx;
    (void)column_names;

    assert(column_count == 2);
    assert(values[0] != NULL);
    assert(values[1] != NULL);
    assert(show_ctx->expected_table_name != NULL);
    assert(show_ctx->rows == 0);
    assert(strcmp(values[0], show_ctx->expected_table_name) == 0);
    show_ctx->create_sql = strdup(values[1]);
    assert(show_ctx->create_sql != NULL);
    ++show_ctx->rows;
    return 0;
}

static int table_callback(void *ctx, int column_count, char **values, char **column_names) {
    table_context *table_ctx = (table_context *)ctx;
    (void)column_names;

    assert(column_count >= 1);
    ++table_ctx->rows;
    assert(values[0] != NULL);
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

static int post_row_callback(void *ctx, int column_count, char **values, char **column_names) {
    post_row_context *row_ctx = (post_row_context *)ctx;
    (void)column_names;

    assert(column_count == 2);
    assert(values[0] != NULL);
    assert(values[1] != NULL);
    ++row_ctx->rows;
    if (strcmp(values[0], "1") == 0 && strcmp(values[1], "draft") == 0) {
        row_ctx->found_draft = 1;
        return 0;
    }
    if (strcmp(values[0], "2") == 0 && strcmp(values[1], "published") == 0) {
        row_ctx->found_published = 1;
        return 0;
    }

    assert(0);
    return 1;
}

static int nullable_row_callback(void *ctx, int column_count, char **values, char **column_names) {
    nullable_row_context *row_ctx = (nullable_row_context *)ctx;
    (void)column_names;

    assert(column_count == 3);
    assert(values[0] != NULL);
    ++row_ctx->rows;
    if (strcmp(values[0], "1") == 0) {
        assert(values[1] == NULL);
        assert(values[2] == NULL);
        row_ctx->found_null_row = 1;
        return 0;
    }
    if (strcmp(values[0], "2") == 0) {
        assert(values[1] != NULL);
        assert(values[2] != NULL);
        assert(strcmp(values[1], "filled") == 0);
        assert(strcmp(values[2], "42") == 0);
        row_ctx->found_value_row = 1;
        return 0;
    }

    assert(0);
    return 1;
}

static int blob_row_callback(void *ctx, int column_count, char **values, char **column_names) {
    blob_row_context *row_ctx = (blob_row_context *)ctx;
    (void)column_names;

    assert(column_count == 7);
    assert(values[0] != NULL);
    ++row_ctx->rows;
    if (strcmp(values[0], "1") == 0) {
        assert(strcmp(values[1], "0") == 0);
        assert(values[2] != NULL && strcmp(values[2], "10") == 0);
        assert(values[3] != NULL && strcmp(values[3], "short text") == 0);
        assert(strcmp(values[4], "0") == 0);
        assert(values[5] != NULL && strcmp(values[5], "4") == 0);
        assert(values[6] != NULL && strcmp(values[6], "000102FF") == 0);
        row_ctx->found_text_binary = 1;
        return 0;
    }
    if (strcmp(values[0], "2") == 0) {
        assert(strcmp(values[1], "1") == 0);
        assert(values[2] == NULL);
        assert(values[3] == NULL);
        assert(strcmp(values[4], "1") == 0);
        assert(values[5] == NULL);
        assert(values[6] == NULL);
        row_ctx->found_nulls = 1;
        return 0;
    }
    if (strcmp(values[0], "3") == 0) {
        assert(strcmp(values[1], "0") == 0);
        assert(values[2] != NULL && strcmp(values[2], "0") == 0);
        assert(values[3] != NULL && strcmp(values[3], "") == 0);
        assert(strcmp(values[4], "0") == 0);
        assert(values[5] != NULL && strcmp(values[5], "0") == 0);
        assert(values[6] != NULL && strcmp(values[6], "") == 0);
        row_ctx->found_empty = 1;
        return 0;
    }
    if (strcmp(values[0], "4") == 0) {
        assert(strcmp(values[1], "0") == 0);
        assert(values[2] != NULL && strcmp(values[2], "5400") == 0);
        assert(values[3] != NULL && strlen(values[3]) == 5400U);
        assert(strcmp(values[4], "1") == 0);
        assert(values[5] == NULL);
        assert(values[6] == NULL);
        row_ctx->found_large = 1;
        return 0;
    }

    assert(0);
    return 1;
}

static int mutable_row_callback(void *ctx, int column_count, char **values, char **column_names) {
    mutable_row_context *row_ctx = (mutable_row_context *)ctx;
    (void)column_names;

    assert(column_count == 4);
    assert(values[0] != NULL);
    ++row_ctx->rows;
    if (strcmp(values[0], "2") == 0) {
        assert(values[1] != NULL && strcmp(values[1], "published edited") == 0);
        assert(values[2] != NULL && strcmp(values[2], "5600") == 0);
        assert(values[3] != NULL && strcmp(values[3], "0") == 0);
        row_ctx->found_updated = 1;
        return 0;
    }
    if (strcmp(values[0], "3") == 0) {
        assert(values[1] != NULL && strcmp(values[1], "untouched") == 0);
        assert(values[2] == NULL);
        assert(values[3] != NULL && strcmp(values[3], "1") == 0);
        row_ctx->found_untouched = 1;
        return 0;
    }

    assert(0);
    return 1;
}

static int alter_row_callback(void *ctx, int column_count, char **values, char **column_names) {
    alter_row_context *row_ctx = (alter_row_context *)ctx;
    (void)column_names;

    assert(column_count == 5);
    assert(values[0] != NULL);
    ++row_ctx->rows;
    if (strcmp(values[0], "1") == 0) {
        assert(values[1] != NULL && strcmp(values[1], "first") == 0);
        assert(values[2] != NULL && strcmp(values[2], "draft") == 0);
        assert(values[3] != NULL && strcmp(values[3], "5") == 0);
        assert(values[4] != NULL && strcmp(values[4], "0") == 0);
        row_ctx->found_first = 1;
        return 0;
    }
    if (strcmp(values[0], "2") == 0) {
        assert(values[1] != NULL && strcmp(values[1], "second") == 0);
        assert(values[2] != NULL && strcmp(values[2], "draft") == 0);
        assert(values[3] != NULL && strcmp(values[3], "4200") == 0);
        assert(values[4] != NULL && strcmp(values[4], "0") == 0);
        row_ctx->found_large = 1;
        return 0;
    }

    assert(0);
    return 1;
}

static int auto_row_callback(void *ctx, int column_count, char **values, char **column_names) {
    auto_row_context *row_ctx = (auto_row_context *)ctx;
    (void)column_names;

    assert(column_count == 2);
    assert(values[0] != NULL);
    assert(values[1] != NULL);
    ++row_ctx->rows;
    if (strcmp(values[0], "1") == 0 && strcmp(values[1], "first") == 0) {
        row_ctx->found_first = 1;
        return 0;
    }
    if (strcmp(values[0], "7") == 0 && strcmp(values[1], "manual") == 0) {
        row_ctx->found_manual = 1;
        return 0;
    }
    if (strcmp(values[0], "8") == 0 && strcmp(values[1], "after manual") == 0) {
        row_ctx->found_after_manual = 1;
        return 0;
    }
    if (strcmp(values[0], "50") == 0 && strcmp(values[1], "after alter") == 0) {
        row_ctx->found_after_alter = 1;
        return 0;
    }
    if (strcmp(values[0], "51") == 0 && strcmp(values[1], "after low alter") == 0) {
        row_ctx->found_after_low_alter = 1;
        return 0;
    }
    if (strcmp(values[0], "52") == 0 && strcmp(values[1], "reopened") == 0) {
        row_ctx->found_reopened = 1;
        return 0;
    }
    if (strcmp(values[0], "80") == 0 && strcmp(values[1], "after reopened alter") == 0) {
        row_ctx->found_after_reopened_alter = 1;
        return 0;
    }

    assert(0);
    return 1;
}

static int id_sequence_callback(void *ctx, int column_count, char **values, char **column_names) {
    id_sequence_context *sequence_ctx = (id_sequence_context *)ctx;
    (void)column_names;

    assert(column_count == 1);
    assert(values[0] != NULL);
    assert(sequence_ctx->expected_ids != NULL);
    assert(sequence_ctx->rows < sequence_ctx->expected_count);
    assert(strcmp(values[0], sequence_ctx->expected_ids[sequence_ctx->rows]) == 0);
    ++sequence_ctx->rows;
    return 0;
}

static int single_value_callback(void *ctx, int column_count, char **values, char **column_names) {
    single_value_context *value_ctx = (single_value_context *)ctx;
    (void)column_names;

    assert(column_count == 1);
    assert(values[0] != NULL);
    assert(value_ctx->expected_value != NULL);
    if (strcmp(values[0], value_ctx->expected_value) != 0) {
        fprintf(
            stderr,
            "unexpected single value: got '%s', expected '%s'\n",
            values[0],
            value_ctx->expected_value
        );
    }
    assert(strcmp(values[0], value_ctx->expected_value) == 0);
    ++value_ctx->rows;
    return 0;
}

static int wordpress_post_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    wordpress_post_context *post_ctx = (wordpress_post_context *)ctx;
    (void)column_names;

    assert(column_count == 3);
    assert(values[0] != NULL && strcmp(values[0], "1") == 0);
    assert(values[1] != NULL && strcmp(values[1], "hello-world") == 0);
    assert(values[2] != NULL);
    assert(post_ctx->expected_status != NULL);
    assert(strcmp(values[2], post_ctx->expected_status) == 0);
    ++post_ctx->rows;
    return 0;
}

static int wordpress_join_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    wordpress_join_context *join_ctx = (wordpress_join_context *)ctx;
    (void)column_names;

    assert(column_count == 4);
    assert(values[0] != NULL && strcmp(values[0], "1") == 0);
    if (values[1] == NULL || strcmp(values[1], "11") != 0 || values[2] == NULL ||
        strcmp(values[2], "Hello world") != 0) {
        fprintf(
            stderr,
            "unexpected joined post title: length='%s' value='%s'\n",
            values[1] ? values[1] : "(null)",
            values[2] ? values[2] : "(null)"
        );
    }
    assert(values[1] != NULL && strcmp(values[1], "11") == 0);
    assert(values[2] != NULL && strcmp(values[2], "Hello world") == 0);
    assert(values[3] != NULL && strcmp(values[3], "42") == 0);
    ++join_ctx->rows;
    return 0;
}

static int catalog_table_callback(void *ctx, const char *schema_name, const char *table_name) {
    catalog_table_context *catalog_ctx = (catalog_table_context *)ctx;

    assert(schema_name != NULL);
    assert(table_name != NULL);
    assert(catalog_ctx->expected_schema_name != NULL);
    if (strcmp(schema_name, catalog_ctx->expected_schema_name) != 0) {
        fprintf(
            stderr,
            "unexpected catalog table schema: got '%s'.'%s', expected schema '%s'\n",
            schema_name,
            table_name,
            catalog_ctx->expected_schema_name
        );
    }
    assert(strcmp(schema_name, catalog_ctx->expected_schema_name) == 0);
    ++catalog_ctx->count;
    return 0;
}

static mylite_db *open_database(const char *root, char **filename) {
    *filename = path_join(root, "storage-engine.mylite");
    return open_database_with_filename(root, *filename);
}

static mylite_db *open_database_with_filename(const char *root, const char *filename) {
    return open_database_with_runtime_name(root, filename, "runtime");
}

static mylite_db *open_database_with_runtime_name(
    const char *root,
    const char *filename,
    const char *runtime_name
) {
    char *runtime_root = path_join(root, runtime_name);
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    if (mkdir(runtime_root, 0700) != 0) {
        assert(errno == EEXIST);
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

static void run_transaction_crash_child(const char *root, const char *filename) {
    mylite_db *db = open_database_with_runtime_name(root, filename, "crash-runtime");

    if (mylite_exec(db, "USE tx_app", NULL, NULL, NULL) != MYLITE_OK) {
        _exit(2);
    }
    if (mylite_exec(db, "BEGIN", NULL, NULL, NULL) != MYLITE_OK) {
        _exit(3);
    }
    if (mylite_exec(
            db,
            "INSERT INTO tx_posts VALUES (99, 'crash-rolled-back')",
            NULL,
            NULL,
            NULL
        ) != MYLITE_OK) {
        _exit(4);
    }
    _exit(0);
}

static pid_t hold_test_lock_for(const char *filename, timed_lock_request request) {
    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(ready_pipe[0]);
        FILE *file = fopen(filename, "r+b");
        if (file == NULL || flock(fileno(file), request.operation) != 0) {
            _exit(2);
        }
        const unsigned char ready = 1U;
        if (write(ready_pipe[1], &ready, sizeof(ready)) != (ssize_t)sizeof(ready)) {
            _exit(3);
        }
        usleep((useconds_t)request.milliseconds * k_microseconds_per_millisecond);
        fclose(file);
        close(ready_pipe[1]);
        _exit(0);
    }

    close(ready_pipe[1]);
    unsigned char ready = 0U;
    assert(read(ready_pipe[0], &ready, sizeof(ready)) == (ssize_t)sizeof(ready));
    assert(ready == 1U);
    close(ready_pipe[0]);
    return pid;
}

static void wait_test_lock_child(pid_t pid) {
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
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

static char *transaction_journal_path(const char *filename) {
    static const char suffix[] = "-transaction-journal";
    const size_t filename_len = strlen(filename);
    char *path = (char *)malloc(filename_len + sizeof(suffix));
    assert(path != NULL);
    memcpy(path, filename, filename_len);
    memcpy(path + filename_len, suffix, sizeof(suffix));
    return path;
}

static void assert_no_durable_sidecars(const char *root, const char *primary_name) {
    char *runtime_root = path_join(root, "runtime");
    assert_no_forbidden_sidecars(root);
    assert_only_primary_and_runtime_root(root, primary_name);
    assert(is_directory_empty(runtime_root));
    free(runtime_root);
}

static void assert_no_runtime_schema_directory(const char *root, const char *schema_name) {
    char *runtime_root = path_join(root, "runtime");
    DIR *directory = opendir(runtime_root);
    unsigned runtime_count = 0;

    assert(directory != NULL);
    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char *runtime_dir = path_join(runtime_root, entry->d_name);
        char *data_dir = path_join(runtime_dir, "data");
        char *schema_dir = path_join(data_dir, schema_name);
        struct stat schema_stat;
        assert(lstat(schema_dir, &schema_stat) != 0);
        assert(errno == ENOENT);
        ++runtime_count;
        free(schema_dir);
        free(data_dir);
        free(runtime_dir);
    }

    assert(runtime_count == 1);
    assert(closedir(directory) == 0);
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
    if (directory == NULL) {
        fprintf(stderr, "expected directory is missing: %s (errno=%d)\n", path, errno);
        return 0;
    }

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

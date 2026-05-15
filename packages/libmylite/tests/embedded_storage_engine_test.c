#include <mylite/mylite.h>
#include <mylite/storage.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct timed_lock_request {
    int operation;
    unsigned milliseconds;
} timed_lock_request;

static const unsigned k_busy_timeout_release_ms = 50U;
static const unsigned k_busy_timeout_wait_ms = 1000U;
static const useconds_t k_microseconds_per_millisecond = 1000U;

typedef struct engine_context {
    int rows;
    int found_mylite;
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
    const char *expected_character_set;
    const char *expected_collation;
} show_create_schema_context;

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

typedef struct catalog_table_context {
    unsigned count;
} catalog_table_context;

static void test_show_engines_reports_mylite(void);
static void test_memory_database_has_empty_mylite_discovery(void);
static void test_schema_namespaces(void);
static void test_prepared_schema_namespaces(void);
static void test_schema_options(void);
static void test_non_table_object_policy(void);
static void test_transaction_and_foreign_key_policies(void);
static void test_create_table_persists_catalog_metadata(void);
static void test_alter_table_rebuilds_keyless_rows(void);
static void test_indexed_rows(void);
static void test_standalone_index_ddl(void);
static void test_blob_text_prefix_indexes(void);
static void test_create_table_like(void);
static void test_create_table_select(void);
static void test_truncate_table_lifecycle(void);
static void test_wordpress_shaped_schema(void);
static void assert_exec_succeeds(mylite_db *db, const char *sql);
static void assert_exec_fails(mylite_db *db, const char *sql);
static void assert_exec_fails_with_message(mylite_db *db, const char *sql, const char *message);
static void assert_non_table_object_exec_fails(mylite_db *db, const char *sql);
static void assert_transaction_control_exec_fails(mylite_db *db, const char *sql);
static void assert_foreign_key_exec_fails(mylite_db *db, const char *sql);
static void assert_prepared_succeeds(mylite_db *db, const char *sql);
static void assert_prepared_fails(mylite_db *db, const char *sql);
static void assert_schema_options(
    mylite_db *db,
    const char *expected_character_set,
    const char *expected_collation,
    const char *expected_comment
);
static void assert_show_create_schema(
    mylite_db *db,
    const char *expected_character_set,
    const char *expected_collation
);
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
static int engine_callback(void *ctx, int column_count, char **values, char **column_names);
static int schema_callback(void *ctx, int column_count, char **values, char **column_names);
static int schema_option_callback(void *ctx, int column_count, char **values, char **column_names);
static int show_create_schema_callback(
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
static mylite_open_config open_config(const char *runtime_root);
static pid_t hold_test_lock_for(const char *filename, timed_lock_request request);
static void wait_test_lock_child(pid_t pid);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
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

int main(void) {
    test_show_engines_reports_mylite();
    test_memory_database_has_empty_mylite_discovery();
    test_schema_namespaces();
    test_prepared_schema_namespaces();
    test_schema_options();
    test_non_table_object_policy();
    test_transaction_and_foreign_key_policies();
    test_create_table_persists_catalog_metadata();
    test_alter_table_rebuilds_keyless_rows();
    test_indexed_rows();
    test_standalone_index_ddl();
    test_blob_text_prefix_indexes();
    test_create_table_like();
    test_create_table_select();
    test_truncate_table_lifecycle();
    test_wordpress_shaped_schema();
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
    assert_schema_options(db, "latin1", "latin1_bin", "first comment");
    assert_show_create_schema(db, "latin1", "latin1_bin");
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
    assert_schema_options(db, "utf8mb4", "utf8mb4_bin", "updated 'comment");
    assert_show_create_schema(db, "utf8mb4", "utf8mb4_bin");

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_no_runtime_schema_directory(root, "option_app");
    assert_exec_succeeds(db, "USE option_app");
    assert_schema_options(db, "utf8mb4", "utf8mb4_bin", "updated 'comment");
    assert_show_create_schema(db, "utf8mb4", "utf8mb4_bin");
    assert_exec_succeeds(
        db,
        "ALTER DATABASE option_app DEFAULT CHARACTER SET latin1 COLLATE latin1_bin "
        "COMMENT 'reopened comment'"
    );
    assert_schema_options(db, "latin1", "latin1_bin", "reopened comment");
    assert_show_create_schema(db, "latin1", "latin1_bin");
    assert_exec_succeeds(db, "DROP DATABASE option_app");
    assert(mylite_storage_schema_exists(filename, "option_app") == MYLITE_STORAGE_NOTFOUND);

    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");
    free(filename);
    remove_tree(root);
    free(root);
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

    assert_transaction_control_exec_fails(db, "START TRANSACTION");
    assert_transaction_control_exec_fails(db, "SET autocommit=0");
    assert_transaction_control_exec_fails(db, "SAVEPOINT mylite_probe");
    assert_transaction_control_exec_fails(db, "ROLLBACK");
    assert_exec_succeeds(db, "INSERT INTO posts VALUES (1, 'autocommit')");
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
    single_value_context generated_title_len = {.expected_value = "5"};
    single_value_context generated_label = {.expected_value = "draft-1"};
    single_value_context rollback_count = {.expected_value = "1"};
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
        "title_len INT AS (CHAR_LENGTH(title)) VIRTUAL, "
        "label VARCHAR(80) AS (CONCAT(title, '-', id)) STORED"
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
        "CREATE TABLE memory_posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=MEMORY"
    );
    assert(mylite_storage_table_exists(filename, "app", "memory_posts") == MYLITE_STORAGE_NOTFOUND);
    assert_catalog_table_count(filename, "app", 14U);
    assert_exec_fails(
        db,
        "CREATE TABLE innodb_posts (id INT PRIMARY KEY, title VARCHAR(255)) ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "app", 14U);
    assert_exec_fails(
        db,
        "CREATE TABLE generated_index_posts ("
        "base INT NOT NULL, "
        "doubled INT AS (base * 2) VIRTUAL, "
        "KEY doubled_key (doubled)"
        ") ENGINE=InnoDB"
    );
    assert(
        mylite_storage_table_exists(filename, "app", "generated_index_posts") ==
        MYLITE_STORAGE_NOTFOUND
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
    assert_exec_succeeds(db, "UPDATE generated_posts SET title = 'published' WHERE id = 1");
    generated_title_len = (single_value_context){.expected_value = "9"};
    generated_label = (single_value_context){.expected_value = "published-1"};
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
    generated_title_len = (single_value_context){.expected_value = "9"};
    generated_label = (single_value_context){.expected_value = "published-1"};
    rollback_count = (single_value_context){.expected_value = "3"};
    db = open_database_with_filename(root, filename);
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
    assert(auto_rows.rows == 6);
    assert(auto_rows.found_first);
    assert(auto_rows.found_manual);
    assert(auto_rows.found_after_manual);
    assert(auto_rows.found_after_alter);
    assert(auto_rows.found_after_low_alter);
    assert(auto_rows.found_reopened);
    assert_exec_fails_with_message(db, "INSERT INTO checked_posts VALUES (5, 10)", "CONSTRAINT");
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

static void test_alter_table_rebuilds_keyless_rows(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    alter_row_context rows = {0};
    table_context tables = {0};
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

    assert_exec_fails(
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
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "alter_posts", "InnoDB", "MYLITE");
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
    table_context reopened_rows = {0};
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
    assert_catalog_table_count(filename, "app", 3U);

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
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "indexed_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "nullable_unique_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "alter_index_posts", "InnoDB", "MYLITE");
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
    table_context dropped_index_rows = {0};
    table_context reopened_slug_rows = {0};
    table_context reopened_dropped_index_rows = {0};
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
    assert_exec_succeeds(db, "INSERT INTO standalone_index_posts VALUES (1, 'alpha', 'news', 10)");
    assert_exec_succeeds(db, "INSERT INTO standalone_index_posts VALUES (2, 'beta', 'tech', 20)");
    assert_exec_succeeds(db, "INSERT INTO standalone_index_posts VALUES (3, 'gamma', 'news', 30)");

    assert_exec_succeeds(
        db,
        "CREATE INDEX category_lookup ON standalone_index_posts (category) ALGORITHM=COPY"
    );
    assert_exec_succeeds(
        db,
        "CREATE UNIQUE INDEX slug_lookup ON standalone_index_posts (slug) ALGORITHM=COPY"
    );
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "standalone_index_posts", "InnoDB", "MYLITE");
    assert_exec_fails(db, "INSERT INTO standalone_index_posts VALUES (4, 'beta', 'docs', 40)");

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

    assert_exec_succeeds(db, "DROP INDEX category_lookup ON standalone_index_posts");
    assert_catalog_table_count(filename, "app", 1U);
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
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE app");
    assert(
        mylite_exec(
            db,
            "SELECT id FROM standalone_index_posts FORCE INDEX (slug_lookup) WHERE slug = 'gamma'",
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
            "SHOW INDEX FROM standalone_index_posts WHERE Key_name = 'category_lookup'",
            table_callback,
            &reopened_dropped_index_rows,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(reopened_dropped_index_rows.rows == 0);
    assert_catalog_table_count(filename, "app", 1U);
    assert_catalog_table_metadata(filename, "app", "standalone_index_posts", "InnoDB", "MYLITE");
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
    table_context reopened_body_rows = {0};
    table_context reopened_payload_rows = {0};
    table_context reopened_standalone_body_rows = {0};
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
    assert_catalog_table_count(filename, "app", 2U);

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
    assert_catalog_table_count(filename, "app", 2U);
    assert_catalog_table_metadata(filename, "app", "blob_prefix_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "app",
        "standalone_blob_prefix_posts",
        "InnoDB",
        "MYLITE"
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    db = open_database_with_filename(root, filename);
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
    assert_catalog_table_count(filename, "app", 2U);
    assert_catalog_table_metadata(filename, "app", "blob_prefix_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(
        filename,
        "app",
        "standalone_blob_prefix_posts",
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
    table_context failed_tables = {0};
    table_context body_rows = {0};
    table_context reopened_body_rows = {0};
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
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "ctas_source_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_default_constants", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_indexed_posts", "InnoDB", "MYLITE");

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
    assert_exec_succeeds(db, "USE app");
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "ctas_source_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_default_constants", "DEFAULT", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "ctas_indexed_posts", "InnoDB", "MYLITE");
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
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
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
    wordpress_post_context published_post = {
        .expected_status = "publish",
    };
    wordpress_join_context joined_postmeta = {0};
    table_context deleted_meta = {0};
    char *errmsg = NULL;

    assert_exec_succeeds(db, "CREATE DATABASE app");
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
        ") ENGINE=InnoDB"
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
        ") ENGINE=InnoDB"
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
        ") ENGINE=InnoDB"
    );
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "wp_options", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_postmeta", "InnoDB", "MYLITE");

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

    assert_exec_succeeds(
        db,
        "UPDATE wp_options SET option_value = 'Updated MyLite Site' "
        "WHERE option_name = 'blogname'"
    );
    assert_exec_succeeds(
        db,
        "UPDATE wp_posts SET post_status = 'draft' WHERE post_name = 'hello-world'"
    );
    assert_exec_succeeds(db, "DELETE FROM wp_postmeta WHERE meta_key = '_edit_lock'");
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
    db = open_database_with_filename(root, filename);
    assert_exec_succeeds(db, "USE app");
    assert_catalog_table_count(filename, "app", 3U);
    assert_catalog_table_metadata(filename, "app", "wp_options", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_posts", "InnoDB", "MYLITE");
    assert_catalog_table_metadata(filename, "app", "wp_postmeta", "InnoDB", "MYLITE");
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
    assert(mylite_close(db) == MYLITE_OK);
    assert_no_durable_sidecars(root, "storage-engine.mylite");

    free(filename);
    remove_tree(root);
    free(root);
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

static void assert_schema_options(
    mylite_db *db,
    const char *expected_character_set,
    const char *expected_collation,
    const char *expected_comment
) {
    schema_option_context options = {
        .expected_character_set = expected_character_set,
        .expected_collation = expected_collation,
        .expected_comment = expected_comment,
    };
    char *errmsg = NULL;

    assert(
        mylite_exec(
            db,
            "SELECT DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME, SCHEMA_COMMENT "
            "FROM INFORMATION_SCHEMA.SCHEMATA WHERE SCHEMA_NAME='option_app'",
            schema_option_callback,
            &options,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(options.rows == 1);
}

static void assert_show_create_schema(
    mylite_db *db,
    const char *expected_character_set,
    const char *expected_collation
) {
    show_create_schema_context show_create = {
        .expected_character_set = expected_character_set,
        .expected_collation = expected_collation,
    };
    char *errmsg = NULL;

    assert(
        mylite_exec(
            db,
            "SHOW CREATE DATABASE option_app",
            show_create_schema_callback,
            &show_create,
            &errmsg
        ) == MYLITE_OK
    );
    assert(errmsg == NULL);
    assert(show_create.rows == 1);
}

static void assert_catalog_table_count(
    const char *filename,
    const char *schema_name,
    unsigned count
) {
    catalog_table_context ctx = {0};

    assert(
        mylite_storage_list_tables(filename, schema_name, catalog_table_callback, &ctx) ==
        MYLITE_STORAGE_OK
    );
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

    if (values[0] == NULL || strcmp(values[0], "MYLITE") != 0) {
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
    assert(values[0] != NULL && strcmp(values[0], "option_app") == 0);
    assert(values[1] != NULL);
    assert(strstr(values[1], show_create_ctx->expected_character_set) != NULL);
    assert(strstr(values[1], show_create_ctx->expected_collation) != NULL);
    ++show_create_ctx->rows;
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
    assert(strcmp(schema_name, "app") == 0);
    ++catalog_ctx->count;
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

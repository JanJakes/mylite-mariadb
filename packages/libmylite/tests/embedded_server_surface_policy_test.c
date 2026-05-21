#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
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

typedef struct substring_query {
    const char *sql;
    const char *column_name;
    const char *substring;
    int seen_rows;
} substring_query;

typedef struct open_database_paths {
    const char *database_path;
    const char *runtime_root;
} open_database_paths;

typedef struct root_entries {
    const char *root;
    const char *database_name;
} root_entries;

static void test_server_surfaces_are_disabled_or_contained(void);
static mylite_db *open_database(open_database_paths paths);
static void assert_runtime_policy_variables(mylite_db *db, const char *database_path);
static void assert_replication_execution_variables_omitted(mylite_db *db);
static void assert_proxy_protocol_variables_omitted(mylite_db *db);
static void assert_system_variable_help_text_omitted(mylite_db *db);
static void assert_status_variables_omitted(mylite_db *db);
static void assert_processlist_metadata_omitted(mylite_db *db);
static void assert_oracle_compat_functions_omitted(mylite_db *db);
static void assert_compact_error_catalog(mylite_db *db);
static void assert_performance_schema_omitted_or_disabled(mylite_db *db);
static int performance_schema_variable_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);
static void assert_server_sql_rejected(mylite_db *db);
static void assert_no_server_sidecar_files(const char *database_path);
static void assert_test_root_contains_only_database_and_runtime(root_entries entries);
static void exec_ok(mylite_db *db, const char *sql);
static void expect_error(mylite_db *db, const char *sql, const char *message_part);
static void expect_prepare_error(mylite_db *db, const char *sql, const char *message_part);
static void query_expect(mylite_db *db, expected_query query);
static int expected_result_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);
static void query_single_value_contains(
    mylite_db *db,
    const char *sql,
    const char *column_name,
    const char *substring
);
static int substring_result_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
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
    test_server_surfaces_are_disabled_or_contained();
    return 0;
}

static void test_server_surfaces_are_disabled_or_contained(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "server-surface.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    db = open_database(paths);
    assert_runtime_policy_variables(db, database_path);
    assert_replication_execution_variables_omitted(db);
    assert_proxy_protocol_variables_omitted(db);
    assert_system_variable_help_text_omitted(db);
    assert_status_variables_omitted(db);
    assert_processlist_metadata_omitted(db);
    assert_oracle_compat_functions_omitted(db);
    assert_compact_error_catalog(db);
    assert_server_sql_rejected(db);
    assert_no_server_sidecar_files(database_path);
    assert(mylite_close(db) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));
    assert_test_root_contains_only_database_and_runtime(
        (root_entries){.root = root, .database_name = "server-surface.mylite"}
    );
    assert_no_server_sidecar_files(database_path);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static mylite_db *open_database(open_database_paths paths) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = paths.runtime_root,
    };
    mylite_db *db = NULL;

    assert(
        mylite_open(
            paths.database_path,
            &db,
            MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE,
            &config
        ) == MYLITE_OK
    );
    assert(db != NULL);
    return db;
}

static void assert_runtime_policy_variables(mylite_db *db, const char *database_path) {
    static const char *const variable_columns[] = {
        "log_bin",
        "have_query_cache",
        "have_profiling",
        "skip_grant_tables",
        "skip_networking",
        "general_log",
        "slow_query_log",
        "log_output",
        "max_digest_length",
        "have_dynamic_loading",
    };
    static const char *const variable_values[] = {
        "0",
        "NO",
        "NO",
        "1",
        "1",
        "0",
        "0",
        "NONE",
        "0",
        "NO",
    };
    const int variable_column_count = (int)(sizeof(variable_columns) / sizeof(variable_columns[0]));
    char *plugin_directory = path_join(database_path, "run/plugins");

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "@@log_bin AS log_bin, "
                   "@@have_query_cache AS have_query_cache, "
                   "@@have_profiling AS have_profiling, "
                   "@@skip_grant_tables AS skip_grant_tables, "
                   "@@skip_networking AS skip_networking, "
                   "@@general_log AS general_log, "
                   "@@slow_query_log AS slow_query_log, "
                   "@@log_output AS log_output, "
                   "@@max_digest_length AS max_digest_length, "
                   "@@have_dynamic_loading AS have_dynamic_loading",
            .column_count = variable_column_count,
            .row_count = 1,
            .column_names = variable_columns,
            .values = variable_values,
        }
    );
    assert_performance_schema_omitted_or_disabled(db);
    query_single_value_contains(
        db,
        "SELECT @@plugin_dir AS plugin_dir",
        "plugin_dir",
        plugin_directory
    );

    free(plugin_directory);
}

static void assert_replication_execution_variables_omitted(mylite_db *db) {
    query_expect(
        db,
        (expected_query){
            .sql = "SHOW VARIABLES LIKE 'slave_type_conversions'",
            .column_count = 2,
            .row_count = 0,
            .column_names = NULL,
            .values = NULL,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SHOW VARIABLES LIKE 'rpl_semi_sync_master_enabled'",
            .column_count = 2,
            .row_count = 0,
            .column_names = NULL,
            .values = NULL,
        }
    );

    expect_error(db, "SELECT @@slave_compressed_protocol", NULL);
    assert(mylite_mariadb_errno(db) == 1193U);
    expect_error(db, "SELECT @@slave_type_conversions", NULL);
    assert(mylite_mariadb_errno(db) == 1193U);
    expect_error(db, "SELECT @@rpl_semi_sync_master_enabled", NULL);
    assert(mylite_mariadb_errno(db) == 1193U);
    expect_prepare_error(db, "SELECT @@slave_type_conversions", NULL);
    assert(mylite_mariadb_errno(db) == 1193U);
}

static void assert_proxy_protocol_variables_omitted(mylite_db *db) {
    query_expect(
        db,
        (expected_query){
            .sql = "SHOW VARIABLES LIKE 'proxy_protocol_networks'",
            .column_count = 2,
            .row_count = 0,
            .column_names = NULL,
            .values = NULL,
        }
    );

    expect_error(db, "SELECT @@proxy_protocol_networks", NULL);
    assert(mylite_mariadb_errno(db) == 1193U);
    expect_prepare_error(db, "SELECT @@proxy_protocol_networks", NULL);
    assert(mylite_mariadb_errno(db) == 1193U);
}

static void assert_system_variable_help_text_omitted(mylite_db *db) {
    static const char *const comment_columns[] = {"comment"};
    static const char *const comment_values[] = {""};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT VARIABLE_COMMENT AS comment "
                   "FROM information_schema.SYSTEM_VARIABLES "
                   "WHERE VARIABLE_NAME = 'VERSION'",
            .column_count = 1,
            .row_count = 1,
            .column_names = comment_columns,
            .values = comment_values,
        }
    );
}

static void assert_status_variables_omitted(mylite_db *db) {
    const char *sql = "SHOW STATUS LIKE 'Questions'";
    mylite_stmt *stmt = NULL;
    const char *tail = NULL;

    query_expect(
        db,
        (expected_query){
            .sql = sql,
            .column_count = 2,
            .row_count = 0,
            .column_names = NULL,
            .values = NULL,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT VARIABLE_NAME, VARIABLE_VALUE "
                   "FROM information_schema.GLOBAL_STATUS LIMIT 1",
            .column_count = 2,
            .row_count = 0,
            .column_names = NULL,
            .values = NULL,
        }
    );
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT VARIABLE_NAME, VARIABLE_VALUE "
                   "FROM information_schema.SESSION_STATUS LIMIT 1",
            .column_count = 2,
            .row_count = 0,
            .column_names = NULL,
            .values = NULL,
        }
    );

    assert(mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, &tail) == MYLITE_OK);
    assert(stmt != NULL);
    assert(tail == sql + strlen(sql));
    assert(mylite_column_count(stmt) == 2U);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_finalize(stmt) == MYLITE_OK);
}

static void assert_processlist_metadata_omitted(mylite_db *db) {
    query_expect(
        db,
        (expected_query){
            .sql = "SELECT ID, USER FROM information_schema.PROCESSLIST LIMIT 1",
            .column_count = 2,
            .row_count = 0,
            .column_names = NULL,
            .values = NULL,
        }
    );
}

static void assert_oracle_compat_functions_omitted(mylite_db *db) {
    static const char *const column_names[] = {
        "concat_value",
        "lpad_value",
        "rpad_value",
        "ltrim_value",
        "rtrim_value",
        "substr_value",
        "replace_value",
        "trim_value",
        "length_value",
    };
    static const char *const values[] = {"ab", "0x", "x0", "x", "x", "b", "axc", "x", "3"};

    query_expect(
        db,
        (expected_query){
            .sql = "SELECT "
                   "CONCAT('a','b') AS concat_value, "
                   "LPAD('x',2,'0') AS lpad_value, "
                   "RPAD('x',2,'0') AS rpad_value, "
                   "LTRIM(' x') AS ltrim_value, "
                   "RTRIM('x ') AS rtrim_value, "
                   "SUBSTR('abc',2,1) AS substr_value, "
                   "REPLACE('abc','b','x') AS replace_value, "
                   "TRIM(' x ') AS trim_value, "
                   "LENGTH('abc') AS length_value",
            .column_count = (int)(sizeof(column_names) / sizeof(column_names[0])),
            .row_count = 1,
            .column_names = column_names,
            .values = values,
        }
    );

    expect_error(db, "SELECT DECODE_ORACLE(1,1,10)", "DECODE_ORACLE");
    expect_error(db, "SELECT oracle_schema.LENGTH('abc')", NULL);
    expect_error(db, "SELECT TRIM_ORACLE(' x ')", "Oracle compatibility SQL functions");
    expect_error(db, "SELECT SQL%ROWCOUNT", NULL);
    expect_prepare_error(db, "SELECT DECODE_ORACLE(1,1,10)", "DECODE_ORACLE");
    expect_prepare_error(db, "SELECT TRIM_ORACLE(' x ')", "Oracle compatibility SQL functions");
}

static void assert_compact_error_catalog(mylite_db *db) {
    char *errmsg = NULL;
    const char *sql = "SELECT 1 UNION SELECT 1, 2";

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 1222U);
    assert(strcmp(mylite_sqlstate(db), "21000") == 0);
    assert(strcmp(mylite_errmsg(db), "MariaDB error") == 0);
    assert(errmsg != NULL);
    assert(strcmp(errmsg, "MariaDB error") == 0);
    mylite_free(errmsg);
}

static void assert_performance_schema_omitted_or_disabled(mylite_db *db) {
    int seen_rows = 0;

    assert(
        mylite_exec(
            db,
            "SHOW VARIABLES LIKE 'performance_schema'",
            performance_schema_variable_callback,
            &seen_rows,
            NULL
        ) == MYLITE_OK
    );
    assert(seen_rows <= 1);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): required callback signature.
static int performance_schema_variable_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    int *seen_rows = (int *)ctx;

    assert(column_count == 2);
    assert(strcmp(column_names[0], "Variable_name") == 0);
    assert(strcmp(column_names[1], "Value") == 0);
    assert(values[0] != NULL);
    assert(values[1] != NULL);
    assert(strcmp(values[0], "performance_schema") == 0);
    assert(strcmp(values[1], "OFF") == 0 || strcmp(values[1], "0") == 0);
    ++(*seen_rows);
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static void assert_server_sql_rejected(mylite_db *db) {
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, "SELECT SQL_CACHE 1");
    exec_ok(db, "SELECT SQL_NO_CACHE 1");
    exec_ok(db, "SELECT FORMAT(1234.5, 2)");
    exec_ok(db, "SET @profiling = 1");
    exec_ok(db, "SET @profiling_history_size = 10");
    exec_ok(db, "SET @query_cache_size = 1048576");
    exec_ok(db, "SET @query_cache_type = 'local'");
    exec_ok(db, "SET @first_local = 1, @query_cache_limit = 1024");
    exec_ok(db, "SET @general_log = 1");
    exec_ok(db, "SET @first_log = 1, @slow_query_log = 1");
    exec_ok(db, "SET @optimizer_trace = 'enabled=on'");
    exec_ok(db, "SET @first_trace = 1, @optimizer_trace_max_mem_size = 8192");
    exec_ok(db, "SET @sql_mode = 'ORACLE'");
    exec_ok(db, "SET @password = 'local'");
    exec_ok(db, "SET sql_mode = @@sql_mode");
    exec_ok(db, "SELECT 'PROCEDURE ANALYSE()' AS literal");
    exec_ok(db, "SELECT 'INFORMATION_SCHEMA.OPTIMIZER_TRACE' AS literal");
    exec_ok(db, "SELECT 'SHOW PROCESSLIST' AS literal");
    exec_ok(db, "SHOW VARIABLES LIKE 'version'");

    expect_error(
        db,
        "CREATE USER 'mylite_user'@'localhost' IDENTIFIED BY 'secret'",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "CREATE OR REPLACE USER 'replace_user'@'localhost' IDENTIFIED BY 'secret'",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "/* leading comment */ CREATE USER 'comment_user'@'localhost' IDENTIFIED BY 'secret'",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "/*! CREATE USER 'executable_comment_user'@'localhost' IDENTIFIED BY 'secret' */",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "/*!40101 CREATE USER 'versioned_comment_user'@'localhost' IDENTIFIED BY 'secret' */",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "/*M! CREATE USER 'mariadb_comment_user'@'localhost' IDENTIFIED BY 'secret' */",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "/*M!100301 CREATE USER 'versioned_mariadb_user'@'localhost' IDENTIFIED BY 'secret' */",
        "server-owned SQL surface"
    );
    expect_error(db, "CREATE ROLE mylite_role", "server-owned SQL surface");
    expect_error(
        db,
        "CREATE SERVER remote_app FOREIGN DATA WRAPPER mysql OPTIONS (HOST '127.0.0.1')",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "CREATE OR REPLACE SERVER remote_app FOREIGN DATA WRAPPER mysql "
        "OPTIONS (HOST '127.0.0.1')",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "ALTER SERVER remote_app OPTIONS (HOST '127.0.0.1')",
        "server-owned SQL surface"
    );
    expect_error(db, "DROP SERVER remote_app", "server-owned SQL surface");
    expect_error(db, "SHOW CREATE SERVER remote_app", "server-owned SQL surface");
    expect_error(db, "BACKUP STAGE START", "server-owned SQL surface");
    expect_error(db, "BACKUP STAGE BLOCK_COMMIT", "server-owned SQL surface");
    expect_error(db, "BACKUP LOCK app.t", "server-owned SQL surface");
    expect_error(db, "BACKUP UNLOCK", "server-owned SQL surface");
    expect_error(
        db,
        "GRANT SELECT ON *.* TO 'mylite_user'@'localhost'",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "CREATE EVENT app.mylite_event ON SCHEDULE EVERY 1 DAY DO SELECT 1",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "CREATE DEFINER = CURRENT_USER EVENT app.definer_event "
        "ON SCHEDULE EVERY 1 DAY DO SELECT 1",
        "server-owned SQL surface"
    );
    expect_error(db, "SET GLOBAL event_scheduler = ON", "server-owned SQL surface");
    expect_error(db, "SET PASSWORD = PASSWORD('secret')", "server-owned SQL surface");
    expect_error(db, "SET SQL_LOG_BIN = 1", "server-owned SQL surface");
    expect_error(db, "SET @@GLOBAL.SQL_LOG_BIN = 1", "server-owned SQL surface");
    exec_ok(db, "SET @SQL_LOG_BIN = 1");
    expect_error(
        db,
        "INSTALL PLUGIN mylite_missing SONAME 'mylite_missing.so'",
        "server-owned SQL surface"
    );
    expect_error(db, "INSTALL SONAME 'ha_blackhole.so'", "server-owned SQL surface");
    expect_error(db, "UNINSTALL PLUGIN mylite_missing", "server-owned SQL surface");
    expect_error(
        db,
        "CREATE FUNCTION mylite_udf RETURNS INTEGER SONAME 'mylite_udf.so'",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "CREATE AGGREGATE FUNCTION mylite_udf_sum RETURNS REAL SONAME 'mylite_udf.so'",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "CHANGE MASTER TO MASTER_HOST = '127.0.0.1', "
        "MASTER_USER = 'mylite', MASTER_PASSWORD = 'mylite'",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "CHANGE REPLICATION SOURCE TO SOURCE_HOST = '127.0.0.1'",
        "server-owned SQL surface"
    );
    expect_error(db, "START SLAVE", "server-owned SQL surface");
    expect_error(db, "RESET MASTER", "server-owned SQL surface");
    expect_error(db, "FLUSH BINARY LOGS", "server-owned SQL surface");
    expect_error(db, "SHOW BINARY LOGS", "server-owned SQL surface");
    expect_error(db, "SHOW BINLOG EVENTS", "server-owned SQL surface");
    expect_error(db, "HELP SELECT", "server-owned SQL surface");
    expect_error(db, "SHOW AUTHORS", "server-owned SQL surface");
    expect_error(db, "SHOW CONTRIBUTORS", "server-owned SQL surface");
    expect_error(db, "SHOW PRIVILEGES", "server-owned SQL surface");
    expect_error(db, "SHOW PROCESSLIST", "server-owned SQL surface");
    expect_error(db, "SHOW FULL PROCESSLIST", "server-owned SQL surface");
    expect_error(db, "/*! SHOW PROCESSLIST */", "server-owned SQL surface");
    expect_error(db, "SHOW PROFILES", "server-owned SQL surface");
    expect_error(db, "SHOW PROFILE CPU FOR QUERY 1", "server-owned SQL surface");
    expect_error(db, "SET profiling = 1", "server-owned SQL surface");
    expect_error(db, "SET @@session.profiling = 1", "server-owned SQL surface");
    expect_error(db, "SET profiling_history_size = 10", "server-owned SQL surface");
    expect_error(db, "SET autocommit = 1, profiling = 1", "server-owned SQL surface");
    expect_error(db, "SET GLOBAL query_cache_size = 1048576", "server-owned SQL surface");
    expect_error(db, "SET query_cache_type = ON", "server-owned SQL surface");
    expect_error(db, "SET autocommit = 1, query_cache_type = ON", "server-owned SQL surface");
    expect_error(
        db,
        "SET STATEMENT query_cache_type = ON FOR SELECT 1",
        "server-owned SQL surface"
    );
    expect_error(db, "FLUSH QUERY CACHE", "server-owned SQL surface");
    expect_error(db, "RESET QUERY CACHE", "server-owned SQL surface");
    expect_error(db, "SET general_log = ON", "server-owned SQL surface");
    expect_error(db, "SET GLOBAL general_log = ON", "server-owned SQL surface");
    expect_error(db, "SET @@GLOBAL.general_log = ON", "server-owned SQL surface");
    expect_error(db, "SET slow_query_log = ON", "server-owned SQL surface");
    expect_error(db, "SET SESSION slow_query_log = ON", "server-owned SQL surface");
    expect_error(db, "SET log_slow_query = ON", "server-owned SQL surface");
    expect_error(db, "SET log_output = 'FILE'", "server-owned SQL surface");
    expect_error(db, "SET long_query_time = 0", "server-owned SQL surface");
    expect_error(db, "SET sql_log_off = 1", "server-owned SQL surface");
    expect_error(db, "SET @@session.sql_log_off = 1", "server-owned SQL surface");
    expect_error(db, "SET autocommit = 1, slow_query_log = ON", "server-owned SQL surface");
    expect_error(db, "SET STATEMENT slow_query_log = ON FOR SELECT 1", "server-owned SQL surface");
    expect_error(db, "SET STATEMENT sql_log_off = 1 FOR SELECT 1", "server-owned SQL surface");
    expect_error(db, "FLUSH LOGS", "server-owned SQL surface");
    expect_error(db, "FLUSH LOCAL LOGS", "server-owned SQL surface");
    expect_error(db, "FLUSH NO_WRITE_TO_BINLOG LOGS", "server-owned SQL surface");
    expect_error(db, "FLUSH GENERAL LOGS", "server-owned SQL surface");
    expect_error(db, "FLUSH LOCAL SLOW LOGS", "server-owned SQL surface");
    expect_error(db, "FLUSH SLOW LOGS", "server-owned SQL surface");
    expect_error(db, "SET optimizer_trace = 'enabled=on'", "server-owned SQL surface");
    expect_error(db, "SET @@session.optimizer_trace = 'enabled=on'", "server-owned SQL surface");
    expect_error(db, "SET optimizer_trace_max_mem_size = 8192", "server-owned SQL surface");
    expect_error(
        db,
        "SET autocommit = 1, optimizer_trace = 'enabled=on'",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "SET STATEMENT optimizer_trace = 'enabled=on' FOR SELECT 1",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "SELECT * FROM INFORMATION_SCHEMA.OPTIMIZER_TRACE",
        "server-owned SQL surface"
    );
    expect_error(
        db,
        "SELECT * FROM `information_schema`.`OPTIMIZER_TRACE`",
        "server-owned SQL surface"
    );
    expect_error(db, "SET sql_mode = 'ORACLE'", "Oracle SQL mode");
    expect_error(db, "SET @@session.sql_mode = 'ORACLE'", "Oracle SQL mode");
    expect_error(db, "SET STATEMENT sql_mode = 'ORACLE' FOR SELECT 1", "Oracle SQL mode");
    expect_error(
        db,
        "SET autocommit = 1, sql_mode = CONCAT(@@sql_mode, ',ORACLE')",
        "Oracle SQL mode"
    );
    expect_error(db, "SELECT SFORMAT('{}', 1)", "SFORMAT");
    expect_error(db, "SELECT 1 PROCEDURE ANALYSE()", "PROCEDURE ANALYSE");
    expect_prepare_error(
        db,
        "CREATE USER 'prepared_user'@'localhost' IDENTIFIED BY 'secret'",
        "server-owned SQL surface"
    );
    expect_prepare_error(
        db,
        "/*! CREATE USER 'prepared_comment_user'@'localhost' IDENTIFIED BY 'secret' */",
        "server-owned SQL surface"
    );
    expect_prepare_error(db, "SET @@GLOBAL.SQL_LOG_BIN = 1", "server-owned SQL surface");
    expect_prepare_error(
        db,
        "SET PASSWORD = PASSWORD('prepared-secret')",
        "server-owned SQL surface"
    );
    expect_prepare_error(db, "HELP SELECT", "server-owned SQL surface");
    expect_prepare_error(db, "SHOW AUTHORS", "server-owned SQL surface");
    expect_prepare_error(db, "SHOW CONTRIBUTORS", "server-owned SQL surface");
    expect_prepare_error(db, "SHOW PRIVILEGES", "server-owned SQL surface");
    expect_prepare_error(db, "SHOW PROCESSLIST", "server-owned SQL surface");
    expect_prepare_error(db, "SHOW FULL PROCESSLIST", "server-owned SQL surface");
    expect_prepare_error(
        db,
        "CREATE SERVER prepared_remote_app FOREIGN DATA WRAPPER mysql "
        "OPTIONS (HOST '127.0.0.1')",
        "server-owned SQL surface"
    );
    expect_prepare_error(
        db,
        "ALTER SERVER prepared_remote_app OPTIONS (HOST '127.0.0.1')",
        "server-owned SQL surface"
    );
    expect_prepare_error(db, "DROP SERVER prepared_remote_app", "server-owned SQL surface");
    expect_prepare_error(db, "SHOW CREATE SERVER prepared_remote_app", "server-owned SQL surface");
    expect_prepare_error(db, "BACKUP STAGE START", "server-owned SQL surface");
    expect_prepare_error(db, "BACKUP LOCK app.t", "server-owned SQL surface");
    expect_prepare_error(db, "BACKUP UNLOCK", "server-owned SQL surface");
    expect_prepare_error(
        db,
        "CREATE FUNCTION prepared_udf RETURNS INTEGER SONAME 'prepared_udf.so'",
        "server-owned SQL surface"
    );
    expect_prepare_error(db, "SHOW PROFILES", "server-owned SQL surface");
    expect_prepare_error(db, "SET profiling = 1", "server-owned SQL surface");
    expect_prepare_error(db, "SET query_cache_type = ON", "server-owned SQL surface");
    expect_prepare_error(db, "RESET QUERY CACHE", "server-owned SQL surface");
    expect_prepare_error(db, "SET general_log = ON", "server-owned SQL surface");
    expect_prepare_error(db, "SET slow_query_log = ON", "server-owned SQL surface");
    expect_prepare_error(db, "SET log_output = 'TABLE'", "server-owned SQL surface");
    expect_prepare_error(db, "FLUSH LOGS", "server-owned SQL surface");
    expect_prepare_error(db, "SET optimizer_trace = 'enabled=on'", "server-owned SQL surface");
    expect_prepare_error(
        db,
        "SELECT * FROM INFORMATION_SCHEMA.OPTIMIZER_TRACE",
        "server-owned SQL surface"
    );
    expect_prepare_error(db, "SET sql_mode = 'ORACLE'", "Oracle SQL mode");
    expect_prepare_error(db, "SELECT SFORMAT('{}', 1)", "SFORMAT");
    expect_prepare_error(db, "SELECT 1 PROCEDURE ANALYSE()", "PROCEDURE ANALYSE");

    exec_ok(db, "USE app");
    exec_ok(db, "CREATE TABLE optimizer_trace (id INT)");
    exec_ok(db, "SELECT * FROM optimizer_trace");
    exec_ok(db, "USE information_schema");
    expect_error(db, "SELECT * FROM OPTIMIZER_TRACE", "server-owned SQL surface");
    expect_prepare_error(db, "SELECT * FROM OPTIMIZER_TRACE", "server-owned SQL surface");
    exec_ok(db, "USE app");
    exec_ok(db, "SELECT * FROM optimizer_trace");
    exec_ok(db, "EXPLAIN SELECT 1");
    exec_ok(db, "EXPLAIN FORMAT=JSON SELECT 1");
}

static void assert_no_server_sidecar_files(const char *database_path) {
    char *data_path = path_join(database_path, "datadir");
    char *master_info_path = path_join(data_path, "master.info");
    char *relay_info_path = path_join(data_path, "relay-log.info");
    char *multi_master_info_path = path_join(data_path, "multi-master.info");
    char *binlog_index_path = path_join(data_path, "mysql-bin.index");
    char *performance_schema_path = path_join(data_path, "performance_schema");
    char *mysql_system_path = path_join(data_path, "mysql");

    assert(!path_exists(master_info_path));
    assert(!path_exists(relay_info_path));
    assert(!path_exists(multi_master_info_path));
    assert(!path_exists(binlog_index_path));
    assert(!path_exists(performance_schema_path));
    assert(!path_exists(mysql_system_path));

    free(mysql_system_path);
    free(performance_schema_path);
    free(binlog_index_path);
    free(multi_master_info_path);
    free(relay_info_path);
    free(master_info_path);
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

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);

    if (result != MYLITE_OK) {
        fprintf(stderr, "SQL failed: %s\n%s\n", sql, errmsg != NULL ? errmsg : mylite_errmsg(db));
    }
    assert(result == MYLITE_OK);
    assert(errmsg == NULL);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): test helper pairs SQL with expected text.
static void expect_error(mylite_db *db, const char *sql, const char *message_part) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);

    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql);
    } else if (message_part != NULL && (errmsg == NULL || strstr(errmsg, message_part) == NULL)) {
        fprintf(
            stderr,
            "SQL failed with unexpected message: %s\n%s\n",
            sql,
            errmsg != NULL ? errmsg : mylite_errmsg(db)
        );
    }
    assert(result == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(strcmp(mylite_sqlstate(db), "00000") != 0);
    if (message_part != NULL) {
        assert(errmsg != NULL);
        assert(strstr(errmsg, message_part) != NULL);
    }
    mylite_free(errmsg);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): test helper pairs SQL with expected text.
static void expect_prepare_error(mylite_db *db, const char *sql, const char *message_part) {
    mylite_stmt *stmt = NULL;
    const char *tail = NULL;
    const int result = mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, &tail);

    if (result == MYLITE_OK) {
        fprintf(stderr, "SQL unexpectedly prepared: %s\n", sql);
    } else if (message_part != NULL && strstr(mylite_errmsg(db), message_part) == NULL) {
        fprintf(
            stderr,
            "SQL prepare failed with unexpected message: %s\n%s\n",
            sql,
            mylite_errmsg(db)
        );
    }
    assert(result == MYLITE_ERROR);
    assert(stmt == NULL);
    assert(tail == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(strcmp(mylite_sqlstate(db), "00000") != 0);
    if (message_part != NULL) {
        assert(strstr(mylite_errmsg(db), message_part) != NULL);
    }
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

static void query_single_value_contains(
    mylite_db *db,
    const char *sql,
    const char *column_name,
    const char *substring
) {
    substring_query query = {
        .sql = sql,
        .column_name = column_name,
        .substring = substring,
        .seen_rows = 0,
    };

    assert(mylite_exec(db, sql, substring_result_callback, &query, NULL) == MYLITE_OK);
    assert(query.seen_rows == 1);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): required callback signature.
static int substring_result_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    substring_query *query = (substring_query *)ctx;
    assert(column_count == 1);
    assert(strcmp(column_names[0], query->column_name) == 0);
    assert(values[0] != NULL);
    if (strstr(values[0], query->substring) == NULL) {
        fprintf(
            stderr,
            "SQL result did not contain expected substring: %s\nvalue: %s\nexpected: %s\n",
            query->sql,
            values[0],
            query->substring
        );
    }
    assert(strstr(values[0], query->substring) != NULL);
    ++query->seen_rows;
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-server-surface.XXXXXX";
    char *root = mkdtemp(template_path);
    char *copy = NULL;

    assert(root != NULL);
    copy = strdup(root);
    assert(copy != NULL);
    return copy;
}

static char *path_join(const char *directory, const char *name) {
    const size_t directory_length = strlen(directory);
    const size_t name_length = strlen(name);
    char *path = malloc(directory_length + name_length + 2U);

    assert(path != NULL);
    memcpy(path, directory, directory_length);
    path[directory_length] = '/';
    memcpy(path + directory_length + 1U, name, name_length + 1U);
    return path;
}

static int is_directory_empty(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;

    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            assert(closedir(directory) == 0);
            return 0;
        }
    }
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

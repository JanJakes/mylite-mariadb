#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct select_context {
    int rows;
} select_context;

typedef struct variable_context {
    const char *name;
    const char *value;
    int rows;
} variable_context;

typedef struct engine_context {
    const char *name;
    int rows;
} engine_context;

typedef struct scalar_context {
    const char *column_name;
    const char *value;
    int rows;
} scalar_context;

typedef struct nullable_scalar_context {
    const char *column_name;
    int rows;
} nullable_scalar_context;

static void test_select_callback(void);
static void test_statement_effects(void);
static void test_callback_abort(void);
static void test_syntax_error_diagnostics(void);
static void test_server_surfaces_are_disabled(void);
static void test_backup_sql_is_rejected(void);
static void test_query_cache_sql_is_rejected(void);
static void test_statement_profiling_sql_is_rejected(void);
static void test_optimizer_trace_sql_is_rejected(void);
static void test_table_maintenance_sql_is_rejected(void);
static void test_sql_handler_commands_are_rejected(void);
static void test_help_command_is_rejected(void);
static void test_static_show_info_is_rejected(void);
static void test_status_metadata_is_empty(void);
static void test_processlist_metadata_is_rejected(void);
static void test_routine_metadata_is_empty(void);
static void test_procedure_analyse_is_rejected(void);
static void test_server_utility_functions_are_rejected(void);
static void test_zlib_compression_is_disabled(void);
static void test_gis_sql_functions_are_rejected(void);
static void test_vector_sql_functions_are_rejected(void);
static void test_sformat_sql_function_is_rejected(void);
static void test_json_schema_valid_sql_function_is_rejected(void);
static void test_json_table_function_is_rejected(void);
static void test_dynamic_column_functions_are_rejected(void);
static void test_sequence_sql_is_rejected(void);
static void test_user_statistics_sql_is_rejected(void);
static void test_xml_sql_functions_are_rejected(void);
static void test_oracle_sql_mode_is_rejected(void);
static void test_file_import_policy_is_rejected(void);
static void test_file_export_policy_is_rejected(void);
static void test_non_table_objects_are_rejected(void);
static void test_view_metadata_is_empty(void);
static void test_trigger_metadata_is_empty(void);
static void test_transaction_control_policy(void);
static void test_locking_sql_is_rejected(void);
static void test_online_alter_policy_is_rejected(void);
static void test_foreign_key_policy_is_rejected(void);
static void test_unsupported_engine_policy_is_rejected(void);
static void test_partition_policy_is_rejected(void);
static void assert_variable_value(mylite_db *db, const char *name, const char *value);
static void assert_variable_value_or_missing(mylite_db *db, const char *name, const char *value);
static void assert_variable_missing(mylite_db *db, const char *name);
static void assert_engine_missing(mylite_db *db, const char *name);
static void assert_exec_fails(mylite_db *db, const char *sql);
static void assert_backup_exec_fails(mylite_db *db, const char *sql);
static void assert_query_cache_exec_fails(mylite_db *db, const char *sql);
static void assert_statement_profiling_exec_fails(mylite_db *db, const char *sql);
static void assert_optimizer_trace_exec_fails(mylite_db *db, const char *sql);
static void assert_table_maintenance_exec_fails(mylite_db *db, const char *sql);
static void assert_sql_handler_exec_fails(mylite_db *db, const char *sql);
static void assert_help_command_exec_fails(mylite_db *db, const char *sql);
static void assert_static_show_info_exec_fails(mylite_db *db, const char *sql);
static void assert_processlist_metadata_exec_fails(mylite_db *db, const char *sql);
static void assert_procedure_analyse_exec_fails(mylite_db *db, const char *sql);
static void assert_select_procedure_exec_fails(mylite_db *db, const char *sql);
static void assert_replication_filter_exec_fails(mylite_db *db, const char *sql);
static void assert_binlog_replication_variable_exec_fails(mylite_db *db, const char *sql);
static void assert_server_utility_exec_fails(mylite_db *db, const char *sql);
static void assert_gis_sql_function_exec_fails(mylite_db *db, const char *sql);
static void assert_vector_sql_function_exec_fails(mylite_db *db, const char *sql);
static void assert_sformat_sql_function_exec_fails(mylite_db *db, const char *sql);
static void assert_json_schema_valid_exec_fails(mylite_db *db, const char *sql);
static void assert_json_table_exec_fails(mylite_db *db, const char *sql);
static void assert_dynamic_column_exec_fails(mylite_db *db, const char *sql);
static void assert_sequence_exec_fails(mylite_db *db, const char *sql);
static void assert_user_statistics_exec_fails(mylite_db *db, const char *sql);
static void assert_xml_sql_function_exec_fails(mylite_db *db, const char *sql);
static void assert_oracle_sql_mode_exec_fails(mylite_db *db, const char *sql);
static void assert_file_import_exec_fails(mylite_db *db, const char *sql);
static void assert_file_export_exec_fails(mylite_db *db, const char *sql);
static void assert_non_table_object_exec_fails(mylite_db *db, const char *sql);
static void assert_transaction_control_exec_fails(mylite_db *db, const char *sql);
static void assert_locking_sql_exec_fails(mylite_db *db, const char *sql);
static void assert_online_alter_exec_fails(mylite_db *db, const char *sql);
static void assert_csv_engine_exec_fails(mylite_db *db, const char *sql);
static void assert_unsupported_engine_exec_fails(mylite_db *db, const char *sql);
static void assert_partition_exec_fails(mylite_db *db, const char *sql);
static void assert_foreign_key_exec_fails(mylite_db *db, const char *sql);
static int select_callback(void *ctx, int column_count, char **values, char **column_names);
static int scalar_callback(void *ctx, int column_count, char **values, char **column_names);
static int nullable_scalar_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);
static int abort_callback(void *ctx, int column_count, char **values, char **column_names);
static int count_only_callback(void *ctx, int column_count, char **values, char **column_names);
static int variable_callback(void *ctx, int column_count, char **values, char **column_names);
static int variable_name_callback(void *ctx, int column_count, char **values, char **column_names);
static int engine_name_callback(void *ctx, int column_count, char **values, char **column_names);
static mylite_db *open_database(const char *root, char **filename);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static int is_directory_empty(const char *path);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

int main(void) {
    test_select_callback();
    test_statement_effects();
    test_callback_abort();
    test_syntax_error_diagnostics();
    test_server_surfaces_are_disabled();
    test_backup_sql_is_rejected();
    test_query_cache_sql_is_rejected();
    test_statement_profiling_sql_is_rejected();
    test_optimizer_trace_sql_is_rejected();
    test_table_maintenance_sql_is_rejected();
    test_sql_handler_commands_are_rejected();
    test_help_command_is_rejected();
    test_static_show_info_is_rejected();
    test_status_metadata_is_empty();
    test_processlist_metadata_is_rejected();
    test_routine_metadata_is_empty();
    test_procedure_analyse_is_rejected();
    test_server_utility_functions_are_rejected();
    test_zlib_compression_is_disabled();
    test_gis_sql_functions_are_rejected();
    test_vector_sql_functions_are_rejected();
    test_sformat_sql_function_is_rejected();
    test_json_schema_valid_sql_function_is_rejected();
    test_json_table_function_is_rejected();
    test_dynamic_column_functions_are_rejected();
    test_sequence_sql_is_rejected();
    test_user_statistics_sql_is_rejected();
    test_xml_sql_functions_are_rejected();
    test_oracle_sql_mode_is_rejected();
    test_file_import_policy_is_rejected();
    test_file_export_policy_is_rejected();
    test_non_table_objects_are_rejected();
    test_view_metadata_is_empty();
    test_trigger_metadata_is_empty();
    test_transaction_control_policy();
    test_locking_sql_is_rejected();
    test_online_alter_policy_is_rejected();
    test_foreign_key_policy_is_rejected();
    test_unsupported_engine_policy_is_rejected();
    test_partition_policy_is_rejected();
    return 0;
}

static void test_select_callback(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    select_context ctx = {.rows = 0};

    assert(
        mylite_exec(db, "SELECT 1 AS one, NULL AS empty", select_callback, &ctx, NULL) == MYLITE_OK
    );
    assert(ctx.rows == 1);
    assert(mylite_changes(db) == 0);
    assert(mylite_last_insert_id(db) == 0U);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_statement_effects(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE direct_effects ("
            "id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, value INT NOT NULL)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_exec(db, "INSERT INTO direct_effects (value) VALUES (10),(20)", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(mylite_changes(db) == 2);
    assert(mylite_last_insert_id(db) == 1U);

    assert(
        mylite_exec(
            db,
            "UPDATE direct_effects SET value = value + 1 WHERE value = 20",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(mylite_changes(db) == 1);

    assert(
        mylite_exec(db, "DELETE FROM direct_effects WHERE value = 10", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(mylite_changes(db) == 1);

    assert(
        mylite_exec(db, "INSERT INTO direct_effects (value) VALUES (30)", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(mylite_changes(db) == 1);
    assert(mylite_last_insert_id(db) == 3U);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_callback_abort(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    int callback_count = 0;
    char *errmsg = NULL;

    assert(mylite_exec(db, "SELECT 1", abort_callback, &callback_count, &errmsg) == MYLITE_ERROR);
    assert(callback_count == 1);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "callback") != NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    mylite_free(errmsg);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_syntax_error_diagnostics(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    char *errmsg = NULL;

    assert(mylite_exec(db, "SELEC broken", NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "syntax") != NULL || strstr(errmsg, "SQL") != NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) != 0U);
    assert(strcmp(mylite_sqlstate(db), "00000") != 0);
    mylite_free(errmsg);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_server_surfaces_are_disabled(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_variable_value(db, "skip_networking", "ON");
    assert_variable_value(db, "log_bin", "OFF");
    assert_variable_value(db, "have_dynamic_loading", "NO");
    assert_variable_value_or_missing(db, "performance_schema", "OFF");
    assert_variable_missing(db, "binlog_format");
    assert_variable_missing(db, "gtid_binlog_state");
    assert_variable_missing(db, "relay_log");
    assert_variable_missing(db, "replicate_do_db");
    assert_variable_missing(db, "concurrent_insert");
    assert_variable_missing(db, "delay_key_write");
    assert_variable_missing(db, "flush");
    assert_engine_missing(db, "CSV");
    assert_engine_missing(db, "InnoDB");
    assert_engine_missing(db, "MyISAM");
    assert_engine_missing(db, "MRG_MyISAM");
    assert_engine_missing(db, "partition");

    assert_exec_fails(db, "CREATE USER 'mylite_probe'@'localhost' IDENTIFIED BY 'secret'");
    assert_exec_fails(db, "GRANT SELECT ON *.* TO 'mylite_probe'@'localhost'");
    assert_exec_fails(db, "SET GLOBAL event_scheduler = ON");
    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert_exec_fails(db, "CREATE EVENT mylite_probe_event ON SCHEDULE EVERY 1 SECOND DO SELECT 1");
    assert_exec_fails(db, "INSTALL PLUGIN mylite_probe SONAME 'mylite_probe.so'");
    assert_exec_fails(db, "INSTALL SONAME 'mylite_probe'");
    assert_exec_fails(db, "UNINSTALL PLUGIN mylite_probe");
    assert_exec_fails(db, "UNINSTALL SONAME 'mylite_probe'");
    assert_exec_fails(db, "BINLOG 'AAAA'");
    assert_exec_fails(db, "CHANGE MASTER TO MASTER_HOST='example.test'");
    assert_exec_fails(db, "SHOW MASTER STATUS");
    assert_replication_filter_exec_fails(db, "SET GLOBAL replicate_do_db = 'app'");
    assert_replication_filter_exec_fails(db, "SET @@global.binlog_ignore_db = 'mysql'");
    assert_replication_filter_exec_fails(
        db,
        "SET sql_mode = '', replicate_rewrite_db = 'source->target'"
    );
    assert_binlog_replication_variable_exec_fails(db, "SET binlog_format = ROW");
    assert_binlog_replication_variable_exec_fails(db, "SET GLOBAL sync_binlog = 1");
    assert_binlog_replication_variable_exec_fails(
        db,
        "SET sql_mode = '', @@session.gtid_domain_id = 7"
    );
    assert_exec_fails(
        db,
        "CREATE SERVER mylite_probe_server "
        "FOREIGN DATA WRAPPER mysql "
        "OPTIONS (USER 'remote', HOST 'localhost', DATABASE 'app')"
    );
    assert_exec_fails(
        db,
        "CREATE OR REPLACE SERVER mylite_probe_server "
        "FOREIGN DATA WRAPPER mysql "
        "OPTIONS (USER 'remote', HOST 'localhost', DATABASE 'app')"
    );
    assert_exec_fails(db, "ALTER SERVER mylite_probe_server OPTIONS (HOST '127.0.0.1')");
    assert_exec_fails(db, "DROP SERVER IF EXISTS mylite_probe_server");
    assert_exec_fails(db, "SHOW CREATE SERVER mylite_probe_server");

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_backup_sql_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE backup_probe (id INT NOT NULL PRIMARY KEY)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert_backup_exec_fails(db, "BACKUP STAGE START");
    assert_backup_exec_fails(db, "BACKUP STAGE FLUSH");
    assert_backup_exec_fails(db, "BACKUP STAGE BLOCK_DDL");
    assert_backup_exec_fails(db, "BACKUP STAGE BLOCK_COMMIT");
    assert_backup_exec_fails(db, "BACKUP STAGE END");
    assert_backup_exec_fails(db, "BACKUP LOCK backup_probe");
    assert_backup_exec_fails(db, "BACKUP UNLOCK");
    assert_backup_exec_fails(db, "/*! BACKUP STAGE START */");
    assert_backup_exec_fails(db, "/*!50600 BACKUP LOCK backup_probe */");
    assert(
        mylite_exec(
            db,
            "SELECT 'BACKUP STAGE START' AS backup_stage_text, "
            "'BACKUP LOCK backup_probe' AS backup_lock_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_query_cache_sql_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_variable_value(db, "have_query_cache", "NO");
    assert_variable_value(db, "query_cache_type", "OFF");
    assert_variable_value(db, "query_cache_size", "0");

    assert(mylite_exec(db, "SELECT SQL_CACHE 1 AS cache_hint", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "SELECT SQL_NO_CACHE 1 AS no_cache_hint", NULL, NULL, NULL) == MYLITE_OK
    );

    assert_query_cache_exec_fails(db, "FLUSH QUERY CACHE");
    assert_query_cache_exec_fails(db, "FLUSH NO_WRITE_TO_BINLOG QUERY CACHE");
    assert_query_cache_exec_fails(db, "RESET QUERY CACHE");
    assert_query_cache_exec_fails(db, "SET query_cache_type=ON");
    assert_query_cache_exec_fails(db, "SET GLOBAL query_cache_size=1048576");
    assert_query_cache_exec_fails(db, "SET @@GLOBAL.query_cache_size=1048576");
    assert_query_cache_exec_fails(db, "SET SESSION query_cache_wlock_invalidate=ON");
    assert_query_cache_exec_fails(db, "SET STATEMENT query_cache_type=ON FOR SELECT 1");
    assert_query_cache_exec_fails(db, "/*! RESET QUERY CACHE */");
    assert_query_cache_exec_fails(db, "/*!50600 FLUSH QUERY CACHE */");
    assert(
        mylite_exec(
            db,
            "SELECT 'RESET QUERY CACHE' AS reset_cache_text, "
            "'SET query_cache_type=ON' AS query_cache_type_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_statement_profiling_sql_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_variable_value(db, "have_profiling", "NO");

    assert_statement_profiling_exec_fails(db, "SHOW PROFILE");
    assert_statement_profiling_exec_fails(db, "SHOW PROFILE CPU FOR QUERY 1");
    assert_statement_profiling_exec_fails(db, "SHOW PROFILES");
    assert_statement_profiling_exec_fails(db, "SET profiling=1");
    assert_statement_profiling_exec_fails(db, "SET GLOBAL profiling_history_size=25");
    assert_statement_profiling_exec_fails(db, "SET @@session.profiling_history_size=25");
    assert_statement_profiling_exec_fails(db, "SET STATEMENT profiling=1 FOR SELECT 1");
    assert_statement_profiling_exec_fails(db, "SELECT * FROM INFORMATION_SCHEMA.PROFILING");
    assert_statement_profiling_exec_fails(db, "SELECT * FROM `information_schema`.`PROFILING`");
    assert(
        mylite_exec(
            db,
            "SELECT 'SHOW PROFILE' AS profile_text, "
            "'INFORMATION_SCHEMA.PROFILING' AS profiling_table_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_optimizer_trace_sql_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_variable_value(db, "optimizer_trace", "enabled=off");

    assert_optimizer_trace_exec_fails(db, "SET optimizer_trace='enabled=on'");
    assert_optimizer_trace_exec_fails(db, "SET @@session.optimizer_trace='enabled=on'");
    assert_optimizer_trace_exec_fails(db, "SET optimizer_trace_max_mem_size=8192");
    assert_optimizer_trace_exec_fails(
        db,
        "SET STATEMENT optimizer_trace='enabled=on' FOR SELECT 1"
    );
    assert_optimizer_trace_exec_fails(db, "SELECT * FROM INFORMATION_SCHEMA.OPTIMIZER_TRACE");
    assert_optimizer_trace_exec_fails(db, "SELECT * FROM `information_schema`.`OPTIMIZER_TRACE`");
    assert(mylite_exec(db, "EXPLAIN SELECT 1", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "SELECT 'SET optimizer_trace=1' AS trace_text, "
            "'INFORMATION_SCHEMA.OPTIMIZER_TRACE' AS trace_table_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_table_maintenance_sql_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_table_maintenance_exec_fails(db, "CHECK TABLE maintenance_probe");
    assert_table_maintenance_exec_fails(db, "ANALYZE TABLE maintenance_probe");
    assert_table_maintenance_exec_fails(db, "ANALYZE LOCAL TABLE maintenance_probe");
    assert_table_maintenance_exec_fails(db, "OPTIMIZE TABLE maintenance_probe");
    assert_table_maintenance_exec_fails(db, "OPTIMIZE NO_WRITE_TO_BINLOG TABLE maintenance_probe");
    assert_table_maintenance_exec_fails(db, "REPAIR TABLE maintenance_probe");
    assert_table_maintenance_exec_fails(db, "REPAIR LOCAL TABLE maintenance_probe");
    assert_table_maintenance_exec_fails(db, "CACHE INDEX maintenance_probe IN keycache");
    assert_table_maintenance_exec_fails(db, "LOAD INDEX INTO CACHE maintenance_probe");
    assert(
        mylite_exec(
            db,
            "SELECT 'CHECK TABLE' AS check_text, 'LOAD INDEX' AS load_index_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_sql_handler_commands_are_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_sql_handler_exec_fails(db, "HANDLER no_such_table OPEN");
    assert_sql_handler_exec_fails(db, "HANDLER no_such_table READ FIRST");
    assert_sql_handler_exec_fails(db, "HANDLER no_such_table CLOSE");
    assert_sql_handler_exec_fails(db, "/*! HANDLER no_such_table READ FIRST */");
    assert_sql_handler_exec_fails(db, "/*!50600 HANDLER no_such_table READ FIRST */");
    assert(
        mylite_exec(db, "SELECT 'HANDLER no_such_table OPEN' AS handler_text", NULL, NULL, NULL) ==
        MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_help_command_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_help_command_exec_fails(db, "HELP 'contents'");
    assert_help_command_exec_fails(db, "help contents");
    assert_help_command_exec_fails(db, "/*! HELP contents */");
    assert(mylite_exec(db, "SELECT 'HELP contents' AS help_text", NULL, NULL, NULL) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_static_show_info_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_static_show_info_exec_fails(db, "SHOW AUTHORS");
    assert_static_show_info_exec_fails(db, "SHOW CONTRIBUTORS");
    assert_static_show_info_exec_fails(db, "SHOW PRIVILEGES");
    assert_static_show_info_exec_fails(db, "/*! SHOW AUTHORS */");
    assert(mylite_exec(db, "SHOW VARIABLES LIKE 'version'", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "SELECT 'SHOW AUTHORS' AS authors_text, "
            "'SHOW PRIVILEGES' AS privileges_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_status_metadata_is_empty(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    int session_status_rows = 0;
    int explicit_session_status_rows = 0;
    int global_status_rows = 0;
    int local_status_rows = 0;
    scalar_context global_status_count = {
        .column_name = "status_count",
        .value = "0",
        .rows = 0,
    };
    scalar_context session_status_count = {
        .column_name = "status_count",
        .value = "0",
        .rows = 0,
    };

    assert(
        mylite_exec(db, "SHOW STATUS", count_only_callback, &session_status_rows, NULL) == MYLITE_OK
    );
    assert(session_status_rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW SESSION STATUS",
            count_only_callback,
            &explicit_session_status_rows,
            NULL
        ) == MYLITE_OK
    );
    assert(explicit_session_status_rows == 0);
    assert(
        mylite_exec(db, "SHOW GLOBAL STATUS", count_only_callback, &global_status_rows, NULL) ==
        MYLITE_OK
    );
    assert(global_status_rows == 0);
    assert(
        mylite_exec(db, "SHOW LOCAL STATUS", count_only_callback, &local_status_rows, NULL) ==
        MYLITE_OK
    );
    assert(local_status_rows == 0);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) AS status_count FROM INFORMATION_SCHEMA.GLOBAL_STATUS",
            scalar_callback,
            &global_status_count,
            NULL
        ) == MYLITE_OK
    );
    assert(global_status_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) AS status_count FROM INFORMATION_SCHEMA.SESSION_STATUS",
            scalar_callback,
            &session_status_count,
            NULL
        ) == MYLITE_OK
    );
    assert(session_status_count.rows == 1);
    assert(mylite_exec(db, "SHOW VARIABLES LIKE 'version'", NULL, NULL, NULL) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_processlist_metadata_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    scalar_context processlist_count = {
        .column_name = "process_count",
        .value = "0",
        .rows = 0,
    };

    assert_processlist_metadata_exec_fails(db, "SHOW PROCESSLIST");
    assert_processlist_metadata_exec_fails(db, "SHOW FULL PROCESSLIST");
    assert_processlist_metadata_exec_fails(db, "/*! SHOW PROCESSLIST */");
    assert_processlist_metadata_exec_fails(db, "/*!50600 SHOW FULL PROCESSLIST */");
    assert(
        mylite_exec(db, "SELECT 'SHOW PROCESSLIST' AS processlist_text", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) AS process_count FROM INFORMATION_SCHEMA.PROCESSLIST",
            scalar_callback,
            &processlist_count,
            NULL
        ) == MYLITE_OK
    );
    assert(processlist_count.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_routine_metadata_is_empty(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    int procedure_status_rows = 0;
    int function_status_rows = 0;
    scalar_context routines_count = {
        .column_name = "routine_count",
        .value = "0",
        .rows = 0,
    };
    scalar_context parameters_count = {
        .column_name = "parameter_count",
        .value = "0",
        .rows = 0,
    };

    assert(
        mylite_exec(
            db,
            "SHOW PROCEDURE STATUS LIKE 'blocked_proc'",
            count_only_callback,
            &procedure_status_rows,
            NULL
        ) == MYLITE_OK
    );
    assert(procedure_status_rows == 0);
    assert(
        mylite_exec(
            db,
            "SHOW FUNCTION STATUS LIKE 'blocked_func'",
            count_only_callback,
            &function_status_rows,
            NULL
        ) == MYLITE_OK
    );
    assert(function_status_rows == 0);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) AS routine_count FROM INFORMATION_SCHEMA.ROUTINES",
            scalar_callback,
            &routines_count,
            NULL
        ) == MYLITE_OK
    );
    assert(routines_count.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) AS parameter_count FROM INFORMATION_SCHEMA.PARAMETERS",
            scalar_callback,
            &parameters_count,
            NULL
        ) == MYLITE_OK
    );
    assert(parameters_count.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_procedure_analyse_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_procedure_analyse_exec_fails(db, "SELECT 1 PROCEDURE ANALYSE()");
    assert_procedure_analyse_exec_fails(db, "SELECT 1 PROCEDURE analyse(10, 2000)");
    assert_select_procedure_exec_fails(db, "SELECT 1 PROCEDURE unknown_procedure()");
    assert(
        mylite_exec(db, "SELECT 'PROCEDURE ANALYSE()' AS procedure_text", NULL, NULL, NULL) ==
        MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_server_utility_functions_are_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_server_utility_exec_fails(db, "SELECT BENCHMARK(1, 1 + 1)");
    assert_server_utility_exec_fails(db, "SELECT SLEEP(0.01)");
    assert_server_utility_exec_fails(db, "SELECT UUID_SHORT()");
    assert_server_utility_exec_fails(db, "SELECT MASTER_POS_WAIT('mysql-bin.000001', 4, 1)");
    assert_server_utility_exec_fails(db, "SELECT MASTER_GTID_WAIT('0-1-1', 1)");
    assert(mylite_exec(db, "SELECT 'SLEEP(' AS quoted_text", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SELECT VERSION()", NULL, NULL, NULL) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_zlib_compression_is_disabled(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    nullable_scalar_context compressed_value = {
        .column_name = "compressed_value",
        .rows = 0,
    };
    nullable_scalar_context uncompressed_value = {
        .column_name = "uncompressed_value",
        .rows = 0,
    };
    char *errmsg = NULL;

    assert_variable_value(db, "have_compress", "NO");
    assert(
        mylite_exec(
            db,
            "SELECT COMPRESS('abc') AS compressed_value",
            nullable_scalar_callback,
            &compressed_value,
            NULL
        ) == MYLITE_OK
    );
    assert(compressed_value.rows == 1);
    assert(
        mylite_exec(
            db,
            "SELECT UNCOMPRESS('abc') AS uncompressed_value",
            nullable_scalar_callback,
            &uncompressed_value,
            NULL
        ) == MYLITE_OK
    );
    assert(uncompressed_value.rows == 1);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE compressed_columns (body TEXT COMPRESSED)",
            NULL,
            NULL,
            &errmsg
        ) == MYLITE_ERROR
    );
    assert(errmsg != NULL);
    assert(strstr(errmsg, "zlib column compression") != NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) != 0U);
    assert(strcmp(mylite_sqlstate(db), "00000") != 0);
    mylite_free(errmsg);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_gis_sql_functions_are_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_gis_sql_function_exec_fails(db, "SELECT ST_AsText(0x00)");
    assert_gis_sql_function_exec_fails(db, "SELECT ST_GeomFromText('POINT(1 1)')");
    assert_gis_sql_function_exec_fails(
        db,
        "SELECT ST_Contains(ST_GeomFromText('POINT(1 1)'), ST_GeomFromText('POINT(1 1)'))"
    );
    assert_gis_sql_function_exec_fails(db, "SELECT PointFromText('POINT(1 1)')");
    assert_gis_sql_function_exec_fails(db, "SELECT Point(1, 2)");
    assert_gis_sql_function_exec_fails(db, "SELECT X(Point(1, 2))");
    assert(
        mylite_exec(
            db,
            "SELECT 'ST_AsText(' AS st_text, 'PointFromText(' AS point_text, 'X(' AS x_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_vector_sql_functions_are_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_vector_sql_function_exec_fails(db, "SELECT VEC_FromText('[1,2,3]')");
    assert_vector_sql_function_exec_fails(db, "SELECT VEC_ToText(X'0000803F')");
    assert_vector_sql_function_exec_fails(db, "SELECT VEC_DISTANCE(X'0000803F', X'00000040')");
    assert_vector_sql_function_exec_fails(
        db,
        "SELECT VEC_DISTANCE_EUCLIDEAN(X'0000803F', X'00000040')"
    );
    assert_vector_sql_function_exec_fails(
        db,
        "SELECT VEC_DISTANCE_COSINE(X'0000803F', X'00000040')"
    );
    assert_vector_sql_function_exec_fails(db, "/*! SELECT VEC_FromText('[1]') */");
    assert(
        mylite_exec(
            db,
            "SELECT 'VEC_FromText(' AS from_text, 'VEC_DISTANCE(' AS distance_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_sformat_sql_function_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_sformat_sql_function_exec_fails(db, "SELECT SFORMAT('The answer is {}.', 42)");
    assert_sformat_sql_function_exec_fails(db, "SELECT sformat('{}', 'value')");
    assert(
        mylite_exec(db, "SELECT 'SFORMAT(' AS sformat_text, FORMAT(1234.5, 1)", NULL, NULL, NULL) ==
        MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_json_schema_valid_sql_function_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_json_schema_valid_exec_fails(db, "SELECT JSON_SCHEMA_VALID('{}', '{}')");
    assert_json_schema_valid_exec_fails(
        db,
        "SELECT json_schema_valid('{\"type\":\"number\"}', '3')"
    );
    assert_json_schema_valid_exec_fails(db, "/*! SELECT JSON_SCHEMA_VALID('{}', '{}') */");
    assert(
        mylite_exec(
            db,
            "SELECT 'JSON_SCHEMA_VALID(' AS quoted_text, JSON_VALID('{\"ok\": true}')",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_json_table_function_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_json_table_exec_fails(
        db,
        "SELECT * FROM JSON_TABLE('[1,2]', '$[*]' COLUMNS (value INT PATH '$')) AS jt"
    );
    assert_json_table_exec_fails(
        db,
        "SELECT * FROM json_table('[3]', '$[*]' COLUMNS (value INT PATH '$')) AS jt"
    );
    assert_json_table_exec_fails(
        db,
        "/*! SELECT * FROM JSON_TABLE('[1]', '$[*]' COLUMNS (value INT PATH '$')) AS jt */"
    );
    assert(
        mylite_exec(
            db,
            "SELECT 'JSON_TABLE(' AS quoted_text, JSON_EXTRACT('{\"ok\": true}', '$.ok')",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_dynamic_column_functions_are_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_dynamic_column_exec_fails(db, "SELECT COLUMN_CREATE('color', 'blue')");
    assert_dynamic_column_exec_fails(db, "SELECT column_get('', 'color' AS CHAR(16))");
    assert_dynamic_column_exec_fails(db, "SELECT COLUMN_ADD('', 'size', 'XL')");
    assert_dynamic_column_exec_fails(db, "SELECT COLUMN_DELETE('', 'size')");
    assert_dynamic_column_exec_fails(db, "SELECT COLUMN_CHECK('')");
    assert_dynamic_column_exec_fails(db, "SELECT COLUMN_EXISTS('', 'size')");
    assert_dynamic_column_exec_fails(db, "SELECT COLUMN_JSON('')");
    assert_dynamic_column_exec_fails(db, "SELECT COLUMN_LIST('')");
    assert_dynamic_column_exec_fails(db, "/*! SELECT COLUMN_CREATE('commented', 'value') */");
    assert(
        mylite_exec(
            db,
            "SELECT 'COLUMN_CREATE(' AS quoted_text, JSON_VALID('{\"ok\": true}')",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_sequence_sql_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_sequence_exec_fails(db, "SELECT NEXT VALUE FOR blocked_sequence");
    assert_sequence_exec_fails(db, "SELECT PREVIOUS VALUE FOR blocked_sequence");
    assert_sequence_exec_fails(db, "SELECT NEXTVAL(blocked_sequence)");
    assert_sequence_exec_fails(db, "SELECT LASTVAL(blocked_sequence)");
    assert_sequence_exec_fails(db, "SELECT SETVAL(blocked_sequence, 1)");
    assert_sequence_exec_fails(db, "/*! SELECT NEXTVAL(blocked_sequence) */");
    assert_sequence_exec_fails(db, "/*!50600 SELECT SETVAL(blocked_sequence, 1) */");
    assert(
        mylite_exec(
            db,
            "SELECT 'NEXTVAL(blocked_sequence)' AS nextval_text, "
            "'NEXT VALUE FOR blocked_sequence' AS next_value_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_user_statistics_sql_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_user_statistics_exec_fails(db, "SHOW USER_STATISTICS");
    assert_user_statistics_exec_fails(db, "SHOW CLIENT_STATISTICS");
    assert_user_statistics_exec_fails(db, "SHOW INDEX_STATISTICS");
    assert_user_statistics_exec_fails(db, "SHOW TABLE_STATISTICS");
    assert_user_statistics_exec_fails(db, "FLUSH USER_STATISTICS");
    assert_user_statistics_exec_fails(db, "FLUSH CLIENT_STATISTICS");
    assert_user_statistics_exec_fails(db, "FLUSH TABLE_STATISTICS, INDEX_STATISTICS");
    assert_user_statistics_exec_fails(db, "SET userstat=1");
    assert_user_statistics_exec_fails(db, "SET GLOBAL userstat=1");
    assert_user_statistics_exec_fails(db, "SET SESSION userstat=1");
    assert_user_statistics_exec_fails(db, "SET @@global.userstat=1");
    assert_user_statistics_exec_fails(db, "SELECT * FROM INFORMATION_SCHEMA.USER_STATISTICS");
    assert_user_statistics_exec_fails(db, "SELECT * FROM information_schema.table_statistics");
    assert_user_statistics_exec_fails(db, "SELECT * FROM `information_schema`.`USER_STATISTICS`");
    assert(mylite_exec(db, "SET @userstat=1", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "SELECT 'USER_STATISTICS' AS user_statistics_text", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(
        mylite_exec(
            db,
            "SELECT 'INFORMATION_SCHEMA.USER_STATISTICS' AS user_statistics_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_xml_sql_functions_are_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_xml_sql_function_exec_fails(db, "SELECT EXTRACTVALUE('<a>text</a>', '/a')");
    assert_xml_sql_function_exec_fails(db, "SELECT UPDATEXML('<a>old</a>', '/a', '<a>new</a>')");
    assert(
        mylite_exec(
            db,
            "SELECT 'EXTRACTVALUE(' AS extractvalue_text, 'UPDATEXML(' AS updatexml_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_oracle_sql_mode_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "SET sql_mode='STRICT_TRANS_TABLES'", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET sql_mode=''", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET @sql_mode='ORACLE'", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "SET sql_mode='STRICT_TRANS_TABLES', @mylite_mode='ORACLE'",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert_oracle_sql_mode_exec_fails(db, "SET sql_mode=ORACLE");
    assert_oracle_sql_mode_exec_fails(db, "SET sql_mode='ORACLE'");
    assert_oracle_sql_mode_exec_fails(db, "SET SESSION sql_mode='ANSI,ORACLE'");
    assert_oracle_sql_mode_exec_fails(db, "SET @@sql_mode='ORACLE'");
    assert_oracle_sql_mode_exec_fails(db, "SET @@session.sql_mode='ORACLE'");
    assert_oracle_sql_mode_exec_fails(db, "SET sql_mode=IF(0, 'STRICT_TRANS_TABLES', 'ORACLE')");
    assert_oracle_sql_mode_exec_fails(db, "SET @mylite_mode='ANSI', sql_mode='ORACLE'");

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_file_import_policy_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE load_target (id INT NOT NULL PRIMARY KEY)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert_file_import_exec_fails(
        db,
        "LOAD DATA INFILE '/tmp/mylite-load.csv' INTO TABLE load_target"
    );
    assert_file_import_exec_fails(
        db,
        "LOAD DATA LOCAL INFILE '/tmp/mylite-load.csv' INTO TABLE load_target"
    );
    assert_file_import_exec_fails(
        db,
        "LOAD XML INFILE '/tmp/mylite-load.xml' INTO TABLE load_target"
    );
    assert_file_import_exec_fails(db, "SELECT LOAD_FILE('/tmp/mylite-load.txt')");
    assert_file_import_exec_fails(db, "/*! SELECT LOAD_FILE('/tmp/mylite-load.txt') */");
    assert(
        mylite_exec(
            db,
            "SELECT 'LOAD DATA INFILE', 'LOAD_FILE(' AS quoted_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_file_export_policy_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_file_export_exec_fails(db, "SELECT 1 INTO OUTFILE '/tmp/mylite-out.csv'");
    assert_file_export_exec_fails(db, "SELECT 'abc' INTO DUMPFILE '/tmp/mylite-out.bin'");
    assert_file_export_exec_fails(db, "/*! SELECT 1 INTO OUTFILE '/tmp/mylite-out.csv' */");
    assert(
        mylite_exec(
            db,
            "SELECT 'INTO OUTFILE', 'INTO DUMPFILE' AS quoted_text",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(mylite_exec(db, "SELECT 7 INTO @outfile", NULL, NULL, NULL) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_non_table_objects_are_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);

    assert_non_table_object_exec_fails(db, "CREATE VIEW blocked_view AS SELECT 1");
    assert_non_table_object_exec_fails(db, "CREATE OR REPLACE VIEW blocked_view AS SELECT 1");
    assert_non_table_object_exec_fails(
        db,
        "CREATE TRIGGER blocked_trigger BEFORE INSERT ON missing_table "
        "FOR EACH ROW SET @mylite_blocked = 1"
    );
    assert_non_table_object_exec_fails(db, "CREATE PROCEDURE blocked_proc() SELECT 1");
    assert_non_table_object_exec_fails(db, "CREATE FUNCTION blocked_func() RETURNS INT RETURN 1");
    assert_non_table_object_exec_fails(
        db,
        "CREATE FUNCTION blocked_udf RETURNS INTEGER SONAME 'blocked_udf.so'"
    );
    assert_non_table_object_exec_fails(db, "CALL blocked_proc()");
    assert_non_table_object_exec_fails(db, "SHOW CREATE PROCEDURE blocked_proc");
    assert_non_table_object_exec_fails(db, "SHOW CREATE FUNCTION blocked_func");
    assert_non_table_object_exec_fails(db, "SHOW CREATE VIEW blocked_view");
    assert_non_table_object_exec_fails(db, "DROP VIEW blocked_view");
    assert_non_table_object_exec_fails(db, "CREATE SEQUENCE blocked_seq");

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_view_metadata_is_empty(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    scalar_context view_count = {
        .column_name = "view_count",
        .value = "0",
        .rows = 0,
    };

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) AS view_count FROM INFORMATION_SCHEMA.VIEWS",
            scalar_callback,
            &view_count,
            NULL
        ) == MYLITE_OK
    );
    assert(view_count.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_trigger_metadata_is_empty(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    int show_trigger_rows = 0;
    scalar_context trigger_count = {
        .column_name = "trigger_count",
        .value = "0",
        .rows = 0,
    };

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "SHOW TRIGGERS", count_only_callback, &show_trigger_rows, NULL) == MYLITE_OK
    );
    assert(show_trigger_rows == 0);
    assert(
        mylite_exec(
            db,
            "SELECT COUNT(*) AS trigger_count FROM INFORMATION_SCHEMA.TRIGGERS",
            scalar_callback,
            &trigger_count,
            NULL
        ) == MYLITE_OK
    );
    assert(trigger_count.rows == 1);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_transaction_control_policy(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "START TRANSACTION", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "START TRANSACTION", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "COMMIT", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "BEGIN WORK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "COMMIT WORK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK WORK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET autocommit=0", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "START TRANSACTION", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET @@session.autocommit=1", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET SESSION autocommit=OFF", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET LOCAL autocommit=TRUE", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET @@autocommit=0", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET @@autocommit=DEFAULT", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET autocommit=0", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET autocommit=DEFAULT", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET SESSION autocommit=OFF", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET SESSION autocommit=DEFAULT", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET SESSION autocommit=OFF", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET @@session.autocommit=DEFAULT", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET LOCAL autocommit=FALSE", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET LOCAL autocommit=DEFAULT", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET autocommit=ON", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET TRANSACTION READ WRITE", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET SESSION TRANSACTION READ WRITE", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET LOCAL TRANSACTION READ WRITE", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(
        mylite_exec(db, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(
        mylite_exec(
            db,
            "SET LOCAL TRANSACTION ISOLATION LEVEL REPEATABLE READ",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_exec(
            db,
            "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED, READ WRITE",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_exec(
            db,
            "SET TRANSACTION READ ONLY, ISOLATION LEVEL SERIALIZABLE",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "START TRANSACTION READ ONLY", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET TRANSACTION READ ONLY", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET SESSION TRANSACTION READ ONLY", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "START TRANSACTION", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET LOCAL TRANSACTION READ WRITE", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "SET transaction_isolation='READ-COMMITTED'", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(
        mylite_exec(db, "SET SESSION tx_isolation='SERIALIZABLE'", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(
        mylite_exec(db, "SET @@transaction_isolation='READ-UNCOMMITTED'", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(mylite_exec(db, "SET transaction_read_only=1", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET transaction_read_only=0", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET @@transaction_read_only=1", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "SET @@session.transaction_read_only=ON", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(mylite_exec(db, "START TRANSACTION", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET LOCAL tx_read_only=OFF", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET completion_type=NO_CHAIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET @@completion_type=0", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET SESSION completion_type=DEFAULT", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "SET sql_mode='', completion_type=NO_CHAIN", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(
        mylite_exec(db, "SET completion_type=NO_CHAIN, autocommit=0", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(mylite_exec(db, "SET autocommit=ON", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET completion_type=CHAIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET @@session.completion_type=1", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "SET sql_mode='', completion_type=CHAIN", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(mylite_exec(db, "SET completion_type=DEFAULT", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "START TRANSACTION READ WRITE", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "COMMIT AND NO CHAIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "COMMIT AND CHAIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK AND NO CHAIN NO RELEASE", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK AND CHAIN NO RELEASE", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "COMMIT NO RELEASE", NULL, NULL, NULL) == MYLITE_OK);
    assert_transaction_control_exec_fails(db, "START TRANSACTION WITH CONSISTENT SNAPSHOT");
    assert_transaction_control_exec_fails(db, "START TRANSACTION READ WRITE, READ ONLY");
    assert_transaction_control_exec_fails(db, "COMMIT RELEASE");
    assert_transaction_control_exec_fails(db, "ROLLBACK RELEASE");
    assert_transaction_control_exec_fails(db, "COMMIT AND CHAIN RELEASE");
    assert_transaction_control_exec_fails(db, "SAVEPOINT mylite_probe");
    assert_transaction_control_exec_fails(db, "SAVEPOINT `quoted probe`");
    assert_transaction_control_exec_fails(db, "SAVEPOINT \"double probe\"");
    assert_transaction_control_exec_fails(db, "SAVEPOINT ``");
    assert_transaction_control_exec_fails(db, "SAVEPOINT `unterminated");
    assert_transaction_control_exec_fails(db, "SAVEPOINT `quoted probe`; SELECT 1");
    assert_transaction_control_exec_fails(db, "ROLLBACK TO SAVEPOINT mylite_probe");
    assert_transaction_control_exec_fails(db, "RELEASE SAVEPOINT mylite_probe");
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SAVEPOINT mylite_probe", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK TO SAVEPOINT mylite_probe", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK TO mylite_probe", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "RELEASE SAVEPOINT mylite_probe", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SAVEPOINT `quoted ``probe`", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "ROLLBACK TO SAVEPOINT `quoted ``probe`", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(mylite_exec(db, "ROLLBACK TO `quoted ``probe`", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "RELEASE SAVEPOINT `quoted ``probe`", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SAVEPOINT Case_Probe", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK TO SAVEPOINT case_probe", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK TO CASE_PROBE", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "RELEASE SAVEPOINT case_probe", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET sql_mode='ANSI_QUOTES'", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SAVEPOINT \"double \"\"probe\"", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "ROLLBACK TO SAVEPOINT \"double \"\"probe\"", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(mylite_exec(db, "ROLLBACK TO \"double \"\"probe\"", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "RELEASE SAVEPOINT \"double \"\"probe\"", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(mylite_exec(db, "ROLLBACK", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET sql_mode=''", NULL, NULL, NULL) == MYLITE_OK);
    assert_transaction_control_exec_fails(db, "SET GLOBAL autocommit=0");
    assert_transaction_control_exec_fails(db, "SET GLOBAL autocommit=DEFAULT");
    assert_transaction_control_exec_fails(db, "SET @@global.autocommit=0");
    assert_transaction_control_exec_fails(db, "SET @@global.autocommit=DEFAULT");
    assert_transaction_control_exec_fails(db, "SET @@completion_type=RELEASE");
    assert_transaction_control_exec_fails(db, "SET STATEMENT completion_type=CHAIN FOR SELECT 1");
    assert_transaction_control_exec_fails(
        db,
        "SET STATEMENT completion_type=NO_CHAIN FOR SELECT 1"
    );
    assert_transaction_control_exec_fails(db, "SET GLOBAL TRANSACTION READ WRITE");
    assert_transaction_control_exec_fails(db, "SET GLOBAL TRANSACTION READ ONLY");
    assert_transaction_control_exec_fails(
        db,
        "SET GLOBAL TRANSACTION ISOLATION LEVEL READ COMMITTED"
    );
    assert_transaction_control_exec_fails(db, "SET TRANSACTION READ WRITE, READ ONLY");
    assert_transaction_control_exec_fails(
        db,
        "SET TRANSACTION ISOLATION LEVEL READ COMMITTED, ISOLATION LEVEL SERIALIZABLE"
    );
    assert_transaction_control_exec_fails(
        db,
        "SET TRANSACTION ISOLATION LEVEL READ COMMITTED; SELECT 1"
    );
    assert_transaction_control_exec_fails(db, "SET GLOBAL completion_type=NO_CHAIN");
    assert_transaction_control_exec_fails(db, "SET @@global.completion_type=0");
    assert_transaction_control_exec_fails(db, "SET completion_type=2");
    assert_transaction_control_exec_fails(db, "SET completion_type=CHAIN, completion_type=RELEASE");
    assert(
        mylite_exec(db, "SET completion_type=CHAIN, completion_type=NO_CHAIN", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "COMMIT", NULL, NULL, NULL) == MYLITE_OK);
    assert_transaction_control_exec_fails(db, "SAVEPOINT completion_type_probe");
    assert(
        mylite_exec(db, "SET completion_type=NO_CHAIN, completion_type=CHAIN", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(mylite_exec(db, "BEGIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "COMMIT", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SAVEPOINT completion_type_probe", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "ROLLBACK AND NO CHAIN", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET completion_type=DEFAULT", NULL, NULL, NULL) == MYLITE_OK);
    assert_transaction_control_exec_fails(db, "SET completion_type=NO_CHAIN; SELECT 1");
    assert_transaction_control_exec_fails(db, "SET GLOBAL transaction_isolation='READ-COMMITTED'");
    assert_transaction_control_exec_fails(db, "SET @@global.tx_isolation='READ-COMMITTED'");
    assert_transaction_control_exec_fails(db, "SET GLOBAL transaction_read_only=1");
    assert_transaction_control_exec_fails(db, "SET @@global.tx_read_only=1");
    assert_transaction_control_exec_fails(db, "SET DEFAULT.transaction_read_only=1");
    assert_transaction_control_exec_fails(
        db,
        "SET @@DEFAULT.transaction_isolation='READ-COMMITTED'"
    );
    assert_transaction_control_exec_fails(db, "SET transaction_read_only=2");
    assert(
        mylite_exec(
            db,
            "SET transaction_isolation='READ-COMMITTED', tx_isolation='SERIALIZABLE'",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_exec(db, "SET transaction_read_only=1, tx_read_only=0", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(
        mylite_exec(db, "SET tx_read_only=0, transaction_read_only=1", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(mylite_exec(db, "SET transaction_read_only=0", NULL, NULL, NULL) == MYLITE_OK);
    assert_transaction_control_exec_fails(
        db,
        "SET transaction_read_only=1, @@global.tx_read_only=0"
    );
    assert_transaction_control_exec_fails(
        db,
        "SET transaction_isolation='READ-COMMITTED'; SELECT 1"
    );
    assert_transaction_control_exec_fails(
        db,
        "SET STATEMENT transaction_isolation='READ-COMMITTED' FOR SELECT 1"
    );
    assert(mylite_exec(db, "SET autocommit=0, sql_mode='ANSI'", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "SET sql_mode='', autocommit=1", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(db, "SET SESSION sql_mode='ANSI', autocommit=OFF", NULL, NULL, NULL) ==
        MYLITE_OK
    );
    assert(
        mylite_exec(
            db,
            "SET @mylite_transaction_label='transaction', autocommit=DEFAULT",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert_transaction_control_exec_fails(db, "SET autocommit=0, autocommit=1");
    assert(
        mylite_exec(db, "SET autocommit=0, completion_type=CHAIN", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(
        mylite_exec(db, "SET autocommit=ON, completion_type=DEFAULT", NULL, NULL, NULL) == MYLITE_OK
    );
    assert_transaction_control_exec_fails(db, "SET autocommit=0, transaction_read_only=1");
    assert(
        mylite_exec(
            db,
            "SET autocommit=0, transaction_isolation='READ-COMMITTED'",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(mylite_exec(db, "SET autocommit=1", NULL, NULL, NULL) == MYLITE_OK);
    assert_transaction_control_exec_fails(db, "SET autocommit=0; SELECT 1");
    assert_transaction_control_exec_fails(db, "SET autocommit=DEFAULT; SELECT 1");
    assert_transaction_control_exec_fails(db, "XA START 'mylite-xid'");
    assert(
        mylite_exec(db, "SET @mylite_transaction_label='transaction'", NULL, NULL, NULL) ==
        MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_locking_sql_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE lock_probe (id INT NOT NULL PRIMARY KEY)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(mylite_exec(db, "INSERT INTO lock_probe VALUES (1)", NULL, NULL, NULL) == MYLITE_OK);

    assert_locking_sql_exec_fails(db, "LOCK TABLES lock_probe WRITE");
    assert_locking_sql_exec_fails(db, "UNLOCK TABLES");
    assert_locking_sql_exec_fails(db, "SELECT id FROM lock_probe FOR UPDATE");
    assert_locking_sql_exec_fails(db, "SELECT id FROM lock_probe LOCK IN SHARE MODE");
    assert_locking_sql_exec_fails(db, "SELECT GET_LOCK('mylite-lock', 1)");
    assert_locking_sql_exec_fails(db, "SELECT RELEASE_LOCK('mylite-lock')");
    assert_locking_sql_exec_fails(db, "DO GET_LOCK('mylite-lock', 1)");
    assert(mylite_exec(db, "SELECT 'FOR UPDATE' AS quoted_text", NULL, NULL, NULL) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_online_alter_policy_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE online_alter_probe (id INT NOT NULL PRIMARY KEY)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert_online_alter_exec_fails(
        db,
        "ALTER ONLINE TABLE online_alter_probe ADD COLUMN blocked_online INT"
    );
    assert_online_alter_exec_fails(
        db,
        "ALTER TABLE online_alter_probe ADD COLUMN blocked_inplace INT, ALGORITHM=INPLACE"
    );
    assert_online_alter_exec_fails(
        db,
        "ALTER TABLE online_alter_probe ADD COLUMN blocked_instant INT, ALGORITHM=INSTANT"
    );
    assert_online_alter_exec_fails(
        db,
        "ALTER TABLE online_alter_probe ADD COLUMN blocked_nocopy INT, ALGORITHM=NOCOPY"
    );
    assert_online_alter_exec_fails(
        db,
        "ALTER OFFLINE TABLE online_alter_probe ADD COLUMN blocked_offline_inplace INT, "
        "ALGORITHM=INPLACE"
    );
    assert_online_alter_exec_fails(
        db,
        "ALTER TABLE online_alter_probe ADD COLUMN blocked_lock INT, LOCK=NONE"
    );
    assert(
        mylite_exec(db, "SELECT 'ALGORITHM=INPLACE' AS quoted_text", NULL, NULL, NULL) == MYLITE_OK
    );

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_foreign_key_policy_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE fk_parent (id INT NOT NULL PRIMARY KEY)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE fk_child (id INT NOT NULL PRIMARY KEY, parent_id INT)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE fk_comment_only ("
            "id INT COMMENT 'REFERENCES fk_parent(id)')",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE fk_quoted_only (`references` INT)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert_foreign_key_exec_fails(
        db,
        "CREATE TEMPORARY TABLE fk_blocked_table ("
        "id INT NOT NULL PRIMARY KEY, parent_id INT, "
        "CONSTRAINT fk_parent FOREIGN KEY (parent_id) REFERENCES fk_parent(id))"
    );
    assert_foreign_key_exec_fails(
        db,
        "CREATE TEMPORARY TABLE fk_blocked_column ("
        "id INT NOT NULL PRIMARY KEY, parent_id INT REFERENCES fk_parent(id))"
    );
    assert_foreign_key_exec_fails(
        db,
        "ALTER TABLE fk_child ADD CONSTRAINT fk_child_parent "
        "FOREIGN KEY (parent_id) REFERENCES fk_parent(id)"
    );
    assert_foreign_key_exec_fails(db, "ALTER TABLE fk_child DROP FOREIGN KEY fk_child_parent");

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_unsupported_engine_policy_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TABLE csv_comment ("
            "id INT COMMENT 'ENGINE=CSV ENGINE=ARCHIVE', engine_label VARCHAR(16))",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert_csv_engine_exec_fails(
        db,
        "CREATE TABLE csv_posts (id INT NOT NULL PRIMARY KEY) ENGINE=CSV"
    );
    assert_csv_engine_exec_fails(
        db,
        "CREATE TEMPORARY TABLE csv_temp_posts (id INT NOT NULL PRIMARY KEY) ENGINE = CSV"
    );
    assert_csv_engine_exec_fails(
        db,
        "CREATE TABLE csv_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE CSV"
    );
    assert_csv_engine_exec_fails(
        db,
        "CREATE TEMPORARY TABLE csv_quoted_posts (id INT NOT NULL PRIMARY KEY) ENGINE='CSV'"
    );
    assert_csv_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE=CSV");
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE archive_posts (id INT NOT NULL PRIMARY KEY) ENGINE=ARCHIVE"
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE archive_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE ARCHIVE"
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE connect_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE CONNECT"
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE federated_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE FEDERATED"
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE mrg_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE MRG_MyISAM"
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE archive_quoted_posts (id INT NOT NULL PRIMARY KEY) ENGINE='ARCHIVE'"
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE sequence_posts (id INT NOT NULL PRIMARY KEY) ENGINE=SEQUENCE"
    );
    assert_unsupported_engine_exec_fails(
        db,
        "CREATE TABLE sequence_no_equal_posts (id INT NOT NULL PRIMARY KEY) ENGINE SEQUENCE"
    );
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE=ARCHIVE");
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE ARCHIVE");
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE ROCKSDB");
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE=SEQUENCE");
    assert_unsupported_engine_exec_fails(db, "ALTER TABLE csv_comment ENGINE SEQUENCE");

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_partition_policy_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE partition_comment ("
            "id INT COMMENT 'PARTITION BY HASH (id)', partition_label VARCHAR(16))",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    assert_partition_exec_fails(
        db,
        "CREATE TABLE partitioned_probe (id INT NOT NULL PRIMARY KEY) "
        "PARTITION BY HASH (id) PARTITIONS 2"
    );
    assert_partition_exec_fails(
        db,
        "ALTER TABLE partition_comment ADD PARTITION (PARTITION p1 VALUES LESS THAN (10))"
    );
    assert_partition_exec_fails(db, "ALTER TABLE partition_comment REMOVE PARTITIONING");

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void assert_variable_value(mylite_db *db, const char *name, const char *value) {
    variable_context ctx = {
        .name = name,
        .value = value,
        .rows = 0,
    };
    char sql[128];
    const int written = snprintf(sql, sizeof(sql), "SHOW VARIABLES LIKE '%s'", name);

    assert(written > 0);
    assert((size_t)written < sizeof(sql));
    assert(mylite_exec(db, sql, variable_callback, &ctx, NULL) == MYLITE_OK);
    if (ctx.rows != 1) {
        fprintf(stderr, "variable not found or duplicate: %s rows=%d\n", name, ctx.rows);
    }
    assert(ctx.rows == 1);
}

static void assert_variable_value_or_missing(mylite_db *db, const char *name, const char *value) {
    variable_context ctx = {
        .name = name,
        .value = value,
        .rows = 0,
    };
    char sql[128];
    const int written = snprintf(sql, sizeof(sql), "SHOW VARIABLES LIKE '%s'", name);

    assert(written > 0);
    assert((size_t)written < sizeof(sql));
    assert(mylite_exec(db, sql, variable_name_callback, &ctx, NULL) == MYLITE_OK);
    if (ctx.rows > 1) {
        fprintf(stderr, "variable duplicate: %s rows=%d\n", name, ctx.rows);
    }
    assert(ctx.rows == 0 || ctx.rows == 1);
}

static void assert_variable_missing(mylite_db *db, const char *name) {
    variable_context ctx = {
        .name = name,
        .value = "",
        .rows = 0,
    };
    char sql[128];
    const int written = snprintf(sql, sizeof(sql), "SHOW VARIABLES LIKE '%s'", name);

    assert(written > 0);
    assert((size_t)written < sizeof(sql));
    assert(mylite_exec(db, sql, variable_callback, &ctx, NULL) == MYLITE_OK);
    if (ctx.rows != 0) {
        fprintf(stderr, "variable unexpectedly present: %s rows=%d\n", name, ctx.rows);
    }
    assert(ctx.rows == 0);
}

static void assert_engine_missing(mylite_db *db, const char *name) {
    engine_context ctx = {
        .name = name,
        .rows = 0,
    };

    assert(mylite_exec(db, "SHOW ENGINES", engine_name_callback, &ctx, NULL) == MYLITE_OK);
    if (ctx.rows != 0) {
        fprintf(stderr, "engine unexpectedly present: %s rows=%d\n", name, ctx.rows);
    }
    assert(ctx.rows == 0);
}

static void assert_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    const int rc = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != MYLITE_ERROR) {
        fprintf(stderr, "expected failure for SQL: %s\n", sql);
    }
    assert(rc == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "server-oriented") != NULL);
    mylite_free(errmsg);
}

static void assert_replication_filter_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "replication filter") != NULL);
    mylite_free(errmsg);
}

static void assert_binlog_replication_variable_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "binlog/replication system variable") != NULL);
    mylite_free(errmsg);
}

static void assert_backup_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "external backup") != NULL);
    mylite_free(errmsg);
}

static void assert_query_cache_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "query-cache") != NULL);
    mylite_free(errmsg);
}

static void assert_statement_profiling_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "statement-profiling") != NULL);
    mylite_free(errmsg);
}

static void assert_optimizer_trace_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "optimizer-trace") != NULL);
    mylite_free(errmsg);
}

static void assert_table_maintenance_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "table-maintenance") != NULL);
    mylite_free(errmsg);
}

static void assert_sql_handler_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "SQL HANDLER") != NULL);
    mylite_free(errmsg);
}

static void assert_help_command_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "HELP SQL command") != NULL);
    mylite_free(errmsg);
}

static void assert_static_show_info_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "static SHOW information") != NULL);
    mylite_free(errmsg);
}

static void assert_processlist_metadata_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "process-list") != NULL);
    mylite_free(errmsg);
}

static void assert_procedure_analyse_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "PROCEDURE ANALYSE") != NULL);
    mylite_free(errmsg);
}

static void assert_select_procedure_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "SELECT PROCEDURE") != NULL);
    mylite_free(errmsg);
}

static void assert_server_utility_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "server utility") != NULL);
    mylite_free(errmsg);
}

static void assert_gis_sql_function_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "GIS SQL function") != NULL);
    mylite_free(errmsg);
}

static void assert_vector_sql_function_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "vector SQL function") != NULL);
    mylite_free(errmsg);
}

static void assert_sformat_sql_function_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "SFORMAT SQL function") != NULL);
    mylite_free(errmsg);
}

static void assert_json_schema_valid_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "JSON_SCHEMA_VALID") != NULL);
    mylite_free(errmsg);
}

static void assert_json_table_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "JSON_TABLE") != NULL);
    mylite_free(errmsg);
}

static void assert_dynamic_column_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "dynamic column") != NULL);
    mylite_free(errmsg);
}

static void assert_sequence_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "SQL sequence") != NULL);
    mylite_free(errmsg);
}

static void assert_user_statistics_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "user-statistics") != NULL);
    mylite_free(errmsg);
}

static void assert_xml_sql_function_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "XML SQL function") != NULL);
    mylite_free(errmsg);
}

static void assert_oracle_sql_mode_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "Oracle SQL mode") != NULL);
    mylite_free(errmsg);
}

static void assert_file_import_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "file import") != NULL);
    mylite_free(errmsg);
}

static void assert_file_export_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "file export") != NULL);
    mylite_free(errmsg);
}

static void assert_non_table_object_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
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

static void assert_locking_sql_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "SQL locking") != NULL);
    mylite_free(errmsg);
}

static void assert_online_alter_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "online ALTER") != NULL);
    mylite_free(errmsg);
}

static void assert_csv_engine_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "CSV storage engine") != NULL);
    mylite_free(errmsg);
}

static void assert_unsupported_engine_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "storage engine request") != NULL);
    mylite_free(errmsg);
}

static void assert_partition_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "partition") != NULL);
    mylite_free(errmsg);
}

static void assert_foreign_key_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(errmsg != NULL);
    assert(
        strstr(errmsg, "foreign-key") != NULL || strstr(errmsg, "foreign key") != NULL ||
        strstr(errmsg, "Foreign key") != NULL || mylite_mariadb_errno(db) != 0U
    );
    mylite_free(errmsg);
}

static int select_callback(void *ctx, int column_count, char **values, char **column_names) {
    select_context *select_ctx = (select_context *)ctx;
    assert(column_count == 2);
    assert(strcmp(column_names[0], "one") == 0);
    assert(strcmp(column_names[1], "empty") == 0);
    assert(values[0] != NULL);
    assert(strcmp(values[0], "1") == 0);
    assert(values[1] == NULL);
    ++select_ctx->rows;
    return 0;
}

static int scalar_callback(void *ctx, int column_count, char **values, char **column_names) {
    scalar_context *scalar_ctx = (scalar_context *)ctx;

    assert(column_count == 1);
    assert(strcmp(column_names[0], scalar_ctx->column_name) == 0);
    assert(values[0] != NULL);
    assert(strcmp(values[0], scalar_ctx->value) == 0);
    ++scalar_ctx->rows;
    return 0;
}

static int nullable_scalar_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    nullable_scalar_context *scalar_ctx = (nullable_scalar_context *)ctx;

    assert(column_count == 1);
    assert(strcmp(column_names[0], scalar_ctx->column_name) == 0);
    assert(values[0] == NULL);
    ++scalar_ctx->rows;
    return 0;
}

static int abort_callback(void *ctx, int column_count, char **values, char **column_names) {
    int *callback_count = (int *)ctx;
    (void)column_count;
    (void)values;
    (void)column_names;
    ++*callback_count;
    return 1;
}

static int count_only_callback(void *ctx, int column_count, char **values, char **column_names) {
    int *row_count = (int *)ctx;
    (void)column_count;
    (void)values;
    (void)column_names;
    ++*row_count;
    return 0;
}

static int variable_callback(void *ctx, int column_count, char **values, char **column_names) {
    variable_context *variable_ctx = (variable_context *)ctx;
    (void)column_names;

    assert(column_count == 2);
    assert(values[0] != NULL);
    assert(values[1] != NULL);
    assert(strcmp(values[0], variable_ctx->name) == 0);
    assert(strcmp(values[1], variable_ctx->value) == 0);
    ++variable_ctx->rows;
    return 0;
}

static int variable_name_callback(void *ctx, int column_count, char **values, char **column_names) {
    variable_context *variable_ctx = (variable_context *)ctx;
    (void)column_names;

    assert(column_count == 2);
    assert(values[0] != NULL);
    assert(strcmp(values[0], variable_ctx->name) == 0);
    ++variable_ctx->rows;
    return 0;
}

static int engine_name_callback(void *ctx, int column_count, char **values, char **column_names) {
    engine_context *engine_ctx = (engine_context *)ctx;
    (void)column_names;

    assert(column_count >= 1);
    assert(values[0] != NULL);
    if (strcmp(values[0], engine_ctx->name) == 0) {
        ++engine_ctx->rows;
    }
    return 0;
}

static mylite_db *open_database(const char *root, char **filename) {
    char *runtime_root = path_join(root, "runtime");
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = runtime_root,
    };
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    *filename = path_join(root, "exec.mylite");
    assert(
        mylite_open_v2(*filename, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    free(runtime_root);
    return db;
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-exec.XXXXXX";
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

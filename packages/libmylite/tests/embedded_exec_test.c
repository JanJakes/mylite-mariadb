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

static void test_select_callback(void);
static void test_statement_effects(void);
static void test_callback_abort(void);
static void test_syntax_error_diagnostics(void);
static void test_server_surfaces_are_disabled(void);
static void test_table_maintenance_sql_is_rejected(void);
static void test_help_command_is_rejected(void);
static void test_procedure_analyse_is_rejected(void);
static void test_server_utility_functions_are_rejected(void);
static void test_gis_sql_functions_are_rejected(void);
static void test_sformat_sql_function_is_rejected(void);
static void test_json_schema_valid_sql_function_is_rejected(void);
static void test_json_table_function_is_rejected(void);
static void test_dynamic_column_functions_are_rejected(void);
static void test_xml_sql_functions_are_rejected(void);
static void test_oracle_sql_mode_is_rejected(void);
static void test_file_import_policy_is_rejected(void);
static void test_file_export_policy_is_rejected(void);
static void test_non_table_objects_are_rejected(void);
static void test_transaction_control_is_rejected(void);
static void test_locking_sql_is_rejected(void);
static void test_online_alter_policy_is_rejected(void);
static void test_foreign_key_policy_is_rejected(void);
static void test_partition_policy_is_rejected(void);
static void assert_variable_value(mylite_db *db, const char *name, const char *value);
static void assert_variable_value_or_missing(mylite_db *db, const char *name, const char *value);
static void assert_exec_fails(mylite_db *db, const char *sql);
static void assert_table_maintenance_exec_fails(mylite_db *db, const char *sql);
static void assert_help_command_exec_fails(mylite_db *db, const char *sql);
static void assert_procedure_analyse_exec_fails(mylite_db *db, const char *sql);
static void assert_select_procedure_exec_fails(mylite_db *db, const char *sql);
static void assert_server_utility_exec_fails(mylite_db *db, const char *sql);
static void assert_gis_sql_function_exec_fails(mylite_db *db, const char *sql);
static void assert_sformat_sql_function_exec_fails(mylite_db *db, const char *sql);
static void assert_json_schema_valid_exec_fails(mylite_db *db, const char *sql);
static void assert_json_table_exec_fails(mylite_db *db, const char *sql);
static void assert_dynamic_column_exec_fails(mylite_db *db, const char *sql);
static void assert_xml_sql_function_exec_fails(mylite_db *db, const char *sql);
static void assert_oracle_sql_mode_exec_fails(mylite_db *db, const char *sql);
static void assert_file_import_exec_fails(mylite_db *db, const char *sql);
static void assert_file_export_exec_fails(mylite_db *db, const char *sql);
static void assert_non_table_object_exec_fails(mylite_db *db, const char *sql);
static void assert_transaction_control_exec_fails(mylite_db *db, const char *sql);
static void assert_locking_sql_exec_fails(mylite_db *db, const char *sql);
static void assert_online_alter_exec_fails(mylite_db *db, const char *sql);
static void assert_partition_exec_fails(mylite_db *db, const char *sql);
static void assert_foreign_key_exec_fails(mylite_db *db, const char *sql);
static int select_callback(void *ctx, int column_count, char **values, char **column_names);
static int abort_callback(void *ctx, int column_count, char **values, char **column_names);
static int variable_callback(void *ctx, int column_count, char **values, char **column_names);
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
    test_table_maintenance_sql_is_rejected();
    test_help_command_is_rejected();
    test_procedure_analyse_is_rejected();
    test_server_utility_functions_are_rejected();
    test_gis_sql_functions_are_rejected();
    test_sformat_sql_function_is_rejected();
    test_json_schema_valid_sql_function_is_rejected();
    test_json_table_function_is_rejected();
    test_dynamic_column_functions_are_rejected();
    test_xml_sql_functions_are_rejected();
    test_oracle_sql_mode_is_rejected();
    test_file_import_policy_is_rejected();
    test_file_export_policy_is_rejected();
    test_non_table_objects_are_rejected();
    test_transaction_control_is_rejected();
    test_locking_sql_is_rejected();
    test_online_alter_policy_is_rejected();
    test_foreign_key_policy_is_rejected();
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
    assert_variable_value_or_missing(db, "performance_schema", "OFF");

    assert_exec_fails(db, "CREATE USER 'mylite_probe'@'localhost' IDENTIFIED BY 'secret'");
    assert_exec_fails(db, "GRANT SELECT ON *.* TO 'mylite_probe'@'localhost'");
    assert_exec_fails(db, "SET GLOBAL event_scheduler = ON");
    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert_exec_fails(db, "CREATE EVENT mylite_probe_event ON SCHEDULE EVERY 1 SECOND DO SELECT 1");
    assert_exec_fails(db, "INSTALL SONAME 'mylite_probe'");
    assert_exec_fails(db, "BINLOG 'AAAA'");
    assert_exec_fails(db, "CHANGE MASTER TO MASTER_HOST='example.test'");
    assert_exec_fails(db, "SHOW MASTER STATUS");

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
    assert_non_table_object_exec_fails(db, "DROP VIEW blocked_view");
    assert_non_table_object_exec_fails(db, "CREATE SEQUENCE blocked_seq");

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_transaction_control_is_rejected(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);

    assert_transaction_control_exec_fails(db, "BEGIN");
    assert_transaction_control_exec_fails(db, "START TRANSACTION");
    assert_transaction_control_exec_fails(db, "COMMIT");
    assert_transaction_control_exec_fails(db, "ROLLBACK");
    assert_transaction_control_exec_fails(db, "SAVEPOINT mylite_probe");
    assert_transaction_control_exec_fails(db, "ROLLBACK TO SAVEPOINT mylite_probe");
    assert_transaction_control_exec_fails(db, "RELEASE SAVEPOINT mylite_probe");
    assert_transaction_control_exec_fails(db, "SET autocommit=0");
    assert_transaction_control_exec_fails(db, "SET @@autocommit=0");
    assert_transaction_control_exec_fails(db, "SET @@session.autocommit=0");
    assert_transaction_control_exec_fails(db, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED");
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
    assert(mylite_exec(db, sql, variable_callback, &ctx, NULL) == MYLITE_OK);
    if (ctx.rows > 1) {
        fprintf(stderr, "variable duplicate: %s rows=%d\n", name, ctx.rows);
    }
    assert(ctx.rows == 0 || ctx.rows == 1);
}

static void assert_exec_fails(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "server-oriented") != NULL);
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

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
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
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "foreign-key") != NULL);
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

static int abort_callback(void *ctx, int column_count, char **values, char **column_names) {
    int *callback_count = (int *)ctx;
    (void)column_count;
    (void)values;
    (void)column_names;
    ++*callback_count;
    return 1;
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

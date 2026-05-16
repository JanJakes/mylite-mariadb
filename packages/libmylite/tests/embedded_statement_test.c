#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_scalar_select(void);
static void test_table_roundtrip(void);
static void test_statement_effects(void);
static void test_segment_reads(void);
static void test_reset_reuse_and_destructors(void);
static void test_finalize_before_drain(void);
static void test_close_rejects_active_statement(void);
static void test_prepare_diagnostics(void);
static void test_invalid_indexes(void);
static void assert_prepare_fails_with_message(mylite_db *db, const char *sql, const char *message);
static mylite_stmt *prepare_statement(mylite_db *db, const char *sql);
static mylite_db *open_database(const char *root, char **filename);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static int is_directory_empty(const char *path);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);
static void counting_destructor(void *ptr);

static int destructor_calls = 0;

int main(void) {
    test_scalar_select();
    test_table_roundtrip();
    test_statement_effects();
    test_segment_reads();
    test_reset_reuse_and_destructors();
    test_finalize_before_drain();
    test_close_rejects_active_statement();
    test_prepare_diagnostics();
    test_invalid_indexes();
    return 0;
}

static void test_scalar_select(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    const char *sql = "SELECT ? AS n, CAST(? AS SIGNED) AS i, CAST(? AS UNSIGNED) AS u, "
                      "CAST(? AS DOUBLE) AS d, CAST(? AS CHAR) AS t";
    const char *tail = NULL;
    mylite_stmt *stmt = NULL;

    assert(mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, &tail) == MYLITE_OK);
    assert(stmt != NULL);
    assert(tail == sql + strlen(sql));
    assert(mylite_bind_parameter_count(stmt) == 5U);
    assert(mylite_column_count(stmt) == 5U);
    assert(strcmp(mylite_column_name(stmt, 0U), "n") == 0);
    assert(strcmp(mylite_column_name(stmt, 1U), "i") == 0);
    assert(strcmp(mylite_column_origin_name(stmt, 0U), "") == 0);

    assert(mylite_bind_null(stmt, 1U) == MYLITE_OK);
    assert(mylite_bind_int64(stmt, 2U, -42) == MYLITE_OK);
    assert(mylite_bind_uint64(stmt, 3U, 42U) == MYLITE_OK);
    assert(mylite_bind_double(stmt, 4U, 3.25) == MYLITE_OK);
    assert(
        mylite_bind_text(stmt, 5U, "hello", MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT) == MYLITE_OK
    );

    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_type(stmt, 0U) == MYLITE_TYPE_NULL);
    assert(mylite_column_type(stmt, 1U) == MYLITE_TYPE_INT64);
    assert(mylite_column_int64(stmt, 1U) == -42);
    assert(mylite_column_type(stmt, 2U) == MYLITE_TYPE_UINT64);
    assert(mylite_column_uint64(stmt, 2U) == 42U);
    assert(mylite_column_type(stmt, 3U) == MYLITE_TYPE_DOUBLE);
    assert(fabs(mylite_column_double(stmt, 3U) - 3.25) < 0.001);
    assert(mylite_column_type(stmt, 4U) == MYLITE_TYPE_TEXT);
    assert(mylite_column_bytes(stmt, 4U) == 5U);
    assert(strcmp(mylite_column_text(stmt, 4U), "hello") == 0);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_step(stmt) == MYLITE_DONE);

    assert(mylite_finalize(stmt) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_table_roundtrip(void) {
    const unsigned char payload[] = {'a', '\0', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *insert = NULL;
    mylite_stmt *select = NULL;

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE prepared_values ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "name VARCHAR(32),"
            "payload BLOB)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    insert = prepare_statement(db, "INSERT INTO prepared_values(name, payload) VALUES (?, ?)");
    assert(
        mylite_bind_text(insert, 1U, "alpha", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK
    );
    assert(mylite_bind_blob(insert, 2U, payload, sizeof(payload), MYLITE_TRANSIENT) == MYLITE_OK);
    assert(mylite_step(insert) == MYLITE_DONE);
    assert(mylite_changes(db) == 1);
    assert(mylite_last_insert_id(db) == 1U);
    assert(mylite_finalize(insert) == MYLITE_OK);

    select = prepare_statement(db, "SELECT id, name, payload FROM prepared_values WHERE id=?");
    assert(strcmp(mylite_column_database_name(select, 0U), "app") == 0);
    assert(strcmp(mylite_column_table_name(select, 1U), "prepared_values") == 0);
    assert(strcmp(mylite_column_origin_table_name(select, 1U), "prepared_values") == 0);
    assert(strcmp(mylite_column_origin_name(select, 1U), "name") == 0);
    assert(mylite_column_mariadb_type(select, 0U) != 0U);
    assert(mylite_column_flags(select, 0U) != 0U);
    assert(mylite_column_charset(select, 1U) != 0U);
    assert(mylite_column_decimals(select, 0U) == 0U);
    assert(mylite_column_length(select, 1U) >= 32U);
    assert(mylite_bind_uint64(select, 1U, 1U) == MYLITE_OK);
    assert(mylite_step(select) == MYLITE_ROW);
    assert(mylite_column_type(select, 0U) == MYLITE_TYPE_UINT64);
    assert(mylite_column_uint64(select, 0U) == 1U);
    assert(mylite_column_type(select, 1U) == MYLITE_TYPE_TEXT);
    assert(strcmp(mylite_column_text(select, 1U), "alpha") == 0);
    assert(mylite_column_type(select, 2U) == MYLITE_TYPE_BLOB);
    assert(mylite_column_bytes(select, 2U) == sizeof(payload));
    assert(memcmp(mylite_column_blob(select, 2U), payload, sizeof(payload)) == 0);
    assert(mylite_step(select) == MYLITE_DONE);
    assert(mylite_finalize(select) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_statement_effects(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *insert = NULL;
    mylite_stmt *update = NULL;
    mylite_stmt *delete_stmt = NULL;
    mylite_stmt *select = NULL;

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE prepared_effects ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "name VARCHAR(32),"
            "qty INT NOT NULL)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    insert = prepare_statement(db, "INSERT INTO prepared_effects(name, qty) VALUES (?, ?), (?, ?)");
    assert(
        mylite_bind_text(insert, 1U, "alpha", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK
    );
    assert(mylite_bind_int64(insert, 2U, 1) == MYLITE_OK);
    assert(mylite_bind_text(insert, 3U, "beta", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK);
    assert(mylite_bind_int64(insert, 4U, 2) == MYLITE_OK);
    assert(mylite_step(insert) == MYLITE_DONE);
    assert(mylite_changes(db) == 2);
    assert(mylite_last_insert_id(db) == 1U);
    assert(mylite_finalize(insert) == MYLITE_OK);

    update = prepare_statement(db, "UPDATE prepared_effects SET qty = qty + ? WHERE qty >= ?");
    assert(mylite_bind_int64(update, 1U, 10) == MYLITE_OK);
    assert(mylite_bind_int64(update, 2U, 2) == MYLITE_OK);
    assert(mylite_step(update) == MYLITE_DONE);
    assert(mylite_changes(db) == 1);
    assert(mylite_finalize(update) == MYLITE_OK);

    delete_stmt = prepare_statement(db, "DELETE FROM prepared_effects WHERE qty < ?");
    assert(mylite_bind_int64(delete_stmt, 1U, 5) == MYLITE_OK);
    assert(mylite_step(delete_stmt) == MYLITE_DONE);
    assert(mylite_changes(db) == 1);
    assert(mylite_finalize(delete_stmt) == MYLITE_OK);

    insert = prepare_statement(db, "INSERT INTO prepared_effects(name, qty) VALUES (?, ?)");
    assert(
        mylite_bind_text(insert, 1U, "gamma", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK
    );
    assert(mylite_bind_int64(insert, 2U, 3) == MYLITE_OK);
    assert(mylite_step(insert) == MYLITE_DONE);
    assert(mylite_changes(db) == 1);
    assert(mylite_last_insert_id(db) == 3U);
    assert(mylite_finalize(insert) == MYLITE_OK);

    select = prepare_statement(db, "SELECT id, name, qty FROM prepared_effects ORDER BY id");
    assert(mylite_step(select) == MYLITE_ROW);
    assert(mylite_column_uint64(select, 0U) == 2U);
    assert(strcmp(mylite_column_text(select, 1U), "beta") == 0);
    assert(mylite_column_int64(select, 2U) == 12);
    assert(mylite_step(select) == MYLITE_ROW);
    assert(mylite_column_uint64(select, 0U) == 3U);
    assert(strcmp(mylite_column_text(select, 1U), "gamma") == 0);
    assert(mylite_column_int64(select, 2U) == 3);
    assert(mylite_step(select) == MYLITE_DONE);
    assert(mylite_finalize(select) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_segment_reads(void) {
    unsigned char payload[600];
    unsigned char chunk[17];
    size_t bytes_read = 99U;
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *insert = NULL;
    mylite_stmt *select = NULL;

    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (unsigned char)((i * 37U) & 0xffU);
    }

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE large_values ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "payload LONGBLOB,"
            "missing LONGBLOB)",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    insert = prepare_statement(db, "INSERT INTO large_values(payload, missing) VALUES (?, ?)");
    assert(mylite_bind_blob(insert, 1U, payload, sizeof(payload), MYLITE_TRANSIENT) == MYLITE_OK);
    assert(mylite_bind_null(insert, 2U) == MYLITE_OK);
    assert(mylite_step(insert) == MYLITE_DONE);
    assert(mylite_finalize(insert) == MYLITE_OK);

    select = prepare_statement(db, "SELECT payload, missing, id FROM large_values WHERE id=1");
    assert(mylite_column_read(select, 0U, 0U, chunk, sizeof(chunk), &bytes_read) == MYLITE_MISUSE);
    assert(mylite_step(select) == MYLITE_ROW);

    assert(mylite_column_read(select, 0U, 5U, chunk, sizeof(chunk), &bytes_read) == MYLITE_OK);
    assert(bytes_read == sizeof(chunk));
    assert(memcmp(chunk, payload + 5U, sizeof(chunk)) == 0);
    assert(
        mylite_column_read(select, 0U, sizeof(payload) - 7U, chunk, sizeof(chunk), &bytes_read) ==
        MYLITE_OK
    );
    assert(bytes_read == 7U);
    assert(memcmp(chunk, payload + sizeof(payload) - 7U, bytes_read) == 0);
    assert(mylite_column_read(select, 0U, 0U, NULL, 0U, &bytes_read) == MYLITE_OK);
    assert(bytes_read == 0U);
    assert(
        mylite_column_read(select, 0U, sizeof(payload), chunk, sizeof(chunk), &bytes_read) ==
        MYLITE_OK
    );
    assert(bytes_read == 0U);
    assert(mylite_column_read(select, 1U, 0U, chunk, sizeof(chunk), &bytes_read) == MYLITE_OK);
    assert(bytes_read == 0U);
    assert(mylite_column_read(select, 2U, 0U, chunk, sizeof(chunk), &bytes_read) == MYLITE_MISUSE);
    assert(mylite_column_read(select, 3U, 0U, chunk, sizeof(chunk), &bytes_read) == MYLITE_MISUSE);

    assert(mylite_column_bytes(select, 0U) == sizeof(payload));
    assert(memcmp(mylite_column_blob(select, 0U), payload, sizeof(payload)) == 0);
    assert(mylite_step(select) == MYLITE_DONE);
    assert(mylite_finalize(select) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_reset_reuse_and_destructors(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *stmt = prepare_statement(db, "SELECT CAST(? AS SIGNED)");
    char borrowed[] = "borrowed";

    destructor_calls = 0;
    assert(
        mylite_bind_text(stmt, 1U, borrowed, MYLITE_NUL_TERMINATED, counting_destructor) ==
        MYLITE_OK
    );
    assert(mylite_clear_bindings(stmt) == MYLITE_OK);
    assert(destructor_calls == 1);

    assert(mylite_bind_int64(stmt, 1U, 7) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_int64(stmt, 0U) == 7);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_reset(stmt) == MYLITE_OK);

    assert(mylite_bind_int64(stmt, 1U, 8) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_int64(stmt, 0U) == 8);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_finalize(stmt) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_finalize_before_drain(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *stmt = prepare_statement(db, "SELECT 1 UNION ALL SELECT 2");

    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_int64(stmt, 0U) == 1);
    assert(mylite_finalize(stmt) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_close_rejects_active_statement(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *stmt = prepare_statement(db, "SELECT 1");

    assert(mylite_close(db) == MYLITE_BUSY);
    assert(mylite_errcode(db) == MYLITE_BUSY);
    assert(strstr(mylite_errmsg(db), "active statements") != NULL);
    assert(mylite_finalize(stmt) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_prepare_diagnostics(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *stmt = NULL;

    assert(mylite_prepare(db, "SELEC ?", MYLITE_NUL_TERMINATED, &stmt, NULL) == MYLITE_ERROR);
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) != 0U);
    assert(strcmp(mylite_sqlstate(db), "00000") != 0);

    assert(
        mylite_prepare(
            db,
            "CREATE USER 'mylite_probe'@'localhost' IDENTIFIED BY 'secret'",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "server-oriented") != NULL);

    assert_prepare_fails_with_message(db, "CHECK TABLE maintenance_probe", "table-maintenance");
    assert_prepare_fails_with_message(db, "ANALYZE TABLE maintenance_probe", "table-maintenance");
    assert_prepare_fails_with_message(
        db,
        "ANALYZE LOCAL TABLE maintenance_probe",
        "table-maintenance"
    );
    assert_prepare_fails_with_message(db, "OPTIMIZE TABLE maintenance_probe", "table-maintenance");
    assert_prepare_fails_with_message(
        db,
        "OPTIMIZE NO_WRITE_TO_BINLOG TABLE maintenance_probe",
        "table-maintenance"
    );
    assert_prepare_fails_with_message(db, "REPAIR TABLE maintenance_probe", "table-maintenance");
    assert_prepare_fails_with_message(
        db,
        "REPAIR LOCAL TABLE maintenance_probe",
        "table-maintenance"
    );
    assert_prepare_fails_with_message(
        db,
        "CACHE INDEX maintenance_probe IN keycache",
        "table-maintenance"
    );
    assert_prepare_fails_with_message(
        db,
        "LOAD INDEX INTO CACHE maintenance_probe",
        "table-maintenance"
    );

    assert(
        mylite_prepare(db, "HELP 'contents'", MYLITE_NUL_TERMINATED, &stmt, NULL) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "HELP SQL command") != NULL);

    assert(
        mylite_prepare(db, "SELECT ? PROCEDURE ANALYSE()", MYLITE_NUL_TERMINATED, &stmt, NULL) ==
        MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "PROCEDURE ANALYSE") != NULL);

    assert(
        mylite_prepare(
            db,
            "SELECT ? PROCEDURE unknown_procedure()",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "SELECT PROCEDURE") != NULL);

    assert(
        mylite_prepare(
            db,
            "LOAD DATA INFILE '/tmp/mylite-load.csv' INTO TABLE blocked_load",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "file import") != NULL);

    assert(
        mylite_prepare(
            db,
            "LOAD XML INFILE '/tmp/mylite-load.xml' INTO TABLE blocked_load",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "file import") != NULL);

    assert(
        mylite_prepare(
            db,
            "SELECT LOAD_FILE('/tmp/mylite-load.txt')",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "file import") != NULL);

    assert(
        mylite_prepare(
            db,
            "SELECT ? INTO OUTFILE '/tmp/mylite-out.csv'",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "file export") != NULL);

    assert(
        mylite_prepare(db, "SELECT SLEEP(?)", MYLITE_NUL_TERMINATED, &stmt, NULL) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "server utility") != NULL);

    assert(
        mylite_prepare(db, "SELECT UUID_SHORT()", MYLITE_NUL_TERMINATED, &stmt, NULL) ==
        MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "server utility") != NULL);

    assert(
        mylite_prepare(db, "SELECT ST_AsText(?)", MYLITE_NUL_TERMINATED, &stmt, NULL) ==
        MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "GIS SQL function") != NULL);

    assert(
        mylite_prepare(db, "SELECT ST_GeomFromText(?)", MYLITE_NUL_TERMINATED, &stmt, NULL) ==
        MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "GIS SQL function") != NULL);

    assert(
        mylite_prepare(db, "SELECT Point(?, ?)", MYLITE_NUL_TERMINATED, &stmt, NULL) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "GIS SQL function") != NULL);

    assert(
        mylite_prepare(db, "SELECT X(Point(?, ?))", MYLITE_NUL_TERMINATED, &stmt, NULL) ==
        MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "GIS SQL function") != NULL);

    assert(
        mylite_prepare(db, "SELECT SFORMAT(?, ?)", MYLITE_NUL_TERMINATED, &stmt, NULL) ==
        MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "SFORMAT SQL function") != NULL);

    assert_prepare_fails_with_message(
        db,
        "SELECT JSON_SCHEMA_VALID('{}', '{}')",
        "JSON_SCHEMA_VALID"
    );
    assert_prepare_fails_with_message(
        db,
        "SELECT json_schema_valid('{\"type\":\"number\"}', ?)",
        "JSON_SCHEMA_VALID"
    );
    assert_prepare_fails_with_message(
        db,
        "SELECT * FROM JSON_TABLE('[1,2]', '$[*]' COLUMNS (value INT PATH '$')) AS jt",
        "JSON_TABLE"
    );
    assert_prepare_fails_with_message(
        db,
        "SELECT * FROM json_table(?, '$[*]' COLUMNS (value INT PATH '$')) AS jt",
        "JSON_TABLE"
    );
    assert_prepare_fails_with_message(db, "SELECT COLUMN_CREATE('color', ?)", "dynamic column");
    assert_prepare_fails_with_message(
        db,
        "SELECT column_get(?, 'color' AS CHAR(16))",
        "dynamic column"
    );

    stmt = prepare_statement(db, "SELECT JSON_VALID(?)");
    assert(
        mylite_bind_text(stmt, 1U, "{\"ok\": true}", MYLITE_NUL_TERMINATED, MYLITE_STATIC) ==
        MYLITE_OK
    );
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_int64(stmt, 0U) == 1);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_finalize(stmt) == MYLITE_OK);
    stmt = NULL;

    assert(
        mylite_prepare(db, "SELECT EXTRACTVALUE(?, '/a')", MYLITE_NUL_TERMINATED, &stmt, NULL) ==
        MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "XML SQL function") != NULL);

    assert(
        mylite_prepare(
            db,
            "SELECT UPDATEXML(?, '/a', '<a>new</a>')",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "XML SQL function") != NULL);

    assert(
        mylite_prepare(db, "SET sql_mode='ORACLE'", MYLITE_NUL_TERMINATED, &stmt, NULL) ==
        MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "Oracle SQL mode") != NULL);

    assert(
        mylite_prepare(
            db,
            "SET SESSION sql_mode='ANSI,ORACLE'",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "Oracle SQL mode") != NULL);

    assert(
        mylite_prepare(
            db,
            "SELECT ? INTO DUMPFILE '/tmp/mylite-out.bin'",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "file export") != NULL);

    assert(
        mylite_prepare(
            db,
            "CREATE VIEW blocked_view AS SELECT 1",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "non-table database object") != NULL);

    assert(
        mylite_prepare(
            db,
            "CREATE FUNCTION blocked_udf RETURNS INTEGER SONAME 'blocked_udf.so'",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "non-table database object") != NULL);

    assert(
        mylite_prepare(db, "START TRANSACTION", MYLITE_NUL_TERMINATED, &stmt, NULL) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "transaction control") != NULL);

    assert(
        mylite_prepare(db, "SELECT 1 FOR UPDATE", MYLITE_NUL_TERMINATED, &stmt, NULL) ==
        MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "SQL locking") != NULL);

    assert(
        mylite_prepare(db, "DO GET_LOCK(?, 1)", MYLITE_NUL_TERMINATED, &stmt, NULL) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "SQL locking") != NULL);

    assert(
        mylite_prepare(
            db,
            "ALTER TABLE online_alter_prepare ADD COLUMN blocked_inplace INT, ALGORITHM=INPLACE",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "online ALTER") != NULL);

    assert(
        mylite_prepare(
            db,
            "CREATE TABLE partitioned_prepare (id INT NOT NULL PRIMARY KEY) "
            "PARTITION BY HASH (id) PARTITIONS 2",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "partition") != NULL);

    assert(
        mylite_prepare(
            db,
            "CREATE TABLE fk_blocked_prepare ("
            "id INT NOT NULL PRIMARY KEY, parent_id INT, "
            "FOREIGN KEY (parent_id) REFERENCES fk_parent(id))",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) == MYLITE_ERROR
    );
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), "foreign-key") != NULL);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_invalid_indexes(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *stmt = prepare_statement(db, "SELECT CAST(? AS SIGNED)");

    assert(mylite_bind_null(stmt, 0U) == MYLITE_MISUSE);
    assert(mylite_bind_int64(stmt, 2U, 1) == MYLITE_MISUSE);
    assert(mylite_bind_text(stmt, 1U, NULL, MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_MISUSE);
    assert(mylite_bind_int64(stmt, 1U, 11) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_name(stmt, 1U) == NULL);
    assert(mylite_column_database_name(stmt, 1U) == NULL);
    assert(mylite_column_origin_name(stmt, 1U) == NULL);
    assert(mylite_column_mariadb_type(stmt, 1U) == 0U);
    assert(mylite_column_flags(stmt, 1U) == 0U);
    assert(mylite_column_charset(stmt, 1U) == 0U);
    assert(mylite_column_decimals(stmt, 1U) == 0U);
    assert(mylite_column_length(stmt, 1U) == 0UL);
    assert(mylite_column_max_length(stmt, 1U) == 0UL);
    assert(mylite_column_type(stmt, 1U) == MYLITE_TYPE_NULL);
    assert(mylite_column_blob(stmt, 1U) == NULL);
    assert(mylite_column_bytes(stmt, 1U) == 0U);
    assert(mylite_finalize(stmt) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void assert_prepare_fails_with_message(mylite_db *db, const char *sql, const char *message) {
    mylite_stmt *stmt = NULL;

    assert(mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, NULL) == MYLITE_ERROR);
    assert(stmt == NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == 0U);
    assert(strcmp(mylite_sqlstate(db), "HY000") == 0);
    assert(strstr(mylite_errmsg(db), message) != NULL);
}

static mylite_stmt *prepare_statement(mylite_db *db, const char *sql) {
    mylite_stmt *stmt = NULL;
    assert(mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, NULL) == MYLITE_OK);
    assert(stmt != NULL);
    return stmt;
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
    *filename = path_join(root, "statement.mylite");
    assert(
        mylite_open_v2(*filename, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    free(runtime_root);
    return db;
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-statement.XXXXXX";
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

static void counting_destructor(void *ptr) {
    assert(ptr != NULL);
    ++destructor_calls;
}

#include <mylite/mylite.h>

#include <assert.h>
#include <stddef.h>
#include <string.h>

static void test_open_validation(void);
static void test_null_database_api(void);
static void test_null_statement_api(void);
static void test_null_column_api(void);

int main(void) {
    test_open_validation();
    test_null_database_api();
    test_null_statement_api();
    test_null_column_api();
    return 0;
}

static void test_open_validation(void) {
    enum { invalid_profile = 99 };

    mylite_db *database = (mylite_db *)1;

    assert(mylite_close(NULL) == MYLITE_OK);
    assert(mylite_open(NULL, &database) == MYLITE_MISUSE);
    assert(database == NULL);
    assert(mylite_open("", &database) == MYLITE_MISUSE);
    assert(database == NULL);
    assert(mylite_open("unused.mylite", NULL) == MYLITE_MISUSE);

    assert(mylite_open_v2("unused.mylite", &database, 0U, NULL) == MYLITE_MISUSE);
    assert(database == NULL);
    assert(
        mylite_open_v2(
            "unused.mylite",
            &database,
            MYLITE_OPEN_READONLY | MYLITE_OPEN_READWRITE,
            NULL
        ) == MYLITE_MISUSE
    );
    assert(database == NULL);
    assert(
        mylite_open_v2(
            "unused.mylite",
            &database,
            MYLITE_OPEN_READONLY | MYLITE_OPEN_CREATE,
            NULL
        ) == MYLITE_MISUSE
    );
    assert(database == NULL);
    assert(
        mylite_open_v2("unused.mylite", &database, MYLITE_OPEN_READWRITE | MYLITE_OPEN_URI, NULL) ==
        MYLITE_MISUSE
    );
    assert(database == NULL);

    mylite_open_config config = {
        .size = sizeof(config),
        .profile = invalid_profile,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = NULL,
    };
    assert(
        mylite_open_v2(
            "unused.mylite",
            &database,
            MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE,
            &config
        ) == MYLITE_MISUSE
    );
    assert(database == NULL);
}

static void test_null_database_api(void) {
    mylite_db *database = NULL;

    assert(mylite_errcode(NULL) == MYLITE_MISUSE);
    assert(mylite_extended_errcode(NULL) == MYLITE_MISUSE);
    assert(mylite_mariadb_errno(NULL) == 0U);
    assert(strcmp(mylite_sqlstate(NULL), "HY000") == 0);
    assert(strcmp(mylite_errmsg(NULL), "bad database handle") == 0);
    assert(mylite_warning_count(NULL) == 0U);
    assert(mylite_warning(NULL, 0U, NULL, NULL, NULL) == MYLITE_MISUSE);
    assert(mylite_busy_timeout(NULL, 1U) == MYLITE_MISUSE);
    assert(mylite_exec(NULL, "SELECT 1", NULL, NULL, NULL) == MYLITE_MISUSE);
    database = (mylite_db *)1;
    assert(mylite_exec(database, NULL, NULL, NULL, NULL) == MYLITE_MISUSE);
    assert(mylite_prepare(NULL, "SELECT 1", MYLITE_NUL_TERMINATED, NULL, NULL) == MYLITE_MISUSE);
    assert(mylite_changes(NULL) == 0);
    assert(mylite_last_insert_id(NULL) == 0U);
    mylite_free(NULL);
}

static void test_null_statement_api(void) {
    assert(mylite_step(NULL) == MYLITE_MISUSE);
    assert(mylite_reset(NULL) == MYLITE_MISUSE);
    assert(mylite_finalize(NULL) == MYLITE_OK);
    assert(mylite_bind_parameter_count(NULL) == 0U);
    assert(mylite_clear_bindings(NULL) == MYLITE_MISUSE);
    assert(mylite_bind_null(NULL, 1U) == MYLITE_MISUSE);
    assert(mylite_bind_int64(NULL, 1U, 1) == MYLITE_MISUSE);
    assert(mylite_bind_uint64(NULL, 1U, 1U) == MYLITE_MISUSE);
    assert(mylite_bind_double(NULL, 1U, 1.0) == MYLITE_MISUSE);
    assert(mylite_bind_text(NULL, 1U, "x", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_MISUSE);
    assert(mylite_bind_blob(NULL, 1U, "x", 1U, MYLITE_STATIC) == MYLITE_MISUSE);
}

static void test_null_column_api(void) {
    assert(mylite_column_count(NULL) == 0U);
    assert(mylite_column_name(NULL, 0U) == NULL);
    assert(mylite_column_database_name(NULL, 0U) == NULL);
    assert(mylite_column_table_name(NULL, 0U) == NULL);
    assert(mylite_column_origin_table_name(NULL, 0U) == NULL);
    assert(mylite_column_origin_name(NULL, 0U) == NULL);
    assert(mylite_column_mariadb_type(NULL, 0U) == 0U);
    assert(mylite_column_flags(NULL, 0U) == 0U);
    assert(mylite_column_charset(NULL, 0U) == 0U);
    assert(mylite_column_decimals(NULL, 0U) == 0U);
    assert(mylite_column_length(NULL, 0U) == 0UL);
    assert(mylite_column_max_length(NULL, 0U) == 0UL);
    assert(mylite_column_type(NULL, 0U) == MYLITE_TYPE_NULL);
    assert(mylite_column_int64(NULL, 0U) == 0);
    assert(mylite_column_uint64(NULL, 0U) == 0U);
    assert(mylite_column_double(NULL, 0U) == 0.0);
    assert(mylite_column_text(NULL, 0U) == NULL);
    assert(mylite_column_blob(NULL, 0U) == NULL);
    assert(mylite_column_bytes(NULL, 0U) == 0U);
    assert(mylite_column_read(NULL, 0U, 0U, NULL, 0U, NULL) == MYLITE_MISUSE);
}

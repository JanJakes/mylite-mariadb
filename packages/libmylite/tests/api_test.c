#include <mylite/mylite.h>

#include <assert.h>
#include <stddef.h>
#include <string.h>

static void assert_open_validation(void);
static void assert_capabilities(void);
static void assert_config_validation(void);
static void assert_handle_diagnostics(void);
static void assert_exec_validation(void);
static void assert_prepare_validation(void);
static void assert_statement_validation(void);
static void assert_warning_validation(void);
static void assert_effect_accessors(void);

int main(void) {
    assert_open_validation();
    assert_capabilities();
    assert_config_validation();
    assert_handle_diagnostics();
    assert_exec_validation();
    assert_prepare_validation();
    assert_statement_validation();
    assert_warning_validation();
    assert_effect_accessors();
    mylite_free(NULL);
    return 0;
}

static void assert_open_validation(void) {
    mylite_db *db = (mylite_db *)1;
    const unsigned long long capabilities = mylite_capabilities();

    assert(mylite_close(NULL) == MYLITE_OK);
    assert(
        mylite_open(NULL, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, NULL) == MYLITE_MISUSE
    );
    assert(db == NULL);
    assert(mylite_open("", &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, NULL) == MYLITE_MISUSE);
    assert(db == NULL);
    assert(
        mylite_open("unused.mylite", NULL, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, NULL) ==
        MYLITE_MISUSE
    );
    assert(mylite_open("unused.mylite", &db, 0U, NULL) == MYLITE_MISUSE);
    assert(db == NULL);
    assert(
        mylite_open("unused.mylite", &db, MYLITE_OPEN_READONLY | MYLITE_OPEN_READWRITE, NULL) ==
        MYLITE_MISUSE
    );
    assert(db == NULL);
    assert(
        mylite_open("unused.mylite", &db, MYLITE_OPEN_READONLY | MYLITE_OPEN_CREATE, NULL) ==
        MYLITE_MISUSE
    );
    assert(db == NULL);
    assert(
        mylite_open("unused.mylite", &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_URI, NULL) ==
        MYLITE_MISUSE
    );
    assert(db == NULL);
    assert(
        mylite_open(
            "unused.mylite",
            &db,
            MYLITE_OPEN_READONLY | MYLITE_OPEN_SHARED_READONLY,
            NULL
        ) == MYLITE_MISUSE
    );
    assert(db == NULL);
    if ((capabilities & MYLITE_CAP_OWNERLESS_RW) == 0U) {
        assert(
            mylite_open(
                "unused.mylite",
                &db,
                MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
                NULL
            ) == MYLITE_MISUSE
        );
        assert(db == NULL);
    }
}

static void assert_capabilities(void) {
    const unsigned long long capabilities = mylite_capabilities();

    assert((capabilities & MYLITE_CAP_SHARED_READONLY) == 0U);
    assert(
        (capabilities &
         ~(MYLITE_CAP_SAME_PROCESS_CONCURRENCY | MYLITE_CAP_SHARED_READONLY |
           MYLITE_CAP_OWNERLESS_RW)) == 0U
    );
}

static void assert_config_validation(void) {
    mylite_db *db = NULL;
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_COMPAT + 1,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = NULL,
    };

    assert(
        mylite_open("unused.mylite", &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_MISUSE
    );
    assert(db == NULL);
}

static void assert_handle_diagnostics(void) {
    assert(mylite_errcode(NULL) == MYLITE_MISUSE);
    assert(mylite_extended_errcode(NULL) == MYLITE_MISUSE);
    assert(mylite_mariadb_errno(NULL) == 0U);
    assert(strcmp(mylite_sqlstate(NULL), "HY000") == 0);
    assert(strcmp(mylite_errmsg(NULL), "bad database handle") == 0);
}

static void assert_exec_validation(void) {
    mylite_db *db = (mylite_db *)1;

    assert(mylite_exec(NULL, "SELECT 1", NULL, NULL, NULL) == MYLITE_MISUSE);
    assert(mylite_exec(db, NULL, NULL, NULL, NULL) == MYLITE_MISUSE);
}

static void assert_prepare_validation(void) {
    mylite_db *db = (mylite_db *)1;
    mylite_stmt *stmt = (mylite_stmt *)1;
    const char *tail = (const char *)1;

    assert(mylite_prepare(NULL, "SELECT 1", MYLITE_NUL_TERMINATED, &stmt, &tail) == MYLITE_MISUSE);
    assert(stmt == NULL);
    assert(tail == NULL);
    assert(mylite_prepare(db, NULL, MYLITE_NUL_TERMINATED, &stmt, &tail) == MYLITE_MISUSE);
    assert(stmt == NULL);
    assert(tail == NULL);
    assert(mylite_prepare(db, "SELECT 1", MYLITE_NUL_TERMINATED, NULL, NULL) == MYLITE_MISUSE);
}

static void assert_statement_validation(void) {
    assert(mylite_step(NULL) == MYLITE_MISUSE);
    assert(mylite_reset(NULL) == MYLITE_MISUSE);
    assert(mylite_finalize(NULL) == MYLITE_OK);
    assert(mylite_bind_parameter_count(NULL) == 0U);
    assert(mylite_clear_bindings(NULL) == MYLITE_MISUSE);
    assert(mylite_bind_null(NULL, 1) == MYLITE_MISUSE);
    assert(mylite_bind_int64(NULL, 1, 1) == MYLITE_MISUSE);
    assert(mylite_bind_uint64(NULL, 1, 1U) == MYLITE_MISUSE);
    assert(mylite_bind_double(NULL, 1, 1.0) == MYLITE_MISUSE);
    assert(
        mylite_bind_text(NULL, 1, "text", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_MISUSE
    );
    assert(mylite_bind_blob(NULL, 1, "blob", 4, MYLITE_STATIC) == MYLITE_MISUSE);
    assert(mylite_column_count(NULL) == 0U);
    assert(mylite_column_name(NULL, 0) == NULL);
    assert(mylite_column_type(NULL, 0) == MYLITE_TYPE_NULL);
    assert(mylite_column_int64(NULL, 0) == 0);
    assert(mylite_column_uint64(NULL, 0) == 0U);
    assert(mylite_column_double(NULL, 0) == 0.0);
    assert(mylite_column_text(NULL, 0) == NULL);
    assert(mylite_column_blob(NULL, 0) == NULL);
    assert(mylite_column_bytes(NULL, 0) == 0U);
}

static void assert_warning_validation(void) {
    assert(mylite_warning_count(NULL) == 0U);
    assert(mylite_warning(NULL, 0, NULL, NULL, NULL) == MYLITE_MISUSE);
}

static void assert_effect_accessors(void) {
    assert(mylite_changes(NULL) == 0);
    assert(mylite_last_insert_id(NULL) == 0U);
}

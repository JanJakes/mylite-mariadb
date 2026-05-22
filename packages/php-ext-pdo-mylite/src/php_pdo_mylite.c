// clang-format off
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <php.h>
#include <ext/pdo/php_pdo_driver.h>
#include <ext/pdo/php_pdo_error.h>
#include <ext/standard/info.h>
// clang-format on

#include <mylite/mylite.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PHP_PDO_MYLITE_EXT_VERSION "0.1.0"
#define PDO_MYLITE_ALL_PARAM_EVENTS ((1U << ((unsigned)PDO_PARAM_EVT_NORMALIZE + 1U)) - 1U)

typedef struct pdo_mylite_db_handle {
    mylite_db *db;
    unsigned native_errno;
    zend_string *errmsg;
} pdo_mylite_db_handle;

typedef struct pdo_mylite_stmt {
    pdo_mylite_db_handle *handle;
    mylite_stmt *stmt;
    int step_result;
    bool prefetched;
    bool done;
} pdo_mylite_stmt;

static int pdo_mylite_handle_factory(pdo_dbh_t *dbh, zval *driver_options);
static void pdo_mylite_handle_closer(pdo_dbh_t *dbh);
static bool pdo_mylite_handle_preparer(
    pdo_dbh_t *dbh,
    zend_string *sql,
    pdo_stmt_t *stmt,
    zval *driver_options
);
static zend_long pdo_mylite_handle_doer(pdo_dbh_t *dbh, const zend_string *sql);
static zend_string *pdo_mylite_handle_quoter(
    pdo_dbh_t *dbh,
    const zend_string *unquoted,
    enum pdo_param_type paramtype
);
static bool pdo_mylite_handle_begin(pdo_dbh_t *dbh);
static bool pdo_mylite_handle_commit(pdo_dbh_t *dbh);
static bool pdo_mylite_handle_rollback(pdo_dbh_t *dbh);
static zend_string *pdo_mylite_last_insert_id(pdo_dbh_t *dbh, const zend_string *name);
static void pdo_mylite_fetch_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, zval *info);
static int pdo_mylite_get_attribute(pdo_dbh_t *dbh, zend_long attr, zval *return_value);
static int pdo_mylite_stmt_dtor(pdo_stmt_t *stmt);
static int pdo_mylite_stmt_execute(pdo_stmt_t *stmt);
static int pdo_mylite_stmt_fetch(
    pdo_stmt_t *stmt,
    enum pdo_fetch_orientation ori,
    zend_long offset
);
static int pdo_mylite_stmt_describe(pdo_stmt_t *stmt, int column);
static int pdo_mylite_stmt_get_col(
    pdo_stmt_t *stmt,
    int column,
    zval *result,
    enum pdo_param_type *type
);
static int pdo_mylite_stmt_param_hook(
    pdo_stmt_t *stmt,
    struct pdo_bound_param_data *param,
    enum pdo_param_event event_type
);
static int pdo_mylite_stmt_cursor_closer(pdo_stmt_t *stmt);
static int pdo_mylite_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, int result, const char *fallback);
static int pdo_mylite_bind_zval(mylite_stmt *stmt, unsigned index, zval *value);
static void pdo_mylite_column_to_zval(mylite_stmt *stmt, unsigned column, zval *value);
static zend_string *pdo_mylite_quote_string(zend_string *input);
static char *pdo_mylite_resolve_path(pdo_dbh_t *dbh);

static const struct pdo_dbh_methods pdo_mylite_dbh_methods = {
    pdo_mylite_handle_closer,
    pdo_mylite_handle_preparer,
    pdo_mylite_handle_doer,
    pdo_mylite_handle_quoter,
    pdo_mylite_handle_begin,
    pdo_mylite_handle_commit,
    pdo_mylite_handle_rollback,
    NULL,
    pdo_mylite_last_insert_id,
    pdo_mylite_fetch_error,
    pdo_mylite_get_attribute,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
#if PHP_VERSION_ID >= 80400
    ,
    NULL
#endif
};

static const struct pdo_stmt_methods pdo_mylite_stmt_methods = {
    pdo_mylite_stmt_dtor,
    pdo_mylite_stmt_execute,
    pdo_mylite_stmt_fetch,
    pdo_mylite_stmt_describe,
    pdo_mylite_stmt_get_col,
    pdo_mylite_stmt_param_hook,
    NULL,
    NULL,
    NULL,
    NULL,
    pdo_mylite_stmt_cursor_closer
};

static const pdo_driver_t pdo_mylite_driver = {
    PDO_DRIVER_HEADER(mylite),
    pdo_mylite_handle_factory
};

static const zend_module_dep pdo_mylite_deps[] = {ZEND_MOD_REQUIRED("pdo")
                                                      ZEND_MOD_REQUIRED("mylite") ZEND_MOD_END};

PHP_MINIT_FUNCTION(pdo_mylite) {
    (void)type;
    (void)module_number;
    return php_pdo_register_driver(&pdo_mylite_driver);
}

PHP_MSHUTDOWN_FUNCTION(pdo_mylite) {
    (void)type;
    (void)module_number;
    php_pdo_unregister_driver(&pdo_mylite_driver);
    return SUCCESS;
}

PHP_MINFO_FUNCTION(pdo_mylite) {
    (void)zend_module;
    php_info_print_table_start();
    php_info_print_table_row(2, "pdo_mylite support", "enabled");
    php_info_print_table_row(2, "pdo_mylite version", PHP_PDO_MYLITE_EXT_VERSION);
    php_info_print_table_end();
}

static zend_module_entry pdo_mylite_module_entry = {
    STANDARD_MODULE_HEADER_EX,
    NULL,
    pdo_mylite_deps,
    "pdo_mylite",
    NULL,
    PHP_MINIT(pdo_mylite),
    PHP_MSHUTDOWN(pdo_mylite),
    NULL,
    NULL,
    PHP_MINFO(pdo_mylite),
    PHP_PDO_MYLITE_EXT_VERSION,
    STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(pdo_mylite)

static int pdo_mylite_handle_factory(pdo_dbh_t *dbh, zval *driver_options) {
    (void)driver_options;
    int ret = 0;
    pdo_mylite_db_handle *handle = pecalloc(1, sizeof(pdo_mylite_db_handle), dbh->is_persistent);
    dbh->driver_data = handle;

    if (dbh->is_persistent) {
        pdo_mylite_error(
            dbh,
            NULL,
            MYLITE_MISUSE,
            "persistent MyLite PDO connections are not supported"
        );
        goto cleanup;
    }

    char *path = pdo_mylite_resolve_path(dbh);
    if (path == NULL || path[0] == '\0') {
        pdo_mylite_error(
            dbh,
            NULL,
            MYLITE_MISUSE,
            "MyLite PDO DSN requires a database directory path"
        );
        efree(path);
        goto cleanup;
    }

    const int open_result =
        mylite_open(path, &handle->db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, NULL);
    efree(path);
    if (open_result != MYLITE_OK) {
        pdo_mylite_error(dbh, NULL, open_result, "could not open MyLite database");
        goto cleanup;
    }

    dbh->alloc_own_columns = true;
    dbh->max_escaped_char_length = 2;
    dbh->skip_param_evt = (uint8_t)(PDO_MYLITE_ALL_PARAM_EVENTS ^ (1U << PDO_PARAM_EVT_EXEC_PRE));
    ret = 1;

cleanup:
    dbh->methods = &pdo_mylite_dbh_methods;
    return ret;
}

static void pdo_mylite_handle_closer(pdo_dbh_t *dbh) {
    pdo_mylite_db_handle *handle = (pdo_mylite_db_handle *)dbh->driver_data;
    if (handle == NULL) {
        return;
    }
    if (handle->db != NULL) {
        (void)mylite_close(handle->db);
        handle->db = NULL;
    }
    if (handle->errmsg != NULL) {
        zend_string_release(handle->errmsg);
        handle->errmsg = NULL;
    }
    pefree(handle, dbh->is_persistent);
    dbh->driver_data = NULL;
}

static bool pdo_mylite_handle_preparer(
    pdo_dbh_t *dbh,
    zend_string *sql,
    pdo_stmt_t *stmt,
    zval *driver_options
) {
    (void)driver_options;
    pdo_mylite_db_handle *handle = (pdo_mylite_db_handle *)dbh->driver_data;
    pdo_mylite_stmt *statement_data = ecalloc(1, sizeof(pdo_mylite_stmt));
    statement_data->handle = handle;
    stmt->driver_data = statement_data;
    stmt->methods = &pdo_mylite_stmt_methods;
    stmt->supports_placeholders = PDO_PLACEHOLDER_POSITIONAL;

    const int result =
        mylite_prepare(handle->db, ZSTR_VAL(sql), ZSTR_LEN(sql), &statement_data->stmt, NULL);
    if (result != MYLITE_OK) {
        efree(statement_data);
        stmt->driver_data = NULL;
        pdo_mylite_error(dbh, stmt, result, "could not prepare MyLite PDO statement");
        return false;
    }
    return true;
}

static zend_long pdo_mylite_handle_doer(pdo_dbh_t *dbh, const zend_string *sql) {
    pdo_mylite_db_handle *handle = (pdo_mylite_db_handle *)dbh->driver_data;
    char *errmsg = NULL;
    const int result = mylite_exec(handle->db, ZSTR_VAL(sql), NULL, NULL, &errmsg);
    if (result != MYLITE_OK) {
        const char *fallback = errmsg != NULL ? errmsg : "MyLite PDO exec failed";
        pdo_mylite_error(dbh, NULL, result, fallback);
        mylite_free(errmsg);
        return -1;
    }
    mylite_free(errmsg);
    return (zend_long)mylite_changes(handle->db);
}

static zend_string *pdo_mylite_handle_quoter(
    pdo_dbh_t *dbh,
    const zend_string *unquoted,
    enum pdo_param_type paramtype
) {
    (void)dbh;
    (void)paramtype;
    zend_string *escaped = pdo_mylite_quote_string((zend_string *)unquoted);
    zend_string *quoted = zend_string_safe_alloc(1, ZSTR_LEN(escaped), 2, false);
    ZSTR_VAL(quoted)[0] = '\'';
    memcpy(ZSTR_VAL(quoted) + 1, ZSTR_VAL(escaped), ZSTR_LEN(escaped));
    ZSTR_VAL(quoted)[ZSTR_LEN(escaped) + 1] = '\'';
    ZSTR_VAL(quoted)[ZSTR_LEN(escaped) + 2] = '\0';
    ZSTR_LEN(quoted) = ZSTR_LEN(escaped) + 2;
    zend_string_release(escaped);
    return quoted;
}

static bool pdo_mylite_handle_begin(pdo_dbh_t *dbh) {
    pdo_mylite_db_handle *handle = (pdo_mylite_db_handle *)dbh->driver_data;
    const int result = mylite_exec(handle->db, "START TRANSACTION", NULL, NULL, NULL);
    if (result != MYLITE_OK) {
        pdo_mylite_error(dbh, NULL, result, "could not start MyLite transaction");
        return false;
    }
    return true;
}

static bool pdo_mylite_handle_commit(pdo_dbh_t *dbh) {
    pdo_mylite_db_handle *handle = (pdo_mylite_db_handle *)dbh->driver_data;
    const int result = mylite_exec(handle->db, "COMMIT", NULL, NULL, NULL);
    if (result != MYLITE_OK) {
        pdo_mylite_error(dbh, NULL, result, "could not commit MyLite transaction");
        return false;
    }
    return true;
}

static bool pdo_mylite_handle_rollback(pdo_dbh_t *dbh) {
    pdo_mylite_db_handle *handle = (pdo_mylite_db_handle *)dbh->driver_data;
    const int result = mylite_exec(handle->db, "ROLLBACK", NULL, NULL, NULL);
    if (result != MYLITE_OK) {
        pdo_mylite_error(dbh, NULL, result, "could not roll back MyLite transaction");
        return false;
    }
    return true;
}

static zend_string *pdo_mylite_last_insert_id(pdo_dbh_t *dbh, const zend_string *name) {
    (void)name;
    pdo_mylite_db_handle *handle = (pdo_mylite_db_handle *)dbh->driver_data;
    return zend_u64_to_str(mylite_last_insert_id(handle->db));
}

static void pdo_mylite_fetch_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, zval *info) {
    (void)stmt;
    pdo_mylite_db_handle *handle = (pdo_mylite_db_handle *)dbh->driver_data;
    if (handle == NULL || handle->native_errno == 0U || handle->errmsg == NULL) {
        return;
    }
    add_next_index_long(info, (zend_long)handle->native_errno);
    add_next_index_str(info, zend_string_copy(handle->errmsg));
}

static int pdo_mylite_get_attribute(pdo_dbh_t *dbh, zend_long attr, zval *return_value) {
    (void)dbh;
    switch (attr) {
    case PDO_ATTR_CLIENT_VERSION:
    case PDO_ATTR_SERVER_VERSION:
        ZVAL_STRING(return_value, mylite_version());
        return 1;
    case PDO_ATTR_DRIVER_NAME:
        ZVAL_STRING(return_value, "mylite");
        return 1;
    default:
        return 0;
    }
}

static int pdo_mylite_stmt_dtor(pdo_stmt_t *stmt) {
    pdo_mylite_stmt *statement_data = (pdo_mylite_stmt *)stmt->driver_data;
    if (statement_data != NULL) {
        if (statement_data->stmt != NULL) {
            (void)mylite_finalize(statement_data->stmt);
            statement_data->stmt = NULL;
        }
        efree(statement_data);
        stmt->driver_data = NULL;
    }
    return 1;
}

static int pdo_mylite_stmt_execute(pdo_stmt_t *stmt) {
    pdo_mylite_stmt *statement_data = (pdo_mylite_stmt *)stmt->driver_data;
    if (stmt->executed) {
        (void)mylite_reset(statement_data->stmt);
    }

    statement_data->prefetched = false;
    statement_data->done = false;
    statement_data->step_result = mylite_step(statement_data->stmt);
    if (statement_data->step_result == MYLITE_ROW) {
        statement_data->prefetched = true;
        php_pdo_stmt_set_column_count(stmt, (int)mylite_column_count(statement_data->stmt));
        return 1;
    }
    if (statement_data->step_result == MYLITE_DONE) {
        statement_data->done = true;
        php_pdo_stmt_set_column_count(stmt, (int)mylite_column_count(statement_data->stmt));
        stmt->row_count = (zend_long)mylite_changes(statement_data->handle->db);
        return 1;
    }

    pdo_mylite_error(
        stmt->dbh,
        stmt,
        statement_data->step_result,
        "could not execute MyLite PDO statement"
    );
    return 0;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
static int pdo_mylite_stmt_fetch(
    pdo_stmt_t *stmt,
    enum pdo_fetch_orientation ori,
    zend_long offset
) {
    (void)ori;
    (void)offset;
    pdo_mylite_stmt *statement_data = (pdo_mylite_stmt *)stmt->driver_data;
    if (statement_data->done) {
        return 0;
    }
    if (statement_data->prefetched) {
        statement_data->prefetched = false;
        return 1;
    }

    statement_data->step_result = mylite_step(statement_data->stmt);
    if (statement_data->step_result == MYLITE_ROW) {
        return 1;
    }
    if (statement_data->step_result == MYLITE_DONE) {
        statement_data->done = true;
        return 0;
    }

    pdo_mylite_error(
        stmt->dbh,
        stmt,
        statement_data->step_result,
        "could not fetch MyLite PDO row"
    );
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static int pdo_mylite_stmt_describe(pdo_stmt_t *stmt, int column) {
    pdo_mylite_stmt *statement_data = (pdo_mylite_stmt *)stmt->driver_data;
    if (column < 0 || (unsigned)column >= mylite_column_count(statement_data->stmt)) {
        return 0;
    }
    const char *name = mylite_column_name(statement_data->stmt, (unsigned)column);
    stmt->columns[column].name =
        zend_string_init(name != NULL ? name : "", name != NULL ? strlen(name) : 0, false);
    stmt->columns[column].maxlen = 0;
    stmt->columns[column].precision = 0;
    return 1;
}

static int pdo_mylite_stmt_get_col(
    pdo_stmt_t *stmt,
    int column,
    zval *result,
    // NOLINTNEXTLINE(readability-non-const-parameter)
    enum pdo_param_type *type
) {
    (void)type;
    pdo_mylite_stmt *statement_data = (pdo_mylite_stmt *)stmt->driver_data;
    if (column < 0 || (unsigned)column >= mylite_column_count(statement_data->stmt)) {
        ZVAL_NULL(result);
        return 0;
    }
    pdo_mylite_column_to_zval(statement_data->stmt, (unsigned)column, result);
    return 1;
}

static int pdo_mylite_stmt_param_hook(
    pdo_stmt_t *stmt,
    struct pdo_bound_param_data *param,
    enum pdo_param_event event_type
) {
    pdo_mylite_stmt *statement_data = (pdo_mylite_stmt *)stmt->driver_data;
    if (event_type != PDO_PARAM_EVT_EXEC_PRE || !param->is_param) {
        return 1;
    }
    if (param->paramno < 0 || param->paramno >= UINT32_MAX) {
        pdo_mylite_error(
            stmt->dbh,
            stmt,
            MYLITE_MISUSE,
            "named MyLite PDO parameters are not supported yet"
        );
        return 0;
    }
    const int result = pdo_mylite_bind_zval(
        statement_data->stmt,
        (unsigned)param->paramno + 1U,
        &param->parameter
    );
    if (result != MYLITE_OK) {
        pdo_mylite_error(stmt->dbh, stmt, result, "could not bind MyLite PDO parameter");
        return 0;
    }
    return 1;
}

static int pdo_mylite_stmt_cursor_closer(pdo_stmt_t *stmt) {
    pdo_mylite_stmt *statement_data = (pdo_mylite_stmt *)stmt->driver_data;
    if (statement_data == NULL) {
        return 1;
    }
    if (statement_data->stmt != NULL) {
        (void)mylite_reset(statement_data->stmt);
    }
    statement_data->prefetched = false;
    statement_data->done = true;
    return 1;
}

static int pdo_mylite_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, int result, const char *fallback) {
    pdo_mylite_db_handle *handle = (pdo_mylite_db_handle *)dbh->driver_data;
    pdo_error_type *pdo_error = stmt != NULL ? &stmt->error_code : &dbh->error_code;
    const char *sqlstate =
        handle != NULL && handle->db != NULL ? mylite_sqlstate(handle->db) : "HY000";
    const char *message = fallback;
    unsigned native_errno = (unsigned)result;
    if (handle != NULL && handle->db != NULL) {
        if (mylite_mariadb_errno(handle->db) != 0U) {
            native_errno = mylite_mariadb_errno(handle->db);
        }
        if (mylite_errmsg(handle->db) != NULL) {
            message = mylite_errmsg(handle->db);
        }
    }

    strncpy(*pdo_error, sqlstate, sizeof(*pdo_error));
    if (handle != NULL) {
        handle->native_errno = native_errno;
        if (handle->errmsg != NULL) {
            zend_string_release(handle->errmsg);
        }
        handle->errmsg = zend_string_init(message, strlen(message), false);
    }
    return result;
}

static int pdo_mylite_bind_zval(mylite_stmt *stmt, unsigned index, zval *value) {
    ZVAL_DEREF(value);
    switch (Z_TYPE_P(value)) {
    case IS_NULL:
        return mylite_bind_null(stmt, index);
    case IS_FALSE:
        return mylite_bind_int64(stmt, index, 0);
    case IS_TRUE:
        return mylite_bind_int64(stmt, index, 1);
    case IS_LONG:
        return mylite_bind_int64(stmt, index, (long long)Z_LVAL_P(value));
    case IS_DOUBLE:
        return mylite_bind_double(stmt, index, Z_DVAL_P(value));
    default: {
        zend_string *string_value = zval_get_string(value);
        if (string_value == NULL) {
            return MYLITE_NOMEM;
        }
        const int result = mylite_bind_text(
            stmt,
            index,
            ZSTR_VAL(string_value),
            ZSTR_LEN(string_value),
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            MYLITE_TRANSIENT
        );
        zend_string_release(string_value);
        return result;
    }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void pdo_mylite_column_to_zval(mylite_stmt *stmt, unsigned column, zval *value) {
    switch (mylite_column_type(stmt, column)) {
    case MYLITE_TYPE_NULL:
        ZVAL_NULL(value);
        return;
    case MYLITE_TYPE_INT64:
        ZVAL_LONG(value, (zend_long)mylite_column_int64(stmt, column));
        return;
    case MYLITE_TYPE_UINT64: {
        const unsigned long long uint_value = mylite_column_uint64(stmt, column);
        if (uint_value <= (unsigned long long)ZEND_LONG_MAX) {
            ZVAL_LONG(value, (zend_long)uint_value);
        } else {
            ZVAL_STR(value, zend_u64_to_str(uint_value));
        }
        return;
    }
    case MYLITE_TYPE_DOUBLE:
        ZVAL_DOUBLE(value, mylite_column_double(stmt, column));
        return;
    case MYLITE_TYPE_TEXT:
    case MYLITE_TYPE_BLOB: {
        const char *text = mylite_column_text(stmt, column);
        if (text == NULL) {
            ZVAL_NULL(value);
        } else {
            ZVAL_STRINGL(value, text, mylite_column_bytes(stmt, column));
        }
        return;
    }
    }
    ZVAL_NULL(value);
}

static zend_string *pdo_mylite_quote_string(zend_string *input) {
    zend_string *escaped = zend_string_safe_alloc(2, ZSTR_LEN(input), 0, false);
    char *target = ZSTR_VAL(escaped);
    const char *source = ZSTR_VAL(input);
    for (size_t index = 0; index < ZSTR_LEN(input); ++index) {
        switch (source[index]) {
        case '\0':
            *target++ = '\\';
            *target++ = '0';
            break;
        case '\n':
            *target++ = '\\';
            *target++ = 'n';
            break;
        case '\r':
            *target++ = '\\';
            *target++ = 'r';
            break;
        case '\\':
        case '\'':
        case '"':
            *target++ = '\\';
            *target++ = source[index];
            break;
        case '\x1A':
            *target++ = '\\';
            *target++ = 'Z';
            break;
        default:
            *target++ = source[index];
            break;
        }
    }
    *target = '\0';
    ZSTR_LEN(escaped) = (size_t)(target - ZSTR_VAL(escaped));
    return escaped;
}

static char *pdo_mylite_resolve_path(pdo_dbh_t *dbh) {
    struct pdo_data_src_parser parsed[] = {
        {"path", NULL, 0},
    };
    if (php_pdo_parse_data_source(dbh->data_source, dbh->data_source_len, parsed, 1) > 0 &&
        parsed[0].optval != NULL) {
        char *path = estrdup(parsed[0].optval);
        if (parsed[0].freeme) {
            efree(parsed[0].optval);
        }
        return path;
    }
    return estrndup(dbh->data_source, dbh->data_source_len);
}

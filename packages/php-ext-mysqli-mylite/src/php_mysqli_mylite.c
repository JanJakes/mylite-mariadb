// clang-format off
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <php.h>
#include <ext/standard/info.h>
// clang-format on

#include <mylite/mylite.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PHP_MYSQLI_MYLITE_EXT_VERSION "0.1.0"

typedef struct php_mylite_mysqli_link {
    mylite_db *db;
    zend_object std;
} php_mylite_mysqli_link;

typedef struct php_mylite_mysqli_result {
    zval rows;
    zend_ulong position;
    zend_object std;
} php_mylite_mysqli_result;

typedef struct php_mylite_mysqli_stmt {
    mylite_stmt *stmt;
    zend_object *link_object;
    zend_string *types;
    zval *bound_values;
    uint32_t bound_count;
    zval rows;
    bool has_rows;
    zend_object std;
} php_mylite_mysqli_stmt;

static zend_class_entry *php_mylite_mysqli_link_ce;
static zend_class_entry *php_mylite_mysqli_result_ce;
static zend_class_entry *php_mylite_mysqli_stmt_ce;
static zend_class_entry *php_mylite_mysqli_global_link_ce;
static zend_class_entry *php_mylite_mysqli_global_result_ce;
static zend_class_entry *php_mylite_mysqli_global_stmt_ce;
static zend_object_handlers php_mylite_mysqli_link_handlers;
static zend_object_handlers php_mylite_mysqli_result_handlers;
static zend_object_handlers php_mylite_mysqli_stmt_handlers;
static bool php_mylite_mysqli_global_symbols_enabled;

static zend_object *php_mylite_mysqli_link_create(zend_class_entry *class_entry);
static void php_mylite_mysqli_link_free(zend_object *object);
static zend_object *php_mylite_mysqli_result_create(zend_class_entry *class_entry);
static void php_mylite_mysqli_result_free(zend_object *object);
static zend_object *php_mylite_mysqli_stmt_create(zend_class_entry *class_entry);
static void php_mylite_mysqli_stmt_free(zend_object *object);

static php_mylite_mysqli_link *php_mylite_mysqli_link_from_object(zend_object *object);
static php_mylite_mysqli_result *php_mylite_mysqli_result_from_object(zend_object *object);
static php_mylite_mysqli_stmt *php_mylite_mysqli_stmt_from_object(zend_object *object);
static bool php_mylite_mysqli_is_link_object(zend_object *object);
static bool php_mylite_mysqli_is_result_object(zend_object *object);
static bool php_mylite_mysqli_is_stmt_object(zend_object *object);
static int php_mylite_mysqli_connect_impl(
    zval *return_value,
    zend_class_entry *link_ce,
    const char *path
);
static int php_mylite_mysqli_open_link(
    php_mylite_mysqli_link *link,
    zend_object *object,
    const char *path
);
static mylite_db *php_mylite_mysqli_require_db(php_mylite_mysqli_link *link);
static void php_mylite_mysqli_clear_error(zend_object *object);
static void php_mylite_mysqli_set_error(
    php_mylite_mysqli_link *link,
    zend_object *object,
    int result,
    const char *fallback
);
static void php_mylite_mysqli_sync_status(php_mylite_mysqli_link *link, zend_object *object);
static int php_mylite_mysqli_query_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    size_t sql_len,
    zval *return_value
);
static int php_mylite_mysqli_prepare_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    size_t sql_len,
    zval *return_value
);
static void php_mylite_mysqli_result_from_rows(
    zval *return_value,
    zval *rows,
    zend_class_entry *result_ce
);
static int php_mylite_mysqli_add_current_row(mylite_stmt *stmt, zval *rows);
static void php_mylite_mysqli_column_to_zval(mylite_stmt *stmt, unsigned column, zval *value);
static int php_mylite_mysqli_bind_zval(mylite_stmt *stmt, unsigned index, zval *value);
static void php_mylite_mysqli_stmt_clear_bindings(php_mylite_mysqli_stmt *stmt);
static void php_mylite_mysqli_stmt_clear_rows(php_mylite_mysqli_stmt *stmt);
static int php_mylite_mysqli_stmt_execute_impl(php_mylite_mysqli_stmt *stmt);
static zend_string *php_mylite_mysqli_escape_sql(zend_string *input);
static zend_class_entry *php_mylite_mysqli_result_class_for_link(zend_object *link_object);
static zend_class_entry *php_mylite_mysqli_stmt_class_for_link(zend_object *link_object);
static void php_mylite_mysqli_declare_link_properties(zend_class_entry *class_entry);
static void php_mylite_mysqli_declare_result_properties(zend_class_entry *class_entry);
static void php_mylite_mysqli_register_global_symbols(void);

#define Z_MYLITE_MYSQLI_LINK_P(zval_ptr) php_mylite_mysqli_link_from_object(Z_OBJ_P((zval_ptr)))
#define Z_MYLITE_MYSQLI_RESULT_P(zval_ptr) php_mylite_mysqli_result_from_object(Z_OBJ_P((zval_ptr)))
#define Z_MYLITE_MYSQLI_STMT_P(zval_ptr) php_mylite_mysqli_stmt_from_object(Z_OBJ_P((zval_ptr)))

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_connect, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, hostname, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, username, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, password, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, database, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port, IS_LONG, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, socket, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_bool, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_query, 0, 0, 2)
ZEND_ARG_INFO(0, link)
ZEND_ARG_TYPE_INFO(0, query, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_method_query, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, query, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_prepare, 0, 0, 2)
ZEND_ARG_INFO(0, link)
ZEND_ARG_TYPE_INFO(0, query, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_method_prepare, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, query, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_fetch_assoc, 0, 0, 1)
ZEND_ARG_INFO(0, result)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_method_fetch_assoc, 0, 0, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_close, 0, 0, 1)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_num_rows, 0, 1, IS_LONG, 0)
ZEND_ARG_INFO(0, result)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_fetch_all, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_escape, 0, 1, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, string, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_stmt_bind_param, 0, 2, _IS_BOOL, 0)
ZEND_ARG_TYPE_INFO(0, types, IS_STRING, 0)
ZEND_ARG_VARIADIC_INFO(1, vars)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_stmt_get_result, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_stmt_bind_param_function, 0, 0, 3)
ZEND_ARG_INFO(0, statement)
ZEND_ARG_TYPE_INFO(0, types, IS_STRING, 0)
ZEND_ARG_VARIADIC_INFO(1, vars)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_stmt_function, 0, 0, 1)
ZEND_ARG_INFO(0, statement)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_global_enabled, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

// NOLINTBEGIN(readability-function-cognitive-complexity)
PHP_FUNCTION(mylite_mysqli_global_symbols_enabled) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(php_mylite_mysqli_global_symbols_enabled);
}

PHP_FUNCTION(mylite_mysqli_connect) {
    char *path = NULL;
    size_t path_len = 0;
    zval *unused = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 6)
    Z_PARAM_STRING(path, path_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    ZEND_PARSE_PARAMETERS_END();

    (void)path_len;
    (void)unused;
    (void)php_mylite_mysqli_connect_impl(return_value, php_mylite_mysqli_link_ce, path);
}

PHP_FUNCTION(mylite_mysqli_global_connect) {
    char *path = NULL;
    size_t path_len = 0;
    zval *unused = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 6)
    Z_PARAM_STRING(path, path_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    ZEND_PARSE_PARAMETERS_END();

    (void)path_len;
    (void)unused;
    (void)php_mylite_mysqli_connect_impl(return_value, php_mylite_mysqli_global_link_ce, path);
}

PHP_FUNCTION(mylite_mysqli_query) {
    zval *link_zval = NULL;
    char *sql = NULL;
    size_t sql_len = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(link_zval)
    Z_PARAM_STRING(sql, sql_len)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }

    php_mylite_mysqli_link *link = php_mylite_mysqli_link_from_object(Z_OBJ_P(link_zval));
    if (php_mylite_mysqli_query_impl(link, Z_OBJ_P(link_zval), sql, sql_len, return_value) !=
        SUCCESS) {
        RETURN_FALSE;
    }
}

PHP_FUNCTION(mylite_mysqli_prepare) {
    zval *link_zval = NULL;
    char *sql = NULL;
    size_t sql_len = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(link_zval)
    Z_PARAM_STRING(sql, sql_len)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }

    php_mylite_mysqli_link *link = php_mylite_mysqli_link_from_object(Z_OBJ_P(link_zval));
    if (php_mylite_mysqli_prepare_impl(link, Z_OBJ_P(link_zval), sql, sql_len, return_value) !=
        SUCCESS) {
        RETURN_FALSE;
    }
}

PHP_FUNCTION(mylite_mysqli_fetch_assoc) {
    zval *result_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(result_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(result_zval) != IS_OBJECT ||
        !php_mylite_mysqli_is_result_object(Z_OBJ_P(result_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli result");
        RETURN_THROWS();
    }

    php_mylite_mysqli_result *result = php_mylite_mysqli_result_from_object(Z_OBJ_P(result_zval));
    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        RETURN_NULL();
    }
    ++result->position;
    RETURN_COPY(row);
}

PHP_FUNCTION(mylite_mysqli_num_rows) {
    zval *result_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(result_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(result_zval) != IS_OBJECT ||
        !php_mylite_mysqli_is_result_object(Z_OBJ_P(result_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli result");
        RETURN_THROWS();
    }

    php_mylite_mysqli_result *result = php_mylite_mysqli_result_from_object(Z_OBJ_P(result_zval));
    RETURN_LONG((zend_long)zend_hash_num_elements(Z_ARRVAL(result->rows)));
}

PHP_FUNCTION(mylite_mysqli_close) {
    zval *link_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(link_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }

    php_mylite_mysqli_link *link = php_mylite_mysqli_link_from_object(Z_OBJ_P(link_zval));
    if (link->db != NULL) {
        const int result = mylite_close(link->db);
        if (result != MYLITE_OK) {
            php_mylite_mysqli_set_error(
                link,
                Z_OBJ_P(link_zval),
                result,
                "could not close database"
            );
            RETURN_FALSE;
        }
        link->db = NULL;
    }
    RETURN_TRUE;
}

PHP_FUNCTION(mylite_mysqli_stmt_bind_param) {
    zval *stmt_zval = NULL;
    zend_string *types = NULL;
    zval *params = NULL;
    uint32_t param_count = 0;

    ZEND_PARSE_PARAMETERS_START(3, -1)
    Z_PARAM_ZVAL(stmt_zval)
    Z_PARAM_STR(types)
    Z_PARAM_VARIADIC('*', params, param_count)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(stmt_zval) != IS_OBJECT || !php_mylite_mysqli_is_stmt_object(Z_OBJ_P(stmt_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli statement");
        RETURN_THROWS();
    }

    php_mylite_mysqli_stmt *stmt = php_mylite_mysqli_stmt_from_object(Z_OBJ_P(stmt_zval));
    if (param_count != ZSTR_LEN(types)) {
        zend_argument_value_error(2, "must contain one type byte per bound parameter");
        RETURN_THROWS();
    }

    php_mylite_mysqli_stmt_clear_bindings(stmt);
    stmt->types = zend_string_copy(types);
    stmt->bound_values = safe_emalloc(param_count, sizeof(zval), 0);
    stmt->bound_count = param_count;
    for (uint32_t index = 0; index < param_count; ++index) {
        ZVAL_COPY(&stmt->bound_values[index], &params[index]);
    }
    RETURN_TRUE;
}

PHP_FUNCTION(mylite_mysqli_stmt_execute) {
    zval *stmt_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(stmt_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(stmt_zval) != IS_OBJECT || !php_mylite_mysqli_is_stmt_object(Z_OBJ_P(stmt_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli statement");
        RETURN_THROWS();
    }

    php_mylite_mysqli_stmt *stmt = php_mylite_mysqli_stmt_from_object(Z_OBJ_P(stmt_zval));
    RETURN_BOOL(php_mylite_mysqli_stmt_execute_impl(stmt) == SUCCESS);
}

PHP_FUNCTION(mylite_mysqli_stmt_get_result) {
    zval *stmt_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(stmt_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(stmt_zval) != IS_OBJECT || !php_mylite_mysqli_is_stmt_object(Z_OBJ_P(stmt_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli statement");
        RETURN_THROWS();
    }

    php_mylite_mysqli_stmt *stmt = php_mylite_mysqli_stmt_from_object(Z_OBJ_P(stmt_zval));
    if (!stmt->has_rows) {
        RETURN_FALSE;
    }
    zval rows;
    ZVAL_COPY(&rows, &stmt->rows);
    php_mylite_mysqli_result_from_rows(return_value, &rows, php_mylite_mysqli_result_ce);
}

PHP_METHOD(MyLite_MySQLi, __construct) {
    php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(ZEND_THIS);
    char *path = NULL;
    size_t path_len = 0;
    zval *unused = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 6)
    Z_PARAM_STRING(path, path_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    ZEND_PARSE_PARAMETERS_END();

    (void)path_len;
    (void)unused;
    if (php_mylite_mysqli_open_link(link, Z_OBJ_P(ZEND_THIS), path) != SUCCESS) {
        zend_throw_error(NULL, "%s", "could not open MyLite mysqli connection");
        RETURN_THROWS();
    }
}

PHP_METHOD(MyLite_MySQLi, query) {
    php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(ZEND_THIS);
    char *sql = NULL;
    size_t sql_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(sql, sql_len)
    ZEND_PARSE_PARAMETERS_END();

    if (php_mylite_mysqli_query_impl(link, Z_OBJ_P(ZEND_THIS), sql, sql_len, return_value) !=
        SUCCESS) {
        RETURN_FALSE;
    }
}

PHP_METHOD(MyLite_MySQLi, prepare) {
    php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(ZEND_THIS);
    char *sql = NULL;
    size_t sql_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(sql, sql_len)
    ZEND_PARSE_PARAMETERS_END();

    if (php_mylite_mysqli_prepare_impl(link, Z_OBJ_P(ZEND_THIS), sql, sql_len, return_value) !=
        SUCCESS) {
        RETURN_FALSE;
    }
}

PHP_METHOD(MyLite_MySQLi, close) {
    ZEND_PARSE_PARAMETERS_NONE();

    php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(ZEND_THIS);
    if (link->db != NULL) {
        const int result = mylite_close(link->db);
        if (result != MYLITE_OK) {
            php_mylite_mysqli_set_error(
                link,
                Z_OBJ_P(ZEND_THIS),
                result,
                "could not close database"
            );
            RETURN_FALSE;
        }
        link->db = NULL;
    }
    RETURN_TRUE;
}

PHP_METHOD(MyLite_MySQLi, real_escape_string) {
    zend_string *input = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STR(input)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_STR(php_mylite_mysqli_escape_sql(input));
}

PHP_METHOD(MyLite_MySQLiResult, fetch_assoc) {
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        RETURN_NULL();
    }
    ++result->position;
    RETURN_COPY(row);
}

PHP_METHOD(MyLite_MySQLiResult, fetch_all) {
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_COPY(&result->rows);
}

PHP_METHOD(MyLite_MySQLiStmt, bind_param) {
    php_mylite_mysqli_stmt *stmt = Z_MYLITE_MYSQLI_STMT_P(ZEND_THIS);
    zend_string *types = NULL;
    zval *params = NULL;
    uint32_t param_count = 0;

    ZEND_PARSE_PARAMETERS_START(1, -1)
    Z_PARAM_STR(types)
    Z_PARAM_VARIADIC('*', params, param_count)
    ZEND_PARSE_PARAMETERS_END();

    if (param_count != ZSTR_LEN(types)) {
        zend_argument_value_error(1, "must contain one type byte per bound parameter");
        RETURN_THROWS();
    }

    php_mylite_mysqli_stmt_clear_bindings(stmt);
    stmt->types = zend_string_copy(types);
    stmt->bound_values = safe_emalloc(param_count, sizeof(zval), 0);
    stmt->bound_count = param_count;
    for (uint32_t index = 0; index < param_count; ++index) {
        ZVAL_COPY(&stmt->bound_values[index], &params[index]);
    }
    RETURN_TRUE;
}

PHP_METHOD(MyLite_MySQLiStmt, execute) {
    php_mylite_mysqli_stmt *stmt = Z_MYLITE_MYSQLI_STMT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_BOOL(php_mylite_mysqli_stmt_execute_impl(stmt) == SUCCESS);
}

PHP_METHOD(MyLite_MySQLiStmt, get_result) {
    php_mylite_mysqli_stmt *stmt = Z_MYLITE_MYSQLI_STMT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    if (!stmt->has_rows) {
        RETURN_FALSE;
    }
    zval rows;
    ZVAL_COPY(&rows, &stmt->rows);
    php_mylite_mysqli_result_from_rows(return_value, &rows, php_mylite_mysqli_result_ce);
}

// NOLINTEND(readability-function-cognitive-complexity)

static const zend_function_entry php_mylite_mysqli_functions[] = {
    ZEND_NS_FENTRY(
        "MyLite",
        mysqli_connect,
        ZEND_FN(mylite_mysqli_connect),
        arginfo_mylite_mysqli_connect,
        0
    )
        ZEND_NS_FENTRY(
            "MyLite",
            mysqli_query,
            ZEND_FN(mylite_mysqli_query),
            arginfo_mylite_mysqli_query,
            0
        )
            ZEND_NS_FENTRY(
                "MyLite",
                mysqli_prepare,
                ZEND_FN(mylite_mysqli_prepare),
                arginfo_mylite_mysqli_prepare,
                0
            )
                ZEND_NS_FENTRY(
                    "MyLite",
                    mysqli_fetch_assoc,
                    ZEND_FN(mylite_mysqli_fetch_assoc),
                    arginfo_mylite_mysqli_fetch_assoc,
                    0
                )
                    ZEND_NS_FENTRY(
                        "MyLite",
                        mysqli_num_rows,
                        ZEND_FN(mylite_mysqli_num_rows),
                        arginfo_mylite_mysqli_num_rows,
                        0
                    )
                        ZEND_NS_FENTRY(
                            "MyLite",
                            mysqli_close,
                            ZEND_FN(mylite_mysqli_close),
                            arginfo_mylite_mysqli_close,
                            0
                        )
                            ZEND_NS_FENTRY(
                                "MyLite",
                                mysqli_stmt_bind_param,
                                ZEND_FN(mylite_mysqli_stmt_bind_param),
                                arginfo_mylite_mysqli_stmt_bind_param_function,
                                0
                            )
                                ZEND_NS_FENTRY(
                                    "MyLite",
                                    mysqli_stmt_execute,
                                    ZEND_FN(mylite_mysqli_stmt_execute),
                                    arginfo_mylite_mysqli_stmt_function,
                                    0
                                )
                                    ZEND_NS_FENTRY(
                                        "MyLite",
                                        mysqli_stmt_get_result,
                                        ZEND_FN(mylite_mysqli_stmt_get_result),
                                        arginfo_mylite_mysqli_stmt_function,
                                        0
                                    )
                                        ZEND_NS_FENTRY(
                                            "MyLite",
                                            mysqli_mylite_global_symbols_enabled,
                                            ZEND_FN(mylite_mysqli_global_symbols_enabled),
                                            arginfo_mylite_mysqli_global_enabled,
                                            0
                                        ) PHP_FE_END
};

static const zend_function_entry php_mylite_mysqli_global_functions[] = {
    ZEND_NAMED_FE(
        mysqli_connect,
        ZEND_FN(mylite_mysqli_global_connect),
        arginfo_mylite_mysqli_connect
    ) ZEND_NAMED_FE(mysqli_query, ZEND_FN(mylite_mysqli_query), arginfo_mylite_mysqli_query)
        ZEND_NAMED_FE(mysqli_prepare, ZEND_FN(mylite_mysqli_prepare), arginfo_mylite_mysqli_prepare)
            ZEND_NAMED_FE(
                mysqli_fetch_assoc,
                ZEND_FN(mylite_mysqli_fetch_assoc),
                arginfo_mylite_mysqli_fetch_assoc
            )
                ZEND_NAMED_FE(
                    mysqli_num_rows,
                    ZEND_FN(mylite_mysqli_num_rows),
                    arginfo_mylite_mysqli_num_rows
                )
                    ZEND_NAMED_FE(
                        mysqli_close,
                        ZEND_FN(mylite_mysqli_close),
                        arginfo_mylite_mysqli_close
                    )
                        ZEND_NAMED_FE(
                            mysqli_stmt_bind_param,
                            ZEND_FN(mylite_mysqli_stmt_bind_param),
                            arginfo_mylite_mysqli_stmt_bind_param_function
                        )
                            ZEND_NAMED_FE(
                                mysqli_stmt_execute,
                                ZEND_FN(mylite_mysqli_stmt_execute),
                                arginfo_mylite_mysqli_stmt_function
                            )
                                ZEND_NAMED_FE(
                                    mysqli_stmt_get_result,
                                    ZEND_FN(mylite_mysqli_stmt_get_result),
                                    arginfo_mylite_mysqli_stmt_function
                                ) PHP_FE_END
};

static const zend_function_entry php_mylite_mysqli_link_methods[] = {
    PHP_ME(MyLite_MySQLi, __construct, arginfo_mylite_mysqli_connect, ZEND_ACC_PUBLIC) PHP_ME(
        MyLite_MySQLi,
        query,
        arginfo_mylite_mysqli_method_query,
        ZEND_ACC_PUBLIC
    ) PHP_ME(MyLite_MySQLi, prepare, arginfo_mylite_mysqli_method_prepare, ZEND_ACC_PUBLIC)
        PHP_ME(MyLite_MySQLi, close, arginfo_mylite_mysqli_bool, ZEND_ACC_PUBLIC)
            PHP_ME(MyLite_MySQLi, real_escape_string, arginfo_mylite_mysqli_escape, ZEND_ACC_PUBLIC)
                PHP_FE_END
};

static const zend_function_entry php_mylite_mysqli_result_methods[] = {PHP_ME(
    MyLite_MySQLiResult,
    fetch_assoc,
    arginfo_mylite_mysqli_method_fetch_assoc,
    ZEND_ACC_PUBLIC
) PHP_ME(MyLite_MySQLiResult, fetch_all, arginfo_mylite_mysqli_fetch_all, ZEND_ACC_PUBLIC)
                                                                           PHP_FE_END};

static const zend_function_entry php_mylite_mysqli_stmt_methods[] = {
    PHP_ME(MyLite_MySQLiStmt, bind_param, arginfo_mylite_mysqli_stmt_bind_param, ZEND_ACC_PUBLIC)
        PHP_ME(MyLite_MySQLiStmt, execute, arginfo_mylite_mysqli_bool, ZEND_ACC_PUBLIC) PHP_ME(
            MyLite_MySQLiStmt,
            get_result,
            arginfo_mylite_mysqli_stmt_get_result,
            ZEND_ACC_PUBLIC
        ) PHP_FE_END
};

static const zend_module_dep php_mylite_mysqli_deps[] = {ZEND_MOD_REQUIRED("mylite") ZEND_MOD_END};

PHP_MINIT_FUNCTION(mysqli_mylite) {
    (void)type;
    (void)module_number;
    zend_class_entry class_entry;

    INIT_NS_CLASS_ENTRY(class_entry, "MyLite", "MySQLi", php_mylite_mysqli_link_methods);
    php_mylite_mysqli_link_ce = zend_register_internal_class(&class_entry);
    php_mylite_mysqli_link_ce->create_object = php_mylite_mysqli_link_create;
    php_mylite_mysqli_declare_link_properties(php_mylite_mysqli_link_ce);

    memcpy(&php_mylite_mysqli_link_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    php_mylite_mysqli_link_handlers.offset = XtOffsetOf(php_mylite_mysqli_link, std);
    php_mylite_mysqli_link_handlers.free_obj = php_mylite_mysqli_link_free;
    php_mylite_mysqli_link_handlers.clone_obj = NULL;

    INIT_NS_CLASS_ENTRY(class_entry, "MyLite", "MySQLiResult", php_mylite_mysqli_result_methods);
    php_mylite_mysqli_result_ce = zend_register_internal_class(&class_entry);
    php_mylite_mysqli_result_ce->create_object = php_mylite_mysqli_result_create;
    php_mylite_mysqli_declare_result_properties(php_mylite_mysqli_result_ce);

    memcpy(&php_mylite_mysqli_result_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    php_mylite_mysqli_result_handlers.offset = XtOffsetOf(php_mylite_mysqli_result, std);
    php_mylite_mysqli_result_handlers.free_obj = php_mylite_mysqli_result_free;
    php_mylite_mysqli_result_handlers.clone_obj = NULL;

    INIT_NS_CLASS_ENTRY(class_entry, "MyLite", "MySQLiStmt", php_mylite_mysqli_stmt_methods);
    php_mylite_mysqli_stmt_ce = zend_register_internal_class(&class_entry);
    php_mylite_mysqli_stmt_ce->create_object = php_mylite_mysqli_stmt_create;

    memcpy(&php_mylite_mysqli_stmt_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    php_mylite_mysqli_stmt_handlers.offset = XtOffsetOf(php_mylite_mysqli_stmt, std);
    php_mylite_mysqli_stmt_handlers.free_obj = php_mylite_mysqli_stmt_free;
    php_mylite_mysqli_stmt_handlers.clone_obj = NULL;

    php_mylite_mysqli_register_global_symbols();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(mysqli_mylite) {
    (void)zend_module;
    const char *global_symbols_status = "unavailable";
    if (php_mylite_mysqli_global_symbols_enabled) {
        global_symbols_status = "enabled";
    }

    php_info_print_table_start();
    php_info_print_table_row(2, "mysqli_mylite support", "enabled");
    php_info_print_table_row(2, "mysqli_mylite version", PHP_MYSQLI_MYLITE_EXT_VERSION);
    php_info_print_table_row(2, "global mysqli replacement symbols", global_symbols_status);
    php_info_print_table_end();
}

static zend_module_entry mysqli_mylite_module_entry = {
    STANDARD_MODULE_HEADER_EX,
    NULL,
    php_mylite_mysqli_deps,
    "mysqli_mylite",
    php_mylite_mysqli_functions,
    PHP_MINIT(mysqli_mylite),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(mysqli_mylite),
    PHP_MYSQLI_MYLITE_EXT_VERSION,
    STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(mysqli_mylite)

static zend_object *php_mylite_mysqli_link_create(zend_class_entry *class_entry) {
    php_mylite_mysqli_link *link = zend_object_alloc(sizeof(php_mylite_mysqli_link), class_entry);
    link->db = NULL;
    zend_object_std_init(&link->std, class_entry);
    object_properties_init(&link->std, class_entry);
    link->std.handlers = &php_mylite_mysqli_link_handlers;
    return &link->std;
}

static void php_mylite_mysqli_link_free(zend_object *object) {
    php_mylite_mysqli_link *link = php_mylite_mysqli_link_from_object(object);
    if (link->db != NULL) {
        (void)mylite_close(link->db);
        link->db = NULL;
    }
    zend_object_std_dtor(&link->std);
}

static zend_object *php_mylite_mysqli_result_create(zend_class_entry *class_entry) {
    php_mylite_mysqli_result *result =
        zend_object_alloc(sizeof(php_mylite_mysqli_result), class_entry);
    array_init(&result->rows);
    result->position = 0;
    zend_object_std_init(&result->std, class_entry);
    object_properties_init(&result->std, class_entry);
    result->std.handlers = &php_mylite_mysqli_result_handlers;
    return &result->std;
}

static void php_mylite_mysqli_result_free(zend_object *object) {
    php_mylite_mysqli_result *result = php_mylite_mysqli_result_from_object(object);
    zval_ptr_dtor(&result->rows);
    zend_object_std_dtor(&result->std);
}

static zend_object *php_mylite_mysqli_stmt_create(zend_class_entry *class_entry) {
    php_mylite_mysqli_stmt *stmt = zend_object_alloc(sizeof(php_mylite_mysqli_stmt), class_entry);
    stmt->stmt = NULL;
    stmt->link_object = NULL;
    stmt->types = NULL;
    stmt->bound_values = NULL;
    stmt->bound_count = 0;
    array_init(&stmt->rows);
    stmt->has_rows = false;
    zend_object_std_init(&stmt->std, class_entry);
    object_properties_init(&stmt->std, class_entry);
    stmt->std.handlers = &php_mylite_mysqli_stmt_handlers;
    return &stmt->std;
}

static void php_mylite_mysqli_stmt_free(zend_object *object) {
    php_mylite_mysqli_stmt *stmt = php_mylite_mysqli_stmt_from_object(object);
    if (stmt->stmt != NULL) {
        (void)mylite_finalize(stmt->stmt);
        stmt->stmt = NULL;
    }
    php_mylite_mysqli_stmt_clear_bindings(stmt);
    zval_ptr_dtor(&stmt->rows);
    stmt->has_rows = false;
    if (stmt->link_object != NULL) {
        OBJ_RELEASE(stmt->link_object);
        stmt->link_object = NULL;
    }
    zend_object_std_dtor(&stmt->std);
}

static php_mylite_mysqli_link *php_mylite_mysqli_link_from_object(zend_object *object) {
    return (php_mylite_mysqli_link *)((char *)object - XtOffsetOf(php_mylite_mysqli_link, std));
}

static php_mylite_mysqli_result *php_mylite_mysqli_result_from_object(zend_object *object) {
    return (php_mylite_mysqli_result *)((char *)object - XtOffsetOf(php_mylite_mysqli_result, std));
}

static php_mylite_mysqli_stmt *php_mylite_mysqli_stmt_from_object(zend_object *object) {
    return (php_mylite_mysqli_stmt *)((char *)object - XtOffsetOf(php_mylite_mysqli_stmt, std));
}

static bool php_mylite_mysqli_is_link_object(zend_object *object) {
    if (instanceof_function(object->ce, php_mylite_mysqli_link_ce)) {
        return true;
    }
    if (php_mylite_mysqli_global_link_ce != NULL &&
        instanceof_function(object->ce, php_mylite_mysqli_global_link_ce)) {
        return true;
    }
    return false;
}

static bool php_mylite_mysqli_is_result_object(zend_object *object) {
    if (instanceof_function(object->ce, php_mylite_mysqli_result_ce)) {
        return true;
    }
    if (php_mylite_mysqli_global_result_ce != NULL &&
        instanceof_function(object->ce, php_mylite_mysqli_global_result_ce)) {
        return true;
    }
    return false;
}

static bool php_mylite_mysqli_is_stmt_object(zend_object *object) {
    if (instanceof_function(object->ce, php_mylite_mysqli_stmt_ce)) {
        return true;
    }
    if (php_mylite_mysqli_global_stmt_ce != NULL &&
        instanceof_function(object->ce, php_mylite_mysqli_global_stmt_ce)) {
        return true;
    }
    return false;
}

static int php_mylite_mysqli_connect_impl(
    zval *return_value,
    zend_class_entry *link_ce,
    const char *path
) {
    object_init_ex(return_value, link_ce);
    php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(return_value);
    if (php_mylite_mysqli_open_link(link, Z_OBJ_P(return_value), path) != SUCCESS) {
        zval_ptr_dtor(return_value);
        ZVAL_FALSE(return_value);
        return FAILURE;
    }
    return SUCCESS;
}

static int php_mylite_mysqli_open_link(
    php_mylite_mysqli_link *link,
    zend_object *object,
    const char *path
) {
    mylite_db *db = NULL;
    const int result = mylite_open(path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, NULL);
    if (result != MYLITE_OK) {
        php_mylite_mysqli_set_error(link, object, result, "could not open MyLite database");
        return FAILURE;
    }
    link->db = db;
    php_mylite_mysqli_clear_error(object);
    php_mylite_mysqli_sync_status(link, object);
    return SUCCESS;
}

static mylite_db *php_mylite_mysqli_require_db(php_mylite_mysqli_link *link) {
    return link->db;
}

static void php_mylite_mysqli_clear_error(zend_object *object) {
    zend_update_property_long(object->ce, object, "errno", sizeof("errno") - 1, 0);
    zend_update_property_string(object->ce, object, "error", sizeof("error") - 1, "");
    zend_update_property_long(object->ce, object, "connect_errno", sizeof("connect_errno") - 1, 0);
    zend_update_property_string(
        object->ce,
        object,
        "connect_error",
        sizeof("connect_error") - 1,
        ""
    );
}

static void php_mylite_mysqli_set_error(
    php_mylite_mysqli_link *link,
    zend_object *object,
    int result,
    const char *fallback
) {
    unsigned mariadb_errno = link->db != NULL ? mylite_mariadb_errno(link->db) : 0U;
    const char *message = fallback;
    if (link->db != NULL && mylite_errmsg(link->db) != NULL) {
        message = mylite_errmsg(link->db);
    }
    if (mariadb_errno == 0U) {
        mariadb_errno = (unsigned)result;
    }
    zend_update_property_long(
        object->ce,
        object,
        "errno",
        sizeof("errno") - 1,
        (zend_long)mariadb_errno
    );
    zend_update_property_string(object->ce, object, "error", sizeof("error") - 1, message);
    zend_update_property_long(
        object->ce,
        object,
        "connect_errno",
        sizeof("connect_errno") - 1,
        (zend_long)mariadb_errno
    );
    zend_update_property_string(
        object->ce,
        object,
        "connect_error",
        sizeof("connect_error") - 1,
        message
    );
}

static void php_mylite_mysqli_sync_status(php_mylite_mysqli_link *link, zend_object *object) {
    if (link->db == NULL) {
        zend_update_property_long(
            object->ce,
            object,
            "affected_rows",
            sizeof("affected_rows") - 1,
            0
        );
        zend_update_property_string(object->ce, object, "insert_id", sizeof("insert_id") - 1, "0");
        return;
    }
    zend_update_property_long(
        object->ce,
        object,
        "affected_rows",
        sizeof("affected_rows") - 1,
        (zend_long)mylite_changes(link->db)
    );
    zend_string *insert_id = zend_u64_to_str(mylite_last_insert_id(link->db));
    zend_update_property_str(object->ce, object, "insert_id", sizeof("insert_id") - 1, insert_id);
    zend_string_release(insert_id);
}

static int php_mylite_mysqli_query_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    size_t sql_len,
    zval *return_value
) {
    mylite_db *db = php_mylite_mysqli_require_db(link);
    if (db == NULL) {
        php_mylite_mysqli_set_error(
            link,
            link_object,
            MYLITE_MISUSE,
            "MyLite mysqli link is closed"
        );
        return FAILURE;
    }

    mylite_stmt *stmt = NULL;
    const int prepare_result = mylite_prepare(db, sql, sql_len, &stmt, NULL);
    if (prepare_result != MYLITE_OK) {
        php_mylite_mysqli_set_error(link, link_object, prepare_result, "could not prepare query");
        return FAILURE;
    }

    zval rows;
    array_init(&rows);
    int step_result = mylite_step(stmt);
    while (step_result == MYLITE_ROW) {
        (void)php_mylite_mysqli_add_current_row(stmt, &rows);
        step_result = mylite_step(stmt);
    }
    const unsigned column_count = mylite_column_count(stmt);
    const int finalize_result = mylite_finalize(stmt);
    if (step_result != MYLITE_DONE || finalize_result != MYLITE_OK) {
        zval_ptr_dtor(&rows);
        php_mylite_mysqli_set_error(link, link_object, step_result, "query failed");
        return FAILURE;
    }

    php_mylite_mysqli_clear_error(link_object);
    php_mylite_mysqli_sync_status(link, link_object);
    if (column_count == 0U) {
        zval_ptr_dtor(&rows);
        ZVAL_TRUE(return_value);
        return SUCCESS;
    }

    php_mylite_mysqli_result_from_rows(
        return_value,
        &rows,
        php_mylite_mysqli_result_class_for_link(link_object)
    );
    return SUCCESS;
}

static int php_mylite_mysqli_prepare_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    size_t sql_len,
    zval *return_value
) {
    mylite_db *db = php_mylite_mysqli_require_db(link);
    if (db == NULL) {
        php_mylite_mysqli_set_error(
            link,
            link_object,
            MYLITE_MISUSE,
            "MyLite mysqli link is closed"
        );
        return FAILURE;
    }

    mylite_stmt *stmt = NULL;
    const int result = mylite_prepare(db, sql, sql_len, &stmt, NULL);
    if (result != MYLITE_OK) {
        php_mylite_mysqli_set_error(link, link_object, result, "could not prepare statement");
        return FAILURE;
    }

    object_init_ex(return_value, php_mylite_mysqli_stmt_class_for_link(link_object));
    php_mylite_mysqli_stmt *stmt_object = Z_MYLITE_MYSQLI_STMT_P(return_value);
    stmt_object->stmt = stmt;
    stmt_object->link_object = link_object;
    GC_ADDREF(stmt_object->link_object);
    return SUCCESS;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
static void php_mylite_mysqli_result_from_rows(
    zval *return_value,
    zval *rows,
    zend_class_entry *result_ce
) {
    object_init_ex(return_value, result_ce);
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(return_value);
    zval_ptr_dtor(&result->rows);
    ZVAL_COPY_VALUE(&result->rows, rows);
    result->position = 0;
    zend_update_property_long(
        result_ce,
        Z_OBJ_P(return_value),
        "num_rows",
        sizeof("num_rows") - 1,
        (zend_long)zend_hash_num_elements(Z_ARRVAL(result->rows))
    );
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static int php_mylite_mysqli_add_current_row(mylite_stmt *stmt, zval *rows) {
    zval row;
    array_init(&row);
    const unsigned column_count = mylite_column_count(stmt);
    for (unsigned column = 0; column < column_count; ++column) {
        zval value;
        const char *name = mylite_column_name(stmt, column);
        php_mylite_mysqli_column_to_zval(stmt, column, &value);
        add_assoc_zval_ex(&row, name != NULL ? name : "", name != NULL ? strlen(name) : 0, &value);
    }
    add_next_index_zval(rows, &row);
    return SUCCESS;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void php_mylite_mysqli_column_to_zval(mylite_stmt *stmt, unsigned column, zval *value) {
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

static int php_mylite_mysqli_bind_zval(mylite_stmt *stmt, unsigned index, zval *value) {
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

static void php_mylite_mysqli_stmt_clear_bindings(php_mylite_mysqli_stmt *stmt) {
    if (stmt->types != NULL) {
        zend_string_release(stmt->types);
        stmt->types = NULL;
    }
    if (stmt->bound_values != NULL) {
        for (uint32_t index = 0; index < stmt->bound_count; ++index) {
            zval_ptr_dtor(&stmt->bound_values[index]);
        }
        efree(stmt->bound_values);
        stmt->bound_values = NULL;
    }
    stmt->bound_count = 0;
}

static void php_mylite_mysqli_stmt_clear_rows(php_mylite_mysqli_stmt *stmt) {
    zval_ptr_dtor(&stmt->rows);
    array_init(&stmt->rows);
    stmt->has_rows = false;
}

static int php_mylite_mysqli_stmt_execute_impl(php_mylite_mysqli_stmt *stmt) {
    if (stmt->stmt == NULL) {
        return FAILURE;
    }

    (void)mylite_reset(stmt->stmt);
    (void)mylite_clear_bindings(stmt->stmt);
    for (uint32_t index = 0; index < stmt->bound_count; ++index) {
        const int bind_result =
            php_mylite_mysqli_bind_zval(stmt->stmt, index + 1U, &stmt->bound_values[index]);
        if (bind_result != MYLITE_OK) {
            return FAILURE;
        }
    }

    php_mylite_mysqli_stmt_clear_rows(stmt);
    int step_result = mylite_step(stmt->stmt);
    const unsigned column_count = mylite_column_count(stmt->stmt);
    while (step_result == MYLITE_ROW) {
        (void)php_mylite_mysqli_add_current_row(stmt->stmt, &stmt->rows);
        step_result = mylite_step(stmt->stmt);
    }
    if (step_result != MYLITE_DONE) {
        return FAILURE;
    }
    stmt->has_rows = column_count > 0U;
    return SUCCESS;
}

static zend_string *php_mylite_mysqli_escape_sql(zend_string *input) {
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

static zend_class_entry *php_mylite_mysqli_result_class_for_link(zend_object *link_object) {
    return link_object->ce == php_mylite_mysqli_global_link_ce &&
                   php_mylite_mysqli_global_result_ce != NULL
               ? php_mylite_mysqli_global_result_ce
               : php_mylite_mysqli_result_ce;
}

static zend_class_entry *php_mylite_mysqli_stmt_class_for_link(zend_object *link_object) {
    return link_object->ce == php_mylite_mysqli_global_link_ce &&
                   php_mylite_mysqli_global_stmt_ce != NULL
               ? php_mylite_mysqli_global_stmt_ce
               : php_mylite_mysqli_stmt_ce;
}

static void php_mylite_mysqli_declare_link_properties(zend_class_entry *class_entry) {
    zend_declare_property_long(class_entry, "errno", sizeof("errno") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_string(class_entry, "error", sizeof("error") - 1, "", ZEND_ACC_PUBLIC);
    zend_declare_property_long(
        class_entry,
        "connect_errno",
        sizeof("connect_errno") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_string(
        class_entry,
        "connect_error",
        sizeof("connect_error") - 1,
        "",
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_long(
        class_entry,
        "affected_rows",
        sizeof("affected_rows") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_string(
        class_entry,
        "insert_id",
        sizeof("insert_id") - 1,
        "0",
        ZEND_ACC_PUBLIC
    );
}

static void php_mylite_mysqli_declare_result_properties(zend_class_entry *class_entry) {
    zend_declare_property_long(class_entry, "num_rows", sizeof("num_rows") - 1, 0, ZEND_ACC_PUBLIC);
}

static void php_mylite_mysqli_register_global_symbols(void) {
    if (zend_hash_str_exists(CG(class_table), "mysqli", sizeof("mysqli") - 1) ||
        zend_hash_str_exists(CG(function_table), "mysqli_connect", sizeof("mysqli_connect") - 1)) {
        php_mylite_mysqli_global_symbols_enabled = false;
        return;
    }

    zend_class_entry class_entry;
    INIT_CLASS_ENTRY(class_entry, "mysqli", php_mylite_mysqli_link_methods);
    php_mylite_mysqli_global_link_ce = zend_register_internal_class(&class_entry);
    php_mylite_mysqli_global_link_ce->create_object = php_mylite_mysqli_link_create;
    php_mylite_mysqli_declare_link_properties(php_mylite_mysqli_global_link_ce);

    INIT_CLASS_ENTRY(class_entry, "mysqli_result", php_mylite_mysqli_result_methods);
    php_mylite_mysqli_global_result_ce = zend_register_internal_class(&class_entry);
    php_mylite_mysqli_global_result_ce->create_object = php_mylite_mysqli_result_create;
    php_mylite_mysqli_declare_result_properties(php_mylite_mysqli_global_result_ce);

    INIT_CLASS_ENTRY(class_entry, "mysqli_stmt", php_mylite_mysqli_stmt_methods);
    php_mylite_mysqli_global_stmt_ce = zend_register_internal_class(&class_entry);
    php_mylite_mysqli_global_stmt_ce->create_object = php_mylite_mysqli_stmt_create;

    zend_register_functions(NULL, php_mylite_mysqli_global_functions, NULL, MODULE_PERSISTENT);
    php_mylite_mysqli_global_symbols_enabled = true;
}

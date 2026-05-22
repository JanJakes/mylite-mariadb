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
#include <stdio.h>
#include <string.h>

#define PHP_MYSQLI_MYLITE_EXT_VERSION "0.1.0"

typedef struct php_mylite_mysqli_link {
    mylite_db *db;
    zend_string *charset;
    zend_object std;
} php_mylite_mysqli_link;

typedef struct php_mylite_mysqli_result {
    zval rows;
    zval fields;
    zend_ulong position;
    zend_ulong field_position;
    zend_object std;
} php_mylite_mysqli_result;

typedef struct php_mylite_mysqli_stmt {
    mylite_stmt *stmt;
    zend_object *link_object;
    zend_string *types;
    zval *bound_values;
    uint32_t bound_count;
    zval rows;
    zval fields;
    bool has_rows;
    zend_object std;
} php_mylite_mysqli_stmt;

typedef struct php_mylite_mysqli_exec_result_context {
    zval *rows;
    zval *fields;
    bool fields_initialized;
} php_mylite_mysqli_exec_result_context;

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
static unsigned php_mylite_mysqli_connect_errno_value;
static char php_mylite_mysqli_connect_error_value[512];

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
static const char *php_mylite_mysqli_connection_path(
    const char *host,
    size_t host_len,
    const char *socket,
    size_t socket_len
);
static int php_mylite_mysqli_select_database(
    php_mylite_mysqli_link *link,
    zend_object *object,
    const char *database,
    size_t database_len
);
static int php_mylite_mysqli_set_charset_impl(
    php_mylite_mysqli_link *link,
    zend_object *object,
    const char *charset,
    size_t charset_len
);
static mylite_db *php_mylite_mysqli_require_db(php_mylite_mysqli_link *link);
static void php_mylite_mysqli_clear_error(zend_object *object);
static void php_mylite_mysqli_clear_connect_error(void);
static void php_mylite_mysqli_set_connect_error(unsigned error_number, const char *message);
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
static int php_mylite_mysqli_exec_query_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    zval *return_value
);
static int php_mylite_mysqli_exec_result_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
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
    zval *fields,
    zend_class_entry *result_ce
);
static void php_mylite_mysqli_fields_from_stmt(mylite_stmt *stmt, zval *fields);
static void php_mylite_mysqli_add_field(
    zval *fields,
    const char *name,
    const char *org_name,
    const char *table,
    const char *org_table
);
static int php_mylite_mysqli_add_current_row(mylite_stmt *stmt, zval *rows);
static void php_mylite_mysqli_column_to_zval(mylite_stmt *stmt, unsigned column, zval *value);
static void php_mylite_mysqli_fetch_array_row(
    php_mylite_mysqli_result *result,
    zval *row,
    zend_long mode,
    zval *return_value
);
static void php_mylite_mysqli_fetch_all_rows(
    php_mylite_mysqli_result *result,
    zend_long mode,
    zval *return_value
);
static void php_mylite_mysqli_fetch_object_row(zval *row, zval *return_value);
static int php_mylite_mysqli_bind_zval(mylite_stmt *stmt, unsigned index, zval *value);
static void php_mylite_mysqli_stmt_clear_bindings(php_mylite_mysqli_stmt *stmt);
static void php_mylite_mysqli_stmt_clear_rows(php_mylite_mysqli_stmt *stmt);
static void php_mylite_mysqli_stmt_clear_fields(php_mylite_mysqli_stmt *stmt);
static int php_mylite_mysqli_stmt_execute_impl(php_mylite_mysqli_stmt *stmt);
static zend_string *php_mylite_mysqli_escape_sql(zend_string *input);
static zend_string *php_mylite_mysqli_use_database_sql(const char *database, size_t database_len);
static zend_string *php_mylite_mysqli_set_charset_sql(const char *charset, size_t charset_len);
static zend_class_entry *php_mylite_mysqli_result_class_for_link(zend_object *link_object);
static zend_class_entry *php_mylite_mysqli_stmt_class_for_link(zend_object *link_object);
static void php_mylite_mysqli_declare_link_properties(zend_class_entry *class_entry);
static void php_mylite_mysqli_declare_result_properties(zend_class_entry *class_entry);
static void php_mylite_mysqli_register_global_symbols(int module_number);
static void php_mylite_mysqli_register_global_constants(int module_number);
static bool php_mylite_mysqli_is_call_query(const char *sql, size_t sql_len);

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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_report, 0, 1, _IS_BOOL, 0)
ZEND_ARG_TYPE_INFO(0, flags, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_mylite_mysqli_init, 0, 0, mysqli, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_real_connect, 0, 1, _IS_BOOL, 0)
ZEND_ARG_INFO(0, mysql)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, hostname, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, username, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, password, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, database, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port, IS_LONG, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, socket, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, flags, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mylite_mysqli_method_real_connect,
    0,
    0,
    _IS_BOOL,
    0
)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, hostname, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, username, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, password, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, database, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port, IS_LONG, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, socket, IS_STRING, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, flags, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_bool, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_select_db, 0, 2, _IS_BOOL, 0)
ZEND_ARG_INFO(0, link)
ZEND_ARG_TYPE_INFO(0, database, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_method_select_db, 0, 1, _IS_BOOL, 0)
ZEND_ARG_TYPE_INFO(0, database, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_set_charset, 0, 2, _IS_BOOL, 0)
ZEND_ARG_INFO(0, link)
ZEND_ARG_TYPE_INFO(0, charset, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_method_set_charset, 0, 1, _IS_BOOL, 0)
ZEND_ARG_TYPE_INFO(0, charset, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_method_string, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_query, 0, 0, 2)
ZEND_ARG_INFO(0, link)
ZEND_ARG_TYPE_INFO(0, query, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, result_mode, IS_LONG, 0, "MYSQLI_STORE_RESULT")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_method_query, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, query, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, result_mode, IS_LONG, 0, "MYSQLI_STORE_RESULT")
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

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_fetch_array, 0, 0, 1)
ZEND_ARG_INFO(0, result)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, mode, IS_LONG, 0, "MYSQLI_BOTH")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_method_fetch_array, 0, 0, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, mode, IS_LONG, 0, "MYSQLI_BOTH")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_fetch_object, 0, 0, 1)
ZEND_ARG_INFO(0, result)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_method_fetch_object, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_method_fetch_assoc, 0, 0, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_close, 0, 0, 1)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_error, 0, 1, IS_STRING, 0)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_errno, 0, 1, IS_LONG, 0)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_connect_error, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_connect_errno, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_affected_rows, 0, 1, IS_LONG, 0)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_insert_id, 0, 0, 1)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mylite_mysqli_real_escape_string,
    0,
    2,
    IS_STRING,
    0
)
ZEND_ARG_INFO(0, link)
ZEND_ARG_TYPE_INFO(0, string, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_num_rows, 0, 1, IS_LONG, 0)
ZEND_ARG_INFO(0, result)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_num_fields, 0, 1, IS_LONG, 0)
ZEND_ARG_INFO(0, result)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_fetch_field, 0, 0, 1)
ZEND_ARG_INFO(0, result)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_free_result, 0, 0, 1)
ZEND_ARG_INFO(0, result)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_more_results, 0, 1, _IS_BOOL, 0)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_next_result, 0, 1, _IS_BOOL, 0)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mylite_mysqli_character_set_name,
    0,
    1,
    IS_STRING,
    0
)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_get_server_info, 0, 1, IS_STRING, 0)
ZEND_ARG_INFO(0, link)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_fetch_all, 0, 1, IS_ARRAY, 0)
ZEND_ARG_INFO(0, result)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, mode, IS_LONG, 0, "MYSQLI_NUM")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_mysqli_method_fetch_all, 0, 0, IS_ARRAY, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, mode, IS_LONG, 0, "MYSQLI_NUM")
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

PHP_FUNCTION(mylite_mysqli_report) {
    zend_long flags = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();

    (void)flags;
    RETURN_TRUE;
}

PHP_FUNCTION(mylite_mysqli_init) {
    ZEND_PARSE_PARAMETERS_NONE();

    object_init_ex(return_value, php_mylite_mysqli_global_link_ce);
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
    char *host = NULL;
    size_t host_len = 0;
    char *database = NULL;
    size_t database_len = 0;
    char *socket = NULL;
    size_t socket_len = 0;
    zval *unused = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 6)
    Z_PARAM_STRING(host, host_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_STRING_OR_NULL(socket, socket_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)unused;
    const char *path = php_mylite_mysqli_connection_path(host, host_len, socket, socket_len);
    if (php_mylite_mysqli_connect_impl(return_value, php_mylite_mysqli_global_link_ce, path) !=
        SUCCESS) {
        return;
    }
    if (database != NULL && database_len > 0) {
        php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(return_value);
        if (php_mylite_mysqli_select_database(
                link,
                Z_OBJ_P(return_value),
                database,
                database_len
            ) != SUCCESS) {
            zval_ptr_dtor(return_value);
            RETURN_FALSE;
        }
    }
}

PHP_FUNCTION(mylite_mysqli_real_connect) {
    zval *link_zval = NULL;
    char *host = NULL;
    size_t host_len = 0;
    char *database = NULL;
    size_t database_len = 0;
    char *socket = NULL;
    size_t socket_len = 0;
    char *unused_string = NULL;
    size_t unused_string_len = 0;
    zval *unused = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 8)
    Z_PARAM_ZVAL(link_zval)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING_OR_NULL(host, host_len)
    Z_PARAM_STRING_OR_NULL(unused_string, unused_string_len)
    Z_PARAM_STRING_OR_NULL(unused_string, unused_string_len)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_STRING_OR_NULL(socket, socket_len)
    Z_PARAM_ZVAL_OR_NULL(unused)
    ZEND_PARSE_PARAMETERS_END();

    (void)unused_string;
    (void)unused_string_len;
    (void)unused;

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }
    const char *path = php_mylite_mysqli_connection_path(host, host_len, socket, socket_len);
    if (path == NULL) {
        php_mylite_mysqli_set_connect_error(
            MYLITE_MISUSE,
            "MyLite mysqli host must be a database directory path"
        );
        RETURN_FALSE;
    }

    php_mylite_mysqli_link *link = php_mylite_mysqli_link_from_object(Z_OBJ_P(link_zval));
    if (php_mylite_mysqli_open_link(link, Z_OBJ_P(link_zval), path) != SUCCESS) {
        RETURN_FALSE;
    }
    if (database != NULL && database_len > 0 &&
        php_mylite_mysqli_select_database(link, Z_OBJ_P(link_zval), database, database_len) !=
            SUCCESS) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_FUNCTION(mylite_mysqli_select_db) {
    zval *link_zval = NULL;
    char *database = NULL;
    size_t database_len = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(link_zval)
    Z_PARAM_STRING(database, database_len)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }

    php_mylite_mysqli_link *link = php_mylite_mysqli_link_from_object(Z_OBJ_P(link_zval));
    RETURN_BOOL(
        php_mylite_mysqli_select_database(link, Z_OBJ_P(link_zval), database, database_len) ==
        SUCCESS
    );
}

PHP_FUNCTION(mylite_mysqli_set_charset) {
    zval *link_zval = NULL;
    char *charset = NULL;
    size_t charset_len = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(link_zval)
    Z_PARAM_STRING(charset, charset_len)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }

    php_mylite_mysqli_link *link = php_mylite_mysqli_link_from_object(Z_OBJ_P(link_zval));
    RETURN_BOOL(
        php_mylite_mysqli_set_charset_impl(link, Z_OBJ_P(link_zval), charset, charset_len) ==
        SUCCESS
    );
}

PHP_FUNCTION(mylite_mysqli_query) {
    zval *link_zval = NULL;
    char *sql = NULL;
    size_t sql_len = 0;
    zend_long unused_result_mode = 0;

    ZEND_PARSE_PARAMETERS_START(2, 3)
    Z_PARAM_ZVAL(link_zval)
    Z_PARAM_STRING(sql, sql_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(unused_result_mode)
    ZEND_PARSE_PARAMETERS_END();

    (void)unused_result_mode;

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

PHP_FUNCTION(mylite_mysqli_fetch_array) {
    zval *result_zval = NULL;
    zend_long mode = 3;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_ZVAL(result_zval)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
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
    php_mylite_mysqli_fetch_array_row(result, row, mode, return_value);
}

PHP_FUNCTION(mylite_mysqli_fetch_object) {
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
    php_mylite_mysqli_fetch_object_row(row, return_value);
}

PHP_FUNCTION(mylite_mysqli_fetch_all) {
    zval *result_zval = NULL;
    zend_long mode = 1;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_ZVAL(result_zval)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(result_zval) != IS_OBJECT ||
        !php_mylite_mysqli_is_result_object(Z_OBJ_P(result_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli result");
        RETURN_THROWS();
    }

    php_mylite_mysqli_result *result = php_mylite_mysqli_result_from_object(Z_OBJ_P(result_zval));
    php_mylite_mysqli_fetch_all_rows(result, mode, return_value);
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

PHP_FUNCTION(mylite_mysqli_num_fields) {
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
    RETURN_LONG((zend_long)zend_hash_num_elements(Z_ARRVAL(result->fields)));
}

PHP_FUNCTION(mylite_mysqli_fetch_field) {
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
    zval *field = zend_hash_index_find(Z_ARRVAL(result->fields), result->field_position);
    if (field == NULL) {
        RETURN_FALSE;
    }
    ++result->field_position;
    RETURN_COPY(field);
}

PHP_FUNCTION(mylite_mysqli_free_result) {
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
    zval_ptr_dtor(&result->rows);
    zval_ptr_dtor(&result->fields);
    array_init(&result->rows);
    array_init(&result->fields);
    result->position = 0;
    result->field_position = 0;
}

PHP_FUNCTION(mylite_mysqli_more_results) {
    zval *unused = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(unused)
    ZEND_PARSE_PARAMETERS_END();

    (void)unused;
    RETURN_FALSE;
}

PHP_FUNCTION(mylite_mysqli_next_result) {
    zval *unused = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(unused)
    ZEND_PARSE_PARAMETERS_END();

    (void)unused;
    RETURN_FALSE;
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

PHP_FUNCTION(mylite_mysqli_error) {
    zval *link_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(link_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }
    zval error;
    zval *error_ptr = zend_read_property(
        Z_OBJCE_P(link_zval),
        Z_OBJ_P(link_zval),
        "error",
        sizeof("error") - 1,
        0,
        &error
    );
    RETURN_STR(zval_get_string(error_ptr));
}

PHP_FUNCTION(mylite_mysqli_errno) {
    zval *link_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(link_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }
    zval errno_value;
    zval *errno_ptr = zend_read_property(
        Z_OBJCE_P(link_zval),
        Z_OBJ_P(link_zval),
        "errno",
        sizeof("errno") - 1,
        0,
        &errno_value
    );
    RETURN_LONG(zval_get_long(errno_ptr));
}

PHP_FUNCTION(mylite_mysqli_connect_error) {
    ZEND_PARSE_PARAMETERS_NONE();

    if (php_mylite_mysqli_connect_error_value[0] == '\0') {
        RETURN_NULL();
    }
    RETURN_STRING(php_mylite_mysqli_connect_error_value);
}

PHP_FUNCTION(mylite_mysqli_connect_errno) {
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_LONG((zend_long)php_mylite_mysqli_connect_errno_value);
}

PHP_FUNCTION(mylite_mysqli_affected_rows) {
    zval *link_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(link_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }
    zval affected_rows;
    zval *affected_rows_ptr = zend_read_property(
        Z_OBJCE_P(link_zval),
        Z_OBJ_P(link_zval),
        "affected_rows",
        sizeof("affected_rows") - 1,
        0,
        &affected_rows
    );
    RETURN_LONG(zval_get_long(affected_rows_ptr));
}

PHP_FUNCTION(mylite_mysqli_insert_id) {
    zval *link_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(link_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }
    zval insert_id;
    zval *insert_id_ptr = zend_read_property(
        Z_OBJCE_P(link_zval),
        Z_OBJ_P(link_zval),
        "insert_id",
        sizeof("insert_id") - 1,
        0,
        &insert_id
    );
    RETURN_COPY(insert_id_ptr);
}

PHP_FUNCTION(mylite_mysqli_real_escape_string) {
    zval *link_zval = NULL;
    zend_string *input = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_ZVAL(link_zval)
    Z_PARAM_STR(input)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }
    RETURN_STR(php_mylite_mysqli_escape_sql(input));
}

PHP_FUNCTION(mylite_mysqli_character_set_name) {
    zval *link_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(link_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }
    php_mylite_mysqli_link *link = php_mylite_mysqli_link_from_object(Z_OBJ_P(link_zval));
    RETURN_STR_COPY(link->charset);
}

PHP_FUNCTION(mylite_mysqli_get_server_info) {
    zval *link_zval = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ZVAL(link_zval)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(link_zval) != IS_OBJECT || !php_mylite_mysqli_is_link_object(Z_OBJ_P(link_zval))) {
        zend_argument_type_error(1, "must be a MyLite mysqli link");
        RETURN_THROWS();
    }
    RETURN_STRING("11.8.6-MariaDB MyLite");
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
    zval fields;
    ZVAL_COPY(&rows, &stmt->rows);
    ZVAL_COPY(&fields, &stmt->fields);
    php_mylite_mysqli_result_from_rows(return_value, &rows, &fields, php_mylite_mysqli_result_ce);
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
    zend_long unused_result_mode = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(sql, sql_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(unused_result_mode)
    ZEND_PARSE_PARAMETERS_END();

    (void)unused_result_mode;

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

PHP_METHOD(MyLite_MySQLi, real_connect) {
    php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(ZEND_THIS);
    char *host = NULL;
    size_t host_len = 0;
    char *database = NULL;
    size_t database_len = 0;
    char *socket = NULL;
    size_t socket_len = 0;
    char *unused_string = NULL;
    size_t unused_string_len = 0;
    zval *unused = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 7)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING_OR_NULL(host, host_len)
    Z_PARAM_STRING_OR_NULL(unused_string, unused_string_len)
    Z_PARAM_STRING_OR_NULL(unused_string, unused_string_len)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    Z_PARAM_ZVAL_OR_NULL(unused)
    Z_PARAM_STRING_OR_NULL(socket, socket_len)
    Z_PARAM_ZVAL_OR_NULL(unused)
    ZEND_PARSE_PARAMETERS_END();

    (void)unused_string;
    (void)unused_string_len;
    (void)unused;
    const char *path = php_mylite_mysqli_connection_path(host, host_len, socket, socket_len);
    if (path == NULL) {
        php_mylite_mysqli_set_connect_error(
            MYLITE_MISUSE,
            "MyLite mysqli host must be a database directory path"
        );
        RETURN_FALSE;
    }

    if (php_mylite_mysqli_open_link(link, Z_OBJ_P(ZEND_THIS), path) != SUCCESS) {
        RETURN_FALSE;
    }
    if (database != NULL && database_len > 0 &&
        php_mylite_mysqli_select_database(link, Z_OBJ_P(ZEND_THIS), database, database_len) !=
            SUCCESS) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_METHOD(MyLite_MySQLi, select_db) {
    php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(ZEND_THIS);
    char *database = NULL;
    size_t database_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(database, database_len)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(
        php_mylite_mysqli_select_database(link, Z_OBJ_P(ZEND_THIS), database, database_len) ==
        SUCCESS
    );
}

PHP_METHOD(MyLite_MySQLi, set_charset) {
    php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(ZEND_THIS);
    char *charset = NULL;
    size_t charset_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(charset, charset_len)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(
        php_mylite_mysqli_set_charset_impl(link, Z_OBJ_P(ZEND_THIS), charset, charset_len) ==
        SUCCESS
    );
}

PHP_METHOD(MyLite_MySQLi, character_set_name) {
    php_mylite_mysqli_link *link = Z_MYLITE_MYSQLI_LINK_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_STR_COPY(link->charset);
}

PHP_METHOD(MyLite_MySQLi, get_server_info) {
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_STRING("11.8.6-MariaDB MyLite");
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

PHP_METHOD(MyLite_MySQLiResult, fetch_array) {
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(ZEND_THIS);
    zend_long mode = 3;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        RETURN_NULL();
    }
    ++result->position;
    php_mylite_mysqli_fetch_array_row(result, row, mode, return_value);
}

PHP_METHOD(MyLite_MySQLiResult, fetch_object) {
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        RETURN_NULL();
    }
    ++result->position;
    php_mylite_mysqli_fetch_object_row(row, return_value);
}

PHP_METHOD(MyLite_MySQLiResult, fetch_all) {
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(ZEND_THIS);
    zend_long mode = 1;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    php_mylite_mysqli_fetch_all_rows(result, mode, return_value);
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
    zval fields;
    ZVAL_COPY(&rows, &stmt->rows);
    ZVAL_COPY(&fields, &stmt->fields);
    php_mylite_mysqli_result_from_rows(return_value, &rows, &fields, php_mylite_mysqli_result_ce);
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
                        mysqli_fetch_array,
                        ZEND_FN(mylite_mysqli_fetch_array),
                        arginfo_mylite_mysqli_fetch_array,
                        0
                    )
                        ZEND_NS_FENTRY(
                            "MyLite",
                            mysqli_fetch_object,
                            ZEND_FN(mylite_mysqli_fetch_object),
                            arginfo_mylite_mysqli_fetch_object,
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
    ZEND_NAMED_FE(mysqli_report, ZEND_FN(mylite_mysqli_report), arginfo_mylite_mysqli_report) ZEND_NAMED_FE(
        mysqli_init,
        ZEND_FN(mylite_mysqli_init),
        arginfo_mylite_mysqli_init
    ) ZEND_NAMED_FE(mysqli_connect, ZEND_FN(mylite_mysqli_global_connect), arginfo_mylite_mysqli_connect)
        ZEND_NAMED_FE(
            mysqli_real_connect,
            ZEND_FN(mylite_mysqli_real_connect),
            arginfo_mylite_mysqli_real_connect
        ) ZEND_NAMED_FE(mysqli_select_db, ZEND_FN(mylite_mysqli_select_db), arginfo_mylite_mysqli_select_db)
            ZEND_NAMED_FE(
                mysqli_set_charset,
                ZEND_FN(mylite_mysqli_set_charset),
                arginfo_mylite_mysqli_set_charset
            ) ZEND_NAMED_FE(mysqli_query, ZEND_FN(mylite_mysqli_query), arginfo_mylite_mysqli_query)
                ZEND_NAMED_FE(mysqli_prepare, ZEND_FN(mylite_mysqli_prepare), arginfo_mylite_mysqli_prepare) ZEND_NAMED_FE(
                    mysqli_fetch_assoc,
                    ZEND_FN(mylite_mysqli_fetch_assoc),
                    arginfo_mylite_mysqli_fetch_assoc
                ) ZEND_NAMED_FE(mysqli_fetch_array, ZEND_FN(mylite_mysqli_fetch_array), arginfo_mylite_mysqli_fetch_array)
                    ZEND_NAMED_FE(
                        mysqli_fetch_object,
                        ZEND_FN(mylite_mysqli_fetch_object),
                        arginfo_mylite_mysqli_fetch_object
                    ) ZEND_NAMED_FE(mysqli_fetch_all, ZEND_FN(mylite_mysqli_fetch_all), arginfo_mylite_mysqli_fetch_all)
                        ZEND_NAMED_FE(
                            mysqli_num_rows,
                            ZEND_FN(mylite_mysqli_num_rows),
                            arginfo_mylite_mysqli_num_rows
                        ) ZEND_NAMED_FE(mysqli_num_fields, ZEND_FN(mylite_mysqli_num_fields), arginfo_mylite_mysqli_num_fields)
                            ZEND_NAMED_FE(
                                mysqli_fetch_field,
                                ZEND_FN(mylite_mysqli_fetch_field),
                                arginfo_mylite_mysqli_fetch_field
                            ) ZEND_NAMED_FE(mysqli_free_result, ZEND_FN(mylite_mysqli_free_result), arginfo_mylite_mysqli_free_result)
                                ZEND_NAMED_FE(
                                    mysqli_more_results,
                                    ZEND_FN(mylite_mysqli_more_results),
                                    arginfo_mylite_mysqli_more_results
                                )
                                    ZEND_NAMED_FE(
                                        mysqli_next_result,
                                        ZEND_FN(mylite_mysqli_next_result),
                                        arginfo_mylite_mysqli_next_result
                                    ) ZEND_NAMED_FE(mysqli_close, ZEND_FN(mylite_mysqli_close), arginfo_mylite_mysqli_close)
                                        ZEND_NAMED_FE(
                                            mysqli_error,
                                            ZEND_FN(mylite_mysqli_error),
                                            arginfo_mylite_mysqli_error
                                        ) ZEND_NAMED_FE(mysqli_errno, ZEND_FN(mylite_mysqli_errno), arginfo_mylite_mysqli_errno)
                                            ZEND_NAMED_FE(
                                                mysqli_connect_error,
                                                ZEND_FN(mylite_mysqli_connect_error),
                                                arginfo_mylite_mysqli_connect_error
                                            )
                                                ZEND_NAMED_FE(
                                                    mysqli_connect_errno,
                                                    ZEND_FN(mylite_mysqli_connect_errno),
                                                    arginfo_mylite_mysqli_connect_errno
                                                )
                                                    ZEND_NAMED_FE(
                                                        mysqli_affected_rows,
                                                        ZEND_FN(mylite_mysqli_affected_rows),
                                                        arginfo_mylite_mysqli_affected_rows
                                                    )
                                                        ZEND_NAMED_FE(
                                                            mysqli_insert_id,
                                                            ZEND_FN(mylite_mysqli_insert_id),
                                                            arginfo_mylite_mysqli_insert_id
                                                        )
                                                            ZEND_NAMED_FE(
                                                                mysqli_real_escape_string,
                                                                ZEND_FN(
                                                                    mylite_mysqli_real_escape_string
                                                                ),
                                                                arginfo_mylite_mysqli_real_escape_string
                                                            )
                                                                ZEND_NAMED_FE(
                                                                    mysqli_character_set_name,
                                                                    ZEND_FN(
                                                                        mylite_mysqli_character_set_name
                                                                    ),
                                                                    arginfo_mylite_mysqli_character_set_name
                                                                )
                                                                    ZEND_NAMED_FE(
                                                                        mysqli_get_server_info,
                                                                        ZEND_FN(
                                                                            mylite_mysqli_get_server_info
                                                                        ),
                                                                        arginfo_mylite_mysqli_get_server_info
                                                                    )
                                                                        ZEND_NAMED_FE(
                                                                            mysqli_stmt_bind_param,
                                                                            ZEND_FN(
                                                                                mylite_mysqli_stmt_bind_param
                                                                            ),
                                                                            arginfo_mylite_mysqli_stmt_bind_param_function
                                                                        )
                                                                            ZEND_NAMED_FE(
                                                                                mysqli_stmt_execute,
                                                                                ZEND_FN(
                                                                                    mylite_mysqli_stmt_execute
                                                                                ),
                                                                                arginfo_mylite_mysqli_stmt_function
                                                                            )
                                                                                ZEND_NAMED_FE(
                                                                                    mysqli_stmt_get_result,
                                                                                    ZEND_FN(
                                                                                        mylite_mysqli_stmt_get_result
                                                                                    ),
                                                                                    arginfo_mylite_mysqli_stmt_function
                                                                                ) PHP_FE_END
};

static const zend_function_entry php_mylite_mysqli_link_methods[] = {
    PHP_ME(MyLite_MySQLi, __construct, arginfo_mylite_mysqli_connect, ZEND_ACC_PUBLIC) PHP_ME(
        MyLite_MySQLi,
        real_connect,
        arginfo_mylite_mysqli_method_real_connect,
        ZEND_ACC_PUBLIC
    ) PHP_ME(MyLite_MySQLi, query, arginfo_mylite_mysqli_method_query, ZEND_ACC_PUBLIC)
        PHP_ME(
            MyLite_MySQLi,
            prepare,
            arginfo_mylite_mysqli_method_prepare,
            ZEND_ACC_PUBLIC
        ) PHP_ME(MyLite_MySQLi, select_db, arginfo_mylite_mysqli_method_select_db, ZEND_ACC_PUBLIC)
            PHP_ME(
                MyLite_MySQLi,
                set_charset,
                arginfo_mylite_mysqli_method_set_charset,
                ZEND_ACC_PUBLIC
            )
                PHP_ME(
                    MyLite_MySQLi,
                    character_set_name,
                    arginfo_mylite_mysqli_method_string,
                    ZEND_ACC_PUBLIC
                )
                    PHP_ME(
                        MyLite_MySQLi,
                        get_server_info,
                        arginfo_mylite_mysqli_method_string,
                        ZEND_ACC_PUBLIC
                    ) PHP_ME(MyLite_MySQLi, close, arginfo_mylite_mysqli_bool, ZEND_ACC_PUBLIC)
                        PHP_ME(
                            MyLite_MySQLi,
                            real_escape_string,
                            arginfo_mylite_mysqli_escape,
                            ZEND_ACC_PUBLIC
                        ) PHP_FE_END
};

static const zend_function_entry php_mylite_mysqli_result_methods[] = {
    PHP_ME(
        MyLite_MySQLiResult,
        fetch_assoc,
        arginfo_mylite_mysqli_method_fetch_assoc,
        ZEND_ACC_PUBLIC
    )
        PHP_ME(
            MyLite_MySQLiResult,
            fetch_array,
            arginfo_mylite_mysqli_method_fetch_array,
            ZEND_ACC_PUBLIC
        )
            PHP_ME(
                MyLite_MySQLiResult,
                fetch_object,
                arginfo_mylite_mysqli_method_fetch_object,
                ZEND_ACC_PUBLIC
            )
                PHP_ME(
                    MyLite_MySQLiResult,
                    fetch_all,
                    arginfo_mylite_mysqli_method_fetch_all,
                    ZEND_ACC_PUBLIC
                ) PHP_FE_END
};

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

    php_mylite_mysqli_register_global_symbols(module_number);
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
    link->charset = zend_string_init("utf8mb4", sizeof("utf8mb4") - 1, false);
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
    if (link->charset != NULL) {
        zend_string_release(link->charset);
        link->charset = NULL;
    }
    zend_object_std_dtor(&link->std);
}

static zend_object *php_mylite_mysqli_result_create(zend_class_entry *class_entry) {
    php_mylite_mysqli_result *result =
        zend_object_alloc(sizeof(php_mylite_mysqli_result), class_entry);
    array_init(&result->rows);
    array_init(&result->fields);
    result->position = 0;
    result->field_position = 0;
    zend_object_std_init(&result->std, class_entry);
    object_properties_init(&result->std, class_entry);
    result->std.handlers = &php_mylite_mysqli_result_handlers;
    return &result->std;
}

static void php_mylite_mysqli_result_free(zend_object *object) {
    php_mylite_mysqli_result *result = php_mylite_mysqli_result_from_object(object);
    zval_ptr_dtor(&result->rows);
    zval_ptr_dtor(&result->fields);
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
    array_init(&stmt->fields);
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
    zval_ptr_dtor(&stmt->fields);
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
    if (path == NULL || path[0] == '\0') {
        php_mylite_mysqli_set_error(
            link,
            object,
            MYLITE_MISUSE,
            "MyLite mysqli host must be a database directory path"
        );
        php_mylite_mysqli_set_connect_error(
            MYLITE_MISUSE,
            "MyLite mysqli host must be a database directory path"
        );
        return FAILURE;
    }

    if (link->db != NULL) {
        (void)mylite_close(link->db);
        link->db = NULL;
    }

    mylite_db *db = NULL;
    const int result = mylite_open(path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, NULL);
    if (result != MYLITE_OK) {
        php_mylite_mysqli_set_error(link, object, result, "could not open MyLite database");
        php_mylite_mysqli_set_connect_error((unsigned)result, "could not open MyLite database");
        return FAILURE;
    }
    link->db = db;
    php_mylite_mysqli_clear_connect_error();
    php_mylite_mysqli_clear_error(object);
    php_mylite_mysqli_sync_status(link, object);
    return SUCCESS;
}

static const char *php_mylite_mysqli_connection_path(
    const char *host,
    size_t host_len,
    const char *socket,
    size_t socket_len
) {
    if (socket != NULL && socket_len > 0) {
        return socket;
    }
    if (host != NULL && host_len > 0) {
        return host;
    }
    return NULL;
}

static int php_mylite_mysqli_select_database(
    php_mylite_mysqli_link *link,
    zend_object *object,
    const char *database,
    size_t database_len
) {
    mylite_db *db = php_mylite_mysqli_require_db(link);
    if (db == NULL) {
        php_mylite_mysqli_set_error(link, object, MYLITE_MISUSE, "MyLite mysqli link is closed");
        return FAILURE;
    }

    zend_string *sql = php_mylite_mysqli_use_database_sql(database, database_len);
    const int result = mylite_exec(db, ZSTR_VAL(sql), NULL, NULL, NULL);
    zend_string_release(sql);
    if (result != MYLITE_OK) {
        php_mylite_mysqli_set_error(link, object, result, "could not select database");
        return FAILURE;
    }
    php_mylite_mysqli_clear_error(object);
    return SUCCESS;
}

static int php_mylite_mysqli_set_charset_impl(
    php_mylite_mysqli_link *link,
    zend_object *object,
    const char *charset,
    size_t charset_len
) {
    mylite_db *db = php_mylite_mysqli_require_db(link);
    if (db == NULL) {
        php_mylite_mysqli_set_error(link, object, MYLITE_MISUSE, "MyLite mysqli link is closed");
        return FAILURE;
    }

    zend_string *sql = php_mylite_mysqli_set_charset_sql(charset, charset_len);
    const int result = mylite_exec(db, ZSTR_VAL(sql), NULL, NULL, NULL);
    zend_string_release(sql);
    if (result != MYLITE_OK) {
        php_mylite_mysqli_set_error(link, object, result, "could not set connection charset");
        return FAILURE;
    }

    zend_string_release(link->charset);
    link->charset = zend_string_init(charset, charset_len, false);
    php_mylite_mysqli_clear_error(object);
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

static void php_mylite_mysqli_clear_connect_error(void) {
    php_mylite_mysqli_connect_errno_value = 0;
    php_mylite_mysqli_connect_error_value[0] = '\0';
}

static void php_mylite_mysqli_set_connect_error(unsigned error_number, const char *message) {
    php_mylite_mysqli_connect_errno_value = error_number;
    snprintf(
        php_mylite_mysqli_connect_error_value,
        sizeof(php_mylite_mysqli_connect_error_value),
        "%s",
        message != NULL ? message : ""
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

    if (php_mylite_mysqli_is_call_query(sql, sql_len)) {
        return php_mylite_mysqli_exec_query_impl(link, link_object, sql, return_value);
    }

    mylite_stmt *stmt = NULL;
    const int prepare_result = mylite_prepare(db, sql, sql_len, &stmt, NULL);
    if (prepare_result != MYLITE_OK) {
        php_mylite_mysqli_set_error(link, link_object, prepare_result, "could not prepare query");
        return FAILURE;
    }

    zval rows;
    zval fields;
    array_init(&rows);
    array_init(&fields);
    int step_result = mylite_step(stmt);
    while (step_result == MYLITE_ROW) {
        (void)php_mylite_mysqli_add_current_row(stmt, &rows);
        step_result = mylite_step(stmt);
    }
    const unsigned column_count = mylite_column_count(stmt);
    php_mylite_mysqli_fields_from_stmt(stmt, &fields);
    const int finalize_result = mylite_finalize(stmt);
    if (step_result != MYLITE_DONE || finalize_result != MYLITE_OK) {
        zval_ptr_dtor(&rows);
        zval_ptr_dtor(&fields);
        php_mylite_mysqli_set_error(link, link_object, step_result, "query failed");
        return FAILURE;
    }

    php_mylite_mysqli_clear_error(link_object);
    php_mylite_mysqli_sync_status(link, link_object);
    if (column_count == 0U) {
        zval_ptr_dtor(&rows);
        zval_ptr_dtor(&fields);
        ZVAL_TRUE(return_value);
        return SUCCESS;
    }

    php_mylite_mysqli_result_from_rows(
        return_value,
        &rows,
        &fields,
        php_mylite_mysqli_result_class_for_link(link_object)
    );
    return SUCCESS;
}

static int php_mylite_mysqli_exec_query_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    zval *return_value
) {
    zval rows;
    zval fields;
    array_init(&rows);
    array_init(&fields);

    php_mylite_mysqli_exec_result_context ctx = {
        .rows = &rows,
        .fields = &fields,
        .fields_initialized = false,
    };
    char *errmsg = NULL;
    const int result =
        mylite_exec(link->db, sql, php_mylite_mysqli_exec_result_callback, &ctx, &errmsg);
    mylite_free(errmsg);
    if (result != MYLITE_OK) {
        zval_ptr_dtor(&rows);
        zval_ptr_dtor(&fields);
        php_mylite_mysqli_set_error(link, link_object, result, "query failed");
        return FAILURE;
    }

    php_mylite_mysqli_clear_error(link_object);
    php_mylite_mysqli_sync_status(link, link_object);
    if (!ctx.fields_initialized) {
        zval_ptr_dtor(&rows);
        zval_ptr_dtor(&fields);
        ZVAL_TRUE(return_value);
        return SUCCESS;
    }

    php_mylite_mysqli_result_from_rows(
        return_value,
        &rows,
        &fields,
        php_mylite_mysqli_result_class_for_link(link_object)
    );
    return SUCCESS;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): required libmylite callback signature.
static int php_mylite_mysqli_exec_result_callback(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
) {
    php_mylite_mysqli_exec_result_context *result_ctx =
        (php_mylite_mysqli_exec_result_context *)ctx;
    if (!result_ctx->fields_initialized) {
        for (int column = 0; column < column_count; ++column) {
            const char *column_name = column_names[column] != NULL ? column_names[column] : "";
            php_mylite_mysqli_add_field(result_ctx->fields, column_name, column_name, "", "");
        }
        result_ctx->fields_initialized = true;
    }

    zval row;
    array_init(&row);
    for (int column = 0; column < column_count; ++column) {
        zval value;
        if (values[column] == NULL) {
            ZVAL_NULL(&value);
        } else {
            ZVAL_STRING(&value, values[column]);
        }
        add_assoc_zval_ex(
            &row,
            column_names[column] != NULL ? column_names[column] : "",
            column_names[column] != NULL ? strlen(column_names[column]) : 0,
            &value
        );
    }
    add_next_index_zval(result_ctx->rows, &row);
    return 0;
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
    zval *fields,
    zend_class_entry *result_ce
) {
    object_init_ex(return_value, result_ce);
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(return_value);
    zval_ptr_dtor(&result->rows);
    zval_ptr_dtor(&result->fields);
    ZVAL_COPY_VALUE(&result->rows, rows);
    ZVAL_COPY_VALUE(&result->fields, fields);
    result->position = 0;
    result->field_position = 0;
    zend_update_property_long(
        result_ce,
        Z_OBJ_P(return_value),
        "num_rows",
        sizeof("num_rows") - 1,
        (zend_long)zend_hash_num_elements(Z_ARRVAL(result->rows))
    );
}

// NOLINTEND(bugprone-easily-swappable-parameters)

static void php_mylite_mysqli_fields_from_stmt(mylite_stmt *stmt, zval *fields) {
    const unsigned column_count = mylite_column_count(stmt);
    for (unsigned column = 0; column < column_count; ++column) {
        php_mylite_mysqli_add_field(
            fields,
            mylite_column_name(stmt, column),
            mylite_column_org_name(stmt, column),
            mylite_column_table(stmt, column),
            mylite_column_org_table(stmt, column)
        );
    }
}

static void php_mylite_mysqli_add_field(
    zval *fields,
    const char *name,
    const char *org_name,
    const char *table,
    const char *org_table
) {
    zval field;
    object_init(&field);
    add_property_string(&field, "name", name != NULL ? name : "");
    add_property_string(&field, "orgname", org_name != NULL ? org_name : "");
    add_property_string(&field, "table", table != NULL ? table : "");
    add_property_string(&field, "orgtable", org_table != NULL ? org_table : "");
    add_property_string(&field, "def", "");
    add_property_long(&field, "max_length", 0);
    add_property_long(&field, "not_null", 0);
    add_property_long(&field, "primary_key", 0);
    add_property_long(&field, "multiple_key", 0);
    add_property_long(&field, "unique_key", 0);
    add_property_long(&field, "numeric", 0);
    add_property_long(&field, "blob", 0);
    add_property_long(&field, "type", 0);
    add_property_long(&field, "unsigned", 0);
    add_property_long(&field, "zerofill", 0);
    add_next_index_zval(fields, &field);
}

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

static void php_mylite_mysqli_column_to_zval(mylite_stmt *stmt, unsigned column, zval *value) {
    if (mylite_column_type(stmt, column) == MYLITE_TYPE_NULL) {
        ZVAL_NULL(value);
        return;
    }

    const char *text = mylite_column_text(stmt, column);
    if (text == NULL) {
        ZVAL_NULL(value);
        return;
    }

    ZVAL_STRINGL(value, text, mylite_column_bytes(stmt, column));
}

static void php_mylite_mysqli_fetch_array_row(
    php_mylite_mysqli_result *result,
    zval *row,
    zend_long mode,
    zval *return_value
) {
    array_init(return_value);
    if ((mode & 2) != 0) {
        zend_string *key = NULL;
        zval *value = NULL;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(row), key, value) {
            if (key != NULL) {
                Z_TRY_ADDREF_P(value);
                add_assoc_zval_ex(return_value, ZSTR_VAL(key), ZSTR_LEN(key), value);
            }
        }
        ZEND_HASH_FOREACH_END();
    }
    if ((mode & 1) != 0) {
        zval *field = NULL;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(result->fields), field) {
            zval name;
            zval *name_ptr = zend_read_property(
                Z_OBJCE_P(field),
                Z_OBJ_P(field),
                "name",
                sizeof("name") - 1,
                0,
                &name
            );
            zend_string *name_string = zval_get_string(name_ptr);
            zval *value = zend_hash_find(Z_ARRVAL_P(row), name_string);
            if (value != NULL) {
                Z_TRY_ADDREF_P(value);
                add_next_index_zval(return_value, value);
            }
            zend_string_release(name_string);
        }
        ZEND_HASH_FOREACH_END();
    }
}

static void php_mylite_mysqli_fetch_all_rows(
    php_mylite_mysqli_result *result,
    zend_long mode,
    zval *return_value
) {
    array_init(return_value);
    zval *row = NULL;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(result->rows), row) {
        zval fetch_row;
        php_mylite_mysqli_fetch_array_row(result, row, mode, &fetch_row);
        add_next_index_zval(return_value, &fetch_row);
    }
    ZEND_HASH_FOREACH_END();
    result->position = zend_hash_num_elements(Z_ARRVAL(result->rows));
}

static void php_mylite_mysqli_fetch_object_row(zval *row, zval *return_value) {
    object_init(return_value);
    zend_string *key = NULL;
    zval *value = NULL;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(row), key, value) {
        if (key != NULL) {
            Z_TRY_ADDREF_P(value);
            add_property_zval_ex(return_value, ZSTR_VAL(key), ZSTR_LEN(key), value);
        }
    }
    ZEND_HASH_FOREACH_END();
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

static void php_mylite_mysqli_stmt_clear_fields(php_mylite_mysqli_stmt *stmt) {
    zval_ptr_dtor(&stmt->fields);
    array_init(&stmt->fields);
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
    php_mylite_mysqli_stmt_clear_fields(stmt);
    int step_result = mylite_step(stmt->stmt);
    const unsigned column_count = mylite_column_count(stmt->stmt);
    while (step_result == MYLITE_ROW) {
        (void)php_mylite_mysqli_add_current_row(stmt->stmt, &stmt->rows);
        step_result = mylite_step(stmt->stmt);
    }
    if (step_result != MYLITE_DONE) {
        return FAILURE;
    }
    php_mylite_mysqli_fields_from_stmt(stmt->stmt, &stmt->fields);
    stmt->has_rows = column_count > 0U;
    return SUCCESS;
}

static zend_string *php_mylite_mysqli_use_database_sql(const char *database, size_t database_len) {
    zend_string *sql = zend_string_alloc(sizeof("USE ``") - 1 + database_len * 2, false);
    char *target = ZSTR_VAL(sql);
    memcpy(target, "USE `", sizeof("USE `") - 1);
    target += sizeof("USE `") - 1;
    for (size_t index = 0; index < database_len; ++index) {
        if (database[index] == '`') {
            *target++ = '`';
        }
        *target++ = database[index];
    }
    *target++ = '`';
    *target = '\0';
    ZSTR_LEN(sql) = (size_t)(target - ZSTR_VAL(sql));
    return sql;
}

static zend_string *php_mylite_mysqli_set_charset_sql(const char *charset, size_t charset_len) {
    zend_string *sql = zend_string_alloc(sizeof("SET NAMES ") - 1 + charset_len, false);
    memcpy(ZSTR_VAL(sql), "SET NAMES ", sizeof("SET NAMES ") - 1);
    memcpy(ZSTR_VAL(sql) + sizeof("SET NAMES ") - 1, charset, charset_len);
    ZSTR_VAL(sql)[sizeof("SET NAMES ") - 1 + charset_len] = '\0';
    ZSTR_LEN(sql) = sizeof("SET NAMES ") - 1 + charset_len;
    return sql;
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

static void php_mylite_mysqli_register_global_symbols(int module_number) {
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
    php_mylite_mysqli_register_global_constants(module_number);
    php_mylite_mysqli_global_symbols_enabled = true;
}

static void php_mylite_mysqli_register_global_constants(int module_number) {
    REGISTER_LONG_CONSTANT("MYSQLI_ASSOC", 2, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_NUM", 1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_BOTH", 3, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_STORE_RESULT", 0, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_USE_RESULT", 1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_OFF", 0, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_ERROR", 1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_STRICT", 2, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_INDEX", 4, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_ALL", 255, CONST_CS | CONST_PERSISTENT);
}

static bool php_mylite_mysqli_is_call_query(const char *sql, size_t sql_len) {
    size_t offset = 0;
    while (offset < sql_len) {
        const char value = sql[offset];
        if (value != ' ' && value != '\t' && value != '\n' && value != '\r' && value != '\f') {
            break;
        }
        ++offset;
    }

    if (sql_len - offset < 4U) {
        return false;
    }

    const char c0 = sql[offset];
    const char c1 = sql[offset + 1U];
    const char c2 = sql[offset + 2U];
    const char c3 = sql[offset + 3U];
    if ((c0 != 'C' && c0 != 'c') || (c1 != 'A' && c1 != 'a') || (c2 != 'L' && c2 != 'l') ||
        (c3 != 'L' && c3 != 'l')) {
        return false;
    }

    if (sql_len == offset + 4U) {
        return true;
    }

    const char next = sql[offset + 4U];
    return next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '\f';
}

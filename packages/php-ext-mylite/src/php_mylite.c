// clang-format off
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <php.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_interfaces.h>
#include <ext/standard/info.h>
// clang-format on

#include <mylite/mylite.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PHP_MYLITE_EXT_VERSION "0.1.0"

typedef struct php_mylite_connection {
    mylite_db *db;
    zend_object std;
} php_mylite_connection;

typedef struct php_mylite_statement {
    mylite_stmt *stmt;
    zend_object *connection_object;
    int step_result;
    zend_object std;
} php_mylite_statement;

typedef struct php_mylite_result {
    zval rows;
    zend_ulong position;
    zend_object std;
} php_mylite_result;

static zend_class_entry *php_mylite_connection_ce;
static zend_class_entry *php_mylite_statement_ce;
static zend_class_entry *php_mylite_result_ce;
static zend_class_entry *php_mylite_exception_ce;
static zend_object_handlers php_mylite_connection_handlers;
static zend_object_handlers php_mylite_statement_handlers;
static zend_object_handlers php_mylite_result_handlers;

static zend_object *php_mylite_connection_create(zend_class_entry *class_entry);
static void php_mylite_connection_free(zend_object *object);
static zend_object *php_mylite_statement_create(zend_class_entry *class_entry);
static void php_mylite_statement_free(zend_object *object);
static zend_object *php_mylite_result_create(zend_class_entry *class_entry);
static void php_mylite_result_free(zend_object *object);

static php_mylite_connection *php_mylite_connection_from_object(zend_object *object);
static php_mylite_statement *php_mylite_statement_from_object(zend_object *object);
static php_mylite_result *php_mylite_result_from_object(zend_object *object);
static mylite_db *php_mylite_require_db(php_mylite_connection *connection);
static void php_mylite_open_into_object(zval *return_value, const char *path, zend_long flags);
static bool php_mylite_validate_open_flags(zend_long flags);
static void php_mylite_throw_db(mylite_db *db, int result, const char *fallback);
static void php_mylite_throw_result(int result, const char *fallback);
static void php_mylite_register_constants(int module_number);
static int php_mylite_bind_zval(mylite_stmt *stmt, unsigned index, zval *value);
static int php_mylite_add_current_row(mylite_stmt *stmt, zval *rows);
static void php_mylite_column_to_zval(mylite_stmt *stmt, unsigned column, zval *value);
static void php_mylite_result_object_from_rows(zval *return_value, zval *rows);

#define Z_MYLITE_CONNECTION_P(zval_ptr) php_mylite_connection_from_object(Z_OBJ_P((zval_ptr)))
#define Z_MYLITE_STATEMENT_P(zval_ptr) php_mylite_statement_from_object(Z_OBJ_P((zval_ptr)))
#define Z_MYLITE_RESULT_P(zval_ptr) php_mylite_result_from_object(Z_OBJ_P((zval_ptr)))

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_version, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_open, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(
    0,
    flags,
    IS_LONG,
    0,
    "MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE"
)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_connection_construct, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(
    0,
    flags,
    IS_LONG,
    0,
    "MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE"
)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_connection_close, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_connection_exec, 0, 1, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, sql, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_connection_query, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, sql, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_connection_prepare, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, sql, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_connection_error_code, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_connection_error_string, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_statement_bind_value, 0, 2, _IS_BOOL, 0)
ZEND_ARG_TYPE_INFO(0, index, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_statement_execute, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_statement_fetch_assoc, 0, 0, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_result_fetch_assoc, 0, 0, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_mylite_result_fetch_all, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

// NOLINTBEGIN(readability-function-cognitive-complexity)
PHP_FUNCTION(mylite_version) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(mylite_version());
}

PHP_FUNCTION(mylite_open) {
    char *path = NULL;
    size_t path_len = 0;
    zend_long flags = MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(path, path_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();

    (void)path_len;
    php_mylite_open_into_object(return_value, path, flags);
}

PHP_METHOD(MyLite_Connection, __construct) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);
    char *path = NULL;
    size_t path_len = 0;
    zend_long flags = MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(path, path_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();

    if (connection->db != NULL) {
        zend_throw_exception(
            php_mylite_exception_ce,
            "MyLite connection is already open",
            MYLITE_MISUSE
        );
        RETURN_THROWS();
    }

    (void)path_len;
    if (!php_mylite_validate_open_flags(flags)) {
        RETURN_THROWS();
    }

    mylite_db *db = NULL;
    const int result = mylite_open(path, &db, (unsigned)flags, NULL);
    if (result != MYLITE_OK) {
        php_mylite_throw_result(result, "could not open MyLite database");
        RETURN_THROWS();
    }
    connection->db = db;
}

PHP_METHOD(MyLite_Connection, close) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    if (connection->db == NULL) {
        RETURN_TRUE;
    }

    mylite_db *db = connection->db;
    const int result = mylite_close(db);
    if (result != MYLITE_OK) {
        php_mylite_throw_db(db, result, "could not close MyLite database");
        RETURN_THROWS();
    }
    connection->db = NULL;
    RETURN_TRUE;
}

PHP_METHOD(MyLite_Connection, exec) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);
    char *sql = NULL;
    size_t sql_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(sql, sql_len)
    ZEND_PARSE_PARAMETERS_END();

    mylite_db *db = php_mylite_require_db(connection);
    if (db == NULL) {
        RETURN_THROWS();
    }

    (void)sql_len;
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result != MYLITE_OK) {
        const char *fallback = errmsg != NULL ? errmsg : "MyLite SQL execution failed";
        php_mylite_throw_db(db, result, fallback);
        mylite_free(errmsg);
        RETURN_THROWS();
    }
    mylite_free(errmsg);
    RETURN_LONG((zend_long)mylite_changes(db));
}

PHP_METHOD(MyLite_Connection, query) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);
    char *sql = NULL;
    size_t sql_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(sql, sql_len)
    ZEND_PARSE_PARAMETERS_END();

    mylite_db *db = php_mylite_require_db(connection);
    if (db == NULL) {
        RETURN_THROWS();
    }

    mylite_stmt *stmt = NULL;
    const int prepare_result = mylite_prepare(db, sql, sql_len, &stmt, NULL);
    if (prepare_result != MYLITE_OK) {
        php_mylite_throw_db(db, prepare_result, "could not prepare MyLite query");
        RETURN_THROWS();
    }

    zval rows;
    array_init(&rows);
    int step_result = mylite_step(stmt);
    while (step_result == MYLITE_ROW) {
        if (php_mylite_add_current_row(stmt, &rows) != SUCCESS) {
            mylite_finalize(stmt);
            zval_ptr_dtor(&rows);
            zend_throw_exception(
                php_mylite_exception_ce,
                "could not allocate MyLite result row",
                MYLITE_NOMEM
            );
            RETURN_THROWS();
        }
        step_result = mylite_step(stmt);
    }

    const unsigned column_count = mylite_column_count(stmt);
    const int finalize_result = mylite_finalize(stmt);
    if (step_result != MYLITE_DONE) {
        zval_ptr_dtor(&rows);
        php_mylite_throw_db(db, step_result, "MyLite query failed");
        RETURN_THROWS();
    }
    if (finalize_result != MYLITE_OK) {
        zval_ptr_dtor(&rows);
        php_mylite_throw_db(db, finalize_result, "could not finalize MyLite query");
        RETURN_THROWS();
    }

    if (column_count == 0U) {
        zval_ptr_dtor(&rows);
        RETURN_TRUE;
    }

    php_mylite_result_object_from_rows(return_value, &rows);
}

PHP_METHOD(MyLite_Connection, prepare) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);
    char *sql = NULL;
    size_t sql_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(sql, sql_len)
    ZEND_PARSE_PARAMETERS_END();

    mylite_db *db = php_mylite_require_db(connection);
    if (db == NULL) {
        RETURN_THROWS();
    }

    mylite_stmt *stmt = NULL;
    const int result = mylite_prepare(db, sql, sql_len, &stmt, NULL);
    if (result != MYLITE_OK) {
        php_mylite_throw_db(db, result, "could not prepare MyLite statement");
        RETURN_THROWS();
    }

    object_init_ex(return_value, php_mylite_statement_ce);
    php_mylite_statement *statement = Z_MYLITE_STATEMENT_P(return_value);
    statement->stmt = stmt;
    statement->connection_object = Z_OBJ_P(ZEND_THIS);
    GC_ADDREF(statement->connection_object);
}

PHP_METHOD(MyLite_Connection, errorCode) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_LONG(connection->db != NULL ? mylite_errcode(connection->db) : MYLITE_MISUSE);
}

PHP_METHOD(MyLite_Connection, sqlState) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_STRING(connection->db != NULL ? mylite_sqlstate(connection->db) : "HY000");
}

PHP_METHOD(MyLite_Connection, errorMessage) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_STRING(
        connection->db != NULL ? mylite_errmsg(connection->db) : "MyLite connection is closed"
    );
}

PHP_METHOD(MyLite_Connection, mariadbErrno) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_LONG(connection->db != NULL ? (zend_long)mylite_mariadb_errno(connection->db) : 0);
}

PHP_METHOD(MyLite_Connection, changes) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_LONG(connection->db != NULL ? (zend_long)mylite_changes(connection->db) : 0);
}

PHP_METHOD(MyLite_Connection, lastInsertId) {
    php_mylite_connection *connection = Z_MYLITE_CONNECTION_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_STR(zend_u64_to_str(connection->db != NULL ? mylite_last_insert_id(connection->db) : 0));
}

PHP_METHOD(MyLite_Statement, bindValue) {
    php_mylite_statement *statement = Z_MYLITE_STATEMENT_P(ZEND_THIS);
    zend_long index = 0;
    zval *value = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_LONG(index)
    Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    if (statement->stmt == NULL || index <= 0 || index > UINT32_MAX) {
        zend_throw_exception(
            php_mylite_exception_ce,
            "invalid MyLite statement binding",
            MYLITE_MISUSE
        );
        RETURN_THROWS();
    }

    const int result = php_mylite_bind_zval(statement->stmt, (unsigned)index, value);
    if (result != MYLITE_OK) {
        php_mylite_throw_result(result, "could not bind MyLite statement value");
        RETURN_THROWS();
    }
    RETURN_TRUE;
}

PHP_METHOD(MyLite_Statement, execute) {
    php_mylite_statement *statement = Z_MYLITE_STATEMENT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    if (statement->stmt == NULL) {
        zend_throw_exception(php_mylite_exception_ce, "MyLite statement is closed", MYLITE_MISUSE);
        RETURN_THROWS();
    }

    if (statement->step_result == MYLITE_ROW || statement->step_result == MYLITE_DONE) {
        const int reset_result = mylite_reset(statement->stmt);
        if (reset_result != MYLITE_OK) {
            php_mylite_throw_result(reset_result, "could not reset MyLite statement");
            RETURN_THROWS();
        }
    }

    statement->step_result = mylite_step(statement->stmt);
    if (statement->step_result != MYLITE_ROW && statement->step_result != MYLITE_DONE) {
        php_mylite_throw_result(statement->step_result, "could not execute MyLite statement");
        RETURN_THROWS();
    }
    RETURN_TRUE;
}

PHP_METHOD(MyLite_Statement, fetchAssociative) {
    php_mylite_statement *statement = Z_MYLITE_STATEMENT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    if (statement->stmt == NULL || statement->step_result != MYLITE_ROW) {
        RETURN_NULL();
    }

    zval row;
    array_init(&row);
    const unsigned column_count = mylite_column_count(statement->stmt);
    for (unsigned column = 0; column < column_count; ++column) {
        zval value;
        const char *name = mylite_column_name(statement->stmt, column);
        php_mylite_column_to_zval(statement->stmt, column, &value);
        add_assoc_zval_ex(&row, name != NULL ? name : "", name != NULL ? strlen(name) : 0, &value);
    }

    statement->step_result = mylite_step(statement->stmt);
    RETURN_COPY_VALUE(&row);
}

PHP_METHOD(MyLite_Result, fetchAssociative) {
    php_mylite_result *result = Z_MYLITE_RESULT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        RETURN_NULL();
    }
    ++result->position;
    RETURN_COPY(row);
}

PHP_METHOD(MyLite_Result, fetchAll) {
    php_mylite_result *result = Z_MYLITE_RESULT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_COPY(&result->rows);
}

// NOLINTEND(readability-function-cognitive-complexity)

static const zend_function_entry php_mylite_functions[] = {
    PHP_FE(mylite_version, arginfo_mylite_version) PHP_FE(mylite_open, arginfo_mylite_open)
        PHP_FE_END
};

static const zend_function_entry php_mylite_connection_methods[] = {
    PHP_ME(MyLite_Connection, __construct, arginfo_mylite_connection_construct, ZEND_ACC_PUBLIC)
        PHP_ME(MyLite_Connection, close, arginfo_mylite_connection_close, ZEND_ACC_PUBLIC) PHP_ME(
            MyLite_Connection,
            exec,
            arginfo_mylite_connection_exec,
            ZEND_ACC_PUBLIC
        ) PHP_ME(MyLite_Connection, query, arginfo_mylite_connection_query, ZEND_ACC_PUBLIC)
            PHP_ME(MyLite_Connection, prepare, arginfo_mylite_connection_prepare, ZEND_ACC_PUBLIC)
                PHP_ME(
                    MyLite_Connection,
                    errorCode,
                    arginfo_mylite_connection_error_code,
                    ZEND_ACC_PUBLIC
                )
                    PHP_ME(
                        MyLite_Connection,
                        sqlState,
                        arginfo_mylite_connection_error_string,
                        ZEND_ACC_PUBLIC
                    )
                        PHP_ME(
                            MyLite_Connection,
                            errorMessage,
                            arginfo_mylite_connection_error_string,
                            ZEND_ACC_PUBLIC
                        )
                            PHP_ME(
                                MyLite_Connection,
                                mariadbErrno,
                                arginfo_mylite_connection_error_code,
                                ZEND_ACC_PUBLIC
                            )
                                PHP_ME(
                                    MyLite_Connection,
                                    changes,
                                    arginfo_mylite_connection_error_code,
                                    ZEND_ACC_PUBLIC
                                )
                                    PHP_ME(
                                        MyLite_Connection,
                                        lastInsertId,
                                        arginfo_mylite_connection_error_string,
                                        ZEND_ACC_PUBLIC
                                    ) PHP_FE_END
};

static const zend_function_entry php_mylite_statement_methods[] = {
    PHP_ME(MyLite_Statement, bindValue, arginfo_mylite_statement_bind_value, ZEND_ACC_PUBLIC)
        PHP_ME(MyLite_Statement, execute, arginfo_mylite_statement_execute, ZEND_ACC_PUBLIC) PHP_ME(
            MyLite_Statement,
            fetchAssociative,
            arginfo_mylite_statement_fetch_assoc,
            ZEND_ACC_PUBLIC
        ) PHP_FE_END
};

static const zend_function_entry php_mylite_result_methods[] = {
    PHP_ME(MyLite_Result, fetchAssociative, arginfo_mylite_result_fetch_assoc, ZEND_ACC_PUBLIC)
        PHP_ME(MyLite_Result, fetchAll, arginfo_mylite_result_fetch_all, ZEND_ACC_PUBLIC) PHP_FE_END
};

PHP_MINIT_FUNCTION(mylite) {
    (void)type;
    zend_class_entry class_entry;

    INIT_NS_CLASS_ENTRY(class_entry, "MyLite", "Exception", NULL);
    php_mylite_exception_ce = zend_register_internal_class_ex(&class_entry, zend_ce_exception);

    INIT_NS_CLASS_ENTRY(class_entry, "MyLite", "Connection", php_mylite_connection_methods);
    php_mylite_connection_ce = zend_register_internal_class(&class_entry);
    php_mylite_connection_ce->create_object = php_mylite_connection_create;
    memcpy(&php_mylite_connection_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    php_mylite_connection_handlers.offset = XtOffsetOf(php_mylite_connection, std);
    php_mylite_connection_handlers.free_obj = php_mylite_connection_free;
    php_mylite_connection_handlers.clone_obj = NULL;

    INIT_NS_CLASS_ENTRY(class_entry, "MyLite", "Statement", php_mylite_statement_methods);
    php_mylite_statement_ce = zend_register_internal_class(&class_entry);
    php_mylite_statement_ce->create_object = php_mylite_statement_create;
    memcpy(&php_mylite_statement_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    php_mylite_statement_handlers.offset = XtOffsetOf(php_mylite_statement, std);
    php_mylite_statement_handlers.free_obj = php_mylite_statement_free;
    php_mylite_statement_handlers.clone_obj = NULL;

    INIT_NS_CLASS_ENTRY(class_entry, "MyLite", "Result", php_mylite_result_methods);
    php_mylite_result_ce = zend_register_internal_class(&class_entry);
    php_mylite_result_ce->create_object = php_mylite_result_create;
    memcpy(&php_mylite_result_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    php_mylite_result_handlers.offset = XtOffsetOf(php_mylite_result, std);
    php_mylite_result_handlers.free_obj = php_mylite_result_free;
    php_mylite_result_handlers.clone_obj = NULL;

    php_mylite_register_constants(module_number);

    return SUCCESS;
}

PHP_MINFO_FUNCTION(mylite) {
    (void)zend_module;
    php_info_print_table_start();
    php_info_print_table_row(2, "MyLite support", "enabled");
    php_info_print_table_row(2, "MyLite extension version", PHP_MYLITE_EXT_VERSION);
    php_info_print_table_row(2, "libmylite version", mylite_version());
    php_info_print_table_end();
}

static zend_module_entry mylite_module_entry = {
    STANDARD_MODULE_HEADER,
    "mylite",
    php_mylite_functions,
    PHP_MINIT(mylite),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(mylite),
    PHP_MYLITE_EXT_VERSION,
    STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(mylite)

static zend_object *php_mylite_connection_create(zend_class_entry *class_entry) {
    php_mylite_connection *connection =
        zend_object_alloc(sizeof(php_mylite_connection), class_entry);
    connection->db = NULL;
    zend_object_std_init(&connection->std, class_entry);
    object_properties_init(&connection->std, class_entry);
    connection->std.handlers = &php_mylite_connection_handlers;
    return &connection->std;
}

static void php_mylite_connection_free(zend_object *object) {
    php_mylite_connection *connection = php_mylite_connection_from_object(object);
    if (connection->db != NULL) {
        (void)mylite_close(connection->db);
        connection->db = NULL;
    }
    zend_object_std_dtor(&connection->std);
}

static zend_object *php_mylite_statement_create(zend_class_entry *class_entry) {
    php_mylite_statement *statement = zend_object_alloc(sizeof(php_mylite_statement), class_entry);
    statement->stmt = NULL;
    statement->connection_object = NULL;
    statement->step_result = MYLITE_DONE;
    zend_object_std_init(&statement->std, class_entry);
    object_properties_init(&statement->std, class_entry);
    statement->std.handlers = &php_mylite_statement_handlers;
    return &statement->std;
}

static void php_mylite_statement_free(zend_object *object) {
    php_mylite_statement *statement = php_mylite_statement_from_object(object);
    if (statement->stmt != NULL) {
        (void)mylite_finalize(statement->stmt);
        statement->stmt = NULL;
    }
    if (statement->connection_object != NULL) {
        OBJ_RELEASE(statement->connection_object);
        statement->connection_object = NULL;
    }
    zend_object_std_dtor(&statement->std);
}

static zend_object *php_mylite_result_create(zend_class_entry *class_entry) {
    php_mylite_result *result = zend_object_alloc(sizeof(php_mylite_result), class_entry);
    array_init(&result->rows);
    result->position = 0;
    zend_object_std_init(&result->std, class_entry);
    object_properties_init(&result->std, class_entry);
    result->std.handlers = &php_mylite_result_handlers;
    return &result->std;
}

static void php_mylite_result_free(zend_object *object) {
    php_mylite_result *result = php_mylite_result_from_object(object);
    zval_ptr_dtor(&result->rows);
    zend_object_std_dtor(&result->std);
}

static php_mylite_connection *php_mylite_connection_from_object(zend_object *object) {
    return (php_mylite_connection *)((char *)object - XtOffsetOf(php_mylite_connection, std));
}

static php_mylite_statement *php_mylite_statement_from_object(zend_object *object) {
    return (php_mylite_statement *)((char *)object - XtOffsetOf(php_mylite_statement, std));
}

static php_mylite_result *php_mylite_result_from_object(zend_object *object) {
    return (php_mylite_result *)((char *)object - XtOffsetOf(php_mylite_result, std));
}

static mylite_db *php_mylite_require_db(php_mylite_connection *connection) {
    if (connection->db == NULL) {
        zend_throw_exception(php_mylite_exception_ce, "MyLite connection is closed", MYLITE_MISUSE);
        return NULL;
    }
    return connection->db;
}

static void php_mylite_open_into_object(zval *return_value, const char *path, zend_long flags) {
    if (!php_mylite_validate_open_flags(flags)) {
        RETURN_THROWS();
    }

    mylite_db *db = NULL;
    const int result = mylite_open(path, &db, (unsigned)flags, NULL);
    if (result != MYLITE_OK) {
        php_mylite_throw_result(result, "could not open MyLite database");
        RETURN_THROWS();
    }

    object_init_ex(return_value, php_mylite_connection_ce);
    Z_MYLITE_CONNECTION_P(return_value)->db = db;
}

static bool php_mylite_validate_open_flags(zend_long flags) {
    if (flags < 0 || flags > UINT32_MAX) {
        zend_throw_exception(php_mylite_exception_ce, "invalid MyLite open flags", MYLITE_MISUSE);
        return false;
    }
    return true;
}

static void php_mylite_throw_db(mylite_db *db, int result, const char *fallback) {
    const char *message = fallback;
    if (db != NULL && mylite_errmsg(db) != NULL) {
        message = mylite_errmsg(db);
    }
    zend_throw_exception(php_mylite_exception_ce, message, result);
}

static void php_mylite_throw_result(int result, const char *fallback) {
    zend_throw_exception(php_mylite_exception_ce, fallback, result);
}

static void php_mylite_register_constants(int module_number) {
    REGISTER_LONG_CONSTANT("MYLITE_OK", MYLITE_OK, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_ERROR", MYLITE_ERROR, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_BUSY", MYLITE_BUSY, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_NOMEM", MYLITE_NOMEM, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_READONLY", MYLITE_READONLY, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_IOERR", MYLITE_IOERR, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_CORRUPT", MYLITE_CORRUPT, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_NOTFOUND", MYLITE_NOTFOUND, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_FULL", MYLITE_FULL, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_CONSTRAINT", MYLITE_CONSTRAINT, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_MISUSE", MYLITE_MISUSE, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_ROW", MYLITE_ROW, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_DONE", MYLITE_DONE, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_OPEN_READONLY", MYLITE_OPEN_READONLY, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_OPEN_READWRITE", MYLITE_OPEN_READWRITE, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_OPEN_CREATE", MYLITE_OPEN_CREATE, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_OPEN_EXCLUSIVE", MYLITE_OPEN_EXCLUSIVE, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_OPEN_URI", MYLITE_OPEN_URI, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_TYPE_NULL", MYLITE_TYPE_NULL, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_TYPE_INT64", MYLITE_TYPE_INT64, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_TYPE_UINT64", MYLITE_TYPE_UINT64, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_TYPE_DOUBLE", MYLITE_TYPE_DOUBLE, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_TYPE_TEXT", MYLITE_TYPE_TEXT, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYLITE_TYPE_BLOB", MYLITE_TYPE_BLOB, CONST_PERSISTENT);
}

static int php_mylite_bind_zval(mylite_stmt *stmt, unsigned index, zval *value) {
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

static int php_mylite_add_current_row(mylite_stmt *stmt, zval *rows) {
    zval row;
    array_init(&row);
    const unsigned column_count = mylite_column_count(stmt);
    for (unsigned column = 0; column < column_count; ++column) {
        zval value;
        const char *name = mylite_column_name(stmt, column);
        php_mylite_column_to_zval(stmt, column, &value);
        add_assoc_zval_ex(&row, name != NULL ? name : "", name != NULL ? strlen(name) : 0, &value);
    }
    add_next_index_zval(rows, &row);
    return SUCCESS;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void php_mylite_column_to_zval(mylite_stmt *stmt, unsigned column, zval *value) {
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
            zend_string *string_value = zend_u64_to_str(uint_value);
            ZVAL_STR(value, string_value);
        }
        return;
    }
    case MYLITE_TYPE_DOUBLE:
        ZVAL_DOUBLE(value, mylite_column_double(stmt, column));
        return;
    case MYLITE_TYPE_TEXT:
    case MYLITE_TYPE_BLOB: {
        const char *text = mylite_column_text(stmt, column);
        const size_t byte_count = mylite_column_bytes(stmt, column);
        if (text == NULL) {
            ZVAL_NULL(value);
        } else {
            ZVAL_STRINGL(value, text, byte_count);
        }
        return;
    }
    }
    ZVAL_NULL(value);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void php_mylite_result_object_from_rows(zval *return_value, zval *rows) {
    object_init_ex(return_value, php_mylite_result_ce);
    php_mylite_result *result = Z_MYLITE_RESULT_P(return_value);
    zval_ptr_dtor(&result->rows);
    ZVAL_COPY_VALUE(&result->rows, rows);
    result->position = 0;
}

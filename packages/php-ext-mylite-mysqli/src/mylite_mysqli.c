// clang-format off
#include "php.h"
#include "php_ini.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_interfaces.h"
#include "Zend/zend_smart_str.h"
#include "ext/standard/info.h"
// clang-format on

#include <mylite/mylite.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PHP_MYLITE_MYSQLI_VERSION "0.1.0"
#define MYLITE_MYSQLI_SERVER_INFO "11.8.6-MariaDB-embedded"
#define MYLITE_MYSQLI_SERVER_VERSION 110806

#define MYLITE_MYSQLI_STORE_RESULT 0
#define MYLITE_MYSQLI_USE_RESULT 1
#define MYLITE_MYSQLI_ASSOC 1
#define MYLITE_MYSQLI_NUM 2
#define MYLITE_MYSQLI_BOTH 3

#define MYLITE_MYSQLI_REPORT_ERROR 1
#define MYLITE_MYSQLI_REPORT_STRICT 2
#define MYLITE_MYSQLI_REPORT_OFF 0

#define MYLITE_MYSQLI_OPT_INT_AND_FLOAT_NATIVE 201

typedef struct {
    mylite_db *db;
    zend_string *filename;
    zend_string *schema;
    zend_string *charset;
    zend_long errno_value;
    zend_string *error;
    zend_string *sqlstate;
    zend_long affected_rows;
    zend_ulong insert_id;
    zend_long field_count;
    zend_long warning_count;
    bool connected;
    bool int_and_float_native;
    zval pending_result;
    zend_object std;
} mylite_mysqli_link;

typedef struct {
    zval rows;
    zval fields;
    zval lengths;
    zend_long current_row;
    zend_long current_field;
    zend_long type;
    zend_object std;
} mylite_mysqli_result;

typedef struct {
    zval link;
    mylite_stmt *stmt;
    zend_string *query;
    zval bound_params;
    zval bound_results;
    zval result;
    zend_string *types;
    zend_long errno_value;
    zend_string *error;
    zend_string *sqlstate;
    zend_long affected_rows;
    zend_ulong insert_id;
    zend_long num_rows;
    zend_long param_count;
    zend_long field_count;
    bool executed;
    zend_object std;
} mylite_mysqli_stmt;

typedef struct {
    zval link;
    unsigned index;
    zend_object std;
} mylite_mysqli_warning;

ZEND_BEGIN_MODULE_GLOBALS(mylite_mysqli)
zend_long report_mode;
zend_long connect_errno;
zend_string *connect_error;
ZEND_END_MODULE_GLOBALS(mylite_mysqli)

ZEND_DECLARE_MODULE_GLOBALS(mylite_mysqli)

#define MYLITE_MYSQLI_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(mylite_mysqli, v)

static zend_class_entry *mylite_mysqli_link_ce;
static zend_class_entry *mylite_mysqli_result_ce;
static zend_class_entry *mylite_mysqli_stmt_ce;
static zend_class_entry *mylite_mysqli_driver_ce;
static zend_class_entry *mylite_mysqli_warning_ce;
static zend_class_entry *mylite_mysqli_exception_ce;

static zend_object_handlers mylite_mysqli_link_handlers;
static zend_object_handlers mylite_mysqli_result_handlers;
static zend_object_handlers mylite_mysqli_stmt_handlers;
static zend_object_handlers mylite_mysqli_warning_handlers;

static inline mylite_mysqli_link *mylite_mysqli_link_from_obj(zend_object *obj) {
    return (mylite_mysqli_link *)((char *)obj - XtOffsetOf(mylite_mysqli_link, std));
}

static inline mylite_mysqli_result *mylite_mysqli_result_from_obj(zend_object *obj) {
    return (mylite_mysqli_result *)((char *)obj - XtOffsetOf(mylite_mysqli_result, std));
}

static inline mylite_mysqli_stmt *mylite_mysqli_stmt_from_obj(zend_object *obj) {
    return (mylite_mysqli_stmt *)((char *)obj - XtOffsetOf(mylite_mysqli_stmt, std));
}

static inline mylite_mysqli_warning *mylite_mysqli_warning_from_obj(zend_object *obj) {
    return (mylite_mysqli_warning *)((char *)obj - XtOffsetOf(mylite_mysqli_warning, std));
}

#define Z_MYSQLI_LINK_P(zv) mylite_mysqli_link_from_obj(Z_OBJ_P(zv))
#define Z_MYSQLI_RESULT_P(zv) mylite_mysqli_result_from_obj(Z_OBJ_P(zv))
#define Z_MYSQLI_STMT_P(zv) mylite_mysqli_stmt_from_obj(Z_OBJ_P(zv))
#define Z_MYSQLI_WARNING_P(zv) mylite_mysqli_warning_from_obj(Z_OBJ_P(zv))

PHP_FUNCTION(mysqli_affected_rows);
PHP_FUNCTION(mysqli_autocommit);
PHP_FUNCTION(mysqli_begin_transaction);
PHP_FUNCTION(mysqli_change_user);
PHP_FUNCTION(mysqli_character_set_name);
PHP_FUNCTION(mysqli_close);
PHP_FUNCTION(mysqli_commit);
PHP_FUNCTION(mysqli_connect);
PHP_FUNCTION(mysqli_connect_errno);
PHP_FUNCTION(mysqli_connect_error);
PHP_FUNCTION(mysqli_data_seek);
PHP_FUNCTION(mysqli_dump_debug_info);
PHP_FUNCTION(mysqli_debug);
PHP_FUNCTION(mysqli_errno);
PHP_FUNCTION(mysqli_error);
PHP_FUNCTION(mysqli_error_list);
PHP_FUNCTION(mysqli_stmt_execute);
PHP_FUNCTION(mysqli_execute_query);
PHP_FUNCTION(mysqli_fetch_field);
PHP_FUNCTION(mysqli_fetch_fields);
PHP_FUNCTION(mysqli_fetch_field_direct);
PHP_FUNCTION(mysqli_fetch_lengths);
PHP_FUNCTION(mysqli_fetch_all);
PHP_FUNCTION(mysqli_fetch_array);
PHP_FUNCTION(mysqli_fetch_assoc);
PHP_FUNCTION(mysqli_fetch_object);
PHP_FUNCTION(mysqli_fetch_row);
PHP_FUNCTION(mysqli_fetch_column);
PHP_FUNCTION(mysqli_field_count);
PHP_FUNCTION(mysqli_field_seek);
PHP_FUNCTION(mysqli_field_tell);
PHP_FUNCTION(mysqli_free_result);
PHP_FUNCTION(mysqli_get_connection_stats);
PHP_FUNCTION(mysqli_get_client_stats);
PHP_FUNCTION(mysqli_get_charset);
PHP_FUNCTION(mysqli_get_client_info);
PHP_FUNCTION(mysqli_get_client_version);
PHP_FUNCTION(mysqli_get_links_stats);
PHP_FUNCTION(mysqli_get_host_info);
PHP_FUNCTION(mysqli_get_proto_info);
PHP_FUNCTION(mysqli_get_server_info);
PHP_FUNCTION(mysqli_get_server_version);
PHP_FUNCTION(mysqli_get_warnings);
PHP_FUNCTION(mysqli_init);
PHP_FUNCTION(mysqli_info);
PHP_FUNCTION(mysqli_insert_id);
PHP_FUNCTION(mysqli_kill);
PHP_FUNCTION(mysqli_more_results);
PHP_FUNCTION(mysqli_multi_query);
PHP_FUNCTION(mysqli_next_result);
PHP_FUNCTION(mysqli_num_fields);
PHP_FUNCTION(mysqli_num_rows);
PHP_FUNCTION(mysqli_options);
PHP_FUNCTION(mysqli_ping);
PHP_FUNCTION(mysqli_poll);
PHP_FUNCTION(mysqli_prepare);
PHP_FUNCTION(mysqli_report);
PHP_FUNCTION(mysqli_query);
PHP_FUNCTION(mysqli_real_connect);
PHP_FUNCTION(mysqli_real_escape_string);
PHP_FUNCTION(mysqli_real_query);
PHP_FUNCTION(mysqli_reap_async_query);
PHP_FUNCTION(mysqli_release_savepoint);
PHP_FUNCTION(mysqli_rollback);
PHP_FUNCTION(mysqli_savepoint);
PHP_FUNCTION(mysqli_select_db);
PHP_FUNCTION(mysqli_set_charset);
PHP_FUNCTION(mysqli_stmt_affected_rows);
PHP_FUNCTION(mysqli_stmt_attr_get);
PHP_FUNCTION(mysqli_stmt_attr_set);
PHP_FUNCTION(mysqli_stmt_bind_param);
PHP_FUNCTION(mysqli_stmt_bind_result);
PHP_FUNCTION(mysqli_stmt_close);
PHP_FUNCTION(mysqli_stmt_data_seek);
PHP_FUNCTION(mysqli_stmt_errno);
PHP_FUNCTION(mysqli_stmt_error);
PHP_FUNCTION(mysqli_stmt_error_list);
PHP_FUNCTION(mysqli_stmt_fetch);
PHP_FUNCTION(mysqli_stmt_field_count);
PHP_FUNCTION(mysqli_stmt_free_result);
PHP_FUNCTION(mysqli_stmt_get_result);
PHP_FUNCTION(mysqli_stmt_get_warnings);
PHP_FUNCTION(mysqli_stmt_init);
PHP_FUNCTION(mysqli_stmt_insert_id);
PHP_FUNCTION(mysqli_stmt_more_results);
PHP_FUNCTION(mysqli_stmt_next_result);
PHP_FUNCTION(mysqli_stmt_num_rows);
PHP_FUNCTION(mysqli_stmt_param_count);
PHP_FUNCTION(mysqli_stmt_prepare);
PHP_FUNCTION(mysqli_stmt_reset);
PHP_FUNCTION(mysqli_stmt_result_metadata);
PHP_FUNCTION(mysqli_stmt_send_long_data);
PHP_FUNCTION(mysqli_stmt_store_result);
PHP_FUNCTION(mysqli_stmt_sqlstate);
PHP_FUNCTION(mysqli_sqlstate);
PHP_FUNCTION(mysqli_ssl_set);
PHP_FUNCTION(mysqli_stat);
PHP_FUNCTION(mysqli_store_result);
PHP_FUNCTION(mysqli_thread_id);
PHP_FUNCTION(mysqli_thread_safe);
PHP_FUNCTION(mysqli_use_result);
PHP_FUNCTION(mysqli_warning_count);
PHP_FUNCTION(mysqli_refresh);

PHP_METHOD(mysqli, __construct);
PHP_METHOD(mysqli, autocommit);
PHP_METHOD(mysqli, begin_transaction);
PHP_METHOD(mysqli, change_user);
PHP_METHOD(mysqli, character_set_name);
PHP_METHOD(mysqli, close);
PHP_METHOD(mysqli, commit);
PHP_METHOD(mysqli, connect);
PHP_METHOD(mysqli, dump_debug_info);
PHP_METHOD(mysqli, debug);
PHP_METHOD(mysqli, get_charset);
PHP_METHOD(mysqli, execute_query);
PHP_METHOD(mysqli, get_client_info);
PHP_METHOD(mysqli, get_connection_stats);
PHP_METHOD(mysqli, get_server_info);
PHP_METHOD(mysqli, get_warnings);
PHP_METHOD(mysqli, init);
PHP_METHOD(mysqli, kill);
PHP_METHOD(mysqli, multi_query);
PHP_METHOD(mysqli, more_results);
PHP_METHOD(mysqli, next_result);
PHP_METHOD(mysqli, ping);
PHP_METHOD(mysqli, poll);
PHP_METHOD(mysqli, prepare);
PHP_METHOD(mysqli, query);
PHP_METHOD(mysqli, real_connect);
PHP_METHOD(mysqli, real_escape_string);
PHP_METHOD(mysqli, reap_async_query);
PHP_METHOD(mysqli, real_query);
PHP_METHOD(mysqli, release_savepoint);
PHP_METHOD(mysqli, rollback);
PHP_METHOD(mysqli, savepoint);
PHP_METHOD(mysqli, select_db);
PHP_METHOD(mysqli, set_charset);
PHP_METHOD(mysqli, options);
PHP_METHOD(mysqli, set_opt);
PHP_METHOD(mysqli, ssl_set);
PHP_METHOD(mysqli, stat);
PHP_METHOD(mysqli, stmt_init);
PHP_METHOD(mysqli, store_result);
PHP_METHOD(mysqli, thread_safe);
PHP_METHOD(mysqli, use_result);
PHP_METHOD(mysqli, refresh);

PHP_METHOD(mysqli_result, __construct);
PHP_METHOD(mysqli_result, close);
PHP_METHOD(mysqli_result, free);
PHP_METHOD(mysqli_result, data_seek);
PHP_METHOD(mysqli_result, fetch_field);
PHP_METHOD(mysqli_result, fetch_fields);
PHP_METHOD(mysqli_result, fetch_field_direct);
PHP_METHOD(mysqli_result, fetch_all);
PHP_METHOD(mysqli_result, fetch_array);
PHP_METHOD(mysqli_result, fetch_assoc);
PHP_METHOD(mysqli_result, fetch_object);
PHP_METHOD(mysqli_result, fetch_row);
PHP_METHOD(mysqli_result, fetch_column);
PHP_METHOD(mysqli_result, field_seek);
PHP_METHOD(mysqli_result, free_result);
PHP_METHOD(mysqli_result, getIterator);

PHP_METHOD(mysqli_stmt, __construct);
PHP_METHOD(mysqli_stmt, attr_get);
PHP_METHOD(mysqli_stmt, attr_set);
PHP_METHOD(mysqli_stmt, bind_param);
PHP_METHOD(mysqli_stmt, bind_result);
PHP_METHOD(mysqli_stmt, close);
PHP_METHOD(mysqli_stmt, data_seek);
PHP_METHOD(mysqli_stmt, execute);
PHP_METHOD(mysqli_stmt, fetch);
PHP_METHOD(mysqli_stmt, get_warnings);
PHP_METHOD(mysqli_stmt, result_metadata);
PHP_METHOD(mysqli_stmt, more_results);
PHP_METHOD(mysqli_stmt, next_result);
PHP_METHOD(mysqli_stmt, num_rows);
PHP_METHOD(mysqli_stmt, send_long_data);
PHP_METHOD(mysqli_stmt, free_result);
PHP_METHOD(mysqli_stmt, reset);
PHP_METHOD(mysqli_stmt, prepare);
PHP_METHOD(mysqli_stmt, store_result);
PHP_METHOD(mysqli_stmt, get_result);

PHP_METHOD(mysqli_warning, __construct);
PHP_METHOD(mysqli_warning, next);
PHP_METHOD(mysqli_sql_exception, getSqlState);

static bool mylite_mysqli_connect_link(
    mylite_mysqli_link *link,
    const char *hostname,
    size_t hostname_len,
    const char *database,
    size_t database_len,
    const char *socket_name,
    size_t socket_name_len
);
static bool mylite_mysqli_query_link(
    mylite_mysqli_link *link,
    const char *query,
    size_t query_len,
    zend_long result_mode,
    zval *return_value
);
static bool mylite_mysqli_real_query_link(
    mylite_mysqli_link *link,
    const char *query,
    size_t query_len
);
static bool mylite_mysqli_prepare_stmt(
    mylite_mysqli_stmt *stmt,
    zval *link_zv,
    const char *query,
    size_t query_len
);
static bool mylite_mysqli_execute_stmt(mylite_mysqli_stmt *stmt, zval *params);
static void mylite_mysqli_result_fetch_array(
    mylite_mysqli_result *result,
    zend_long mode,
    zval *return_value
);
static void mylite_mysqli_result_fetch_object(
    mylite_mysqli_result *result,
    const char *class_name,
    size_t class_name_len,
    zval *constructor_args,
    zval *return_value
);
static void mylite_mysqli_result_fetch_column(
    mylite_mysqli_result *result,
    zend_long column,
    zval *return_value
);
static bool mylite_mysqli_result_seek(mylite_mysqli_result *result, zend_long offset);
static bool mylite_mysqli_field_seek_result(mylite_mysqli_result *result, zend_long index);
static bool mylite_mysqli_update_warning(mylite_mysqli_warning *warning);
static zend_object *mylite_mysqli_link_create_object(zend_class_entry *class_type);
static zend_object *mylite_mysqli_result_create_object(zend_class_entry *class_type);
static zend_object *mylite_mysqli_stmt_create_object(zend_class_entry *class_type);
static zend_object *mylite_mysqli_warning_create_object(zend_class_entry *class_type);
static void mylite_mysqli_link_free_obj(zend_object *object);
static void mylite_mysqli_result_free_obj(zend_object *object);
static void mylite_mysqli_stmt_free_obj(zend_object *object);
static void mylite_mysqli_warning_free_obj(zend_object *object);
static PHP_MINIT_FUNCTION(mylite_mysqli);
static PHP_MSHUTDOWN_FUNCTION(mylite_mysqli);
static PHP_MINFO_FUNCTION(mylite_mysqli);
static void zm_globals_ctor_mylite_mysqli(zend_mylite_mysqli_globals *globals);
static void zm_globals_dtor_mylite_mysqli(zend_mylite_mysqli_globals *globals);

ZEND_BEGIN_ARG_INFO_EX(arginfo_none, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_mysqli_result_get_iterator, 0, 0, Traversable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_one, 0, 0, 1)
ZEND_ARG_INFO(0, arg)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_two, 0, 0, 2)
ZEND_ARG_INFO(0, arg1)
ZEND_ARG_INFO(0, arg2)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_three, 0, 0, 3)
ZEND_ARG_INFO(0, arg1)
ZEND_ARG_INFO(0, arg2)
ZEND_ARG_INFO(0, arg3)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_connect, 0, 0, 0)
ZEND_ARG_INFO(0, hostname)
ZEND_ARG_INFO(0, username)
ZEND_ARG_INFO(0, password)
ZEND_ARG_INFO(0, database)
ZEND_ARG_INFO(0, port)
ZEND_ARG_INFO(0, socket)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_real_connect, 0, 0, 0)
ZEND_ARG_INFO(0, hostname)
ZEND_ARG_INFO(0, username)
ZEND_ARG_INFO(0, password)
ZEND_ARG_INFO(0, database)
ZEND_ARG_INFO(0, port)
ZEND_ARG_INFO(0, socket)
ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mysqli_real_connect, 0, 0, 1)
ZEND_ARG_INFO(0, mysql)
ZEND_ARG_INFO(0, hostname)
ZEND_ARG_INFO(0, username)
ZEND_ARG_INFO(0, password)
ZEND_ARG_INFO(0, database)
ZEND_ARG_INFO(0, port)
ZEND_ARG_INFO(0, socket)
ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_stmt_bind_param, 0, 0, 2)
ZEND_ARG_INFO(0, statement)
ZEND_ARG_TYPE_INFO(0, types, IS_STRING, 0)
ZEND_ARG_VARIADIC_INFO(1, vars)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_stmt_method_bind_param, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, types, IS_STRING, 0)
ZEND_ARG_VARIADIC_INFO(1, vars)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_stmt_bind_result, 0, 0, 1)
ZEND_ARG_INFO(0, statement)
ZEND_ARG_VARIADIC_INFO(1, vars)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_stmt_method_bind_result, 0, 0, 0)
ZEND_ARG_VARIADIC_INFO(1, vars)
ZEND_END_ARG_INFO()

static const zend_function_entry mylite_mysqli_functions
    [] = {PHP_FE(mysqli_affected_rows, arginfo_one) PHP_FE(mysqli_autocommit, arginfo_two) PHP_FE(
        mysqli_begin_transaction,
        arginfo_one
    ) PHP_FE(mysqli_change_user, arginfo_three) PHP_FE(mysqli_character_set_name, arginfo_one)
              PHP_FE(mysqli_close, arginfo_one) PHP_FE(mysqli_commit, arginfo_one) PHP_FE(
                  mysqli_connect,
                  arginfo_connect
              ) PHP_FE(mysqli_connect_errno, arginfo_none)
                  PHP_FE(mysqli_connect_error, arginfo_none) PHP_FE(
                      mysqli_data_seek,
                      arginfo_two
                  ) PHP_FE(mysqli_dump_debug_info, arginfo_one) PHP_FE(mysqli_debug, arginfo_one)
                      PHP_FE(mysqli_errno, arginfo_one) PHP_FE(mysqli_error, arginfo_one) PHP_FE(
                          mysqli_error_list,
                          arginfo_one
                      ) PHP_FE(mysqli_stmt_execute, arginfo_one)
                          PHP_FALIAS(mysqli_execute, mysqli_stmt_execute, arginfo_one) PHP_FE(
                              mysqli_execute_query,
                              arginfo_two
                          ) PHP_FE(mysqli_fetch_field, arginfo_one) PHP_FE(mysqli_fetch_fields, arginfo_one)
                              PHP_FE(mysqli_fetch_field_direct, arginfo_two) PHP_FE(
                                  mysqli_fetch_lengths,
                                  arginfo_one
                              ) PHP_FE(mysqli_fetch_all, arginfo_one)
                                  PHP_FE(mysqli_fetch_array, arginfo_one) PHP_FE(
                                      mysqli_fetch_assoc,
                                      arginfo_one
                                  ) PHP_FE(mysqli_fetch_object, arginfo_one) PHP_FE(mysqli_fetch_row, arginfo_one)
                                      PHP_FE(mysqli_fetch_column, arginfo_one) PHP_FE(
                                          mysqli_field_count,
                                          arginfo_one
                                      )
                                          PHP_FE(mysqli_field_seek, arginfo_two) PHP_FE(mysqli_field_tell, arginfo_one) PHP_FE(mysqli_free_result, arginfo_one) PHP_FE(mysqli_get_connection_stats, arginfo_one) PHP_FE(mysqli_get_client_stats, arginfo_none) PHP_FE(mysqli_get_charset, arginfo_one) PHP_FE(mysqli_get_client_info, arginfo_none) PHP_FE(mysqli_get_client_version, arginfo_none) PHP_FE(mysqli_get_links_stats, arginfo_none) PHP_FE(mysqli_get_host_info, arginfo_one) PHP_FE(mysqli_get_proto_info, arginfo_one) PHP_FE(mysqli_get_server_info, arginfo_one) PHP_FE(mysqli_get_server_version, arginfo_one) PHP_FE(mysqli_get_warnings, arginfo_one) PHP_FE(mysqli_init, arginfo_none) PHP_FE(mysqli_info, arginfo_one) PHP_FE(mysqli_insert_id, arginfo_one) PHP_FE(mysqli_kill, arginfo_two) PHP_FE(mysqli_more_results, arginfo_one) PHP_FE(mysqli_multi_query, arginfo_two) PHP_FE(mysqli_next_result, arginfo_one) PHP_FE(mysqli_num_fields, arginfo_one) PHP_FE(mysqli_num_rows, arginfo_one) PHP_FE(mysqli_options, arginfo_three) PHP_FALIAS(mysqli_set_opt, mysqli_options, arginfo_three) PHP_FE(mysqli_ping, arginfo_one) PHP_FE(mysqli_poll, arginfo_none) PHP_FE(mysqli_prepare, arginfo_two) PHP_FE(mysqli_report, arginfo_one) PHP_FE(mysqli_query, arginfo_two) PHP_FE(mysqli_real_connect, arginfo_mysqli_real_connect) PHP_FE(mysqli_real_escape_string, arginfo_two) PHP_FALIAS(
                                              mysqli_escape_string,
                                              mysqli_real_escape_string,
                                              arginfo_two
                                          ) PHP_FE(mysqli_real_query, arginfo_two)
                                              PHP_FE(mysqli_reap_async_query, arginfo_one) PHP_FE(mysqli_release_savepoint, arginfo_two) PHP_FE(mysqli_rollback, arginfo_one) PHP_FE(mysqli_savepoint, arginfo_two) PHP_FE(mysqli_select_db, arginfo_two) PHP_FE(mysqli_set_charset, arginfo_two) PHP_FE(mysqli_stmt_affected_rows, arginfo_one) PHP_FE(mysqli_stmt_attr_get, arginfo_two) PHP_FE(mysqli_stmt_attr_set, arginfo_three) PHP_FE(
                                                  mysqli_stmt_bind_param,
                                                  arginfo_stmt_bind_param
                                              ) PHP_FE(mysqli_stmt_bind_result, arginfo_stmt_bind_result)
                                                  PHP_FE(
                                                      mysqli_stmt_close,
                                                      arginfo_one
                                                  ) PHP_FE(mysqli_stmt_data_seek, arginfo_two)
                                                      PHP_FE(
                                                          mysqli_stmt_errno,
                                                          arginfo_one
                                                      ) PHP_FE(mysqli_stmt_error, arginfo_one)
                                                          PHP_FE(
                                                              mysqli_stmt_error_list,
                                                              arginfo_one
                                                          ) PHP_FE(mysqli_stmt_fetch, arginfo_one)
                                                              PHP_FE(mysqli_stmt_field_count, arginfo_one) PHP_FE(
                                                                  mysqli_stmt_free_result,
                                                                  arginfo_one
                                                              ) PHP_FE(mysqli_stmt_get_result, arginfo_one)
                                                                  PHP_FE(
                                                                      mysqli_stmt_get_warnings,
                                                                      arginfo_one
                                                                  ) PHP_FE(mysqli_stmt_init, arginfo_one)
                                                                      PHP_FE(
                                                                          mysqli_stmt_insert_id,
                                                                          arginfo_one
                                                                      ) PHP_FE(mysqli_stmt_more_results, arginfo_one)
                                                                          PHP_FE(mysqli_stmt_next_result, arginfo_one) PHP_FE(
                                                                              mysqli_stmt_num_rows,
                                                                              arginfo_one
                                                                          ) PHP_FE(mysqli_stmt_param_count, arginfo_one)
                                                                              PHP_FE(mysqli_stmt_prepare, arginfo_two) PHP_FE(
                                                                                  mysqli_stmt_reset,
                                                                                  arginfo_one
                                                                              ) PHP_FE(mysqli_stmt_result_metadata, arginfo_one)
                                                                                  PHP_FE(
                                                                                      mysqli_stmt_send_long_data,
                                                                                      arginfo_three
                                                                                  ) PHP_FE(mysqli_stmt_store_result, arginfo_one)
                                                                                      PHP_FE(
                                                                                          mysqli_stmt_sqlstate,
                                                                                          arginfo_one
                                                                                      ) PHP_FE(mysqli_sqlstate, arginfo_one)
                                                                                          PHP_FE(
                                                                                              mysqli_ssl_set,
                                                                                              arginfo_one
                                                                                          ) PHP_FE(mysqli_stat, arginfo_one)
                                                                                              PHP_FE(
                                                                                                  mysqli_store_result,
                                                                                                  arginfo_one
                                                                                              ) PHP_FE(mysqli_thread_id, arginfo_one)
                                                                                                  PHP_FE(
                                                                                                      mysqli_thread_safe,
                                                                                                      arginfo_none
                                                                                                  ) PHP_FE(mysqli_use_result, arginfo_one)
                                                                                                      PHP_FE(
                                                                                                          mysqli_warning_count,
                                                                                                          arginfo_one
                                                                                                      )
                                                                                                          PHP_FE(
                                                                                                              mysqli_refresh,
                                                                                                              arginfo_two
                                                                                                          ) PHP_FE_END};

static const zend_function_entry mylite_mysqli_link_methods[] = {
    PHP_ME(mysqli, __construct, arginfo_connect, ZEND_ACC_PUBLIC) PHP_ME(
        mysqli,
        autocommit,
        arginfo_one,
        ZEND_ACC_PUBLIC
    ) PHP_ME(mysqli, begin_transaction, arginfo_none, ZEND_ACC_PUBLIC)
        PHP_ME(mysqli, change_user, arginfo_three, ZEND_ACC_PUBLIC) PHP_ME(
            mysqli,
            character_set_name,
            arginfo_none,
            ZEND_ACC_PUBLIC
        ) PHP_ME(mysqli, close, arginfo_none, ZEND_ACC_PUBLIC)
            PHP_ME(mysqli, commit, arginfo_none, ZEND_ACC_PUBLIC) PHP_ME(
                mysqli,
                connect,
                arginfo_connect,
                ZEND_ACC_PUBLIC
            ) PHP_ME(mysqli, dump_debug_info, arginfo_none, ZEND_ACC_PUBLIC)
                PHP_ME(mysqli, debug, arginfo_one, ZEND_ACC_PUBLIC) PHP_ME(
                    mysqli,
                    get_charset,
                    arginfo_none,
                    ZEND_ACC_PUBLIC
                ) PHP_ME(mysqli, execute_query, arginfo_two, ZEND_ACC_PUBLIC)
                    PHP_ME(mysqli, get_client_info, arginfo_none, ZEND_ACC_PUBLIC) PHP_ME(
                        mysqli,
                        get_connection_stats,
                        arginfo_none,
                        ZEND_ACC_PUBLIC
                    ) PHP_ME(mysqli, get_server_info, arginfo_none, ZEND_ACC_PUBLIC)
                        PHP_ME(mysqli, get_warnings, arginfo_none, ZEND_ACC_PUBLIC) PHP_ME(
                            mysqli,
                            init,
                            arginfo_none,
                            ZEND_ACC_PUBLIC
                        ) PHP_ME(mysqli, kill, arginfo_one, ZEND_ACC_PUBLIC)
                            PHP_ME(mysqli, multi_query, arginfo_one, ZEND_ACC_PUBLIC) PHP_ME(
                                mysqli,
                                more_results,
                                arginfo_none,
                                ZEND_ACC_PUBLIC
                            ) PHP_ME(mysqli, next_result, arginfo_none, ZEND_ACC_PUBLIC)
                                PHP_ME(mysqli, ping, arginfo_none, ZEND_ACC_PUBLIC) PHP_ME(
                                    mysqli,
                                    poll,
                                    arginfo_none,
                                    ZEND_ACC_PUBLIC | ZEND_ACC_STATIC
                                ) PHP_ME(mysqli, prepare, arginfo_one, ZEND_ACC_PUBLIC)
                                    PHP_ME(mysqli, query, arginfo_one, ZEND_ACC_PUBLIC) PHP_ME(
                                        mysqli,
                                        real_connect,
                                        arginfo_real_connect,
                                        ZEND_ACC_PUBLIC
                                    ) PHP_ME(mysqli, real_escape_string, arginfo_one, ZEND_ACC_PUBLIC)
                                        PHP_MALIAS(
                                            mysqli,
                                            escape_string,
                                            real_escape_string,
                                            arginfo_one,
                                            ZEND_ACC_PUBLIC
                                        ) PHP_ME(mysqli, reap_async_query, arginfo_none, ZEND_ACC_PUBLIC)
                                            PHP_ME(mysqli, real_query, arginfo_one, ZEND_ACC_PUBLIC) PHP_ME(
                                                mysqli,
                                                release_savepoint,
                                                arginfo_one,
                                                ZEND_ACC_PUBLIC
                                            ) PHP_ME(mysqli, rollback, arginfo_none, ZEND_ACC_PUBLIC)
                                                PHP_ME(
                                                    mysqli,
                                                    savepoint,
                                                    arginfo_one,
                                                    ZEND_ACC_PUBLIC
                                                ) PHP_ME(mysqli, select_db, arginfo_one, ZEND_ACC_PUBLIC)
                                                    PHP_ME(
                                                        mysqli,
                                                        set_charset,
                                                        arginfo_one,
                                                        ZEND_ACC_PUBLIC
                                                    ) PHP_ME(mysqli, options, arginfo_two, ZEND_ACC_PUBLIC)
                                                        PHP_ME(
                                                            mysqli,
                                                            set_opt,
                                                            arginfo_two,
                                                            ZEND_ACC_PUBLIC
                                                        )
                                                            PHP_ME(
                                                                mysqli,
                                                                ssl_set,
                                                                arginfo_none,
                                                                ZEND_ACC_PUBLIC
                                                            )
                                                                PHP_ME(
                                                                    mysqli,
                                                                    stat,
                                                                    arginfo_none,
                                                                    ZEND_ACC_PUBLIC
                                                                )
                                                                    PHP_ME(
                                                                        mysqli,
                                                                        stmt_init,
                                                                        arginfo_none,
                                                                        ZEND_ACC_PUBLIC
                                                                    )
                                                                        PHP_ME(
                                                                            mysqli,
                                                                            store_result,
                                                                            arginfo_none,
                                                                            ZEND_ACC_PUBLIC
                                                                        )
                                                                            PHP_ME(
                                                                                mysqli,
                                                                                thread_safe,
                                                                                arginfo_none,
                                                                                ZEND_ACC_PUBLIC
                                                                            )
                                                                                PHP_ME(
                                                                                    mysqli,
                                                                                    use_result,
                                                                                    arginfo_none,
                                                                                    ZEND_ACC_PUBLIC
                                                                                )
                                                                                    PHP_ME(
                                                                                        mysqli,
                                                                                        refresh,
                                                                                        arginfo_one,
                                                                                        ZEND_ACC_PUBLIC
                                                                                    ) PHP_FE_END
};

static const zend_function_entry mylite_mysqli_result_methods[] = {
    PHP_ME(mysqli_result, __construct, arginfo_one, ZEND_ACC_PUBLIC) PHP_ME(
        mysqli_result,
        close,
        arginfo_none,
        ZEND_ACC_PUBLIC
    ) PHP_ME(mysqli_result, free, arginfo_none, ZEND_ACC_PUBLIC)
        PHP_ME(mysqli_result, data_seek, arginfo_one, ZEND_ACC_PUBLIC) PHP_ME(
            mysqli_result,
            fetch_field,
            arginfo_none,
            ZEND_ACC_PUBLIC
        ) PHP_ME(mysqli_result, fetch_fields, arginfo_none, ZEND_ACC_PUBLIC)
            PHP_ME(mysqli_result, fetch_field_direct, arginfo_one, ZEND_ACC_PUBLIC) PHP_ME(
                mysqli_result,
                fetch_all,
                arginfo_none,
                ZEND_ACC_PUBLIC
            ) PHP_ME(mysqli_result, fetch_array, arginfo_none, ZEND_ACC_PUBLIC)
                PHP_ME(mysqli_result, fetch_assoc, arginfo_none, ZEND_ACC_PUBLIC) PHP_ME(
                    mysqli_result,
                    fetch_object,
                    arginfo_none,
                    ZEND_ACC_PUBLIC
                ) PHP_ME(mysqli_result, fetch_row, arginfo_none, ZEND_ACC_PUBLIC)
                    PHP_ME(mysqli_result, fetch_column, arginfo_none, ZEND_ACC_PUBLIC)
                        PHP_ME(mysqli_result, field_seek, arginfo_one, ZEND_ACC_PUBLIC)
                            PHP_ME(mysqli_result, free_result, arginfo_none, ZEND_ACC_PUBLIC)
                                PHP_ME(
                                    mysqli_result,
                                    getIterator,
                                    arginfo_mysqli_result_get_iterator,
                                    ZEND_ACC_PUBLIC
                                ) PHP_FE_END
};

static const zend_function_entry mylite_mysqli_stmt_methods[] = {
    PHP_ME(mysqli_stmt, __construct, arginfo_one, ZEND_ACC_PUBLIC) PHP_ME(
        mysqli_stmt,
        attr_get,
        arginfo_one,
        ZEND_ACC_PUBLIC
    ) PHP_ME(mysqli_stmt, attr_set, arginfo_two, ZEND_ACC_PUBLIC)
        PHP_ME(mysqli_stmt, bind_param, arginfo_stmt_method_bind_param, ZEND_ACC_PUBLIC) PHP_ME(
            mysqli_stmt,
            bind_result,
            arginfo_stmt_method_bind_result,
            ZEND_ACC_PUBLIC
        ) PHP_ME(mysqli_stmt, close, arginfo_none, ZEND_ACC_PUBLIC)
            PHP_ME(mysqli_stmt, data_seek, arginfo_one, ZEND_ACC_PUBLIC) PHP_ME(
                mysqli_stmt,
                execute,
                arginfo_none,
                ZEND_ACC_PUBLIC
            ) PHP_ME(mysqli_stmt, fetch, arginfo_none, ZEND_ACC_PUBLIC)
                PHP_ME(mysqli_stmt, get_warnings, arginfo_none, ZEND_ACC_PUBLIC) PHP_ME(
                    mysqli_stmt,
                    result_metadata,
                    arginfo_none,
                    ZEND_ACC_PUBLIC
                ) PHP_ME(mysqli_stmt, more_results, arginfo_none, ZEND_ACC_PUBLIC)
                    PHP_ME(mysqli_stmt, next_result, arginfo_none, ZEND_ACC_PUBLIC) PHP_ME(
                        mysqli_stmt,
                        num_rows,
                        arginfo_none,
                        ZEND_ACC_PUBLIC
                    ) PHP_ME(mysqli_stmt, send_long_data, arginfo_two, ZEND_ACC_PUBLIC)
                        PHP_ME(mysqli_stmt, free_result, arginfo_none, ZEND_ACC_PUBLIC) PHP_ME(
                            mysqli_stmt,
                            reset,
                            arginfo_none,
                            ZEND_ACC_PUBLIC
                        ) PHP_ME(mysqli_stmt, prepare, arginfo_one, ZEND_ACC_PUBLIC)
                            PHP_ME(mysqli_stmt, store_result, arginfo_none, ZEND_ACC_PUBLIC)
                                PHP_ME(mysqli_stmt, get_result, arginfo_none, ZEND_ACC_PUBLIC)
                                    PHP_FE_END
};

static const zend_function_entry mylite_mysqli_warning_methods[] = {
    PHP_ME(mysqli_warning, __construct, arginfo_none, ZEND_ACC_PRIVATE)
        PHP_ME(mysqli_warning, next, arginfo_none, ZEND_ACC_PUBLIC) PHP_FE_END
};

static const zend_function_entry mylite_mysqli_exception_methods[] = {
    PHP_ME(mysqli_sql_exception, getSqlState, arginfo_none, ZEND_ACC_PUBLIC) PHP_FE_END
};

zend_module_entry mysqli_module_entry = {
    STANDARD_MODULE_HEADER,
    "mysqli",
    mylite_mysqli_functions,
    PHP_MINIT(mylite_mysqli),
    PHP_MSHUTDOWN(mylite_mysqli),
    NULL,
    NULL,
    PHP_MINFO(mylite_mysqli),
    PHP_MYLITE_MYSQLI_VERSION,
    PHP_MODULE_GLOBALS(mylite_mysqli),
    PHP_GINIT(mylite_mysqli),
    PHP_GSHUTDOWN(mylite_mysqli),
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

ZEND_GET_MODULE(mysqli)

PHP_FUNCTION(mysqli_connect) {
    char *hostname = NULL;
    char *username = NULL;
    char *password = NULL;
    char *database = NULL;
    char *socket_name = NULL;
    size_t hostname_len = 0;
    size_t username_len = 0;
    size_t password_len = 0;
    size_t database_len = 0;
    size_t socket_name_len = 0;
    zval *port = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 6)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING_OR_NULL(hostname, hostname_len)
    Z_PARAM_STRING_OR_NULL(username, username_len)
    Z_PARAM_STRING_OR_NULL(password, password_len)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    Z_PARAM_ZVAL_OR_NULL(port)
    Z_PARAM_STRING_OR_NULL(socket_name, socket_name_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)username;
    (void)username_len;
    (void)password;
    (void)password_len;
    (void)port;

    object_init_ex(return_value, mylite_mysqli_link_ce);
    if (!mylite_mysqli_connect_link(
            Z_MYSQLI_LINK_P(return_value),
            hostname,
            hostname_len,
            database,
            database_len,
            socket_name,
            socket_name_len
        )) {
        zval_ptr_dtor(return_value);
        RETURN_FALSE;
    }
}

PHP_METHOD(mysqli, __construct) {
    char *hostname = NULL;
    char *username = NULL;
    char *password = NULL;
    char *database = NULL;
    char *socket_name = NULL;
    size_t hostname_len = 0;
    size_t username_len = 0;
    size_t password_len = 0;
    size_t database_len = 0;
    size_t socket_name_len = 0;
    zval *port = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 6)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING_OR_NULL(hostname, hostname_len)
    Z_PARAM_STRING_OR_NULL(username, username_len)
    Z_PARAM_STRING_OR_NULL(password, password_len)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    Z_PARAM_ZVAL_OR_NULL(port)
    Z_PARAM_STRING_OR_NULL(socket_name, socket_name_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)username;
    (void)username_len;
    (void)password;
    (void)password_len;
    (void)port;

    if (ZEND_NUM_ARGS() == 0) {
        return;
    }

    (void)mylite_mysqli_connect_link(
        Z_MYSQLI_LINK_P(ZEND_THIS),
        hostname,
        hostname_len,
        database,
        database_len,
        socket_name,
        socket_name_len
    );
}

PHP_FUNCTION(mysqli_real_connect) {
    zval *link_zv;
    char *hostname = NULL;
    char *username = NULL;
    char *password = NULL;
    char *database = NULL;
    char *socket_name = NULL;
    size_t hostname_len = 0;
    size_t username_len = 0;
    size_t password_len = 0;
    size_t database_len = 0;
    size_t socket_name_len = 0;
    zval *port = NULL;
    zend_long flags = 0;

    ZEND_PARSE_PARAMETERS_START(1, 8)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING_OR_NULL(hostname, hostname_len)
    Z_PARAM_STRING_OR_NULL(username, username_len)
    Z_PARAM_STRING_OR_NULL(password, password_len)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    Z_PARAM_ZVAL_OR_NULL(port)
    Z_PARAM_STRING_OR_NULL(socket_name, socket_name_len)
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();

    (void)username;
    (void)username_len;
    (void)password;
    (void)password_len;
    (void)port;
    (void)flags;

    RETURN_BOOL(mylite_mysqli_connect_link(
        Z_MYSQLI_LINK_P(link_zv),
        hostname,
        hostname_len,
        database,
        database_len,
        socket_name,
        socket_name_len
    ));
}

PHP_METHOD(mysqli, real_connect) {
    char *hostname = NULL;
    char *username = NULL;
    char *password = NULL;
    char *database = NULL;
    char *socket_name = NULL;
    size_t hostname_len = 0;
    size_t username_len = 0;
    size_t password_len = 0;
    size_t database_len = 0;
    size_t socket_name_len = 0;
    zval *port = NULL;
    zend_long flags = 0;

    ZEND_PARSE_PARAMETERS_START(0, 7)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING_OR_NULL(hostname, hostname_len)
    Z_PARAM_STRING_OR_NULL(username, username_len)
    Z_PARAM_STRING_OR_NULL(password, password_len)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    Z_PARAM_ZVAL_OR_NULL(port)
    Z_PARAM_STRING_OR_NULL(socket_name, socket_name_len)
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();

    (void)username;
    (void)username_len;
    (void)password;
    (void)password_len;
    (void)port;
    (void)flags;

    RETURN_BOOL(mylite_mysqli_connect_link(
        Z_MYSQLI_LINK_P(ZEND_THIS),
        hostname,
        hostname_len,
        database,
        database_len,
        socket_name,
        socket_name_len
    ));
}

PHP_METHOD(mysqli, connect) {
    char *hostname = NULL;
    char *username = NULL;
    char *password = NULL;
    char *database = NULL;
    char *socket_name = NULL;
    size_t hostname_len = 0;
    size_t username_len = 0;
    size_t password_len = 0;
    size_t database_len = 0;
    size_t socket_name_len = 0;
    zval *port = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 6)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING_OR_NULL(hostname, hostname_len)
    Z_PARAM_STRING_OR_NULL(username, username_len)
    Z_PARAM_STRING_OR_NULL(password, password_len)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    Z_PARAM_ZVAL_OR_NULL(port)
    Z_PARAM_STRING_OR_NULL(socket_name, socket_name_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)username;
    (void)username_len;
    (void)password;
    (void)password_len;
    (void)port;

    RETURN_BOOL(mylite_mysqli_connect_link(
        Z_MYSQLI_LINK_P(ZEND_THIS),
        hostname,
        hostname_len,
        database,
        database_len,
        socket_name,
        socket_name_len
    ));
}

PHP_FUNCTION(mysqli_init) {
    ZEND_PARSE_PARAMETERS_NONE();
    object_init_ex(return_value, mylite_mysqli_link_ce);
}

PHP_METHOD(mysqli, init) {
    ZEND_PARSE_PARAMETERS_NONE();
}

PHP_FUNCTION(mysqli_close) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(link_zv);
    if (link->db != NULL) {
        (void)mylite_close(link->db);
        link->db = NULL;
    }
    link->connected = false;
    RETURN_TRUE;
}

PHP_METHOD(mysqli, close) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(ZEND_THIS);
    if (link->db != NULL) {
        (void)mylite_close(link->db);
        link->db = NULL;
    }
    link->connected = false;
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_query) {
    zval *link_zv;
    char *query;
    size_t query_len;
    zend_long result_mode = MYLITE_MYSQLI_STORE_RESULT;

    ZEND_PARSE_PARAMETERS_START(2, 3)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(query, query_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(result_mode)
    ZEND_PARSE_PARAMETERS_END();

    if (!mylite_mysqli_query_link(
            Z_MYSQLI_LINK_P(link_zv),
            query,
            query_len,
            result_mode,
            return_value
        )) {
        RETURN_FALSE;
    }
}

PHP_METHOD(mysqli, query) {
    char *query;
    size_t query_len;
    zend_long result_mode = MYLITE_MYSQLI_STORE_RESULT;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(query, query_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(result_mode)
    ZEND_PARSE_PARAMETERS_END();

    if (!mylite_mysqli_query_link(
            Z_MYSQLI_LINK_P(ZEND_THIS),
            query,
            query_len,
            result_mode,
            return_value
        )) {
        RETURN_FALSE;
    }
}

PHP_FUNCTION(mysqli_real_query) {
    zval *link_zv;
    char *query;
    size_t query_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(query, query_len)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(mylite_mysqli_real_query_link(Z_MYSQLI_LINK_P(link_zv), query, query_len));
}

PHP_METHOD(mysqli, real_query) {
    char *query;
    size_t query_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(query, query_len)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(mylite_mysqli_real_query_link(Z_MYSQLI_LINK_P(ZEND_THIS), query, query_len));
}

PHP_FUNCTION(mysqli_store_result) {
    zval *link_zv;
    zend_long mode = MYLITE_MYSQLI_STORE_RESULT;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    (void)mode;
    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(link_zv);
    if (Z_ISUNDEF(link->pending_result)) {
        RETURN_FALSE;
    }
    ZVAL_COPY(return_value, &link->pending_result);
    zval_ptr_dtor(&link->pending_result);
    ZVAL_UNDEF(&link->pending_result);
}

PHP_METHOD(mysqli, store_result) {
    zend_long mode = MYLITE_MYSQLI_STORE_RESULT;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    (void)mode;
    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(ZEND_THIS);
    if (Z_ISUNDEF(link->pending_result)) {
        RETURN_FALSE;
    }
    ZVAL_COPY(return_value, &link->pending_result);
    zval_ptr_dtor(&link->pending_result);
    ZVAL_UNDEF(&link->pending_result);
}

PHP_FUNCTION(mysqli_use_result) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(link_zv);
    if (Z_ISUNDEF(link->pending_result)) {
        RETURN_FALSE;
    }
    ZVAL_COPY(return_value, &link->pending_result);
    zval_ptr_dtor(&link->pending_result);
    ZVAL_UNDEF(&link->pending_result);
}

PHP_METHOD(mysqli, use_result) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(ZEND_THIS);
    if (Z_ISUNDEF(link->pending_result)) {
        RETURN_FALSE;
    }
    ZVAL_COPY(return_value, &link->pending_result);
    zval_ptr_dtor(&link->pending_result);
    ZVAL_UNDEF(&link->pending_result);
}

PHP_FUNCTION(mysqli_execute_query) {
    zval *link_zv;
    char *query;
    size_t query_len;
    zval *params = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 3)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(query, query_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ARRAY_OR_NULL(params)
    ZEND_PARSE_PARAMETERS_END();

    zval stmt_zv;
    object_init_ex(&stmt_zv, mylite_mysqli_stmt_ce);
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(&stmt_zv);
    if (!mylite_mysqli_prepare_stmt(stmt, link_zv, query, query_len) ||
        !mylite_mysqli_execute_stmt(stmt, params)) {
        zval_ptr_dtor(&stmt_zv);
        RETURN_FALSE;
    }

    if (Z_ISUNDEF(stmt->result)) {
        zval_ptr_dtor(&stmt_zv);
        RETURN_TRUE;
    }

    ZVAL_COPY(return_value, &stmt->result);
    zval_ptr_dtor(&stmt_zv);
}

PHP_METHOD(mysqli, execute_query) {
    char *query;
    size_t query_len;
    zval *params = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STRING(query, query_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_ARRAY_OR_NULL(params)
    ZEND_PARSE_PARAMETERS_END();

    zval stmt_zv;
    object_init_ex(&stmt_zv, mylite_mysqli_stmt_ce);
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(&stmt_zv);
    if (!mylite_mysqli_prepare_stmt(stmt, ZEND_THIS, query, query_len) ||
        !mylite_mysqli_execute_stmt(stmt, params)) {
        zval_ptr_dtor(&stmt_zv);
        RETURN_FALSE;
    }

    if (Z_ISUNDEF(stmt->result)) {
        zval_ptr_dtor(&stmt_zv);
        RETURN_TRUE;
    }

    ZVAL_COPY(return_value, &stmt->result);
    zval_ptr_dtor(&stmt_zv);
}

PHP_FUNCTION(mysqli_prepare) {
    zval *link_zv;
    char *query;
    size_t query_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(query, query_len)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, mylite_mysqli_stmt_ce);
    if (!mylite_mysqli_prepare_stmt(Z_MYSQLI_STMT_P(return_value), link_zv, query, query_len)) {
        zval_ptr_dtor(return_value);
        RETURN_FALSE;
    }
}

PHP_METHOD(mysqli, prepare) {
    char *query;
    size_t query_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(query, query_len)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, mylite_mysqli_stmt_ce);
    if (!mylite_mysqli_prepare_stmt(Z_MYSQLI_STMT_P(return_value), ZEND_THIS, query, query_len)) {
        zval_ptr_dtor(return_value);
        RETURN_FALSE;
    }
}

PHP_FUNCTION(mysqli_stmt_init) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, mylite_mysqli_stmt_ce);
    ZVAL_COPY(&Z_MYSQLI_STMT_P(return_value)->link, link_zv);
}

PHP_METHOD(mysqli, stmt_init) {
    ZEND_PARSE_PARAMETERS_NONE();
    object_init_ex(return_value, mylite_mysqli_stmt_ce);
    ZVAL_COPY(&Z_MYSQLI_STMT_P(return_value)->link, ZEND_THIS);
}

PHP_METHOD(mysqli_stmt, __construct) {
    zval *link_zv;
    char *query = NULL;
    size_t query_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING_OR_NULL(query, query_len)
    ZEND_PARSE_PARAMETERS_END();

    ZVAL_COPY(&Z_MYSQLI_STMT_P(ZEND_THIS)->link, link_zv);
    if (query != NULL) {
        (void)mylite_mysqli_prepare_stmt(Z_MYSQLI_STMT_P(ZEND_THIS), link_zv, query, query_len);
    }
}

PHP_FUNCTION(mysqli_stmt_prepare) {
    zval *stmt_zv;
    char *query;
    size_t query_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    Z_PARAM_STRING(query, query_len)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (Z_ISUNDEF(stmt->link)) {
        RETURN_FALSE;
    }
    RETURN_BOOL(mylite_mysqli_prepare_stmt(stmt, &stmt->link, query, query_len));
}

PHP_METHOD(mysqli_stmt, prepare) {
    char *query;
    size_t query_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(query, query_len)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (Z_ISUNDEF(stmt->link)) {
        RETURN_FALSE;
    }
    RETURN_BOOL(mylite_mysqli_prepare_stmt(stmt, &stmt->link, query, query_len));
}

PHP_FUNCTION(mysqli_stmt_bind_param) {
    zval *stmt_zv;
    char *types;
    size_t types_len;
    zval *vars = NULL;
    uint32_t vars_count = 0;

    ZEND_PARSE_PARAMETERS_START(2, -1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    Z_PARAM_STRING(types, types_len)
    Z_PARAM_VARIADIC('*', vars, vars_count)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (types_len != vars_count || (zend_long)types_len != stmt->param_count) {
        RETURN_FALSE;
    }

    if (!Z_ISUNDEF(stmt->bound_params)) {
        zval_ptr_dtor(&stmt->bound_params);
    }
    array_init_size(&stmt->bound_params, vars_count);
    for (uint32_t i = 0; i < vars_count; ++i) {
        zval copied;
        ZVAL_COPY(&copied, &vars[i]);
        add_index_zval(&stmt->bound_params, i, &copied);
    }

    if (stmt->types != NULL) {
        zend_string_release(stmt->types);
    }
    stmt->types = zend_string_init(types, types_len, false);
    RETURN_TRUE;
}

PHP_METHOD(mysqli_stmt, bind_param) {
    char *types;
    size_t types_len;
    zval *vars = NULL;
    uint32_t vars_count = 0;

    ZEND_PARSE_PARAMETERS_START(1, -1)
    Z_PARAM_STRING(types, types_len)
    Z_PARAM_VARIADIC('*', vars, vars_count)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (types_len != vars_count || (zend_long)types_len != stmt->param_count) {
        RETURN_FALSE;
    }

    if (!Z_ISUNDEF(stmt->bound_params)) {
        zval_ptr_dtor(&stmt->bound_params);
    }
    array_init_size(&stmt->bound_params, vars_count);
    for (uint32_t i = 0; i < vars_count; ++i) {
        zval copied;
        ZVAL_COPY(&copied, &vars[i]);
        add_index_zval(&stmt->bound_params, i, &copied);
    }

    if (stmt->types != NULL) {
        zend_string_release(stmt->types);
    }
    stmt->types = zend_string_init(types, types_len, false);
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_stmt_execute) {
    zval *stmt_zv;
    zval *params = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_ARRAY_OR_NULL(params)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(mylite_mysqli_execute_stmt(Z_MYSQLI_STMT_P(stmt_zv), params));
}

PHP_METHOD(mysqli_stmt, execute) {
    zval *params = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_ARRAY_OR_NULL(params)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(mylite_mysqli_execute_stmt(Z_MYSQLI_STMT_P(ZEND_THIS), params));
}

PHP_FUNCTION(mysqli_stmt_get_result) {
    zval *stmt_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (Z_ISUNDEF(stmt->result)) {
        RETURN_FALSE;
    }
    ZVAL_COPY(return_value, &stmt->result);
}

PHP_METHOD(mysqli_stmt, get_result) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (Z_ISUNDEF(stmt->result)) {
        RETURN_FALSE;
    }
    ZVAL_COPY(return_value, &stmt->result);
}

PHP_FUNCTION(mysqli_stmt_bind_result) {
    zval *stmt_zv;
    zval *vars = NULL;
    uint32_t vars_count = 0;

    ZEND_PARSE_PARAMETERS_START(1, -1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    Z_PARAM_VARIADIC('*', vars, vars_count)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (!Z_ISUNDEF(stmt->bound_results)) {
        zval_ptr_dtor(&stmt->bound_results);
    }
    array_init_size(&stmt->bound_results, vars_count);
    for (uint32_t i = 0; i < vars_count; ++i) {
        zval copied;
        ZVAL_COPY(&copied, &vars[i]);
        add_index_zval(&stmt->bound_results, i, &copied);
    }
    RETURN_TRUE;
}

PHP_METHOD(mysqli_stmt, bind_result) {
    zval *vars = NULL;
    uint32_t vars_count = 0;

    ZEND_PARSE_PARAMETERS_START(0, -1)
    Z_PARAM_VARIADIC('*', vars, vars_count)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (!Z_ISUNDEF(stmt->bound_results)) {
        zval_ptr_dtor(&stmt->bound_results);
    }
    array_init_size(&stmt->bound_results, vars_count);
    for (uint32_t i = 0; i < vars_count; ++i) {
        zval copied;
        ZVAL_COPY(&copied, &vars[i]);
        add_index_zval(&stmt->bound_results, i, &copied);
    }
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_stmt_fetch) {
    zval *stmt_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (Z_ISUNDEF(stmt->result)) {
        RETURN_FALSE;
    }
    mylite_mysqli_result *result = Z_MYSQLI_RESULT_P(&stmt->result);
    if (result->current_row >= zend_hash_num_elements(Z_ARRVAL(result->rows))) {
        RETURN_NULL();
    }

    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), (zend_ulong)result->current_row);
    if (row == NULL || Z_TYPE_P(row) != IS_ARRAY) {
        RETURN_FALSE;
    }

    if (!Z_ISUNDEF(stmt->bound_results)) {
        uint32_t index = 0;
        zval *target;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(stmt->bound_results), target) {
            zval *value = zend_hash_index_find(Z_ARRVAL_P(row), index);
            if (value == NULL || !Z_ISREF_P(target)) {
                break;
            }
            zval *slot = Z_REFVAL_P(target);
            zval_ptr_dtor(slot);
            ZVAL_COPY(slot, value);
            ++index;
        }
        ZEND_HASH_FOREACH_END();
    }

    ++result->current_row;
    RETURN_TRUE;
}

PHP_METHOD(mysqli_stmt, fetch) {
    ZEND_PARSE_PARAMETERS_NONE();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (Z_ISUNDEF(stmt->result)) {
        RETURN_FALSE;
    }
    mylite_mysqli_result *result = Z_MYSQLI_RESULT_P(&stmt->result);
    if (result->current_row >= zend_hash_num_elements(Z_ARRVAL(result->rows))) {
        RETURN_NULL();
    }

    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), (zend_ulong)result->current_row);
    if (row == NULL || Z_TYPE_P(row) != IS_ARRAY) {
        RETURN_FALSE;
    }

    if (!Z_ISUNDEF(stmt->bound_results)) {
        uint32_t index = 0;
        zval *target;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(stmt->bound_results), target) {
            zval *value = zend_hash_index_find(Z_ARRVAL_P(row), index);
            if (value == NULL || !Z_ISREF_P(target)) {
                break;
            }
            zval *slot = Z_REFVAL_P(target);
            zval_ptr_dtor(slot);
            ZVAL_COPY(slot, value);
            ++index;
        }
        ZEND_HASH_FOREACH_END();
    }

    ++result->current_row;
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_fetch_assoc) {
    zval *result_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result_fetch_array(
        Z_MYSQLI_RESULT_P(result_zv),
        MYLITE_MYSQLI_ASSOC,
        return_value
    );
}

PHP_METHOD(mysqli_result, fetch_assoc) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_result_fetch_array(
        Z_MYSQLI_RESULT_P(ZEND_THIS),
        MYLITE_MYSQLI_ASSOC,
        return_value
    );
}

PHP_FUNCTION(mysqli_fetch_row) {
    zval *result_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result_fetch_array(Z_MYSQLI_RESULT_P(result_zv), MYLITE_MYSQLI_NUM, return_value);
}

PHP_METHOD(mysqli_result, fetch_row) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_result_fetch_array(Z_MYSQLI_RESULT_P(ZEND_THIS), MYLITE_MYSQLI_NUM, return_value);
}

PHP_FUNCTION(mysqli_fetch_array) {
    zval *result_zv;
    zend_long mode = MYLITE_MYSQLI_BOTH;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result_fetch_array(Z_MYSQLI_RESULT_P(result_zv), mode, return_value);
}

PHP_METHOD(mysqli_result, fetch_array) {
    zend_long mode = MYLITE_MYSQLI_BOTH;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result_fetch_array(Z_MYSQLI_RESULT_P(ZEND_THIS), mode, return_value);
}

PHP_FUNCTION(mysqli_fetch_all) {
    zval *result_zv;
    zend_long mode = MYLITE_MYSQLI_NUM;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result *result = Z_MYSQLI_RESULT_P(result_zv);
    array_init(return_value);
    while (result->current_row < zend_hash_num_elements(Z_ARRVAL(result->rows))) {
        zval row;
        mylite_mysqli_result_fetch_array(result, mode, &row);
        add_next_index_zval(return_value, &row);
    }
}

PHP_METHOD(mysqli_result, fetch_all) {
    zend_long mode = MYLITE_MYSQLI_NUM;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result *result = Z_MYSQLI_RESULT_P(ZEND_THIS);
    array_init(return_value);
    while (result->current_row < zend_hash_num_elements(Z_ARRVAL(result->rows))) {
        zval row;
        mylite_mysqli_result_fetch_array(result, mode, &row);
        add_next_index_zval(return_value, &row);
    }
}

PHP_FUNCTION(mysqli_fetch_object) {
    zval *result_zv;
    char *class_name = "stdClass";
    size_t class_name_len = sizeof("stdClass") - 1;
    zval *constructor_args = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 3)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING(class_name, class_name_len)
    Z_PARAM_ARRAY(constructor_args)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result_fetch_object(
        Z_MYSQLI_RESULT_P(result_zv),
        class_name,
        class_name_len,
        constructor_args,
        return_value
    );
}

PHP_METHOD(mysqli_result, fetch_object) {
    char *class_name = "stdClass";
    size_t class_name_len = sizeof("stdClass") - 1;
    zval *constructor_args = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 2)
    Z_PARAM_OPTIONAL
    Z_PARAM_STRING(class_name, class_name_len)
    Z_PARAM_ARRAY(constructor_args)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result_fetch_object(
        Z_MYSQLI_RESULT_P(ZEND_THIS),
        class_name,
        class_name_len,
        constructor_args,
        return_value
    );
}

PHP_FUNCTION(mysqli_fetch_column) {
    zval *result_zv;
    zend_long column = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(column)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result_fetch_column(Z_MYSQLI_RESULT_P(result_zv), column, return_value);
}

PHP_METHOD(mysqli_result, fetch_column) {
    zend_long column = 0;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(column)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result_fetch_column(Z_MYSQLI_RESULT_P(ZEND_THIS), column, return_value);
}

static void mylite_mysqli_return_field(zval *field, zval *return_value);
static void mylite_mysqli_return_field_at(
    mylite_mysqli_result *result,
    zend_ulong index,
    zval *return_value
);
static zend_string *mylite_mysqli_quote_identifier(const char *value, size_t value_len);
static zend_string *mylite_mysqli_resolve_filename(
    const char *hostname,
    size_t hostname_len,
    const char *database,
    size_t database_len,
    const char *socket_name,
    size_t socket_name_len
);
static bool mylite_mysqli_execute_sql(mylite_mysqli_link *link, zend_string *sql);
static bool mylite_mysqli_run_prepared(
    mylite_mysqli_link *link,
    mylite_stmt *stmt,
    zval *result_zv,
    zend_long result_mode
);
static bool mylite_mysqli_bind_stmt_params(mylite_mysqli_stmt *stmt, zval *params);
static bool mylite_mysqli_bind_one(mylite_stmt *stmt, unsigned index, zval *value, char type);
static void mylite_mysqli_column_to_zval(
    mylite_stmt *stmt,
    unsigned column,
    bool native_numbers,
    zval *return_value
);
static void mylite_mysqli_init_result_from_stmt(
    zval *result_zv,
    mylite_stmt *stmt,
    zend_long result_mode
);
static void mylite_mysqli_append_current_row(
    mylite_mysqli_result *result,
    mylite_stmt *stmt,
    bool native_numbers
);
static void mylite_mysqli_update_link_props(mylite_mysqli_link *link);
static void mylite_mysqli_set_link_error(
    mylite_mysqli_link *link,
    zend_long errno_value,
    const char *sqlstate,
    const char *message
);
static void mylite_mysqli_set_link_error_from_db(mylite_mysqli_link *link, int result);
static void mylite_mysqli_clear_link_error(mylite_mysqli_link *link);
static void mylite_mysqli_update_stmt_props(mylite_mysqli_stmt *stmt);
static void mylite_mysqli_set_stmt_error_from_link(mylite_mysqli_stmt *stmt);
static void mylite_mysqli_clear_stmt_error(mylite_mysqli_stmt *stmt);
static void mylite_mysqli_update_result_props(mylite_mysqli_result *result);
static void mylite_mysqli_update_result_lengths(mylite_mysqli_result *result, zval *row);
static void mylite_mysqli_register_constants(INIT_FUNC_ARGS);
static void mylite_mysqli_register_classes(INIT_FUNC_ARGS);
static void mylite_mysqli_register_link_properties(zend_class_entry *ce);
static void mylite_mysqli_register_result_properties(zend_class_entry *ce);
static void mylite_mysqli_register_stmt_properties(zend_class_entry *ce);
static void mylite_mysqli_register_driver_properties(zend_class_entry *ce);
static void mylite_mysqli_register_warning_properties(zend_class_entry *ce);
static void mylite_mysqli_maybe_report_error(mylite_mysqli_link *link);
static void mylite_mysqli_set_global_connect_error(zend_long errno_value, const char *message);
static bool mylite_mysqli_is_identifier_safe(const char *value, size_t value_len);
static bool mylite_mysqli_is_pathlike(const char *value, size_t value_len);
static bool mylite_mysqli_has_suffix(const char *value, size_t value_len, const char *suffix);
static void mylite_mysqli_string_or_null_property(
    zend_class_entry *ce,
    zend_object *object,
    const char *name,
    const char *value
);

PHP_FUNCTION(mysqli_fetch_field) {
    zval *result_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result *result = Z_MYSQLI_RESULT_P(result_zv);
    mylite_mysqli_return_field_at(result, (zend_ulong)result->current_field, return_value);
    if (Z_TYPE_P(return_value) != IS_FALSE) {
        ++result->current_field;
        mylite_mysqli_update_result_props(result);
    }
}

PHP_METHOD(mysqli_result, fetch_field) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_result *result = Z_MYSQLI_RESULT_P(ZEND_THIS);
    mylite_mysqli_return_field_at(result, (zend_ulong)result->current_field, return_value);
    if (Z_TYPE_P(return_value) != IS_FALSE) {
        ++result->current_field;
        mylite_mysqli_update_result_props(result);
    }
}

PHP_FUNCTION(mysqli_fetch_fields) {
    zval *result_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    ZEND_PARSE_PARAMETERS_END();

    array_init(return_value);
    zval *field;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(Z_MYSQLI_RESULT_P(result_zv)->fields), field) {
        zval copied;
        mylite_mysqli_return_field(field, &copied);
        add_next_index_zval(return_value, &copied);
    }
    ZEND_HASH_FOREACH_END();
}

PHP_METHOD(mysqli_result, fetch_fields) {
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    zval *field;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(Z_MYSQLI_RESULT_P(ZEND_THIS)->fields), field) {
        zval copied;
        mylite_mysqli_return_field(field, &copied);
        add_next_index_zval(return_value, &copied);
    }
    ZEND_HASH_FOREACH_END();
}

PHP_FUNCTION(mysqli_fetch_field_direct) {
    zval *result_zv;
    zend_long index;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    Z_PARAM_LONG(index)
    ZEND_PARSE_PARAMETERS_END();

    if (index < 0) {
        RETURN_FALSE;
    }
    mylite_mysqli_return_field_at(Z_MYSQLI_RESULT_P(result_zv), (zend_ulong)index, return_value);
}

PHP_METHOD(mysqli_result, fetch_field_direct) {
    zend_long index;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(index)
    ZEND_PARSE_PARAMETERS_END();

    if (index < 0) {
        RETURN_FALSE;
    }
    mylite_mysqli_return_field_at(Z_MYSQLI_RESULT_P(ZEND_THIS), (zend_ulong)index, return_value);
}

PHP_FUNCTION(mysqli_fetch_lengths) {
    zval *result_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_result *result = Z_MYSQLI_RESULT_P(result_zv);
    if (Z_TYPE(result->lengths) != IS_ARRAY) {
        RETURN_FALSE;
    }
    ZVAL_COPY(return_value, &result->lengths);
}

PHP_FUNCTION(mysqli_data_seek) {
    zval *result_zv;
    zend_long offset;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    Z_PARAM_LONG(offset)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(mylite_mysqli_result_seek(Z_MYSQLI_RESULT_P(result_zv), offset));
}

PHP_METHOD(mysqli_result, data_seek) {
    zend_long offset;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(offset)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(mylite_mysqli_result_seek(Z_MYSQLI_RESULT_P(ZEND_THIS), offset));
}

PHP_FUNCTION(mysqli_field_seek) {
    zval *result_zv;
    zend_long index;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    Z_PARAM_LONG(index)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(mylite_mysqli_field_seek_result(Z_MYSQLI_RESULT_P(result_zv), index));
}

PHP_METHOD(mysqli_result, field_seek) {
    zend_long index;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(index)
    ZEND_PARSE_PARAMETERS_END();

    if (!mylite_mysqli_field_seek_result(Z_MYSQLI_RESULT_P(ZEND_THIS), index)) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_field_tell) {
    zval *result_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_LONG(Z_MYSQLI_RESULT_P(result_zv)->current_field);
}

PHP_FUNCTION(mysqli_free_result) {
    zval *result_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    ZEND_PARSE_PARAMETERS_END();

    zval_ptr_dtor(&Z_MYSQLI_RESULT_P(result_zv)->rows);
    array_init(&Z_MYSQLI_RESULT_P(result_zv)->rows);
    Z_MYSQLI_RESULT_P(result_zv)->current_row = 0;
}

PHP_METHOD(mysqli_result, close) {
    ZEND_PARSE_PARAMETERS_NONE();
    zval_ptr_dtor(&Z_MYSQLI_RESULT_P(ZEND_THIS)->rows);
    array_init(&Z_MYSQLI_RESULT_P(ZEND_THIS)->rows);
    Z_MYSQLI_RESULT_P(ZEND_THIS)->current_row = 0;
}

PHP_METHOD(mysqli_result, free) {
    ZEND_PARSE_PARAMETERS_NONE();
    zval_ptr_dtor(&Z_MYSQLI_RESULT_P(ZEND_THIS)->rows);
    array_init(&Z_MYSQLI_RESULT_P(ZEND_THIS)->rows);
    Z_MYSQLI_RESULT_P(ZEND_THIS)->current_row = 0;
}

PHP_METHOD(mysqli_result, free_result) {
    ZEND_PARSE_PARAMETERS_NONE();
    zval_ptr_dtor(&Z_MYSQLI_RESULT_P(ZEND_THIS)->rows);
    array_init(&Z_MYSQLI_RESULT_P(ZEND_THIS)->rows);
    Z_MYSQLI_RESULT_P(ZEND_THIS)->current_row = 0;
}

PHP_METHOD(mysqli_result, __construct) {
    zval *link_zv;
    zend_long mode = MYLITE_MYSQLI_STORE_RESULT;

    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    Z_MYSQLI_RESULT_P(ZEND_THIS)->type = mode;
    mylite_mysqli_update_result_props(Z_MYSQLI_RESULT_P(ZEND_THIS));
}

PHP_METHOD(mysqli_result, getIterator) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_result *result = Z_MYSQLI_RESULT_P(ZEND_THIS);
    zend_string *class_name = zend_string_init("ArrayIterator", sizeof("ArrayIterator") - 1, false);
    zend_class_entry *ce = zend_lookup_class(class_name);
    zend_string_release(class_name);
    if (ce == NULL) {
        RETURN_FALSE;
    }

    zval rows;
    array_init(&rows);
    zend_ulong old_row = (zend_ulong)result->current_row;
    result->current_row = 0;
    while (result->current_row < zend_hash_num_elements(Z_ARRVAL(result->rows))) {
        zval row;
        mylite_mysqli_result_fetch_array(result, MYLITE_MYSQLI_ASSOC, &row);
        add_next_index_zval(&rows, &row);
    }
    result->current_row = (zend_long)old_row;

    object_init_ex(return_value, ce);
    zend_call_method_with_1_params(Z_OBJ_P(return_value), ce, NULL, "__construct", NULL, &rows);
    zval_ptr_dtor(&rows);
}

PHP_FUNCTION(mysqli_num_rows) {
    zval *result_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_LONG((zend_long)zend_hash_num_elements(Z_ARRVAL(Z_MYSQLI_RESULT_P(result_zv)->rows)));
}

PHP_FUNCTION(mysqli_num_fields) {
    zval *result_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(result_zv, mylite_mysqli_result_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_LONG((zend_long)zend_hash_num_elements(Z_ARRVAL(Z_MYSQLI_RESULT_P(result_zv)->fields)));
}

PHP_FUNCTION(mysqli_field_count) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_LONG(Z_MYSQLI_LINK_P(link_zv)->field_count);
}

PHP_FUNCTION(mysqli_affected_rows) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_LONG(Z_MYSQLI_LINK_P(link_zv)->affected_rows);
}

PHP_FUNCTION(mysqli_insert_id) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_LONG((zend_long)Z_MYSQLI_LINK_P(link_zv)->insert_id);
}

PHP_FUNCTION(mysqli_errno) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_LONG(Z_MYSQLI_LINK_P(link_zv)->errno_value);
}

PHP_FUNCTION(mysqli_error) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_STR_COPY(Z_MYSQLI_LINK_P(link_zv)->error);
}

PHP_FUNCTION(mysqli_sqlstate) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_STR_COPY(Z_MYSQLI_LINK_P(link_zv)->sqlstate);
}

PHP_FUNCTION(mysqli_error_list) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(link_zv);
    array_init(return_value);
    if (link->errno_value != 0) {
        zval item;
        array_init(&item);
        add_assoc_long(&item, "errno", link->errno_value);
        add_assoc_str(&item, "sqlstate", zend_string_copy(link->sqlstate));
        add_assoc_str(&item, "error", zend_string_copy(link->error));
        add_next_index_zval(return_value, &item);
    }
}

PHP_FUNCTION(mysqli_connect_errno) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(MYLITE_MYSQLI_G(connect_errno));
}

PHP_FUNCTION(mysqli_connect_error) {
    ZEND_PARSE_PARAMETERS_NONE();
    if (MYLITE_MYSQLI_G(connect_error) == NULL) {
        RETURN_NULL();
    }
    RETURN_STR_COPY(MYLITE_MYSQLI_G(connect_error));
}

PHP_FUNCTION(mysqli_warning_count) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(link_zv);
    if (link->db != NULL) {
        link->warning_count = (zend_long)mylite_warning_count(link->db);
    }
    mylite_mysqli_update_link_props(link);
    RETURN_LONG(link->warning_count);
}

PHP_FUNCTION(mysqli_get_warnings) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_MYSQLI_LINK_P(link_zv)->db == NULL ||
        mylite_warning_count(Z_MYSQLI_LINK_P(link_zv)->db) == 0) {
        RETURN_FALSE;
    }

    object_init_ex(return_value, mylite_mysqli_warning_ce);
    mylite_mysqli_warning *warning = Z_MYSQLI_WARNING_P(return_value);
    ZVAL_COPY(&warning->link, link_zv);
    warning->index = 0;
    if (!mylite_mysqli_update_warning(warning)) {
        zval_ptr_dtor(return_value);
        RETURN_FALSE;
    }
}

PHP_METHOD(mysqli, get_warnings) {
    ZEND_PARSE_PARAMETERS_NONE();
    if (Z_MYSQLI_LINK_P(ZEND_THIS)->db == NULL ||
        mylite_warning_count(Z_MYSQLI_LINK_P(ZEND_THIS)->db) == 0) {
        RETURN_FALSE;
    }

    object_init_ex(return_value, mylite_mysqli_warning_ce);
    mylite_mysqli_warning *warning = Z_MYSQLI_WARNING_P(return_value);
    ZVAL_COPY(&warning->link, ZEND_THIS);
    warning->index = 0;
    if (!mylite_mysqli_update_warning(warning)) {
        zval_ptr_dtor(return_value);
        RETURN_FALSE;
    }
}

PHP_FUNCTION(mysqli_real_escape_string) {
    zval *link_zv;
    char *input;
    size_t input_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(input, input_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    zend_string *escaped = zend_string_safe_alloc(input_len, 2, 0, false);
    char *out = ZSTR_VAL(escaped);
    size_t out_len = 0;
    for (size_t i = 0; i < input_len; ++i) {
        const unsigned char ch = (unsigned char)input[i];
        switch (ch) {
        case '\0':
            out[out_len++] = '\\';
            out[out_len++] = '0';
            break;
        case '\n':
            out[out_len++] = '\\';
            out[out_len++] = 'n';
            break;
        case '\r':
            out[out_len++] = '\\';
            out[out_len++] = 'r';
            break;
        case '\\':
        case '\'':
        case '"':
            out[out_len++] = '\\';
            out[out_len++] = (char)ch;
            break;
        case 0x1A:
            out[out_len++] = '\\';
            out[out_len++] = 'Z';
            break;
        default:
            out[out_len++] = (char)ch;
            break;
        }
    }
    out[out_len] = '\0';
    ZSTR_LEN(escaped) = out_len;
    RETURN_STR(escaped);
}

PHP_METHOD(mysqli, real_escape_string) {
    char *input;
    size_t input_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(input, input_len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *escaped = zend_string_safe_alloc(input_len, 2, 0, false);
    char *out = ZSTR_VAL(escaped);
    size_t out_len = 0;
    for (size_t i = 0; i < input_len; ++i) {
        const unsigned char ch = (unsigned char)input[i];
        switch (ch) {
        case '\0':
            out[out_len++] = '\\';
            out[out_len++] = '0';
            break;
        case '\n':
            out[out_len++] = '\\';
            out[out_len++] = 'n';
            break;
        case '\r':
            out[out_len++] = '\\';
            out[out_len++] = 'r';
            break;
        case '\\':
        case '\'':
        case '"':
            out[out_len++] = '\\';
            out[out_len++] = (char)ch;
            break;
        case 0x1A:
            out[out_len++] = '\\';
            out[out_len++] = 'Z';
            break;
        default:
            out[out_len++] = (char)ch;
            break;
        }
    }
    out[out_len] = '\0';
    ZSTR_LEN(escaped) = out_len;
    RETURN_STR(escaped);
}

PHP_FUNCTION(mysqli_select_db) {
    zval *link_zv;
    char *database;
    size_t database_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(database, database_len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *quoted = mylite_mysqli_quote_identifier(database, database_len);
    zend_string *sql = strpprintf(0, "USE %s", ZSTR_VAL(quoted));
    zend_string_release(quoted);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(link_zv), sql);
    zend_string_release(sql);
    if (ok) {
        mylite_mysqli_link *link = Z_MYSQLI_LINK_P(link_zv);
        if (link->schema != NULL) {
            zend_string_release(link->schema);
        }
        link->schema = zend_string_init(database, database_len, false);
    }
    RETURN_BOOL(ok);
}

PHP_METHOD(mysqli, select_db) {
    char *database;
    size_t database_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(database, database_len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *quoted = mylite_mysqli_quote_identifier(database, database_len);
    zend_string *sql = strpprintf(0, "USE %s", ZSTR_VAL(quoted));
    zend_string_release(quoted);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(ZEND_THIS), sql);
    zend_string_release(sql);
    if (ok) {
        mylite_mysqli_link *link = Z_MYSQLI_LINK_P(ZEND_THIS);
        if (link->schema != NULL) {
            zend_string_release(link->schema);
        }
        link->schema = zend_string_init(database, database_len, false);
    }
    RETURN_BOOL(ok);
}

PHP_FUNCTION(mysqli_set_charset) {
    zval *link_zv;
    char *charset;
    size_t charset_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(charset, charset_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!mylite_mysqli_is_identifier_safe(charset, charset_len)) {
        RETURN_FALSE;
    }
    zend_string *sql = strpprintf(0, "SET NAMES '%s'", charset);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(link_zv), sql);
    zend_string_release(sql);
    if (ok) {
        mylite_mysqli_link *link = Z_MYSQLI_LINK_P(link_zv);
        if (link->charset != NULL) {
            zend_string_release(link->charset);
        }
        link->charset = zend_string_init(charset, charset_len, false);
    }
    RETURN_BOOL(ok);
}

PHP_METHOD(mysqli, set_charset) {
    char *charset;
    size_t charset_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(charset, charset_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!mylite_mysqli_is_identifier_safe(charset, charset_len)) {
        RETURN_FALSE;
    }
    zend_string *sql = strpprintf(0, "SET NAMES '%s'", charset);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(ZEND_THIS), sql);
    zend_string_release(sql);
    if (ok) {
        mylite_mysqli_link *link = Z_MYSQLI_LINK_P(ZEND_THIS);
        if (link->charset != NULL) {
            zend_string_release(link->charset);
        }
        link->charset = zend_string_init(charset, charset_len, false);
    }
    RETURN_BOOL(ok);
}

PHP_FUNCTION(mysqli_character_set_name) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_STR_COPY(Z_MYSQLI_LINK_P(link_zv)->charset);
}

PHP_METHOD(mysqli, character_set_name) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STR_COPY(Z_MYSQLI_LINK_P(ZEND_THIS)->charset);
}

PHP_FUNCTION(mysqli_get_charset) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    object_init(return_value);
    add_property_str(return_value, "charset", zend_string_copy(Z_MYSQLI_LINK_P(link_zv)->charset));
    add_property_string(return_value, "collation", "utf8mb4_general_ci");
    add_property_string(return_value, "dir", "");
    add_property_long(return_value, "min_length", 1);
    add_property_long(return_value, "max_length", 4);
    add_property_long(return_value, "number", 45);
    add_property_long(return_value, "state", 1);
    add_property_string(return_value, "comment", "MyLite charset");
}

PHP_METHOD(mysqli, get_charset) {
    ZEND_PARSE_PARAMETERS_NONE();
    object_init(return_value);
    add_property_str(
        return_value,
        "charset",
        zend_string_copy(Z_MYSQLI_LINK_P(ZEND_THIS)->charset)
    );
    add_property_string(return_value, "collation", "utf8mb4_general_ci");
    add_property_string(return_value, "dir", "");
    add_property_long(return_value, "min_length", 1);
    add_property_long(return_value, "max_length", 4);
    add_property_long(return_value, "number", 45);
    add_property_long(return_value, "state", 1);
    add_property_string(return_value, "comment", "MyLite charset");
}

PHP_FUNCTION(mysqli_autocommit) {
    zval *link_zv;
    bool enable;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *sql = zend_string_init(
        enable ? "SET autocommit=1" : "SET autocommit=0",
        enable ? sizeof("SET autocommit=1") - 1 : sizeof("SET autocommit=0") - 1,
        false
    );
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(link_zv), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_METHOD(mysqli, autocommit) {
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *sql = zend_string_init(
        enable ? "SET autocommit=1" : "SET autocommit=0",
        enable ? sizeof("SET autocommit=1") - 1 : sizeof("SET autocommit=0") - 1,
        false
    );
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(ZEND_THIS), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_FUNCTION(mysqli_begin_transaction) {
    zval *link_zv;
    zend_long flags = 0;
    char *name = NULL;
    size_t name_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 3)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(flags)
    Z_PARAM_STRING_OR_NULL(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)flags;
    (void)name;
    (void)name_len;
    zend_string *sql =
        zend_string_init("START TRANSACTION", sizeof("START TRANSACTION") - 1, false);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(link_zv), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_METHOD(mysqli, begin_transaction) {
    zend_long flags = 0;
    char *name = NULL;
    size_t name_len = 0;

    ZEND_PARSE_PARAMETERS_START(0, 2)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(flags)
    Z_PARAM_STRING_OR_NULL(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)flags;
    (void)name;
    (void)name_len;
    zend_string *sql =
        zend_string_init("START TRANSACTION", sizeof("START TRANSACTION") - 1, false);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(ZEND_THIS), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_FUNCTION(mysqli_commit) {
    zval *link_zv;
    zend_long flags = 0;
    char *name = NULL;
    size_t name_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 3)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(flags)
    Z_PARAM_STRING_OR_NULL(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)flags;
    (void)name;
    (void)name_len;
    zend_string *sql = zend_string_init("COMMIT", sizeof("COMMIT") - 1, false);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(link_zv), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_METHOD(mysqli, commit) {
    zend_long flags = 0;
    char *name = NULL;
    size_t name_len = 0;

    ZEND_PARSE_PARAMETERS_START(0, 2)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(flags)
    Z_PARAM_STRING_OR_NULL(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)flags;
    (void)name;
    (void)name_len;
    zend_string *sql = zend_string_init("COMMIT", sizeof("COMMIT") - 1, false);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(ZEND_THIS), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_FUNCTION(mysqli_rollback) {
    zval *link_zv;
    zend_long flags = 0;
    char *name = NULL;
    size_t name_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 3)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(flags)
    Z_PARAM_STRING_OR_NULL(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)flags;
    (void)name;
    (void)name_len;
    zend_string *sql = zend_string_init("ROLLBACK", sizeof("ROLLBACK") - 1, false);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(link_zv), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_METHOD(mysqli, rollback) {
    zend_long flags = 0;
    char *name = NULL;
    size_t name_len = 0;

    ZEND_PARSE_PARAMETERS_START(0, 2)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(flags)
    Z_PARAM_STRING_OR_NULL(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)flags;
    (void)name;
    (void)name_len;
    zend_string *sql = zend_string_init("ROLLBACK", sizeof("ROLLBACK") - 1, false);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(ZEND_THIS), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_FUNCTION(mysqli_savepoint) {
    zval *link_zv;
    char *name;
    size_t name_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *quoted = mylite_mysqli_quote_identifier(name, name_len);
    zend_string *sql = strpprintf(0, "SAVEPOINT %s", ZSTR_VAL(quoted));
    zend_string_release(quoted);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(link_zv), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_METHOD(mysqli, savepoint) {
    char *name;
    size_t name_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *quoted = mylite_mysqli_quote_identifier(name, name_len);
    zend_string *sql = strpprintf(0, "SAVEPOINT %s", ZSTR_VAL(quoted));
    zend_string_release(quoted);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(ZEND_THIS), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_FUNCTION(mysqli_release_savepoint) {
    zval *link_zv;
    char *name;
    size_t name_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *quoted = mylite_mysqli_quote_identifier(name, name_len);
    zend_string *sql = strpprintf(0, "RELEASE SAVEPOINT %s", ZSTR_VAL(quoted));
    zend_string_release(quoted);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(link_zv), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_METHOD(mysqli, release_savepoint) {
    char *name;
    size_t name_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *quoted = mylite_mysqli_quote_identifier(name, name_len);
    zend_string *sql = strpprintf(0, "RELEASE SAVEPOINT %s", ZSTR_VAL(quoted));
    zend_string_release(quoted);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(ZEND_THIS), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_FUNCTION(mysqli_options) {
    zval *link_zv;
    zend_long option;
    zval *value;

    ZEND_PARSE_PARAMETERS_START(3, 3)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_LONG(option)
    Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    if (option == MYLITE_MYSQLI_OPT_INT_AND_FLOAT_NATIVE) {
        mylite_mysqli_link *link = Z_MYSQLI_LINK_P(link_zv);
        link->int_and_float_native = zend_is_true(value);
    }
    RETURN_TRUE;
}

PHP_METHOD(mysqli, options) {
    zend_long option;
    zval *value;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_LONG(option)
    Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    if (option == MYLITE_MYSQLI_OPT_INT_AND_FLOAT_NATIVE) {
        Z_MYSQLI_LINK_P(ZEND_THIS)->int_and_float_native = zend_is_true(value);
    }
    RETURN_TRUE;
}

PHP_METHOD(mysqli, set_opt) {
    zend_long option;
    zval *value;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_LONG(option)
    Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    if (option == MYLITE_MYSQLI_OPT_INT_AND_FLOAT_NATIVE) {
        Z_MYSQLI_LINK_P(ZEND_THIS)->int_and_float_native = zend_is_true(value);
    }
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_report) {
    zend_long flags;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();

    MYLITE_MYSQLI_G(report_mode) = flags;
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_get_client_info) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(mylite_version());
}

PHP_METHOD(mysqli, get_client_info) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(mylite_version());
}

PHP_FUNCTION(mysqli_get_client_version) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(100100);
}

PHP_FUNCTION(mysqli_get_server_info) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    RETURN_STRING(MYLITE_MYSQLI_SERVER_INFO);
}

PHP_METHOD(mysqli, get_server_info) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(MYLITE_MYSQLI_SERVER_INFO);
}

PHP_FUNCTION(mysqli_get_server_version) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    RETURN_LONG(MYLITE_MYSQLI_SERVER_VERSION);
}

PHP_FUNCTION(mysqli_get_host_info) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    RETURN_STRING("MyLite embedded");
}

PHP_FUNCTION(mysqli_get_proto_info) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    RETURN_LONG(0);
}

PHP_FUNCTION(mysqli_info) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    RETURN_NULL();
}

PHP_FUNCTION(mysqli_stat) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    RETURN_STRING("MyLite embedded");
}

PHP_METHOD(mysqli, stat) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING("MyLite embedded");
}

PHP_FUNCTION(mysqli_ping) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(Z_MYSQLI_LINK_P(link_zv)->db != NULL);
}

PHP_METHOD(mysqli, ping) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(Z_MYSQLI_LINK_P(ZEND_THIS)->db != NULL);
}

PHP_FUNCTION(mysqli_thread_id) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    RETURN_LONG(0);
}

PHP_FUNCTION(mysqli_thread_safe) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_TRUE;
}

PHP_METHOD(mysqli, thread_safe) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_more_results) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    RETURN_FALSE;
}

PHP_METHOD(mysqli, more_results) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_FALSE;
}

PHP_FUNCTION(mysqli_next_result) {
    zval *link_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)link_zv;
    RETURN_FALSE;
}

PHP_METHOD(mysqli, next_result) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_FALSE;
}

PHP_FUNCTION(mysqli_multi_query) {
    zval *link_zv;
    char *query;
    size_t query_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(query, query_len)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(mylite_mysqli_real_query_link(Z_MYSQLI_LINK_P(link_zv), query, query_len));
}

PHP_METHOD(mysqli, multi_query) {
    char *query;
    size_t query_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(query, query_len)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(mylite_mysqli_real_query_link(Z_MYSQLI_LINK_P(ZEND_THIS), query, query_len));
}

PHP_FUNCTION(mysqli_change_user) {
    zval *link_zv;
    char *username;
    char *password;
    char *database = NULL;
    size_t username_len;
    size_t password_len;
    size_t database_len = 0;

    ZEND_PARSE_PARAMETERS_START(4, 4)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_STRING(username, username_len)
    Z_PARAM_STRING(password, password_len)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)username;
    (void)username_len;
    (void)password;
    (void)password_len;
    if (database == NULL) {
        RETURN_TRUE;
    }
    zend_string *quoted = mylite_mysqli_quote_identifier(database, database_len);
    zend_string *sql = strpprintf(0, "USE %s", ZSTR_VAL(quoted));
    zend_string_release(quoted);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(link_zv), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_METHOD(mysqli, change_user) {
    char *username;
    char *password;
    char *database = NULL;
    size_t username_len;
    size_t password_len;
    size_t database_len = 0;

    ZEND_PARSE_PARAMETERS_START(3, 3)
    Z_PARAM_STRING(username, username_len)
    Z_PARAM_STRING(password, password_len)
    Z_PARAM_STRING_OR_NULL(database, database_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)username;
    (void)username_len;
    (void)password;
    (void)password_len;
    if (database == NULL) {
        RETURN_TRUE;
    }
    zend_string *quoted = mylite_mysqli_quote_identifier(database, database_len);
    zend_string *sql = strpprintf(0, "USE %s", ZSTR_VAL(quoted));
    zend_string_release(quoted);
    bool ok = mylite_mysqli_execute_sql(Z_MYSQLI_LINK_P(ZEND_THIS), sql);
    zend_string_release(sql);
    RETURN_BOOL(ok);
}

PHP_FUNCTION(mysqli_dump_debug_info) {
    zval *link_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();
    (void)link_zv;
    RETURN_TRUE;
}

PHP_METHOD(mysqli, dump_debug_info) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_debug) {
    char *options;
    size_t options_len;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(options, options_len)
    ZEND_PARSE_PARAMETERS_END();
    (void)options;
    (void)options_len;
    RETURN_TRUE;
}

PHP_METHOD(mysqli, debug) {
    char *options;
    size_t options_len;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(options, options_len)
    ZEND_PARSE_PARAMETERS_END();
    (void)options;
    (void)options_len;
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_kill) {
    zval *link_zv;
    zend_long process_id;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_LONG(process_id)
    ZEND_PARSE_PARAMETERS_END();
    (void)link_zv;
    (void)process_id;
    RETURN_FALSE;
}

PHP_METHOD(mysqli, kill) {
    zend_long process_id;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(process_id)
    ZEND_PARSE_PARAMETERS_END();
    (void)process_id;
    RETURN_FALSE;
}

PHP_FUNCTION(mysqli_poll) {
    zval *read;
    zval *error;
    zval *reject;
    zend_long seconds;
    zend_long microseconds = 0;
    ZEND_PARSE_PARAMETERS_START(4, 5)
    Z_PARAM_ZVAL(read)
    Z_PARAM_ZVAL(error)
    Z_PARAM_ZVAL(reject)
    Z_PARAM_LONG(seconds)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(microseconds)
    ZEND_PARSE_PARAMETERS_END();
    (void)read;
    (void)error;
    (void)reject;
    (void)seconds;
    (void)microseconds;
    RETURN_FALSE;
}

PHP_METHOD(mysqli, poll) {
    zval *read;
    zval *error;
    zval *reject;
    zend_long seconds;
    zend_long microseconds = 0;
    ZEND_PARSE_PARAMETERS_START(4, 5)
    Z_PARAM_ZVAL(read)
    Z_PARAM_ZVAL(error)
    Z_PARAM_ZVAL(reject)
    Z_PARAM_LONG(seconds)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(microseconds)
    ZEND_PARSE_PARAMETERS_END();
    (void)read;
    (void)error;
    (void)reject;
    (void)seconds;
    (void)microseconds;
    RETURN_FALSE;
}

PHP_FUNCTION(mysqli_reap_async_query) {
    zval *link_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();
    (void)link_zv;
    RETURN_FALSE;
}

PHP_METHOD(mysqli, reap_async_query) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_FALSE;
}

PHP_FUNCTION(mysqli_ssl_set) {
    zval *link_zv;
    zval *args = NULL;
    uint32_t args_count = 0;
    ZEND_PARSE_PARAMETERS_START(1, 6)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_OPTIONAL
    Z_PARAM_VARIADIC('*', args, args_count)
    ZEND_PARSE_PARAMETERS_END();
    (void)link_zv;
    (void)args;
    (void)args_count;
    RETURN_TRUE;
}

PHP_METHOD(mysqli, ssl_set) {
    zval *args = NULL;
    uint32_t args_count = 0;
    ZEND_PARSE_PARAMETERS_START(0, 5)
    Z_PARAM_OPTIONAL
    Z_PARAM_VARIADIC('*', args, args_count)
    ZEND_PARSE_PARAMETERS_END();
    (void)args;
    (void)args_count;
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_refresh) {
    zval *link_zv;
    zend_long flags;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();
    (void)link_zv;
    (void)flags;
    RETURN_FALSE;
}

PHP_METHOD(mysqli, refresh) {
    zend_long flags;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();
    (void)flags;
    RETURN_FALSE;
}

PHP_FUNCTION(mysqli_get_connection_stats) {
    zval *link_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(link_zv, mylite_mysqli_link_ce)
    ZEND_PARSE_PARAMETERS_END();
    (void)link_zv;
    array_init(return_value);
}

PHP_METHOD(mysqli, get_connection_stats) {
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
}

PHP_FUNCTION(mysqli_get_client_stats) {
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
}

PHP_FUNCTION(mysqli_get_links_stats) {
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
}

PHP_FUNCTION(mysqli_stmt_affected_rows) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG(Z_MYSQLI_STMT_P(stmt_zv)->affected_rows);
}

PHP_FUNCTION(mysqli_stmt_insert_id) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG((zend_long)Z_MYSQLI_STMT_P(stmt_zv)->insert_id);
}

PHP_FUNCTION(mysqli_stmt_num_rows) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG(Z_MYSQLI_STMT_P(stmt_zv)->num_rows);
}

PHP_METHOD(mysqli_stmt, num_rows) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(Z_MYSQLI_STMT_P(ZEND_THIS)->num_rows);
}

PHP_FUNCTION(mysqli_stmt_param_count) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG(Z_MYSQLI_STMT_P(stmt_zv)->param_count);
}

PHP_FUNCTION(mysqli_stmt_field_count) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG(Z_MYSQLI_STMT_P(stmt_zv)->field_count);
}

PHP_FUNCTION(mysqli_stmt_errno) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG(Z_MYSQLI_STMT_P(stmt_zv)->errno_value);
}

PHP_FUNCTION(mysqli_stmt_error) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_STR_COPY(Z_MYSQLI_STMT_P(stmt_zv)->error);
}

PHP_FUNCTION(mysqli_stmt_sqlstate) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_STR_COPY(Z_MYSQLI_STMT_P(stmt_zv)->sqlstate);
}

PHP_FUNCTION(mysqli_stmt_error_list) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    array_init(return_value);
    if (stmt->errno_value != 0) {
        zval item;
        array_init(&item);
        add_assoc_long(&item, "errno", stmt->errno_value);
        add_assoc_str(&item, "sqlstate", zend_string_copy(stmt->sqlstate));
        add_assoc_str(&item, "error", zend_string_copy(stmt->error));
        add_next_index_zval(return_value, &item);
    }
}

PHP_FUNCTION(mysqli_stmt_close) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();

    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (stmt->stmt != NULL) {
        (void)mylite_finalize(stmt->stmt);
        stmt->stmt = NULL;
    }
    RETURN_TRUE;
}

PHP_METHOD(mysqli_stmt, close) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (stmt->stmt != NULL) {
        (void)mylite_finalize(stmt->stmt);
        stmt->stmt = NULL;
    }
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_stmt_free_result) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (!Z_ISUNDEF(stmt->result)) {
        zval_ptr_dtor(&stmt->result);
        ZVAL_UNDEF(&stmt->result);
    }
}

PHP_METHOD(mysqli_stmt, free_result) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (!Z_ISUNDEF(stmt->result)) {
        zval_ptr_dtor(&stmt->result);
        ZVAL_UNDEF(&stmt->result);
    }
}

PHP_FUNCTION(mysqli_stmt_store_result) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_BOOL(!Z_ISUNDEF(Z_MYSQLI_STMT_P(stmt_zv)->result));
}

PHP_METHOD(mysqli_stmt, store_result) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(!Z_ISUNDEF(Z_MYSQLI_STMT_P(ZEND_THIS)->result));
}

PHP_FUNCTION(mysqli_stmt_data_seek) {
    zval *stmt_zv;
    zend_long offset;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    Z_PARAM_LONG(offset)
    ZEND_PARSE_PARAMETERS_END();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (!Z_ISUNDEF(stmt->result)) {
        (void)mylite_mysqli_result_seek(Z_MYSQLI_RESULT_P(&stmt->result), offset);
    }
}

PHP_METHOD(mysqli_stmt, data_seek) {
    zend_long offset;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(offset)
    ZEND_PARSE_PARAMETERS_END();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (!Z_ISUNDEF(stmt->result)) {
        (void)mylite_mysqli_result_seek(Z_MYSQLI_RESULT_P(&stmt->result), offset);
    }
}

PHP_FUNCTION(mysqli_stmt_reset) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (stmt->stmt == NULL) {
        RETURN_FALSE;
    }
    int rc = mylite_reset(stmt->stmt);
    stmt->executed = false;
    RETURN_BOOL(rc == MYLITE_OK);
}

PHP_METHOD(mysqli_stmt, reset) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (stmt->stmt == NULL) {
        RETURN_FALSE;
    }
    int rc = mylite_reset(stmt->stmt);
    stmt->executed = false;
    RETURN_BOOL(rc == MYLITE_OK);
}

PHP_FUNCTION(mysqli_stmt_result_metadata) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (stmt->stmt == NULL || stmt->field_count == 0) {
        RETURN_FALSE;
    }
    mylite_mysqli_init_result_from_stmt(return_value, stmt->stmt, MYLITE_MYSQLI_STORE_RESULT);
    mylite_mysqli_update_result_props(Z_MYSQLI_RESULT_P(return_value));
}

PHP_METHOD(mysqli_stmt, result_metadata) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (stmt->stmt == NULL || stmt->field_count == 0) {
        RETURN_FALSE;
    }
    mylite_mysqli_init_result_from_stmt(return_value, stmt->stmt, MYLITE_MYSQLI_STORE_RESULT);
    mylite_mysqli_update_result_props(Z_MYSQLI_RESULT_P(return_value));
}

PHP_FUNCTION(mysqli_stmt_attr_get) {
    zval *stmt_zv;
    zend_long attribute;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    Z_PARAM_LONG(attribute)
    ZEND_PARSE_PARAMETERS_END();
    (void)stmt_zv;
    (void)attribute;
    RETURN_LONG(0);
}

PHP_METHOD(mysqli_stmt, attr_get) {
    zend_long attribute;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(attribute)
    ZEND_PARSE_PARAMETERS_END();
    (void)attribute;
    RETURN_LONG(0);
}

PHP_FUNCTION(mysqli_stmt_attr_set) {
    zval *stmt_zv;
    zend_long attribute;
    zend_long value;
    ZEND_PARSE_PARAMETERS_START(3, 3)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    Z_PARAM_LONG(attribute)
    Z_PARAM_LONG(value)
    ZEND_PARSE_PARAMETERS_END();
    (void)stmt_zv;
    (void)attribute;
    (void)value;
    RETURN_TRUE;
}

PHP_METHOD(mysqli_stmt, attr_set) {
    zend_long attribute;
    zend_long value;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_LONG(attribute)
    Z_PARAM_LONG(value)
    ZEND_PARSE_PARAMETERS_END();
    (void)attribute;
    (void)value;
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_stmt_send_long_data) {
    zval *stmt_zv;
    zend_long param_num;
    char *data;
    size_t data_len;
    ZEND_PARSE_PARAMETERS_START(3, 3)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    Z_PARAM_LONG(param_num)
    Z_PARAM_STRING(data, data_len)
    ZEND_PARSE_PARAMETERS_END();
    (void)stmt_zv;
    (void)param_num;
    (void)data;
    (void)data_len;
    RETURN_TRUE;
}

PHP_METHOD(mysqli_stmt, send_long_data) {
    zend_long param_num;
    char *data;
    size_t data_len;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_LONG(param_num)
    Z_PARAM_STRING(data, data_len)
    ZEND_PARSE_PARAMETERS_END();
    (void)param_num;
    (void)data;
    (void)data_len;
    RETURN_TRUE;
}

PHP_FUNCTION(mysqli_stmt_get_warnings) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(stmt_zv);
    if (Z_ISUNDEF(stmt->link)) {
        RETURN_FALSE;
    }
    zval rv;
    zval *link_zv = &stmt->link;
    (void)rv;
    if (Z_MYSQLI_LINK_P(link_zv)->db == NULL ||
        mylite_warning_count(Z_MYSQLI_LINK_P(link_zv)->db) == 0) {
        RETURN_FALSE;
    }
    object_init_ex(return_value, mylite_mysqli_warning_ce);
    mylite_mysqli_warning *warning = Z_MYSQLI_WARNING_P(return_value);
    ZVAL_COPY(&warning->link, link_zv);
    warning->index = 0;
    if (!mylite_mysqli_update_warning(warning)) {
        zval_ptr_dtor(return_value);
        RETURN_FALSE;
    }
}

PHP_METHOD(mysqli_stmt, get_warnings) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_stmt *stmt = Z_MYSQLI_STMT_P(ZEND_THIS);
    if (Z_ISUNDEF(stmt->link) || Z_MYSQLI_LINK_P(&stmt->link)->db == NULL ||
        mylite_warning_count(Z_MYSQLI_LINK_P(&stmt->link)->db) == 0) {
        RETURN_FALSE;
    }
    object_init_ex(return_value, mylite_mysqli_warning_ce);
    mylite_mysqli_warning *warning = Z_MYSQLI_WARNING_P(return_value);
    ZVAL_COPY(&warning->link, &stmt->link);
    warning->index = 0;
    if (!mylite_mysqli_update_warning(warning)) {
        zval_ptr_dtor(return_value);
        RETURN_FALSE;
    }
}

PHP_FUNCTION(mysqli_stmt_more_results) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    (void)stmt_zv;
    RETURN_FALSE;
}

PHP_METHOD(mysqli_stmt, more_results) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_FALSE;
}

PHP_FUNCTION(mysqli_stmt_next_result) {
    zval *stmt_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(stmt_zv, mylite_mysqli_stmt_ce)
    ZEND_PARSE_PARAMETERS_END();
    (void)stmt_zv;
    RETURN_FALSE;
}

PHP_METHOD(mysqli_stmt, next_result) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_FALSE;
}

PHP_METHOD(mysqli_warning, __construct) {
    ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(mysqli_warning, next) {
    ZEND_PARSE_PARAMETERS_NONE();
    mylite_mysqli_warning *warning = Z_MYSQLI_WARNING_P(ZEND_THIS);
    ++warning->index;
    RETURN_BOOL(mylite_mysqli_update_warning(warning));
}

PHP_METHOD(mysqli_sql_exception, getSqlState) {
    ZEND_PARSE_PARAMETERS_NONE();
    zval *sqlstate = zend_read_property(
        mylite_mysqli_exception_ce,
        Z_OBJ_P(ZEND_THIS),
        "sqlstate",
        sizeof("sqlstate") - 1,
        true,
        NULL
    );
    if (sqlstate == NULL || Z_TYPE_P(sqlstate) != IS_STRING) {
        RETURN_STRING("HY000");
    }
    RETURN_STR_COPY(Z_STR_P(sqlstate));
}

static bool mylite_mysqli_connect_link(
    mylite_mysqli_link *link,
    const char *hostname,
    size_t hostname_len,
    const char *database,
    size_t database_len,
    const char *socket_name,
    size_t socket_name_len
) {
    if (link->db != NULL) {
        (void)mylite_close(link->db);
        link->db = NULL;
        link->connected = false;
    }

    zend_string *filename = mylite_mysqli_resolve_filename(
        hostname,
        hostname_len,
        database,
        database_len,
        socket_name,
        socket_name_len
    );
    mylite_db *db = NULL;
    const int rc = mylite_open(ZSTR_VAL(filename), &db);
    if (rc != MYLITE_OK) {
        const char *message = db != NULL ? mylite_errmsg(db) : "MyLite open failed";
        mylite_mysqli_set_link_error(link, rc, "HY000", message);
        mylite_mysqli_set_global_connect_error(link->errno_value, ZSTR_VAL(link->error));
        if (db != NULL) {
            (void)mylite_close(db);
        }
        zend_string_release(filename);
        mylite_mysqli_maybe_report_error(link);
        return false;
    }

    link->db = db;
    link->connected = true;
    if (link->filename != NULL) {
        zend_string_release(link->filename);
    }
    link->filename = filename;
    mylite_mysqli_clear_link_error(link);
    mylite_mysqli_set_global_connect_error(0, NULL);

    if (database != NULL && database_len > 0 &&
        !mylite_mysqli_is_pathlike(database, database_len)) {
        zend_string *quoted = mylite_mysqli_quote_identifier(database, database_len);
        zend_string *create_sql =
            strpprintf(0, "CREATE DATABASE IF NOT EXISTS %s", ZSTR_VAL(quoted));
        bool ok = mylite_mysqli_execute_sql(link, create_sql);
        zend_string_release(create_sql);
        if (ok) {
            zend_string *use_sql = strpprintf(0, "USE %s", ZSTR_VAL(quoted));
            ok = mylite_mysqli_execute_sql(link, use_sql);
            zend_string_release(use_sql);
        }
        zend_string_release(quoted);
        if (!ok) {
            mylite_mysqli_maybe_report_error(link);
            return false;
        }
        if (link->schema != NULL) {
            zend_string_release(link->schema);
        }
        link->schema = zend_string_init(database, database_len, false);
    }

    mylite_mysqli_update_link_props(link);
    return true;
}

static bool mylite_mysqli_query_link(
    mylite_mysqli_link *link,
    const char *query,
    size_t query_len,
    zend_long result_mode,
    zval *return_value
) {
    if (link->db == NULL) {
        mylite_mysqli_set_link_error(link, 2006, "HY000", "MyLite mysqli link is not connected");
        mylite_mysqli_maybe_report_error(link);
        return false;
    }

    mylite_stmt *stmt = NULL;
    int rc = mylite_prepare(link->db, query, query_len, &stmt, NULL);
    if (rc != MYLITE_OK) {
        mylite_mysqli_set_link_error_from_db(link, rc);
        mylite_mysqli_maybe_report_error(link);
        return false;
    }

    zval result_zv;
    ZVAL_UNDEF(&result_zv);
    bool ok = mylite_mysqli_run_prepared(link, stmt, &result_zv, result_mode);
    (void)mylite_finalize(stmt);
    if (!ok) {
        mylite_mysqli_maybe_report_error(link);
        return false;
    }

    if (Z_ISUNDEF(result_zv)) {
        ZVAL_TRUE(return_value);
        return true;
    }

    ZVAL_COPY_VALUE(return_value, &result_zv);
    return true;
}

static bool mylite_mysqli_real_query_link(
    mylite_mysqli_link *link,
    const char *query,
    size_t query_len
) {
    zval result_zv;
    ZVAL_UNDEF(&result_zv);
    bool ok =
        mylite_mysqli_query_link(link, query, query_len, MYLITE_MYSQLI_STORE_RESULT, &result_zv);
    if (!ok) {
        return false;
    }

    if (!Z_ISUNDEF(link->pending_result)) {
        zval_ptr_dtor(&link->pending_result);
        ZVAL_UNDEF(&link->pending_result);
    }
    if (Z_TYPE(result_zv) == IS_OBJECT &&
        instanceof_function(Z_OBJCE(result_zv), mylite_mysqli_result_ce)) {
        ZVAL_COPY_VALUE(&link->pending_result, &result_zv);
    } else if (!Z_ISUNDEF(result_zv)) {
        zval_ptr_dtor(&result_zv);
    }
    return true;
}

static bool mylite_mysqli_prepare_stmt(
    mylite_mysqli_stmt *stmt,
    zval *link_zv,
    const char *query,
    size_t query_len
) {
    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(link_zv);
    if (link->db == NULL) {
        mylite_mysqli_set_link_error(link, 2006, "HY000", "MyLite mysqli link is not connected");
        mylite_mysqli_set_stmt_error_from_link(stmt);
        return false;
    }

    if (stmt->stmt != NULL) {
        (void)mylite_finalize(stmt->stmt);
        stmt->stmt = NULL;
    }
    if (!Z_ISUNDEF(stmt->result)) {
        zval_ptr_dtor(&stmt->result);
        ZVAL_UNDEF(&stmt->result);
    }

    mylite_stmt *native_stmt = NULL;
    int rc = mylite_prepare(link->db, query, query_len, &native_stmt, NULL);
    if (rc != MYLITE_OK) {
        mylite_mysqli_set_link_error_from_db(link, rc);
        mylite_mysqli_set_stmt_error_from_link(stmt);
        return false;
    }

    if (!Z_ISUNDEF(stmt->link)) {
        zval_ptr_dtor(&stmt->link);
    }
    ZVAL_COPY(&stmt->link, link_zv);
    if (stmt->query != NULL) {
        zend_string_release(stmt->query);
    }
    stmt->query = zend_string_init(query, query_len, false);
    stmt->stmt = native_stmt;
    stmt->param_count = (zend_long)mylite_bind_parameter_count(native_stmt);
    stmt->field_count = (zend_long)mylite_column_count(native_stmt);
    stmt->executed = false;
    mylite_mysqli_clear_stmt_error(stmt);
    mylite_mysqli_update_stmt_props(stmt);
    return true;
}

static bool mylite_mysqli_execute_stmt(mylite_mysqli_stmt *stmt, zval *params) {
    if (stmt->stmt == NULL || Z_ISUNDEF(stmt->link)) {
        mylite_mysqli_clear_stmt_error(stmt);
        stmt->errno_value = MYLITE_MISUSE;
        if (stmt->error != NULL) {
            zend_string_release(stmt->error);
        }
        stmt->error = zend_string_init(
            "mysqli statement is not prepared",
            sizeof("mysqli statement is not prepared") - 1,
            false
        );
        mylite_mysqli_update_stmt_props(stmt);
        return false;
    }

    if (stmt->executed) {
        int reset_rc = mylite_reset(stmt->stmt);
        if (reset_rc != MYLITE_OK) {
            mylite_mysqli_set_stmt_error_from_link(stmt);
            return false;
        }
        stmt->executed = false;
    }
    (void)mylite_clear_bindings(stmt->stmt);

    if (!mylite_mysqli_bind_stmt_params(stmt, params)) {
        return false;
    }

    if (!Z_ISUNDEF(stmt->result)) {
        zval_ptr_dtor(&stmt->result);
        ZVAL_UNDEF(&stmt->result);
    }

    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(&stmt->link);
    zval result_zv;
    ZVAL_UNDEF(&result_zv);
    bool ok = mylite_mysqli_run_prepared(link, stmt->stmt, &result_zv, MYLITE_MYSQLI_STORE_RESULT);
    stmt->executed = true;
    stmt->affected_rows = link->affected_rows;
    stmt->insert_id = link->insert_id;
    stmt->field_count = link->field_count;
    if (!ok) {
        mylite_mysqli_set_stmt_error_from_link(stmt);
        return false;
    }

    if (!Z_ISUNDEF(result_zv)) {
        stmt->num_rows =
            (zend_long)zend_hash_num_elements(Z_ARRVAL(Z_MYSQLI_RESULT_P(&result_zv)->rows));
        ZVAL_COPY_VALUE(&stmt->result, &result_zv);
    } else {
        stmt->num_rows = 0;
    }

    mylite_mysqli_clear_stmt_error(stmt);
    mylite_mysqli_update_stmt_props(stmt);
    return true;
}

static void mylite_mysqli_result_fetch_array(
    mylite_mysqli_result *result,
    zend_long mode,
    zval *return_value
) {
    if (result->current_row < 0 ||
        result->current_row >= (zend_long)zend_hash_num_elements(Z_ARRVAL(result->rows))) {
        ZVAL_NULL(return_value);
        return;
    }

    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), (zend_ulong)result->current_row);
    if (row == NULL || Z_TYPE_P(row) != IS_ARRAY) {
        ZVAL_FALSE(return_value);
        return;
    }

    array_init(return_value);
    for (zend_ulong i = 0; i < zend_hash_num_elements(Z_ARRVAL_P(row)); ++i) {
        zval *value = zend_hash_index_find(Z_ARRVAL_P(row), i);
        if (value == NULL) {
            continue;
        }
        if ((mode & MYLITE_MYSQLI_NUM) != 0) {
            zval copied;
            ZVAL_COPY(&copied, value);
            add_index_zval(return_value, i, &copied);
        }
        if ((mode & MYLITE_MYSQLI_ASSOC) != 0) {
            zval *field = zend_hash_index_find(Z_ARRVAL(result->fields), i);
            zval *name = field != NULL && Z_TYPE_P(field) == IS_OBJECT
                             ? zend_hash_str_find(Z_OBJPROP_P(field), "name", sizeof("name") - 1)
                             : NULL;
            if (name != NULL && Z_TYPE_P(name) == IS_STRING) {
                zval copied;
                ZVAL_COPY(&copied, value);
                zend_hash_update(Z_ARRVAL_P(return_value), Z_STR_P(name), &copied);
            }
        }
    }

    mylite_mysqli_update_result_lengths(result, row);
    ++result->current_row;
    mylite_mysqli_update_result_props(result);
}

static void mylite_mysqli_result_fetch_object(
    mylite_mysqli_result *result,
    const char *class_name,
    size_t class_name_len,
    zval *constructor_args,
    zval *return_value
) {
    (void)constructor_args;
    zval row;
    mylite_mysqli_result_fetch_array(result, MYLITE_MYSQLI_ASSOC, &row);
    if (Z_TYPE(row) != IS_ARRAY) {
        ZVAL_COPY_VALUE(return_value, &row);
        return;
    }

    zend_string *class_string = zend_string_init(class_name, class_name_len, false);
    zend_class_entry *ce = zend_lookup_class(class_string);
    zend_string_release(class_string);
    if (ce == NULL) {
        zval_ptr_dtor(&row);
        ZVAL_FALSE(return_value);
        return;
    }

    object_init_ex(return_value, ce);
    zend_string *key;
    zval *value;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL(row), key, value) {
        if (key != NULL) {
            zval copied;
            ZVAL_COPY(&copied, value);
            zend_update_property(ce, Z_OBJ_P(return_value), ZSTR_VAL(key), ZSTR_LEN(key), &copied);
            zval_ptr_dtor(&copied);
        }
    }
    ZEND_HASH_FOREACH_END();
    zval_ptr_dtor(&row);
}

static void mylite_mysqli_result_fetch_column(
    mylite_mysqli_result *result,
    zend_long column,
    zval *return_value
) {
    if (column < 0 || result->current_row < 0 ||
        result->current_row >= (zend_long)zend_hash_num_elements(Z_ARRVAL(result->rows))) {
        ZVAL_FALSE(return_value);
        return;
    }

    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), (zend_ulong)result->current_row);
    if (row == NULL || Z_TYPE_P(row) != IS_ARRAY) {
        ZVAL_FALSE(return_value);
        return;
    }

    zval *value = zend_hash_index_find(Z_ARRVAL_P(row), (zend_ulong)column);
    if (value == NULL) {
        ZVAL_FALSE(return_value);
        return;
    }

    ZVAL_COPY(return_value, value);
    mylite_mysqli_update_result_lengths(result, row);
    ++result->current_row;
    mylite_mysqli_update_result_props(result);
}

static bool mylite_mysqli_result_seek(mylite_mysqli_result *result, zend_long offset) {
    if (offset < 0 || offset >= (zend_long)zend_hash_num_elements(Z_ARRVAL(result->rows))) {
        return false;
    }
    result->current_row = offset;
    mylite_mysqli_update_result_props(result);
    return true;
}

static bool mylite_mysqli_field_seek_result(mylite_mysqli_result *result, zend_long index) {
    if (index < 0 || index >= (zend_long)zend_hash_num_elements(Z_ARRVAL(result->fields))) {
        return false;
    }
    result->current_field = index;
    mylite_mysqli_update_result_props(result);
    return true;
}

static bool mylite_mysqli_update_warning(mylite_mysqli_warning *warning) {
    if (Z_ISUNDEF(warning->link)) {
        return false;
    }
    mylite_mysqli_link *link = Z_MYSQLI_LINK_P(&warning->link);
    if (link->db == NULL) {
        return false;
    }

    mylite_warning_level level;
    unsigned code;
    const char *message;
    int rc = mylite_warning(link->db, warning->index, &level, &code, &message);
    if (rc != MYLITE_OK) {
        return false;
    }

    (void)level;
    zend_update_property_string(
        mylite_mysqli_warning_ce,
        &warning->std,
        "message",
        sizeof("message") - 1,
        message != NULL ? message : ""
    );
    zend_update_property_string(
        mylite_mysqli_warning_ce,
        &warning->std,
        "sqlstate",
        sizeof("sqlstate") - 1,
        "01000"
    );
    zend_update_property_long(
        mylite_mysqli_warning_ce,
        &warning->std,
        "errno",
        sizeof("errno") - 1,
        (zend_long)code
    );
    return true;
}

static bool mylite_mysqli_execute_sql(mylite_mysqli_link *link, zend_string *sql) {
    zval ignored;
    ZVAL_UNDEF(&ignored);
    bool ok = mylite_mysqli_query_link(
        link,
        ZSTR_VAL(sql),
        ZSTR_LEN(sql),
        MYLITE_MYSQLI_STORE_RESULT,
        &ignored
    );
    if (!Z_ISUNDEF(ignored)) {
        zval_ptr_dtor(&ignored);
    }
    return ok;
}

static bool mylite_mysqli_run_prepared(
    mylite_mysqli_link *link,
    mylite_stmt *stmt,
    zval *result_zv,
    zend_long result_mode
) {
    const unsigned column_count = mylite_column_count(stmt);
    if (column_count > 0) {
        mylite_mysqli_init_result_from_stmt(result_zv, stmt, result_mode);
    } else {
        ZVAL_UNDEF(result_zv);
    }

    int step_rc;
    while ((step_rc = mylite_step(stmt)) == MYLITE_ROW) {
        if (!Z_ISUNDEF(*result_zv)) {
            mylite_mysqli_append_current_row(
                Z_MYSQLI_RESULT_P(result_zv),
                stmt,
                link->int_and_float_native
            );
        }
    }

    if (step_rc != MYLITE_DONE) {
        if (!Z_ISUNDEF(*result_zv)) {
            zval_ptr_dtor(result_zv);
            ZVAL_UNDEF(result_zv);
        }
        mylite_mysqli_set_link_error_from_db(link, step_rc);
        return false;
    }

    link->field_count = (zend_long)column_count;
    link->warning_count = link->db != NULL ? (zend_long)mylite_warning_count(link->db) : 0;
    link->insert_id = link->db != NULL ? (zend_ulong)mylite_last_insert_id(link->db) : 0;
    link->affected_rows = column_count > 0 ? -1 : (zend_long)mylite_changes(link->db);
    mylite_mysqli_clear_link_error(link);
    if (!Z_ISUNDEF(*result_zv)) {
        mylite_mysqli_update_result_props(Z_MYSQLI_RESULT_P(result_zv));
    }
    mylite_mysqli_update_link_props(link);
    return true;
}

static bool mylite_mysqli_bind_stmt_params(mylite_mysqli_stmt *stmt, zval *params) {
    if (params != NULL) {
        unsigned index = 1;
        zval *value;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(params), value) {
            if (!mylite_mysqli_bind_one(stmt->stmt, index, value, '\0')) {
                mylite_mysqli_set_stmt_error_from_link(stmt);
                return false;
            }
            ++index;
        }
        ZEND_HASH_FOREACH_END();
        return true;
    }

    if (Z_ISUNDEF(stmt->bound_params)) {
        return stmt->param_count == 0;
    }

    unsigned index = 1;
    zval *value;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(stmt->bound_params), value) {
        const char type = stmt->types != NULL && index <= ZSTR_LEN(stmt->types)
                              ? ZSTR_VAL(stmt->types)[index - 1]
                              : '\0';
        if (!mylite_mysqli_bind_one(stmt->stmt, index, value, type)) {
            mylite_mysqli_set_stmt_error_from_link(stmt);
            return false;
        }
        ++index;
    }
    ZEND_HASH_FOREACH_END();
    return true;
}

static bool mylite_mysqli_bind_one(mylite_stmt *stmt, unsigned index, zval *value, char type) {
    zval tmp;
    ZVAL_DEREF(value);
    if (Z_TYPE_P(value) == IS_NULL) {
        return mylite_bind_null(stmt, index) == MYLITE_OK;
    }

    switch (type) {
    case 'i':
        ZVAL_COPY(&tmp, value);
        convert_to_long(&tmp);
        const zend_long long_value = Z_LVAL(tmp);
        zval_ptr_dtor(&tmp);
        return mylite_bind_int64(stmt, index, (long long)long_value) == MYLITE_OK;
    case 'd':
        ZVAL_COPY(&tmp, value);
        convert_to_double(&tmp);
        const double double_value = Z_DVAL(tmp);
        zval_ptr_dtor(&tmp);
        return mylite_bind_double(stmt, index, double_value) == MYLITE_OK;
    case 'b':
        ZVAL_COPY(&tmp, value);
        convert_to_string(&tmp);
        const bool blob_ok =
            mylite_bind_blob(stmt, index, Z_STRVAL(tmp), Z_STRLEN(tmp), MYLITE_TRANSIENT) ==
            MYLITE_OK;
        zval_ptr_dtor(&tmp);
        return blob_ok;
    case 's':
    default:
        if (type == '\0' && Z_TYPE_P(value) == IS_LONG) {
            return mylite_bind_int64(stmt, index, (long long)Z_LVAL_P(value)) == MYLITE_OK;
        }
        if (type == '\0' && Z_TYPE_P(value) == IS_DOUBLE) {
            return mylite_bind_double(stmt, index, Z_DVAL_P(value)) == MYLITE_OK;
        }
        ZVAL_COPY(&tmp, value);
        convert_to_string(&tmp);
        const bool text_ok =
            mylite_bind_text(stmt, index, Z_STRVAL(tmp), Z_STRLEN(tmp), MYLITE_TRANSIENT) ==
            MYLITE_OK;
        zval_ptr_dtor(&tmp);
        return text_ok;
    }
}

static void mylite_mysqli_column_to_zval(
    mylite_stmt *stmt,
    unsigned column,
    bool native_numbers,
    zval *return_value
) {
    switch (mylite_column_type(stmt, column)) {
    case MYLITE_TYPE_NULL:
        ZVAL_NULL(return_value);
        return;
    case MYLITE_TYPE_INT64:
        if (native_numbers) {
            ZVAL_LONG(return_value, (zend_long)mylite_column_int64(stmt, column));
        } else {
            ZVAL_STR(return_value, strpprintf(0, "%lld", mylite_column_int64(stmt, column)));
        }
        return;
    case MYLITE_TYPE_UINT64:
        if (native_numbers &&
            mylite_column_uint64(stmt, column) <= (unsigned long long)ZEND_LONG_MAX) {
            ZVAL_LONG(return_value, (zend_long)mylite_column_uint64(stmt, column));
        } else {
            ZVAL_STR(return_value, strpprintf(0, "%llu", mylite_column_uint64(stmt, column)));
        }
        return;
    case MYLITE_TYPE_DOUBLE:
        if (native_numbers) {
            ZVAL_DOUBLE(return_value, mylite_column_double(stmt, column));
        } else {
            ZVAL_STR(return_value, strpprintf(0, "%.17G", mylite_column_double(stmt, column)));
        }
        return;
    case MYLITE_TYPE_TEXT: {
        const char *text = mylite_column_text(stmt, column);
        const size_t bytes = mylite_column_bytes(stmt, column);
        ZVAL_STRINGL(return_value, text != NULL ? text : "", bytes);
        return;
    }
    case MYLITE_TYPE_BLOB: {
        const void *blob = mylite_column_blob(stmt, column);
        const size_t bytes = mylite_column_bytes(stmt, column);
        ZVAL_STRINGL(return_value, blob != NULL ? (const char *)blob : "", bytes);
        return;
    }
    }

    ZVAL_NULL(return_value);
}

static void mylite_mysqli_init_result_from_stmt(
    zval *result_zv,
    mylite_stmt *stmt,
    zend_long result_mode
) {
    object_init_ex(result_zv, mylite_mysqli_result_ce);
    mylite_mysqli_result *result = Z_MYSQLI_RESULT_P(result_zv);
    result->type = result_mode;

    const unsigned column_count = mylite_column_count(stmt);
    for (unsigned i = 0; i < column_count; ++i) {
        zval field;
        object_init(&field);
        const char *name = mylite_column_name(stmt, i);
        const char *orgname = mylite_column_origin_name(stmt, i);
        const char *table = mylite_column_table_name(stmt, i);
        const char *orgtable = mylite_column_origin_table_name(stmt, i);
        const char *db = mylite_column_database_name(stmt, i);
        add_property_string(&field, "name", name != NULL ? name : "");
        add_property_string(&field, "orgname", orgname != NULL ? orgname : "");
        add_property_string(&field, "table", table != NULL ? table : "");
        add_property_string(&field, "orgtable", orgtable != NULL ? orgtable : "");
        add_property_string(&field, "def", "");
        add_property_string(&field, "db", db != NULL ? db : "");
        add_property_string(&field, "catalog", "def");
        add_property_long(&field, "max_length", (zend_long)mylite_column_max_length(stmt, i));
        add_property_long(&field, "length", (zend_long)mylite_column_length(stmt, i));
        add_property_long(&field, "charsetnr", (zend_long)mylite_column_charset(stmt, i));
        add_property_long(&field, "flags", (zend_long)mylite_column_flags(stmt, i));
        add_property_long(&field, "type", (zend_long)mylite_column_mariadb_type(stmt, i));
        add_property_long(&field, "decimals", (zend_long)mylite_column_decimals(stmt, i));
        add_index_zval(&result->fields, i, &field);
    }
}

static void mylite_mysqli_append_current_row(
    mylite_mysqli_result *result,
    mylite_stmt *stmt,
    bool native_numbers
) {
    zval row;
    const unsigned column_count = mylite_column_count(stmt);
    array_init_size(&row, column_count);
    for (unsigned i = 0; i < column_count; ++i) {
        zval value;
        mylite_mysqli_column_to_zval(stmt, i, native_numbers, &value);
        add_index_zval(&row, i, &value);
    }
    add_next_index_zval(&result->rows, &row);
}

static void mylite_mysqli_return_field(zval *field, zval *return_value) {
    if (field == NULL || Z_TYPE_P(field) != IS_OBJECT) {
        ZVAL_FALSE(return_value);
        return;
    }

    object_init(return_value);
    zend_string *key;
    zval *value;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_OBJPROP_P(field), key, value) {
        if (key != NULL) {
            zval copied;
            ZVAL_COPY(&copied, value);
            zend_update_property(
                Z_OBJCE_P(return_value),
                Z_OBJ_P(return_value),
                ZSTR_VAL(key),
                ZSTR_LEN(key),
                &copied
            );
            zval_ptr_dtor(&copied);
        }
    }
    ZEND_HASH_FOREACH_END();
}

static void mylite_mysqli_return_field_at(
    mylite_mysqli_result *result,
    zend_ulong index,
    zval *return_value
) {
    zval *field = zend_hash_index_find(Z_ARRVAL(result->fields), index);
    mylite_mysqli_return_field(field, return_value);
}

static void mylite_mysqli_update_link_props(mylite_mysqli_link *link) {
    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "affected_rows",
        sizeof("affected_rows") - 1,
        link->affected_rows
    );
    zend_update_property_string(
        mylite_mysqli_link_ce,
        &link->std,
        "client_info",
        sizeof("client_info") - 1,
        mylite_version()
    );
    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "client_version",
        sizeof("client_version") - 1,
        100100
    );
    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "connect_errno",
        sizeof("connect_errno") - 1,
        MYLITE_MYSQLI_G(connect_errno)
    );
    mylite_mysqli_string_or_null_property(
        mylite_mysqli_link_ce,
        &link->std,
        "connect_error",
        MYLITE_MYSQLI_G(connect_error) != NULL ? ZSTR_VAL(MYLITE_MYSQLI_G(connect_error)) : NULL
    );
    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "errno",
        sizeof("errno") - 1,
        link->errno_value
    );
    zend_update_property_str(
        mylite_mysqli_link_ce,
        &link->std,
        "error",
        sizeof("error") - 1,
        link->error
    );

    zval error_list;
    array_init(&error_list);
    if (link->errno_value != 0) {
        zval item;
        array_init(&item);
        add_assoc_long(&item, "errno", link->errno_value);
        add_assoc_str(&item, "sqlstate", zend_string_copy(link->sqlstate));
        add_assoc_str(&item, "error", zend_string_copy(link->error));
        add_next_index_zval(&error_list, &item);
    }
    zend_update_property(
        mylite_mysqli_link_ce,
        &link->std,
        "error_list",
        sizeof("error_list") - 1,
        &error_list
    );
    zval_ptr_dtor(&error_list);

    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "field_count",
        sizeof("field_count") - 1,
        link->field_count
    );
    zend_update_property_string(
        mylite_mysqli_link_ce,
        &link->std,
        "host_info",
        sizeof("host_info") - 1,
        "MyLite embedded"
    );
    mylite_mysqli_string_or_null_property(mylite_mysqli_link_ce, &link->std, "info", NULL);
    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "insert_id",
        sizeof("insert_id") - 1,
        (zend_long)link->insert_id
    );
    zend_update_property_string(
        mylite_mysqli_link_ce,
        &link->std,
        "server_info",
        sizeof("server_info") - 1,
        MYLITE_MYSQLI_SERVER_INFO
    );
    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "server_version",
        sizeof("server_version") - 1,
        MYLITE_MYSQLI_SERVER_VERSION
    );
    zend_update_property_str(
        mylite_mysqli_link_ce,
        &link->std,
        "sqlstate",
        sizeof("sqlstate") - 1,
        link->sqlstate
    );
    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "protocol_version",
        sizeof("protocol_version") - 1,
        0
    );
    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "thread_id",
        sizeof("thread_id") - 1,
        0
    );
    zend_update_property_long(
        mylite_mysqli_link_ce,
        &link->std,
        "warning_count",
        sizeof("warning_count") - 1,
        link->warning_count
    );
}

static void mylite_mysqli_set_link_error(
    mylite_mysqli_link *link,
    zend_long errno_value,
    const char *sqlstate,
    const char *message
) {
    link->errno_value = errno_value;
    if (link->error != NULL) {
        zend_string_release(link->error);
    }
    if (link->sqlstate != NULL) {
        zend_string_release(link->sqlstate);
    }
    link->error = zend_string_init(
        message != NULL ? message : "",
        message != NULL ? strlen(message) : 0,
        false
    );
    link->sqlstate = zend_string_init(
        sqlstate != NULL ? sqlstate : "HY000",
        sqlstate != NULL ? strlen(sqlstate) : 5,
        false
    );
    mylite_mysqli_update_link_props(link);
}

static void mylite_mysqli_set_link_error_from_db(mylite_mysqli_link *link, int result) {
    const unsigned maria_errno = link->db != NULL ? mylite_mariadb_errno(link->db) : 0;
    const char *sqlstate = link->db != NULL ? mylite_sqlstate(link->db) : "HY000";
    const char *message = link->db != NULL ? mylite_errmsg(link->db) : "MyLite error";
    mylite_mysqli_set_link_error(
        link,
        maria_errno != 0 ? (zend_long)maria_errno : (zend_long)result,
        sqlstate != NULL ? sqlstate : "HY000",
        message != NULL ? message : "MyLite error"
    );
}

static void mylite_mysqli_clear_link_error(mylite_mysqli_link *link) {
    mylite_mysqli_set_link_error(link, 0, "00000", "");
}

static void mylite_mysqli_update_stmt_props(mylite_mysqli_stmt *stmt) {
    zend_update_property_long(
        mylite_mysqli_stmt_ce,
        &stmt->std,
        "affected_rows",
        sizeof("affected_rows") - 1,
        stmt->affected_rows
    );
    zend_update_property_long(
        mylite_mysqli_stmt_ce,
        &stmt->std,
        "insert_id",
        sizeof("insert_id") - 1,
        (zend_long)stmt->insert_id
    );
    zend_update_property_long(
        mylite_mysqli_stmt_ce,
        &stmt->std,
        "num_rows",
        sizeof("num_rows") - 1,
        stmt->num_rows
    );
    zend_update_property_long(
        mylite_mysqli_stmt_ce,
        &stmt->std,
        "param_count",
        sizeof("param_count") - 1,
        stmt->param_count
    );
    zend_update_property_long(
        mylite_mysqli_stmt_ce,
        &stmt->std,
        "field_count",
        sizeof("field_count") - 1,
        stmt->field_count
    );
    zend_update_property_long(
        mylite_mysqli_stmt_ce,
        &stmt->std,
        "errno",
        sizeof("errno") - 1,
        stmt->errno_value
    );
    zend_update_property_str(
        mylite_mysqli_stmt_ce,
        &stmt->std,
        "error",
        sizeof("error") - 1,
        stmt->error
    );

    zval error_list;
    array_init(&error_list);
    if (stmt->errno_value != 0) {
        zval item;
        array_init(&item);
        add_assoc_long(&item, "errno", stmt->errno_value);
        add_assoc_str(&item, "sqlstate", zend_string_copy(stmt->sqlstate));
        add_assoc_str(&item, "error", zend_string_copy(stmt->error));
        add_next_index_zval(&error_list, &item);
    }
    zend_update_property(
        mylite_mysqli_stmt_ce,
        &stmt->std,
        "error_list",
        sizeof("error_list") - 1,
        &error_list
    );
    zval_ptr_dtor(&error_list);

    zend_update_property_str(
        mylite_mysqli_stmt_ce,
        &stmt->std,
        "sqlstate",
        sizeof("sqlstate") - 1,
        stmt->sqlstate
    );
    zend_update_property_long(mylite_mysqli_stmt_ce, &stmt->std, "id", sizeof("id") - 1, 0);
}

static void mylite_mysqli_set_stmt_error_from_link(mylite_mysqli_stmt *stmt) {
    if (!Z_ISUNDEF(stmt->link)) {
        mylite_mysqli_link *link = Z_MYSQLI_LINK_P(&stmt->link);
        stmt->errno_value = link->errno_value;
        if (stmt->error != NULL) {
            zend_string_release(stmt->error);
        }
        if (stmt->sqlstate != NULL) {
            zend_string_release(stmt->sqlstate);
        }
        stmt->error = zend_string_copy(link->error);
        stmt->sqlstate = zend_string_copy(link->sqlstate);
        mylite_mysqli_update_stmt_props(stmt);
    }
}

static void mylite_mysqli_clear_stmt_error(mylite_mysqli_stmt *stmt) {
    stmt->errno_value = 0;
    if (stmt->error != NULL) {
        zend_string_release(stmt->error);
    }
    if (stmt->sqlstate != NULL) {
        zend_string_release(stmt->sqlstate);
    }
    stmt->error = zend_string_init("", 0, false);
    stmt->sqlstate = zend_string_init("00000", sizeof("00000") - 1, false);
    mylite_mysqli_update_stmt_props(stmt);
}

static void mylite_mysqli_update_result_props(mylite_mysqli_result *result) {
    zend_update_property_long(
        mylite_mysqli_result_ce,
        &result->std,
        "current_field",
        sizeof("current_field") - 1,
        result->current_field
    );
    zend_update_property_long(
        mylite_mysqli_result_ce,
        &result->std,
        "field_count",
        sizeof("field_count") - 1,
        (zend_long)zend_hash_num_elements(Z_ARRVAL(result->fields))
    );
    if (Z_TYPE(result->lengths) == IS_ARRAY) {
        zend_update_property(
            mylite_mysqli_result_ce,
            &result->std,
            "lengths",
            sizeof("lengths") - 1,
            &result->lengths
        );
    } else {
        zval null_value;
        ZVAL_NULL(&null_value);
        zend_update_property(
            mylite_mysqli_result_ce,
            &result->std,
            "lengths",
            sizeof("lengths") - 1,
            &null_value
        );
    }
    zend_update_property_long(
        mylite_mysqli_result_ce,
        &result->std,
        "num_rows",
        sizeof("num_rows") - 1,
        (zend_long)zend_hash_num_elements(Z_ARRVAL(result->rows))
    );
    zend_update_property_long(
        mylite_mysqli_result_ce,
        &result->std,
        "type",
        sizeof("type") - 1,
        result->type
    );
}

static void mylite_mysqli_update_result_lengths(mylite_mysqli_result *result, zval *row) {
    if (Z_TYPE(result->lengths) != IS_UNDEF) {
        zval_ptr_dtor(&result->lengths);
    }
    array_init(&result->lengths);
    for (zend_ulong i = 0; i < zend_hash_num_elements(Z_ARRVAL_P(row)); ++i) {
        zval *value = zend_hash_index_find(Z_ARRVAL_P(row), i);
        if (value == NULL || Z_TYPE_P(value) == IS_NULL) {
            add_index_long(&result->lengths, i, 0);
            continue;
        }
        zval tmp;
        ZVAL_COPY(&tmp, value);
        convert_to_string(&tmp);
        add_index_long(&result->lengths, i, (zend_long)Z_STRLEN(tmp));
        zval_ptr_dtor(&tmp);
    }
    mylite_mysqli_update_result_props(result);
}

static zend_string *mylite_mysqli_resolve_filename(
    const char *hostname,
    size_t hostname_len,
    const char *database,
    size_t database_len,
    const char *socket_name,
    size_t socket_name_len
) {
    const char *env_path = getenv("MYLITE_DATABASE_PATH");
    if (env_path != NULL && env_path[0] != '\0') {
        return zend_string_init(env_path, strlen(env_path), false);
    }
    if (socket_name != NULL && socket_name_len > 0 &&
        mylite_mysqli_is_pathlike(socket_name, socket_name_len)) {
        return zend_string_init(socket_name, socket_name_len, false);
    }
    if (hostname != NULL && hostname_len > 0) {
        if (hostname_len > 2 && hostname[0] == 'p' && hostname[1] == ':') {
            hostname += 2;
            hostname_len -= 2;
        }
        if (hostname_len > sizeof("mylite:") - 1 &&
            memcmp(hostname, "mylite:", sizeof("mylite:") - 1) == 0) {
            return zend_string_init(
                hostname + sizeof("mylite:") - 1,
                hostname_len - (sizeof("mylite:") - 1),
                false
            );
        }
        if (mylite_mysqli_is_pathlike(hostname, hostname_len)) {
            return zend_string_init(hostname, hostname_len, false);
        }
    }
    if (database != NULL && database_len > 0) {
        if (mylite_mysqli_is_pathlike(database, database_len)) {
            return zend_string_init(database, database_len, false);
        }
        return strpprintf(0, "%.*s.mylite", (int)database_len, database);
    }
    return zend_string_init(":memory:", sizeof(":memory:") - 1, false);
}

static zend_string *mylite_mysqli_quote_identifier(const char *value, size_t value_len) {
    smart_str sql = {0};
    smart_str_appendc(&sql, '`');
    for (size_t i = 0; i < value_len; ++i) {
        if (value[i] == '`') {
            smart_str_appendc(&sql, '`');
        }
        smart_str_appendc(&sql, value[i]);
    }
    smart_str_appendc(&sql, '`');
    smart_str_0(&sql);
    return sql.s;
}

static void mylite_mysqli_maybe_report_error(mylite_mysqli_link *link) {
    if (link->errno_value == 0 ||
        (MYLITE_MYSQLI_G(report_mode) & MYLITE_MYSQLI_REPORT_ERROR) == 0) {
        return;
    }
    zend_object *exception = zend_throw_exception(
        mylite_mysqli_exception_ce,
        ZSTR_VAL(link->error),
        (zend_long)link->errno_value
    );
    if (exception != NULL) {
        zend_update_property_str(
            mylite_mysqli_exception_ce,
            exception,
            "sqlstate",
            sizeof("sqlstate") - 1,
            link->sqlstate
        );
    }
}

static void mylite_mysqli_set_global_connect_error(zend_long errno_value, const char *message) {
    MYLITE_MYSQLI_G(connect_errno) = errno_value;
    if (MYLITE_MYSQLI_G(connect_error) != NULL) {
        zend_string_release(MYLITE_MYSQLI_G(connect_error));
        MYLITE_MYSQLI_G(connect_error) = NULL;
    }
    if (message != NULL) {
        MYLITE_MYSQLI_G(connect_error) = zend_string_init(message, strlen(message), false);
    }
}

static bool mylite_mysqli_is_identifier_safe(const char *value, size_t value_len) {
    if (value == NULL || value_len == 0) {
        return false;
    }
    for (size_t i = 0; i < value_len; ++i) {
        const char ch = value[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static bool mylite_mysqli_is_pathlike(const char *value, size_t value_len) {
    if (value == NULL || value_len == 0) {
        return false;
    }
    if (value_len == sizeof(":memory:") - 1 && memcmp(value, ":memory:", value_len) == 0) {
        return true;
    }
    if (mylite_mysqli_has_suffix(value, value_len, ".mylite")) {
        return true;
    }
    return memchr(value, '/', value_len) != NULL || memchr(value, '\\', value_len) != NULL;
}

static bool mylite_mysqli_has_suffix(const char *value, size_t value_len, const char *suffix) {
    const size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len &&
           memcmp(value + value_len - suffix_len, suffix, suffix_len) == 0;
}

static void mylite_mysqli_string_or_null_property(
    zend_class_entry *ce,
    zend_object *object,
    const char *name,
    const char *value
) {
    if (value == NULL) {
        zval null_value;
        ZVAL_NULL(&null_value);
        zend_update_property(ce, object, name, strlen(name), &null_value);
    } else {
        zend_update_property_string(ce, object, name, strlen(name), value);
    }
}

static zend_object *mylite_mysqli_link_create_object(zend_class_entry *class_type) {
    mylite_mysqli_link *link = zend_object_alloc(sizeof(mylite_mysqli_link), class_type);
    zend_object_std_init(&link->std, class_type);
    object_properties_init(&link->std, class_type);
    link->std.handlers = &mylite_mysqli_link_handlers;
    link->db = NULL;
    link->filename = NULL;
    link->schema = NULL;
    link->charset = zend_string_init("utf8mb4", sizeof("utf8mb4") - 1, false);
    link->errno_value = 0;
    link->error = zend_string_init("", 0, false);
    link->sqlstate = zend_string_init("00000", sizeof("00000") - 1, false);
    link->affected_rows = 0;
    link->insert_id = 0;
    link->field_count = 0;
    link->warning_count = 0;
    link->connected = false;
    link->int_and_float_native = false;
    ZVAL_UNDEF(&link->pending_result);
    return &link->std;
}

static void mylite_mysqli_link_free_obj(zend_object *object) {
    mylite_mysqli_link *link = mylite_mysqli_link_from_obj(object);
    if (link->db != NULL) {
        (void)mylite_close(link->db);
        link->db = NULL;
    }
    if (link->filename != NULL) {
        zend_string_release(link->filename);
    }
    if (link->schema != NULL) {
        zend_string_release(link->schema);
    }
    if (link->charset != NULL) {
        zend_string_release(link->charset);
    }
    if (link->error != NULL) {
        zend_string_release(link->error);
    }
    if (link->sqlstate != NULL) {
        zend_string_release(link->sqlstate);
    }
    if (!Z_ISUNDEF(link->pending_result)) {
        zval_ptr_dtor(&link->pending_result);
    }
    zend_object_std_dtor(&link->std);
}

static zend_object *mylite_mysqli_result_create_object(zend_class_entry *class_type) {
    mylite_mysqli_result *result = zend_object_alloc(sizeof(mylite_mysqli_result), class_type);
    zend_object_std_init(&result->std, class_type);
    object_properties_init(&result->std, class_type);
    result->std.handlers = &mylite_mysqli_result_handlers;
    array_init(&result->rows);
    array_init(&result->fields);
    ZVAL_UNDEF(&result->lengths);
    result->current_row = 0;
    result->current_field = 0;
    result->type = MYLITE_MYSQLI_STORE_RESULT;
    return &result->std;
}

static void mylite_mysqli_result_free_obj(zend_object *object) {
    mylite_mysqli_result *result = mylite_mysqli_result_from_obj(object);
    zval_ptr_dtor(&result->rows);
    zval_ptr_dtor(&result->fields);
    if (!Z_ISUNDEF(result->lengths)) {
        zval_ptr_dtor(&result->lengths);
    }
    zend_object_std_dtor(&result->std);
}

static zend_object *mylite_mysqli_stmt_create_object(zend_class_entry *class_type) {
    mylite_mysqli_stmt *stmt = zend_object_alloc(sizeof(mylite_mysqli_stmt), class_type);
    zend_object_std_init(&stmt->std, class_type);
    object_properties_init(&stmt->std, class_type);
    stmt->std.handlers = &mylite_mysqli_stmt_handlers;
    ZVAL_UNDEF(&stmt->link);
    stmt->stmt = NULL;
    stmt->query = NULL;
    ZVAL_UNDEF(&stmt->bound_params);
    ZVAL_UNDEF(&stmt->bound_results);
    ZVAL_UNDEF(&stmt->result);
    stmt->types = NULL;
    stmt->errno_value = 0;
    stmt->error = zend_string_init("", 0, false);
    stmt->sqlstate = zend_string_init("00000", sizeof("00000") - 1, false);
    stmt->affected_rows = 0;
    stmt->insert_id = 0;
    stmt->num_rows = 0;
    stmt->param_count = 0;
    stmt->field_count = 0;
    stmt->executed = false;
    return &stmt->std;
}

static void mylite_mysqli_stmt_free_obj(zend_object *object) {
    mylite_mysqli_stmt *stmt = mylite_mysqli_stmt_from_obj(object);
    if (stmt->stmt != NULL) {
        (void)mylite_finalize(stmt->stmt);
    }
    if (!Z_ISUNDEF(stmt->link)) {
        zval_ptr_dtor(&stmt->link);
    }
    if (stmt->query != NULL) {
        zend_string_release(stmt->query);
    }
    if (!Z_ISUNDEF(stmt->bound_params)) {
        zval_ptr_dtor(&stmt->bound_params);
    }
    if (!Z_ISUNDEF(stmt->bound_results)) {
        zval_ptr_dtor(&stmt->bound_results);
    }
    if (!Z_ISUNDEF(stmt->result)) {
        zval_ptr_dtor(&stmt->result);
    }
    if (stmt->types != NULL) {
        zend_string_release(stmt->types);
    }
    if (stmt->error != NULL) {
        zend_string_release(stmt->error);
    }
    if (stmt->sqlstate != NULL) {
        zend_string_release(stmt->sqlstate);
    }
    zend_object_std_dtor(&stmt->std);
}

static zend_object *mylite_mysqli_warning_create_object(zend_class_entry *class_type) {
    mylite_mysqli_warning *warning = zend_object_alloc(sizeof(mylite_mysqli_warning), class_type);
    zend_object_std_init(&warning->std, class_type);
    object_properties_init(&warning->std, class_type);
    warning->std.handlers = &mylite_mysqli_warning_handlers;
    ZVAL_UNDEF(&warning->link);
    warning->index = 0;
    return &warning->std;
}

static void mylite_mysqli_warning_free_obj(zend_object *object) {
    mylite_mysqli_warning *warning = mylite_mysqli_warning_from_obj(object);
    if (!Z_ISUNDEF(warning->link)) {
        zval_ptr_dtor(&warning->link);
    }
    zend_object_std_dtor(&warning->std);
}

static void zm_globals_ctor_mylite_mysqli(zend_mylite_mysqli_globals *globals) {
#if defined(COMPILE_DL_MYSQLI) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    globals->report_mode = MYLITE_MYSQLI_REPORT_OFF;
    globals->connect_errno = 0;
    globals->connect_error = NULL;
}

static void zm_globals_dtor_mylite_mysqli(zend_mylite_mysqli_globals *globals) {
    if (globals->connect_error != NULL) {
        zend_string_release(globals->connect_error);
        globals->connect_error = NULL;
    }
}

static PHP_MINIT_FUNCTION(mylite_mysqli) {
    mylite_mysqli_register_classes(INIT_FUNC_ARGS_PASSTHRU);
    mylite_mysqli_register_constants(INIT_FUNC_ARGS_PASSTHRU);
    return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(mylite_mysqli) {
    return SUCCESS;
}

static PHP_MINFO_FUNCTION(mylite_mysqli) {
    php_info_print_table_start();
    php_info_print_table_header(2, "mysqli support", "enabled");
    php_info_print_table_row(2, "MyLite mysqli version", PHP_MYLITE_MYSQLI_VERSION);
    php_info_print_table_row(2, "MyLite version", mylite_version());
    php_info_print_table_end();
}

static void mylite_mysqli_register_classes(INIT_FUNC_ARGS) {
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "mysqli", mylite_mysqli_link_methods);
    mylite_mysqli_link_ce = zend_register_internal_class(&ce);
    mylite_mysqli_link_ce->create_object = mylite_mysqli_link_create_object;
    memcpy(
        &mylite_mysqli_link_handlers,
        zend_get_std_object_handlers(),
        sizeof(zend_object_handlers)
    );
    mylite_mysqli_link_handlers.offset = XtOffsetOf(mylite_mysqli_link, std);
    mylite_mysqli_link_handlers.free_obj = mylite_mysqli_link_free_obj;
    mylite_mysqli_register_link_properties(mylite_mysqli_link_ce);

    INIT_CLASS_ENTRY(ce, "mysqli_result", mylite_mysqli_result_methods);
    mylite_mysqli_result_ce = zend_register_internal_class(&ce);
    mylite_mysqli_result_ce->create_object = mylite_mysqli_result_create_object;
    zend_class_implements(mylite_mysqli_result_ce, 1, zend_ce_aggregate);
    memcpy(
        &mylite_mysqli_result_handlers,
        zend_get_std_object_handlers(),
        sizeof(zend_object_handlers)
    );
    mylite_mysqli_result_handlers.offset = XtOffsetOf(mylite_mysqli_result, std);
    mylite_mysqli_result_handlers.free_obj = mylite_mysqli_result_free_obj;
    mylite_mysqli_register_result_properties(mylite_mysqli_result_ce);

    INIT_CLASS_ENTRY(ce, "mysqli_stmt", mylite_mysqli_stmt_methods);
    mylite_mysqli_stmt_ce = zend_register_internal_class(&ce);
    mylite_mysqli_stmt_ce->create_object = mylite_mysqli_stmt_create_object;
    memcpy(
        &mylite_mysqli_stmt_handlers,
        zend_get_std_object_handlers(),
        sizeof(zend_object_handlers)
    );
    mylite_mysqli_stmt_handlers.offset = XtOffsetOf(mylite_mysqli_stmt, std);
    mylite_mysqli_stmt_handlers.free_obj = mylite_mysqli_stmt_free_obj;
    mylite_mysqli_register_stmt_properties(mylite_mysqli_stmt_ce);

    INIT_CLASS_ENTRY(ce, "mysqli_driver", NULL);
    mylite_mysqli_driver_ce = zend_register_internal_class(&ce);
    mylite_mysqli_driver_ce->ce_flags |= ZEND_ACC_FINAL;
    mylite_mysqli_register_driver_properties(mylite_mysqli_driver_ce);

    INIT_CLASS_ENTRY(ce, "mysqli_warning", mylite_mysqli_warning_methods);
    mylite_mysqli_warning_ce = zend_register_internal_class(&ce);
    mylite_mysqli_warning_ce->create_object = mylite_mysqli_warning_create_object;
    mylite_mysqli_warning_ce->ce_flags |= ZEND_ACC_FINAL;
    memcpy(
        &mylite_mysqli_warning_handlers,
        zend_get_std_object_handlers(),
        sizeof(zend_object_handlers)
    );
    mylite_mysqli_warning_handlers.offset = XtOffsetOf(mylite_mysqli_warning, std);
    mylite_mysqli_warning_handlers.free_obj = mylite_mysqli_warning_free_obj;
    mylite_mysqli_register_warning_properties(mylite_mysqli_warning_ce);

    INIT_CLASS_ENTRY(ce, "mysqli_sql_exception", mylite_mysqli_exception_methods);
    mylite_mysqli_exception_ce = zend_register_internal_class_ex(&ce, zend_ce_exception);
    zend_declare_property_string(
        mylite_mysqli_exception_ce,
        "sqlstate",
        sizeof("sqlstate") - 1,
        "00000",
        ZEND_ACC_PROTECTED
    );
}

static void mylite_mysqli_register_link_properties(zend_class_entry *ce) {
    zend_declare_property_long(
        ce,
        "affected_rows",
        sizeof("affected_rows") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_string(ce, "client_info", sizeof("client_info") - 1, "", ZEND_ACC_PUBLIC);
    zend_declare_property_long(
        ce,
        "client_version",
        sizeof("client_version") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_long(
        ce,
        "connect_errno",
        sizeof("connect_errno") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_null(ce, "connect_error", sizeof("connect_error") - 1, ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "errno", sizeof("errno") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_string(ce, "error", sizeof("error") - 1, "", ZEND_ACC_PUBLIC);
    zend_declare_property_null(ce, "error_list", sizeof("error_list") - 1, ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "field_count", sizeof("field_count") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_string(ce, "host_info", sizeof("host_info") - 1, "", ZEND_ACC_PUBLIC);
    zend_declare_property_null(ce, "info", sizeof("info") - 1, ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "insert_id", sizeof("insert_id") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_string(ce, "server_info", sizeof("server_info") - 1, "", ZEND_ACC_PUBLIC);
    zend_declare_property_long(
        ce,
        "server_version",
        sizeof("server_version") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_string(ce, "sqlstate", sizeof("sqlstate") - 1, "00000", ZEND_ACC_PUBLIC);
    zend_declare_property_long(
        ce,
        "protocol_version",
        sizeof("protocol_version") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_long(ce, "thread_id", sizeof("thread_id") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(
        ce,
        "warning_count",
        sizeof("warning_count") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
}

static void mylite_mysqli_register_result_properties(zend_class_entry *ce) {
    zend_declare_property_long(
        ce,
        "current_field",
        sizeof("current_field") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_long(ce, "field_count", sizeof("field_count") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_null(ce, "lengths", sizeof("lengths") - 1, ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "num_rows", sizeof("num_rows") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(
        ce,
        "type",
        sizeof("type") - 1,
        MYLITE_MYSQLI_STORE_RESULT,
        ZEND_ACC_PUBLIC
    );
}

static void mylite_mysqli_register_stmt_properties(zend_class_entry *ce) {
    zend_declare_property_long(
        ce,
        "affected_rows",
        sizeof("affected_rows") - 1,
        0,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_long(ce, "insert_id", sizeof("insert_id") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "num_rows", sizeof("num_rows") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "param_count", sizeof("param_count") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "field_count", sizeof("field_count") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "errno", sizeof("errno") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_string(ce, "error", sizeof("error") - 1, "", ZEND_ACC_PUBLIC);
    zend_declare_property_null(ce, "error_list", sizeof("error_list") - 1, ZEND_ACC_PUBLIC);
    zend_declare_property_string(ce, "sqlstate", sizeof("sqlstate") - 1, "00000", ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "id", sizeof("id") - 1, 0, ZEND_ACC_PUBLIC);
}

static void mylite_mysqli_register_driver_properties(zend_class_entry *ce) {
    zend_declare_property_string(
        ce,
        "client_info",
        sizeof("client_info") - 1,
        "mylite",
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_long(
        ce,
        "client_version",
        sizeof("client_version") - 1,
        100100,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_long(
        ce,
        "driver_version",
        sizeof("driver_version") - 1,
        100100,
        ZEND_ACC_PUBLIC
    );
    zend_declare_property_long(ce, "report_mode", sizeof("report_mode") - 1, 0, ZEND_ACC_PUBLIC);
}

static void mylite_mysqli_register_warning_properties(zend_class_entry *ce) {
    zend_declare_property_string(ce, "message", sizeof("message") - 1, "", ZEND_ACC_PUBLIC);
    zend_declare_property_string(ce, "sqlstate", sizeof("sqlstate") - 1, "01000", ZEND_ACC_PUBLIC);
    zend_declare_property_long(ce, "errno", sizeof("errno") - 1, 0, ZEND_ACC_PUBLIC);
}

static void mylite_mysqli_register_constants(INIT_FUNC_ARGS) {
    REGISTER_BOOL_CONSTANT("MYLITE_MYSQLI", 1, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_READ_DEFAULT_GROUP", 5, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_READ_DEFAULT_FILE", 4, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_OPT_CONNECT_TIMEOUT", 0, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_OPT_LOCAL_INFILE", 8, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_OPT_LOAD_DATA_LOCAL_DIR", 43, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_INIT_COMMAND", 3, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_OPT_READ_TIMEOUT", 11, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_OPT_NET_CMD_BUFFER_SIZE", 202, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_OPT_NET_READ_BUFFER_SIZE", 203, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_OPT_INT_AND_FLOAT_NATIVE", 201, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_OPT_SSL_VERIFY_SERVER_CERT", 21, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_SERVER_PUBLIC_KEY", 35, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CLIENT_SSL", 2048, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CLIENT_COMPRESS", 32, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CLIENT_INTERACTIVE", 1024, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CLIENT_IGNORE_SPACE", 256, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CLIENT_NO_SCHEMA", 16, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CLIENT_FOUND_ROWS", 2, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CLIENT_SSL_VERIFY_SERVER_CERT", 1073741824, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CLIENT_SSL_DONT_VERIFY_SERVER_CERT", 64, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS", 4194304, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_OPT_CAN_HANDLE_EXPIRED_PASSWORDS", 37, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_STORE_RESULT", MYLITE_MYSQLI_STORE_RESULT, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_USE_RESULT", MYLITE_MYSQLI_USE_RESULT, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_ASYNC", 8, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_STORE_RESULT_COPY_DATA", 16, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_ASSOC", MYLITE_MYSQLI_ASSOC, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_NUM", MYLITE_MYSQLI_NUM, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_BOTH", MYLITE_MYSQLI_BOTH, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_STMT_ATTR_UPDATE_MAX_LENGTH", 0, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_STMT_ATTR_CURSOR_TYPE", 1, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CURSOR_TYPE_NO_CURSOR", 0, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_CURSOR_TYPE_READ_ONLY", 1, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_NOT_NULL_FLAG", 1, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_PRI_KEY_FLAG", 2, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_UNIQUE_KEY_FLAG", 4, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_MULTIPLE_KEY_FLAG", 8, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_BLOB_FLAG", 16, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_UNSIGNED_FLAG", 32, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_ZEROFILL_FLAG", 64, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_AUTO_INCREMENT_FLAG", 512, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TIMESTAMP_FLAG", 1024, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_SET_FLAG", 2048, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_NUM_FLAG", 32768, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_PART_KEY_FLAG", 16384, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_GROUP_FLAG", 32768, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_ENUM_FLAG", 256, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_BINARY_FLAG", 128, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_NO_DEFAULT_VALUE_FLAG", 4096, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_ON_UPDATE_NOW_FLAG", 8192, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_DECIMAL", 0, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_TINY", 1, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_SHORT", 2, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_LONG", 3, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_FLOAT", 4, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_DOUBLE", 5, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_NULL", 6, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_TIMESTAMP", 7, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_LONGLONG", 8, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_INT24", 9, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_DATE", 10, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_TIME", 11, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_DATETIME", 12, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_YEAR", 13, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_NEWDATE", 14, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_ENUM", 247, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_SET", 248, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_TINY_BLOB", 249, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_MEDIUM_BLOB", 250, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_LONG_BLOB", 251, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_BLOB", 252, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_VAR_STRING", 253, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_STRING", 254, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_CHAR", 1, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_GEOMETRY", 255, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_VECTOR", 242, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_JSON", 245, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_NEWDECIMAL", 246, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TYPE_BIT", 16, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_SET_CHARSET_NAME", 7, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_NO_DATA", 100, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_DATA_TRUNCATED", 101, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_INDEX", 4, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_ERROR", MYLITE_MYSQLI_REPORT_ERROR, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_STRICT", MYLITE_MYSQLI_REPORT_STRICT, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_ALL", 255, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REPORT_OFF", MYLITE_MYSQLI_REPORT_OFF, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_DEBUG_TRACE_ENABLED", 0, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_SERVER_QUERY_NO_GOOD_INDEX_USED", 16, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_SERVER_QUERY_NO_INDEX_USED", 32, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_SERVER_QUERY_WAS_SLOW", 2048, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_SERVER_PS_OUT_PARAMS", 4096, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_GRANT", 1, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_LOG", 2, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_TABLES", 4, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_HOSTS", 8, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_STATUS", 16, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_THREADS", 32, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_REPLICA", 64, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_SLAVE", 64, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_MASTER", 128, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_REFRESH_BACKUP_LOG", 2097152, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TRANS_START_WITH_CONSISTENT_SNAPSHOT", 1, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TRANS_START_READ_WRITE", 2, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TRANS_START_READ_ONLY", 4, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TRANS_COR_AND_CHAIN", 1, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TRANS_COR_AND_NO_CHAIN", 2, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TRANS_COR_RELEASE", 4, CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("MYSQLI_TRANS_COR_NO_RELEASE", 8, CONST_PERSISTENT);
    REGISTER_BOOL_CONSTANT("MYSQLI_IS_MARIADB", 0, CONST_PERSISTENT);
}

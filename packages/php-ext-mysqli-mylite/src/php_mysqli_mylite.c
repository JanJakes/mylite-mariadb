// clang-format off
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <php.h>
#include <ext/standard/info.h>
// clang-format on

#include <mylite/mylite.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define PHP_MYSQLI_MYLITE_EXT_VERSION "0.1.0"
#define PHP_MYLITE_MYSQLI_QUERY_CACHE_CAPACITY 8U

typedef struct php_mylite_mysqli_query_cache_entry {
    mylite_stmt *stmt;
    zend_string *sql;
    uint64_t last_used;
} php_mylite_mysqli_query_cache_entry;

typedef struct php_mylite_mysqli_link {
    mylite_db *db;
    zend_string *charset;
    zend_string *recent_result_sql;
    php_mylite_mysqli_query_cache_entry query_cache[PHP_MYLITE_MYSQLI_QUERY_CACHE_CAPACITY];
    uint64_t query_cache_clock;
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

typedef enum php_mylite_mysqli_profile_close_kind {
    PHP_MYLITE_MYSQLI_PROFILE_CLOSE_EXPLICIT = 0,
    PHP_MYLITE_MYSQLI_PROFILE_CLOSE_OBJECT_FREE = 1,
    PHP_MYLITE_MYSQLI_PROFILE_CLOSE_REOPEN = 2,
} php_mylite_mysqli_profile_close_kind;

typedef struct php_mylite_mysqli_profile_stats {
    uint64_t open_calls;
    uint64_t open_successes;
    uint64_t open_failures;
    uint64_t open_ns;
    uint64_t close_calls;
    uint64_t close_successes;
    uint64_t close_failures;
    uint64_t close_explicit_calls;
    uint64_t close_object_free_calls;
    uint64_t close_reopen_calls;
    uint64_t close_ns;
    uint64_t query_calls;
    uint64_t query_successes;
    uint64_t query_failures;
    uint64_t query_result_calls;
    uint64_t query_no_result_calls;
    uint64_t query_call_exec_calls;
    uint64_t query_cache_hits;
    uint64_t query_cache_misses;
    uint64_t query_classify_calls;
    uint64_t query_classify_ns;
    uint64_t query_cache_lookup_calls;
    uint64_t query_cache_lookup_ns;
    uint64_t query_cache_clear_calls;
    uint64_t query_cache_clear_finalize_calls;
    uint64_t query_cache_clear_ns;
    uint64_t query_cache_preserved_no_result_calls;
    uint64_t query_cache_reset_failures;
    uint64_t query_prepare_calls;
    uint64_t query_prepare_ns;
    uint64_t query_result_execute_calls;
    uint64_t query_result_execute_ns;
    uint64_t query_result_step_calls;
    uint64_t query_result_step_ns;
    uint64_t query_result_row_materialize_calls;
    uint64_t query_result_row_materialize_ns;
    uint64_t query_result_field_calls;
    uint64_t query_result_field_ns;
    uint64_t query_result_object_calls;
    uint64_t query_result_object_ns;
    uint64_t query_status_sync_calls;
    uint64_t query_status_sync_ns;
    uint64_t query_result_rows;
    uint64_t query_ns;
    uint64_t exec_result_calls;
    uint64_t exec_result_ns;
    uint64_t exec_result_rows;
    uint64_t exec_result_callback_ns;
    uint64_t exec_no_result_calls;
    uint64_t exec_no_result_ns;
    uint64_t explicit_prepare_calls;
    uint64_t explicit_prepare_successes;
    uint64_t explicit_prepare_failures;
    uint64_t explicit_prepare_ns;
    uint64_t stmt_execute_calls;
    uint64_t stmt_execute_successes;
    uint64_t stmt_execute_failures;
    uint64_t stmt_execute_ns;
    uint64_t stmt_reset_calls;
    uint64_t stmt_reset_ns;
    uint64_t stmt_bind_calls;
    uint64_t stmt_bind_ns;
    uint64_t stmt_step_calls;
    uint64_t stmt_step_ns;
    uint64_t stmt_rows;
    uint64_t fetch_assoc_calls;
    uint64_t fetch_assoc_ns;
    uint64_t fetch_array_calls;
    uint64_t fetch_array_ns;
    uint64_t fetch_object_calls;
    uint64_t fetch_object_ns;
    uint64_t fetch_all_calls;
    uint64_t fetch_all_ns;
} php_mylite_mysqli_profile_stats;

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
static bool php_mylite_mysqli_profile_enabled;
static bool php_mylite_mysqli_prepared_query_results_enabled;
static FILE *php_mylite_mysqli_profile_output_file;
static php_mylite_mysqli_profile_stats php_mylite_mysqli_profile;
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
static bool php_mylite_mysqli_profile_env_enabled(void);
static bool php_mylite_mysqli_prepared_query_results_env_enabled(void);
static uint64_t php_mylite_mysqli_profile_now_ns(void);
static uint64_t php_mylite_mysqli_profile_start(void);
static uint64_t php_mylite_mysqli_profile_elapsed_ns(uint64_t start);
static void php_mylite_mysqli_profile_add_elapsed(uint64_t *target, uint64_t start);
static void php_mylite_mysqli_profile_print(void);
static void php_mylite_mysqli_profile_print_counter(const char *name, uint64_t value);
static void php_mylite_mysqli_profile_print_context(void);
static void php_mylite_mysqli_profile_print_millis(const char *name, uint64_t ns);
static void php_mylite_mysqli_profile_print_average_millis(
    const char *name,
    uint64_t ns,
    uint64_t count
);
static int php_mylite_mysqli_profile_finish_query(int status, uint64_t start);
static int php_mylite_mysqli_profile_finish_prepare(int status, uint64_t start);
static int php_mylite_mysqli_profile_finish_stmt_execute(int status, uint64_t start);
static int php_mylite_mysqli_profiled_close(
    mylite_db *db,
    php_mylite_mysqli_profile_close_kind kind
);
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
static void php_mylite_mysqli_clear_query_cache(php_mylite_mysqli_link *link);
static void php_mylite_mysqli_clear_recent_result_sql(php_mylite_mysqli_link *link);
static bool php_mylite_mysqli_recent_result_sql_matches(
    php_mylite_mysqli_link *link,
    const char *sql,
    size_t sql_len
);
static void php_mylite_mysqli_remember_recent_result_sql(
    php_mylite_mysqli_link *link,
    const char *sql,
    size_t sql_len
);
static php_mylite_mysqli_query_cache_entry *php_mylite_mysqli_query_cache_find(
    php_mylite_mysqli_link *link,
    const char *sql,
    size_t sql_len
);
static php_mylite_mysqli_query_cache_entry *php_mylite_mysqli_query_cache_store(
    php_mylite_mysqli_link *link,
    const char *sql,
    size_t sql_len,
    mylite_stmt *stmt
);
static void php_mylite_mysqli_query_cache_clear_entry(php_mylite_mysqli_query_cache_entry *entry);
static void php_mylite_mysqli_update_property_long_if_changed(
    zend_object *object,
    const char *name,
    size_t name_len,
    zend_long value
);
static void php_mylite_mysqli_update_property_string_if_changed(
    zend_object *object,
    const char *name,
    size_t name_len,
    const char *value
);
static void php_mylite_mysqli_update_property_str_if_changed(
    zend_object *object,
    const char *name,
    size_t name_len,
    zend_string *value
);
static void php_mylite_mysqli_update_property_u64_string_if_changed(
    zend_object *object,
    const char *name,
    size_t name_len,
    uint64_t value
);
static int php_mylite_mysqli_query_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    size_t sql_len,
    zval *return_value
);
static int php_mylite_mysqli_execute_result_stmt(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    mylite_stmt *stmt,
    zval *return_value,
    int *out_result
);
static int php_mylite_mysqli_exec_query_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    zval *return_value
);
static int php_mylite_mysqli_exec_no_result_query_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    zval *return_value
);
static int php_mylite_mysqli_exec_result_metadata_callback(
    void *ctx,
    int column_count,
    const mylite_exec_column *columns
);
static int php_mylite_mysqli_exec_result_callback(
    void *ctx,
    int column_count,
    char **values,
    const size_t *value_lengths,
    const mylite_exec_column *columns
);
static void php_mylite_mysqli_exec_result_init_fields(
    php_mylite_mysqli_exec_result_context *result_ctx,
    int column_count,
    const mylite_exec_column *columns
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
static bool php_mylite_mysqli_stmt_bindings_cover_native_params(php_mylite_mysqli_stmt *stmt);
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
static bool php_mylite_mysqli_is_no_result_query(const char *sql, size_t sql_len);
static bool php_mylite_mysqli_no_result_query_preserves_cache(const char *sql, size_t sql_len);
static bool php_mylite_mysqli_sql_contains_token(
    const char *sql,
    size_t sql_len,
    const char *token,
    size_t token_len
);
static bool php_mylite_mysqli_keyword_equals(
    const char *keyword,
    size_t keyword_len,
    const char *expected
);
static bool php_mylite_mysqli_sql_token_char(char value);
static char php_mylite_mysqli_ascii_lower(char value);

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

ZEND_BEGIN_ARG_INFO_EX(arginfo_mylite_mysqli_method_fetch_field, 0, 0, 0)
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
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.fetch_assoc_calls;
    }
    const uint64_t fetch_start = php_mylite_mysqli_profile_start();
    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.fetch_assoc_ns,
            fetch_start
        );
        RETURN_NULL();
    }
    ++result->position;
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.fetch_assoc_ns, fetch_start);
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
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.fetch_array_calls;
    }
    const uint64_t fetch_start = php_mylite_mysqli_profile_start();
    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.fetch_array_ns,
            fetch_start
        );
        RETURN_NULL();
    }
    ++result->position;
    php_mylite_mysqli_fetch_array_row(result, row, mode, return_value);
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.fetch_array_ns, fetch_start);
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
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.fetch_object_calls;
    }
    const uint64_t fetch_start = php_mylite_mysqli_profile_start();
    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.fetch_object_ns,
            fetch_start
        );
        RETURN_NULL();
    }
    ++result->position;
    php_mylite_mysqli_fetch_object_row(row, return_value);
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.fetch_object_ns, fetch_start);
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
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.fetch_all_calls;
    }
    const uint64_t fetch_start = php_mylite_mysqli_profile_start();
    php_mylite_mysqli_fetch_all_rows(result, mode, return_value);
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.fetch_all_ns, fetch_start);
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
        php_mylite_mysqli_clear_query_cache(link);
        const int result =
            php_mylite_mysqli_profiled_close(link->db, PHP_MYLITE_MYSQLI_PROFILE_CLOSE_EXPLICIT);
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
        php_mylite_mysqli_clear_query_cache(link);
        const int result =
            php_mylite_mysqli_profiled_close(link->db, PHP_MYLITE_MYSQLI_PROFILE_CLOSE_EXPLICIT);
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

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.fetch_assoc_calls;
    }
    const uint64_t fetch_start = php_mylite_mysqli_profile_start();
    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.fetch_assoc_ns,
            fetch_start
        );
        RETURN_NULL();
    }
    ++result->position;
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.fetch_assoc_ns, fetch_start);
    RETURN_COPY(row);
}

PHP_METHOD(MyLite_MySQLiResult, fetch_array) {
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(ZEND_THIS);
    zend_long mode = 3;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.fetch_array_calls;
    }
    const uint64_t fetch_start = php_mylite_mysqli_profile_start();
    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.fetch_array_ns,
            fetch_start
        );
        RETURN_NULL();
    }
    ++result->position;
    php_mylite_mysqli_fetch_array_row(result, row, mode, return_value);
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.fetch_array_ns, fetch_start);
}

PHP_METHOD(MyLite_MySQLiResult, fetch_object) {
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.fetch_object_calls;
    }
    const uint64_t fetch_start = php_mylite_mysqli_profile_start();
    zval *row = zend_hash_index_find(Z_ARRVAL(result->rows), result->position);
    if (row == NULL) {
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.fetch_object_ns,
            fetch_start
        );
        RETURN_NULL();
    }
    ++result->position;
    php_mylite_mysqli_fetch_object_row(row, return_value);
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.fetch_object_ns, fetch_start);
}

PHP_METHOD(MyLite_MySQLiResult, fetch_all) {
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(ZEND_THIS);
    zend_long mode = 1;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.fetch_all_calls;
    }
    const uint64_t fetch_start = php_mylite_mysqli_profile_start();
    php_mylite_mysqli_fetch_all_rows(result, mode, return_value);
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.fetch_all_ns, fetch_start);
}

PHP_METHOD(MyLite_MySQLiResult, fetch_field) {
    php_mylite_mysqli_result *result = Z_MYLITE_MYSQLI_RESULT_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_NONE();

    zval *field = zend_hash_index_find(Z_ARRVAL(result->fields), result->field_position);
    if (field == NULL) {
        RETURN_FALSE;
    }
    ++result->field_position;
    RETURN_COPY(field);
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
                )
                    PHP_ME(
                        MyLite_MySQLiResult,
                        fetch_field,
                        arginfo_mylite_mysqli_method_fetch_field,
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

    memset(&php_mylite_mysqli_profile, 0, sizeof(php_mylite_mysqli_profile));
    php_mylite_mysqli_profile_enabled = php_mylite_mysqli_profile_env_enabled();
    php_mylite_mysqli_prepared_query_results_enabled =
        php_mylite_mysqli_prepared_query_results_env_enabled();

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

PHP_MSHUTDOWN_FUNCTION(mysqli_mylite) {
    (void)type;
    (void)module_number;
    php_mylite_mysqli_profile_print();
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
    PHP_MSHUTDOWN(mysqli_mylite),
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
    link->recent_result_sql = NULL;
    memset(link->query_cache, 0, sizeof(link->query_cache));
    link->query_cache_clock = 0U;
    zend_object_std_init(&link->std, class_entry);
    object_properties_init(&link->std, class_entry);
    link->std.handlers = &php_mylite_mysqli_link_handlers;
    return &link->std;
}

static void php_mylite_mysqli_link_free(zend_object *object) {
    php_mylite_mysqli_link *link = php_mylite_mysqli_link_from_object(object);
    php_mylite_mysqli_clear_query_cache(link);
    php_mylite_mysqli_clear_recent_result_sql(link);
    if (link->db != NULL) {
        const int close_result =
            php_mylite_mysqli_profiled_close(link->db, PHP_MYLITE_MYSQLI_PROFILE_CLOSE_OBJECT_FREE);
        (void)close_result;
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

static bool php_mylite_mysqli_profile_env_enabled(void) {
    const char *value = getenv("MYLITE_MYSQLI_PROFILE");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool php_mylite_mysqli_prepared_query_results_env_enabled(void) {
    const char *value = getenv("MYLITE_MYSQLI_PREPARED_QUERY_RESULTS");
    return value != NULL && strcmp(value, "1") == 0;
}

static uint64_t php_mylite_mysqli_profile_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static uint64_t php_mylite_mysqli_profile_start(void) {
    return php_mylite_mysqli_profile_enabled ? php_mylite_mysqli_profile_now_ns() : 0;
}

static uint64_t php_mylite_mysqli_profile_elapsed_ns(uint64_t start) {
    if (!php_mylite_mysqli_profile_enabled || start == 0) {
        return 0;
    }
    const uint64_t end = php_mylite_mysqli_profile_now_ns();
    if (end <= start) {
        return 0;
    }
    return end - start;
}

static void php_mylite_mysqli_profile_add_elapsed(uint64_t *target, uint64_t start) {
    if (!php_mylite_mysqli_profile_enabled || target == NULL) {
        return;
    }
    *target += php_mylite_mysqli_profile_elapsed_ns(start);
}

static void php_mylite_mysqli_profile_print_counter(const char *name, uint64_t value) {
    FILE *output = php_mylite_mysqli_profile_output_file != NULL
                       ? php_mylite_mysqli_profile_output_file
                       : stderr;
    fprintf(output, "mylite_mysqli_profile_%s=%" PRIu64 "\n", name, value);
}

static void php_mylite_mysqli_profile_print_context(void) {
    const char *context = getenv("MYLITE_MYSQLI_PROFILE_CONTEXT");
    if (context == NULL || context[0] == '\0') {
        return;
    }

    char sanitized[65];
    size_t out = 0;
    for (size_t in = 0; context[in] != '\0' && out + 1 < sizeof(sanitized); ++in) {
        const unsigned char ch = (unsigned char)context[in];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-' || ch == '.' || ch == ':') {
            sanitized[out++] = (char)ch;
        } else {
            sanitized[out++] = '_';
        }
    }
    sanitized[out] = '\0';
    if (out == 0) {
        return;
    }

    FILE *output = php_mylite_mysqli_profile_output_file != NULL
                       ? php_mylite_mysqli_profile_output_file
                       : stderr;
    fprintf(output, "mylite_mysqli_profile_context=%s\n", sanitized);
}

static void php_mylite_mysqli_profile_print_millis(const char *name, uint64_t ns) {
    FILE *output = php_mylite_mysqli_profile_output_file != NULL
                       ? php_mylite_mysqli_profile_output_file
                       : stderr;
    fprintf(output, "mylite_mysqli_profile_%s=%.3f\n", name, (double)ns / 1000000.0);
}

static void php_mylite_mysqli_profile_print_average_millis(
    const char *name,
    uint64_t ns,
    uint64_t count
) {
    const double average = count == 0 ? 0.0 : ((double)ns / 1000000.0) / (double)count;
    FILE *output = php_mylite_mysqli_profile_output_file != NULL
                       ? php_mylite_mysqli_profile_output_file
                       : stderr;
    fprintf(output, "mylite_mysqli_profile_%s=%.3f\n", name, average);
}

static void php_mylite_mysqli_profile_print(void) {
    if (!php_mylite_mysqli_profile_enabled) {
        return;
    }

    const uint64_t operations =
        php_mylite_mysqli_profile.open_calls + php_mylite_mysqli_profile.close_calls +
        php_mylite_mysqli_profile.query_calls + php_mylite_mysqli_profile.explicit_prepare_calls +
        php_mylite_mysqli_profile.stmt_execute_calls;
    if (operations == 0) {
        return;
    }

    FILE *profile_output = stderr;
    const char *profile_output_path = getenv("MYLITE_MYSQLI_PROFILE_OUTPUT");
    if (profile_output_path != NULL && profile_output_path[0] != '\0') {
        FILE *file = fopen(profile_output_path, "a");
        if (file != NULL) {
            profile_output = file;
            (void)flock(fileno(profile_output), LOCK_EX);
        }
    }
    php_mylite_mysqli_profile_output_file = profile_output;

    php_mylite_mysqli_profile_print_counter("enabled", 1);
    php_mylite_mysqli_profile_print_counter("pid", (uint64_t)getpid());
    php_mylite_mysqli_profile_print_context();
    php_mylite_mysqli_profile_print_counter("open_calls", php_mylite_mysqli_profile.open_calls);
    php_mylite_mysqli_profile_print_counter(
        "open_successes",
        php_mylite_mysqli_profile.open_successes
    );
    php_mylite_mysqli_profile_print_counter(
        "open_failures",
        php_mylite_mysqli_profile.open_failures
    );
    php_mylite_mysqli_profile_print_millis("open_ms_total", php_mylite_mysqli_profile.open_ns);
    php_mylite_mysqli_profile_print_average_millis(
        "open_ms_avg",
        php_mylite_mysqli_profile.open_ns,
        php_mylite_mysqli_profile.open_calls
    );
    php_mylite_mysqli_profile_print_counter("close_calls", php_mylite_mysqli_profile.close_calls);
    php_mylite_mysqli_profile_print_counter(
        "close_successes",
        php_mylite_mysqli_profile.close_successes
    );
    php_mylite_mysqli_profile_print_counter(
        "close_failures",
        php_mylite_mysqli_profile.close_failures
    );
    php_mylite_mysqli_profile_print_counter(
        "close_explicit_calls",
        php_mylite_mysqli_profile.close_explicit_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "close_object_free_calls",
        php_mylite_mysqli_profile.close_object_free_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "close_reopen_calls",
        php_mylite_mysqli_profile.close_reopen_calls
    );
    php_mylite_mysqli_profile_print_millis("close_ms_total", php_mylite_mysqli_profile.close_ns);
    php_mylite_mysqli_profile_print_average_millis(
        "close_ms_avg",
        php_mylite_mysqli_profile.close_ns,
        php_mylite_mysqli_profile.close_calls
    );
    php_mylite_mysqli_profile_print_counter("query_calls", php_mylite_mysqli_profile.query_calls);
    php_mylite_mysqli_profile_print_counter(
        "query_successes",
        php_mylite_mysqli_profile.query_successes
    );
    php_mylite_mysqli_profile_print_counter(
        "query_failures",
        php_mylite_mysqli_profile.query_failures
    );
    php_mylite_mysqli_profile_print_counter(
        "query_result_calls",
        php_mylite_mysqli_profile.query_result_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "query_no_result_calls",
        php_mylite_mysqli_profile.query_no_result_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "query_call_exec_calls",
        php_mylite_mysqli_profile.query_call_exec_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "query_cache_hits",
        php_mylite_mysqli_profile.query_cache_hits
    );
    php_mylite_mysqli_profile_print_counter(
        "query_cache_misses",
        php_mylite_mysqli_profile.query_cache_misses
    );
    php_mylite_mysqli_profile_print_counter(
        "query_classify_calls",
        php_mylite_mysqli_profile.query_classify_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_classify_ms_total",
        php_mylite_mysqli_profile.query_classify_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_cache_lookup_calls",
        php_mylite_mysqli_profile.query_cache_lookup_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_cache_lookup_ms_total",
        php_mylite_mysqli_profile.query_cache_lookup_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_cache_clear_calls",
        php_mylite_mysqli_profile.query_cache_clear_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "query_cache_clear_finalize_calls",
        php_mylite_mysqli_profile.query_cache_clear_finalize_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_cache_clear_ms_total",
        php_mylite_mysqli_profile.query_cache_clear_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_cache_preserved_no_result_calls",
        php_mylite_mysqli_profile.query_cache_preserved_no_result_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "query_cache_reset_failures",
        php_mylite_mysqli_profile.query_cache_reset_failures
    );
    php_mylite_mysqli_profile_print_counter(
        "query_prepare_calls",
        php_mylite_mysqli_profile.query_prepare_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_prepare_ms_total",
        php_mylite_mysqli_profile.query_prepare_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_result_execute_calls",
        php_mylite_mysqli_profile.query_result_execute_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_result_execute_ms_total",
        php_mylite_mysqli_profile.query_result_execute_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_result_step_calls",
        php_mylite_mysqli_profile.query_result_step_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_result_step_ms_total",
        php_mylite_mysqli_profile.query_result_step_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_result_row_materialize_calls",
        php_mylite_mysqli_profile.query_result_row_materialize_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_result_row_materialize_ms_total",
        php_mylite_mysqli_profile.query_result_row_materialize_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_result_field_calls",
        php_mylite_mysqli_profile.query_result_field_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_result_field_ms_total",
        php_mylite_mysqli_profile.query_result_field_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_result_object_calls",
        php_mylite_mysqli_profile.query_result_object_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_result_object_ms_total",
        php_mylite_mysqli_profile.query_result_object_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_status_sync_calls",
        php_mylite_mysqli_profile.query_status_sync_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "query_status_sync_ms_total",
        php_mylite_mysqli_profile.query_status_sync_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "query_result_rows",
        php_mylite_mysqli_profile.query_result_rows
    );
    php_mylite_mysqli_profile_print_millis("query_ms_total", php_mylite_mysqli_profile.query_ns);
    php_mylite_mysqli_profile_print_average_millis(
        "query_ms_avg",
        php_mylite_mysqli_profile.query_ns,
        php_mylite_mysqli_profile.query_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "exec_result_calls",
        php_mylite_mysqli_profile.exec_result_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "exec_result_ms_total",
        php_mylite_mysqli_profile.exec_result_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "exec_result_rows",
        php_mylite_mysqli_profile.exec_result_rows
    );
    php_mylite_mysqli_profile_print_millis(
        "exec_result_callback_ms_total",
        php_mylite_mysqli_profile.exec_result_callback_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "exec_no_result_calls",
        php_mylite_mysqli_profile.exec_no_result_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "exec_no_result_ms_total",
        php_mylite_mysqli_profile.exec_no_result_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "explicit_prepare_calls",
        php_mylite_mysqli_profile.explicit_prepare_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "explicit_prepare_successes",
        php_mylite_mysqli_profile.explicit_prepare_successes
    );
    php_mylite_mysqli_profile_print_counter(
        "explicit_prepare_failures",
        php_mylite_mysqli_profile.explicit_prepare_failures
    );
    php_mylite_mysqli_profile_print_millis(
        "explicit_prepare_ms_total",
        php_mylite_mysqli_profile.explicit_prepare_ns
    );
    php_mylite_mysqli_profile_print_average_millis(
        "explicit_prepare_ms_avg",
        php_mylite_mysqli_profile.explicit_prepare_ns,
        php_mylite_mysqli_profile.explicit_prepare_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "stmt_execute_calls",
        php_mylite_mysqli_profile.stmt_execute_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "stmt_execute_successes",
        php_mylite_mysqli_profile.stmt_execute_successes
    );
    php_mylite_mysqli_profile_print_counter(
        "stmt_execute_failures",
        php_mylite_mysqli_profile.stmt_execute_failures
    );
    php_mylite_mysqli_profile_print_millis(
        "stmt_execute_ms_total",
        php_mylite_mysqli_profile.stmt_execute_ns
    );
    php_mylite_mysqli_profile_print_average_millis(
        "stmt_execute_ms_avg",
        php_mylite_mysqli_profile.stmt_execute_ns,
        php_mylite_mysqli_profile.stmt_execute_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "stmt_reset_calls",
        php_mylite_mysqli_profile.stmt_reset_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "stmt_reset_ms_total",
        php_mylite_mysqli_profile.stmt_reset_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "stmt_bind_calls",
        php_mylite_mysqli_profile.stmt_bind_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "stmt_bind_ms_total",
        php_mylite_mysqli_profile.stmt_bind_ns
    );
    php_mylite_mysqli_profile_print_counter(
        "stmt_step_calls",
        php_mylite_mysqli_profile.stmt_step_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "stmt_step_ms_total",
        php_mylite_mysqli_profile.stmt_step_ns
    );
    php_mylite_mysqli_profile_print_counter("stmt_rows", php_mylite_mysqli_profile.stmt_rows);
    php_mylite_mysqli_profile_print_counter(
        "fetch_assoc_calls",
        php_mylite_mysqli_profile.fetch_assoc_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "fetch_assoc_ms_total",
        php_mylite_mysqli_profile.fetch_assoc_ns
    );
    php_mylite_mysqli_profile_print_average_millis(
        "fetch_assoc_ms_avg",
        php_mylite_mysqli_profile.fetch_assoc_ns,
        php_mylite_mysqli_profile.fetch_assoc_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "fetch_array_calls",
        php_mylite_mysqli_profile.fetch_array_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "fetch_array_ms_total",
        php_mylite_mysqli_profile.fetch_array_ns
    );
    php_mylite_mysqli_profile_print_average_millis(
        "fetch_array_ms_avg",
        php_mylite_mysqli_profile.fetch_array_ns,
        php_mylite_mysqli_profile.fetch_array_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "fetch_object_calls",
        php_mylite_mysqli_profile.fetch_object_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "fetch_object_ms_total",
        php_mylite_mysqli_profile.fetch_object_ns
    );
    php_mylite_mysqli_profile_print_average_millis(
        "fetch_object_ms_avg",
        php_mylite_mysqli_profile.fetch_object_ns,
        php_mylite_mysqli_profile.fetch_object_calls
    );
    php_mylite_mysqli_profile_print_counter(
        "fetch_all_calls",
        php_mylite_mysqli_profile.fetch_all_calls
    );
    php_mylite_mysqli_profile_print_millis(
        "fetch_all_ms_total",
        php_mylite_mysqli_profile.fetch_all_ns
    );
    php_mylite_mysqli_profile_print_average_millis(
        "fetch_all_ms_avg",
        php_mylite_mysqli_profile.fetch_all_ns,
        php_mylite_mysqli_profile.fetch_all_calls
    );

    fflush(profile_output);
    php_mylite_mysqli_profile_output_file = NULL;
    if (profile_output != stderr) {
        (void)flock(fileno(profile_output), LOCK_UN);
        fclose(profile_output);
    }
}

static int php_mylite_mysqli_profile_finish_query(int status, uint64_t start) {
    if (php_mylite_mysqli_profile_enabled) {
        php_mylite_mysqli_profile.query_ns += php_mylite_mysqli_profile_elapsed_ns(start);
        if (status == SUCCESS) {
            ++php_mylite_mysqli_profile.query_successes;
        } else {
            ++php_mylite_mysqli_profile.query_failures;
        }
    }
    return status;
}

static int php_mylite_mysqli_profile_finish_prepare(int status, uint64_t start) {
    if (php_mylite_mysqli_profile_enabled) {
        php_mylite_mysqli_profile.explicit_prepare_ns +=
            php_mylite_mysqli_profile_elapsed_ns(start);
        if (status == SUCCESS) {
            ++php_mylite_mysqli_profile.explicit_prepare_successes;
        } else {
            ++php_mylite_mysqli_profile.explicit_prepare_failures;
        }
    }
    return status;
}

static int php_mylite_mysqli_profile_finish_stmt_execute(int status, uint64_t start) {
    if (php_mylite_mysqli_profile_enabled) {
        php_mylite_mysqli_profile.stmt_execute_ns += php_mylite_mysqli_profile_elapsed_ns(start);
        if (status == SUCCESS) {
            ++php_mylite_mysqli_profile.stmt_execute_successes;
        } else {
            ++php_mylite_mysqli_profile.stmt_execute_failures;
        }
    }
    return status;
}

static int php_mylite_mysqli_profiled_close(
    mylite_db *db,
    php_mylite_mysqli_profile_close_kind kind
) {
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.close_calls;
        switch (kind) {
        case PHP_MYLITE_MYSQLI_PROFILE_CLOSE_EXPLICIT:
            ++php_mylite_mysqli_profile.close_explicit_calls;
            break;
        case PHP_MYLITE_MYSQLI_PROFILE_CLOSE_OBJECT_FREE:
            ++php_mylite_mysqli_profile.close_object_free_calls;
            break;
        case PHP_MYLITE_MYSQLI_PROFILE_CLOSE_REOPEN:
            ++php_mylite_mysqli_profile.close_reopen_calls;
            break;
        }
    }

    const uint64_t close_start = php_mylite_mysqli_profile_start();
    const int result = mylite_close(db);
    if (php_mylite_mysqli_profile_enabled) {
        php_mylite_mysqli_profile.close_ns += php_mylite_mysqli_profile_elapsed_ns(close_start);
        if (result == MYLITE_OK) {
            ++php_mylite_mysqli_profile.close_successes;
        } else {
            ++php_mylite_mysqli_profile.close_failures;
        }
    }
    return result;
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
        php_mylite_mysqli_clear_query_cache(link);
        (void)php_mylite_mysqli_profiled_close(link->db, PHP_MYLITE_MYSQLI_PROFILE_CLOSE_REOPEN);
        link->db = NULL;
    }

    mylite_db *db = NULL;
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.open_calls;
    }
    const uint64_t open_start = php_mylite_mysqli_profile_start();
    const int result = mylite_open(path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, NULL);
    if (php_mylite_mysqli_profile_enabled) {
        php_mylite_mysqli_profile.open_ns += php_mylite_mysqli_profile_elapsed_ns(open_start);
        if (result == MYLITE_OK) {
            ++php_mylite_mysqli_profile.open_successes;
        } else {
            ++php_mylite_mysqli_profile.open_failures;
        }
    }
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
    php_mylite_mysqli_clear_query_cache(link);
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
    php_mylite_mysqli_clear_query_cache(link);
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
    php_mylite_mysqli_update_property_long_if_changed(object, "errno", sizeof("errno") - 1, 0);
    php_mylite_mysqli_update_property_string_if_changed(object, "error", sizeof("error") - 1, "");
    php_mylite_mysqli_update_property_long_if_changed(
        object,
        "connect_errno",
        sizeof("connect_errno") - 1,
        0
    );
    php_mylite_mysqli_update_property_string_if_changed(
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
        php_mylite_mysqli_update_property_long_if_changed(
            object,
            "affected_rows",
            sizeof("affected_rows") - 1,
            0
        );
        php_mylite_mysqli_update_property_string_if_changed(
            object,
            "insert_id",
            sizeof("insert_id") - 1,
            "0"
        );
        return;
    }
    php_mylite_mysqli_update_property_long_if_changed(
        object,
        "affected_rows",
        sizeof("affected_rows") - 1,
        (zend_long)mylite_changes(link->db)
    );
    php_mylite_mysqli_update_property_u64_string_if_changed(
        object,
        "insert_id",
        sizeof("insert_id") - 1,
        mylite_last_insert_id(link->db)
    );
}

static void php_mylite_mysqli_clear_query_cache(php_mylite_mysqli_link *link) {
    if (link == NULL) {
        return;
    }
    php_mylite_mysqli_clear_recent_result_sql(link);
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_cache_clear_calls;
    }
    const uint64_t clear_start = php_mylite_mysqli_profile_start();
    for (size_t index = 0; index < PHP_MYLITE_MYSQLI_QUERY_CACHE_CAPACITY; ++index) {
        php_mylite_mysqli_query_cache_clear_entry(&link->query_cache[index]);
    }
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_cache_clear_ns,
        clear_start
    );
}

static void php_mylite_mysqli_clear_recent_result_sql(php_mylite_mysqli_link *link) {
    if (link == NULL || link->recent_result_sql == NULL) {
        return;
    }
    zend_string_release(link->recent_result_sql);
    link->recent_result_sql = NULL;
}

static bool php_mylite_mysqli_recent_result_sql_matches(
    php_mylite_mysqli_link *link,
    const char *sql,
    size_t sql_len
) {
    return link != NULL && link->recent_result_sql != NULL &&
           ZSTR_LEN(link->recent_result_sql) == sql_len &&
           memcmp(ZSTR_VAL(link->recent_result_sql), sql, sql_len) == 0;
}

static void php_mylite_mysqli_remember_recent_result_sql(
    php_mylite_mysqli_link *link,
    const char *sql,
    size_t sql_len
) {
    if (link == NULL) {
        return;
    }
    if (php_mylite_mysqli_recent_result_sql_matches(link, sql, sql_len)) {
        return;
    }
    php_mylite_mysqli_clear_recent_result_sql(link);
    link->recent_result_sql = zend_string_init(sql, sql_len, false);
}

static php_mylite_mysqli_query_cache_entry *php_mylite_mysqli_query_cache_find(
    php_mylite_mysqli_link *link,
    const char *sql,
    size_t sql_len
) {
    if (link == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < PHP_MYLITE_MYSQLI_QUERY_CACHE_CAPACITY; ++index) {
        php_mylite_mysqli_query_cache_entry *entry = &link->query_cache[index];
        if (entry->stmt != NULL && entry->sql != NULL && ZSTR_LEN(entry->sql) == sql_len &&
            memcmp(ZSTR_VAL(entry->sql), sql, sql_len) == 0) {
            return entry;
        }
    }
    return NULL;
}

static php_mylite_mysqli_query_cache_entry *php_mylite_mysqli_query_cache_store(
    php_mylite_mysqli_link *link,
    const char *sql,
    size_t sql_len,
    mylite_stmt *stmt
) {
    php_mylite_mysqli_query_cache_entry *target = NULL;
    for (size_t index = 0; index < PHP_MYLITE_MYSQLI_QUERY_CACHE_CAPACITY; ++index) {
        php_mylite_mysqli_query_cache_entry *entry = &link->query_cache[index];
        if (entry->stmt == NULL) {
            target = entry;
            break;
        }
        if (target == NULL || entry->last_used < target->last_used) {
            target = entry;
        }
    }

    php_mylite_mysqli_query_cache_clear_entry(target);
    target->stmt = stmt;
    target->sql = zend_string_init(sql, sql_len, false);
    target->last_used = ++link->query_cache_clock;
    return target;
}

static void php_mylite_mysqli_query_cache_clear_entry(php_mylite_mysqli_query_cache_entry *entry) {
    if (entry == NULL) {
        return;
    }
    if (entry->stmt != NULL) {
        if (php_mylite_mysqli_profile_enabled) {
            ++php_mylite_mysqli_profile.query_cache_clear_finalize_calls;
        }
        (void)mylite_finalize(entry->stmt);
        entry->stmt = NULL;
    }
    if (entry->sql != NULL) {
        zend_string_release(entry->sql);
        entry->sql = NULL;
    }
    entry->last_used = 0U;
}

static void php_mylite_mysqli_update_property_long_if_changed(
    zend_object *object,
    const char *name,
    size_t name_len,
    zend_long value
) {
    zval property_value;
    zval *current = zend_read_property(object->ce, object, name, name_len, false, &property_value);
    ZVAL_DEREF(current);
    if (Z_TYPE_P(current) == IS_LONG && Z_LVAL_P(current) == value) {
        return;
    }
    zend_update_property_long(object->ce, object, name, name_len, value);
}

static void php_mylite_mysqli_update_property_string_if_changed(
    zend_object *object,
    const char *name,
    size_t name_len,
    const char *value
) {
    zval property_value;
    zval *current = zend_read_property(object->ce, object, name, name_len, false, &property_value);
    const size_t value_len = strlen(value);
    ZVAL_DEREF(current);
    if (Z_TYPE_P(current) == IS_STRING && Z_STRLEN_P(current) == value_len &&
        memcmp(Z_STRVAL_P(current), value, value_len) == 0) {
        return;
    }
    zend_update_property_string(object->ce, object, name, name_len, value);
}

static void php_mylite_mysqli_update_property_str_if_changed(
    zend_object *object,
    const char *name,
    size_t name_len,
    zend_string *value
) {
    zval property_value;
    zval *current = zend_read_property(object->ce, object, name, name_len, false, &property_value);
    ZVAL_DEREF(current);
    if (Z_TYPE_P(current) == IS_STRING && zend_string_equals(Z_STR_P(current), value)) {
        return;
    }
    zend_update_property_str(object->ce, object, name, name_len, value);
}

static void php_mylite_mysqli_update_property_u64_string_if_changed(
    zend_object *object,
    const char *name,
    size_t name_len,
    uint64_t value
) {
    if (value == 0U) {
        php_mylite_mysqli_update_property_string_if_changed(object, name, name_len, "0");
        return;
    }
    zend_string *string_value = zend_u64_to_str(value);
    php_mylite_mysqli_update_property_str_if_changed(object, name, name_len, string_value);
    zend_string_release(string_value);
}

static int php_mylite_mysqli_query_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    size_t sql_len,
    zval *return_value
) {
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_calls;
    }
    const uint64_t query_start = php_mylite_mysqli_profile_start();
    mylite_db *db = php_mylite_mysqli_require_db(link);
    if (db == NULL) {
        php_mylite_mysqli_set_error(
            link,
            link_object,
            MYLITE_MISUSE,
            "MyLite mysqli link is closed"
        );
        return php_mylite_mysqli_profile_finish_query(FAILURE, query_start);
    }

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_classify_calls;
    }
    uint64_t subphase_start = php_mylite_mysqli_profile_start();
    const bool is_call_query = php_mylite_mysqli_is_call_query(sql, sql_len);
    const bool is_no_result_query =
        !is_call_query && php_mylite_mysqli_is_no_result_query(sql, sql_len);
    const bool no_result_preserves_cache =
        is_no_result_query && php_mylite_mysqli_no_result_query_preserves_cache(sql, sql_len);
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_classify_ns,
        subphase_start
    );

    if (is_call_query) {
        if (php_mylite_mysqli_profile_enabled) {
            ++php_mylite_mysqli_profile.query_call_exec_calls;
        }
        php_mylite_mysqli_clear_query_cache(link);
        return php_mylite_mysqli_profile_finish_query(
            php_mylite_mysqli_exec_query_impl(link, link_object, sql, return_value),
            query_start
        );
    }
    if (is_no_result_query) {
        if (php_mylite_mysqli_profile_enabled) {
            ++php_mylite_mysqli_profile.query_no_result_calls;
        }
        if (no_result_preserves_cache) {
            if (php_mylite_mysqli_profile_enabled) {
                ++php_mylite_mysqli_profile.query_cache_preserved_no_result_calls;
            }
            php_mylite_mysqli_clear_recent_result_sql(link);
        } else {
            php_mylite_mysqli_clear_query_cache(link);
        }
        return php_mylite_mysqli_profile_finish_query(
            php_mylite_mysqli_exec_no_result_query_impl(link, link_object, sql, return_value),
            query_start
        );
    }

    if (!php_mylite_mysqli_prepared_query_results_enabled &&
        !php_mylite_mysqli_recent_result_sql_matches(link, sql, sql_len)) {
        if (php_mylite_mysqli_profile_enabled) {
            ++php_mylite_mysqli_profile.query_result_calls;
        }
        php_mylite_mysqli_clear_recent_result_sql(link);
        const int text_result_status =
            php_mylite_mysqli_exec_query_impl(link, link_object, sql, return_value);
        if (text_result_status == SUCCESS) {
            php_mylite_mysqli_remember_recent_result_sql(link, sql, sql_len);
        }
        return php_mylite_mysqli_profile_finish_query(text_result_status, query_start);
    }

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_result_calls;
        ++php_mylite_mysqli_profile.query_cache_lookup_calls;
    }
    subphase_start = php_mylite_mysqli_profile_start();
    bool from_cache = false;
    php_mylite_mysqli_query_cache_entry *cache_entry =
        php_mylite_mysqli_query_cache_find(link, sql, sql_len);
    if (cache_entry != NULL) {
        const int reset_result = mylite_reset(cache_entry->stmt);
        if (reset_result == MYLITE_OK) {
            from_cache = true;
            cache_entry->last_used = ++link->query_cache_clock;
            if (php_mylite_mysqli_profile_enabled) {
                ++php_mylite_mysqli_profile.query_cache_hits;
            }
        } else {
            if (php_mylite_mysqli_profile_enabled) {
                ++php_mylite_mysqli_profile.query_cache_reset_failures;
            }
            php_mylite_mysqli_clear_query_cache(link);
            cache_entry = NULL;
        }
    }
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_cache_lookup_ns,
        subphase_start
    );
    if (!from_cache && php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_cache_misses;
    }

    if (cache_entry == NULL) {
        mylite_stmt *stmt = NULL;
        if (php_mylite_mysqli_profile_enabled) {
            ++php_mylite_mysqli_profile.query_prepare_calls;
        }
        const uint64_t prepare_start = php_mylite_mysqli_profile_start();
        const int prepare_result = mylite_prepare(db, sql, sql_len, &stmt, NULL);
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.query_prepare_ns,
            prepare_start
        );
        if (prepare_result != MYLITE_OK) {
            php_mylite_mysqli_set_error(
                link,
                link_object,
                prepare_result,
                "could not prepare query"
            );
            return php_mylite_mysqli_profile_finish_query(FAILURE, query_start);
        }
        cache_entry = php_mylite_mysqli_query_cache_store(link, sql, sql_len, stmt);
    }

    int query_result = MYLITE_OK;
    if (php_mylite_mysqli_execute_result_stmt(
            link,
            link_object,
            cache_entry->stmt,
            return_value,
            &query_result
        ) == SUCCESS) {
        return php_mylite_mysqli_profile_finish_query(SUCCESS, query_start);
    }

    if (from_cache) {
        php_mylite_mysqli_clear_query_cache(link);
        mylite_stmt *stmt = NULL;
        if (php_mylite_mysqli_profile_enabled) {
            ++php_mylite_mysqli_profile.query_prepare_calls;
        }
        const uint64_t prepare_start = php_mylite_mysqli_profile_start();
        const int prepare_result = mylite_prepare(db, sql, sql_len, &stmt, NULL);
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.query_prepare_ns,
            prepare_start
        );
        if (prepare_result != MYLITE_OK) {
            php_mylite_mysqli_set_error(
                link,
                link_object,
                prepare_result,
                "could not prepare query"
            );
            return php_mylite_mysqli_profile_finish_query(FAILURE, query_start);
        }
        cache_entry = php_mylite_mysqli_query_cache_store(link, sql, sql_len, stmt);
        if (php_mylite_mysqli_execute_result_stmt(
                link,
                link_object,
                cache_entry->stmt,
                return_value,
                &query_result
            ) == SUCCESS) {
            return php_mylite_mysqli_profile_finish_query(SUCCESS, query_start);
        }
    }

    php_mylite_mysqli_clear_query_cache(link);
    php_mylite_mysqli_set_error(link, link_object, query_result, "query failed");
    return php_mylite_mysqli_profile_finish_query(FAILURE, query_start);
}

static int php_mylite_mysqli_execute_result_stmt(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    mylite_stmt *stmt,
    zval *return_value,
    int *out_result
) {
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_result_execute_calls;
    }
    const uint64_t execute_start = php_mylite_mysqli_profile_start();
    zval rows;
    zval fields;
    array_init(&rows);
    array_init(&fields);
    uint64_t row_count = 0;
    uint64_t subphase_start = php_mylite_mysqli_profile_start();
    int step_result = mylite_step(stmt);
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_result_step_ns,
        subphase_start
    );
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_result_step_calls;
    }
    while (step_result == MYLITE_ROW) {
        subphase_start = php_mylite_mysqli_profile_start();
        (void)php_mylite_mysqli_add_current_row(stmt, &rows);
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.query_result_row_materialize_ns,
            subphase_start
        );
        if (php_mylite_mysqli_profile_enabled) {
            ++php_mylite_mysqli_profile.query_result_row_materialize_calls;
        }
        ++row_count;
        subphase_start = php_mylite_mysqli_profile_start();
        step_result = mylite_step(stmt);
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.query_result_step_ns,
            subphase_start
        );
        if (php_mylite_mysqli_profile_enabled) {
            ++php_mylite_mysqli_profile.query_result_step_calls;
        }
    }
    if (step_result != MYLITE_DONE) {
        zval_ptr_dtor(&rows);
        zval_ptr_dtor(&fields);
        if (out_result != NULL) {
            *out_result = step_result;
        }
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.query_result_execute_ns,
            execute_start
        );
        return FAILURE;
    }

    subphase_start = php_mylite_mysqli_profile_start();
    const unsigned column_count = mylite_column_count(stmt);
    php_mylite_mysqli_fields_from_stmt(stmt, &fields);
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_result_field_ns,
        subphase_start
    );
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_result_field_calls;
    }
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_status_sync_calls;
    }
    subphase_start = php_mylite_mysqli_profile_start();
    php_mylite_mysqli_clear_error(link_object);
    php_mylite_mysqli_sync_status(link, link_object);
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_status_sync_ns,
        subphase_start
    );
    if (column_count == 0U) {
        zval_ptr_dtor(&rows);
        zval_ptr_dtor(&fields);
        php_mylite_mysqli_clear_query_cache(link);
        ZVAL_TRUE(return_value);
        php_mylite_mysqli_profile_add_elapsed(
            &php_mylite_mysqli_profile.query_result_execute_ns,
            execute_start
        );
        if (php_mylite_mysqli_profile_enabled) {
            php_mylite_mysqli_profile.query_result_rows += row_count;
        }
        return SUCCESS;
    }

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_result_object_calls;
    }
    subphase_start = php_mylite_mysqli_profile_start();
    php_mylite_mysqli_result_from_rows(
        return_value,
        &rows,
        &fields,
        php_mylite_mysqli_result_class_for_link(link_object)
    );
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_result_object_ns,
        subphase_start
    );
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_result_execute_ns,
        execute_start
    );
    if (php_mylite_mysqli_profile_enabled) {
        php_mylite_mysqli_profile.query_result_rows += row_count;
    }
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
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.exec_result_calls;
    }
    const uint64_t exec_start = php_mylite_mysqli_profile_start();
    const int result = mylite_exec_result_with_metadata(
        link->db,
        sql,
        php_mylite_mysqli_exec_result_metadata_callback,
        php_mylite_mysqli_exec_result_callback,
        &ctx,
        &errmsg
    );
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.exec_result_ns, exec_start);
    mylite_free(errmsg);
    if (result != MYLITE_OK) {
        zval_ptr_dtor(&rows);
        zval_ptr_dtor(&fields);
        php_mylite_mysqli_set_error(link, link_object, result, "query failed");
        return FAILURE;
    }

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_status_sync_calls;
    }
    uint64_t subphase_start = php_mylite_mysqli_profile_start();
    php_mylite_mysqli_clear_error(link_object);
    php_mylite_mysqli_sync_status(link, link_object);
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_status_sync_ns,
        subphase_start
    );
    if (!ctx.fields_initialized) {
        zval_ptr_dtor(&rows);
        zval_ptr_dtor(&fields);
        ZVAL_TRUE(return_value);
        return SUCCESS;
    }

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_result_object_calls;
    }
    subphase_start = php_mylite_mysqli_profile_start();
    php_mylite_mysqli_result_from_rows(
        return_value,
        &rows,
        &fields,
        php_mylite_mysqli_result_class_for_link(link_object)
    );
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_result_object_ns,
        subphase_start
    );
    return SUCCESS;
}

static int php_mylite_mysqli_exec_no_result_query_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    zval *return_value
) {
    char *errmsg = NULL;
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.exec_no_result_calls;
    }
    const uint64_t exec_start = php_mylite_mysqli_profile_start();
    const int result = mylite_exec(link->db, sql, NULL, NULL, &errmsg);
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.exec_no_result_ns, exec_start);
    mylite_free(errmsg);
    if (result != MYLITE_OK) {
        php_mylite_mysqli_set_error(link, link_object, result, "query failed");
        return FAILURE;
    }

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.query_status_sync_calls;
    }
    const uint64_t subphase_start = php_mylite_mysqli_profile_start();
    php_mylite_mysqli_clear_error(link_object);
    php_mylite_mysqli_sync_status(link, link_object);
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.query_status_sync_ns,
        subphase_start
    );
    ZVAL_TRUE(return_value);
    return SUCCESS;
}

static int php_mylite_mysqli_exec_result_metadata_callback(
    void *ctx,
    int column_count,
    const mylite_exec_column *columns
) {
    php_mylite_mysqli_exec_result_context *result_ctx =
        (php_mylite_mysqli_exec_result_context *)ctx;
    php_mylite_mysqli_exec_result_init_fields(result_ctx, column_count, columns);
    return 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): required libmylite callback signature.
static int php_mylite_mysqli_exec_result_callback(
    void *ctx,
    int column_count,
    char **values,
    const size_t *value_lengths,
    const mylite_exec_column *columns
) {
    const uint64_t callback_start = php_mylite_mysqli_profile_start();
    php_mylite_mysqli_exec_result_context *result_ctx =
        (php_mylite_mysqli_exec_result_context *)ctx;
    if (!result_ctx->fields_initialized) {
        php_mylite_mysqli_exec_result_init_fields(result_ctx, column_count, columns);
    }

    zval row;
    array_init(&row);
    for (int column = 0; column < column_count; ++column) {
        zval value;
        if (values[column] == NULL) {
            ZVAL_NULL(&value);
        } else {
            ZVAL_STRINGL(&value, values[column], value_lengths[column]);
        }
        const char *column_name = columns[column].name != NULL ? columns[column].name : "";
        add_assoc_zval_ex(&row, column_name, strlen(column_name), &value);
    }
    add_next_index_zval(result_ctx->rows, &row);
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.exec_result_rows;
        ++php_mylite_mysqli_profile.query_result_rows;
    }
    php_mylite_mysqli_profile_add_elapsed(
        &php_mylite_mysqli_profile.exec_result_callback_ns,
        callback_start
    );
    return 0;
}

static void php_mylite_mysqli_exec_result_init_fields(
    php_mylite_mysqli_exec_result_context *result_ctx,
    int column_count,
    const mylite_exec_column *columns
) {
    if (result_ctx == NULL || result_ctx->fields_initialized || column_count <= 0 ||
        columns == NULL) {
        return;
    }
    for (int column = 0; column < column_count; ++column) {
        const mylite_exec_column *metadata = &columns[column];
        php_mylite_mysqli_add_field(
            result_ctx->fields,
            metadata->name,
            metadata->org_name,
            metadata->table,
            metadata->org_table
        );
    }
    result_ctx->fields_initialized = true;
}

static int php_mylite_mysqli_prepare_impl(
    php_mylite_mysqli_link *link,
    zend_object *link_object,
    const char *sql,
    size_t sql_len,
    zval *return_value
) {
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.explicit_prepare_calls;
    }
    const uint64_t prepare_start = php_mylite_mysqli_profile_start();
    mylite_db *db = php_mylite_mysqli_require_db(link);
    if (db == NULL) {
        php_mylite_mysqli_set_error(
            link,
            link_object,
            MYLITE_MISUSE,
            "MyLite mysqli link is closed"
        );
        return php_mylite_mysqli_profile_finish_prepare(FAILURE, prepare_start);
    }

    php_mylite_mysqli_clear_query_cache(link);
    mylite_stmt *stmt = NULL;
    const int result = mylite_prepare(db, sql, sql_len, &stmt, NULL);
    if (result != MYLITE_OK) {
        php_mylite_mysqli_set_error(link, link_object, result, "could not prepare statement");
        return php_mylite_mysqli_profile_finish_prepare(FAILURE, prepare_start);
    }

    object_init_ex(return_value, php_mylite_mysqli_stmt_class_for_link(link_object));
    php_mylite_mysqli_stmt *stmt_object = Z_MYLITE_MYSQLI_STMT_P(return_value);
    stmt_object->stmt = stmt;
    stmt_object->link_object = link_object;
    GC_ADDREF(stmt_object->link_object);
    return php_mylite_mysqli_profile_finish_prepare(SUCCESS, prepare_start);
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

static bool php_mylite_mysqli_stmt_bindings_cover_native_params(php_mylite_mysqli_stmt *stmt) {
    if (stmt == NULL || stmt->stmt == NULL) {
        return false;
    }

    return stmt->bound_count >= mylite_bind_parameter_count(stmt->stmt);
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
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.stmt_execute_calls;
    }
    const uint64_t execute_start = php_mylite_mysqli_profile_start();
    if (stmt->stmt == NULL) {
        return php_mylite_mysqli_profile_finish_stmt_execute(FAILURE, execute_start);
    }

    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.stmt_reset_calls;
    }
    uint64_t subphase_start = php_mylite_mysqli_profile_start();
    (void)mylite_reset(stmt->stmt);
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.stmt_reset_ns, subphase_start);
    if (!php_mylite_mysqli_stmt_bindings_cover_native_params(stmt)) {
        (void)mylite_clear_bindings(stmt->stmt);
    }
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.stmt_bind_calls;
    }
    subphase_start = php_mylite_mysqli_profile_start();
    for (uint32_t index = 0; index < stmt->bound_count; ++index) {
        const int bind_result =
            php_mylite_mysqli_bind_zval(stmt->stmt, index + 1U, &stmt->bound_values[index]);
        if (bind_result != MYLITE_OK) {
            php_mylite_mysqli_profile_add_elapsed(
                &php_mylite_mysqli_profile.stmt_bind_ns,
                subphase_start
            );
            return php_mylite_mysqli_profile_finish_stmt_execute(FAILURE, execute_start);
        }
    }
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.stmt_bind_ns, subphase_start);

    php_mylite_mysqli_stmt_clear_rows(stmt);
    php_mylite_mysqli_stmt_clear_fields(stmt);
    if (php_mylite_mysqli_profile_enabled) {
        ++php_mylite_mysqli_profile.stmt_step_calls;
    }
    subphase_start = php_mylite_mysqli_profile_start();
    uint64_t row_count = 0;
    int step_result = mylite_step(stmt->stmt);
    const unsigned column_count = mylite_column_count(stmt->stmt);
    while (step_result == MYLITE_ROW) {
        (void)php_mylite_mysqli_add_current_row(stmt->stmt, &stmt->rows);
        ++row_count;
        step_result = mylite_step(stmt->stmt);
    }
    php_mylite_mysqli_profile_add_elapsed(&php_mylite_mysqli_profile.stmt_step_ns, subphase_start);
    if (step_result != MYLITE_DONE) {
        return php_mylite_mysqli_profile_finish_stmt_execute(FAILURE, execute_start);
    }
    php_mylite_mysqli_fields_from_stmt(stmt->stmt, &stmt->fields);
    stmt->has_rows = column_count > 0U;
    if (php_mylite_mysqli_profile_enabled) {
        php_mylite_mysqli_profile.stmt_rows += row_count;
    }
    return php_mylite_mysqli_profile_finish_stmt_execute(SUCCESS, execute_start);
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

static bool php_mylite_mysqli_is_no_result_query(const char *sql, size_t sql_len) {
    size_t offset = 0;
    while (offset < sql_len) {
        const char value = sql[offset];
        if (value != ' ' && value != '\t' && value != '\n' && value != '\r' && value != '\f') {
            break;
        }
        ++offset;
    }

    const size_t keyword_start = offset;
    while (offset < sql_len && php_mylite_mysqli_sql_token_char(sql[offset])) {
        ++offset;
    }
    const size_t keyword_len = offset - keyword_start;
    if (keyword_len == 0U) {
        return false;
    }

    const char *keyword = sql + keyword_start;
    if (php_mylite_mysqli_keyword_equals(keyword, keyword_len, "DELETE") ||
        php_mylite_mysqli_keyword_equals(keyword, keyword_len, "INSERT") ||
        php_mylite_mysqli_keyword_equals(keyword, keyword_len, "REPLACE") ||
        php_mylite_mysqli_keyword_equals(keyword, keyword_len, "UPDATE")) {
        return !php_mylite_mysqli_sql_contains_token(
            sql + offset,
            sql_len - offset,
            "RETURNING",
            sizeof("RETURNING") - 1U
        );
    }

    return php_mylite_mysqli_keyword_equals(keyword, keyword_len, "ALTER") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "BEGIN") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "COMMIT") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "CREATE") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "DROP") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "LOCK") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "RELEASE") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "ROLLBACK") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "SAVEPOINT") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "SET") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "START") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "TRUNCATE") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "UNLOCK") ||
           php_mylite_mysqli_keyword_equals(keyword, keyword_len, "USE");
}

static bool php_mylite_mysqli_no_result_query_preserves_cache(const char *sql, size_t sql_len) {
    size_t offset = 0;
    while (offset < sql_len) {
        const char value = sql[offset];
        if (value != ' ' && value != '\t' && value != '\n' && value != '\r' && value != '\f') {
            break;
        }
        ++offset;
    }

    const size_t keyword_start = offset;
    while (offset < sql_len && php_mylite_mysqli_sql_token_char(sql[offset])) {
        ++offset;
    }
    const size_t keyword_len = offset - keyword_start;
    if (keyword_len == 0U) {
        return false;
    }

    const char *keyword = sql + keyword_start;
    if (!php_mylite_mysqli_keyword_equals(keyword, keyword_len, "DELETE") &&
        !php_mylite_mysqli_keyword_equals(keyword, keyword_len, "INSERT") &&
        !php_mylite_mysqli_keyword_equals(keyword, keyword_len, "REPLACE") &&
        !php_mylite_mysqli_keyword_equals(keyword, keyword_len, "UPDATE")) {
        return false;
    }

    return !php_mylite_mysqli_sql_contains_token(
        sql + offset,
        sql_len - offset,
        "RETURNING",
        sizeof("RETURNING") - 1U
    );
}

static bool php_mylite_mysqli_sql_contains_token(
    const char *sql,
    size_t sql_len,
    const char *token,
    size_t token_len
) {
    for (size_t offset = 0; offset + token_len <= sql_len; ++offset) {
        if (offset > 0U && php_mylite_mysqli_sql_token_char(sql[offset - 1U])) {
            continue;
        }
        if (offset + token_len < sql_len &&
            php_mylite_mysqli_sql_token_char(sql[offset + token_len])) {
            continue;
        }

        bool matches = true;
        for (size_t token_offset = 0; token_offset < token_len; ++token_offset) {
            if (php_mylite_mysqli_ascii_lower(sql[offset + token_offset]) !=
                php_mylite_mysqli_ascii_lower(token[token_offset])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

static bool php_mylite_mysqli_keyword_equals(
    const char *keyword,
    size_t keyword_len,
    const char *expected
) {
    const size_t expected_len = strlen(expected);
    if (keyword_len != expected_len) {
        return false;
    }
    for (size_t offset = 0; offset < expected_len; ++offset) {
        if (php_mylite_mysqli_ascii_lower(keyword[offset]) !=
            php_mylite_mysqli_ascii_lower(expected[offset])) {
            return false;
        }
    }
    return true;
}

static bool php_mylite_mysqli_sql_token_char(char value) {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') || value == '_';
}

static char php_mylite_mysqli_ascii_lower(char value) {
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

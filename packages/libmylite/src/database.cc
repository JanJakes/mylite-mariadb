#include <mylite/mylite.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#if MYLITE_WITH_MARIADB_EMBEDDED
#  include <mysql.h>
#endif

#ifndef MYLITE_MARIADB_MESSAGES_DIR
#  define MYLITE_MARIADB_MESSAGES_DIR ""
#endif

#ifndef MYLITE_MARIADB_CHARSETS_DIR
#  define MYLITE_MARIADB_CHARSETS_DIR ""
#endif

namespace {

constexpr unsigned k_known_open_flags = MYLITE_OPEN_READONLY | MYLITE_OPEN_READWRITE |
                                        MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE |
                                        MYLITE_OPEN_URI;
constexpr const char *k_sqlstate_ok = "00000";
constexpr const char *k_sqlstate_general = "HY000";
constexpr const char *k_not_an_error = "not an error";
constexpr const char *k_bad_db_handle = "bad database handle";
constexpr int k_decimal_base = 10;

#if MYLITE_WITH_MARIADB_EMBEDDED
constexpr std::size_t k_sql_policy_token_count = 32;
constexpr const char *k_memory_database_path = ":memory:";
constexpr const char *k_meta_filename = "mylite.meta";
constexpr const char *k_lock_filename = "mylite.lock";
constexpr const char *k_datadir_name = "datadir";
constexpr const char *k_tmpdir_name = "tmp";
constexpr const char *k_rundir_name = "run";
constexpr const char *k_plugin_directory_name = "plugins";
constexpr const char *k_mariadb_base_ref = "mariadb-11.8.6";
constexpr const char *k_metadata_format_line = "format=1";
constexpr const char *k_innodb_temp_data_file_path = "ibtmp1:12M:autoextend";
constexpr int k_runtime_directory_attempts = 100;
constexpr unsigned k_lock_poll_interval_ms = 10;
constexpr unsigned long k_initial_result_buffer_size = 4096;

struct RuntimeLayout {
    std::filesystem::path cleanup_directory;
    std::filesystem::path persistent_tmp_directory;
    std::filesystem::path data_directory;
    std::filesystem::path tmp_directory;
    std::filesystem::path plugin_directory;
};

struct DatabaseLockWait {
    int lock_fd = -1;
    unsigned busy_timeout_ms = 0;
};

struct ParameterBinding {
    MYSQL_BIND bind = {};
    std::vector<unsigned char> bytes;
    unsigned long length = 0;
    my_bool is_null = 0;
    my_bool error = 0;
    long long int64_value = 0;
    unsigned long long uint64_value = 0;
    double double_value = 0.0;
};

struct ResultColumn {
    MYSQL_BIND bind = {};
    enum enum_field_types field_type = MYSQL_TYPE_NULL;
    unsigned int flags = 0;
    std::string name;
    std::vector<unsigned char> buffer;
    unsigned long length = 0;
    my_bool is_null = 0;
    my_bool error = 0;
};

struct SqlPolicyTokens {
    std::array<std::string_view, k_sql_policy_token_count> values = {};
    std::size_t count = 0;
};
#endif

struct RuntimeState {
    std::mutex mutex;
    unsigned ref_count = 0;
    std::filesystem::path cleanup_directory;
    std::filesystem::path persistent_tmp_directory;
    std::string database_path;
    std::vector<std::string> arguments;
    std::vector<char *> argv;
    int lock_fd = -1;
};

RuntimeState g_runtime;

} // namespace

struct mylite_db {
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL mysql = {};
#endif
    std::string database_path;
    std::string current_schema;
    int errcode = MYLITE_OK;
    int extended_errcode = MYLITE_OK;
    unsigned mariadb_errno = 0;
    std::string sqlstate = k_sqlstate_ok;
    std::string errmsg = k_not_an_error;
    std::string warning_message;
    long long changes = 0;
    unsigned long long last_insert_id = 0;
    unsigned active_statement_count = 0;
    bool connected = false;
};

struct mylite_stmt {
    mylite_db *db = nullptr;
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL_STMT *stmt = nullptr;
    MYSQL_RES *metadata = nullptr;
    std::vector<ParameterBinding> parameters;
    std::vector<MYSQL_BIND> parameter_binds;
    std::vector<ResultColumn> columns;
    std::vector<MYSQL_BIND> result_binds;
#endif
    bool executed = false;
    bool has_result = false;
    bool has_row = false;
};

namespace {

int open_impl(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
int validate_open_args(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
#if MYLITE_WITH_MARIADB_EMBEDDED
int validate_runtime_database_path(mylite_db &db);
int prepare_database_directory(const std::filesystem::path &database_path, unsigned flags);
int prepare_existing_database_directory(const std::filesystem::path &database_path, unsigned flags);
int validate_database_layout(const std::filesystem::path &database_path);
int validate_layout_directory(const std::filesystem::path &directory);
int validate_database_metadata(const std::filesystem::path &metadata_path);
bool database_directory_is_empty(
    const std::filesystem::path &database_path,
    std::error_code &error
);
int start_runtime(mylite_db &db, const mylite_open_config *config);
int connect_runtime(mylite_db &db);
int acquire_database_lock(
    mylite_db &db,
    const std::filesystem::path &database_path,
    const mylite_open_config *config
);
int wait_for_database_lock(DatabaseLockWait wait);
void release_database_lock(int lock_fd);
unsigned configured_busy_timeout_ms(const mylite_open_config *config);
#endif
void close_connection(mylite_db &db);
void release_runtime(void);
int exec_impl(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
);
int prepare_impl(
    mylite_db *db,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
);
#if MYLITE_WITH_MARIADB_EMBEDDED
int store_and_emit_result(
    mylite_db &db,
    mylite_exec_callback callback,
    void *ctx,
    bool *has_result
);
int initialize_statement_results(mylite_stmt &stmt);
int fetch_statement_row(mylite_stmt &stmt);
int fetch_truncated_statement_columns(mylite_stmt &stmt);
int configure_column_buffer(ResultColumn &column, unsigned long buffer_length);
void release_statement_results(mylite_stmt &stmt);
ParameterBinding *parameter_at(mylite_stmt &stmt, unsigned index);
int bind_null_value(mylite_stmt &stmt, unsigned index);
int bind_bytes(
    mylite_stmt &stmt,
    unsigned index,
    const void *value,
    std::size_t value_len,
    enum enum_field_types buffer_type,
    mylite_destructor destructor
);
void bind_parameter_buffer(ParameterBinding &parameter);
int bind_parameters(mylite_stmt &stmt);
mylite_value_type column_type(const ResultColumn &column);
const ResultColumn *column_at(const mylite_stmt *stmt, unsigned column);
void set_mariadb_statement_error(mylite_stmt &stmt);
int reject_unsupported_sql_policy(mylite_db &db, std::string_view sql);
void update_current_schema_after_successful_sql(mylite_db &db, std::string_view sql);
bool is_unsupported_server_surface_sql(std::string_view sql, const std::string &current_schema);
bool is_unsupported_oracle_sql_mode_statement(std::string_view sql);
bool is_unsupported_procedure_analyse_statement(std::string_view sql);
bool is_unsupported_account_or_event_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_plugin_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_udf_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_replication_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_binlog_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_help_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_static_show_info_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_statement_profiling_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_query_cache_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_query_log_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_optimizer_trace_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool is_unsupported_server_set_statement(const SqlPolicyTokens &tokens);
SqlPolicyTokens collect_sql_policy_tokens(std::string_view sql);
bool next_sql_token(std::string_view sql, std::size_t &offset, std::string_view &token);
void skip_sql_spacing_and_comments(std::string_view sql, std::size_t &offset);
bool enter_executable_sql_comment(std::string_view sql, std::size_t &offset);
bool skip_dash_sql_comment(std::string_view sql, std::size_t &offset);
bool skip_hash_sql_comment(std::string_view sql, std::size_t &offset);
bool skip_block_sql_comment(std::string_view sql, std::size_t &offset);
void skip_quoted_sql_token(std::string_view sql, std::size_t &offset);
bool is_sql_space(char value);
bool is_sql_identifier_char(char value);
bool is_sql_identifier_token(std::string_view token);
std::string_view identifier_token_at(const SqlPolicyTokens &tokens, std::size_t index);
std::string_view unquoted_identifier_token(std::string_view token);
bool has_identifier_token(
    const SqlPolicyTokens &tokens,
    const char *keyword,
    std::size_t start_index
);
bool has_information_schema_table(const SqlPolicyTokens &tokens, const char *table_name);
bool has_current_schema_table_reference(
    const SqlPolicyTokens &tokens,
    const char *table_name,
    std::string_view current_schema
);
bool has_unqualified_table_reference(const SqlPolicyTokens &tokens, const char *table_name);
bool is_sql_mode_assignment_target(const SqlPolicyTokens &tokens, std::size_t index);
bool sql_mode_assignment_mentions_oracle(const SqlPolicyTokens &tokens, std::size_t index);
bool token_contains_sql_mode_name(std::string_view token, const char *mode_name);
bool token_equals(std::string_view token, const char *keyword);
bool identifier_token_equals(std::string_view token, const char *keyword);
bool table_reference_keyword(std::string_view token);
bool token_in(std::string_view token, const char *first, const char *second);
bool token_in(std::string_view token, const char *first, const char *second, const char *third);
bool token_in(
    std::string_view token,
    const char *first,
    const char *second,
    const char *third,
    const char *fourth
);
bool is_server_variable_token(std::string_view token);
bool is_query_log_variable_token(std::string_view token);
bool is_system_variable_qualified_token(const SqlPolicyTokens &tokens, std::size_t index);
bool is_system_variable_assignment_start(const SqlPolicyTokens &tokens, std::size_t index);
std::size_t first_set_assignment_token_index(const SqlPolicyTokens &tokens);
std::filesystem::path normalize_database_path(const char *path);
bool is_memory_database_path(const std::filesystem::path &database_path);
void initialize_database_layout(const std::filesystem::path &database_path);
void create_layout_directory(const std::filesystem::path &directory, const char *message);
void write_database_metadata(const std::filesystem::path &metadata_path);
RuntimeLayout create_runtime_layout(
    const std::filesystem::path &database_path,
    const mylite_open_config *config
);
RuntimeLayout create_memory_runtime_layout(const mylite_open_config *config);
RuntimeLayout create_persistent_runtime_layout(const std::filesystem::path &database_path);
std::filesystem::path runtime_root(const mylite_open_config *config);
std::string unique_runtime_name(void);
void create_runtime_subdirectory(const std::filesystem::path &directory, const char *message);
std::vector<std::string> runtime_arguments(const RuntimeLayout &layout);
std::vector<char *> mutable_arguments(std::vector<std::string> &arguments);
#endif
void remove_directory_if_present(const std::filesystem::path &directory);
void remove_directory_contents_if_present(const std::filesystem::path &directory);
int copy_error_message(mylite_db &db, char **errmsg);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_ok(mylite_db &db);
#endif
void set_error(mylite_db &db, int code, const char *message);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &db);
int parse_warning_level(const char *level);
#endif
const char *safe_c_str(const std::string &value);
bool has_config_field(const mylite_open_config *config, std::size_t field_end);

} // namespace

int mylite_open(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    return open_impl(path, out_db, flags, config);
}

int mylite_close(mylite_db *db) {
    if (db == nullptr) {
        return MYLITE_OK;
    }
    if (db->active_statement_count > 0U) {
        set_error(*db, MYLITE_BUSY, "database has active statements");
        return MYLITE_BUSY;
    }

    close_connection(*db);
    release_runtime();
    delete db;
    return MYLITE_OK;
}

int mylite_exec(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
) {
    return exec_impl(db, sql, callback, ctx, errmsg);
}

int mylite_prepare(
    mylite_db *db,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
) {
    return prepare_impl(db, sql, sql_len, out_stmt, tail);
}

int mylite_step(mylite_stmt *stmt) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    set_ok(*stmt->db);
    if (!stmt->executed) {
        const int bind_result = bind_parameters(*stmt);
        if (bind_result != MYLITE_OK) {
            return bind_result;
        }
        const int result_setup = initialize_statement_results(*stmt);
        if (result_setup != MYLITE_OK) {
            return result_setup;
        }
        if (mysql_stmt_execute(stmt->stmt) != 0) {
            set_mariadb_statement_error(*stmt);
            return MYLITE_ERROR;
        }

        stmt->db->changes = 0;
        stmt->db->last_insert_id =
            static_cast<unsigned long long>(mysql_stmt_insert_id(stmt->stmt));
        stmt->executed = true;

        if (!stmt->has_result) {
            const my_ulonglong affected_rows = mysql_stmt_affected_rows(stmt->stmt);
            stmt->db->changes = affected_rows == static_cast<my_ulonglong>(-1)
                                    ? 0
                                    : static_cast<long long>(std::min<my_ulonglong>(
                                          affected_rows,
                                          static_cast<my_ulonglong>(LLONG_MAX)
                                      ));
            return MYLITE_DONE;
        }
    }

    return stmt->has_result ? fetch_statement_row(*stmt) : MYLITE_DONE;
#endif
}

int mylite_reset(mylite_stmt *stmt) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    set_ok(*stmt->db);
    release_statement_results(*stmt);
    if (mysql_stmt_reset(stmt->stmt) != 0) {
        set_mariadb_statement_error(*stmt);
        return MYLITE_ERROR;
    }
    stmt->executed = false;
    stmt->has_result = false;
    stmt->has_row = false;
    return MYLITE_OK;
#endif
}

int mylite_finalize(mylite_stmt *stmt) {
    if (stmt == nullptr) {
        return MYLITE_OK;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    release_statement_results(*stmt);
    if (stmt->stmt != nullptr) {
        static_cast<void>(mysql_stmt_close(stmt->stmt));
    }
#endif
    if (stmt->db != nullptr && stmt->db->active_statement_count > 0U) {
        --stmt->db->active_statement_count;
    }
    delete stmt;
    return MYLITE_OK;
}

unsigned mylite_bind_parameter_count(mylite_stmt *stmt) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    return 0U;
#else
    return stmt != nullptr ? static_cast<unsigned>(stmt->parameters.size()) : 0U;
#endif
}

int mylite_clear_bindings(mylite_stmt *stmt) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    if (stmt->executed) {
        return MYLITE_MISUSE;
    }
    for (unsigned index = 1; index <= stmt->parameters.size(); ++index) {
        const int result = bind_null_value(*stmt, index);
        if (result != MYLITE_OK) {
            return result;
        }
    }
    return MYLITE_OK;
#endif
}

int mylite_bind_null(mylite_stmt *stmt, unsigned index) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    return bind_null_value(*stmt, index);
#endif
}

// Public binding APIs use SQLite-style index, value order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
int mylite_bind_int64(mylite_stmt *stmt, unsigned index, long long value) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    ParameterBinding *parameter = parameter_at(*stmt, index);
    if (parameter == nullptr || stmt->executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->int64_value = value;
    parameter->length = sizeof(parameter->int64_value);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_LONGLONG;
    parameter->bind.buffer = &parameter->int64_value;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    return MYLITE_OK;
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): public binding API uses SQLite-style index,
// value order.
int mylite_bind_uint64(mylite_stmt *stmt, unsigned index, unsigned long long value) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    ParameterBinding *parameter = parameter_at(*stmt, index);
    if (parameter == nullptr || stmt->executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->uint64_value = value;
    parameter->length = sizeof(parameter->uint64_value);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_LONGLONG;
    parameter->bind.buffer = &parameter->uint64_value;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    parameter->bind.is_unsigned = 1;
    return MYLITE_OK;
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): public binding API uses SQLite-style index,
// value order.
int mylite_bind_double(mylite_stmt *stmt, unsigned index, double value) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    ParameterBinding *parameter = parameter_at(*stmt, index);
    if (parameter == nullptr || stmt->executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->double_value = value;
    parameter->length = sizeof(parameter->double_value);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_DOUBLE;
    parameter->bind.buffer = &parameter->double_value;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    return MYLITE_OK;
#endif
}

// NOLINTEND(bugprone-easily-swappable-parameters)

int mylite_bind_text(
    mylite_stmt *stmt,
    unsigned index,
    const char *value,
    std::size_t value_len,
    mylite_destructor destructor
) {
    if (stmt == nullptr || stmt->db == nullptr || (value == nullptr && value_len != 0U)) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value_len;
    (void)destructor;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    const std::size_t resolved_len =
        value_len == MYLITE_NUL_TERMINATED ? std::strlen(value) : value_len;
    return bind_bytes(*stmt, index, value, resolved_len, MYSQL_TYPE_STRING, destructor);
#endif
}

int mylite_bind_blob(
    mylite_stmt *stmt,
    unsigned index,
    const void *value,
    std::size_t value_len,
    mylite_destructor destructor
) {
    if (stmt == nullptr || stmt->db == nullptr || (value == nullptr && value_len != 0U) ||
        value_len == MYLITE_NUL_TERMINATED) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value_len;
    (void)destructor;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    return bind_bytes(*stmt, index, value, value_len, MYSQL_TYPE_BLOB, destructor);
#endif
}

unsigned mylite_column_count(mylite_stmt *stmt) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    return 0U;
#else
    if (stmt == nullptr || stmt->stmt == nullptr) {
        return 0U;
    }
    if (!stmt->columns.empty()) {
        return static_cast<unsigned>(stmt->columns.size());
    }
    return mysql_stmt_field_count(stmt->stmt);
#endif
}

const char *mylite_column_name(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = column_at(stmt, column);
    return result_column != nullptr ? result_column->name.c_str() : nullptr;
#endif
}

mylite_value_type mylite_column_type(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return MYLITE_TYPE_NULL;
#else
    const ResultColumn *result_column = column_at(stmt, column);
    return result_column != nullptr ? column_type(*result_column) : MYLITE_TYPE_NULL;
#endif
}

long long mylite_column_int64(mylite_stmt *stmt, unsigned column) {
    const char *text = mylite_column_text(stmt, column);
    return text != nullptr ? std::strtoll(text, nullptr, k_decimal_base) : 0;
}

unsigned long long mylite_column_uint64(mylite_stmt *stmt, unsigned column) {
    const char *text = mylite_column_text(stmt, column);
    return text != nullptr ? std::strtoull(text, nullptr, k_decimal_base) : 0U;
}

double mylite_column_double(mylite_stmt *stmt, unsigned column) {
    const char *text = mylite_column_text(stmt, column);
    return text != nullptr ? std::strtod(text, nullptr) : 0.0;
}

const char *mylite_column_text(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = column_at(stmt, column);
    if (result_column == nullptr || result_column->is_null != 0) {
        return nullptr;
    }
    return reinterpret_cast<const char *>(result_column->buffer.data());
#endif
}

const void *mylite_column_blob(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = column_at(stmt, column);
    if (result_column == nullptr || result_column->is_null != 0) {
        return nullptr;
    }
    return result_column->buffer.data();
#endif
}

std::size_t mylite_column_bytes(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return 0U;
#else
    const ResultColumn *result_column = column_at(stmt, column);
    if (result_column == nullptr || result_column->is_null != 0) {
        return 0U;
    }
    return result_column->length;
#endif
}

int mylite_errcode(mylite_db *db) {
    return db != nullptr ? db->errcode : MYLITE_MISUSE;
}

int mylite_extended_errcode(mylite_db *db) {
    return db != nullptr ? db->extended_errcode : MYLITE_MISUSE;
}

unsigned mylite_mariadb_errno(mylite_db *db) {
    return db != nullptr ? db->mariadb_errno : 0;
}

const char *mylite_sqlstate(mylite_db *db) {
    return db != nullptr ? safe_c_str(db->sqlstate) : k_sqlstate_general;
}

const char *mylite_errmsg(mylite_db *db) {
    return db != nullptr ? safe_c_str(db->errmsg) : k_bad_db_handle;
}

unsigned mylite_warning_count(mylite_db *db) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)db;
    return 0U;
#else
    return db != nullptr ? mysql_warning_count(&db->mysql) : 0U;
#endif
}

// NOLINTBEGIN(readability-non-const-parameter): output parameters are part of the public C API.
int mylite_warning(
    mylite_db *db,
    unsigned index,
    mylite_warning_level *level,
    unsigned *code,
    const char **message
) {
    if (db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)level;
    (void)code;
    (void)message;
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    const unsigned warning_count = mysql_warning_count(&db->mysql);
    if (index >= warning_count) {
        return MYLITE_NOTFOUND;
    }

    const std::string sql = "SHOW WARNINGS LIMIT " + std::to_string(index) + ", 1";
    if (mysql_query(&db->mysql, sql.c_str()) != 0) {
        set_mariadb_error(*db);
        return MYLITE_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(&db->mysql);
    if (result == nullptr) {
        set_mariadb_error(*db);
        return MYLITE_ERROR;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == nullptr) {
        mysql_free_result(result);
        return MYLITE_NOTFOUND;
    }

    if (level != nullptr) {
        *level = static_cast<mylite_warning_level>(parse_warning_level(row[0]));
    }
    if (code != nullptr) {
        *code = static_cast<unsigned>(
            std::strtoul(row[1] != nullptr ? row[1] : "0", nullptr, k_decimal_base)
        );
    }
    db->warning_message = row[2] != nullptr ? row[2] : "";
    if (message != nullptr) {
        *message = db->warning_message.c_str();
    }

    mysql_free_result(result);
    set_ok(*db);
    return MYLITE_OK;
#endif
}

// NOLINTEND(readability-non-const-parameter)

long long mylite_changes(mylite_db *db) {
    return db != nullptr ? db->changes : 0;
}

unsigned long long mylite_last_insert_id(mylite_db *db) {
    return db != nullptr ? db->last_insert_id : 0;
}

void mylite_free(void *ptr) {
    std::free(ptr);
}

namespace {

int open_impl(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    const int validation_result = validate_open_args(path, out_db, flags, config);
    if (validation_result != MYLITE_OK) {
        return validation_result;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)config;
    return MYLITE_ERROR;
#else
    try {
        std::unique_ptr<mylite_db> db(new mylite_db());
        db->database_path = normalize_database_path(path).string();

        const int runtime_path_result = validate_runtime_database_path(*db);
        if (runtime_path_result != MYLITE_OK) {
            return runtime_path_result;
        }

        const int directory_result = prepare_database_directory(db->database_path, flags);
        if (directory_result != MYLITE_OK) {
            return directory_result;
        }

        const int runtime_result = start_runtime(*db, config);
        if (runtime_result != MYLITE_OK) {
            return runtime_result;
        }

        const int connect_result = connect_runtime(*db);
        if (connect_result != MYLITE_OK) {
            close_connection(*db);
            release_runtime();
            return connect_result;
        }

        *out_db = db.release();
        return MYLITE_OK;
    } catch (const std::bad_alloc &) {
        return MYLITE_NOMEM;
    } catch (const std::filesystem::filesystem_error &) {
        return MYLITE_IOERR;
    }
#endif
}

int validate_open_args(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    if (out_db == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_db = nullptr;

    if (path == nullptr || path[0] == '\0') {
        return MYLITE_MISUSE;
    }

    if ((flags & ~k_known_open_flags) != 0U) {
        return MYLITE_MISUSE;
    }

    const bool readonly = (flags & MYLITE_OPEN_READONLY) != 0U;
    const bool readwrite = (flags & MYLITE_OPEN_READWRITE) != 0U;
    if (readonly == readwrite) {
        return MYLITE_MISUSE;
    }

    if (readonly) {
        return MYLITE_MISUSE;
    }

    if ((flags & MYLITE_OPEN_URI) != 0U) {
        return MYLITE_MISUSE;
    }

    if (config != nullptr && config->size > 0U) {
        if (has_config_field(
                config,
                offsetof(mylite_open_config, profile) + sizeof(config->profile)
            ) &&
            config->profile != MYLITE_PROFILE_DEFAULT && config->profile != MYLITE_PROFILE_STRICT &&
            config->profile != MYLITE_PROFILE_COMPAT) {
            return MYLITE_MISUSE;
        }

        if (has_config_field(
                config,
                offsetof(mylite_open_config, durability) + sizeof(config->durability)
            ) &&
            config->durability != MYLITE_DURABILITY_OFF &&
            config->durability != MYLITE_DURABILITY_NORMAL &&
            config->durability != MYLITE_DURABILITY_FULL) {
            return MYLITE_MISUSE;
        }
    }

    return MYLITE_OK;
}

int exec_impl(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
) {
    if (errmsg != nullptr) {
        *errmsg = nullptr;
    }

    if (db == nullptr || sql == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)callback;
    (void)ctx;
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return copy_error_message(*db, errmsg);
#else
    set_ok(*db);
    if (reject_unsupported_sql_policy(*db, sql) != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    if (mysql_query(&db->mysql, sql) != 0) {
        set_mariadb_error(*db);
        return copy_error_message(*db, errmsg);
    }

    bool has_result = false;
    const int result = store_and_emit_result(*db, callback, ctx, &has_result);
    if (result != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    update_current_schema_after_successful_sql(*db, sql);

    const my_ulonglong affected_rows = mysql_affected_rows(&db->mysql);
    db->changes =
        has_result || affected_rows == static_cast<my_ulonglong>(-1)
            ? 0
            : static_cast<long long>(
                  std::min<my_ulonglong>(affected_rows, static_cast<my_ulonglong>(LLONG_MAX))
              );
    db->last_insert_id = static_cast<unsigned long long>(mysql_insert_id(&db->mysql));
    return MYLITE_OK;
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void update_current_schema_after_successful_sql(mylite_db &db, std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "USE") || tokens.count < 2U) {
        return;
    }
    db.current_schema = std::string(unquoted_identifier_token(tokens.values[1]));
}
#endif

int prepare_impl(
    mylite_db *db,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
) {
    if (out_stmt == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_stmt = nullptr;
    if (tail != nullptr) {
        *tail = nullptr;
    }
    if (db == nullptr || sql == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)sql_len;
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    const std::size_t resolved_len = sql_len == MYLITE_NUL_TERMINATED ? std::strlen(sql) : sql_len;
    if (resolved_len > ULONG_MAX) {
        return MYLITE_MISUSE;
    }
    set_ok(*db);
    if (reject_unsupported_sql_policy(*db, std::string_view(sql, resolved_len)) != MYLITE_OK) {
        return MYLITE_ERROR;
    }

    std::unique_ptr<mylite_stmt> statement(new mylite_stmt());
    statement->db = db;
    statement->stmt = mysql_stmt_init(&db->mysql);
    if (statement->stmt == nullptr) {
        set_error(*db, MYLITE_NOMEM, "statement could not be allocated");
        return MYLITE_NOMEM;
    }

    my_bool update_max_length = 1;
    static_cast<void>(
        mysql_stmt_attr_set(statement->stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length)
    );

    if (mysql_stmt_prepare(statement->stmt, sql, static_cast<unsigned long>(resolved_len)) != 0) {
        set_mariadb_statement_error(*statement);
        static_cast<void>(mysql_stmt_close(statement->stmt));
        statement->stmt = nullptr;
        return MYLITE_ERROR;
    }

    statement->parameters.resize(mysql_stmt_param_count(statement->stmt));
    for (unsigned index = 1; index <= statement->parameters.size(); ++index) {
        const int result = bind_null_value(*statement, index);
        if (result != MYLITE_OK) {
            static_cast<void>(mysql_stmt_close(statement->stmt));
            statement->stmt = nullptr;
            return result;
        }
    }

    if (tail != nullptr) {
        *tail = sql + resolved_len;
    }
    ++db->active_statement_count;
    *out_stmt = statement.release();
    set_ok(*db);
    return MYLITE_OK;
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
int reject_unsupported_sql_policy(mylite_db &db, std::string_view sql) {
    if (is_unsupported_oracle_sql_mode_statement(sql)) {
        set_error(db, MYLITE_ERROR, "Oracle SQL mode is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_procedure_analyse_statement(sql)) {
        set_error(db, MYLITE_ERROR, "PROCEDURE ANALYSE is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_server_surface_sql(sql, db.current_schema)) {
        set_error(db, MYLITE_ERROR, "server-owned SQL surface is not supported by MyLite");
        return MYLITE_ERROR;
    }

    return MYLITE_OK;
}

bool is_unsupported_oracle_sql_mode_statement(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "SQL_MODE") &&
            is_sql_mode_assignment_target(tokens, index) &&
            sql_mode_assignment_mentions_oracle(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_procedure_analyse_statement(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "SELECT")) {
        return false;
    }

    for (std::size_t index = 1; index + 2U < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "PROCEDURE") &&
            token_equals(tokens.values[index + 1U], "ANALYSE") &&
            token_equals(tokens.values[index + 2U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_server_surface_sql(std::string_view sql, const std::string &current_schema) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (identifier_token_at(tokens, 0).empty()) {
        return false;
    }

    return is_unsupported_account_or_event_statement(tokens) ||
           is_unsupported_plugin_statement(tokens) || is_unsupported_udf_statement(tokens) ||
           is_unsupported_replication_statement(tokens) ||
           is_unsupported_binlog_statement(tokens) || is_unsupported_help_statement(tokens) ||
           is_unsupported_static_show_info_statement(tokens) ||
           is_unsupported_statement_profiling_statement(tokens) ||
           is_unsupported_query_cache_statement(tokens) ||
           is_unsupported_query_log_statement(tokens) ||
           is_unsupported_optimizer_trace_statement(tokens, current_schema) ||
           is_unsupported_server_set_statement(tokens);
}

bool is_unsupported_account_or_event_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);
    const std::string_view fourth = identifier_token_at(tokens, 3);

    if (token_in(first, "GRANT", "REVOKE")) {
        return true;
    }
    if (token_equals(first, "CREATE")) {
        if (token_equals(second, "DEFINER")) {
            return has_identifier_token(tokens, "EVENT", 2);
        }
        if (token_equals(second, "OR") && token_equals(third, "REPLACE")) {
            return token_equals(fourth, "DEFINER")
                       ? has_identifier_token(tokens, "EVENT", 4)
                       : token_in(fourth, "USER", "ROLE", "EVENT", "SERVER");
        }
        return token_in(second, "USER", "ROLE", "EVENT", "SERVER");
    }
    if (token_equals(first, "ALTER")) {
        if (token_equals(second, "DEFINER")) {
            return has_identifier_token(tokens, "EVENT", 2);
        }
        return token_in(second, "USER", "EVENT", "SERVER");
    }
    if (token_equals(first, "DROP")) {
        return token_in(second, "USER", "ROLE", "EVENT", "SERVER");
    }
    return token_equals(first, "RENAME") && token_equals(second, "USER");
}

bool is_unsupported_plugin_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    return (token_equals(first, "INSTALL") || token_equals(first, "UNINSTALL")) &&
           token_in(second, "PLUGIN", "SONAME");
}

bool is_unsupported_udf_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);
    std::size_t function_index = 1;

    if (!token_equals(first, "CREATE")) {
        return false;
    }
    if (token_equals(second, "OR") && token_equals(third, "REPLACE")) {
        function_index = 3;
    }
    if (token_equals(identifier_token_at(tokens, function_index), "AGGREGATE")) {
        ++function_index;
    }
    return token_equals(identifier_token_at(tokens, function_index), "FUNCTION") &&
           has_identifier_token(tokens, "SONAME", function_index + 1U);
}

bool is_unsupported_replication_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "CHANGE") && token_in(second, "MASTER", "REPLICATION")) {
        return true;
    }
    if (token_equals(first, "RESET") && token_equals(second, "MASTER")) {
        return true;
    }
    return token_in(first, "START", "STOP", "RESET") && token_in(second, "SLAVE", "REPLICA");
}

bool is_unsupported_binlog_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    if (token_equals(first, "SHOW") && token_equals(second, "BINARY")) {
        return token_in(third, "LOGS", "STATUS");
    }
    if (token_equals(first, "SHOW") && token_equals(second, "BINLOG")) {
        return token_equals(third, "EVENTS");
    }
    if (token_equals(first, "SHOW") && token_in(second, "MASTER", "SLAVE", "REPLICA")) {
        return token_equals(third, "STATUS");
    }
    if (token_equals(first, "FLUSH") && token_equals(second, "BINARY")) {
        return token_equals(third, "LOGS");
    }
    if (token_equals(first, "RESET") && token_equals(second, "MASTER")) {
        return true;
    }
    return token_equals(first, "PURGE") && token_in(second, "BINARY", "MASTER");
}

bool is_unsupported_help_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "HELP");
}

bool is_unsupported_static_show_info_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "SHOW") &&
           token_in(identifier_token_at(tokens, 1), "AUTHORS", "CONTRIBUTORS", "PRIVILEGES");
}

bool is_unsupported_statement_profiling_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "SHOW") && token_in(second, "PROFILE", "PROFILES")) {
        return true;
    }
    if (!token_equals(first, "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_in(tokens.values[index], "PROFILING", "PROFILING_HISTORY_SIZE") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_query_cache_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    return (token_equals(first, "FLUSH") || token_equals(first, "RESET")) &&
           token_equals(second, "QUERY") && token_equals(third, "CACHE");
}

bool is_unsupported_query_log_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "FLUSH")) {
        const std::size_t flush_target_index =
            token_in(second, "LOCAL", "NO_WRITE_TO_BINLOG") ? 2U : 1U;
        const std::string_view flush_target = identifier_token_at(tokens, flush_target_index);
        const std::string_view flush_target_tail =
            identifier_token_at(tokens, flush_target_index + 1U);

        return token_equals(flush_target, "LOGS") || (token_in(flush_target, "GENERAL", "SLOW") &&
                                                      token_equals(flush_target_tail, "LOGS"));
    }
    if (!token_equals(first, "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (is_query_log_variable_token(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_optimizer_trace_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    if (has_information_schema_table(tokens, "OPTIMIZER_TRACE") ||
        has_current_schema_table_reference(tokens, "OPTIMIZER_TRACE", current_schema)) {
        return true;
    }
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_in(tokens.values[index], "OPTIMIZER_TRACE", "OPTIMIZER_TRACE_MAX_MEM_SIZE") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_server_set_statement(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "PASSWORD") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
        if (is_server_variable_token(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

SqlPolicyTokens collect_sql_policy_tokens(std::string_view sql) {
    std::size_t offset = 0;
    SqlPolicyTokens tokens = {};
    while (tokens.count < tokens.values.size() &&
           next_sql_token(sql, offset, tokens.values[tokens.count])) {
        ++tokens.count;
    }
    return tokens;
}

bool next_sql_token(std::string_view sql, std::size_t &offset, std::string_view &token) {
    skip_sql_spacing_and_comments(sql, offset);
    if (offset >= sql.size()) {
        token = {};
        return false;
    }

    const std::size_t start = offset;
    if (sql[offset] == '\'' || sql[offset] == '"' || sql[offset] == '`') {
        skip_quoted_sql_token(sql, offset);
        token = sql.substr(start, offset - start);
        return true;
    }

    if (is_sql_identifier_char(sql[offset])) {
        while (offset < sql.size() && is_sql_identifier_char(sql[offset])) {
            ++offset;
        }
        token = sql.substr(start, offset - start);
        return true;
    }

    ++offset;
    token = sql.substr(start, 1);
    return true;
}

void skip_sql_spacing_and_comments(std::string_view sql, std::size_t &offset) {
    for (;;) {
        while (offset < sql.size() && is_sql_space(sql[offset])) {
            ++offset;
        }
        if (enter_executable_sql_comment(sql, offset)) {
            continue;
        }
        if (skip_dash_sql_comment(sql, offset)) {
            continue;
        }
        if (skip_hash_sql_comment(sql, offset)) {
            continue;
        }
        if (skip_block_sql_comment(sql, offset)) {
            continue;
        }
        return;
    }
}

bool enter_executable_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset + 2U < sql.size() && sql[offset] == '/' && sql[offset + 1U] == '*' &&
        sql[offset + 2U] == '!') {
        offset += 3U;
    } else if (
        offset + 3U < sql.size() && sql[offset] == '/' && sql[offset + 1U] == '*' &&
        (sql[offset + 2U] == 'M' || sql[offset + 2U] == 'm') && sql[offset + 3U] == '!'
    ) {
        offset += 4U;
    } else {
        return false;
    }

    while (offset < sql.size() && sql[offset] >= '0' && sql[offset] <= '9') {
        ++offset;
    }
    return true;
}

bool skip_dash_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset + 1U >= sql.size() || sql[offset] != '-' || sql[offset + 1U] != '-') {
        return false;
    }
    offset += 2U;
    while (offset < sql.size() && sql[offset] != '\n') {
        ++offset;
    }
    return true;
}

bool skip_hash_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset >= sql.size() || sql[offset] != '#') {
        return false;
    }
    ++offset;
    while (offset < sql.size() && sql[offset] != '\n') {
        ++offset;
    }
    return true;
}

bool skip_block_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset + 1U >= sql.size() || sql[offset] != '/' || sql[offset + 1U] != '*') {
        return false;
    }
    offset += 2U;
    while (offset + 1U < sql.size() && (sql[offset] != '*' || sql[offset + 1U] != '/')) {
        ++offset;
    }
    if (offset + 1U < sql.size()) {
        offset += 2U;
    }
    return true;
}

void skip_quoted_sql_token(std::string_view sql, std::size_t &offset) {
    const char quote = sql[offset++];
    while (offset < sql.size()) {
        if (sql[offset] == '\\' && offset + 1U < sql.size()) {
            offset += 2U;
            continue;
        }
        if (sql[offset++] == quote) {
            return;
        }
    }
}

bool is_sql_space(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f';
}

bool is_sql_identifier_char(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

bool is_sql_identifier_token(std::string_view token) {
    return !token.empty() && is_sql_identifier_char(token[0]);
}

std::string_view identifier_token_at(const SqlPolicyTokens &tokens, std::size_t index) {
    std::size_t identifier_index = 0;
    for (std::size_t raw_index = 0; raw_index < tokens.count; ++raw_index) {
        if (!is_sql_identifier_token(tokens.values[raw_index])) {
            continue;
        }
        if (identifier_index == index) {
            return tokens.values[raw_index];
        }
        ++identifier_index;
    }
    return {};
}

std::string_view unquoted_identifier_token(std::string_view token) {
    if (token.size() >= 2U && token.front() == '`' && token.back() == '`') {
        token.remove_prefix(1U);
        token.remove_suffix(1U);
    }
    return token;
}

bool has_identifier_token(
    const SqlPolicyTokens &tokens,
    const char *keyword,
    std::size_t start_index
) {
    for (std::size_t index = start_index; !identifier_token_at(tokens, index).empty(); ++index) {
        if (token_equals(identifier_token_at(tokens, index), keyword)) {
            return true;
        }
    }
    return false;
}

bool has_information_schema_table(const SqlPolicyTokens &tokens, const char *table_name) {
    for (std::size_t index = 0; index + 2U < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], "INFORMATION_SCHEMA") &&
            token_equals(tokens.values[index + 1U], ".") &&
            identifier_token_equals(tokens.values[index + 2U], table_name)) {
            return true;
        }
    }
    return false;
}

bool has_current_schema_table_reference(
    const SqlPolicyTokens &tokens,
    const char *table_name,
    std::string_view current_schema
) {
    return identifier_token_equals(current_schema, "INFORMATION_SCHEMA") &&
           has_unqualified_table_reference(tokens, table_name);
}

bool has_unqualified_table_reference(const SqlPolicyTokens &tokens, const char *table_name) {
    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], table_name) &&
            table_reference_keyword(tokens.values[index - 1U])) {
            return true;
        }
    }
    return false;
}

bool is_sql_mode_assignment_target(const SqlPolicyTokens &tokens, std::size_t index) {
    return is_system_variable_qualified_token(tokens, index);
}

bool sql_mode_assignment_mentions_oracle(const SqlPolicyTokens &tokens, std::size_t index) {
    int paren_depth = 0;
    for (std::size_t value_index = index + 2U; value_index < tokens.count; ++value_index) {
        const std::string_view token = tokens.values[value_index];
        if (token_equals(token, "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(token, ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth == 0 && token_equals(token, ",")) {
            return false;
        }
        if (token_contains_sql_mode_name(token, "ORACLE")) {
            return true;
        }
    }
    return false;
}

bool token_contains_sql_mode_name(std::string_view token, const char *mode_name) {
    const std::size_t mode_length = std::strlen(mode_name);
    if (mode_length == 0U || token.size() < mode_length) {
        return false;
    }

    for (std::size_t start = 0; start + mode_length <= token.size(); ++start) {
        bool matches = true;
        for (std::size_t offset = 0; offset < mode_length; ++offset) {
            char left = token[start + offset];
            char right = mode_name[offset];
            if (left >= 'a' && left <= 'z') {
                left = static_cast<char>(left - ('a' - 'A'));
            }
            if (right >= 'a' && right <= 'z') {
                right = static_cast<char>(right - ('a' - 'A'));
            }
            if (left != right) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }

        const bool left_boundary = start == 0U || !is_sql_identifier_char(token[start - 1U]);
        const std::size_t end = start + mode_length;
        const bool right_boundary = end == token.size() || !is_sql_identifier_char(token[end]);
        if (left_boundary && right_boundary) {
            return true;
        }
    }
    return false;
}

bool token_equals(std::string_view token, const char *keyword) {
    const std::size_t keyword_length = std::strlen(keyword);
    if (token.size() != keyword_length) {
        return false;
    }
    for (std::size_t index = 0; index < token.size(); ++index) {
        char left = token[index];
        char right = keyword[index];
        if (left >= 'a' && left <= 'z') {
            left = static_cast<char>(left - ('a' - 'A'));
        }
        if (right >= 'a' && right <= 'z') {
            right = static_cast<char>(right - ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

bool identifier_token_equals(std::string_view token, const char *keyword) {
    return token_equals(unquoted_identifier_token(token), keyword);
}

bool table_reference_keyword(std::string_view token) {
    return token_in(token, "FROM", "JOIN", "UPDATE") ||
           token_in(token, "INTO", "TABLE", "DESC", "DESCRIBE");
}

bool token_in(std::string_view token, const char *first, const char *second) {
    return token_equals(token, first) || token_equals(token, second);
}

bool token_in(std::string_view token, const char *first, const char *second, const char *third) {
    return token_equals(token, first) || token_equals(token, second) || token_equals(token, third);
}

bool token_in(
    std::string_view token,
    const char *first,
    const char *second,
    const char *third,
    const char *fourth
) {
    return token_equals(token, first) || token_equals(token, second) ||
           token_equals(token, third) || token_equals(token, fourth);
}

bool is_server_variable_token(std::string_view token) {
    return token_in(token, "EVENT_SCHEDULER", "SQL_LOG_BIN", "LOG_BIN", "BINLOG_FORMAT") ||
           token_in(token, "QUERY_CACHE_SIZE", "QUERY_CACHE_TYPE", "QUERY_CACHE_LIMIT") ||
           token_in(
               token,
               "QUERY_CACHE_MIN_RES_UNIT",
               "QUERY_CACHE_WLOCK_INVALIDATE",
               "QUERY_CACHE_STRIP_COMMENTS"
           );
}

bool is_query_log_variable_token(std::string_view token) {
    return token_in(token, "GENERAL_LOG", "GENERAL_LOG_FILE", "LOG_OUTPUT") ||
           token_in(token, "SLOW_QUERY_LOG", "SLOW_QUERY_LOG_FILE", "LOG_SLOW_QUERY") ||
           token_in(token, "LOG_SLOW_QUERY_FILE", "SQL_LOG_OFF", "LONG_QUERY_TIME") ||
           token_in(
               token,
               "MIN_EXAMINED_ROW_LIMIT",
               "LOG_SLOW_MIN_EXAMINED_ROW_LIMIT",
               "LOG_SLOW_RATE_LIMIT"
           ) ||
           token_in(
               token,
               "LOG_SLOW_FILTER",
               "LOG_SLOW_VERBOSITY",
               "LOG_SLOW_DISABLED_STATEMENTS"
           ) ||
           token_in(
               token,
               "LOG_SLOW_ADMIN_STATEMENTS",
               "LOG_SLOW_SLAVE_STATEMENTS",
               "LOG_SLOW_MAX_WARNINGS"
           );
}

bool is_system_variable_qualified_token(const SqlPolicyTokens &tokens, std::size_t index) {
    if (index + 1U >= tokens.count || !token_equals(tokens.values[index + 1U], "=")) {
        return false;
    }
    if (is_system_variable_assignment_start(tokens, index)) {
        return true;
    }
    if (token_in(tokens.values[index - 1U], "GLOBAL", "SESSION", "LOCAL")) {
        return is_system_variable_assignment_start(tokens, index - 1U);
    }
    if (index >= 2U && token_equals(tokens.values[index - 1U], "@") &&
        token_equals(tokens.values[index - 2U], "@")) {
        return true;
    }
    return index >= 4U && token_equals(tokens.values[index - 1U], ".") &&
           token_in(tokens.values[index - 2U], "GLOBAL", "SESSION", "LOCAL") &&
           token_equals(tokens.values[index - 3U], "@") &&
           token_equals(tokens.values[index - 4U], "@");
}

bool is_system_variable_assignment_start(const SqlPolicyTokens &tokens, std::size_t index) {
    const std::size_t first_assignment = first_set_assignment_token_index(tokens);
    int paren_depth = 0;

    if (index == first_assignment) {
        return true;
    }
    for (std::size_t token_index = first_assignment; token_index < index; ++token_index) {
        if (token_equals(tokens.values[token_index], "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(tokens.values[token_index], ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth == 0 && first_assignment == 2U &&
            token_equals(tokens.values[token_index], "FOR")) {
            return false;
        }
    }
    return index > 0U && paren_depth == 0 && token_equals(tokens.values[index - 1U], ",");
}

std::size_t first_set_assignment_token_index(const SqlPolicyTokens &tokens) {
    if (tokens.count > 2U && token_equals(tokens.values[1], "STATEMENT") &&
        !token_equals(tokens.values[2], "=")) {
        return 2U;
    }
    return 1U;
}

int validate_runtime_database_path(mylite_db &db) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count > 0U && g_runtime.database_path != db.database_path) {
        set_error(db, MYLITE_BUSY, "embedded runtime is already open for another database");
        return MYLITE_BUSY;
    }
    return MYLITE_OK;
}

int store_and_emit_result(
    mylite_db &db,
    mylite_exec_callback callback,
    void *ctx,
    bool *has_result
) {
    MYSQL_RES *result = mysql_store_result(&db.mysql);
    if (result == nullptr) {
        if (mysql_field_count(&db.mysql) != 0U) {
            set_mariadb_error(db);
            return MYLITE_ERROR;
        }
        return MYLITE_OK;
    }
    *has_result = true;

    const unsigned field_count = mysql_num_fields(result);
    if (field_count > static_cast<unsigned>(INT_MAX)) {
        mysql_free_result(result);
        set_error(db, MYLITE_ERROR, "result has too many columns");
        return MYLITE_ERROR;
    }

    std::vector<char *> column_names;
    column_names.reserve(field_count);
    const MYSQL_FIELD *fields = mysql_fetch_fields(result);
    for (unsigned i = 0; i < field_count; ++i) {
        column_names.push_back(fields[i].name);
    }

    for (MYSQL_ROW row = mysql_fetch_row(result); row != nullptr; row = mysql_fetch_row(result)) {
        if (callback != nullptr &&
            callback(ctx, static_cast<int>(field_count), row, column_names.data()) != 0) {
            mysql_free_result(result);
            set_error(db, MYLITE_ERROR, "query callback requested abort");
            return MYLITE_ERROR;
        }
    }

    mysql_free_result(result);
    return MYLITE_OK;
}

int initialize_statement_results(mylite_stmt &stmt) {
    release_statement_results(stmt);

    stmt.metadata = mysql_stmt_result_metadata(stmt.stmt);
    if (stmt.metadata == nullptr) {
        if (mysql_stmt_field_count(stmt.stmt) != 0U) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }
        stmt.has_result = false;
        return MYLITE_OK;
    }

    const unsigned field_count = mysql_num_fields(stmt.metadata);
    const MYSQL_FIELD *fields = mysql_fetch_fields(stmt.metadata);
    stmt.columns.resize(field_count);
    stmt.result_binds.resize(field_count);
    for (unsigned index = 0; index < field_count; ++index) {
        ResultColumn &column = stmt.columns[index];
        column.field_type = fields[index].type;
        column.flags = fields[index].flags;
        column.name = fields[index].name != nullptr ? fields[index].name : "";
        column.length = 0;
        column.is_null = 0;
        column.error = 0;
        column.bind = {};
        column.bind.buffer_type = MYSQL_TYPE_STRING;
        column.bind.length = &column.length;
        column.bind.is_null = &column.is_null;
        column.bind.error = &column.error;
        const unsigned long buffer_length =
            std::min(std::max(fields[index].length, 1UL), k_initial_result_buffer_size);
        if (configure_column_buffer(column, buffer_length) != MYLITE_OK) {
            set_error(*stmt.db, MYLITE_NOMEM, "result column buffer could not be allocated");
            return MYLITE_NOMEM;
        }
        stmt.result_binds[index] = column.bind;
    }

    if (mysql_stmt_bind_result(stmt.stmt, stmt.result_binds.data()) != 0) {
        set_mariadb_statement_error(stmt);
        return MYLITE_ERROR;
    }
    stmt.has_result = true;
    return MYLITE_OK;
}

int fetch_statement_row(mylite_stmt &stmt) {
    const int fetch_result = mysql_stmt_fetch(stmt.stmt);
    if (fetch_result == MYSQL_NO_DATA) {
        stmt.has_row = false;
        return MYLITE_DONE;
    }
    if (fetch_result != 0 && fetch_result != MYSQL_DATA_TRUNCATED) {
        set_mariadb_statement_error(stmt);
        return MYLITE_ERROR;
    }
    if (fetch_result == MYSQL_DATA_TRUNCATED) {
        const int truncated_result = fetch_truncated_statement_columns(stmt);
        if (truncated_result != MYLITE_OK) {
            return truncated_result;
        }
    }

    for (ResultColumn &column : stmt.columns) {
        if (column.length < column.buffer.size()) {
            column.buffer[column.length] = 0U;
        } else if (!column.buffer.empty()) {
            column.buffer.back() = 0U;
        }
    }
    stmt.has_row = true;
    return MYLITE_ROW;
}

int fetch_truncated_statement_columns(mylite_stmt &stmt) {
    for (unsigned index = 0; index < stmt.columns.size(); ++index) {
        ResultColumn &column = stmt.columns[index];
        if (column.is_null != 0 || column.error == 0) {
            continue;
        }
        if (column.length == ULONG_MAX) {
            set_error(*stmt.db, MYLITE_ERROR, "result column is too large");
            return MYLITE_ERROR;
        }

        const unsigned long buffer_length = std::max(column.length, 1UL);
        if (configure_column_buffer(column, buffer_length) != MYLITE_OK) {
            set_error(*stmt.db, MYLITE_NOMEM, "result column buffer could not be allocated");
            return MYLITE_NOMEM;
        }
        stmt.result_binds[index] = column.bind;
        if (mysql_stmt_fetch_column(stmt.stmt, &stmt.result_binds[index], index, 0) != 0) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }
    }
    return MYLITE_OK;
}

int configure_column_buffer(ResultColumn &column, unsigned long buffer_length) {
    if (buffer_length == ULONG_MAX) {
        return MYLITE_NOMEM;
    }

    try {
        column.buffer.assign(buffer_length + 1UL, 0U);
    } catch (const std::bad_alloc &) {
        return MYLITE_NOMEM;
    }
    column.bind.buffer = column.buffer.data();
    column.bind.buffer_length = buffer_length;
    return MYLITE_OK;
}

void release_statement_results(mylite_stmt &stmt) {
    if (stmt.stmt != nullptr) {
        static_cast<void>(mysql_stmt_free_result(stmt.stmt));
    }
    if (stmt.metadata != nullptr) {
        mysql_free_result(stmt.metadata);
        stmt.metadata = nullptr;
    }
    stmt.columns.clear();
    stmt.result_binds.clear();
    stmt.has_result = false;
    stmt.has_row = false;
}

ParameterBinding *parameter_at(mylite_stmt &stmt, unsigned index) {
    if (index == 0U || index > stmt.parameters.size()) {
        return nullptr;
    }
    return &stmt.parameters[index - 1U];
}

int bind_null_value(mylite_stmt &stmt, unsigned index) {
    ParameterBinding *parameter = parameter_at(stmt, index);
    if (parameter == nullptr || stmt.executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->length = 0;
    parameter->is_null = 1;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_NULL;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    return MYLITE_OK;
}

int bind_bytes(
    mylite_stmt &stmt,
    unsigned index,
    const void *value,
    std::size_t value_len,
    enum enum_field_types buffer_type,
    mylite_destructor destructor
) {
    ParameterBinding *parameter = parameter_at(stmt, index);
    if (parameter == nullptr || stmt.executed || value_len > ULONG_MAX) {
        return MYLITE_MISUSE;
    }

    const auto *bytes = static_cast<const unsigned char *>(value);
    parameter->bytes.clear();
    if (value_len > 0U) {
        parameter->bytes.assign(bytes, bytes + value_len);
    }
    if (parameter->bytes.empty()) {
        parameter->bytes.push_back(0U);
    }
    parameter->length = static_cast<unsigned long>(value_len);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = buffer_type;
    bind_parameter_buffer(*parameter);

    // MYLITE_TRANSIENT mirrors SQLite's public -1 destructor sentinel.
    // NOLINTBEGIN(performance-no-int-to-ptr)
    if (value != nullptr && destructor != MYLITE_STATIC && destructor != MYLITE_TRANSIENT) {
        destructor(const_cast<void *>(value));
    }
    // NOLINTEND(performance-no-int-to-ptr)
    return MYLITE_OK;
}

void bind_parameter_buffer(ParameterBinding &parameter) {
    parameter.bind.buffer =
        parameter.bytes.empty() ? nullptr : static_cast<void *>(parameter.bytes.data());
    parameter.bind.buffer_length = parameter.length;
    parameter.bind.length = &parameter.length;
    parameter.bind.is_null = &parameter.is_null;
    parameter.bind.error = &parameter.error;
}

int bind_parameters(mylite_stmt &stmt) {
    if (stmt.parameters.empty()) {
        return MYLITE_OK;
    }
    stmt.parameter_binds.resize(stmt.parameters.size());
    for (std::size_t index = 0; index < stmt.parameters.size(); ++index) {
        stmt.parameter_binds[index] = stmt.parameters[index].bind;
    }
    if (mysql_stmt_bind_param(stmt.stmt, stmt.parameter_binds.data()) != 0) {
        set_mariadb_statement_error(stmt);
        return MYLITE_ERROR;
    }
    return MYLITE_OK;
}

mylite_value_type column_type(const ResultColumn &column) {
    if (column.is_null != 0) {
        return MYLITE_TYPE_NULL;
    }

    switch (column.field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
        return (column.flags & UNSIGNED_FLAG) != 0U ? MYLITE_TYPE_UINT64 : MYLITE_TYPE_INT64;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
        return MYLITE_TYPE_DOUBLE;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
        return (column.flags & BINARY_FLAG) != 0U ? MYLITE_TYPE_BLOB : MYLITE_TYPE_TEXT;
    default:
        return MYLITE_TYPE_TEXT;
    }
}

const ResultColumn *column_at(const mylite_stmt *stmt, unsigned column) {
    if (stmt == nullptr || !stmt->has_row || column >= stmt->columns.size()) {
        return nullptr;
    }
    return &stmt->columns[column];
}

int prepare_database_directory(const std::filesystem::path &database_path, unsigned flags) {
    if (is_memory_database_path(database_path)) {
        return MYLITE_OK;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(database_path, error);
    if (error) {
        return MYLITE_IOERR;
    }

    if ((flags & MYLITE_OPEN_EXCLUSIVE) != 0U && exists) {
        return MYLITE_ERROR;
    }

    if (!exists && (flags & MYLITE_OPEN_CREATE) == 0U) {
        return MYLITE_NOTFOUND;
    }

    if (exists) {
        const bool is_directory = std::filesystem::is_directory(database_path, error);
        if (error || !is_directory) {
            return MYLITE_IOERR;
        }
        return prepare_existing_database_directory(database_path, flags);
    }

    std::filesystem::create_directories(database_path, error);
    if (error) {
        return MYLITE_IOERR;
    }
    initialize_database_layout(database_path);

    return MYLITE_OK;
}

int prepare_existing_database_directory(
    const std::filesystem::path &database_path,
    unsigned flags
) {
    const std::filesystem::path metadata_path = database_path / k_meta_filename;
    std::error_code error;
    const bool metadata_exists = std::filesystem::exists(metadata_path, error);
    if (error) {
        return MYLITE_IOERR;
    }

    if (!metadata_exists) {
        const bool empty = database_directory_is_empty(database_path, error);
        if (error) {
            return MYLITE_IOERR;
        }
        if (empty && (flags & MYLITE_OPEN_CREATE) != 0U) {
            initialize_database_layout(database_path);
            return MYLITE_OK;
        }
        return empty ? MYLITE_NOTFOUND : MYLITE_CORRUPT;
    }

    const int layout_result = validate_database_layout(database_path);
    if (layout_result != MYLITE_OK) {
        return layout_result;
    }
    return MYLITE_OK;
}

int validate_database_layout(const std::filesystem::path &database_path) {
    const std::filesystem::path metadata_path = database_path / k_meta_filename;
    std::error_code error;

    const bool metadata_is_file = std::filesystem::is_regular_file(metadata_path, error);
    if (error || !metadata_is_file) {
        return error ? MYLITE_IOERR : MYLITE_CORRUPT;
    }

    const int metadata_result = validate_database_metadata(metadata_path);
    if (metadata_result != MYLITE_OK) {
        return metadata_result;
    }

    const int data_result = validate_layout_directory(database_path / k_datadir_name);
    if (data_result != MYLITE_OK) {
        return data_result;
    }

    const int tmp_result = validate_layout_directory(database_path / k_tmpdir_name);
    if (tmp_result != MYLITE_OK) {
        return tmp_result;
    }

    return MYLITE_OK;
}

int validate_layout_directory(const std::filesystem::path &directory) {
    std::error_code error;
    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        return MYLITE_IOERR;
    }
    if (!exists) {
        return MYLITE_CORRUPT;
    }

    const bool is_directory = std::filesystem::is_directory(directory, error);
    if (error) {
        return MYLITE_IOERR;
    }
    return is_directory ? MYLITE_OK : MYLITE_CORRUPT;
}

int validate_database_metadata(const std::filesystem::path &metadata_path) {
    std::ifstream metadata(metadata_path, std::ios::binary);
    if (!metadata) {
        return MYLITE_IOERR;
    }

    bool has_format = false;
    bool has_mariadb_base = false;
    const std::string mariadb_base_line = std::string("mariadb_base=") + k_mariadb_base_ref;
    for (std::string line; std::getline(metadata, line);) {
        if (line == k_metadata_format_line) {
            has_format = true;
            continue;
        }
        if (line == mariadb_base_line) {
            has_mariadb_base = true;
        }
    }
    if (!metadata.eof()) {
        return MYLITE_IOERR;
    }

    return has_format && has_mariadb_base ? MYLITE_OK : MYLITE_CORRUPT;
}

bool database_directory_is_empty(
    const std::filesystem::path &database_path,
    std::error_code &error
) {
    const std::filesystem::directory_iterator entry(database_path, error);
    if (error) {
        return false;
    }
    return entry == std::filesystem::directory_iterator();
}

int start_runtime(mylite_db &db, const mylite_open_config *config) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count > 0U) {
        if (g_runtime.database_path != db.database_path) {
            set_error(db, MYLITE_BUSY, "embedded runtime is already open for another database");
            return MYLITE_BUSY;
        }
        ++g_runtime.ref_count;
        return MYLITE_OK;
    }

    int lock_fd = -1;
    if (!is_memory_database_path(db.database_path)) {
        lock_fd = acquire_database_lock(db, db.database_path, config);
        if (lock_fd < 0) {
            return db.errcode;
        }
    }

    try {
        const RuntimeLayout layout = create_runtime_layout(db.database_path, config);
        g_runtime.arguments = runtime_arguments(layout);
        g_runtime.argv = mutable_arguments(g_runtime.arguments);
        g_runtime.cleanup_directory = layout.cleanup_directory;
        g_runtime.persistent_tmp_directory = layout.persistent_tmp_directory;
        g_runtime.database_path = db.database_path;
        char *groups[] = {const_cast<char *>("server"), const_cast<char *>("embedded"), nullptr};

        const int init_result = mysql_server_init(
            static_cast<int>(g_runtime.argv.size()),
            g_runtime.argv.data(),
            groups
        );
        if (init_result != 0) {
            g_runtime.argv.clear();
            g_runtime.arguments.clear();
            g_runtime.cleanup_directory.clear();
            g_runtime.persistent_tmp_directory.clear();
            g_runtime.database_path.clear();
            remove_directory_if_present(layout.cleanup_directory);
            release_database_lock(lock_fd);
            set_error(db, MYLITE_ERROR, "MariaDB embedded runtime initialization failed");
            return MYLITE_ERROR;
        }

        g_runtime.ref_count = 1;
        g_runtime.lock_fd = lock_fd;
        return MYLITE_OK;
    } catch (...) {
        g_runtime.argv.clear();
        g_runtime.arguments.clear();
        g_runtime.cleanup_directory.clear();
        g_runtime.persistent_tmp_directory.clear();
        g_runtime.database_path.clear();
        release_database_lock(lock_fd);
        throw;
    }
}

int connect_runtime(mylite_db &db) {
    if (mysql_init(&db.mysql) == nullptr) {
        set_error(db, MYLITE_NOMEM, "MariaDB connection allocation failed");
        return MYLITE_NOMEM;
    }

    const MYSQL *connection =
        mysql_real_connect(&db.mysql, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0);
    if (connection == nullptr) {
        set_mariadb_error(db);
        return MYLITE_ERROR;
    }

    db.connected = true;
    return MYLITE_OK;
}
#endif

void close_connection(mylite_db &db) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (db.connected) {
        mysql_close(&db.mysql);
        db.connected = false;
    }
#else
    (void)db;
#endif
}

void release_runtime(void) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count == 0U) {
        return;
    }

    --g_runtime.ref_count;
    if (g_runtime.ref_count > 0U) {
        return;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    mysql_server_end();
#endif
    remove_directory_if_present(g_runtime.cleanup_directory);
    remove_directory_contents_if_present(g_runtime.persistent_tmp_directory);
#if MYLITE_WITH_MARIADB_EMBEDDED
    release_database_lock(g_runtime.lock_fd);
    g_runtime.lock_fd = -1;
#endif

    g_runtime.cleanup_directory.clear();
    g_runtime.persistent_tmp_directory.clear();
    g_runtime.database_path.clear();
    g_runtime.argv.clear();
    g_runtime.arguments.clear();
}

#if MYLITE_WITH_MARIADB_EMBEDDED
std::filesystem::path normalize_database_path(const char *path) {
    if (std::strcmp(path, k_memory_database_path) == 0) {
        return std::filesystem::path(k_memory_database_path);
    }
    return std::filesystem::absolute(std::filesystem::path(path));
}

bool is_memory_database_path(const std::filesystem::path &database_path) {
    return database_path == std::filesystem::path(k_memory_database_path);
}

void initialize_database_layout(const std::filesystem::path &database_path) {
    create_layout_directory(database_path / k_datadir_name, "create database data directory");
    create_layout_directory(database_path / k_tmpdir_name, "create database temporary directory");

    const std::filesystem::path metadata_path = database_path / k_meta_filename;
    std::error_code error;
    if (std::filesystem::exists(metadata_path, error)) {
        if (error || !std::filesystem::is_regular_file(metadata_path, error) || error) {
            throw std::filesystem::filesystem_error(
                "validate database metadata",
                metadata_path,
                error ? error : std::make_error_code(std::errc::invalid_argument)
            );
        }
        return;
    }
    if (error) {
        throw std::filesystem::filesystem_error("validate database metadata", metadata_path, error);
    }

    write_database_metadata(metadata_path);
}

void create_layout_directory(const std::filesystem::path &directory, const char *message) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(message, directory, error);
    }
}

void write_database_metadata(const std::filesystem::path &metadata_path) {
    std::ofstream metadata(metadata_path, std::ios::binary | std::ios::trunc);
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "create database metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }

    metadata << k_metadata_format_line << "\n";
    metadata << "mariadb_base=" << k_mariadb_base_ref << "\n";
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "write database metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }
}

RuntimeLayout create_runtime_layout(
    const std::filesystem::path &database_path,
    const mylite_open_config *config
) {
    if (is_memory_database_path(database_path)) {
        return create_memory_runtime_layout(config);
    }
    return create_persistent_runtime_layout(database_path);
}

RuntimeLayout create_memory_runtime_layout(const mylite_open_config *config) {
    const std::filesystem::path root = runtime_root(config);
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        throw std::filesystem::filesystem_error("create runtime root", root, error);
    }

    for (int attempt = 0; attempt < k_runtime_directory_attempts; ++attempt) {
        const std::filesystem::path candidate = root / unique_runtime_name();
        if (std::filesystem::create_directory(candidate, error)) {
            RuntimeLayout layout = {};
            layout.cleanup_directory = candidate;
            layout.data_directory = candidate / "data";
            layout.tmp_directory = candidate / "tmp";
            layout.plugin_directory = candidate / "plugins";
            create_runtime_subdirectory(layout.data_directory, "create runtime data directory");
            create_runtime_subdirectory(layout.tmp_directory, "create runtime temporary directory");
            create_runtime_subdirectory(layout.plugin_directory, "create runtime plugin directory");
            return layout;
        }
        if (error) {
            throw std::filesystem::filesystem_error("create runtime directory", candidate, error);
        }
    }

    throw std::filesystem::filesystem_error(
        "create runtime directory",
        root,
        std::make_error_code(std::errc::file_exists)
    );
}

RuntimeLayout create_persistent_runtime_layout(const std::filesystem::path &database_path) {
    RuntimeLayout layout = {};
    layout.cleanup_directory = database_path / k_rundir_name;
    layout.persistent_tmp_directory = database_path / k_tmpdir_name;
    layout.data_directory = database_path / k_datadir_name;
    layout.tmp_directory = layout.persistent_tmp_directory;
    layout.plugin_directory = layout.cleanup_directory / k_plugin_directory_name;

    remove_directory_if_present(layout.cleanup_directory);
    create_runtime_subdirectory(layout.cleanup_directory, "create database runtime directory");
    create_runtime_subdirectory(layout.tmp_directory, "create database temporary directory");
    create_runtime_subdirectory(layout.plugin_directory, "create database plugin directory");
    return layout;
}

int acquire_database_lock(
    mylite_db &db,
    const std::filesystem::path &database_path,
    const mylite_open_config *config
) {
    const std::filesystem::path lock_path = database_path / k_lock_filename;
    const std::string lock_name = lock_path.string();
    const int lock_fd = ::open(lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        set_error(db, MYLITE_IOERR, "database lock file could not be opened");
        return -1;
    }

    DatabaseLockWait wait = {};
    wait.lock_fd = lock_fd;
    wait.busy_timeout_ms = configured_busy_timeout_ms(config);
    const int lock_result = wait_for_database_lock(wait);
    if (lock_result == MYLITE_OK) {
        return lock_fd;
    }

    release_database_lock(lock_fd);
    if (lock_result == MYLITE_BUSY) {
        set_error(db, MYLITE_BUSY, "database directory is locked by another process");
    } else {
        set_error(db, MYLITE_IOERR, "database lock could not be acquired");
    }
    return -1;
}

int wait_for_database_lock(DatabaseLockWait wait) {
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(wait.busy_timeout_ms);
    for (;;) {
        if (::flock(wait.lock_fd, LOCK_EX | LOCK_NB) == 0) {
            return MYLITE_OK;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            return MYLITE_IOERR;
        }
        if (wait.busy_timeout_ms == 0U || std::chrono::steady_clock::now() - start >= timeout) {
            return MYLITE_BUSY;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(k_lock_poll_interval_ms));
    }
}

void release_database_lock(int lock_fd) {
    if (lock_fd >= 0) {
        static_cast<void>(::flock(lock_fd, LOCK_UN));
        static_cast<void>(::close(lock_fd));
    }
}

unsigned configured_busy_timeout_ms(const mylite_open_config *config) {
    if (has_config_field(
            config,
            offsetof(mylite_open_config, busy_timeout_ms) + sizeof(config->busy_timeout_ms)
        )) {
        return config->busy_timeout_ms;
    }
    return 0U;
}

std::filesystem::path runtime_root(const mylite_open_config *config) {
    if (config != nullptr &&
        has_config_field(
            config,
            offsetof(mylite_open_config, temp_directory) + sizeof(config->temp_directory)
        ) &&
        config->temp_directory != nullptr && config->temp_directory[0] != '\0') {
        return std::filesystem::path(config->temp_directory);
    }
    return std::filesystem::temp_directory_path();
}

std::string unique_runtime_name(void) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    static unsigned counter = 0;
    return "mylite-runtime-" + std::to_string(now) + "-" + std::to_string(++counter);
}

void create_runtime_subdirectory(const std::filesystem::path &directory, const char *message) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(message, directory, error);
    }
}

std::vector<std::string> runtime_arguments(const RuntimeLayout &layout) {
    return {
        "mylite",
        "--no-defaults",
        "--datadir=" + layout.data_directory.string(),
        "--tmpdir=" + layout.tmp_directory.string(),
        "--plugin-dir=" + layout.plugin_directory.string(),
        "--aria-log-dir-path=" + layout.data_directory.string(),
        "--innodb-data-home-dir=" + layout.data_directory.string(),
        "--innodb-log-group-home-dir=" + layout.data_directory.string(),
        "--innodb-undo-directory=" + layout.data_directory.string(),
        "--innodb-tmpdir=" + layout.tmp_directory.string(),
        std::string("--innodb-temp-data-file-path=") + k_innodb_temp_data_file_path,
        "--innodb-flush-log-at-trx-commit=1",
        "--innodb-fast-shutdown=1",
        "--log-output=NONE",
        "--skip-log-bin",
        "--skip-slave-start",
        "--skip-grant-tables",
        "--skip-networking",
#  ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
        "--performance-schema=OFF",
#  endif
        "--default-storage-engine=MyISAM",
        std::string("--lc-messages-dir=") + MYLITE_MARIADB_MESSAGES_DIR,
        std::string("--character-sets-dir=") + MYLITE_MARIADB_CHARSETS_DIR,
    };
}

std::vector<char *> mutable_arguments(std::vector<std::string> &arguments) {
    std::vector<char *> argv;
    argv.reserve(arguments.size());
    std::transform(
        arguments.begin(),
        arguments.end(),
        std::back_inserter(argv),
        [](std::string &argument) { return argument.data(); }
    );
    return argv;
}
#endif

void remove_directory_if_present(const std::filesystem::path &directory) {
    if (directory.empty()) {
        return;
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void remove_directory_contents_if_present(const std::filesystem::path &directory) {
    if (directory.empty()) {
        return;
    }

    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return;
    }

    std::filesystem::directory_iterator entry(directory, error);
    const std::filesystem::directory_iterator end;
    for (; entry != end && !error; entry.increment(error)) {
        std::error_code ignored;
        std::filesystem::remove_all(entry->path(), ignored);
    }
}

int copy_error_message(mylite_db &db, char **errmsg) {
    if (errmsg == nullptr) {
        return db.errcode;
    }

    const std::size_t length = db.errmsg.size();
    char *copy = static_cast<char *>(std::malloc(length + 1U));
    if (copy == nullptr) {
        return MYLITE_NOMEM;
    }

    std::memcpy(copy, db.errmsg.c_str(), length + 1U);
    *errmsg = copy;
    return db.errcode;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void set_ok(mylite_db &db) {
    db.errcode = MYLITE_OK;
    db.extended_errcode = MYLITE_OK;
    db.mariadb_errno = 0;
    db.sqlstate = k_sqlstate_ok;
    db.errmsg = k_not_an_error;
}
#endif

void set_error(mylite_db &db, int code, const char *message) {
    db.errcode = code;
    db.extended_errcode = code;
    db.mariadb_errno = 0;
    db.sqlstate = k_sqlstate_general;
    db.errmsg = message;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &db) {
    db.errcode = MYLITE_ERROR;
    db.extended_errcode = MYLITE_ERROR;
    db.mariadb_errno = mysql_errno(&db.mysql);
    db.sqlstate = mysql_sqlstate(&db.mysql);
    db.errmsg = mysql_error(&db.mysql);
}

void set_mariadb_statement_error(mylite_stmt &stmt) {
    mylite_db &db = *stmt.db;
    db.errcode = MYLITE_ERROR;
    db.extended_errcode = MYLITE_ERROR;
    db.mariadb_errno = mysql_stmt_errno(stmt.stmt);
    db.sqlstate = mysql_stmt_sqlstate(stmt.stmt);
    db.errmsg = mysql_stmt_error(stmt.stmt);
}

int parse_warning_level(const char *level) {
    if (level != nullptr && std::strcmp(level, "Note") == 0) {
        return MYLITE_WARNING_NOTE;
    }
    if (level != nullptr && std::strcmp(level, "Error") == 0) {
        return MYLITE_WARNING_ERROR;
    }
    return MYLITE_WARNING_WARNING;
}
#endif

const char *safe_c_str(const std::string &value) {
    return value.empty() ? "" : value.c_str();
}

bool has_config_field(const mylite_open_config *config, std::size_t field_end) {
    return config != nullptr && config->size >= field_end;
}

} // namespace

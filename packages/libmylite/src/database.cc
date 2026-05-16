#include <mylite/mylite.h>
#include <mylite/storage.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if MYLITE_WITH_MARIADB_EMBEDDED
#  include <mysql.h>
#endif

#ifndef MYLITE_MARIADB_MESSAGES_DIR
#  define MYLITE_MARIADB_MESSAGES_DIR ""
#endif

#ifndef MYLITE_MARIADB_CHARSETS_DIR
#  define MYLITE_MARIADB_CHARSETS_DIR ""
#endif

#ifndef MYLITE_MARIADB_HAS_PERFSCHEMA
#  define MYLITE_MARIADB_HAS_PERFSCHEMA 1
#endif

namespace {

constexpr unsigned k_known_open_flags = MYLITE_OPEN_READONLY | MYLITE_OPEN_READWRITE |
                                        MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE |
                                        MYLITE_OPEN_URI;
constexpr const char *k_sqlstate_ok = "00000";
constexpr const char *k_sqlstate_general = "HY000";
constexpr const char *k_not_an_error = "not an error";
constexpr const char *k_bad_db_handle = "bad database handle";
#if MYLITE_WITH_MARIADB_EMBEDDED
constexpr unsigned long k_initial_column_buffer_size = 256;
#endif

enum class BoundValueKind : unsigned char {
    Null,
    Int64,
    UInt64,
    Double,
    Text,
    Blob,
};

bool is_custom_destructor(mylite_destructor destructor);

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct BoundValue {
    BoundValue() = default;
    BoundValue(const BoundValue &) = delete;
    BoundValue &operator=(const BoundValue &) = delete;

    BoundValue(BoundValue &&other) noexcept {
        move_from(other);
    }

    BoundValue &operator=(BoundValue &&other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }

    ~BoundValue() {
        release();
    }

    void reset_to_null() {
        release();
        kind = BoundValueKind::Null;
        int64_value = 0;
        uint64_value = 0;
        double_value = 0.0;
        borrowed_data = nullptr;
        owned_data.clear();
        length = 0;
        mysql_length = 0;
        mysql_is_null = true;
    }

    void release() {
        if (is_custom_destructor(destructor) && destructor_arg != nullptr) {
            destructor(destructor_arg);
        }
        destructor = MYLITE_STATIC;
        destructor_arg = nullptr;
    }

    void move_from(BoundValue &other) noexcept {
        kind = other.kind;
        int64_value = other.int64_value;
        uint64_value = other.uint64_value;
        double_value = other.double_value;
        borrowed_data = other.borrowed_data;
        owned_data = std::move(other.owned_data);
        length = other.length;
        mysql_length = other.mysql_length;
        mysql_is_null = other.mysql_is_null;
        destructor = other.destructor;
        destructor_arg = other.destructor_arg;

        other.kind = BoundValueKind::Null;
        other.borrowed_data = nullptr;
        other.length = 0;
        other.mysql_length = 0;
        other.mysql_is_null = true;
        other.destructor = MYLITE_STATIC;
        other.destructor_arg = nullptr;
    }

    BoundValueKind kind = BoundValueKind::Null;
    long long int64_value = 0;
    unsigned long long uint64_value = 0;
    double double_value = 0.0;
    const void *borrowed_data = nullptr;
    std::vector<unsigned char> owned_data;
    std::size_t length = 0;
    unsigned long mysql_length = 0;
#if MYLITE_WITH_MARIADB_EMBEDDED
    my_bool mysql_is_null = true;
#else
    bool mysql_is_null = true;
#endif
    mylite_destructor destructor = MYLITE_STATIC;
    void *destructor_arg = nullptr;
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

#if MYLITE_WITH_MARIADB_EMBEDDED
struct ColumnInfo {
    std::string name;
    std::string database_name;
    std::string table_name;
    std::string origin_table_name;
    std::string origin_name;
    enum enum_field_types type = MYSQL_TYPE_NULL;
    unsigned int flags = 0;
    unsigned int charset = 0;
    unsigned int decimals = 0;
    unsigned long length = 0;
    unsigned long max_length = 0;
};

struct ColumnValue {
    mylite_value_type type = MYLITE_TYPE_NULL;
    long long int64_value = 0;
    unsigned long long uint64_value = 0;
    double double_value = 0.0;
    std::vector<unsigned char> bytes;
    unsigned long mysql_length = 0;
    unsigned long buffer_length = 0;
    my_bool mysql_is_null = true;
    my_bool mysql_error = 0;
    bool bytes_complete = false;
};

struct SchemaDefinition {
    std::string name;
    std::string default_character_set_name;
    std::string default_collation_name;
    std::string comment;
};
#endif

struct StoredWarning {
    mylite_warning_level level = MYLITE_WARNING_WARNING;
    unsigned code = 0;
    std::string message;
};

struct RuntimeState {
    std::mutex mutex;
    unsigned ref_count = 0;
    std::filesystem::path directory;
    std::string filename;
    std::vector<std::string> arguments;
    std::vector<char *> argv;
};

RuntimeState g_runtime;

enum class TransactionControlKind : unsigned char {
    None,
    Begin,
    BeginReadOnly,
    BeginReadWrite,
    Commit,
    CommitNoChain,
    CommitAndChain,
    Rollback,
    RollbackNoChain,
    RollbackAndChain,
    Savepoint,
    RollbackToSavepoint,
    ReleaseSavepoint,
    SetAutocommitOff,
    SetAutocommitOn,
    SetCompletionTypeNoChain,
    SetCompletionTypeChain,
    SetNextTransactionReadOnly,
    SetNextTransactionReadWrite,
    SetNextTransactionIsolation,
    SetSessionTransactionReadOnly,
    SetSessionTransactionReadWrite,
    SetSessionTransactionIsolation,
    Unsupported,
};

#if MYLITE_WITH_MARIADB_EMBEDDED && MYLITE_MARIADB_HAS_MYLITE_SE
struct DirectSavepoint {
    std::string name;
    mylite_storage_statement *statement = nullptr;
    bool active = true;
};
#endif

} // namespace

struct mylite_db {
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL mysql = {};
#endif
#if MYLITE_WITH_MARIADB_EMBEDDED && MYLITE_MARIADB_HAS_MYLITE_SE
    mylite_storage_statement *transaction_statement = nullptr;
    std::vector<DirectSavepoint> savepoints;
#endif
    std::string filename;
    unsigned busy_timeout_ms = 0;
    int errcode = MYLITE_OK;
    int extended_errcode = MYLITE_OK;
    unsigned mariadb_errno = 0;
    std::string sqlstate = k_sqlstate_ok;
    std::string errmsg = k_not_an_error;
    unsigned warning_count = 0;
    std::vector<StoredWarning> warnings;
    long long changes = 0;
    unsigned long long last_insert_id = 0;
    unsigned active_statements = 0;
    bool connected = false;
    bool transaction_active = false;
    bool transaction_read_only = false;
    bool transaction_read_only_default = false;
    bool transaction_read_only_next = false;
    bool transaction_read_only_next_set = false;
    bool autocommit_disabled = false;
    bool completion_type_chain = false;
};

struct mylite_stmt {
    mylite_db *database = nullptr;
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL_STMT *statement = nullptr;
    std::vector<ColumnInfo> columns;
    std::vector<MYSQL_BIND> parameter_binds;
    std::vector<MYSQL_BIND> result_binds;
    std::vector<ColumnValue> values;
#endif
    std::vector<BoundValue> parameters;
    bool executed = false;
    bool done = false;
    bool has_current_row = false;
    TransactionControlKind prepared_transaction_control = TransactionControlKind::None;
    std::string prepared_transaction_control_name;
    bool sync_schema_catalog_after_execute = false;
    bool writes_storage = false;
    bool uses_storage_outer_checkpoint = false;
    bool uses_statement_checkpoint = false;
    bool uses_transaction_statement_checkpoint = false;
};

namespace {

#if MYLITE_WITH_MARIADB_EMBEDDED && MYLITE_MARIADB_HAS_MYLITE_SE
class StorageStatementCheckpoint {
  public:
    StorageStatementCheckpoint() = default;
    StorageStatementCheckpoint(const StorageStatementCheckpoint &) = delete;
    StorageStatementCheckpoint &operator=(const StorageStatementCheckpoint &) = delete;
    ~StorageStatementCheckpoint();

    int begin(mylite_db &database, bool enabled);
    int commit(mylite_db &database);
    int rollback(mylite_db &database);

  private:
    mylite_storage_statement *statement_ = nullptr;
};
#endif

class StorageBusyTimeoutScope {
  public:
    explicit StorageBusyTimeoutScope(unsigned milliseconds)
        : previous_(mylite_storage_busy_timeout()) {
        mylite_storage_set_busy_timeout(milliseconds);
    }

    StorageBusyTimeoutScope(const StorageBusyTimeoutScope &) = delete;
    StorageBusyTimeoutScope &operator=(const StorageBusyTimeoutScope &) = delete;

    ~StorageBusyTimeoutScope() {
        mylite_storage_set_busy_timeout(previous_);
    }

  private:
    unsigned previous_ = 0;
};

int open_v2_impl(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
int validate_open_args(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
#if MYLITE_WITH_MARIADB_EMBEDDED
int prepare_primary_file(const std::filesystem::path &filename, unsigned flags);
int map_storage_result(mylite_storage_result result);
int start_runtime(mylite_db &database, const mylite_open_config *config);
int connect_runtime(mylite_db &database);
int configure_connection(mylite_db &database);
#endif
void close_connection(mylite_db &database);
void release_runtime(void);
int exec_impl(
    mylite_db *database,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
);
int prepare_impl(
    mylite_db *database,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
);
#if MYLITE_WITH_MARIADB_EMBEDDED
int initialize_statement_metadata(mylite_stmt &statement);
int bind_statement_parameters(mylite_stmt &statement);
int execute_statement(mylite_stmt &statement);
int fetch_statement_row(mylite_stmt &statement);
int bind_statement_results(mylite_stmt &statement);
int fetch_truncated_column(mylite_stmt &statement, unsigned column);
int materialize_column_value(mylite_stmt &statement, unsigned column);
int capture_warnings(mylite_db &database, unsigned warning_count, bool force_query = false);
#  if MYLITE_MARIADB_HAS_MYLITE_SE
int execute_prepared_savepoint_control(mylite_stmt &statement);
int begin_direct_transaction(mylite_db &database);
int finish_direct_transaction(mylite_db &database, bool commit);
int execute_direct_savepoint_control(
    mylite_db &database,
    TransactionControlKind kind,
    std::string_view name
);
int set_direct_savepoint(mylite_db &database, std::string_view name);
int rollback_to_direct_savepoint(mylite_db &database, std::string_view name);
int release_direct_savepoint(mylite_db &database, std::string_view name);
int finish_direct_savepoints(mylite_db &database, bool commit);
std::size_t find_direct_savepoint(const mylite_db &database, std::string_view name);
int finish_direct_savepoint_frame(mylite_db &database, std::size_t index, bool commit);
int begin_direct_savepoint_frame(mylite_db &database, std::string_view name);
#  endif
void clear_current_row(mylite_stmt &statement);
bool is_variable_column_type(mylite_value_type type);
const void *bound_value_data(const BoundValue &value);
enum enum_field_types bound_value_type(const BoundValue &value);
mylite_value_type map_column_type(const ColumnInfo &column);
mylite_value_type map_mariadb_type(enum enum_field_types type, unsigned int flags);
bool is_unsigned_column(unsigned int flags);
mylite_warning_level map_warning_level(const char *level);
std::string field_string(const char *value, unsigned int length);
const char *unsupported_sql_surface_message(std::string_view sql);
TransactionControlKind direct_transaction_control_kind(std::string_view sql);
TransactionControlKind direct_start_transaction_control_kind(std::string_view sql);
TransactionControlKind direct_completion_transaction_control_kind(
    std::string_view sql,
    TransactionControlKind plain_kind,
    TransactionControlKind no_chain_kind,
    TransactionControlKind chained_kind
);
TransactionControlKind direct_set_transaction_control_kind(std::string_view sql);
TransactionControlKind direct_set_transaction_characteristics_control_kind(
    std::string_view sql,
    bool has_statement_tail
);
#  if MYLITE_MARIADB_HAS_MYLITE_SE
bool direct_savepoint_control_name(
    std::string_view sql,
    TransactionControlKind kind,
    std::string &out_name
);
#  endif
bool pop_sql_savepoint_name(std::string_view &sql, std::string *out_name);
bool is_server_surface_sql(std::string_view sql);
bool is_backup_sql(std::string_view sql);
bool is_query_cache_sql(std::string_view sql);
bool is_statement_profiling_sql(std::string_view sql);
bool is_optimizer_trace_sql(std::string_view sql);
bool is_table_maintenance_sql(std::string_view sql);
bool is_sql_handler_command_sql(std::string_view sql);
bool is_help_command_sql(std::string_view sql);
bool is_static_show_info_sql(std::string_view sql);
bool is_processlist_metadata_sql(std::string_view sql);
bool is_file_import_sql(std::string_view sql);
bool is_file_export_sql(std::string_view sql);
bool is_server_utility_function_sql(std::string_view sql);
bool is_gis_sql_function_sql(std::string_view sql);
bool is_sformat_sql_function_sql(std::string_view sql);
bool is_json_schema_valid_sql(std::string_view sql);
bool is_json_table_sql(std::string_view sql);
bool is_dynamic_column_sql(std::string_view sql);
bool is_sequence_sql(std::string_view sql);
bool is_user_statistics_sql(std::string_view sql);
bool is_xml_sql_function_sql(std::string_view sql);
bool is_oracle_sql_mode_sql(std::string_view sql);
bool sql_set_assignment_has_oracle_sql_mode(std::string_view assignment);
std::string_view sql_set_assignment_policy_span(std::string_view sql);
std::string_view sql_set_statement_assignment_span(std::string_view sql);
bool sql_set_assignment_targets_query_cache_variable(std::string_view assignment);
bool sql_set_assignment_targets_statement_profiling_variable(std::string_view assignment);
bool sql_set_assignment_targets_optimizer_trace_variable(std::string_view assignment);
std::string_view pop_sql_set_assignment(std::string_view &sql);
bool sql_set_assignment_targets_variable(std::string_view &assignment, const char *keyword);
bool is_non_table_object_sql(std::string_view sql);
bool is_procedure_analyse_sql(std::string_view sql);
bool is_select_procedure_sql(std::string_view sql);
bool is_locking_sql(std::string_view sql);
bool is_online_alter_sql(std::string_view sql);
bool is_partition_sql(std::string_view sql);
bool is_foreign_key_sql(std::string_view sql);
bool is_non_table_object_keyword(std::string_view token);
TransactionControlKind direct_set_assignment_transaction_control_kind(std::string_view sql);
TransactionControlKind autocommit_assignment_control_kind(std::string_view assignment);
TransactionControlKind completion_type_assignment_control_kind(std::string_view assignment);
bool direct_set_completion_type_chain_default(std::string_view sql, bool *out_chain);
bool sql_token_is_completion_type_no_chain(std::string_view token);
bool sql_token_is_completion_type_chain(std::string_view token);
bool sql_set_assignment_targets_transaction_variable(std::string_view assignment);
bool pop_transaction_access_mode(std::string_view &sql, bool *out_read_only);
bool pop_transaction_isolation_level(std::string_view &sql);
TransactionControlKind transaction_access_mode_control_kind(bool session_scope, bool read_only);
TransactionControlKind transaction_isolation_control_kind(bool session_scope);
bool is_begin_transaction_control(TransactionControlKind kind);
bool is_completion_transaction_control(TransactionControlKind kind);
bool is_savepoint_transaction_control(TransactionControlKind kind);
bool is_chained_completion_transaction_control(TransactionControlKind kind);
bool transaction_start_read_only(const mylite_db &database, TransactionControlKind kind);
void mark_transaction_started(mylite_db &database, bool read_only);
void mark_transaction_completed(mylite_db &database);
void apply_transaction_characteristics_control(mylite_db &database, TransactionControlKind kind);
bool sql_token_is_autocommit_off(std::string_view token);
bool sql_token_is_autocommit_on(std::string_view token);
bool sql_has_statement_tail_after_semicolon(std::string_view sql);
bool sql_rest_is_statement_end(std::string_view sql);
bool sql_tokens_contain_file_import_function(std::string_view sql);
bool sql_tokens_contain_file_export_marker(std::string_view sql);
bool sql_tokens_contain_server_utility_function(std::string_view sql);
bool sql_tokens_contain_gis_sql_function(std::string_view sql);
bool sql_tokens_contain_sformat_sql_function(std::string_view sql);
bool sql_tokens_contain_json_schema_valid_function(std::string_view sql);
bool sql_tokens_contain_json_table_function(std::string_view sql);
bool sql_tokens_contain_dynamic_column_function(std::string_view sql);
bool sql_token_is_dynamic_column_function(std::string_view token);
bool sql_tokens_contain_sequence_value_surface(std::string_view sql);
bool sql_tokens_contain_user_statistics_information_schema_table(std::string_view sql);
bool sql_tokens_contain_statement_profiling_information_schema_table(std::string_view sql);
bool sql_tokens_contain_optimizer_trace_information_schema_table(std::string_view sql);
bool sql_token_is_user_statistics_table(std::string_view token);
bool pop_sql_identifier_token(std::string_view &sql, std::string_view &out_token);
bool consume_sql_reference_dot(std::string_view &sql);
bool sql_tokens_contain_xml_sql_function(std::string_view sql);
bool sql_tokens_contain_locking_marker(std::string_view sql);
bool sql_tokens_contain_named_lock_function(std::string_view sql);
bool sql_token_is_gis_sql_function(std::string_view token);
bool sql_span_contains_token(std::string_view sql, const char *keyword);
bool sql_quoted_span_contains_token(std::string_view &sql, char quote, const char *keyword);
bool sql_tokens_contain_online_alter_marker(std::string_view sql);
bool sql_tokens_contain_partition_marker(std::string_view sql);
bool sql_tokens_contain_foreign_key_marker(std::string_view sql);
std::string_view skip_sql_leading_noise(std::string_view sql);
std::string_view pop_sql_token(std::string_view &sql);
std::string_view pop_sql_token_after_separators(std::string_view &sql);
bool pop_sql_scanned_token(std::string_view &sql, std::string_view &out_token);
void skip_sql_quoted_span(std::string_view &sql, char quote);
bool sql_next_non_noise_is(std::string_view sql, char expected);
bool sql_starts_with_token(std::string_view sql, const char *keyword);
std::size_t executable_sql_comment_prefix_size(std::string_view sql);
void skip_executable_sql_comment_prefix(std::string_view &sql);
bool sql_token_equals(std::string_view token, const char *keyword);
#  if MYLITE_MARIADB_HAS_MYLITE_SE
int sync_schema_catalog(mylite_db &database);
bool is_schema_catalog_sql(std::string_view sql);
bool is_storage_outer_checkpoint_sql(std::string_view sql);
bool is_row_dml_checkpoint_sql(std::string_view sql);
int collect_storage_schema(void *ctx, const char *schema_name);
int load_runtime_schema_definitions(
    mylite_db &database,
    std::vector<SchemaDefinition> &out_definitions
);
bool is_system_schema(std::string_view schema_name);
bool has_schema_name(const std::vector<SchemaDefinition> &schemas, const std::string &schema_name);
#  endif
int store_and_emit_result(
    mylite_db &database,
    mylite_exec_callback callback,
    void *ctx,
    bool *has_result
);
std::filesystem::path normalize_filename(const char *filename);
unsigned busy_timeout_from_config(const mylite_open_config *config);
std::filesystem::path create_runtime_directory(const mylite_open_config *config);
std::filesystem::path runtime_root(const mylite_open_config *config);
std::string unique_runtime_name(void);
std::vector<std::string> runtime_arguments(
    const std::filesystem::path &runtime_dir,
    const std::string &primary_filename
);
std::vector<char *> mutable_arguments(std::vector<std::string> &arguments);
#endif
void remove_directory_if_present(const std::filesystem::path &directory);
int copy_error_message(mylite_db &database, char **errmsg);
#if MYLITE_WITH_MARIADB_EMBEDDED
void clear_warnings(mylite_db &database);
#endif
void set_ok(mylite_db &database);
void set_error(mylite_db &database, int code, const char *message);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &database);
void set_mariadb_statement_error(mylite_stmt &statement);
#endif
const char *safe_c_str(const std::string &value);
bool has_config_field(const mylite_open_config *config, std::size_t field_end);
int bind_parameter_index(mylite_stmt *statement, unsigned index);
int bind_bytes(
    mylite_stmt *statement,
    unsigned index,
    BoundValueKind kind,
    const void *value,
    std::size_t value_len,
    mylite_destructor destructor
);
bool is_transient_destructor(mylite_destructor destructor);
void reset_statement_bindings(mylite_stmt &statement);

} // namespace

int mylite_open(const char *filename, mylite_db **out_db) {
    return mylite_open_v2(filename, out_db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, nullptr);
}

int mylite_open_v2(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    return open_v2_impl(filename, out_db, flags, config);
}

int mylite_close(mylite_db *database) {
    if (database == nullptr) {
        return MYLITE_OK;
    }

    if (database->active_statements > 0U) {
        set_error(*database, MYLITE_BUSY, "database has active statements");
        return MYLITE_BUSY;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    if (database->transaction_active && mysql_query(&database->mysql, "ROLLBACK") != 0) {
        set_mariadb_error(*database);
        return MYLITE_ERROR;
    }
#  if MYLITE_MARIADB_HAS_MYLITE_SE
    if (database->transaction_active) {
        const int transaction_result = finish_direct_transaction(*database, false);
        if (transaction_result != MYLITE_OK) {
            return transaction_result;
        }
    }
#  endif
    database->transaction_active = false;
    database->autocommit_disabled = false;
#endif

    close_connection(*database);
    release_runtime();
    delete database;
    return MYLITE_OK;
}

int mylite_busy_timeout(mylite_db *database, unsigned milliseconds) {
    if (database == nullptr) {
        return MYLITE_MISUSE;
    }

    database->busy_timeout_ms = milliseconds;
    set_ok(*database);
    return MYLITE_OK;
}

int mylite_exec(
    mylite_db *database,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
) {
    return exec_impl(database, sql, callback, ctx, errmsg);
}

int mylite_prepare(
    mylite_db *database,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
) {
    return prepare_impl(database, sql, sql_len, out_stmt, tail);
}

int mylite_step(mylite_stmt *statement) {
    if (statement == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*statement->database, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    StorageBusyTimeoutScope busy_timeout(
        statement->database != nullptr ? statement->database->busy_timeout_ms : 0U
    );
    try {
        if (statement->done) {
            return MYLITE_DONE;
        }

        if (!statement->executed) {
            return execute_statement(*statement);
        }

        return fetch_statement_row(*statement);
    } catch (const std::bad_alloc &) {
        set_error(*statement->database, MYLITE_NOMEM, "statement allocation failed");
        return MYLITE_NOMEM;
    }
#endif
}

int mylite_reset(mylite_stmt *statement) {
    if (statement == nullptr) {
        return MYLITE_MISUSE;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement->statement != nullptr && statement->executed) {
        mysql_stmt_free_result(statement->statement);
        if (mysql_stmt_reset(statement->statement) != 0) {
            set_mariadb_statement_error(*statement);
            return MYLITE_ERROR;
        }
    }
#endif

    statement->executed = false;
    statement->done = false;
    statement->has_current_row = false;
    reset_statement_bindings(*statement);
    if (statement->database != nullptr) {
        set_ok(*statement->database);
    }
    return MYLITE_OK;
}

int mylite_finalize(mylite_stmt *statement) {
    if (statement == nullptr) {
        return MYLITE_OK;
    }

    mylite_db *database = statement->database;
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement->statement != nullptr) {
        if (statement->executed && !statement->columns.empty()) {
            mysql_stmt_free_result(statement->statement);
        }
        mysql_stmt_close(statement->statement);
        statement->statement = nullptr;
    }
#endif
    delete statement;

    if (database != nullptr && database->active_statements > 0U) {
        --database->active_statements;
    }
    if (database != nullptr) {
        set_ok(*database);
    }
    return MYLITE_OK;
}

unsigned mylite_bind_parameter_count(mylite_stmt *statement) {
    return statement != nullptr ? static_cast<unsigned>(statement->parameters.size()) : 0U;
}

int mylite_clear_bindings(mylite_stmt *statement) {
    if (statement == nullptr) {
        return MYLITE_MISUSE;
    }
    reset_statement_bindings(*statement);
    if (statement->database != nullptr) {
        set_ok(*statement->database);
    }
    return MYLITE_OK;
}

int mylite_bind_null(mylite_stmt *statement, unsigned index) {
    const int parameter = bind_parameter_index(statement, index);
    if (parameter < 0) {
        return MYLITE_MISUSE;
    }
    statement->parameters[static_cast<std::size_t>(parameter)].reset_to_null();
    set_ok(*statement->database);
    return MYLITE_OK;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
int mylite_bind_int64(mylite_stmt *statement, unsigned index, long long value) {
    const int parameter = bind_parameter_index(statement, index);
    if (parameter < 0) {
        return MYLITE_MISUSE;
    }

    BoundValue &bound = statement->parameters[static_cast<std::size_t>(parameter)];
    bound.reset_to_null();
    bound.kind = BoundValueKind::Int64;
    bound.int64_value = value;
    bound.mysql_is_null = false;
    set_ok(*statement->database);
    return MYLITE_OK;
}

int mylite_bind_uint64(mylite_stmt *statement, unsigned index, unsigned long long value) {
    const int parameter = bind_parameter_index(statement, index);
    if (parameter < 0) {
        return MYLITE_MISUSE;
    }

    BoundValue &bound = statement->parameters[static_cast<std::size_t>(parameter)];
    bound.reset_to_null();
    bound.kind = BoundValueKind::UInt64;
    bound.uint64_value = value;
    bound.mysql_is_null = false;
    set_ok(*statement->database);
    return MYLITE_OK;
}

int mylite_bind_double(mylite_stmt *statement, unsigned index, double value) {
    const int parameter = bind_parameter_index(statement, index);
    if (parameter < 0) {
        return MYLITE_MISUSE;
    }

    BoundValue &bound = statement->parameters[static_cast<std::size_t>(parameter)];
    bound.reset_to_null();
    bound.kind = BoundValueKind::Double;
    bound.double_value = value;
    bound.mysql_is_null = false;
    set_ok(*statement->database);
    return MYLITE_OK;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

int mylite_bind_text(
    mylite_stmt *statement,
    unsigned index,
    const char *value,
    std::size_t value_len,
    mylite_destructor destructor
) {
    if (value_len == MYLITE_NUL_TERMINATED) {
        if (value == nullptr) {
            if (statement != nullptr && statement->database != nullptr) {
                set_error(*statement->database, MYLITE_MISUSE, "NUL-terminated text is NULL");
            }
            return MYLITE_MISUSE;
        }
        value_len = std::strlen(value);
    }
    return bind_bytes(statement, index, BoundValueKind::Text, value, value_len, destructor);
}

int mylite_bind_blob(
    mylite_stmt *statement,
    unsigned index,
    const void *value,
    std::size_t value_len,
    mylite_destructor destructor
) {
    if (value_len == MYLITE_NUL_TERMINATED) {
        if (statement != nullptr && statement->database != nullptr) {
            set_error(*statement->database, MYLITE_MISUSE, "BLOB length must be explicit");
        }
        return MYLITE_MISUSE;
    }
    return bind_bytes(statement, index, BoundValueKind::Blob, value, value_len, destructor);
}

unsigned mylite_column_count(mylite_stmt *statement) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    return statement != nullptr ? static_cast<unsigned>(statement->columns.size()) : 0U;
#else
    (void)statement;
    return 0U;
#endif
}

const char *mylite_column_name(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return nullptr;
    }
    return statement->columns[column].name.c_str();
#else
    (void)statement;
    (void)column;
    return nullptr;
#endif
}

const char *mylite_column_database_name(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return nullptr;
    }
    return statement->columns[column].database_name.c_str();
#else
    (void)statement;
    (void)column;
    return nullptr;
#endif
}

const char *mylite_column_table_name(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return nullptr;
    }
    return statement->columns[column].table_name.c_str();
#else
    (void)statement;
    (void)column;
    return nullptr;
#endif
}

const char *mylite_column_origin_table_name(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return nullptr;
    }
    return statement->columns[column].origin_table_name.c_str();
#else
    (void)statement;
    (void)column;
    return nullptr;
#endif
}

const char *mylite_column_origin_name(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return nullptr;
    }
    return statement->columns[column].origin_name.c_str();
#else
    (void)statement;
    (void)column;
    return nullptr;
#endif
}

unsigned mylite_column_mariadb_type(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return 0U;
    }
    return static_cast<unsigned>(statement->columns[column].type);
#else
    (void)statement;
    (void)column;
    return 0U;
#endif
}

unsigned mylite_column_flags(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return 0U;
    }
    return statement->columns[column].flags;
#else
    (void)statement;
    (void)column;
    return 0U;
#endif
}

unsigned mylite_column_charset(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return 0U;
    }
    return statement->columns[column].charset;
#else
    (void)statement;
    (void)column;
    return 0U;
#endif
}

unsigned mylite_column_decimals(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return 0U;
    }
    return statement->columns[column].decimals;
#else
    (void)statement;
    (void)column;
    return 0U;
#endif
}

unsigned long mylite_column_length(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return 0UL;
    }
    return statement->columns[column].length;
#else
    (void)statement;
    (void)column;
    return 0UL;
#endif
}

unsigned long mylite_column_max_length(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || column >= statement->columns.size()) {
        return 0UL;
    }
    return statement->columns[column].max_length;
#else
    (void)statement;
    (void)column;
    return 0UL;
#endif
}

mylite_value_type mylite_column_type(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || !statement->has_current_row || column >= statement->values.size()) {
        return MYLITE_TYPE_NULL;
    }
    return statement->values[column].type;
#else
    (void)statement;
    (void)column;
    return MYLITE_TYPE_NULL;
#endif
}

long long mylite_column_int64(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || !statement->has_current_row || column >= statement->values.size()) {
        return 0;
    }
    return statement->values[column].int64_value;
#else
    (void)statement;
    (void)column;
    return 0;
#endif
}

unsigned long long mylite_column_uint64(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || !statement->has_current_row || column >= statement->values.size()) {
        return 0U;
    }
    return statement->values[column].uint64_value;
#else
    (void)statement;
    (void)column;
    return 0U;
#endif
}

double mylite_column_double(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || !statement->has_current_row || column >= statement->values.size()) {
        return 0.0;
    }
    return statement->values[column].double_value;
#else
    (void)statement;
    (void)column;
    return 0.0;
#endif
}

const char *mylite_column_text(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || !statement->has_current_row || column >= statement->values.size()) {
        return nullptr;
    }
    ColumnValue &value = statement->values[column];
    if (value.type != MYLITE_TYPE_TEXT || value.mysql_is_null != 0) {
        return nullptr;
    }
    if (!value.bytes_complete && materialize_column_value(*statement, column) != MYLITE_OK) {
        return nullptr;
    }
    return reinterpret_cast<const char *>(value.bytes.data());
#else
    (void)statement;
    (void)column;
    return nullptr;
#endif
}

const void *mylite_column_blob(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || !statement->has_current_row || column >= statement->values.size()) {
        return nullptr;
    }
    ColumnValue &value = statement->values[column];
    if (!is_variable_column_type(value.type) || value.mysql_is_null != 0) {
        return nullptr;
    }
    if (!value.bytes_complete && materialize_column_value(*statement, column) != MYLITE_OK) {
        return nullptr;
    }
    return value.bytes.data();
#else
    (void)statement;
    (void)column;
    return nullptr;
#endif
}

std::size_t mylite_column_bytes(mylite_stmt *statement, unsigned column) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (statement == nullptr || !statement->has_current_row || column >= statement->values.size()) {
        return 0U;
    }
    const ColumnValue &value = statement->values[column];
    return value.mysql_is_null != 0 ? 0U : static_cast<std::size_t>(value.mysql_length);
#else
    (void)statement;
    (void)column;
    return 0U;
#endif
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
int mylite_column_read(
    mylite_stmt *statement,
    unsigned column,
    std::size_t offset,
    void *buffer,
    std::size_t buffer_len,
    std::size_t *out_read
) {
    if (out_read == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_read = 0U;
    if (statement == nullptr || (buffer == nullptr && buffer_len != 0U)) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)column;
    (void)offset;
    (void)buffer;
    (void)buffer_len;
    set_error(*statement->database, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    if (!statement->has_current_row || column >= statement->values.size()) {
        set_error(*statement->database, MYLITE_MISUSE, "invalid column read");
        return MYLITE_MISUSE;
    }

    ColumnValue &value = statement->values[column];
    if (value.mysql_is_null != 0) {
        set_ok(*statement->database);
        return MYLITE_OK;
    }
    if (!is_variable_column_type(value.type)) {
        set_error(*statement->database, MYLITE_MISUSE, "column is not TEXT or BLOB");
        return MYLITE_MISUSE;
    }

    const auto total_length = static_cast<std::size_t>(value.mysql_length);
    if (offset >= total_length || buffer_len == 0U) {
        set_ok(*statement->database);
        return MYLITE_OK;
    }

    const std::size_t requested = std::min(buffer_len, total_length - offset);
    if (value.bytes_complete) {
        std::memcpy(buffer, value.bytes.data() + offset, requested);
        *out_read = requested;
        set_ok(*statement->database);
        return MYLITE_OK;
    }

    if (offset > static_cast<std::size_t>(ULONG_MAX) ||
        requested > static_cast<std::size_t>(ULONG_MAX)) {
        set_error(*statement->database, MYLITE_MISUSE, "column read range is too large");
        return MYLITE_MISUSE;
    }

    unsigned long mysql_length = 0;
    my_bool mysql_is_null = false;
    my_bool mysql_error = 0;
    MYSQL_BIND bind = {};
    bind.buffer_type = value.type == MYLITE_TYPE_BLOB ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
    bind.buffer = buffer;
    bind.buffer_length = static_cast<unsigned long>(requested);
    bind.length = &mysql_length;
    bind.is_null = &mysql_is_null;
    bind.error = &mysql_error;

    const int fetch_result = mysql_stmt_fetch_column(
        statement->statement,
        &bind,
        column,
        static_cast<unsigned long>(offset)
    );
    if (fetch_result != 0 && fetch_result != MYSQL_DATA_TRUNCATED) {
        set_mariadb_statement_error(*statement);
        return MYLITE_ERROR;
    }

    *out_read = requested;
    set_ok(*statement->database);
    return MYLITE_OK;
#endif
}

// NOLINTEND(bugprone-easily-swappable-parameters)

int mylite_errcode(mylite_db *database) {
    return database != nullptr ? database->errcode : MYLITE_MISUSE;
}

int mylite_extended_errcode(mylite_db *database) {
    return database != nullptr ? database->extended_errcode : MYLITE_MISUSE;
}

unsigned mylite_mariadb_errno(mylite_db *database) {
    return database != nullptr ? database->mariadb_errno : 0;
}

const char *mylite_sqlstate(mylite_db *database) {
    return database != nullptr ? safe_c_str(database->sqlstate) : k_sqlstate_general;
}

const char *mylite_errmsg(mylite_db *database) {
    return database != nullptr ? safe_c_str(database->errmsg) : k_bad_db_handle;
}

unsigned mylite_warning_count(mylite_db *database) {
    return database != nullptr ? database->warning_count : 0U;
}

int mylite_warning(
    mylite_db *database,
    unsigned index,
    mylite_warning_level *level,
    unsigned *code,
    const char **message
) {
    if (database == nullptr) {
        return MYLITE_MISUSE;
    }
    if (level == nullptr || code == nullptr || message == nullptr) {
        set_error(*database, MYLITE_MISUSE, "bad warning output argument");
        return MYLITE_MISUSE;
    }
    *level = MYLITE_WARNING_NOTE;
    *code = 0;
    *message = nullptr;
    if (index >= database->warnings.size()) {
        set_error(*database, MYLITE_NOTFOUND, "warning is not stored");
        return MYLITE_NOTFOUND;
    }

    const StoredWarning &warning = database->warnings[index];
    *level = warning.level;
    *code = warning.code;
    *message = warning.message.c_str();
    set_ok(*database);
    return MYLITE_OK;
}

long long mylite_changes(mylite_db *database) {
    return database != nullptr ? database->changes : 0;
}

unsigned long long mylite_last_insert_id(mylite_db *database) {
    return database != nullptr ? database->last_insert_id : 0;
}

void mylite_free(void *ptr) {
    std::free(ptr);
}

namespace {

int open_v2_impl(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    const int validation_result = validate_open_args(filename, out_db, flags, config);
    if (validation_result != MYLITE_OK) {
        return validation_result;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)config;
    return MYLITE_ERROR;
#else
    try {
        std::unique_ptr<mylite_db> database(new mylite_db());
        database->filename = normalize_filename(filename).string();
        database->busy_timeout_ms = busy_timeout_from_config(config);

        StorageBusyTimeoutScope busy_timeout(database->busy_timeout_ms);
        const int file_result = prepare_primary_file(database->filename, flags);
        if (file_result != MYLITE_OK) {
            return file_result;
        }

        const int runtime_result = start_runtime(*database, config);
        if (runtime_result != MYLITE_OK) {
            return runtime_result;
        }

        const int connect_result = connect_runtime(*database);
        if (connect_result != MYLITE_OK) {
            close_connection(*database);
            release_runtime();
            return connect_result;
        }

        *out_db = database.release();
        return MYLITE_OK;
    } catch (const std::bad_alloc &) {
        return MYLITE_NOMEM;
    } catch (const std::filesystem::filesystem_error &) {
        return MYLITE_IOERR;
    }
#endif
}

int validate_open_args(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    if (out_db == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_db = nullptr;

    if (filename == nullptr || filename[0] == '\0') {
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

    if (readonly && (flags & (MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE)) != 0U) {
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
    mylite_db *database,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
) {
    if (errmsg != nullptr) {
        *errmsg = nullptr;
    }

    if (database == nullptr || sql == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)callback;
    (void)ctx;
    set_error(*database, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return copy_error_message(*database, errmsg);
#else
    StorageBusyTimeoutScope busy_timeout(database->busy_timeout_ms);
    set_ok(*database);
    clear_warnings(*database);
    const TransactionControlKind transaction_control =
        direct_transaction_control_kind(std::string_view(sql));
    if (transaction_control == TransactionControlKind::Unsupported) {
        set_error(*database, MYLITE_ERROR, "unsupported SQL transaction control");
        return copy_error_message(*database, errmsg);
    }
    if (is_savepoint_transaction_control(transaction_control)) {
        if (!database->transaction_active) {
            set_error(*database, MYLITE_ERROR, "unsupported savepoint transaction control");
            return copy_error_message(*database, errmsg);
        }
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        if (database->filename != ":memory:") {
            std::string savepoint_name;
            bool parsed_savepoint_name = false;
            try {
                parsed_savepoint_name = direct_savepoint_control_name(
                    std::string_view(sql),
                    transaction_control,
                    savepoint_name
                );
            } catch (const std::bad_alloc &) {
                set_error(*database, MYLITE_NOMEM, "savepoint allocation failed");
                return copy_error_message(*database, errmsg);
            }
            if (!parsed_savepoint_name) {
                set_error(*database, MYLITE_ERROR, "unsupported savepoint transaction control");
                return copy_error_message(*database, errmsg);
            }
            const int savepoint_result =
                execute_direct_savepoint_control(*database, transaction_control, savepoint_name);
            if (savepoint_result != MYLITE_OK) {
                return copy_error_message(*database, errmsg);
            }
            database->changes = 0;
            database->last_insert_id = 0;
            return MYLITE_OK;
        }
#  endif
    }
#  if MYLITE_MARIADB_HAS_MYLITE_SE
    if (database->transaction_active && database->transaction_read_only &&
        transaction_control == TransactionControlKind::None &&
        is_row_dml_checkpoint_sql(std::string_view(sql))) {
        set_error(*database, MYLITE_ERROR, "unsupported read-only transaction write SQL surface");
        return copy_error_message(*database, errmsg);
    }
    if (database->transaction_active && transaction_control == TransactionControlKind::None &&
        is_storage_outer_checkpoint_sql(std::string_view(sql))) {
        set_error(*database, MYLITE_ERROR, "unsupported transactional DDL SQL surface");
        return copy_error_message(*database, errmsg);
    }
#  endif
    if (const char *unsupported_message = unsupported_sql_surface_message(std::string_view(sql))) {
        set_error(*database, MYLITE_ERROR, unsupported_message);
        return copy_error_message(*database, errmsg);
    }

#  if MYLITE_MARIADB_HAS_MYLITE_SE
    StorageStatementCheckpoint checkpoint;
    const bool checkpoint_enabled =
        database->filename != ":memory:" &&
        (is_storage_outer_checkpoint_sql(std::string_view(sql)) ||
         (database->transaction_active && is_row_dml_checkpoint_sql(std::string_view(sql))));
    int checkpoint_result = checkpoint.begin(*database, checkpoint_enabled);
    if (checkpoint_result != MYLITE_OK) {
        return copy_error_message(*database, errmsg);
    }
#  endif

    if (mysql_query(&database->mysql, sql) != 0) {
        const unsigned warning_count = mysql_warning_count(&database->mysql);
        set_mariadb_error(*database);
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        checkpoint_result = checkpoint.rollback(*database);
        if (checkpoint_result != MYLITE_OK) {
            return copy_error_message(*database, errmsg);
        }
#  endif
        const int warning_result = capture_warnings(*database, warning_count, true);
        if (warning_result != MYLITE_OK) {
            return copy_error_message(*database, errmsg);
        }
        return copy_error_message(*database, errmsg);
    }

    bool has_result = false;
    const int result = store_and_emit_result(*database, callback, ctx, &has_result);
    if (result != MYLITE_OK) {
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        checkpoint_result = checkpoint.commit(*database);
        if (checkpoint_result != MYLITE_OK) {
            return copy_error_message(*database, errmsg);
        }
#  endif
        return copy_error_message(*database, errmsg);
    }

    const my_ulonglong affected_rows = mysql_affected_rows(&database->mysql);
    database->changes =
        has_result || affected_rows == static_cast<my_ulonglong>(-1)
            ? 0
            : static_cast<long long>(
                  std::min<my_ulonglong>(affected_rows, static_cast<my_ulonglong>(LLONG_MAX))
              );
    database->last_insert_id = static_cast<unsigned long long>(mysql_insert_id(&database->mysql));
    const unsigned warning_count = mysql_warning_count(&database->mysql);
    const int warning_result = capture_warnings(*database, warning_count);
    if (warning_result != MYLITE_OK) {
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        checkpoint_result = checkpoint.commit(*database);
        if (checkpoint_result != MYLITE_OK) {
            return copy_error_message(*database, errmsg);
        }
#  endif
        return copy_error_message(*database, errmsg);
    }
#  if MYLITE_MARIADB_HAS_MYLITE_SE
    if (database->filename != ":memory:" && is_schema_catalog_sql(std::string_view(sql))) {
        const int sync_result = sync_schema_catalog(*database);
        if (sync_result != MYLITE_OK) {
            checkpoint_result = checkpoint.rollback(*database);
            if (checkpoint_result != MYLITE_OK) {
                return copy_error_message(*database, errmsg);
            }
            return copy_error_message(*database, errmsg);
        }
    }
    checkpoint_result = checkpoint.commit(*database);
    if (checkpoint_result != MYLITE_OK) {
        return copy_error_message(*database, errmsg);
    }
#  endif
    bool completion_type_chain = false;
    if (direct_set_completion_type_chain_default(std::string_view(sql), &completion_type_chain)) {
        database->completion_type_chain = completion_type_chain;
    }
    apply_transaction_characteristics_control(*database, transaction_control);

    if (is_begin_transaction_control(transaction_control)) {
        const bool read_only = transaction_start_read_only(*database, transaction_control);
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        if (database->transaction_active) {
            const int finish_result = finish_direct_transaction(*database, true);
            mark_transaction_completed(*database);
            if (finish_result != MYLITE_OK) {
                return copy_error_message(*database, errmsg);
            }
        }
        const int transaction_result = begin_direct_transaction(*database);
        if (transaction_result != MYLITE_OK) {
            mysql_query(&database->mysql, "ROLLBACK");
            return copy_error_message(*database, errmsg);
        }
#  endif
        mark_transaction_started(*database, read_only);
    } else if (
        is_completion_transaction_control(transaction_control) ||
        is_chained_completion_transaction_control(transaction_control)
    ) {
        const bool chain = is_chained_completion_transaction_control(transaction_control) ||
                           ((transaction_control == TransactionControlKind::Commit ||
                             transaction_control == TransactionControlKind::Rollback) &&
                            database->completion_type_chain);
        const bool restart_read_only =
            chain ? database->transaction_read_only
                  : transaction_start_read_only(*database, TransactionControlKind::Begin);
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        const bool commit = transaction_control == TransactionControlKind::Commit ||
                            transaction_control == TransactionControlKind::CommitNoChain ||
                            transaction_control == TransactionControlKind::CommitAndChain;
        const int transaction_result = finish_direct_transaction(*database, commit);
        if (transaction_result != MYLITE_OK) {
            return copy_error_message(*database, errmsg);
        }
        if (database->autocommit_disabled || chain) {
            const int restart_result = begin_direct_transaction(*database);
            if (restart_result != MYLITE_OK) {
                return copy_error_message(*database, errmsg);
            }
        }
#  endif
        mark_transaction_completed(*database);
        if (database->autocommit_disabled || chain) {
            mark_transaction_started(*database, restart_read_only);
        }
    } else if (transaction_control == TransactionControlKind::SetAutocommitOff) {
        const bool read_only = transaction_start_read_only(*database, transaction_control);
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        if (!database->transaction_active) {
            const int transaction_result = begin_direct_transaction(*database);
            if (transaction_result != MYLITE_OK) {
                mysql_query(&database->mysql, "SET autocommit=1");
                return copy_error_message(*database, errmsg);
            }
        }
#  endif
        database->autocommit_disabled = true;
        if (!database->transaction_active) {
            mark_transaction_started(*database, read_only);
        }
    } else if (transaction_control == TransactionControlKind::SetAutocommitOn) {
        if (database->autocommit_disabled) {
#  if MYLITE_MARIADB_HAS_MYLITE_SE
            const int transaction_result = finish_direct_transaction(*database, true);
            mark_transaction_completed(*database);
            if (transaction_result != MYLITE_OK) {
                return copy_error_message(*database, errmsg);
            }
#  endif
            database->autocommit_disabled = false;
            mark_transaction_completed(*database);
        }
    }
    return MYLITE_OK;
#endif
}

int prepare_impl(
    mylite_db *database,
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

    if (database == nullptr || sql == nullptr) {
        return MYLITE_MISUSE;
    }

    if (sql_len == MYLITE_NUL_TERMINATED) {
        sql_len = std::strlen(sql);
    }
    if (sql_len > static_cast<std::size_t>(ULONG_MAX)) {
        set_error(*database, MYLITE_MISUSE, "SQL text is too large");
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*database, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    StorageBusyTimeoutScope busy_timeout(database->busy_timeout_ms);
    set_ok(*database);
    clear_warnings(*database);
    const std::string_view sql_view(sql, sql_len);
    const TransactionControlKind transaction_control = direct_transaction_control_kind(sql_view);
    if (transaction_control == TransactionControlKind::Unsupported) {
        set_error(*database, MYLITE_ERROR, "unsupported SQL transaction control");
        return MYLITE_ERROR;
    }
#  if MYLITE_MARIADB_HAS_MYLITE_SE
    std::string savepoint_name;
    bool prepared_savepoint_control = false;
    if (is_savepoint_transaction_control(transaction_control)) {
        try {
            prepared_savepoint_control =
                direct_savepoint_control_name(sql_view, transaction_control, savepoint_name);
        } catch (const std::bad_alloc &) {
            set_error(*database, MYLITE_NOMEM, "savepoint allocation failed");
            return MYLITE_NOMEM;
        }
    }
#  else
    const bool prepared_savepoint_control = false;
#  endif
    if (transaction_control != TransactionControlKind::None && !prepared_savepoint_control) {
        set_error(*database, MYLITE_ERROR, "unsupported SQL transaction control");
        return MYLITE_ERROR;
    }
    if (const char *unsupported_message = unsupported_sql_surface_message(sql_view)) {
        set_error(*database, MYLITE_ERROR, unsupported_message);
        return MYLITE_ERROR;
    }

    try {
        std::unique_ptr<mylite_stmt> statement(new mylite_stmt());
        statement->database = database;
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        if (prepared_savepoint_control) {
            statement->prepared_transaction_control = transaction_control;
            statement->prepared_transaction_control_name = std::move(savepoint_name);
            ++database->active_statements;
            if (tail != nullptr) {
                *tail = sql + sql_len;
            }
            *out_stmt = statement.release();
            return MYLITE_OK;
        }
        const bool storage_outer_checkpoint_sql = is_storage_outer_checkpoint_sql(sql_view);
        const bool row_dml_checkpoint_sql = is_row_dml_checkpoint_sql(sql_view);
        statement->writes_storage = storage_outer_checkpoint_sql || row_dml_checkpoint_sql;
        statement->uses_storage_outer_checkpoint = storage_outer_checkpoint_sql;
        statement->sync_schema_catalog_after_execute =
            database->filename != ":memory:" && is_schema_catalog_sql(sql_view);
        statement->uses_statement_checkpoint =
            database->filename != ":memory:" && storage_outer_checkpoint_sql;
        statement->uses_transaction_statement_checkpoint =
            database->filename != ":memory:" && row_dml_checkpoint_sql;
#  endif
        statement->statement = mysql_stmt_init(&database->mysql);
        if (statement->statement == nullptr) {
            set_error(*database, MYLITE_NOMEM, "statement allocation failed");
            return MYLITE_NOMEM;
        }

        if (mysql_stmt_prepare(statement->statement, sql, static_cast<unsigned long>(sql_len)) !=
            0) {
            const unsigned warning_count = mysql_warning_count(&database->mysql);
            set_mariadb_statement_error(*statement);
            const int warning_result = capture_warnings(*database, warning_count, true);
            mysql_stmt_close(statement->statement);
            statement->statement = nullptr;
            if (warning_result != MYLITE_OK) {
                return warning_result;
            }
            return MYLITE_ERROR;
        }

        const unsigned long parameter_count = mysql_stmt_param_count(statement->statement);
        if (parameter_count > static_cast<unsigned long>(UINT_MAX)) {
            mysql_stmt_close(statement->statement);
            statement->statement = nullptr;
            set_error(*database, MYLITE_ERROR, "statement has too many parameters");
            return MYLITE_ERROR;
        }
        statement->parameters.resize(parameter_count);

        const int metadata_result = initialize_statement_metadata(*statement);
        if (metadata_result != MYLITE_OK) {
            mysql_stmt_close(statement->statement);
            statement->statement = nullptr;
            return metadata_result;
        }

        ++database->active_statements;
        if (tail != nullptr) {
            *tail = sql + sql_len;
        }
        *out_stmt = statement.release();
        return MYLITE_OK;
    } catch (const std::bad_alloc &) {
        set_error(*database, MYLITE_NOMEM, "statement allocation failed");
        return MYLITE_NOMEM;
    }
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
int initialize_statement_metadata(mylite_stmt &statement) {
    const unsigned field_count = mysql_stmt_field_count(statement.statement);
    if (field_count == 0U) {
        return MYLITE_OK;
    }

    MYSQL_RES *metadata = mysql_stmt_result_metadata(statement.statement);
    if (metadata == nullptr) {
        set_mariadb_statement_error(statement);
        return MYLITE_ERROR;
    }

    MYSQL_FIELD *fields = mysql_fetch_fields(metadata);
    if (fields == nullptr) {
        mysql_free_result(metadata);
        set_mariadb_statement_error(statement);
        return MYLITE_ERROR;
    }

    try {
        statement.columns.reserve(field_count);
        statement.values.resize(field_count);
        for (unsigned i = 0; i < field_count; ++i) {
            statement.columns.push_back(
                ColumnInfo{
                    field_string(fields[i].name, fields[i].name_length),
                    field_string(fields[i].db, fields[i].db_length),
                    field_string(fields[i].table, fields[i].table_length),
                    field_string(fields[i].org_table, fields[i].org_table_length),
                    field_string(fields[i].org_name, fields[i].org_name_length),
                    fields[i].type,
                    fields[i].flags,
                    fields[i].charsetnr,
                    fields[i].decimals,
                    fields[i].length,
                    fields[i].max_length,
                }
            );
        }
    } catch (const std::bad_alloc &) {
        mysql_free_result(metadata);
        set_error(*statement.database, MYLITE_NOMEM, "statement metadata allocation failed");
        return MYLITE_NOMEM;
    }

    mysql_free_result(metadata);
    return MYLITE_OK;
}

int bind_statement_parameters(mylite_stmt &statement) {
    const std::size_t parameter_count = statement.parameters.size();
    if (parameter_count == 0U) {
        return MYLITE_OK;
    }

    statement.parameter_binds.assign(parameter_count, MYSQL_BIND{});
    for (std::size_t i = 0; i < parameter_count; ++i) {
        BoundValue &value = statement.parameters[i];
        value.mysql_length = static_cast<unsigned long>(value.length);
        value.mysql_is_null = value.kind == BoundValueKind::Null ? 1 : 0;

        MYSQL_BIND &bind = statement.parameter_binds[i];
        bind.buffer_type = bound_value_type(value);
        bind.buffer = const_cast<void *>(bound_value_data(value));
        bind.length = &value.mysql_length;
        bind.is_null = &value.mysql_is_null;
        bind.buffer_length = value.mysql_length;
        bind.is_unsigned = value.kind == BoundValueKind::UInt64 ? 1 : 0;
    }

    if (mysql_stmt_bind_param(statement.statement, statement.parameter_binds.data()) != 0) {
        set_mariadb_statement_error(statement);
        return MYLITE_ERROR;
    }
    return MYLITE_OK;
}

int execute_statement(mylite_stmt &statement) {
    set_ok(*statement.database);
    clear_warnings(*statement.database);
    clear_current_row(statement);

#  if MYLITE_MARIADB_HAS_MYLITE_SE
    if (statement.prepared_transaction_control != TransactionControlKind::None) {
        return execute_prepared_savepoint_control(statement);
    }
    if (statement.database->transaction_active && statement.uses_storage_outer_checkpoint) {
        set_error(*statement.database, MYLITE_ERROR, "unsupported transactional DDL SQL surface");
        return MYLITE_ERROR;
    }
    if (statement.database->transaction_active && statement.database->transaction_read_only &&
        statement.writes_storage) {
        set_error(
            *statement.database,
            MYLITE_ERROR,
            "unsupported read-only transaction write SQL surface"
        );
        return MYLITE_ERROR;
    }
#  endif

    const int bind_result = bind_statement_parameters(statement);
    if (bind_result != MYLITE_OK) {
        return bind_result;
    }

#  if MYLITE_MARIADB_HAS_MYLITE_SE
    StorageStatementCheckpoint checkpoint;
    const bool checkpoint_enabled =
        statement.uses_statement_checkpoint ||
        (statement.database->transaction_active && statement.uses_transaction_statement_checkpoint);
    int checkpoint_result = checkpoint.begin(*statement.database, checkpoint_enabled);
    if (checkpoint_result != MYLITE_OK) {
        return checkpoint_result;
    }
#  endif

    if (mysql_stmt_execute(statement.statement) != 0) {
        const unsigned warning_count = mysql_warning_count(&statement.database->mysql);
        set_mariadb_statement_error(statement);
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        checkpoint_result = checkpoint.rollback(*statement.database);
        if (checkpoint_result != MYLITE_OK) {
            return checkpoint_result;
        }
#  endif
        const int warning_result = capture_warnings(*statement.database, warning_count, true);
        return warning_result == MYLITE_OK ? MYLITE_ERROR : warning_result;
    }

    statement.executed = true;
    statement.database->changes = 0;
    statement.database->last_insert_id =
        static_cast<unsigned long long>(mysql_stmt_insert_id(statement.statement));

    if (statement.columns.empty()) {
        const my_ulonglong affected_rows = mysql_stmt_affected_rows(statement.statement);
        statement.database->changes =
            affected_rows == static_cast<my_ulonglong>(-1)
                ? 0
                : static_cast<long long>(
                      std::min<my_ulonglong>(affected_rows, static_cast<my_ulonglong>(LLONG_MAX))
                  );
        statement.done = true;
        const unsigned warning_count = mysql_warning_count(&statement.database->mysql);
        mysql_stmt_free_result(statement.statement);
        const int warning_result = capture_warnings(*statement.database, warning_count);
        if (warning_result != MYLITE_OK) {
#  if MYLITE_MARIADB_HAS_MYLITE_SE
            checkpoint_result = checkpoint.commit(*statement.database);
            if (checkpoint_result != MYLITE_OK) {
                return checkpoint_result;
            }
#  endif
            return warning_result;
        }
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        if (statement.sync_schema_catalog_after_execute) {
            const int sync_result = sync_schema_catalog(*statement.database);
            if (sync_result != MYLITE_OK) {
                checkpoint_result = checkpoint.rollback(*statement.database);
                if (checkpoint_result != MYLITE_OK) {
                    return checkpoint_result;
                }
                return sync_result;
            }
        }
        checkpoint_result = checkpoint.commit(*statement.database);
        if (checkpoint_result != MYLITE_OK) {
            return checkpoint_result;
        }
#  endif
        return MYLITE_DONE;
    }

    const int result_bind_result = bind_statement_results(statement);
    if (result_bind_result != MYLITE_OK) {
#  if MYLITE_MARIADB_HAS_MYLITE_SE
        checkpoint_result = checkpoint.commit(*statement.database);
        if (checkpoint_result != MYLITE_OK) {
            return checkpoint_result;
        }
#  endif
        return result_bind_result;
    }
#  if MYLITE_MARIADB_HAS_MYLITE_SE
    checkpoint_result = checkpoint.commit(*statement.database);
    if (checkpoint_result != MYLITE_OK) {
        return checkpoint_result;
    }
#  endif
    return fetch_statement_row(statement);
}

#  if MYLITE_MARIADB_HAS_MYLITE_SE
int execute_prepared_savepoint_control(mylite_stmt &statement) {
    mylite_db &database = *statement.database;
    if (!database.transaction_active || database.filename == ":memory:") {
        set_error(database, MYLITE_ERROR, "unsupported savepoint transaction control");
        return MYLITE_ERROR;
    }

    const int result = execute_direct_savepoint_control(
        database,
        statement.prepared_transaction_control,
        statement.prepared_transaction_control_name
    );
    if (result != MYLITE_OK) {
        return result;
    }

    statement.executed = true;
    statement.done = true;
    database.changes = 0;
    database.last_insert_id = 0;
    return MYLITE_DONE;
}

StorageStatementCheckpoint::~StorageStatementCheckpoint() {
    if (statement_ != nullptr) {
        mylite_storage_commit_statement(statement_);
    }
}

int StorageStatementCheckpoint::begin(mylite_db &database, bool enabled) {
    if (!enabled || database.filename == ":memory:") {
        return MYLITE_OK;
    }

    const mylite_storage_result result =
        mylite_storage_begin_statement(database.filename.c_str(), &statement_);
    if (result != MYLITE_STORAGE_OK) {
        set_error(database, map_storage_result(result), "statement checkpoint failed");
        return database.errcode;
    }
    return MYLITE_OK;
}

int StorageStatementCheckpoint::commit(mylite_db &database) {
    if (statement_ == nullptr) {
        return MYLITE_OK;
    }

    mylite_storage_statement *statement = statement_;
    statement_ = nullptr;
    const mylite_storage_result result = mylite_storage_commit_statement(statement);
    if (result != MYLITE_STORAGE_OK) {
        set_error(database, map_storage_result(result), "statement checkpoint commit failed");
        return database.errcode;
    }
    return MYLITE_OK;
}

int StorageStatementCheckpoint::rollback(mylite_db &database) {
    if (statement_ == nullptr) {
        return MYLITE_OK;
    }

    mylite_storage_statement *statement = statement_;
    statement_ = nullptr;
    const mylite_storage_result result = mylite_storage_rollback_statement(statement);
    if (result != MYLITE_STORAGE_OK) {
        set_error(database, map_storage_result(result), "statement rollback failed");
        return database.errcode;
    }
    return MYLITE_OK;
}

int begin_direct_transaction(mylite_db &database) {
    if (database.filename == ":memory:" || database.transaction_statement != nullptr) {
        return MYLITE_OK;
    }

    const mylite_storage_result result = mylite_storage_begin_transaction(
        database.filename.c_str(),
        &database.transaction_statement
    );
    if (result != MYLITE_STORAGE_OK) {
        set_error(database, map_storage_result(result), "transaction checkpoint failed");
        return database.errcode;
    }
    return MYLITE_OK;
}

int finish_direct_transaction(mylite_db &database, bool commit) {
    const int savepoint_result = finish_direct_savepoints(database, commit);
    if (savepoint_result != MYLITE_OK) {
        return savepoint_result;
    }

    if (database.transaction_statement == nullptr) {
        return MYLITE_OK;
    }

    mylite_storage_statement *transaction = database.transaction_statement;
    database.transaction_statement = nullptr;
    const mylite_storage_result result = commit ? mylite_storage_commit_statement(transaction)
                                                : mylite_storage_rollback_statement(transaction);
    if (result != MYLITE_STORAGE_OK) {
        set_error(
            database,
            map_storage_result(result),
            commit ? "transaction checkpoint commit failed" : "transaction rollback failed"
        );
        return database.errcode;
    }
    return MYLITE_OK;
}

int execute_direct_savepoint_control(
    mylite_db &database,
    TransactionControlKind kind,
    std::string_view name
) {
    if (kind == TransactionControlKind::Savepoint) {
        return set_direct_savepoint(database, name);
    }
    if (kind == TransactionControlKind::RollbackToSavepoint) {
        return rollback_to_direct_savepoint(database, name);
    }
    if (kind == TransactionControlKind::ReleaseSavepoint) {
        return release_direct_savepoint(database, name);
    }
    return MYLITE_OK;
}

int set_direct_savepoint(mylite_db &database, std::string_view name) {
    const int result = begin_direct_savepoint_frame(database, name);
    if (result != MYLITE_OK) {
        return result;
    }

    const std::size_t replacement_index = database.savepoints.size() - 1U;
    const std::string_view replacement_name = database.savepoints[replacement_index].name;
    for (std::size_t index = replacement_index; index > 0U; --index) {
        DirectSavepoint &candidate = database.savepoints[index - 1U];
        if (candidate.active && candidate.name == replacement_name) {
            candidate.active = false;
            break;
        }
    }
    return MYLITE_OK;
}

int rollback_to_direct_savepoint(mylite_db &database, std::string_view name) {
    const std::size_t target = find_direct_savepoint(database, name);
    if (target == database.savepoints.size()) {
        set_error(database, MYLITE_ERROR, "savepoint transaction control target not found");
        return MYLITE_ERROR;
    }

    std::string savepoint_name;
    try {
        savepoint_name = database.savepoints[target].name;
    } catch (const std::bad_alloc &) {
        set_error(database, MYLITE_NOMEM, "savepoint allocation failed");
        return MYLITE_NOMEM;
    }

    while (database.savepoints.size() > target + 1U) {
        const int result =
            finish_direct_savepoint_frame(database, database.savepoints.size() - 1U, false);
        if (result != MYLITE_OK) {
            return result;
        }
    }

    const int rollback_result = finish_direct_savepoint_frame(database, target, false);
    if (rollback_result != MYLITE_OK) {
        return rollback_result;
    }
    return begin_direct_savepoint_frame(database, savepoint_name);
}

int release_direct_savepoint(mylite_db &database, std::string_view name) {
    const std::size_t target = find_direct_savepoint(database, name);
    if (target == database.savepoints.size()) {
        set_error(database, MYLITE_ERROR, "savepoint transaction control target not found");
        return MYLITE_ERROR;
    }

    while (database.savepoints.size() > target) {
        const int result =
            finish_direct_savepoint_frame(database, database.savepoints.size() - 1U, true);
        if (result != MYLITE_OK) {
            return result;
        }
    }
    return MYLITE_OK;
}

int finish_direct_savepoints(mylite_db &database, bool commit) {
    while (!database.savepoints.empty()) {
        const int result =
            finish_direct_savepoint_frame(database, database.savepoints.size() - 1U, commit);
        if (result != MYLITE_OK) {
            return result;
        }
    }
    return MYLITE_OK;
}

std::size_t find_direct_savepoint(const mylite_db &database, std::string_view name) {
    for (std::size_t index = database.savepoints.size(); index > 0U; --index) {
        const DirectSavepoint &candidate = database.savepoints[index - 1U];
        if (candidate.active && candidate.name == name) {
            return index - 1U;
        }
    }
    return database.savepoints.size();
}

int finish_direct_savepoint_frame(mylite_db &database, std::size_t index, bool commit) {
    if (index + 1U != database.savepoints.size()) {
        set_error(database, MYLITE_MISUSE, "savepoint checkpoint order mismatch");
        return MYLITE_MISUSE;
    }

    mylite_storage_statement *statement = database.savepoints.back().statement;
    database.savepoints.pop_back();
    if (statement == nullptr) {
        return MYLITE_OK;
    }

    const mylite_storage_result result = commit ? mylite_storage_commit_statement(statement)
                                                : mylite_storage_rollback_statement(statement);
    if (result != MYLITE_STORAGE_OK) {
        set_error(
            database,
            map_storage_result(result),
            commit ? "savepoint checkpoint commit failed" : "savepoint rollback failed"
        );
        return database.errcode;
    }
    return MYLITE_OK;
}

int begin_direct_savepoint_frame(mylite_db &database, std::string_view name) {
    DirectSavepoint savepoint;
    try {
        savepoint.name = std::string(name);
    } catch (const std::bad_alloc &) {
        set_error(database, MYLITE_NOMEM, "savepoint allocation failed");
        return MYLITE_NOMEM;
    }

    if (database.filename != ":memory:") {
        const mylite_storage_result result =
            mylite_storage_begin_statement(database.filename.c_str(), &savepoint.statement);
        if (result != MYLITE_STORAGE_OK) {
            set_error(database, map_storage_result(result), "savepoint checkpoint failed");
            return database.errcode;
        }
    }

    try {
        database.savepoints.push_back(std::move(savepoint));
    } catch (const std::bad_alloc &) {
        if (savepoint.statement != nullptr) {
            mylite_storage_rollback_statement(savepoint.statement);
        }
        set_error(database, MYLITE_NOMEM, "savepoint allocation failed");
        return MYLITE_NOMEM;
    }
    return MYLITE_OK;
}
#  endif

int fetch_statement_row(mylite_stmt &statement) {
    clear_current_row(statement);
    const int fetch_result = mysql_stmt_fetch(statement.statement);

    if (fetch_result == MYSQL_NO_DATA) {
        statement.done = true;
        const unsigned warning_count = mysql_warning_count(&statement.database->mysql);
        mysql_stmt_free_result(statement.statement);
        const int warning_result = capture_warnings(*statement.database, warning_count);
        return warning_result == MYLITE_OK ? MYLITE_DONE : warning_result;
    }
    if (fetch_result != 0 && fetch_result != MYSQL_DATA_TRUNCATED) {
        set_mariadb_statement_error(statement);
        return MYLITE_ERROR;
    }

    for (unsigned i = 0; i < statement.values.size(); ++i) {
        ColumnValue &value = statement.values[i];
        if (value.mysql_error != 0 &&
            !is_variable_column_type(map_column_type(statement.columns[i]))) {
            set_error(*statement.database, MYLITE_ERROR, "numeric column truncated during fetch");
            return MYLITE_ERROR;
        }
    }

    for (unsigned i = 0; i < statement.values.size(); ++i) {
        ColumnValue &value = statement.values[i];
        value.type =
            value.mysql_is_null != 0 ? MYLITE_TYPE_NULL : map_column_type(statement.columns[i]);
        if (is_variable_column_type(value.type)) {
            const std::size_t stored_bytes =
                std::min<std::size_t>(value.buffer_length, value.mysql_length);
            value.bytes_complete = value.mysql_is_null != 0 || stored_bytes >= value.mysql_length;
            if (value.type == MYLITE_TYPE_TEXT) {
                if (value.bytes.size() <= stored_bytes) {
                    value.bytes.resize(stored_bytes + 1U);
                }
                value.bytes[stored_bytes] = '\0';
            }
        } else {
            value.bytes_complete = true;
        }
    }

    statement.has_current_row = true;
    return MYLITE_ROW;
}

int bind_statement_results(mylite_stmt &statement) {
    statement.result_binds.assign(statement.columns.size(), MYSQL_BIND{});
    for (std::size_t i = 0; i < statement.columns.size(); ++i) {
        ColumnValue &value = statement.values[i];
        const mylite_value_type type = map_column_type(statement.columns[i]);
        value.type = type;
        value.mysql_length = 0;
        value.mysql_is_null = false;
        value.mysql_error = 0;

        MYSQL_BIND &bind = statement.result_binds[i];
        bind.length = &value.mysql_length;
        bind.is_null = &value.mysql_is_null;
        bind.error = &value.mysql_error;

        switch (type) {
        case MYLITE_TYPE_INT64:
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.buffer = &value.int64_value;
            break;
        case MYLITE_TYPE_UINT64:
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.buffer = &value.uint64_value;
            bind.is_unsigned = 1;
            break;
        case MYLITE_TYPE_DOUBLE:
            bind.buffer_type = MYSQL_TYPE_DOUBLE;
            bind.buffer = &value.double_value;
            break;
        case MYLITE_TYPE_BLOB:
        case MYLITE_TYPE_TEXT:
            value.buffer_length = k_initial_column_buffer_size;
            value.bytes_complete = false;
            value.bytes.assign(
                static_cast<std::size_t>(value.buffer_length) +
                    (type == MYLITE_TYPE_TEXT ? 1U : 0U),
                0
            );
            bind.buffer_type = type == MYLITE_TYPE_BLOB ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
            bind.buffer = value.bytes.data();
            bind.buffer_length = value.buffer_length;
            break;
        case MYLITE_TYPE_NULL:
            bind.buffer_type = MYSQL_TYPE_NULL;
            break;
        }
    }

    if (mysql_stmt_bind_result(statement.statement, statement.result_binds.data()) != 0) {
        set_mariadb_statement_error(statement);
        return MYLITE_ERROR;
    }
    return MYLITE_OK;
}

int fetch_truncated_column(mylite_stmt &statement, unsigned column) {
    ColumnValue &value = statement.values[column];
    const mylite_value_type type = map_column_type(statement.columns[column]);
    if (!is_variable_column_type(type)) {
        set_error(*statement.database, MYLITE_ERROR, "numeric column truncated during fetch");
        return MYLITE_ERROR;
    }

    value.buffer_length = value.mysql_length;
    value.bytes.assign(
        static_cast<std::size_t>(value.buffer_length) + (type == MYLITE_TYPE_TEXT ? 1U : 0U),
        0
    );
    value.mysql_error = 0;

    MYSQL_BIND bind = {};
    bind.buffer_type = type == MYLITE_TYPE_BLOB ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
    bind.buffer = value.bytes.data();
    bind.buffer_length = value.buffer_length;
    bind.length = &value.mysql_length;
    bind.is_null = &value.mysql_is_null;
    bind.error = &value.mysql_error;

    if (mysql_stmt_fetch_column(statement.statement, &bind, column, 0) != 0) {
        set_mariadb_statement_error(statement);
        return MYLITE_ERROR;
    }

    if (type == MYLITE_TYPE_TEXT) {
        value.bytes[static_cast<std::size_t>(value.mysql_length)] = '\0';
    }
    value.bytes_complete = true;
    statement.result_binds[column].buffer = value.bytes.data();
    statement.result_binds[column].buffer_length = value.buffer_length;
    return MYLITE_OK;
}

int materialize_column_value(mylite_stmt &statement, unsigned column) {
    if (column >= statement.values.size()) {
        return MYLITE_MISUSE;
    }
    ColumnValue &value = statement.values[column];
    if (value.bytes_complete || value.mysql_is_null != 0) {
        return MYLITE_OK;
    }
    return fetch_truncated_column(statement, column);
}

int capture_warnings(mylite_db &database, unsigned warning_count, bool force_query) {
    clear_warnings(database);
    database.warning_count = warning_count;
    if (warning_count == 0U && !force_query) {
        return MYLITE_OK;
    }

    if (mysql_query(&database.mysql, "SHOW WARNINGS") != 0) {
        clear_warnings(database);
        set_mariadb_error(database);
        return MYLITE_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(&database.mysql);
    if (result == nullptr) {
        if (mysql_field_count(&database.mysql) != 0U) {
            clear_warnings(database);
            set_mariadb_error(database);
            return MYLITE_ERROR;
        }
        return MYLITE_OK;
    }

    try {
        for (MYSQL_ROW row = mysql_fetch_row(result); row != nullptr;
             row = mysql_fetch_row(result)) {
            unsigned long *lengths = mysql_fetch_lengths(result);
            if (lengths == nullptr || row[0] == nullptr || row[1] == nullptr || row[2] == nullptr) {
                mysql_free_result(result);
                clear_warnings(database);
                set_error(database, MYLITE_ERROR, "malformed SHOW WARNINGS result");
                return MYLITE_ERROR;
            }

            const unsigned long parsed_code = std::strtoul(row[1], nullptr, 10);
            database.warnings.push_back(
                StoredWarning{
                    map_warning_level(row[0]),
                    parsed_code > static_cast<unsigned long>(UINT_MAX)
                        ? UINT_MAX
                        : static_cast<unsigned>(parsed_code),
                    std::string(row[2], static_cast<std::size_t>(lengths[2])),
                }
            );
        }
        database.warning_count = database.warnings.size() > static_cast<std::size_t>(UINT_MAX)
                                     ? UINT_MAX
                                     : static_cast<unsigned>(database.warnings.size());
    } catch (const std::bad_alloc &) {
        mysql_free_result(result);
        clear_warnings(database);
        set_error(database, MYLITE_NOMEM, "warning allocation failed");
        return MYLITE_NOMEM;
    }

    if (mysql_errno(&database.mysql) != 0U) {
        mysql_free_result(result);
        clear_warnings(database);
        set_mariadb_error(database);
        return MYLITE_ERROR;
    }

    mysql_free_result(result);
    return MYLITE_OK;
}

void clear_current_row(mylite_stmt &statement) {
    statement.has_current_row = false;
    for (ColumnValue &value : statement.values) {
        value.type = MYLITE_TYPE_NULL;
        value.mysql_length = 0;
        value.mysql_is_null = true;
        value.mysql_error = 0;
        value.bytes_complete = false;
    }
}

bool is_variable_column_type(mylite_value_type type) {
    return type == MYLITE_TYPE_TEXT || type == MYLITE_TYPE_BLOB;
}

const void *bound_value_data(const BoundValue &value) {
    static const unsigned char empty_value = 0;
    switch (value.kind) {
    case BoundValueKind::Int64:
        return &value.int64_value;
    case BoundValueKind::UInt64:
        return &value.uint64_value;
    case BoundValueKind::Double:
        return &value.double_value;
    case BoundValueKind::Null:
    case BoundValueKind::Text:
    case BoundValueKind::Blob:
        break;
    }
    if (!value.owned_data.empty()) {
        return value.owned_data.data();
    }
    if (value.borrowed_data != nullptr) {
        return value.borrowed_data;
    }
    return &empty_value;
}

enum enum_field_types bound_value_type(const BoundValue &value) {
    switch (value.kind) {
    case BoundValueKind::Null:
        return MYSQL_TYPE_NULL;
    case BoundValueKind::Int64:
    case BoundValueKind::UInt64:
        return MYSQL_TYPE_LONGLONG;
    case BoundValueKind::Double:
        return MYSQL_TYPE_DOUBLE;
    case BoundValueKind::Text:
        return MYSQL_TYPE_STRING;
    case BoundValueKind::Blob:
        return MYSQL_TYPE_BLOB;
    }
    return MYSQL_TYPE_NULL;
}

mylite_value_type map_column_type(const ColumnInfo &column) {
    return map_mariadb_type(column.type, column.flags);
}

mylite_value_type map_mariadb_type(enum enum_field_types type, unsigned int flags) {
    switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_YEAR:
        return is_unsigned_column(flags) ? MYLITE_TYPE_UINT64 : MYLITE_TYPE_INT64;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
        return MYLITE_TYPE_DOUBLE;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_GEOMETRY:
        return MYLITE_TYPE_BLOB;
    case MYSQL_TYPE_NULL:
        return MYLITE_TYPE_NULL;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_TIMESTAMP2:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_BLOB_COMPRESSED:
    case MYSQL_TYPE_VARCHAR_COMPRESSED:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
        return MYLITE_TYPE_TEXT;
    }
    return MYLITE_TYPE_TEXT;
}

bool is_unsigned_column(unsigned int flags) {
    return (flags & UNSIGNED_FLAG) != 0U;
}

mylite_warning_level map_warning_level(const char *level) {
    if (std::strcmp(level, "Note") == 0) {
        return MYLITE_WARNING_NOTE;
    }
    if (std::strcmp(level, "Error") == 0) {
        return MYLITE_WARNING_ERROR;
    }
    return MYLITE_WARNING_WARNING;
}

std::string field_string(const char *value, unsigned int length) {
    return value != nullptr ? std::string(value, static_cast<std::size_t>(length)) : std::string();
}

int store_and_emit_result(
    mylite_db &database,
    mylite_exec_callback callback,
    void *ctx,
    bool *has_result
) {
    MYSQL_RES *result = mysql_store_result(&database.mysql);
    if (result == nullptr) {
        if (mysql_field_count(&database.mysql) != 0U) {
            set_mariadb_error(database);
            return MYLITE_ERROR;
        }
        return MYLITE_OK;
    }
    *has_result = true;

    const unsigned field_count = mysql_num_fields(result);
    if (field_count > static_cast<unsigned>(INT_MAX)) {
        mysql_free_result(result);
        set_error(database, MYLITE_ERROR, "result has too many columns");
        return MYLITE_ERROR;
    }

    std::vector<char *> column_names;
    column_names.reserve(field_count);
    MYSQL_FIELD *fields = mysql_fetch_fields(result);
    for (unsigned i = 0; i < field_count; ++i) {
        column_names.push_back(fields[i].name);
    }

    for (MYSQL_ROW row = mysql_fetch_row(result); row != nullptr; row = mysql_fetch_row(result)) {
        if (callback != nullptr &&
            callback(ctx, static_cast<int>(field_count), row, column_names.data()) != 0) {
            mysql_free_result(result);
            set_error(database, MYLITE_ERROR, "query callback requested abort");
            return MYLITE_ERROR;
        }
    }

    mysql_free_result(result);
    return MYLITE_OK;
}

const char *unsupported_sql_surface_message(std::string_view sql) {
    if (is_server_surface_sql(sql)) {
        return "unsupported server-oriented SQL surface";
    }
    if (is_backup_sql(sql)) {
        return "unsupported external backup SQL surface";
    }
    if (is_query_cache_sql(sql)) {
        return "unsupported query-cache SQL surface";
    }
    if (is_statement_profiling_sql(sql)) {
        return "unsupported statement-profiling SQL surface";
    }
    if (is_optimizer_trace_sql(sql)) {
        return "unsupported optimizer-trace SQL surface";
    }
    if (is_table_maintenance_sql(sql)) {
        return "unsupported table-maintenance SQL surface";
    }
    if (is_sql_handler_command_sql(sql)) {
        return "unsupported SQL HANDLER command";
    }
    if (is_help_command_sql(sql)) {
        return "unsupported HELP SQL command";
    }
    if (is_static_show_info_sql(sql)) {
        return "unsupported static SHOW information SQL surface";
    }
    if (is_processlist_metadata_sql(sql)) {
        return "unsupported process-list metadata SQL surface";
    }
    if (is_file_import_sql(sql)) {
        return "unsupported SQL file import surface";
    }
    if (is_file_export_sql(sql)) {
        return "unsupported SQL file export surface";
    }
    if (is_server_utility_function_sql(sql)) {
        return "unsupported server utility SQL function";
    }
    if (is_gis_sql_function_sql(sql)) {
        return "unsupported GIS SQL function";
    }
    if (is_sformat_sql_function_sql(sql)) {
        return "unsupported SFORMAT SQL function";
    }
    if (is_json_schema_valid_sql(sql)) {
        return "unsupported JSON_SCHEMA_VALID SQL function";
    }
    if (is_json_table_sql(sql)) {
        return "unsupported JSON_TABLE table function";
    }
    if (is_dynamic_column_sql(sql)) {
        return "unsupported dynamic column SQL function";
    }
    if (is_sequence_sql(sql)) {
        return "unsupported SQL sequence surface";
    }
    if (is_user_statistics_sql(sql)) {
        return "unsupported user-statistics SQL surface";
    }
    if (is_xml_sql_function_sql(sql)) {
        return "unsupported XML SQL function";
    }
    if (is_oracle_sql_mode_sql(sql)) {
        return "unsupported Oracle SQL mode";
    }
    if (is_non_table_object_sql(sql)) {
        return "unsupported non-table database object SQL surface";
    }
    if (is_procedure_analyse_sql(sql)) {
        return "unsupported PROCEDURE ANALYSE SQL clause";
    }
    if (is_select_procedure_sql(sql)) {
        return "unsupported SELECT PROCEDURE SQL clause";
    }
    if (direct_transaction_control_kind(sql) == TransactionControlKind::Unsupported) {
        return "unsupported SQL transaction control";
    }
    if (is_locking_sql(sql)) {
        return "unsupported SQL locking surface";
    }
    if (is_online_alter_sql(sql)) {
        return "unsupported online ALTER SQL surface";
    }
    if (is_partition_sql(sql)) {
        return "unsupported partition SQL surface";
    }
    if (is_foreign_key_sql(sql)) {
        return "unsupported foreign-key SQL surface";
    }
    return nullptr;
}

bool is_server_surface_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);
    const std::string_view second = pop_sql_token(rest);
    const std::string_view third = pop_sql_token(rest);
    const std::string_view fourth = pop_sql_token(rest);

    if (sql_token_equals(first, "BINLOG") || sql_token_equals(first, "GRANT") ||
        sql_token_equals(first, "REVOKE")) {
        return true;
    }

    if (sql_token_equals(first, "CREATE")) {
        if (sql_token_equals(second, "OR") && sql_token_equals(third, "REPLACE")) {
            return sql_token_equals(fourth, "SERVER");
        }
        return sql_token_equals(second, "USER") || sql_token_equals(second, "ROLE") ||
               sql_token_equals(second, "EVENT") || sql_token_equals(second, "SERVER");
    }

    if (sql_token_equals(first, "ALTER")) {
        return sql_token_equals(second, "USER") || sql_token_equals(second, "EVENT") ||
               sql_token_equals(second, "SERVER");
    }

    if (sql_token_equals(first, "DROP")) {
        return sql_token_equals(second, "USER") || sql_token_equals(second, "ROLE") ||
               sql_token_equals(second, "EVENT") || sql_token_equals(second, "SERVER");
    }

    if (sql_token_equals(first, "RENAME")) {
        return sql_token_equals(second, "USER");
    }

    if (sql_token_equals(first, "SET")) {
        return sql_token_equals(second, "PASSWORD") ||
               (sql_token_equals(second, "GLOBAL") && sql_token_equals(third, "EVENT_SCHEDULER"));
    }

    if (sql_token_equals(first, "SHOW")) {
        return (sql_token_equals(second, "CREATE") && sql_token_equals(third, "SERVER")) ||
               sql_token_equals(second, "MASTER") || sql_token_equals(second, "SLAVE") ||
               sql_token_equals(second, "REPLICA");
    }

    if (sql_token_equals(first, "INSTALL") || sql_token_equals(first, "UNINSTALL")) {
        return sql_token_equals(second, "PLUGIN") || sql_token_equals(second, "SONAME");
    }

    if (sql_token_equals(first, "CHANGE")) {
        return sql_token_equals(second, "MASTER") || sql_token_equals(second, "REPLICATION");
    }

    if (sql_token_equals(first, "START") || sql_token_equals(first, "STOP") ||
        sql_token_equals(first, "RESET")) {
        return sql_token_equals(second, "MASTER") || sql_token_equals(second, "SLAVE") ||
               sql_token_equals(second, "REPLICA");
    }

    return false;
}

bool is_backup_sql(std::string_view sql) {
    std::string_view first;
    return pop_sql_scanned_token(sql, first) && sql_token_equals(first, "BACKUP");
}

bool is_query_cache_sql(std::string_view sql) {
    std::string_view token;
    if (!pop_sql_scanned_token(sql, token)) {
        return false;
    }

    if (sql_token_equals(token, "FLUSH")) {
        if (!pop_sql_scanned_token(sql, token)) {
            return false;
        }
        if (sql_token_equals(token, "LOCAL") || sql_token_equals(token, "NO_WRITE_TO_BINLOG")) {
            if (!pop_sql_scanned_token(sql, token)) {
                return false;
            }
        }
        if (!sql_token_equals(token, "QUERY")) {
            return false;
        }
        return pop_sql_scanned_token(sql, token) && sql_token_equals(token, "CACHE");
    }

    if (sql_token_equals(token, "RESET")) {
        return pop_sql_scanned_token(sql, token) && sql_token_equals(token, "QUERY") &&
               pop_sql_scanned_token(sql, token) && sql_token_equals(token, "CACHE");
    }

    if (sql_token_equals(token, "SET")) {
        sql = sql_set_assignment_policy_span(sql);
        while (!skip_sql_leading_noise(sql).empty()) {
            if (sql_set_assignment_targets_query_cache_variable(pop_sql_set_assignment(sql))) {
                return true;
            }
        }
    }

    return false;
}

bool is_statement_profiling_sql(std::string_view sql) {
    std::string_view rest = sql;
    std::string_view token;
    if (!pop_sql_scanned_token(rest, token)) {
        return false;
    }

    if (sql_token_equals(token, "SHOW")) {
        return pop_sql_scanned_token(rest, token) &&
               (sql_token_equals(token, "PROFILE") || sql_token_equals(token, "PROFILES"));
    }

    if (sql_token_equals(token, "SET")) {
        rest = sql_set_assignment_policy_span(rest);
        while (!skip_sql_leading_noise(rest).empty()) {
            if (sql_set_assignment_targets_statement_profiling_variable(
                    pop_sql_set_assignment(rest)
                )) {
                return true;
            }
        }
    }

    return sql_tokens_contain_statement_profiling_information_schema_table(sql);
}

bool is_optimizer_trace_sql(std::string_view sql) {
    std::string_view rest = sql;
    std::string_view token;
    if (!pop_sql_scanned_token(rest, token)) {
        return false;
    }

    if (sql_token_equals(token, "SET")) {
        rest = sql_set_assignment_policy_span(rest);
        while (!skip_sql_leading_noise(rest).empty()) {
            if (sql_set_assignment_targets_optimizer_trace_variable(pop_sql_set_assignment(rest))) {
                return true;
            }
        }
    }

    return sql_tokens_contain_optimizer_trace_information_schema_table(sql);
}

bool is_table_maintenance_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);
    std::string_view second = pop_sql_token(rest);
    const bool is_table_maintenance_command =
        sql_token_equals(first, "ANALYZE") || sql_token_equals(first, "CHECK") ||
        sql_token_equals(first, "OPTIMIZE") || sql_token_equals(first, "REPAIR");

    if (is_table_maintenance_command &&
        (sql_token_equals(second, "LOCAL") || sql_token_equals(second, "NO_WRITE_TO_BINLOG"))) {
        second = pop_sql_token(rest);
    }

    if (is_table_maintenance_command && sql_token_equals(second, "TABLE")) {
        return true;
    }

    if (sql_token_equals(first, "CACHE")) {
        return sql_token_equals(second, "INDEX");
    }

    return sql_token_equals(first, "LOAD") && sql_token_equals(second, "INDEX");
}

bool is_sql_handler_command_sql(std::string_view sql) {
    std::string_view first;
    return pop_sql_scanned_token(sql, first) && sql_token_equals(first, "HANDLER");
}

bool is_help_command_sql(std::string_view sql) {
    std::string_view rest = sql;
    std::string_view first;
    return pop_sql_scanned_token(rest, first) && sql_token_equals(first, "HELP");
}

bool is_static_show_info_sql(std::string_view sql) {
    std::string_view rest = sql;
    std::string_view first;
    std::string_view second;
    if (!pop_sql_scanned_token(rest, first) || !sql_token_equals(first, "SHOW") ||
        !pop_sql_scanned_token(rest, second)) {
        return false;
    }

    return sql_token_equals(second, "AUTHORS") || sql_token_equals(second, "CONTRIBUTORS") ||
           sql_token_equals(second, "PRIVILEGES");
}

bool is_processlist_metadata_sql(std::string_view sql) {
    std::string_view rest = sql;
    std::string_view first;
    std::string_view second;
    std::string_view third;
    if (!pop_sql_scanned_token(rest, first) || !sql_token_equals(first, "SHOW") ||
        !pop_sql_scanned_token(rest, second)) {
        return false;
    }

    if (sql_token_equals(second, "PROCESSLIST")) {
        return true;
    }

    return sql_token_equals(second, "FULL") && pop_sql_scanned_token(rest, third) &&
           sql_token_equals(third, "PROCESSLIST");
}

bool is_file_import_sql(std::string_view sql) {
    std::string_view rest = sql;
    std::string_view first;
    std::string_view second;

    if (pop_sql_scanned_token(rest, first) && sql_token_equals(first, "LOAD") &&
        pop_sql_scanned_token(rest, second) &&
        (sql_token_equals(second, "DATA") || sql_token_equals(second, "XML"))) {
        return true;
    }

    return sql_tokens_contain_file_import_function(sql);
}

bool is_file_export_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);

    if (sql_token_equals(first, "EXPLAIN")) {
        return false;
    }

    return sql_tokens_contain_file_export_marker(rest);
}

bool is_server_utility_function_sql(std::string_view sql) {
    return sql_tokens_contain_server_utility_function(sql);
}

bool is_gis_sql_function_sql(std::string_view sql) {
    return sql_tokens_contain_gis_sql_function(sql);
}

bool is_sformat_sql_function_sql(std::string_view sql) {
    return sql_tokens_contain_sformat_sql_function(sql);
}

bool is_json_schema_valid_sql(std::string_view sql) {
    return sql_tokens_contain_json_schema_valid_function(sql);
}

bool is_json_table_sql(std::string_view sql) {
    return sql_tokens_contain_json_table_function(sql);
}

bool is_dynamic_column_sql(std::string_view sql) {
    return sql_tokens_contain_dynamic_column_function(sql);
}

bool is_sequence_sql(std::string_view sql) {
    return sql_tokens_contain_sequence_value_surface(sql);
}

bool is_user_statistics_sql(std::string_view sql) {
    std::string_view rest = sql;
    std::string_view token;
    if (!pop_sql_scanned_token(rest, token)) {
        return false;
    }

    if (sql_token_equals(token, "SHOW")) {
        return pop_sql_scanned_token(rest, token) && sql_token_is_user_statistics_table(token);
    }

    if (sql_token_equals(token, "FLUSH")) {
        while (pop_sql_scanned_token(rest, token)) {
            if (sql_token_is_user_statistics_table(token)) {
                return true;
            }
        }
        return false;
    }

    if (sql_token_equals(token, "SET")) {
        while (!skip_sql_leading_noise(rest).empty()) {
            std::string_view assignment = pop_sql_set_assignment(rest);
            if (sql_set_assignment_targets_variable(assignment, "USERSTAT")) {
                return true;
            }
        }
        return false;
    }

    return sql_tokens_contain_user_statistics_information_schema_table(sql);
}

bool is_xml_sql_function_sql(std::string_view sql) {
    return sql_tokens_contain_xml_sql_function(sql);
}

bool is_oracle_sql_mode_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);
    if (!sql_token_equals(first, "SET")) {
        return false;
    }

    while (!skip_sql_leading_noise(rest).empty()) {
        if (sql_set_assignment_has_oracle_sql_mode(pop_sql_set_assignment(rest))) {
            return true;
        }
    }
    return false;
}

bool sql_set_assignment_has_oracle_sql_mode(std::string_view assignment) {
    if (!sql_set_assignment_targets_variable(assignment, "SQL_MODE")) {
        return false;
    }
    return sql_span_contains_token(assignment, "ORACLE");
}

std::string_view sql_set_assignment_policy_span(std::string_view sql) {
    std::string_view maybe_statement = sql;
    std::string_view token;
    if (pop_sql_scanned_token(maybe_statement, token) && sql_token_equals(token, "STATEMENT")) {
        return sql_set_statement_assignment_span(maybe_statement);
    }
    return sql;
}

std::string_view sql_set_statement_assignment_span(std::string_view sql) {
    const std::string_view assignments = sql;
    std::string_view scan = sql;
    unsigned paren_depth = 0;

    while (!scan.empty()) {
        if (scan.front() == '\'' || scan.front() == '"' || scan.front() == '`') {
            skip_sql_quoted_span(scan, scan.front());
            continue;
        }

        if (scan.size() >= 2U && scan[0] == '-' && scan[1] == '-') {
            const std::size_t newline = scan.find('\n');
            if (newline == std::string_view::npos) {
                break;
            }
            scan.remove_prefix(newline + 1U);
            continue;
        }

        if (scan.front() == '#') {
            const std::size_t newline = scan.find('\n');
            if (newline == std::string_view::npos) {
                break;
            }
            scan.remove_prefix(newline + 1U);
            continue;
        }

        if (scan.size() >= 2U && scan[0] == '/' && scan[1] == '*') {
            const std::size_t end = scan.find("*/");
            if (end == std::string_view::npos) {
                break;
            }
            scan.remove_prefix(end + 2U);
            continue;
        }

        if (scan.front() == '(') {
            ++paren_depth;
            scan.remove_prefix(1);
            continue;
        }

        if (scan.front() == ')' && paren_depth > 0U) {
            --paren_depth;
            scan.remove_prefix(1);
            continue;
        }

        if (paren_depth == 0U &&
            (std::isalnum(static_cast<unsigned char>(scan.front())) != 0 || scan.front() == '_')) {
            const std::size_t token_end = scan.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
            );
            const std::size_t token_size =
                token_end == std::string_view::npos ? scan.size() : token_end;
            const std::string_view token = scan.substr(0, token_size);
            if (sql_token_equals(token, "FOR")) {
                const std::size_t assignment_size =
                    static_cast<std::size_t>(scan.data() - assignments.data());
                return assignments.substr(0, assignment_size);
            }
            scan.remove_prefix(token_size);
            continue;
        }

        scan.remove_prefix(1);
    }

    return assignments;
}

bool sql_set_assignment_targets_query_cache_variable(std::string_view assignment) {
    constexpr const char *k_query_cache_variables[] = {
        "QUERY_CACHE_LIMIT",
        "QUERY_CACHE_MIN_RES_UNIT",
        "QUERY_CACHE_SIZE",
        "QUERY_CACHE_STRIP_COMMENTS",
        "QUERY_CACHE_TYPE",
        "QUERY_CACHE_WLOCK_INVALIDATE",
    };

    for (const char *variable : k_query_cache_variables) {
        std::string_view candidate = assignment;
        if (sql_set_assignment_targets_variable(candidate, variable)) {
            return true;
        }
    }

    return false;
}

bool sql_set_assignment_targets_statement_profiling_variable(std::string_view assignment) {
    std::string_view candidate = assignment;
    if (sql_set_assignment_targets_variable(candidate, "PROFILING")) {
        return true;
    }

    candidate = assignment;
    return sql_set_assignment_targets_variable(candidate, "PROFILING_HISTORY_SIZE");
}

bool sql_set_assignment_targets_optimizer_trace_variable(std::string_view assignment) {
    std::string_view candidate = assignment;
    if (sql_set_assignment_targets_variable(candidate, "OPTIMIZER_TRACE")) {
        return true;
    }

    candidate = assignment;
    return sql_set_assignment_targets_variable(candidate, "OPTIMIZER_TRACE_MAX_MEM_SIZE");
}

std::string_view pop_sql_set_assignment(std::string_view &sql) {
    sql = skip_sql_leading_noise(sql);
    if (!sql.empty() && sql.front() == ',') {
        sql.remove_prefix(1);
        sql = skip_sql_leading_noise(sql);
    }

    const std::string_view assignment = sql;
    std::string_view scan = sql;
    unsigned paren_depth = 0;
    while (!scan.empty()) {
        if (scan.front() == '\'' || scan.front() == '"' || scan.front() == '`') {
            skip_sql_quoted_span(scan, scan.front());
            continue;
        }

        if (scan.size() >= 2U && scan[0] == '-' && scan[1] == '-') {
            const std::size_t newline = scan.find('\n');
            if (newline == std::string_view::npos) {
                scan = {};
                break;
            }
            scan.remove_prefix(newline + 1U);
            continue;
        }

        if (scan.front() == '#') {
            const std::size_t newline = scan.find('\n');
            if (newline == std::string_view::npos) {
                scan = {};
                break;
            }
            scan.remove_prefix(newline + 1U);
            continue;
        }

        if (scan.size() >= 2U && scan[0] == '/' && scan[1] == '*') {
            const std::size_t end = scan.find("*/");
            if (end == std::string_view::npos) {
                scan = {};
                break;
            }
            scan.remove_prefix(end + 2U);
            continue;
        }

        if (scan.front() == '(') {
            ++paren_depth;
            scan.remove_prefix(1);
            continue;
        }

        if (scan.front() == ')' && paren_depth > 0U) {
            --paren_depth;
            scan.remove_prefix(1);
            continue;
        }

        if ((scan.front() == ',' && paren_depth == 0U) || scan.front() == ';') {
            const std::size_t assignment_size =
                static_cast<std::size_t>(scan.data() - assignment.data());
            sql = scan.front() == ',' ? scan.substr(1U) : std::string_view();
            return assignment.substr(0, assignment_size);
        }

        scan.remove_prefix(1);
    }

    sql = {};
    return assignment;
}

bool sql_set_assignment_targets_variable(std::string_view &assignment, const char *keyword) {
    assignment = skip_sql_leading_noise(assignment);
    if (assignment.empty()) {
        return false;
    }

    if (assignment.front() == '@') {
        assignment.remove_prefix(1);
        if (assignment.empty() || assignment.front() != '@') {
            return false;
        }
        assignment.remove_prefix(1);
    }

    std::string_view token = pop_sql_token_after_separators(assignment);
    if (sql_token_equals(token, "GLOBAL") || sql_token_equals(token, "LOCAL") ||
        sql_token_equals(token, "SESSION")) {
        token = pop_sql_token_after_separators(assignment);
    }

    return sql_token_equals(token, keyword);
}

bool sql_span_contains_token(std::string_view sql, const char *keyword) {
    for (;;) {
        sql = skip_sql_leading_noise(sql);
        if (sql.empty()) {
            return false;
        }

        if (sql.front() == '\'' || sql.front() == '"') {
            if (sql_quoted_span_contains_token(sql, sql.front(), keyword)) {
                return true;
            }
            continue;
        }

        if (sql.front() == '`') {
            skip_sql_quoted_span(sql, sql.front());
            continue;
        }

        if (std::isalnum(static_cast<unsigned char>(sql.front())) != 0 || sql.front() == '_') {
            const std::size_t token_end = sql.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
            );
            const std::string_view token =
                token_end == std::string_view::npos ? sql : sql.substr(0, token_end);
            if (sql_token_equals(token, keyword)) {
                return true;
            }
            if (token_end == std::string_view::npos) {
                return false;
            }
            sql.remove_prefix(token_end);
            continue;
        }

        sql.remove_prefix(1);
    }
}

bool sql_quoted_span_contains_token(std::string_view &sql, char quote, const char *keyword) {
    sql.remove_prefix(1);
    while (!sql.empty()) {
        if (std::isalnum(static_cast<unsigned char>(sql.front())) != 0 || sql.front() == '_') {
            const std::size_t token_end = sql.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
            );
            const std::string_view token =
                token_end == std::string_view::npos ? sql : sql.substr(0, token_end);
            if (sql_token_equals(token, keyword)) {
                return true;
            }
            if (token_end == std::string_view::npos) {
                sql = {};
                return false;
            }
            sql.remove_prefix(token_end);
            continue;
        }

        const char current = sql.front();
        sql.remove_prefix(1);

        if (current == '\\') {
            if (!sql.empty()) {
                sql.remove_prefix(1);
            }
            continue;
        }

        if (current == quote) {
            if (!sql.empty() && sql.front() == quote) {
                sql.remove_prefix(1);
                continue;
            }
            return false;
        }
    }
    return false;
}

bool is_non_table_object_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);
    std::string_view second = pop_sql_token(rest);
    const std::string_view third = pop_sql_token(rest);

    if (sql_token_equals(first, "CALL")) {
        return true;
    }

    if (sql_token_equals(first, "CREATE")) {
        if (sql_token_equals(second, "OR") && sql_token_equals(third, "REPLACE")) {
            second = pop_sql_token(rest);
        }
        return is_non_table_object_keyword(second) || sql_token_equals(second, "DEFINER") ||
               sql_token_equals(second, "ALGORITHM") || sql_token_equals(second, "SQL");
    }

    if (sql_token_equals(first, "ALTER") || sql_token_equals(first, "DROP")) {
        return is_non_table_object_keyword(second);
    }

    return sql_token_equals(first, "SHOW") && sql_token_equals(second, "CREATE") &&
           is_non_table_object_keyword(third);
}

bool is_non_table_object_keyword(std::string_view token) {
    return sql_token_equals(token, "VIEW") || sql_token_equals(token, "TRIGGER") ||
           sql_token_equals(token, "PROCEDURE") || sql_token_equals(token, "FUNCTION") ||
           sql_token_equals(token, "PACKAGE") || sql_token_equals(token, "SEQUENCE");
}

bool is_procedure_analyse_sql(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (!sql_token_equals(token, "PROCEDURE")) {
            continue;
        }

        std::string_view after_procedure = sql;
        std::string_view procedure_name;
        if (!pop_sql_scanned_token(after_procedure, procedure_name)) {
            return false;
        }

        if (sql_token_equals(procedure_name, "ANALYSE") &&
            sql_next_non_noise_is(after_procedure, '(')) {
            return true;
        }
    }
    return false;
}

bool is_select_procedure_sql(std::string_view sql) {
    std::string_view scan = sql;
    std::string_view token;
    if (!pop_sql_scanned_token(scan, token) || !sql_token_equals(token, "SELECT")) {
        return false;
    }

    while (pop_sql_scanned_token(scan, token)) {
        if (!sql_token_equals(token, "PROCEDURE")) {
            continue;
        }

        std::string_view after_procedure = scan;
        std::string_view procedure_name;
        if (!pop_sql_scanned_token(after_procedure, procedure_name)) {
            return false;
        }

        if (!procedure_name.empty() && sql_next_non_noise_is(after_procedure, '(')) {
            return true;
        }
    }
    return false;
}

TransactionControlKind direct_transaction_control_kind(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);
    std::string_view after_first = rest;
    const std::string_view second = pop_sql_token(rest);

    if (sql_token_equals(first, "BEGIN")) {
        if (second.empty()) {
            return sql_rest_is_statement_end(after_first) ? TransactionControlKind::Begin
                                                          : TransactionControlKind::Unsupported;
        }
        return sql_token_equals(second, "WORK") && sql_rest_is_statement_end(rest)
                   ? TransactionControlKind::Begin
                   : TransactionControlKind::Unsupported;
    }

    if (sql_token_equals(first, "COMMIT")) {
        return direct_completion_transaction_control_kind(
            after_first,
            TransactionControlKind::Commit,
            TransactionControlKind::CommitNoChain,
            TransactionControlKind::CommitAndChain
        );
    }

    if (sql_token_equals(first, "ROLLBACK")) {
        if (second.empty()) {
            return sql_rest_is_statement_end(after_first) ? TransactionControlKind::Rollback
                                                          : TransactionControlKind::Unsupported;
        }
        if (sql_token_equals(second, "TO")) {
            std::string_view savepoint_rest = rest;
            const std::string_view savepoint_keyword = pop_sql_token(savepoint_rest);
            if (!sql_token_equals(savepoint_keyword, "SAVEPOINT")) {
                savepoint_rest = rest;
            }
            return pop_sql_savepoint_name(savepoint_rest, nullptr) &&
                           sql_rest_is_statement_end(savepoint_rest)
                       ? TransactionControlKind::RollbackToSavepoint
                       : TransactionControlKind::Unsupported;
        }
        return direct_completion_transaction_control_kind(
            after_first,
            TransactionControlKind::Rollback,
            TransactionControlKind::RollbackNoChain,
            TransactionControlKind::RollbackAndChain
        );
    }

    if (sql_token_equals(first, "SAVEPOINT")) {
        std::string_view savepoint_rest = after_first;
        return pop_sql_savepoint_name(savepoint_rest, nullptr) &&
                       sql_rest_is_statement_end(savepoint_rest)
                   ? TransactionControlKind::Savepoint
                   : TransactionControlKind::Unsupported;
    }

    if (sql_token_equals(first, "XA")) {
        return TransactionControlKind::Unsupported;
    }

    if (sql_token_equals(first, "START")) {
        if (!sql_token_equals(second, "TRANSACTION")) {
            return TransactionControlKind::None;
        }
        return direct_start_transaction_control_kind(rest);
    }

    if (sql_token_equals(first, "RELEASE")) {
        if (!sql_token_equals(second, "SAVEPOINT")) {
            return TransactionControlKind::None;
        }
        return pop_sql_savepoint_name(rest, nullptr) && sql_rest_is_statement_end(rest)
                   ? TransactionControlKind::ReleaseSavepoint
                   : TransactionControlKind::Unsupported;
    }

    if (sql_token_equals(first, "SET")) {
        return direct_set_transaction_control_kind(after_first);
    }

    return TransactionControlKind::None;
}

TransactionControlKind direct_start_transaction_control_kind(std::string_view sql) {
    if (sql_rest_is_statement_end(sql)) {
        return TransactionControlKind::Begin;
    }

    bool read_only = false;
    bool read_write = false;
    for (;;) {
        const std::string_view read_token = pop_sql_token(sql);
        if (!sql_token_equals(read_token, "READ")) {
            return TransactionControlKind::Unsupported;
        }
        const std::string_view mode_token = pop_sql_token(sql);
        if (sql_token_equals(mode_token, "ONLY")) {
            read_only = true;
        } else if (sql_token_equals(mode_token, "WRITE")) {
            read_write = true;
        } else {
            return TransactionControlKind::Unsupported;
        }
        if (read_only && read_write) {
            return TransactionControlKind::Unsupported;
        }

        sql = skip_sql_leading_noise(sql);
        if (sql_rest_is_statement_end(sql)) {
            return read_only ? TransactionControlKind::BeginReadOnly
                             : TransactionControlKind::BeginReadWrite;
        }
        if (sql.empty() || sql.front() != ',') {
            return TransactionControlKind::Unsupported;
        }
        sql.remove_prefix(1);
    }
}

TransactionControlKind direct_completion_transaction_control_kind(
    std::string_view sql,
    TransactionControlKind plain_kind,
    TransactionControlKind no_chain_kind,
    TransactionControlKind chained_kind
) {
    if (sql_rest_is_statement_end(sql)) {
        return plain_kind;
    }

    std::string_view rest = sql;
    std::string_view token = pop_sql_token(rest);
    if (sql_token_equals(token, "WORK")) {
        sql = rest;
        if (sql_rest_is_statement_end(sql)) {
            return plain_kind;
        }
        token = pop_sql_token(sql);
    } else {
        sql = rest;
    }

    bool chain = false;
    if (sql_token_equals(token, "AND")) {
        token = pop_sql_token(sql);
        if (sql_token_equals(token, "CHAIN")) {
            chain = true;
        } else if (sql_token_equals(token, "NO")) {
            token = pop_sql_token(sql);
            if (!sql_token_equals(token, "CHAIN")) {
                return TransactionControlKind::Unsupported;
            }
            plain_kind = no_chain_kind;
        } else {
            return TransactionControlKind::Unsupported;
        }

        if (sql_rest_is_statement_end(sql)) {
            return chain ? chained_kind : plain_kind;
        }
        token = pop_sql_token(sql);
    }

    if (sql_token_equals(token, "RELEASE")) {
        return TransactionControlKind::Unsupported;
    }
    if (!sql_token_equals(token, "NO")) {
        return TransactionControlKind::Unsupported;
    }
    token = pop_sql_token(sql);
    if (!sql_token_equals(token, "RELEASE")) {
        return TransactionControlKind::Unsupported;
    }
    return sql_rest_is_statement_end(sql) ? (chain ? chained_kind : plain_kind)
                                          : TransactionControlKind::Unsupported;
}

#  if MYLITE_MARIADB_HAS_MYLITE_SE
bool direct_savepoint_control_name(
    std::string_view sql,
    TransactionControlKind kind,
    std::string &out_name
) {
    out_name.clear();
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);

    if (kind == TransactionControlKind::Savepoint && sql_token_equals(first, "SAVEPOINT")) {
        return pop_sql_savepoint_name(rest, &out_name) && sql_rest_is_statement_end(rest);
    }

    if (kind == TransactionControlKind::RollbackToSavepoint &&
        sql_token_equals(first, "ROLLBACK")) {
        if (!sql_token_equals(pop_sql_token(rest), "TO")) {
            return false;
        }
        std::string_view savepoint_rest = rest;
        const std::string_view savepoint_keyword = pop_sql_token(savepoint_rest);
        if (!sql_token_equals(savepoint_keyword, "SAVEPOINT")) {
            savepoint_rest = rest;
        }
        return pop_sql_savepoint_name(savepoint_rest, &out_name) &&
               sql_rest_is_statement_end(savepoint_rest);
    }

    if (kind == TransactionControlKind::ReleaseSavepoint && sql_token_equals(first, "RELEASE")) {
        if (!sql_token_equals(pop_sql_token(rest), "SAVEPOINT")) {
            return false;
        }
        return pop_sql_savepoint_name(rest, &out_name) && sql_rest_is_statement_end(rest);
    }

    return false;
}
#  endif

bool pop_sql_savepoint_name(std::string_view &sql, std::string *out_name) {
    sql = skip_sql_leading_noise(sql);
    if (sql.empty()) {
        return false;
    }

    if (out_name != nullptr) {
        out_name->clear();
    }

    if (sql.front() != '`') {
        const std::string_view token = pop_sql_token(sql);
        if (token.empty()) {
            return false;
        }
        if (out_name != nullptr) {
            out_name->assign(token);
        }
        return true;
    }

    sql.remove_prefix(1);
    bool has_character = false;
    while (!sql.empty()) {
        const char current = sql.front();
        sql.remove_prefix(1);
        if (current == '`') {
            if (!sql.empty() && sql.front() == '`') {
                sql.remove_prefix(1);
                has_character = true;
                if (out_name != nullptr) {
                    out_name->push_back('`');
                }
                continue;
            }
            return has_character;
        }

        has_character = true;
        if (out_name != nullptr) {
            out_name->push_back(current);
        }
    }
    return false;
}

TransactionControlKind direct_set_transaction_control_kind(std::string_view sql) {
    std::string_view statement_check = skip_sql_leading_noise(sql);
    if (sql_token_equals(pop_sql_token(statement_check), "STATEMENT")) {
        std::string_view assignments = sql_set_statement_assignment_span(statement_check);
        while (!skip_sql_leading_noise(assignments).empty()) {
            if (sql_set_assignment_targets_transaction_variable(
                    pop_sql_set_assignment(assignments)
                )) {
                return TransactionControlKind::Unsupported;
            }
        }
        return TransactionControlKind::None;
    }

    const bool has_statement_tail = sql_has_statement_tail_after_semicolon(sql);
    const TransactionControlKind characteristics_control =
        direct_set_transaction_characteristics_control_kind(sql, has_statement_tail);
    if (characteristics_control != TransactionControlKind::None) {
        return characteristics_control;
    }

    return direct_set_assignment_transaction_control_kind(sql);
}

TransactionControlKind direct_set_transaction_characteristics_control_kind(
    std::string_view sql,
    bool has_statement_tail
) {
    std::string_view rest = skip_sql_leading_noise(sql);
    std::string_view token = pop_sql_token(rest);
    bool global = false;
    bool session_scope = false;

    if (sql_token_equals(token, "GLOBAL") || sql_token_equals(token, "LOCAL") ||
        sql_token_equals(token, "SESSION")) {
        global = sql_token_equals(token, "GLOBAL");
        session_scope = !global;
        token = pop_sql_token(rest);
    }

    if (!sql_token_equals(token, "TRANSACTION")) {
        return TransactionControlKind::None;
    }
    if (global || has_statement_tail) {
        return TransactionControlKind::Unsupported;
    }

    bool saw_access_mode = false;
    bool saw_isolation_level = false;
    bool read_only = false;
    for (;;) {
        std::string_view candidate = rest;
        bool candidate_read_only = false;
        if (!saw_access_mode && pop_transaction_access_mode(candidate, &candidate_read_only)) {
            saw_access_mode = true;
            read_only = candidate_read_only;
            rest = candidate;
        } else if (!saw_isolation_level && pop_transaction_isolation_level(candidate)) {
            saw_isolation_level = true;
            rest = candidate;
        } else {
            return TransactionControlKind::Unsupported;
        }

        rest = skip_sql_leading_noise(rest);
        if (sql_rest_is_statement_end(rest)) {
            break;
        }
        if (rest.empty() || rest.front() != ',') {
            return TransactionControlKind::Unsupported;
        }
        rest.remove_prefix(1);
    }

    return saw_access_mode ? transaction_access_mode_control_kind(session_scope, read_only)
                           : transaction_isolation_control_kind(session_scope);
}

TransactionControlKind direct_set_assignment_transaction_control_kind(std::string_view sql) {
    const bool has_statement_tail = sql_has_statement_tail_after_semicolon(sql);
    TransactionControlKind result = TransactionControlKind::None;
    bool completion_type_seen = false;

    while (!skip_sql_leading_noise(sql).empty()) {
        std::string_view assignment = pop_sql_set_assignment(sql);
        if (skip_sql_leading_noise(assignment).empty()) {
            return result;
        }

        const TransactionControlKind assignment_result =
            autocommit_assignment_control_kind(assignment);
        if (assignment_result == TransactionControlKind::Unsupported) {
            return TransactionControlKind::Unsupported;
        }
        if (assignment_result != TransactionControlKind::None) {
            if (result == TransactionControlKind::SetAutocommitOff ||
                result == TransactionControlKind::SetAutocommitOn) {
                return TransactionControlKind::Unsupported;
            }
            result = assignment_result;
            continue;
        }

        const TransactionControlKind completion_type_result =
            completion_type_assignment_control_kind(assignment);
        if (completion_type_result == TransactionControlKind::Unsupported) {
            return TransactionControlKind::Unsupported;
        }
        if (completion_type_result == TransactionControlKind::SetCompletionTypeNoChain ||
            completion_type_result == TransactionControlKind::SetCompletionTypeChain) {
            if (completion_type_seen) {
                return TransactionControlKind::Unsupported;
            }
            completion_type_seen = true;
            if (result == TransactionControlKind::None) {
                result = completion_type_result;
            }
            continue;
        }

        if (sql_set_assignment_targets_transaction_variable(assignment)) {
            return TransactionControlKind::Unsupported;
        }
    }

    if (result == TransactionControlKind::None) {
        return TransactionControlKind::None;
    }
    if (has_statement_tail) {
        return TransactionControlKind::Unsupported;
    }
    return result;
}

TransactionControlKind autocommit_assignment_control_kind(std::string_view assignment) {
    assignment = skip_sql_leading_noise(assignment);
    if (assignment.empty()) {
        return TransactionControlKind::None;
    }

    bool system_variable = false;
    if (assignment.front() == '@') {
        assignment.remove_prefix(1);
        if (assignment.empty() || assignment.front() != '@') {
            return TransactionControlKind::None;
        }
        assignment.remove_prefix(1);
        system_variable = true;
    }

    std::string_view token = pop_sql_token_after_separators(assignment);
    if (sql_token_equals(token, "GLOBAL")) {
        std::string_view next = pop_sql_token_after_separators(assignment);
        return sql_token_equals(next, "AUTOCOMMIT") ? TransactionControlKind::Unsupported
                                                    : TransactionControlKind::None;
    }
    if (sql_token_equals(token, "LOCAL") || sql_token_equals(token, "SESSION")) {
        token = pop_sql_token_after_separators(assignment);
    } else if (system_variable && !sql_token_equals(token, "AUTOCOMMIT")) {
        return TransactionControlKind::None;
    }

    if (!sql_token_equals(token, "AUTOCOMMIT")) {
        return TransactionControlKind::None;
    }

    assignment = skip_sql_leading_noise(assignment);
    if (assignment.empty() || assignment.front() != '=') {
        return TransactionControlKind::Unsupported;
    }
    assignment.remove_prefix(1);
    assignment = skip_sql_leading_noise(assignment);

    const std::string_view value = pop_sql_token(assignment);
    if (value.empty() || !sql_rest_is_statement_end(assignment)) {
        return TransactionControlKind::Unsupported;
    }
    if (sql_token_is_autocommit_off(value)) {
        return TransactionControlKind::SetAutocommitOff;
    }
    if (sql_token_is_autocommit_on(value)) {
        return TransactionControlKind::SetAutocommitOn;
    }
    if (sql_token_equals(value, "DEFAULT")) {
        return TransactionControlKind::SetAutocommitOn;
    }
    return TransactionControlKind::Unsupported;
}

TransactionControlKind completion_type_assignment_control_kind(std::string_view assignment) {
    assignment = skip_sql_leading_noise(assignment);
    if (assignment.empty()) {
        return TransactionControlKind::None;
    }

    bool system_variable = false;
    if (assignment.front() == '@') {
        assignment.remove_prefix(1);
        if (assignment.empty() || assignment.front() != '@') {
            return TransactionControlKind::None;
        }
        assignment.remove_prefix(1);
        system_variable = true;
    }

    std::string_view token = pop_sql_token_after_separators(assignment);
    if (sql_token_equals(token, "GLOBAL")) {
        std::string_view next = pop_sql_token_after_separators(assignment);
        return sql_token_equals(next, "COMPLETION_TYPE") ? TransactionControlKind::Unsupported
                                                         : TransactionControlKind::None;
    }
    if (sql_token_equals(token, "LOCAL") || sql_token_equals(token, "SESSION")) {
        token = pop_sql_token_after_separators(assignment);
    } else if (system_variable && !sql_token_equals(token, "COMPLETION_TYPE")) {
        return TransactionControlKind::None;
    }

    if (!sql_token_equals(token, "COMPLETION_TYPE")) {
        return TransactionControlKind::None;
    }

    assignment = skip_sql_leading_noise(assignment);
    if (assignment.empty() || assignment.front() != '=') {
        return TransactionControlKind::Unsupported;
    }
    assignment.remove_prefix(1);
    assignment = skip_sql_leading_noise(assignment);

    const std::string_view value = pop_sql_token(assignment);
    if (value.empty() || !sql_rest_is_statement_end(assignment)) {
        return TransactionControlKind::Unsupported;
    }
    if (sql_token_is_completion_type_no_chain(value)) {
        return TransactionControlKind::SetCompletionTypeNoChain;
    }
    if (sql_token_is_completion_type_chain(value)) {
        return TransactionControlKind::SetCompletionTypeChain;
    }
    return TransactionControlKind::Unsupported;
}

bool direct_set_completion_type_chain_default(std::string_view sql, bool *out_chain) {
    std::string_view rest = skip_sql_leading_noise(sql);
    if (!sql_token_equals(pop_sql_token(rest), "SET")) {
        return false;
    }

    std::string_view statement_check = skip_sql_leading_noise(rest);
    if (sql_token_equals(pop_sql_token(statement_check), "STATEMENT")) {
        return false;
    }

    while (!skip_sql_leading_noise(rest).empty()) {
        const TransactionControlKind completion_type_result =
            completion_type_assignment_control_kind(pop_sql_set_assignment(rest));
        if (completion_type_result == TransactionControlKind::SetCompletionTypeNoChain) {
            *out_chain = false;
            return true;
        }
        if (completion_type_result == TransactionControlKind::SetCompletionTypeChain) {
            *out_chain = true;
            return true;
        }
    }
    return false;
}

bool sql_token_is_completion_type_no_chain(std::string_view token) {
    return token == "0" || sql_token_equals(token, "NO_CHAIN") ||
           sql_token_equals(token, "DEFAULT");
}

bool sql_token_is_completion_type_chain(std::string_view token) {
    return token == "1" || sql_token_equals(token, "CHAIN");
}

bool sql_set_assignment_targets_transaction_variable(std::string_view assignment) {
    constexpr const char *k_transaction_variables[] = {
        "AUTOCOMMIT",
        "COMPLETION_TYPE",
        "TRANSACTION_ISOLATION",
        "TRANSACTION_READ_ONLY",
        "TX_ISOLATION",
        "TX_READ_ONLY",
    };

    for (const char *variable : k_transaction_variables) {
        std::string_view candidate = assignment;
        if (sql_set_assignment_targets_variable(candidate, variable)) {
            return true;
        }
    }
    return false;
}

bool pop_transaction_access_mode(std::string_view &sql, bool *out_read_only) {
    std::string_view candidate = skip_sql_leading_noise(sql);
    if (!sql_token_equals(pop_sql_token(candidate), "READ")) {
        return false;
    }
    const std::string_view mode = pop_sql_token(candidate);
    if (sql_token_equals(mode, "ONLY")) {
        *out_read_only = true;
    } else if (sql_token_equals(mode, "WRITE")) {
        *out_read_only = false;
    } else {
        return false;
    }
    sql = candidate;
    return true;
}

bool pop_transaction_isolation_level(std::string_view &sql) {
    std::string_view candidate = skip_sql_leading_noise(sql);
    if (!sql_token_equals(pop_sql_token(candidate), "ISOLATION")) {
        return false;
    }
    if (!sql_token_equals(pop_sql_token(candidate), "LEVEL")) {
        return false;
    }

    const std::string_view first = pop_sql_token(candidate);
    if (sql_token_equals(first, "READ")) {
        const std::string_view second = pop_sql_token(candidate);
        if (!sql_token_equals(second, "UNCOMMITTED") && !sql_token_equals(second, "COMMITTED")) {
            return false;
        }
        sql = candidate;
        return true;
    }
    if (sql_token_equals(first, "REPEATABLE")) {
        if (!sql_token_equals(pop_sql_token(candidate), "READ")) {
            return false;
        }
        sql = candidate;
        return true;
    }
    if (sql_token_equals(first, "SERIALIZABLE")) {
        sql = candidate;
        return true;
    }
    return false;
}

TransactionControlKind transaction_access_mode_control_kind(bool session_scope, bool read_only) {
    if (read_only) {
        return session_scope ? TransactionControlKind::SetSessionTransactionReadOnly
                             : TransactionControlKind::SetNextTransactionReadOnly;
    }
    return session_scope ? TransactionControlKind::SetSessionTransactionReadWrite
                         : TransactionControlKind::SetNextTransactionReadWrite;
}

TransactionControlKind transaction_isolation_control_kind(bool session_scope) {
    return session_scope ? TransactionControlKind::SetSessionTransactionIsolation
                         : TransactionControlKind::SetNextTransactionIsolation;
}

bool is_begin_transaction_control(TransactionControlKind kind) {
    return kind == TransactionControlKind::Begin || kind == TransactionControlKind::BeginReadOnly ||
           kind == TransactionControlKind::BeginReadWrite;
}

bool is_completion_transaction_control(TransactionControlKind kind) {
    return kind == TransactionControlKind::Commit ||
           kind == TransactionControlKind::CommitNoChain ||
           kind == TransactionControlKind::Rollback ||
           kind == TransactionControlKind::RollbackNoChain;
}

bool is_savepoint_transaction_control(TransactionControlKind kind) {
    return kind == TransactionControlKind::Savepoint ||
           kind == TransactionControlKind::RollbackToSavepoint ||
           kind == TransactionControlKind::ReleaseSavepoint;
}

bool is_chained_completion_transaction_control(TransactionControlKind kind) {
    return kind == TransactionControlKind::CommitAndChain ||
           kind == TransactionControlKind::RollbackAndChain;
}

bool transaction_start_read_only(const mylite_db &database, TransactionControlKind kind) {
    if (kind == TransactionControlKind::BeginReadOnly) {
        return true;
    }
    if (kind == TransactionControlKind::BeginReadWrite) {
        return false;
    }
    return database.transaction_read_only_next_set ? database.transaction_read_only_next
                                                   : database.transaction_read_only_default;
}

void mark_transaction_started(mylite_db &database, bool read_only) {
    database.transaction_active = true;
    database.transaction_read_only = read_only;
    database.transaction_read_only_next = false;
    database.transaction_read_only_next_set = false;
}

void mark_transaction_completed(mylite_db &database) {
    database.transaction_active = false;
    database.transaction_read_only = database.transaction_read_only_default;
    database.transaction_read_only_next = false;
    database.transaction_read_only_next_set = false;
}

void apply_transaction_characteristics_control(mylite_db &database, TransactionControlKind kind) {
    if (kind == TransactionControlKind::SetNextTransactionReadOnly ||
        kind == TransactionControlKind::SetNextTransactionReadWrite) {
        const bool read_only = kind == TransactionControlKind::SetNextTransactionReadOnly;
        if (database.transaction_active) {
            database.transaction_read_only = read_only;
            return;
        }
        database.transaction_read_only_next = read_only;
        database.transaction_read_only_next_set = true;
        return;
    }

    if (kind == TransactionControlKind::SetSessionTransactionReadOnly ||
        kind == TransactionControlKind::SetSessionTransactionReadWrite) {
        database.transaction_read_only_default =
            kind == TransactionControlKind::SetSessionTransactionReadOnly;
        database.transaction_read_only_next = false;
        database.transaction_read_only_next_set = false;
        if (!database.transaction_active) {
            database.transaction_read_only = database.transaction_read_only_default;
        }
    }
}

bool sql_token_is_autocommit_off(std::string_view token) {
    return sql_token_equals(token, "0") || sql_token_equals(token, "OFF") ||
           sql_token_equals(token, "FALSE");
}

bool sql_token_is_autocommit_on(std::string_view token) {
    return sql_token_equals(token, "1") || sql_token_equals(token, "ON") ||
           sql_token_equals(token, "TRUE");
}

bool sql_has_statement_tail_after_semicolon(std::string_view sql) {
    while (!sql.empty()) {
        if (sql.front() == '\'' || sql.front() == '"' || sql.front() == '`') {
            skip_sql_quoted_span(sql, sql.front());
            continue;
        }

        if (sql.size() >= 2U && sql[0] == '-' && sql[1] == '-') {
            const std::size_t newline = sql.find('\n');
            if (newline == std::string_view::npos) {
                return false;
            }
            sql.remove_prefix(newline + 1U);
            continue;
        }

        if (sql.front() == '#') {
            const std::size_t newline = sql.find('\n');
            if (newline == std::string_view::npos) {
                return false;
            }
            sql.remove_prefix(newline + 1U);
            continue;
        }

        if (sql.size() >= 2U && sql[0] == '/' && sql[1] == '*') {
            const std::size_t end = sql.find("*/");
            if (end == std::string_view::npos) {
                return false;
            }
            sql.remove_prefix(end + 2U);
            continue;
        }

        if (sql.front() == ';') {
            sql.remove_prefix(1);
            return !skip_sql_leading_noise(sql).empty();
        }
        sql.remove_prefix(1);
    }
    return false;
}

bool sql_rest_is_statement_end(std::string_view sql) {
    sql = skip_sql_leading_noise(sql);
    if (sql.empty()) {
        return true;
    }
    if (sql.front() != ';') {
        return false;
    }

    sql.remove_prefix(1);
    return skip_sql_leading_noise(sql).empty();
}

bool sql_tokens_contain_file_import_function(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (sql_token_equals(token, "LOAD_FILE") && sql_next_non_noise_is(sql, '(')) {
            return true;
        }
    }
    return false;
}

bool sql_tokens_contain_file_export_marker(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (sql_token_equals(token, "INTO") &&
            (sql_starts_with_token(sql, "OUTFILE") || sql_starts_with_token(sql, "DUMPFILE"))) {
            return true;
        }
    }
    return false;
}

bool sql_tokens_contain_server_utility_function(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (!sql_next_non_noise_is(sql, '(')) {
            continue;
        }

        if (sql_token_equals(token, "BENCHMARK") || sql_token_equals(token, "SLEEP") ||
            sql_token_equals(token, "UUID_SHORT") || sql_token_equals(token, "MASTER_POS_WAIT") ||
            sql_token_equals(token, "MASTER_GTID_WAIT")) {
            return true;
        }
    }
    return false;
}

bool sql_tokens_contain_gis_sql_function(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (!sql_next_non_noise_is(sql, '(')) {
            continue;
        }

        if (sql_token_is_gis_sql_function(token)) {
            return true;
        }
    }
    return false;
}

bool sql_tokens_contain_sformat_sql_function(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (!sql_next_non_noise_is(sql, '(')) {
            continue;
        }

        if (sql_token_equals(token, "SFORMAT")) {
            return true;
        }
    }
    return false;
}

bool sql_tokens_contain_json_schema_valid_function(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (!sql_next_non_noise_is(sql, '(')) {
            continue;
        }

        if (sql_token_equals(token, "JSON_SCHEMA_VALID")) {
            return true;
        }
    }
    return false;
}

bool sql_tokens_contain_json_table_function(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (!sql_next_non_noise_is(sql, '(')) {
            continue;
        }

        if (sql_token_equals(token, "JSON_TABLE")) {
            return true;
        }
    }
    return false;
}

bool sql_tokens_contain_dynamic_column_function(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (!sql_next_non_noise_is(sql, '(')) {
            continue;
        }

        if (sql_token_is_dynamic_column_function(token)) {
            return true;
        }
    }
    return false;
}

bool sql_token_is_dynamic_column_function(std::string_view token) {
    return sql_token_equals(token, "COLUMN_ADD") || sql_token_equals(token, "COLUMN_CHECK") ||
           sql_token_equals(token, "COLUMN_CREATE") || sql_token_equals(token, "COLUMN_DELETE") ||
           sql_token_equals(token, "COLUMN_EXISTS") || sql_token_equals(token, "COLUMN_GET") ||
           sql_token_equals(token, "COLUMN_JSON") || sql_token_equals(token, "COLUMN_LIST");
}

bool sql_tokens_contain_sequence_value_surface(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (sql_token_equals(token, "NEXT") || sql_token_equals(token, "PREVIOUS")) {
            std::string_view after_direction = sql;
            std::string_view value_token;
            std::string_view for_token;
            if (pop_sql_scanned_token(after_direction, value_token) &&
                sql_token_equals(value_token, "VALUE") &&
                pop_sql_scanned_token(after_direction, for_token) &&
                sql_token_equals(for_token, "FOR")) {
                return true;
            }
        }

        if (!sql_next_non_noise_is(sql, '(')) {
            continue;
        }

        if (sql_token_equals(token, "NEXTVAL") || sql_token_equals(token, "LASTVAL") ||
            sql_token_equals(token, "SETVAL")) {
            return true;
        }
    }
    return false;
}

bool sql_token_is_user_statistics_table(std::string_view token) {
    return sql_token_equals(token, "CLIENT_STATISTICS") ||
           sql_token_equals(token, "INDEX_STATISTICS") ||
           sql_token_equals(token, "TABLE_STATISTICS") ||
           sql_token_equals(token, "USER_STATISTICS");
}

bool sql_tokens_contain_user_statistics_information_schema_table(std::string_view sql) {
    std::string_view token;
    while (!sql.empty()) {
        if (!pop_sql_identifier_token(sql, token)) {
            if (!sql.empty()) {
                sql.remove_prefix(1);
            }
            continue;
        }

        if (!sql_token_equals(token, "INFORMATION_SCHEMA")) {
            continue;
        }

        std::string_view after_schema = sql;
        if (!consume_sql_reference_dot(after_schema)) {
            continue;
        }
        if (pop_sql_identifier_token(after_schema, token) &&
            sql_token_is_user_statistics_table(token)) {
            return true;
        }
    }
    return false;
}

bool sql_tokens_contain_statement_profiling_information_schema_table(std::string_view sql) {
    std::string_view token;
    while (!sql.empty()) {
        if (!pop_sql_identifier_token(sql, token)) {
            if (!sql.empty()) {
                sql.remove_prefix(1);
            }
            continue;
        }

        if (!sql_token_equals(token, "INFORMATION_SCHEMA")) {
            continue;
        }

        std::string_view after_schema = sql;
        if (!consume_sql_reference_dot(after_schema)) {
            continue;
        }
        if (pop_sql_identifier_token(after_schema, token) && sql_token_equals(token, "PROFILING")) {
            return true;
        }
    }
    return false;
}

bool sql_tokens_contain_optimizer_trace_information_schema_table(std::string_view sql) {
    std::string_view token;
    while (!sql.empty()) {
        if (!pop_sql_identifier_token(sql, token)) {
            if (!sql.empty()) {
                sql.remove_prefix(1);
            }
            continue;
        }

        if (!sql_token_equals(token, "INFORMATION_SCHEMA")) {
            continue;
        }

        std::string_view after_schema = sql;
        if (!consume_sql_reference_dot(after_schema)) {
            continue;
        }
        if (pop_sql_identifier_token(after_schema, token) &&
            sql_token_equals(token, "OPTIMIZER_TRACE")) {
            return true;
        }
    }
    return false;
}

bool pop_sql_identifier_token(std::string_view &sql, std::string_view &out_token) {
    for (;;) {
        sql = skip_sql_leading_noise(sql);
        if (sql.empty()) {
            out_token = {};
            return false;
        }

        if (sql.front() == '\'' || sql.front() == '"') {
            skip_sql_quoted_span(sql, sql.front());
            continue;
        }

        if (sql.front() == '`') {
            sql.remove_prefix(1);
            const std::size_t token_end = sql.find('`');
            if (token_end == std::string_view::npos) {
                sql = {};
                out_token = {};
                return false;
            }
            out_token = sql.substr(0, token_end);
            sql.remove_prefix(token_end + 1U);
            return true;
        }

        if (std::isalnum(static_cast<unsigned char>(sql.front())) == 0 && sql.front() != '_') {
            out_token = {};
            return false;
        }

        const std::size_t token_end = sql.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
        );
        if (token_end == std::string_view::npos) {
            out_token = sql;
            sql = {};
            return true;
        }

        out_token = sql.substr(0, token_end);
        sql.remove_prefix(token_end);
        return true;
    }
}

bool consume_sql_reference_dot(std::string_view &sql) {
    sql = skip_sql_leading_noise(sql);
    if (sql.empty() || sql.front() != '.') {
        return false;
    }
    sql.remove_prefix(1);
    return true;
}

bool sql_tokens_contain_xml_sql_function(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (!sql_next_non_noise_is(sql, '(')) {
            continue;
        }

        if (sql_token_equals(token, "EXTRACTVALUE") || sql_token_equals(token, "UPDATEXML")) {
            return true;
        }
    }
    return false;
}

bool sql_token_is_gis_sql_function(std::string_view token) {
    constexpr const char *k_gis_sql_function_names[] = {
        "AREA",
        "ASBINARY",
        "ASTEXT",
        "ASWKB",
        "ASWKT",
        "BOUNDARY",
        "BUFFER",
        "CENTROID",
        "CONTAINS",
        "CONVEXHULL",
        "CROSSES",
        "DIMENSION",
        "DISJOINT",
        "ENDPOINT",
        "ENVELOPE",
        "EQUALS",
        "EXTERIORRING",
        "GEOMCOLLFROMTEXT",
        "GEOMCOLLFROMWKB",
        "GEOMETRY",
        "GEOMETRYCOLLECTION",
        "GEOMETRYCOLLECTIONFROMTEXT",
        "GEOMETRYCOLLECTIONFROMWKB",
        "GEOMETRYFROMTEXT",
        "GEOMETRYFROMWKB",
        "GEOMETRYN",
        "GEOMETRYTYPE",
        "GEOMFROMTEXT",
        "GEOMFROMWKB",
        "GLENGTH",
        "INTERIORRINGN",
        "INTERSECTS",
        "ISCLOSED",
        "ISEMPTY",
        "ISRING",
        "ISSIMPLE",
        "LINEFROMTEXT",
        "LINEFROMWKB",
        "LINESTRING",
        "LINESTRINGFROMTEXT",
        "LINESTRINGFROMWKB",
        "MBRCONTAINS",
        "MBRDISJOINT",
        "MBREQUAL",
        "MBREQUALS",
        "MBRINTERSECTS",
        "MBROVERLAPS",
        "MBRTOUCHES",
        "MBRWITHIN",
        "MLINEFROMTEXT",
        "MLINEFROMWKB",
        "MPOINTFROMTEXT",
        "MPOINTFROMWKB",
        "MPOLYFROMTEXT",
        "MPOLYFROMWKB",
        "MULTILINESTRING",
        "MULTILINESTRINGFROMTEXT",
        "MULTILINESTRINGFROMWKB",
        "MULTIPOINT",
        "MULTIPOINTFROMTEXT",
        "MULTIPOINTFROMWKB",
        "MULTIPOLYGON",
        "MULTIPOLYGONFROMTEXT",
        "MULTIPOLYGONFROMWKB",
        "NUMGEOMETRIES",
        "NUMINTERIORRINGS",
        "NUMPOINTS",
        "OVERLAPS",
        "POINT",
        "POINTFROMTEXT",
        "POINTFROMWKB",
        "POINTN",
        "POINTONSURFACE",
        "POLYFROMTEXT",
        "POLYFROMWKB",
        "POLYGON",
        "POLYGONFROMTEXT",
        "POLYGONFROMWKB",
        "SRID",
        "ST_AREA",
        "STARTPOINT",
        "ST_ASBINARY",
        "ST_ASGEOJSON",
        "ST_ASTEXT",
        "ST_ASWKB",
        "ST_ASWKT",
        "ST_BOUNDARY",
        "ST_BUFFER",
        "ST_CENTROID",
        "ST_CONTAINS",
        "ST_CONVEXHULL",
        "ST_CROSSES",
        "ST_DIFFERENCE",
        "ST_DIMENSION",
        "ST_DISJOINT",
        "ST_DISTANCE",
        "ST_DISTANCE_SPHERE",
        "ST_ENDPOINT",
        "ST_ENVELOPE",
        "ST_EQUALS",
        "ST_EXTERIORRING",
        "ST_GEOMCOLLFROMTEXT",
        "ST_GEOMCOLLFROMWKB",
        "ST_GEOMETRYCOLLECTIONFROMTEXT",
        "ST_GEOMETRYCOLLECTIONFROMWKB",
        "ST_GEOMETRYFROMTEXT",
        "ST_GEOMETRYFROMWKB",
        "ST_GEOMETRYN",
        "ST_GEOMETRYTYPE",
        "ST_GEOMFROMGEOJSON",
        "ST_GEOMFROMTEXT",
        "ST_GEOMFROMWKB",
        "ST_GIS_DEBUG",
        "ST_INTERIORRINGN",
        "ST_INTERSECTION",
        "ST_INTERSECTS",
        "ST_ISCLOSED",
        "ST_ISEMPTY",
        "ST_ISRING",
        "ST_ISSIMPLE",
        "ST_LENGTH",
        "ST_LINEFROMTEXT",
        "ST_LINEFROMWKB",
        "ST_LINESTRINGFROMTEXT",
        "ST_LINESTRINGFROMWKB",
        "ST_MLINEFROMTEXT",
        "ST_MLINEFROMWKB",
        "ST_MPOINTFROMTEXT",
        "ST_MPOINTFROMWKB",
        "ST_MPOLYFROMTEXT",
        "ST_MPOLYFROMWKB",
        "ST_MULTILINESTRINGFROMTEXT",
        "ST_MULTILINESTRINGFROMWKB",
        "ST_MULTIPOINTFROMTEXT",
        "ST_MULTIPOINTFROMWKB",
        "ST_MULTIPOLYGONFROMTEXT",
        "ST_MULTIPOLYGONFROMWKB",
        "ST_NUMGEOMETRIES",
        "ST_NUMINTERIORRINGS",
        "ST_NUMPOINTS",
        "ST_OVERLAPS",
        "ST_POINTFROMTEXT",
        "ST_POINTFROMWKB",
        "ST_POINTN",
        "ST_POINTONSURFACE",
        "ST_POLYFROMTEXT",
        "ST_POLYFROMWKB",
        "ST_POLYGONFROMTEXT",
        "ST_POLYGONFROMWKB",
        "ST_RELATE",
        "ST_SRID",
        "ST_STARTPOINT",
        "ST_SYMDIFFERENCE",
        "ST_TOUCHES",
        "ST_UNION",
        "ST_WITHIN",
        "ST_X",
        "ST_Y",
        "TOUCHES",
        "WITHIN",
        "X",
        "Y",
    };

    for (const char *name : k_gis_sql_function_names) {
        if (sql_token_equals(token, name)) {
            return true;
        }
    }
    return false;
}

bool is_locking_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);

    if (sql_token_equals(first, "LOCK") || sql_token_equals(first, "UNLOCK")) {
        const std::string_view second = pop_sql_token(rest);
        return sql_token_equals(second, "TABLE") || sql_token_equals(second, "TABLES");
    }
    if (sql_tokens_contain_named_lock_function(sql)) {
        return true;
    }

    return sql_token_equals(first, "SELECT") && sql_tokens_contain_locking_marker(rest);
}

bool sql_tokens_contain_locking_marker(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (sql_token_equals(token, "FOR")) {
            std::string_view after_for = sql;
            std::string_view next;
            if (pop_sql_scanned_token(after_for, next) && sql_token_equals(next, "UPDATE")) {
                return true;
            }
        }

        if (sql_token_equals(token, "LOCK")) {
            std::string_view after_lock = sql;
            std::string_view next;
            if (!pop_sql_scanned_token(after_lock, next) || !sql_token_equals(next, "IN")) {
                continue;
            }
            if (!pop_sql_scanned_token(after_lock, next) || !sql_token_equals(next, "SHARE")) {
                continue;
            }
            if (pop_sql_scanned_token(after_lock, next) && sql_token_equals(next, "MODE")) {
                return true;
            }
        }
    }
    return false;
}

bool sql_tokens_contain_named_lock_function(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (!sql_next_non_noise_is(sql, '(')) {
            continue;
        }

        if (sql_token_equals(token, "GET_LOCK") || sql_token_equals(token, "RELEASE_LOCK") ||
            sql_token_equals(token, "RELEASE_ALL_LOCKS") ||
            sql_token_equals(token, "IS_FREE_LOCK") || sql_token_equals(token, "IS_USED_LOCK")) {
            return true;
        }
    }
    return false;
}

bool is_online_alter_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    std::string_view token = pop_sql_token(rest);
    if (!sql_token_equals(token, "ALTER")) {
        return false;
    }

    token = pop_sql_token(rest);
    if (sql_token_equals(token, "IGNORE")) {
        token = pop_sql_token(rest);
    }

    if (sql_token_equals(token, "ONLINE")) {
        return true;
    }
    if (sql_token_equals(token, "OFFLINE")) {
        token = pop_sql_token(rest);
    }

    if (!sql_token_equals(token, "TABLE")) {
        return false;
    }

    return sql_tokens_contain_online_alter_marker(rest);
}

bool sql_tokens_contain_online_alter_marker(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (sql_token_equals(token, "ALGORITHM")) {
            std::string_view after_algorithm = sql;
            std::string_view next;
            if (!pop_sql_scanned_token(after_algorithm, next)) {
                continue;
            }
            if (sql_token_equals(next, "INPLACE") || sql_token_equals(next, "INSTANT") ||
                sql_token_equals(next, "NOCOPY")) {
                return true;
            }
        }

        if (sql_token_equals(token, "LOCK")) {
            std::string_view after_lock = sql;
            std::string_view next;
            if (pop_sql_scanned_token(after_lock, next) && sql_token_equals(next, "NONE")) {
                return true;
            }
        }
    }
    return false;
}

bool is_partition_sql(std::string_view sql) {
    std::string_view rest = sql;
    std::string_view token;
    if (!pop_sql_scanned_token(rest, token)) {
        return false;
    }

    if (sql_token_equals(token, "CREATE")) {
        if (!pop_sql_scanned_token(rest, token)) {
            return false;
        }
        if (sql_token_equals(token, "OR")) {
            if (!pop_sql_scanned_token(rest, token) || !sql_token_equals(token, "REPLACE") ||
                !pop_sql_scanned_token(rest, token)) {
                return false;
            }
        }
        if (sql_token_equals(token, "TEMPORARY")) {
            if (!pop_sql_scanned_token(rest, token)) {
                return false;
            }
        }
        return sql_token_equals(token, "TABLE") && sql_tokens_contain_partition_marker(rest);
    }

    if (sql_token_equals(token, "ALTER")) {
        if (!pop_sql_scanned_token(rest, token)) {
            return false;
        }
        if (sql_token_equals(token, "IGNORE") || sql_token_equals(token, "ONLINE") ||
            sql_token_equals(token, "OFFLINE")) {
            if (!pop_sql_scanned_token(rest, token)) {
                return false;
            }
        }
        return sql_token_equals(token, "TABLE") && sql_tokens_contain_partition_marker(rest);
    }

    return false;
}

bool sql_tokens_contain_partition_marker(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (sql_token_equals(token, "PARTITION") || sql_token_equals(token, "PARTITIONS") ||
            sql_token_equals(token, "PARTITIONING") || sql_token_equals(token, "SUBPARTITION") ||
            sql_token_equals(token, "SUBPARTITIONS")) {
            return true;
        }
    }
    return false;
}

bool is_foreign_key_sql(std::string_view sql) {
    std::string_view rest = sql;
    std::string_view token;
    if (!pop_sql_scanned_token(rest, token)) {
        return false;
    }

    if (sql_token_equals(token, "CREATE")) {
        if (!pop_sql_scanned_token(rest, token)) {
            return false;
        }
        if (sql_token_equals(token, "OR")) {
            if (!pop_sql_scanned_token(rest, token) || !sql_token_equals(token, "REPLACE") ||
                !pop_sql_scanned_token(rest, token)) {
                return false;
            }
        }
        if (sql_token_equals(token, "TEMPORARY")) {
            if (!pop_sql_scanned_token(rest, token)) {
                return false;
            }
        }
        return sql_token_equals(token, "TABLE") && sql_tokens_contain_foreign_key_marker(rest);
    }

    if (sql_token_equals(token, "ALTER")) {
        if (!pop_sql_scanned_token(rest, token)) {
            return false;
        }
        if (sql_token_equals(token, "IGNORE") || sql_token_equals(token, "ONLINE") ||
            sql_token_equals(token, "OFFLINE")) {
            if (!pop_sql_scanned_token(rest, token)) {
                return false;
            }
        }
        return sql_token_equals(token, "TABLE") && sql_tokens_contain_foreign_key_marker(rest);
    }

    return false;
}

bool sql_tokens_contain_foreign_key_marker(std::string_view sql) {
    std::string_view token;
    while (pop_sql_scanned_token(sql, token)) {
        if (sql_token_equals(token, "REFERENCES")) {
            return true;
        }
        if (sql_token_equals(token, "FOREIGN")) {
            std::string_view after_foreign = sql;
            std::string_view next;
            if (pop_sql_scanned_token(after_foreign, next) && sql_token_equals(next, "KEY")) {
                return true;
            }
        }
    }
    return false;
}

std::string_view skip_sql_leading_noise(std::string_view sql) {
    for (;;) {
        while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.front())) != 0) {
            sql.remove_prefix(1);
        }
        if (sql.size() >= 2U && sql[0] == '-' && sql[1] == '-') {
            const std::size_t newline = sql.find('\n');
            if (newline == std::string_view::npos) {
                return {};
            }
            sql.remove_prefix(newline + 1U);
            continue;
        }
        if (!sql.empty() && sql.front() == '#') {
            const std::size_t newline = sql.find('\n');
            if (newline == std::string_view::npos) {
                return {};
            }
            sql.remove_prefix(newline + 1U);
            continue;
        }
        if (sql.size() >= 2U && sql[0] == '/' && sql[1] == '*') {
            if (executable_sql_comment_prefix_size(sql) != 0U) {
                return sql;
            }
            const std::size_t end = sql.find("*/");
            if (end == std::string_view::npos) {
                return {};
            }
            sql.remove_prefix(end + 2U);
            continue;
        }
        return sql;
    }
}

std::string_view pop_sql_token(std::string_view &sql) {
    while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.front())) != 0) {
        sql.remove_prefix(1);
    }

    const std::size_t token_end =
        sql.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
    if (token_end == std::string_view::npos) {
        std::string_view token = sql;
        sql = {};
        return token;
    }

    std::string_view token = sql.substr(0, token_end);
    sql.remove_prefix(token_end);
    return token;
}

std::string_view pop_sql_token_after_separators(std::string_view &sql) {
    while (!sql.empty() &&
           (std::isalnum(static_cast<unsigned char>(sql.front())) == 0 && sql.front() != '_')) {
        sql.remove_prefix(1);
    }
    return pop_sql_token(sql);
}

bool pop_sql_scanned_token(std::string_view &sql, std::string_view &out_token) {
    for (;;) {
        while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.front())) != 0) {
            sql.remove_prefix(1);
        }

        if (sql.empty()) {
            out_token = {};
            return false;
        }

        if (sql.size() >= 2U && sql[0] == '-' && sql[1] == '-') {
            const std::size_t newline = sql.find('\n');
            if (newline == std::string_view::npos) {
                sql = {};
                out_token = {};
                return false;
            }
            sql.remove_prefix(newline + 1U);
            continue;
        }

        if (sql.front() == '#') {
            const std::size_t newline = sql.find('\n');
            if (newline == std::string_view::npos) {
                sql = {};
                out_token = {};
                return false;
            }
            sql.remove_prefix(newline + 1U);
            continue;
        }

        if (sql.size() >= 2U && sql[0] == '/' && sql[1] == '*') {
            const std::size_t executable_comment_prefix = executable_sql_comment_prefix_size(sql);
            if (executable_comment_prefix != 0U) {
                skip_executable_sql_comment_prefix(sql);
                continue;
            }

            const std::size_t end = sql.find("*/");
            if (end == std::string_view::npos) {
                sql = {};
                out_token = {};
                return false;
            }
            sql.remove_prefix(end + 2U);
            continue;
        }

        if (sql.front() == '\'' || sql.front() == '"' || sql.front() == '`') {
            skip_sql_quoted_span(sql, sql.front());
            continue;
        }

        if (std::isalnum(static_cast<unsigned char>(sql.front())) != 0 || sql.front() == '_') {
            const std::size_t token_end = sql.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
            );
            if (token_end == std::string_view::npos) {
                out_token = sql;
                sql = {};
                return true;
            }

            out_token = sql.substr(0, token_end);
            sql.remove_prefix(token_end);
            return true;
        }

        sql.remove_prefix(1);
    }
}

void skip_sql_quoted_span(std::string_view &sql, char quote) {
    sql.remove_prefix(1);
    while (!sql.empty()) {
        const char current = sql.front();
        sql.remove_prefix(1);

        if (quote != '`' && current == '\\') {
            if (!sql.empty()) {
                sql.remove_prefix(1);
            }
            continue;
        }

        if (current == quote) {
            if (!sql.empty() && sql.front() == quote) {
                sql.remove_prefix(1);
                continue;
            }
            return;
        }
    }
}

bool sql_next_non_noise_is(std::string_view sql, char expected) {
    const std::string_view rest = skip_sql_leading_noise(sql);
    return !rest.empty() && rest.front() == expected;
}

bool sql_starts_with_token(std::string_view sql, const char *keyword) {
    const std::string_view rest = skip_sql_leading_noise(sql);
    std::size_t i = 0;
    for (; keyword[i] != '\0'; ++i) {
        if (i >= rest.size()) {
            return false;
        }
        const auto token_char = static_cast<unsigned char>(rest[i]);
        const auto keyword_char = static_cast<unsigned char>(keyword[i]);
        if (std::toupper(token_char) != std::toupper(keyword_char)) {
            return false;
        }
    }
    return i == rest.size() ||
           (std::isalnum(static_cast<unsigned char>(rest[i])) == 0 && rest[i] != '_');
}

std::size_t executable_sql_comment_prefix_size(std::string_view sql) {
    if (sql.size() >= 3U && sql[0] == '/' && sql[1] == '*' && sql[2] == '!') {
        return 3U;
    }
    if (sql.size() >= 4U && sql[0] == '/' && sql[1] == '*' && sql[2] == 'M' && sql[3] == '!') {
        return 4U;
    }
    return 0U;
}

void skip_executable_sql_comment_prefix(std::string_view &sql) {
    sql.remove_prefix(executable_sql_comment_prefix_size(sql));
    while (!sql.empty() && std::isdigit(static_cast<unsigned char>(sql.front())) != 0) {
        sql.remove_prefix(1);
    }
}

bool sql_token_equals(std::string_view token, const char *keyword) {
    for (std::size_t i = 0; keyword[i] != '\0'; ++i) {
        if (i >= token.size()) {
            return false;
        }
        const auto token_char = static_cast<unsigned char>(token[i]);
        const auto keyword_char = static_cast<unsigned char>(keyword[i]);
        if (std::toupper(token_char) != std::toupper(keyword_char)) {
            return false;
        }
    }
    return token.size() == std::strlen(keyword);
}

#  if MYLITE_MARIADB_HAS_MYLITE_SE
int sync_schema_catalog(mylite_db &database) {
    std::vector<SchemaDefinition> runtime_schemas;
    int result = load_runtime_schema_definitions(database, runtime_schemas);
    if (result != MYLITE_OK) {
        return result;
    }

    for (const SchemaDefinition &schema : runtime_schemas) {
        mylite_storage_schema_definition definition = {};
        definition.size = sizeof(definition);
        definition.schema_name = schema.name.c_str();
        definition.default_character_set_name = schema.default_character_set_name.c_str();
        definition.default_collation_name = schema.default_collation_name.c_str();
        definition.schema_comment = schema.comment.c_str();
        const mylite_storage_result storage_result =
            mylite_storage_store_schema_definition(database.filename.c_str(), &definition);
        if (storage_result != MYLITE_STORAGE_OK) {
            set_error(database, map_storage_result(storage_result), "schema catalog write failed");
            return database.errcode;
        }
    }

    std::vector<std::string> catalog_schema_names;
    mylite_storage_result storage_result = mylite_storage_list_schemas(
        database.filename.c_str(),
        collect_storage_schema,
        &catalog_schema_names
    );
    if (storage_result != MYLITE_STORAGE_OK) {
        set_error(database, map_storage_result(storage_result), "schema catalog read failed");
        return database.errcode;
    }

    for (const std::string &schema_name : catalog_schema_names) {
        if (has_schema_name(runtime_schemas, schema_name)) {
            continue;
        }
        storage_result = mylite_storage_drop_schema(database.filename.c_str(), schema_name.c_str());
        if (storage_result != MYLITE_STORAGE_OK && storage_result != MYLITE_STORAGE_NOTFOUND) {
            set_error(database, map_storage_result(storage_result), "schema catalog drop failed");
            return database.errcode;
        }
    }

    return MYLITE_OK;
}

bool is_schema_catalog_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);
    std::string_view second = pop_sql_token(rest);

    if (sql_token_equals(first, "CREATE") && sql_token_equals(second, "OR")) {
        const std::string_view third = pop_sql_token(rest);
        if (!sql_token_equals(third, "REPLACE")) {
            return false;
        }
        second = pop_sql_token(rest);
    }

    return (sql_token_equals(first, "CREATE") || sql_token_equals(first, "DROP") ||
            sql_token_equals(first, "ALTER")) &&
           (sql_token_equals(second, "DATABASE") || sql_token_equals(second, "SCHEMA"));
}

bool is_storage_outer_checkpoint_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);

    return sql_token_equals(first, "CREATE") || sql_token_equals(first, "ALTER") ||
           sql_token_equals(first, "DROP") || sql_token_equals(first, "RENAME") ||
           sql_token_equals(first, "TRUNCATE");
}

bool is_row_dml_checkpoint_sql(std::string_view sql) {
    std::string_view rest = skip_sql_leading_noise(sql);
    const std::string_view first = pop_sql_token(rest);

    return sql_token_equals(first, "INSERT") || sql_token_equals(first, "UPDATE") ||
           sql_token_equals(first, "DELETE") || sql_token_equals(first, "REPLACE");
}

int collect_storage_schema(void *ctx, const char *schema_name) {
    auto *schema_names = static_cast<std::vector<std::string> *>(ctx);
    try {
        schema_names->emplace_back(schema_name);
    } catch (const std::bad_alloc &) {
        return 1;
    }
    return 0;
}

int load_runtime_schema_definitions(
    mylite_db &database,
    std::vector<SchemaDefinition> &out_definitions
) {
    if (mysql_query(
            &database.mysql,
            "SELECT SCHEMA_NAME, DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME, "
            "SCHEMA_COMMENT FROM INFORMATION_SCHEMA.SCHEMATA"
        ) != 0) {
        set_mariadb_error(database);
        return MYLITE_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(&database.mysql);
    if (result == nullptr) {
        if (mysql_field_count(&database.mysql) != 0U) {
            set_mariadb_error(database);
            return MYLITE_ERROR;
        }
        return MYLITE_OK;
    }

    try {
        for (MYSQL_ROW row = mysql_fetch_row(result); row != nullptr;
             row = mysql_fetch_row(result)) {
            unsigned long *lengths = mysql_fetch_lengths(result);
            if (lengths == nullptr || row[0] == nullptr || row[1] == nullptr || row[2] == nullptr ||
                row[3] == nullptr) {
                mysql_free_result(result);
                set_error(database, MYLITE_ERROR, "malformed INFORMATION_SCHEMA.SCHEMATA result");
                return MYLITE_ERROR;
            }

            std::string_view schema_name(row[0], static_cast<std::size_t>(lengths[0]));
            if (!is_system_schema(schema_name)) {
                out_definitions.emplace_back(
                    SchemaDefinition{
                        std::string(schema_name),
                        std::string(row[1], static_cast<std::size_t>(lengths[1])),
                        std::string(row[2], static_cast<std::size_t>(lengths[2])),
                        std::string(row[3], static_cast<std::size_t>(lengths[3])),
                    }
                );
            }
        }
    } catch (const std::bad_alloc &) {
        mysql_free_result(result);
        set_error(database, MYLITE_NOMEM, "schema list allocation failed");
        return MYLITE_NOMEM;
    }

    if (mysql_errno(&database.mysql) != 0U) {
        mysql_free_result(result);
        set_mariadb_error(database);
        return MYLITE_ERROR;
    }

    mysql_free_result(result);
    return MYLITE_OK;
}

bool is_system_schema(std::string_view schema_name) {
    return sql_token_equals(schema_name, "information_schema") ||
           sql_token_equals(schema_name, "mysql") ||
           sql_token_equals(schema_name, "performance_schema") ||
           sql_token_equals(schema_name, "sys");
}

bool has_schema_name(const std::vector<SchemaDefinition> &schemas, const std::string &schema_name) {
    return std::find_if(
               schemas.begin(),
               schemas.end(),
               [&schema_name](const SchemaDefinition &schema) { return schema.name == schema_name; }
           ) != schemas.end();
}
#  endif

int prepare_primary_file(const std::filesystem::path &filename, unsigned flags) {
    if (filename == ":memory:") {
        return MYLITE_OK;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(filename, error);
    if (error) {
        return MYLITE_IOERR;
    }

    if ((flags & MYLITE_OPEN_EXCLUSIVE) != 0U && exists) {
        return MYLITE_ERROR;
    }

    if (!exists && (flags & MYLITE_OPEN_CREATE) == 0U) {
        return MYLITE_NOTFOUND;
    }

    const std::string path = filename.string();
    if (exists) {
        mylite_storage_header header = {};
        return map_storage_result(mylite_storage_open_header(path.c_str(), &header));
    }

    if (!exists) {
        const std::filesystem::path parent = filename.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error) {
                return MYLITE_IOERR;
            }
        }
        return map_storage_result(mylite_storage_create_empty(path.c_str()));
    }

    return MYLITE_OK;
}

int map_storage_result(mylite_storage_result result) {
    switch (result) {
    case MYLITE_STORAGE_OK:
        return MYLITE_OK;
    case MYLITE_STORAGE_NOMEM:
        return MYLITE_NOMEM;
    case MYLITE_STORAGE_BUSY:
        return MYLITE_BUSY;
    case MYLITE_STORAGE_READONLY:
        return MYLITE_READONLY;
    case MYLITE_STORAGE_IOERR:
        return MYLITE_IOERR;
    case MYLITE_STORAGE_CORRUPT:
    case MYLITE_STORAGE_UNSUPPORTED:
        return MYLITE_CORRUPT;
    case MYLITE_STORAGE_NOTFOUND:
        return MYLITE_NOTFOUND;
    case MYLITE_STORAGE_FULL:
        return MYLITE_FULL;
    case MYLITE_STORAGE_MISUSE:
        return MYLITE_MISUSE;
    case MYLITE_STORAGE_ERROR:
        return MYLITE_ERROR;
    }

    return MYLITE_ERROR;
}

int start_runtime(mylite_db &database, const mylite_open_config *config) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count > 0U) {
        if (g_runtime.filename != database.filename) {
            set_error(
                database,
                MYLITE_BUSY,
                "embedded runtime is already open for another database"
            );
            return MYLITE_BUSY;
        }
        ++g_runtime.ref_count;
        return MYLITE_OK;
    }

#  if MYLITE_WITH_MARIADB_EMBEDDED
    const std::filesystem::path runtime_dir = create_runtime_directory(config);
    g_runtime.arguments = runtime_arguments(runtime_dir, database.filename);
    g_runtime.argv = mutable_arguments(g_runtime.arguments);
    char *groups[] = {const_cast<char *>("server"), const_cast<char *>("embedded"), nullptr};

    const int init_result =
        mysql_server_init(static_cast<int>(g_runtime.argv.size()), g_runtime.argv.data(), groups);
    if (init_result != 0) {
        g_runtime.argv.clear();
        g_runtime.arguments.clear();
        remove_directory_if_present(runtime_dir);
        set_error(database, MYLITE_ERROR, "MariaDB embedded runtime initialization failed");
        return MYLITE_ERROR;
    }

    g_runtime.directory = runtime_dir;
    g_runtime.filename = database.filename;
    g_runtime.ref_count = 1;
    return MYLITE_OK;
#  else
    (void)config;
    set_error(database, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#  endif
}

int connect_runtime(mylite_db &database) {
#  if MYLITE_WITH_MARIADB_EMBEDDED
    if (mysql_init(&database.mysql) == nullptr) {
        set_error(database, MYLITE_NOMEM, "MariaDB connection allocation failed");
        return MYLITE_NOMEM;
    }

    MYSQL *connection =
        mysql_real_connect(&database.mysql, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0);
    if (connection == nullptr) {
        set_mariadb_error(database);
        return MYLITE_ERROR;
    }

    database.connected = true;
    return configure_connection(database);
#  endif
}

int configure_connection(mylite_db &database) {
#  if MYLITE_MARIADB_HAS_MYLITE_SE
    if (database.filename == ":memory:") {
        return MYLITE_OK;
    }

    if (mysql_query(&database.mysql, "SET SESSION sql_mode=''") != 0 ||
        mysql_query(&database.mysql, "SET SESSION default_storage_engine=MYLITE") != 0 ||
        mysql_query(&database.mysql, "SET SESSION enforce_storage_engine=MYLITE") != 0) {
        set_mariadb_error(database);
        return MYLITE_ERROR;
    }
    return MYLITE_OK;
#  else
    (void)database;
#  endif

    return MYLITE_OK;
}
#endif

void close_connection(mylite_db &database) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (database.connected) {
        mysql_close(&database.mysql);
        database.connected = false;
    }
#else
    (void)database;
#endif
}

void release_runtime(void) {
    std::filesystem::path runtime_dir;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        if (g_runtime.ref_count == 0U) {
            return;
        }

        --g_runtime.ref_count;
        if (g_runtime.ref_count > 0U) {
            return;
        }

        runtime_dir = g_runtime.directory;
#if MYLITE_WITH_MARIADB_EMBEDDED
        mysql_server_end();
#endif
        g_runtime.directory.clear();
        g_runtime.filename.clear();
        g_runtime.argv.clear();
        g_runtime.arguments.clear();
    }

    remove_directory_if_present(runtime_dir);
}

#if MYLITE_WITH_MARIADB_EMBEDDED
std::filesystem::path normalize_filename(const char *filename) {
    if (std::strcmp(filename, ":memory:") == 0) {
        return std::filesystem::path(":memory:");
    }
    return std::filesystem::absolute(std::filesystem::path(filename));
}

unsigned busy_timeout_from_config(const mylite_open_config *config) {
    if (config != nullptr &&
        has_config_field(
            config,
            offsetof(mylite_open_config, busy_timeout_ms) + sizeof(config->busy_timeout_ms)
        )) {
        return config->busy_timeout_ms;
    }
    return 0U;
}

std::filesystem::path create_runtime_directory(const mylite_open_config *config) {
    std::filesystem::path root = runtime_root(config);
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        throw std::filesystem::filesystem_error("create runtime root", root, error);
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        std::filesystem::path candidate = root / unique_runtime_name();
        if (std::filesystem::create_directory(candidate, error)) {
            std::filesystem::create_directories(candidate / "data");
            std::filesystem::create_directories(candidate / "tmp");
            std::filesystem::create_directories(candidate / "plugins");
            return candidate;
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

std::vector<std::string> runtime_arguments(
    const std::filesystem::path &runtime_dir,
    const std::string &primary_filename
) {
    const std::filesystem::path data_dir = runtime_dir / "data";
    const std::filesystem::path tmp_dir = runtime_dir / "tmp";
    const std::filesystem::path plugin_dir = runtime_dir / "plugins";

    std::vector<std::string> arguments = {
        "mylite",
        "--no-defaults",
        "--datadir=" + data_dir.string(),
        "--tmpdir=" + tmp_dir.string(),
        "--plugin-dir=" + plugin_dir.string(),
        "--skip-grant-tables",
        "--skip-log-bin",
        "--skip-networking",
        "--default-storage-engine=MyISAM",
        "--innodb=OFF",
#  if MYLITE_MARIADB_HAS_PERFSCHEMA
        "--performance-schema=OFF",
#  endif
        "--lc-messages-dir=" MYLITE_MARIADB_MESSAGES_DIR,
        "--character-sets-dir=" MYLITE_MARIADB_CHARSETS_DIR,
    };
#  if MYLITE_MARIADB_HAS_MYLITE_SE
    if (primary_filename != ":memory:") {
        arguments.push_back("--mylite-primary-file=" + primary_filename);
    }
#  else
    (void)primary_filename;
#  endif
    return arguments;
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

int copy_error_message(mylite_db &database, char **errmsg) {
    if (errmsg == nullptr) {
        return database.errcode;
    }

    const std::size_t length = database.errmsg.size();
    char *copy = static_cast<char *>(std::malloc(length + 1U));
    if (copy == nullptr) {
        return MYLITE_NOMEM;
    }

    std::memcpy(copy, database.errmsg.c_str(), length + 1U);
    *errmsg = copy;
    return database.errcode;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void clear_warnings(mylite_db &database) {
    database.warning_count = 0;
    database.warnings.clear();
}
#endif

int bind_parameter_index(mylite_stmt *statement, unsigned index) {
    if (statement == nullptr || statement->database == nullptr) {
        return -1;
    }
    if (index == 0U || index > statement->parameters.size()) {
        set_error(*statement->database, MYLITE_MISUSE, "invalid bind parameter index");
        return -1;
    }
    return static_cast<int>(index - 1U);
}

int bind_bytes(
    mylite_stmt *statement,
    unsigned index,
    BoundValueKind kind,
    const void *value,
    std::size_t value_len,
    mylite_destructor destructor
) {
    const int parameter = bind_parameter_index(statement, index);
    if (parameter < 0) {
        return MYLITE_MISUSE;
    }

    if (value == nullptr && value_len != 0U) {
        set_error(*statement->database, MYLITE_MISUSE, "non-empty bind value is NULL");
        return MYLITE_MISUSE;
    }
    if (value_len > static_cast<std::size_t>(ULONG_MAX)) {
        set_error(*statement->database, MYLITE_MISUSE, "bind value is too large");
        return MYLITE_MISUSE;
    }

    try {
        BoundValue &bound = statement->parameters[static_cast<std::size_t>(parameter)];
        bound.reset_to_null();
        bound.kind = kind;
        bound.length = value_len;
        bound.mysql_length = static_cast<unsigned long>(value_len);
        bound.mysql_is_null = false;

        if (is_transient_destructor(destructor)) {
            if (value_len > 0U) {
                const auto *begin = static_cast<const unsigned char *>(value);
                bound.owned_data.assign(begin, begin + value_len);
            }
        } else {
            bound.borrowed_data = value;
            if (is_custom_destructor(destructor)) {
                bound.destructor = destructor;
                bound.destructor_arg = const_cast<void *>(value);
            }
        }
    } catch (const std::bad_alloc &) {
        set_error(*statement->database, MYLITE_NOMEM, "bind value allocation failed");
        return MYLITE_NOMEM;
    }

    set_ok(*statement->database);
    return MYLITE_OK;
}

bool is_custom_destructor(mylite_destructor destructor) {
    return destructor != MYLITE_STATIC && !is_transient_destructor(destructor);
}

bool is_transient_destructor(mylite_destructor destructor) {
    return destructor == MYLITE_TRANSIENT; // NOLINT(performance-no-int-to-ptr)
}

void reset_statement_bindings(mylite_stmt &statement) {
    for (BoundValue &parameter : statement.parameters) {
        parameter.reset_to_null();
    }
}

void set_ok(mylite_db &database) {
    database.errcode = MYLITE_OK;
    database.extended_errcode = MYLITE_OK;
    database.mariadb_errno = 0;
    database.sqlstate = k_sqlstate_ok;
    database.errmsg = k_not_an_error;
}

void set_error(mylite_db &database, int code, const char *message) {
    database.errcode = code;
    database.extended_errcode = code;
    database.mariadb_errno = 0;
    database.sqlstate = k_sqlstate_general;
    database.errmsg = message;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &database) {
    database.errcode = MYLITE_ERROR;
    database.extended_errcode = MYLITE_ERROR;
    database.mariadb_errno = mysql_errno(&database.mysql);
    database.sqlstate = mysql_sqlstate(&database.mysql);
    database.errmsg = mysql_error(&database.mysql);
}

void set_mariadb_statement_error(mylite_stmt &statement) {
    mylite_db &database = *statement.database;
    database.errcode = MYLITE_ERROR;
    database.extended_errcode = MYLITE_ERROR;
    database.mariadb_errno = mysql_stmt_errno(statement.statement);
    database.sqlstate = mysql_stmt_sqlstate(statement.statement);
    database.errmsg = mysql_stmt_error(statement.statement);
}
#endif

const char *safe_c_str(const std::string &value) {
    return value.empty() ? "" : value.c_str();
}

bool has_config_field(const mylite_open_config *config, std::size_t field_end) {
    return config != nullptr && config->size >= field_end;
}

} // namespace

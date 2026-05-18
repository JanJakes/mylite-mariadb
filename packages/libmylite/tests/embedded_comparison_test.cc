#include <mylite/mylite.h>

#include <mysql.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef MYLITE_MARIADB_MESSAGES_DIR
#  define MYLITE_MARIADB_MESSAGES_DIR ""
#endif

#ifndef MYLITE_MARIADB_CHARSETS_DIR
#  define MYLITE_MARIADB_CHARSETS_DIR ""
#endif

#ifndef MYLITE_MARIADB_HAS_PERFSCHEMA
#  define MYLITE_MARIADB_HAS_PERFSCHEMA 1
#endif

#ifndef MYLITE_MARIADB_HAS_INNOBASE
#  define MYLITE_MARIADB_HAS_INNOBASE 1
#endif

namespace {

struct Cell {
    bool is_null = true;
    std::string value;
};

struct QueryResult {
    std::vector<std::string> names;
    std::vector<std::vector<Cell>> rows;
    unsigned warning_count = 0;
};

struct PreparedResult {
    std::vector<std::string> names;
    std::vector<unsigned> native_types;
    bool missing_is_null = false;
    long long signed_value = 0;
    unsigned long long unsigned_value = 0;
    double double_value = 0.0;
    std::string text_value;
};

struct WarningRow {
    std::string level;
    unsigned code = 0;
    std::string message;
};

struct StatementEffect {
    long long affected_rows = 0;
    unsigned long long last_insert_id = 0;
};

struct StatementEffects {
    std::vector<StatementEffect> values;
};

struct Observation {
    QueryResult direct;
    std::vector<QueryResult> direct_expressions;
    PreparedResult prepared;
    StatementEffects effects;
    unsigned warning_count = 0;
    WarningRow warning;
};

struct RawRuntime {
    MYSQL mysql = {};
    std::filesystem::path root;
    std::filesystem::path runtime_dir;
    std::vector<std::string> arguments;
    std::vector<char *> argv;
    bool connected = false;
    bool initialized = false;
};

struct MyLiteRuntime {
    mylite_db *database = nullptr;
    std::filesystem::path root;
};

constexpr const char *kDirectSql = "SELECT 1 AS one,"
                                   "CAST(-42 AS SIGNED) AS signed_value,"
                                   "CAST(42 AS UNSIGNED) AS unsigned_value,"
                                   "CAST(3.25 AS DOUBLE) AS double_value,"
                                   "CONCAT('al','pha') AS text_value,"
                                   "NULL AS missing";

constexpr std::array<const char *, 8> kDirectExpressionSql = {{
    "SELECT "
    "CASE WHEN 2 > 1 THEN 'case-hit' ELSE 'case-miss' END AS case_value,"
    "COALESCE(NULL, 'fallback') AS coalesced,"
    "NULLIF('same', 'same') AS nullif_value",
    "SELECT "
    "7 DIV 3 AS div_value,"
    "7 MOD 3 AS mod_value,"
    "ROUND(12.345, 2) AS rounded_value",
    "SELECT "
    "'alpha' IN ('alpha', 'beta') AS in_hit,"
    "'alpha' NOT IN ('gamma', 'delta') AS not_in_hit,"
    "NULL IN ('alpha') AS null_in",
    "SELECT "
    "DATE_ADD('2026-05-18', INTERVAL 2 DAY) AS plus_two_days,"
    "TIMESTAMPDIFF(DAY, '2026-05-18', '2026-05-21') AS date_delta",
    "SELECT "
    "CONCAT('my', 'lite') AS joined_text,"
    "REPLACE('embedded db', 'db', 'sql') AS replaced_text,"
    "CHAR_LENGTH('portable') AS text_length",
    "SELECT "
    "CAST('42' AS SIGNED) + CAST('8' AS UNSIGNED) AS numeric_cast,"
    "CAST(3.14159 AS DECIMAL(6,3)) AS decimal_cast",
    "SELECT 1 AS n UNION ALL SELECT 2 UNION ALL SELECT 3 ORDER BY n",
    "SELECT "
    "NOT (1 = 0) AS not_value,"
    "(1 <=> NULL) AS null_safe_false,"
    "(NULL <=> NULL) AS null_safe_true",
}};

constexpr const char *kPreparedSql = "SELECT ? AS missing,"
                                     "CAST(? AS SIGNED) AS signed_value,"
                                     "CAST(? AS UNSIGNED) AS unsigned_value,"
                                     "CAST(? AS DOUBLE) AS double_value,"
                                     "CAST(? AS CHAR) AS text_value";

constexpr const char *kWarningSql = "SELECT CAST('not-a-number' AS UNSIGNED)";
constexpr std::size_t kPreparedColumnCount = 5;
constexpr unsigned kMissingParameter = 1;
constexpr unsigned kSignedParameter = 2;
constexpr unsigned kUnsignedParameter = 3;
constexpr unsigned kDoubleParameter = 4;
constexpr unsigned kTextParameter = 5;
constexpr long long kSignedProbeValue = -42;
constexpr unsigned long long kUnsignedProbeValue = 42U;
constexpr double kDoubleProbeValue = 3.25;
constexpr double kDoubleTolerance = 0.0001;
constexpr const char *kTextProbeValue = "hello";
constexpr unsigned long kTextProbeLength = 5;
constexpr std::size_t kTextOutputBufferSize = 32;
constexpr std::size_t kWarningColumnCount = 3;
constexpr std::size_t kStatementEffectCount = 6;

Observation run_raw_observation(void);
Observation run_mylite_observation(void);
void compare_observations(const Observation &expected, const Observation &actual);
void start_raw_runtime(RawRuntime &runtime);
void stop_raw_runtime(RawRuntime &runtime);
QueryResult run_raw_query(RawRuntime &runtime, const char *sql);
std::vector<QueryResult> run_raw_direct_expression_matrix(RawRuntime &runtime);
PreparedResult run_raw_prepared_query(RawRuntime &runtime);
StatementEffects run_raw_statement_effects(RawRuntime &runtime);
StatementEffect run_raw_effect_query(RawRuntime &runtime, const char *sql);
StatementEffect run_raw_prepared_insert(RawRuntime &runtime);
StatementEffect run_raw_prepared_update(RawRuntime &runtime);
StatementEffect run_raw_prepared_delete(RawRuntime &runtime);
StatementEffect raw_statement_effect(RawRuntime &runtime);
Observation run_mylite_queries(mylite_db *database);
QueryResult run_mylite_query(mylite_db *database, const char *sql);
std::vector<QueryResult> run_mylite_direct_expression_matrix(mylite_db *database);
PreparedResult run_mylite_prepared_query(mylite_db *database);
StatementEffects run_mylite_statement_effects(mylite_db *database);
StatementEffect run_mylite_effect_query(mylite_db *database, const char *sql);
StatementEffect run_mylite_prepared_insert(mylite_db *database);
StatementEffect run_mylite_prepared_update(mylite_db *database);
StatementEffect run_mylite_prepared_delete(mylite_db *database);
StatementEffect mylite_statement_effect(mylite_db *database);
WarningRow first_mylite_warning(mylite_db *database);
void open_mylite_runtime(MyLiteRuntime &runtime);
void close_mylite_runtime(MyLiteRuntime &runtime);
int collect_mylite_row(void *ctx, int column_count, char **values, char **column_names);
void compare_query_result_sets(
    const std::vector<QueryResult> &expected,
    const std::vector<QueryResult> &actual
);
void compare_query_results(const QueryResult &expected, const QueryResult &actual);
void compare_prepared_results(const PreparedResult &expected, const PreparedResult &actual);
void compare_statement_effects(const StatementEffects &expected, const StatementEffects &actual);
void compare_warning_rows(const WarningRow &expected, const WarningRow &actual);
WarningRow first_raw_warning(RawRuntime &runtime);
std::filesystem::path make_temp_root(const char *prefix);
std::vector<std::string> raw_runtime_arguments(const std::filesystem::path &runtime_dir);
std::vector<char *> mutable_arguments(std::vector<std::string> &arguments);
void create_raw_runtime_directories(const std::filesystem::path &runtime_dir);
void remove_tree(const std::filesystem::path &path);
std::string cell_value(const char *value, unsigned long length);
std::string mylite_warning_level_name(mylite_warning_level level);
unsigned long string_length(const char *value);

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() noexcept {
    const Observation expected = run_raw_observation();
    const Observation actual = run_mylite_observation();
    compare_observations(expected, actual);
    return 0;
}

namespace {

Observation run_raw_observation(void) {
    RawRuntime runtime;
    start_raw_runtime(runtime);
    Observation observation = {};

    observation.direct = run_raw_query(runtime, kDirectSql);
    observation.direct_expressions = run_raw_direct_expression_matrix(runtime);
    observation.prepared = run_raw_prepared_query(runtime);
    observation.effects = run_raw_statement_effects(runtime);
    const QueryResult warning_result = run_raw_query(runtime, kWarningSql);
    observation.warning_count = warning_result.warning_count;
    observation.warning = first_raw_warning(runtime);

    stop_raw_runtime(runtime);
    return observation;
}

Observation run_mylite_observation(void) {
    MyLiteRuntime runtime;
    open_mylite_runtime(runtime);
    Observation observation = run_mylite_queries(runtime.database);

    close_mylite_runtime(runtime);
    return observation;
}

void compare_observations(const Observation &expected, const Observation &actual) {
    compare_query_results(expected.direct, actual.direct);
    compare_query_result_sets(expected.direct_expressions, actual.direct_expressions);
    compare_prepared_results(expected.prepared, actual.prepared);
    compare_statement_effects(expected.effects, actual.effects);
    assert(actual.warning_count == expected.warning_count);
    compare_warning_rows(expected.warning, actual.warning);
}

void start_raw_runtime(RawRuntime &runtime) {
    runtime.root = make_temp_root("mylite-sql-comparison-raw");
    runtime.runtime_dir = runtime.root / "runtime";
    create_raw_runtime_directories(runtime.runtime_dir);
    runtime.arguments = raw_runtime_arguments(runtime.runtime_dir);
    runtime.argv = mutable_arguments(runtime.arguments);

    char *groups[] = {const_cast<char *>("server"), const_cast<char *>("embedded"), nullptr};
    assert(
        mysql_server_init(static_cast<int>(runtime.argv.size()), runtime.argv.data(), groups) == 0
    );
    runtime.initialized = true;
    assert(mysql_init(&runtime.mysql) != nullptr);
    assert(
        mysql_real_connect(&runtime.mysql, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0) !=
        nullptr
    );
    runtime.connected = true;
}

void stop_raw_runtime(RawRuntime &runtime) {
    if (runtime.connected) {
        mysql_close(&runtime.mysql);
        runtime.connected = false;
    }
    if (runtime.initialized) {
        mysql_server_end();
        runtime.initialized = false;
    }
    remove_tree(runtime.root);
}

QueryResult run_raw_query(RawRuntime &runtime, const char *sql) {
    QueryResult result = {};
    assert(mysql_query(&runtime.mysql, sql) == 0);
    result.warning_count = mysql_warning_count(&runtime.mysql);

    MYSQL_RES *raw_result = mysql_store_result(&runtime.mysql);
    assert(raw_result != nullptr);

    const unsigned column_count = mysql_num_fields(raw_result);
    const MYSQL_FIELD *fields = mysql_fetch_fields(raw_result);
    assert(fields != nullptr);
    for (unsigned i = 0; i < column_count; ++i) {
        result.names.emplace_back(fields[i].name, fields[i].name_length);
    }

    for (MYSQL_ROW row = mysql_fetch_row(raw_result); row != nullptr;
         row = mysql_fetch_row(raw_result)) {
        const unsigned long *lengths = mysql_fetch_lengths(raw_result);
        assert(lengths != nullptr);
        std::vector<Cell> cells;
        cells.reserve(column_count);
        for (unsigned i = 0; i < column_count; ++i) {
            Cell cell = {};
            if (row[i] != nullptr) {
                cell.is_null = false;
                cell.value = cell_value(row[i], lengths[i]);
            }
            cells.push_back(std::move(cell));
        }
        result.rows.push_back(std::move(cells));
    }

    mysql_free_result(raw_result);
    return result;
}

std::vector<QueryResult> run_raw_direct_expression_matrix(RawRuntime &runtime) {
    std::vector<QueryResult> results;
    results.reserve(kDirectExpressionSql.size());
    for (const char *sql : kDirectExpressionSql) {
        results.push_back(run_raw_query(runtime, sql));
    }
    return results;
}

PreparedResult run_raw_prepared_query(RawRuntime &runtime) {
    PreparedResult result = {};
    MYSQL_STMT *statement = mysql_stmt_init(&runtime.mysql);
    assert(statement != nullptr);
    assert(mysql_stmt_prepare(statement, kPreparedSql, string_length(kPreparedSql)) == 0);
    assert(mysql_stmt_param_count(statement) == kPreparedColumnCount);

    MYSQL_RES *metadata = mysql_stmt_result_metadata(statement);
    assert(metadata != nullptr);
    const unsigned column_count = mysql_num_fields(metadata);
    assert(column_count == kPreparedColumnCount);
    const MYSQL_FIELD *fields = mysql_fetch_fields(metadata);
    assert(fields != nullptr);
    for (unsigned i = 0; i < column_count; ++i) {
        result.names.emplace_back(fields[i].name, fields[i].name_length);
        result.native_types.push_back(static_cast<unsigned>(fields[i].type));
    }
    mysql_free_result(metadata);

    // NOLINTBEGIN(misc-const-correctness)
    my_bool null_value = 1;
    long long signed_input = kSignedProbeValue;
    unsigned long long unsigned_input = kUnsignedProbeValue;
    double double_input = kDoubleProbeValue;
    char text_input[] = "hello";
    unsigned long text_length = kTextProbeLength;
    // NOLINTEND(misc-const-correctness)

    std::array<MYSQL_BIND, kPreparedColumnCount> parameters = {};
    parameters[0].buffer_type = MYSQL_TYPE_NULL;
    parameters[0].is_null = &null_value;
    parameters[1].buffer_type = MYSQL_TYPE_LONGLONG;
    parameters[1].buffer = &signed_input;
    parameters[2].buffer_type = MYSQL_TYPE_LONGLONG;
    parameters[2].buffer = &unsigned_input;
    parameters[2].is_unsigned = 1;
    parameters[3].buffer_type = MYSQL_TYPE_DOUBLE;
    parameters[3].buffer = &double_input;
    parameters[4].buffer_type = MYSQL_TYPE_STRING;
    parameters[4].buffer = text_input;
    parameters[4].buffer_length = text_length;
    parameters[4].length = &text_length;

    assert(mysql_stmt_bind_param(statement, parameters.data()) == 0);
    assert(mysql_stmt_execute(statement) == 0);

    // NOLINTBEGIN(misc-const-correctness)
    my_bool missing_is_null = 0;
    long long signed_output = 0;
    unsigned long long unsigned_output = 0;
    double double_output = 0.0;
    std::array<char, kTextOutputBufferSize> text_output = {};
    unsigned long text_output_length = 0;
    my_bool text_is_null = 0;
    my_bool text_error = 0;
    // NOLINTEND(misc-const-correctness)

    std::array<MYSQL_BIND, kPreparedColumnCount> outputs = {};
    outputs[0].buffer_type = MYSQL_TYPE_NULL;
    outputs[0].is_null = &missing_is_null;
    outputs[1].buffer_type = MYSQL_TYPE_LONGLONG;
    outputs[1].buffer = &signed_output;
    outputs[2].buffer_type = MYSQL_TYPE_LONGLONG;
    outputs[2].buffer = &unsigned_output;
    outputs[2].is_unsigned = 1;
    outputs[3].buffer_type = MYSQL_TYPE_DOUBLE;
    outputs[3].buffer = &double_output;
    outputs[4].buffer_type = MYSQL_TYPE_STRING;
    outputs[4].buffer = text_output.data();
    outputs[4].buffer_length = static_cast<unsigned long>(text_output.size());
    outputs[4].length = &text_output_length;
    outputs[4].is_null = &text_is_null;
    outputs[4].error = &text_error;

    assert(mysql_stmt_bind_result(statement, outputs.data()) == 0);
    assert(mysql_stmt_fetch(statement) == 0);
    assert(mysql_stmt_fetch(statement) == MYSQL_NO_DATA);

    result.missing_is_null = missing_is_null != 0;
    result.signed_value = signed_output;
    result.unsigned_value = unsigned_output;
    result.double_value = double_output;
    assert(text_is_null == 0);
    assert(text_error == 0);
    result.text_value.assign(text_output.data(), text_output_length);

    assert(mysql_stmt_close(statement) == 0);
    return result;
}

StatementEffects run_raw_statement_effects(RawRuntime &runtime) {
    StatementEffects effects = {};
    effects.values.reserve(kStatementEffectCount);

    assert(mysql_query(&runtime.mysql, "CREATE DATABASE effect_compare") == 0);
    assert(mysql_query(&runtime.mysql, "USE effect_compare") == 0);
    assert(
        mysql_query(
            &runtime.mysql,
            "CREATE TEMPORARY TABLE effect_values ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "name VARCHAR(32),"
            "qty INT NOT NULL)"
        ) == 0
    );

    effects.values.push_back(run_raw_effect_query(
        runtime,
        "INSERT INTO effect_values(name, qty) VALUES ('alpha', 1), ('beta', 2)"
    ));
    effects.values.push_back(
        run_raw_effect_query(runtime, "UPDATE effect_values SET qty = qty + 10 WHERE name = 'beta'")
    );
    effects.values.push_back(
        run_raw_effect_query(runtime, "DELETE FROM effect_values WHERE name = 'alpha'")
    );
    effects.values.push_back(run_raw_prepared_insert(runtime));
    effects.values.push_back(run_raw_prepared_update(runtime));
    effects.values.push_back(run_raw_prepared_delete(runtime));
    return effects;
}

StatementEffect run_raw_effect_query(RawRuntime &runtime, const char *sql) {
    assert(mysql_query(&runtime.mysql, sql) == 0);
    return raw_statement_effect(runtime);
}

StatementEffect run_raw_prepared_insert(RawRuntime &runtime) {
    MYSQL_STMT *statement = mysql_stmt_init(&runtime.mysql);
    assert(statement != nullptr);
    constexpr const char *sql = "INSERT INTO effect_values(name, qty) VALUES (?, ?)";
    assert(mysql_stmt_prepare(statement, sql, string_length(sql)) == 0);

    char name[] = "gamma";
    unsigned long name_length = 5;
    int quantity = 3;
    std::array<MYSQL_BIND, 2> parameters = {};
    parameters[0].buffer_type = MYSQL_TYPE_STRING;
    parameters[0].buffer = name;
    parameters[0].buffer_length = name_length;
    parameters[0].length = &name_length;
    parameters[1].buffer_type = MYSQL_TYPE_LONG;
    parameters[1].buffer = &quantity;

    assert(mysql_stmt_bind_param(statement, parameters.data()) == 0);
    assert(mysql_stmt_execute(statement) == 0);
    const StatementEffect effect{
        static_cast<long long>(mysql_stmt_affected_rows(statement)),
        mysql_stmt_insert_id(statement),
    };
    assert(mysql_stmt_close(statement) == 0);
    return effect;
}

StatementEffect run_raw_prepared_update(RawRuntime &runtime) {
    MYSQL_STMT *statement = mysql_stmt_init(&runtime.mysql);
    assert(statement != nullptr);
    constexpr const char *sql = "UPDATE effect_values SET qty = qty + ? WHERE name = ?";
    assert(mysql_stmt_prepare(statement, sql, string_length(sql)) == 0);

    int increment = 5;
    char name[] = "gamma";
    unsigned long name_length = 5;
    std::array<MYSQL_BIND, 2> parameters = {};
    parameters[0].buffer_type = MYSQL_TYPE_LONG;
    parameters[0].buffer = &increment;
    parameters[1].buffer_type = MYSQL_TYPE_STRING;
    parameters[1].buffer = name;
    parameters[1].buffer_length = name_length;
    parameters[1].length = &name_length;

    assert(mysql_stmt_bind_param(statement, parameters.data()) == 0);
    assert(mysql_stmt_execute(statement) == 0);
    const StatementEffect effect{
        static_cast<long long>(mysql_stmt_affected_rows(statement)),
        mysql_stmt_insert_id(statement),
    };
    assert(mysql_stmt_close(statement) == 0);
    return effect;
}

StatementEffect run_raw_prepared_delete(RawRuntime &runtime) {
    MYSQL_STMT *statement = mysql_stmt_init(&runtime.mysql);
    assert(statement != nullptr);
    constexpr const char *sql = "DELETE FROM effect_values WHERE name = ?";
    assert(mysql_stmt_prepare(statement, sql, string_length(sql)) == 0);

    char name[] = "beta";
    unsigned long name_length = 4;
    std::array<MYSQL_BIND, 1> parameters = {};
    parameters[0].buffer_type = MYSQL_TYPE_STRING;
    parameters[0].buffer = name;
    parameters[0].buffer_length = name_length;
    parameters[0].length = &name_length;

    assert(mysql_stmt_bind_param(statement, parameters.data()) == 0);
    assert(mysql_stmt_execute(statement) == 0);
    const StatementEffect effect{
        static_cast<long long>(mysql_stmt_affected_rows(statement)),
        mysql_stmt_insert_id(statement),
    };
    assert(mysql_stmt_close(statement) == 0);
    return effect;
}

StatementEffect raw_statement_effect(RawRuntime &runtime) {
    return StatementEffect{
        static_cast<long long>(mysql_affected_rows(&runtime.mysql)),
        mysql_insert_id(&runtime.mysql),
    };
}

Observation run_mylite_queries(mylite_db *database) {
    Observation observation = {};

    observation.direct = run_mylite_query(database, kDirectSql);
    observation.direct_expressions = run_mylite_direct_expression_matrix(database);
    observation.prepared = run_mylite_prepared_query(database);
    observation.effects = run_mylite_statement_effects(database);
    assert(mylite_exec(database, kWarningSql, nullptr, nullptr, nullptr) == MYLITE_OK);
    observation.warning_count = mylite_warning_count(database);
    observation.warning = first_mylite_warning(database);
    return observation;
}

QueryResult run_mylite_query(mylite_db *database, const char *sql) {
    QueryResult result = {};
    assert(mylite_exec(database, sql, collect_mylite_row, &result, nullptr) == MYLITE_OK);
    result.warning_count = mylite_warning_count(database);
    return result;
}

std::vector<QueryResult> run_mylite_direct_expression_matrix(mylite_db *database) {
    std::vector<QueryResult> results;
    results.reserve(kDirectExpressionSql.size());
    for (const char *sql : kDirectExpressionSql) {
        results.push_back(run_mylite_query(database, sql));
    }
    return results;
}

PreparedResult run_mylite_prepared_query(mylite_db *database) {
    PreparedResult result = {};
    mylite_stmt *statement = nullptr;
    assert(
        mylite_prepare(database, kPreparedSql, MYLITE_NUL_TERMINATED, &statement, nullptr) ==
        MYLITE_OK
    );
    assert(statement != nullptr);
    assert(mylite_bind_parameter_count(statement) == kPreparedColumnCount);
    assert(mylite_column_count(statement) == kPreparedColumnCount);

    assert(mylite_bind_null(statement, kMissingParameter) == MYLITE_OK);
    assert(mylite_bind_int64(statement, kSignedParameter, kSignedProbeValue) == MYLITE_OK);
    assert(mylite_bind_uint64(statement, kUnsignedParameter, kUnsignedProbeValue) == MYLITE_OK);
    assert(mylite_bind_double(statement, kDoubleParameter, kDoubleProbeValue) == MYLITE_OK);
    assert(
        mylite_bind_text(
            statement,
            kTextParameter,
            kTextProbeValue,
            MYLITE_NUL_TERMINATED,
            MYLITE_STATIC
        ) == MYLITE_OK
    );

    for (unsigned i = 0; i < mylite_column_count(statement); ++i) {
        result.names.emplace_back(mylite_column_name(statement, i));
        result.native_types.push_back(mylite_column_mariadb_type(statement, i));
    }

    assert(mylite_step(statement) == MYLITE_ROW);
    result.missing_is_null = mylite_column_type(statement, 0U) == MYLITE_TYPE_NULL;
    result.signed_value = mylite_column_int64(statement, 1U);
    result.unsigned_value = mylite_column_uint64(statement, 2U);
    result.double_value = mylite_column_double(statement, 3U);
    result.text_value.assign(mylite_column_text(statement, 4U), mylite_column_bytes(statement, 4U));
    assert(mylite_step(statement) == MYLITE_DONE);
    assert(mylite_finalize(statement) == MYLITE_OK);
    return result;
}

StatementEffects run_mylite_statement_effects(mylite_db *database) {
    StatementEffects effects = {};
    effects.values.reserve(kStatementEffectCount);

    assert(
        mylite_exec(database, "CREATE DATABASE effect_compare", nullptr, nullptr, nullptr) ==
        MYLITE_OK
    );
    assert(mylite_exec(database, "USE effect_compare", nullptr, nullptr, nullptr) == MYLITE_OK);
    assert(
        mylite_exec(
            database,
            "CREATE TEMPORARY TABLE effect_values ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "name VARCHAR(32),"
            "qty INT NOT NULL)",
            nullptr,
            nullptr,
            nullptr
        ) == MYLITE_OK
    );

    effects.values.push_back(run_mylite_effect_query(
        database,
        "INSERT INTO effect_values(name, qty) VALUES ('alpha', 1), ('beta', 2)"
    ));
    effects.values.push_back(run_mylite_effect_query(
        database,
        "UPDATE effect_values SET qty = qty + 10 WHERE name = 'beta'"
    ));
    effects.values.push_back(
        run_mylite_effect_query(database, "DELETE FROM effect_values WHERE name = 'alpha'")
    );
    effects.values.push_back(run_mylite_prepared_insert(database));
    effects.values.push_back(run_mylite_prepared_update(database));
    effects.values.push_back(run_mylite_prepared_delete(database));
    return effects;
}

StatementEffect run_mylite_effect_query(mylite_db *database, const char *sql) {
    assert(mylite_exec(database, sql, nullptr, nullptr, nullptr) == MYLITE_OK);
    return mylite_statement_effect(database);
}

StatementEffect run_mylite_prepared_insert(mylite_db *database) {
    mylite_stmt *statement = nullptr;
    assert(
        mylite_prepare(
            database,
            "INSERT INTO effect_values(name, qty) VALUES (?, ?)",
            MYLITE_NUL_TERMINATED,
            &statement,
            nullptr
        ) == MYLITE_OK
    );
    assert(statement != nullptr);
    assert(
        mylite_bind_text(statement, 1U, "gamma", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK
    );
    assert(mylite_bind_int64(statement, 2U, 3) == MYLITE_OK);
    assert(mylite_step(statement) == MYLITE_DONE);
    const StatementEffect effect = mylite_statement_effect(database);
    assert(mylite_finalize(statement) == MYLITE_OK);
    return effect;
}

StatementEffect run_mylite_prepared_update(mylite_db *database) {
    mylite_stmt *statement = nullptr;
    assert(
        mylite_prepare(
            database,
            "UPDATE effect_values SET qty = qty + ? WHERE name = ?",
            MYLITE_NUL_TERMINATED,
            &statement,
            nullptr
        ) == MYLITE_OK
    );
    assert(statement != nullptr);
    assert(mylite_bind_int64(statement, 1U, 5) == MYLITE_OK);
    assert(
        mylite_bind_text(statement, 2U, "gamma", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK
    );
    assert(mylite_step(statement) == MYLITE_DONE);
    const StatementEffect effect = mylite_statement_effect(database);
    assert(mylite_finalize(statement) == MYLITE_OK);
    return effect;
}

StatementEffect run_mylite_prepared_delete(mylite_db *database) {
    mylite_stmt *statement = nullptr;
    assert(
        mylite_prepare(
            database,
            "DELETE FROM effect_values WHERE name = ?",
            MYLITE_NUL_TERMINATED,
            &statement,
            nullptr
        ) == MYLITE_OK
    );
    assert(statement != nullptr);
    assert(
        mylite_bind_text(statement, 1U, "beta", MYLITE_NUL_TERMINATED, MYLITE_STATIC) == MYLITE_OK
    );
    assert(mylite_step(statement) == MYLITE_DONE);
    const StatementEffect effect = mylite_statement_effect(database);
    assert(mylite_finalize(statement) == MYLITE_OK);
    return effect;
}

StatementEffect mylite_statement_effect(mylite_db *database) {
    return StatementEffect{mylite_changes(database), mylite_last_insert_id(database)};
}

WarningRow first_mylite_warning(mylite_db *database) {
    mylite_warning_level level = MYLITE_WARNING_NOTE;
    unsigned code = 0;
    const char *message = nullptr;
    assert(mylite_warning_count(database) >= 1U);
    assert(mylite_warning(database, 0U, &level, &code, &message) == MYLITE_OK);
    assert(message != nullptr);
    return WarningRow{mylite_warning_level_name(level), code, message};
}

void open_mylite_runtime(MyLiteRuntime &runtime) {
    runtime.root = make_temp_root("mylite-sql-comparison");
    const std::filesystem::path runtime_root = runtime.root / "runtime";
    std::filesystem::create_directories(runtime_root);

    const std::string filename = (runtime.root / "comparison.mylite").string();
    const std::string temp_directory = runtime_root.string();
    mylite_open_config config = {};
    config.size = sizeof(config);
    config.profile = MYLITE_PROFILE_DEFAULT;
    config.busy_timeout_ms = 0;
    config.durability = MYLITE_DURABILITY_FULL;
    config.temp_directory = temp_directory.c_str();
    assert(
        mylite_open_v2(
            filename.c_str(),
            &runtime.database,
            MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE,
            &config
        ) == MYLITE_OK
    );
}

void close_mylite_runtime(MyLiteRuntime &runtime) {
    assert(mylite_close(runtime.database) == MYLITE_OK);
    runtime.database = nullptr;
    remove_tree(runtime.root);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
int collect_mylite_row(void *ctx, int column_count, char **values, char **column_names) {
    QueryResult &result = *static_cast<QueryResult *>(ctx);
    assert(column_count >= 0);
    if (result.names.empty()) {
        for (int i = 0; i < column_count; ++i) {
            result.names.emplace_back(column_names[i]);
        }
    }

    std::vector<Cell> row;
    row.reserve(static_cast<std::size_t>(column_count));
    for (int i = 0; i < column_count; ++i) {
        Cell cell = {};
        if (values[i] != nullptr) {
            cell.is_null = false;
            cell.value = values[i];
        }
        row.push_back(std::move(cell));
    }
    result.rows.push_back(std::move(row));
    return 0;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

void compare_query_result_sets(
    const std::vector<QueryResult> &expected,
    const std::vector<QueryResult> &actual
) {
    assert(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        compare_query_results(expected[i], actual[i]);
    }
}

void compare_query_results(const QueryResult &expected, const QueryResult &actual) {
    assert(actual.warning_count == expected.warning_count);
    assert(actual.names == expected.names);
    assert(actual.rows.size() == expected.rows.size());
    for (std::size_t row = 0; row < expected.rows.size(); ++row) {
        assert(actual.rows[row].size() == expected.rows[row].size());
        for (std::size_t column = 0; column < expected.rows[row].size(); ++column) {
            assert(actual.rows[row][column].is_null == expected.rows[row][column].is_null);
            assert(actual.rows[row][column].value == expected.rows[row][column].value);
        }
    }
}

void compare_prepared_results(const PreparedResult &expected, const PreparedResult &actual) {
    assert(actual.names == expected.names);
    assert(actual.native_types == expected.native_types);
    assert(actual.missing_is_null == expected.missing_is_null);
    assert(actual.signed_value == expected.signed_value);
    assert(actual.unsigned_value == expected.unsigned_value);
    assert(std::fabs(actual.double_value - expected.double_value) < kDoubleTolerance);
    assert(actual.text_value == expected.text_value);
}

void compare_statement_effects(const StatementEffects &expected, const StatementEffects &actual) {
    assert(actual.values.size() == expected.values.size());
    assert(actual.values.size() == kStatementEffectCount);
    for (std::size_t i = 0; i < expected.values.size(); ++i) {
        assert(actual.values[i].affected_rows == expected.values[i].affected_rows);
        assert(actual.values[i].last_insert_id == expected.values[i].last_insert_id);
    }
}

void compare_warning_rows(const WarningRow &expected, const WarningRow &actual) {
    assert(actual.level == expected.level);
    assert(actual.code == expected.code);
    assert(actual.message == expected.message);
}

WarningRow first_raw_warning(RawRuntime &runtime) {
    const QueryResult warnings = run_raw_query(runtime, "SHOW WARNINGS");
    assert(!warnings.rows.empty());
    assert(warnings.rows[0].size() == kWarningColumnCount);
    assert(!warnings.rows[0][0].is_null);
    assert(!warnings.rows[0][1].is_null);
    assert(!warnings.rows[0][2].is_null);
    return WarningRow{
        warnings.rows[0][0].value,
        static_cast<unsigned>(std::stoul(warnings.rows[0][1].value)),
        warnings.rows[0][2].value,
    };
}

std::filesystem::path make_temp_root(const char *prefix) {
    std::string template_path = std::string("/tmp/") + prefix + ".XXXXXX";
    std::vector<char> buffer(template_path.begin(), template_path.end());
    buffer.push_back('\0');
    const char *path = mkdtemp(buffer.data());
    assert(path != nullptr);
    return std::filesystem::path(path);
}

std::vector<std::string> raw_runtime_arguments(const std::filesystem::path &runtime_dir) {
    std::vector<std::string> arguments = {
        "mylite-comparison",
        "--no-defaults",
        "--datadir=" + (runtime_dir / "data").string(),
        "--tmpdir=" + (runtime_dir / "tmp").string(),
        "--plugin-dir=" + (runtime_dir / "plugins").string(),
        "--skip-grant-tables",
        "--skip-log-bin",
        "--skip-networking",
        "--default-storage-engine=Aria",
        std::string("--lc-messages-dir=") + MYLITE_MARIADB_MESSAGES_DIR,
        std::string("--character-sets-dir=") + MYLITE_MARIADB_CHARSETS_DIR,
    };
#if MYLITE_MARIADB_HAS_INNOBASE
    arguments.push_back("--innodb=OFF");
#endif
#if MYLITE_MARIADB_HAS_PERFSCHEMA
    arguments.push_back("--performance-schema=OFF");
#endif
    return arguments;
}

std::vector<char *> mutable_arguments(std::vector<std::string> &arguments) {
    std::vector<char *> argv;
    argv.reserve(arguments.size());
    for (std::string &argument : arguments) {
        argv.push_back(argument.data());
    }
    return argv;
}

void create_raw_runtime_directories(const std::filesystem::path &runtime_dir) {
    std::filesystem::create_directories(runtime_dir / "data");
    std::filesystem::create_directories(runtime_dir / "tmp");
    std::filesystem::create_directories(runtime_dir / "plugins");
}

void remove_tree(const std::filesystem::path &path) {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

std::string cell_value(const char *value, unsigned long length) {
    return std::string(value, static_cast<std::size_t>(length));
}

std::string mylite_warning_level_name(mylite_warning_level level) {
    switch (level) {
    case MYLITE_WARNING_NOTE:
        return "Note";
    case MYLITE_WARNING_WARNING:
        return "Warning";
    case MYLITE_WARNING_ERROR:
        return "Error";
    }
    return "Warning";
}

unsigned long string_length(const char *value) {
    return static_cast<unsigned long>(std::strlen(value));
}

} // namespace

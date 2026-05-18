#include <mylite/storage.h>

#include <mysql.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef MYLITE_MARIADB_MESSAGES_DIR
#  define MYLITE_MARIADB_MESSAGES_DIR ""
#endif

#ifndef MYLITE_MARIADB_CHARSETS_DIR
#  define MYLITE_MARIADB_CHARSETS_DIR ""
#endif

#ifndef MYLITE_MARIADB_HAS_INNOBASE
#  define MYLITE_MARIADB_HAS_INNOBASE 1
#endif

#ifndef MYLITE_MARIADB_HAS_PERFSCHEMA
#  define MYLITE_MARIADB_HAS_PERFSCHEMA 1
#endif

namespace {

struct RawRuntime {
    std::filesystem::path root;
    std::filesystem::path runtime_dir;
    std::filesystem::path primary_file;
    std::vector<std::string> arguments;
    std::vector<char *> argv;
    MYSQL mysql = {};
    bool initialized = false;
    bool connected = false;
};

void run_handler_savepoint_test(void);
void start_raw_runtime(RawRuntime &runtime);
void stop_raw_runtime(RawRuntime &runtime);
void run_savepoint_queries(MYSQL &mysql);
void assert_query_succeeds(MYSQL &mysql, const char *sql);
void assert_single_count(MYSQL &mysql, const char *sql, int expected);
std::filesystem::path make_temp_root(void);
void create_runtime_directories(const std::filesystem::path &runtime_dir);
std::vector<std::string> raw_runtime_arguments(
    const std::filesystem::path &runtime_dir,
    const std::filesystem::path &primary_file
);
std::vector<char *> mutable_arguments(std::vector<std::string> &arguments);

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() noexcept {
    run_handler_savepoint_test();
    return 0;
}

namespace {

void run_handler_savepoint_test(void) {
    RawRuntime runtime;
    start_raw_runtime(runtime);
    run_savepoint_queries(runtime.mysql);
    stop_raw_runtime(runtime);
}

void start_raw_runtime(RawRuntime &runtime) {
    runtime.root = make_temp_root();
    runtime.runtime_dir = runtime.root / "runtime";
    runtime.primary_file = runtime.root / "handler-savepoint.mylite";
    create_runtime_directories(runtime.runtime_dir);
    assert(
        mylite_storage_create_empty(runtime.primary_file.string().c_str()) == MYLITE_STORAGE_OK
    );

    runtime.arguments = raw_runtime_arguments(runtime.runtime_dir, runtime.primary_file);
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

    assert_query_succeeds(runtime.mysql, "SET SESSION sql_mode=''");
    assert_query_succeeds(runtime.mysql, "SET SESSION default_storage_engine=MYLITE");
    assert_query_succeeds(runtime.mysql, "SET SESSION enforce_storage_engine=MYLITE");
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
    std::filesystem::remove_all(runtime.root);
}

void run_savepoint_queries(MYSQL &mysql) {
    assert_query_succeeds(mysql, "CREATE DATABASE raw_tx");
    assert_query_succeeds(mysql, "USE raw_tx");
    assert_query_succeeds(
        mysql,
        "CREATE TABLE raw_posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL UNIQUE"
        ") ENGINE=InnoDB"
    );
    assert_query_succeeds(
        mysql,
        "CREATE TABLE raw_memory_posts ("
        "id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL UNIQUE KEY"
        ") ENGINE=MEMORY"
    );

    assert_query_succeeds(mysql, "BEGIN");
    assert_query_succeeds(mysql, "INSERT INTO raw_posts VALUES (1, 'before')");
    assert_query_succeeds(mysql, "SAVEPOINT first_sp");
    assert_query_succeeds(mysql, "INSERT INTO raw_posts VALUES (2, 'rolled')");
    assert_query_succeeds(mysql, "ROLLBACK TO SAVEPOINT first_sp");
    assert_single_count(mysql, "SELECT COUNT(*) FROM raw_posts WHERE title = 'before'", 1);
    assert_single_count(mysql, "SELECT COUNT(*) FROM raw_posts WHERE title = 'rolled'", 0);
    assert_query_succeeds(mysql, "INSERT INTO raw_posts VALUES (3, 'kept-after-rollback')");
    assert_query_succeeds(mysql, "RELEASE SAVEPOINT first_sp");
    assert_query_succeeds(mysql, "COMMIT");
    assert_single_count(
        mysql,
        "SELECT COUNT(*) FROM raw_posts "
        "WHERE title IN ('before', 'kept-after-rollback')",
        2
    );
    assert_single_count(mysql, "SELECT COUNT(*) FROM raw_posts WHERE title = 'rolled'", 0);

    assert_query_succeeds(mysql, "BEGIN");
    assert_query_succeeds(mysql, "INSERT INTO raw_posts VALUES (10, 'dup-before')");
    assert_query_succeeds(mysql, "SAVEPOINT same_sp");
    assert_query_succeeds(mysql, "INSERT INTO raw_posts VALUES (11, 'dup-kept')");
    assert_query_succeeds(mysql, "SAVEPOINT other_sp");
    assert_query_succeeds(mysql, "INSERT INTO raw_posts VALUES (12, 'dup-rolled-by-other')");
    assert_query_succeeds(mysql, "SAVEPOINT same_sp");
    assert_query_succeeds(mysql, "INSERT INTO raw_posts VALUES (13, 'dup-rolled-latest')");
    assert_query_succeeds(mysql, "ROLLBACK TO SAVEPOINT other_sp");
    assert_query_succeeds(mysql, "RELEASE SAVEPOINT other_sp");
    assert_query_succeeds(mysql, "COMMIT");
    assert_single_count(
        mysql,
        "SELECT COUNT(*) FROM raw_posts WHERE title IN ('dup-before', 'dup-kept')",
        2
    );
    assert_single_count(
        mysql,
        "SELECT COUNT(*) FROM raw_posts "
        "WHERE title IN ('dup-rolled-by-other', 'dup-rolled-latest')",
        0
    );

    assert_query_succeeds(mysql, "BEGIN");
    assert_query_succeeds(mysql, "INSERT INTO raw_posts VALUES (20, 'full-before')");
    assert_query_succeeds(mysql, "SAVEPOINT full_sp");
    assert_query_succeeds(mysql, "INSERT INTO raw_posts VALUES (21, 'full-after')");
    assert_query_succeeds(mysql, "ROLLBACK");
    assert_single_count(
        mysql,
        "SELECT COUNT(*) FROM raw_posts WHERE title IN ('full-before', 'full-after')",
        0
    );

    assert_query_succeeds(mysql, "BEGIN");
    assert_query_succeeds(mysql, "INSERT INTO raw_memory_posts (title) VALUES ('memory-before')");
    assert_query_succeeds(mysql, "SAVEPOINT memory_sp");
    assert_query_succeeds(mysql, "INSERT INTO raw_memory_posts (title) VALUES ('memory-rolled')");
    assert_query_succeeds(mysql, "ROLLBACK TO SAVEPOINT memory_sp");
    assert_single_count(
        mysql,
        "SELECT COUNT(*) FROM raw_memory_posts WHERE title = 'memory-before'",
        1
    );
    assert_single_count(
        mysql,
        "SELECT COUNT(*) FROM raw_memory_posts WHERE title = 'memory-rolled'",
        0
    );
    assert_query_succeeds(mysql, "INSERT INTO raw_memory_posts (title) VALUES ('memory-after')");
    assert_single_count(
        mysql,
        "SELECT COUNT(*) FROM raw_memory_posts WHERE id = 3 AND title = 'memory-after'",
        1
    );
    assert_query_succeeds(mysql, "RELEASE SAVEPOINT memory_sp");
    assert_query_succeeds(mysql, "COMMIT");

    assert_query_succeeds(mysql, "BEGIN");
    assert_query_succeeds(mysql, "INSERT INTO raw_memory_posts (title) VALUES ('memory-full')");
    assert_query_succeeds(mysql, "SAVEPOINT memory_full_sp");
    assert_query_succeeds(mysql, "INSERT INTO raw_memory_posts (title) VALUES ('memory-full-after')");
    assert_query_succeeds(mysql, "ROLLBACK");
    assert_single_count(
        mysql,
        "SELECT COUNT(*) FROM raw_memory_posts "
        "WHERE title IN ('memory-full', 'memory-full-after')",
        0
    );
}

void assert_query_succeeds(MYSQL &mysql, const char *sql) {
    if (mysql_query(&mysql, sql) != 0) {
        std::fprintf(stderr, "SQL failed: %s\n%s\n", sql, mysql_error(&mysql));
    }
    assert(mysql_errno(&mysql) == 0U);
    MYSQL_RES *result = mysql_store_result(&mysql);
    if (result != nullptr) {
        mysql_free_result(result);
    } else {
        assert(mysql_field_count(&mysql) == 0U);
    }
}

void assert_single_count(MYSQL &mysql, const char *sql, int expected) {
    if (mysql_query(&mysql, sql) != 0) {
        std::fprintf(stderr, "SQL failed: %s\n%s\n", sql, mysql_error(&mysql));
    }
    assert(mysql_errno(&mysql) == 0U);

    MYSQL_RES *result = mysql_store_result(&mysql);
    assert(result != nullptr);
    assert(mysql_num_fields(result) == 1U);
    MYSQL_ROW row = mysql_fetch_row(result);
    assert(row != nullptr);
    assert(row[0] != nullptr);
    const int actual = std::atoi(row[0]);
    if (actual != expected) {
        std::fprintf(stderr, "unexpected count for %s: got %d, expected %d\n", sql, actual, expected);
    }
    assert(actual == expected);
    assert(mysql_fetch_row(result) == nullptr);
    mysql_free_result(result);
}

std::filesystem::path make_temp_root(void) {
    std::string template_path = "/tmp/mylite-handler-savepoint.XXXXXX";
    std::vector<char> buffer(template_path.begin(), template_path.end());
    buffer.push_back('\0');
    const char *path = mkdtemp(buffer.data());
    assert(path != nullptr);
    return std::filesystem::path(path);
}

void create_runtime_directories(const std::filesystem::path &runtime_dir) {
    std::filesystem::create_directories(runtime_dir / "data");
    std::filesystem::create_directories(runtime_dir / "tmp");
    std::filesystem::create_directories(runtime_dir / "plugins");
}

std::vector<std::string> raw_runtime_arguments(
    const std::filesystem::path &runtime_dir,
    const std::filesystem::path &primary_file
) {
    std::vector<std::string> arguments = {
        "mylite-handler-savepoint",
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
        "--mylite-primary-file=" + primary_file.string(),
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

} // namespace

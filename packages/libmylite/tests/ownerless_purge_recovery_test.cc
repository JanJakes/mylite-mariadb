#include <mylite/mylite.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" int mylite_ownerless_innodb_test_choose_startup_rseg_history(
    int native_valid,
    std::uint32_t native_len,
    std::uint32_t native_first_page,
    std::uint16_t native_first_offset,
    std::uint32_t native_last_page,
    std::uint16_t native_last_offset,
    std::uint64_t native_page_lsn,
    std::uint32_t retained_len,
    std::uint32_t retained_first_page,
    std::uint16_t retained_first_offset,
    std::uint32_t retained_last_page,
    std::uint16_t retained_last_offset,
    std::uint64_t retained_page_lsn,
    int retained_has_pair
);

extern "C" int mylite_ownerless_innodb_test_required_undo_commit_matches(
    std::uint64_t required_commit_lsn,
    int read_result,
    std::uint64_t commit_lsn
);

extern "C" std::uint64_t mylite_ownerless_innodb_test_startup_lsn_limit_rejections(void);

extern "C" int mylite_ownerless_innodb_test_purge_history_addr_is_valid(
    std::uint32_t page_no,
    std::uint16_t byte_offset,
    std::uint32_t free_limit,
    std::uint32_t physical_size
);

namespace {

constexpr unsigned k_error_during_commit = 1180;
constexpr std::uint32_t k_fil_null = UINT32_MAX;
constexpr int k_lock_ok = 0;
constexpr int k_lock_unavailable = 1;
constexpr int k_refresh_native = 1;
constexpr int k_refresh_retained = 2;
constexpr int k_refresh_error = 3;

struct TestPaths {
    std::string root;
    std::string runtime;
    std::string database;
};

TestPaths make_paths(const char *name) {
    std::string root = "/tmp/mylite-purge-recovery-XXXXXX";
    assert(::mkdtemp(root.data()) != nullptr);
    TestPaths paths{
        root,
        root + "/runtime",
        root + "/" + name + ".mylite",
    };
    assert(::mkdir(paths.runtime.c_str(), 0700) == 0);
    return paths;
}

mylite_db *open_database(const TestPaths &paths, unsigned flags) {
    const mylite_open_config config{
        sizeof(mylite_open_config),
        MYLITE_PROFILE_DEFAULT,
        0,
        MYLITE_DURABILITY_FULL,
        paths.runtime.c_str(),
        0,
    };

    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        mylite_db *db = nullptr;
        const int result = mylite_open(paths.database.c_str(), &db, flags, &config);
        if (result == MYLITE_OK) {
            assert(db != nullptr);
            return db;
        }
        assert(db == nullptr);
        if (result != MYLITE_BUSY) {
            std::fprintf(
                stderr,
                "mylite_open failed: result=%d path=%s\n",
                result,
                paths.database.c_str()
            );
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::fprintf(stderr, "mylite_open remained busy: path=%s\n", paths.database.c_str());
    std::abort();
}

void exec_ok(mylite_db *db, const char *sql) {
    char *message = nullptr;
    const int result = mylite_exec(db, sql, nullptr, nullptr, &message);
    if (result != MYLITE_OK) {
        std::fprintf(
            stderr,
            "SQL failed: result=%d errno=%u sql=%s message=%s\n",
            result,
            mylite_mariadb_errno(db),
            sql,
            message != nullptr ? message : mylite_errmsg(db)
        );
        mylite_free(message);
        std::abort();
    }
    assert(message == nullptr);
}

void write_signal(int fd) {
    const char value = '1';
    assert(::write(fd, &value, sizeof(value)) == sizeof(value));
}

void wait_for_signal(int fd) {
    char value = 0;
    assert(::read(fd, &value, sizeof(value)) == sizeof(value));
    assert(value == '1');
}

struct UnsignedQueryResult {
    unsigned long long value = 0;
    unsigned row_count = 0;
};

int capture_unsigned(void *context, int column_count, char **values, char **) {
    assert(column_count == 1);
    assert(values != nullptr && values[0] != nullptr);
    auto *result = static_cast<UnsignedQueryResult *>(context);
    char *end = nullptr;
    result->value = std::strtoull(values[0], &end, 10);
    assert(end != values[0] && *end == '\0');
    ++result->row_count;
    return 0;
}

unsigned long long query_unsigned(mylite_db *db, const char *sql) {
    UnsignedQueryResult query_result;
    char *message = nullptr;
    const int result = mylite_exec(db, sql, capture_unsigned, &query_result, &message);
    if (result != MYLITE_OK) {
        std::fprintf(
            stderr,
            "query failed: result=%d errno=%u sql=%s message=%s\n",
            result,
            mylite_mariadb_errno(db),
            sql,
            message != nullptr ? message : mylite_errmsg(db)
        );
        mylite_free(message);
        std::abort();
    }
    assert(message == nullptr);
    assert(query_result.row_count == 1);
    return query_result.value;
}

unsigned long long query_history_length(mylite_db *db) {
    return query_unsigned(
        db,
        "SELECT COUNT FROM information_schema.INNODB_METRICS "
        "WHERE NAME = 'trx_rseg_history_len'"
    );
}

void assert_purge_history_quiescent(mylite_db *db) {
    unsigned long long previous = query_history_length(db);
    unsigned stable_rounds = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const unsigned long long current = query_history_length(db);
        assert(current <= previous);
        if (current == previous) {
            if (++stable_rounds == 5) {
                return;
            }
        } else {
            stable_rounds = 0;
        }
        previous = current;
    }
    std::abort();
}

struct CheckTableResult {
    unsigned row_count = 0;
    bool okay = false;
};

int capture_check_table(void *context, int column_count, char **values, char **) {
    assert(column_count == 4);
    assert(values != nullptr && values[2] != nullptr && values[3] != nullptr);
    auto *result = static_cast<CheckTableResult *>(context);
    ++result->row_count;
    if (std::strcmp(values[2], "status") == 0 && std::strcmp(values[3], "OK") == 0) {
        result->okay = true;
    }
    return 0;
}

void assert_table_integrity(mylite_db *db) {
    CheckTableResult check_result;
    char *message = nullptr;
    const int result = mylite_exec(
        db,
        "CHECK TABLE app.items EXTENDED",
        capture_check_table,
        &check_result,
        &message
    );
    if (result != MYLITE_OK) {
        std::fprintf(
            stderr,
            "CHECK TABLE failed: result=%d errno=%u message=%s\n",
            result,
            mylite_mariadb_errno(db),
            message != nullptr ? message : mylite_errmsg(db)
        );
        mylite_free(message);
        std::abort();
    }
    assert(message == nullptr);
    assert(check_result.row_count != 0);
    assert(check_result.okay);
}

int choose_startup_rseg_history(
    int native_valid,
    std::uint32_t native_len,
    std::uint32_t native_first_page,
    std::uint16_t native_first_offset,
    std::uint32_t native_last_page,
    std::uint16_t native_last_offset,
    std::uint64_t native_page_lsn,
    std::uint32_t retained_len,
    std::uint32_t retained_first_page,
    std::uint16_t retained_first_offset,
    std::uint32_t retained_last_page,
    std::uint16_t retained_last_offset,
    std::uint64_t retained_page_lsn,
    int retained_has_pair
) {
    return mylite_ownerless_innodb_test_choose_startup_rseg_history(
        native_valid,
        native_len,
        native_first_page,
        native_first_offset,
        native_last_page,
        native_last_offset,
        native_page_lsn,
        retained_len,
        retained_first_page,
        retained_first_offset,
        retained_last_page,
        retained_last_offset,
        retained_page_lsn,
        retained_has_pair
    );
}

void run_startup_selection_contract() {
    assert(
        choose_startup_rseg_history(
            1,
            0,
            k_fil_null,
            0,
            k_fil_null,
            0,
            10,
            1,
            11,
            120,
            11,
            120,
            20,
            1
        ) == k_refresh_retained
    );

    assert(mylite_ownerless_innodb_test_purge_history_addr_is_valid(k_fil_null, 0, 100, 16384));
    assert(mylite_ownerless_innodb_test_purge_history_addr_is_valid(11, 120, 100, 16384));
    assert(!mylite_ownerless_innodb_test_purge_history_addr_is_valid(100, 120, 100, 16384));
    assert(!mylite_ownerless_innodb_test_purge_history_addr_is_valid(11, 0, 100, 16384));
    assert(!mylite_ownerless_innodb_test_purge_history_addr_is_valid(11, 16380, 100, 16384));
    assert(
        choose_startup_rseg_history(1, 1, 11, 120, 11, 120, 10, 2, 12, 120, 11, 120, 20, 1) ==
        k_refresh_retained
    );
    assert(
        choose_startup_rseg_history(1, 2, 12, 120, 11, 120, 30, 3, 13, 120, 11, 120, 20, 1) ==
        k_refresh_native
    );
    assert(
        choose_startup_rseg_history(1, 1, 11, 120, 11, 120, 10, 1, 11, 120, 11, 120, 20, 0) ==
        k_refresh_retained
    );
    assert(
        choose_startup_rseg_history(1, 1, 11, 120, 11, 120, 10, 2, 12, 120, 11, 120, 20, 0) ==
        k_refresh_error
    );
    assert(
        choose_startup_rseg_history(
            0,
            0,
            k_fil_null,
            0,
            k_fil_null,
            0,
            0,
            1,
            11,
            120,
            11,
            120,
            20,
            1
        ) == k_refresh_retained
    );
    assert(
        choose_startup_rseg_history(
            0,
            0,
            k_fil_null,
            0,
            k_fil_null,
            0,
            0,
            1,
            11,
            120,
            11,
            120,
            20,
            0
        ) == k_refresh_error
    );
    assert(
        choose_startup_rseg_history(
            1,
            1,
            11,
            120,
            11,
            120,
            10,
            0,
            k_fil_null,
            0,
            k_fil_null,
            0,
            20,
            1
        ) == k_refresh_retained
    );

    assert(mylite_ownerless_innodb_test_required_undo_commit_matches(20, k_lock_ok, 20));
    assert(!mylite_ownerless_innodb_test_required_undo_commit_matches(20, k_lock_ok, 19));
    assert(!mylite_ownerless_innodb_test_required_undo_commit_matches(20, k_lock_unavailable, 20));
}

[[noreturn]] void run_faulting_child(
    const TestPaths &paths,
    const char *fault_name,
    bool retain_first_history,
    bool expect_commit_error
) {
    mylite_db *bootstrap = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    exec_ok(bootstrap, "CREATE DATABASE app");
    exec_ok(
        bootstrap,
        "CREATE TABLE app.items (id INT NOT NULL PRIMARY KEY, value INT NOT NULL, "
        "payload VARBINARY(2048) NOT NULL) ENGINE=InnoDB"
    );
    if (retain_first_history) {
        exec_ok(bootstrap, "INSERT INTO app.items VALUES (1,10,REPEAT('a',2048))");
    }
    assert(mylite_close(bootstrap) == MYLITE_OK);

    int ready_pipe[2] = {-1, -1};
    int release_pipe[2] = {-1, -1};
    pid_t reader_child = -1;
    if (retain_first_history) {
        assert(::pipe(ready_pipe) == 0);
        assert(::pipe(release_pipe) == 0);
        reader_child = ::fork();
        assert(reader_child >= 0);
        if (reader_child == 0) {
            assert(::close(ready_pipe[0]) == 0);
            assert(::close(release_pipe[1]) == 0);
            mylite_db *reader =
                open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
            exec_ok(reader, "START TRANSACTION WITH CONSISTENT SNAPSHOT");
            assert(query_unsigned(reader, "SELECT value FROM app.items WHERE id = 1") == 10);
            write_signal(ready_pipe[1]);
            wait_for_signal(release_pipe[0]);
            assert(mylite_close(reader) == MYLITE_OK);
            _exit(0);
        }
        assert(::close(ready_pipe[1]) == 0);
        assert(::close(release_pipe[0]) == 0);
        wait_for_signal(ready_pipe[0]);
        assert(::close(ready_pipe[0]) == 0);
    }

    if (retain_first_history) {
        const pid_t history_writer = ::fork();
        assert(history_writer >= 0);
        if (history_writer == 0) {
            mylite_db *history_db =
                open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
            exec_ok(history_db, "UPDATE app.items SET value = 11, payload = REPEAT('b', 2048)");
            assert(mylite_close(history_db) == MYLITE_OK);
            _exit(0);
        }
        int history_status = 0;
        assert(::waitpid(history_writer, &history_status, 0) == history_writer);
        assert(WIFEXITED(history_status) && WEXITSTATUS(history_status) == 0);
    }

    mylite_db *writer = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    char *message = nullptr;
    assert(::setenv("MYLITE_OWNERLESS_TEST_FAULT", fault_name, 1) == 0);
    const int result = mylite_exec(
        writer,
        "INSERT INTO app.items VALUES (1000, 20, REPEAT('c', 2048))",
        nullptr,
        nullptr,
        &message
    );
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FAULT") == 0);
    assert(result != MYLITE_OK);
    assert(mylite_errcode(writer) == result);
    assert(std::strcmp(mylite_sqlstate(writer), "HY000") == 0);
    assert(message != nullptr);
    if (expect_commit_error) {
        assert(result == MYLITE_ERROR);
        assert(mylite_mariadb_errno(writer) == k_error_during_commit);
    }
    mylite_free(message);

    if (reader_child > 0) {
        write_signal(release_pipe[1]);
        assert(::close(release_pipe[1]) == 0);
        int reader_status = 0;
        assert(::waitpid(reader_child, &reader_status, 0) == reader_child);
        assert(WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0);
    }
    const int writer_close_result = mylite_close(writer);
    assert(writer_close_result == MYLITE_OK);
    _exit(0);
}

void run_fault_case(
    const char *name,
    const char *fault_name,
    bool retain_first_history,
    bool expect_commit_error
) {
    const TestPaths paths = make_paths(name);
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        run_faulting_child(paths, fault_name, retain_first_history, expect_commit_error);
    }

    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(mylite_ownerless_innodb_test_startup_lsn_limit_rejections() == 0);
    assert_purge_history_quiescent(db);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.items WHERE id = 1000") == 0);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.items WHERE id = 1") ==
        (retain_first_history ? 1ULL : 0ULL)
    );
    if (retain_first_history) {
        assert(query_unsigned(db, "SELECT value FROM app.items WHERE id = 1") == 11);
        assert(query_unsigned(db, "SELECT LENGTH(payload) FROM app.items WHERE id = 1") == 2048);
    }
    assert_table_integrity(db);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.items WHERE id = 1000") == 0);
    assert(
        query_unsigned(db, "SELECT COUNT(*) FROM app.items WHERE id = 1") ==
        (retain_first_history ? 1ULL : 0ULL)
    );
    exec_ok(db, "INSERT INTO app.items VALUES (1001, 30, REPEAT('d', 2048))");
    exec_ok(db, "UPDATE app.items SET value = value + 1 WHERE id = 1001");
    assert(query_unsigned(db, "SELECT value FROM app.items WHERE id = 1001") == 31);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(mylite_ownerless_innodb_test_startup_lsn_limit_rejections() == 0);
    assert_purge_history_quiescent(db);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.items WHERE id = 1000") == 0);
    assert(query_unsigned(db, "SELECT value FROM app.items WHERE id = 1001") == 31);
    assert_table_integrity(db);
    assert(mylite_close(db) == MYLITE_OK);
    assert(std::filesystem::remove_all(paths.root) > 0);
}

void run_history_pair_roundtrip() {
    const TestPaths paths = make_paths("history-pair-roundtrip");
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.items (id INT NOT NULL PRIMARY KEY, value INT NOT NULL) ENGINE=InnoDB"
    );
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "INSERT INTO app.items VALUES (1, 10)");
    assert(mylite_close(db) == MYLITE_OK);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert_purge_history_quiescent(db);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.items WHERE id = 1") == 1);
    assert(mylite_close(db) == MYLITE_OK);
    assert(std::filesystem::remove_all(paths.root) > 0);
}

void run_isolated_case(const char *executable, const char *case_name) {
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        ::execl(executable, executable, case_name, nullptr);
        _exit(127);
    }

    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

} // namespace

int main(int argc, char **argv) {
    run_startup_selection_contract();
    if (argc == 2 && std::strcmp(argv[1], "history-pair-roundtrip") == 0) {
        run_history_pair_roundtrip();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "rseg-refresh") == 0) {
        run_fault_case("rseg-refresh", "purge-history-rseg-refresh-error", false, true);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "first-history-refresh") == 0) {
        run_fault_case("first-history-refresh", "purge-history-first-refresh-error", true, true);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "undo-space-refresh") == 0) {
        run_fault_case("undo-space-refresh", "undo-seg-space-refresh-error", false, false);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "undo-rseg-unavailable") == 0) {
        run_fault_case("undo-rseg-unavailable", "undo-seg-rseg-refresh-unavailable", false, false);
        return 0;
    }

    assert(argc == 1);
    run_isolated_case(argv[0], "history-pair-roundtrip");
    run_isolated_case(argv[0], "rseg-refresh");
    run_isolated_case(argv[0], "first-history-refresh");
    run_isolated_case(argv[0], "undo-space-refresh");
    run_isolated_case(argv[0], "undo-rseg-unavailable");
    return 0;
}

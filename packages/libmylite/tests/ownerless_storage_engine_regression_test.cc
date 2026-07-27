#include <mylite/mylite.h>

#include "mylite_ownerless_innodb_lock_hooks.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" void mysql_thread_end();

namespace {

constexpr unsigned k_lock_wait_timeout = 1205;
constexpr unsigned k_lock_table_full = 1206;
constexpr unsigned k_lock_deadlock = 1213;
constexpr unsigned k_storage_engine_error = 1030;
constexpr unsigned k_cant_create_table = 1005;
constexpr unsigned k_autoinc_read_failed = 1467;
constexpr std::uint64_t k_shm_segment_table_offset = 56;
constexpr std::uint64_t k_shm_segment_count_offset = 60;
constexpr std::uint64_t k_shm_segment_descriptor_size = 32;
constexpr std::uint64_t k_shm_segment_type_offset = 0;
constexpr std::uint64_t k_shm_segment_data_offset = 8;
constexpr std::uint32_t k_innodb_lock_segment_type = 6;
constexpr std::uint32_t k_page_write_lock_segment_type = 10;
constexpr std::uint64_t k_innodb_lock_waiting_count_offset = 64;
constexpr std::uint64_t k_innodb_lock_header_size = 96;
constexpr std::uint64_t k_innodb_lock_slot_state_offset = 12;
constexpr std::uint64_t k_innodb_lock_slot_page_no_offset = 60;
constexpr std::uint32_t k_innodb_lock_state_active = 1;
constexpr std::uint32_t k_space_transaction_read_page_no = UINT32_MAX - 2U;

struct TestPaths {
    std::string root;
    std::string runtime;
    std::string database;
};

struct QueryResult {
    unsigned long long value = 0;
    unsigned rows = 0;
};

TestPaths make_paths(const char *name) {
    std::string root = "/tmp/mylite-ownerless-storage-XXXXXX";
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

    std::fprintf(stderr, "mylite_open remained busy: %s\n", paths.database.c_str());
    std::abort();
}

void exec_ok(mylite_db *db, const std::string &sql) {
    char *message = nullptr;
    const int result = mylite_exec(db, sql.c_str(), nullptr, nullptr, &message);
    if (result != MYLITE_OK) {
        std::fprintf(
            stderr,
            "SQL failed: result=%d errno=%u sql=%s message=%s\n",
            result,
            mylite_mariadb_errno(db),
            sql.c_str(),
            message != nullptr ? message : mylite_errmsg(db)
        );
        mylite_free(message);
        std::abort();
    }
    assert(message == nullptr);
}

unsigned exec_error(mylite_db *db, const std::string &sql) {
    char *message = nullptr;
    const int result = mylite_exec(db, sql.c_str(), nullptr, nullptr, &message);
    if (result == MYLITE_OK) {
        std::fprintf(stderr, "SQL unexpectedly succeeded: %s\n", sql.c_str());
        std::abort();
    }
    const unsigned error = mylite_mariadb_errno(db);
    assert(message != nullptr);
    mylite_free(message);
    return error;
}

int capture_unsigned(void *context, int columns, char **values, char **) {
    assert(columns == 1);
    assert(values != nullptr && values[0] != nullptr);
    auto *result = static_cast<QueryResult *>(context);
    char *end = nullptr;
    result->value = std::strtoull(values[0], &end, 10);
    assert(end != values[0] && *end == '\0');
    ++result->rows;
    return 0;
}

unsigned long long query_unsigned(mylite_db *db, const std::string &sql) {
    QueryResult query;
    char *message = nullptr;
    const int result = mylite_exec(db, sql.c_str(), capture_unsigned, &query, &message);
    if (result != MYLITE_OK) {
        std::fprintf(
            stderr,
            "query failed: result=%d errno=%u sql=%s message=%s\n",
            result,
            mylite_mariadb_errno(db),
            sql.c_str(),
            message != nullptr ? message : mylite_errmsg(db)
        );
        mylite_free(message);
        std::abort();
    }
    assert(message == nullptr);
    assert(query.rows == 1);
    return query.value;
}

void read_exact_at(int fd, void *buffer, std::size_t size, off_t offset) {
    auto *cursor = static_cast<unsigned char *>(buffer);
    while (size != 0) {
        const ssize_t read_size = ::pread(fd, cursor, size, offset);
        if (read_size < 0 && errno == EINTR) {
            continue;
        }
        assert(read_size > 0);
        cursor += static_cast<std::size_t>(read_size);
        size -= static_cast<std::size_t>(read_size);
        offset += read_size;
    }
}

std::uint32_t read_le32(const unsigned char *bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t read_le64(const unsigned char *bytes) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    }
    return value;
}

std::uint64_t innodb_lock_registry_offset(int fd, std::uint32_t segment_type) {
    unsigned char bytes[8] = {};
    read_exact_at(fd, bytes, 4, k_shm_segment_table_offset);
    const std::uint64_t segment_table_offset = read_le32(bytes);
    read_exact_at(fd, bytes, 4, k_shm_segment_count_offset);
    const std::uint32_t segment_count = read_le32(bytes);
    for (std::uint32_t index = 0; index < segment_count; ++index) {
        const std::uint64_t descriptor_offset =
            segment_table_offset + index * k_shm_segment_descriptor_size;
        read_exact_at(
            fd,
            bytes,
            4,
            static_cast<off_t>(descriptor_offset + k_shm_segment_type_offset)
        );
        if (read_le32(bytes) != segment_type) {
            continue;
        }
        read_exact_at(
            fd,
            bytes,
            sizeof(bytes),
            static_cast<off_t>(descriptor_offset + k_shm_segment_data_offset)
        );
        return read_le64(bytes);
    }
    std::abort();
}

std::uint64_t innodb_waiting_count(const TestPaths &paths) {
    const std::string shm_path = paths.database + "/concurrency/mylite-concurrency.shm";
    const int fd = ::open(shm_path.c_str(), O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    const std::uint64_t registry_offset =
        innodb_lock_registry_offset(fd, k_innodb_lock_segment_type);
    unsigned char bytes[8] = {};
    read_exact_at(
        fd,
        bytes,
        sizeof(bytes),
        static_cast<off_t>(registry_offset + k_innodb_lock_waiting_count_offset)
    );
    assert(::close(fd) == 0);
    return read_le64(bytes);
}

std::uint32_t active_space_read_gate_count(const TestPaths &paths) {
    const std::string shm_path = paths.database + "/concurrency/mylite-concurrency.shm";
    const int fd = ::open(shm_path.c_str(), O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    const std::uint64_t registry_offset =
        innodb_lock_registry_offset(fd, k_page_write_lock_segment_type);
    unsigned char bytes[8] = {};
    read_exact_at(fd, bytes, 4, static_cast<off_t>(registry_offset));
    const std::uint32_t slot_count = read_le32(bytes);
    read_exact_at(fd, bytes, 4, static_cast<off_t>(registry_offset + 4));
    const std::uint32_t slot_size = read_le32(bytes);
    std::uint32_t active_read_gates = 0;
    for (std::uint32_t index = 0; index < slot_count; ++index) {
        const std::uint64_t slot_offset = registry_offset + k_innodb_lock_header_size +
                                          static_cast<std::uint64_t>(index) * slot_size;
        read_exact_at(
            fd,
            bytes,
            4,
            static_cast<off_t>(slot_offset + k_innodb_lock_slot_state_offset)
        );
        if (read_le32(bytes) != k_innodb_lock_state_active) {
            continue;
        }
        read_exact_at(
            fd,
            bytes,
            4,
            static_cast<off_t>(slot_offset + k_innodb_lock_slot_page_no_offset)
        );
        const std::uint32_t page_no = read_le32(bytes);
        if (page_no == k_space_transaction_read_page_no) {
            ++active_read_gates;
        }
    }
    assert(::close(fd) == 0);
    return active_read_gates;
}

void wait_for_innodb_waiter(const TestPaths &paths) {
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        if (innodb_waiting_count(paths) != 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fprintf(stderr, "ownerless InnoDB waiter was not published\n");
    std::abort();
}

void close_ok(mylite_db *db) {
    const int result = mylite_close(db);
    if (result != MYLITE_OK) {
        std::fprintf(stderr, "mylite_close failed: result=%d\n", result);
        std::abort();
    }
}

void close_faulted_database(mylite_db *db) {
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const int result = mylite_close(db);
        if (result == MYLITE_OK) {
            return;
        }
        if (result != MYLITE_IOERR && result != MYLITE_BUSY) {
            std::fprintf(stderr, "faulted mylite_close failed: result=%d\n", result);
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fprintf(stderr, "faulted mylite_close remained pending: %s\n", mylite_errmsg(db));
    std::abort();
}

void bootstrap_table(const TestPaths &paths, const std::string &table_sql) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, table_sql);
    close_ok(db);
}

void remove_paths(const TestPaths &paths) {
    std::error_code error;
    std::filesystem::remove_all(paths.root, error);
    assert(!error);
}

void test_memmove_is_persistent() {
    const TestPaths paths = make_paths("memmove");
    bootstrap_table(
        paths,
        "CREATE TABLE app.items ("
        "id INT NOT NULL PRIMARY KEY, payload VARBINARY(1400) NOT NULL"
        ") ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8"
    );

    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    mylite_ownerless_innodb_set_test_faults_enabled(1);
    mylite_ownerless_innodb_test_reset_mtr_memmove_count();
    exec_ok(db, "START TRANSACTION");
    for (unsigned i = 0; i < 96; ++i) {
        const unsigned id = (i % 2 == 0) ? i / 2 : 1000 - i / 2;
        exec_ok(
            db,
            "INSERT INTO app.items VALUES (" + std::to_string(id) + ",REPEAT(CHAR(65 + (" +
                std::to_string(i) + " % 20)),1200))"
        );
    }
    exec_ok(
        db,
        "UPDATE app.items SET payload=CONCAT(payload,REPEAT('z',100)) "
        "WHERE MOD(id,3)=0"
    );
    exec_ok(db, "COMMIT");
    std::uint64_t memmoves = mylite_ownerless_innodb_test_mtr_memmove_count();
    assert(memmoves != 0);
    unsigned long long row_count_before = query_unsigned(db, "SELECT COUNT(*) FROM app.items");
    assert(row_count_before == 96);
    unsigned long long checksum_before =
        query_unsigned(db, "SELECT COALESCE(SUM(CRC32(payload)),0) FROM app.items");
    const std::uint64_t memmoves_before_probe = memmoves;
    exec_ok(db, "START TRANSACTION");
    assert(::setenv("MYLITE_OWNERLESS_TEST_FAULT", "mtr-memmove-prepare-error", 1) == 0);
    bool fault_observed = false;
    for (unsigned i = 0; i < 512 && !fault_observed; ++i) {
        char *message = nullptr;
        const std::string sql =
            "INSERT INTO app.items VALUES (" + std::to_string(2000 + i) + ",REPEAT('q',1200))";
        const int result = mylite_exec(db, sql.c_str(), nullptr, nullptr, &message);
        if (result == MYLITE_OK) {
            assert(message == nullptr);
            continue;
        }
        const unsigned error = mylite_mariadb_errno(db);
        mylite_free(message);
        assert(error == k_storage_engine_error);
        fault_observed = true;
    }
    if (fault_observed) {
        assert(::getenv("MYLITE_OWNERLESS_TEST_FAULT") == nullptr);
        exec_ok(db, "ROLLBACK");
    } else {
        assert(::getenv("MYLITE_OWNERLESS_TEST_FAULT") != nullptr);
        assert(::unsetenv("MYLITE_OWNERLESS_TEST_FAULT") == 0);
        exec_ok(db, "COMMIT");
        row_count_before += 512;
        checksum_before =
            query_unsigned(db, "SELECT COALESCE(SUM(CRC32(payload)),0) FROM app.items");
    }
    memmoves = mylite_ownerless_innodb_test_mtr_memmove_count();
    assert(memmoves > memmoves_before_probe);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.items") == row_count_before);
    assert(
        query_unsigned(db, "SELECT COALESCE(SUM(CRC32(payload)),0) FROM app.items") ==
        checksum_before
    );
    close_ok(db);

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.items") == row_count_before);
    assert(
        query_unsigned(
            db,
            "SELECT COUNT(*) FROM app.items "
            "WHERE LENGTH(payload) NOT IN (1200,1300)"
        ) == 0
    );
    assert(
        query_unsigned(db, "SELECT COALESCE(SUM(CRC32(payload)),0) FROM app.items") ==
        checksum_before
    );
    exec_ok(db, "CHECK TABLE app.items EXTENDED");
    close_ok(db);
    remove_paths(paths);
}

void test_space_write_failures() {
    struct FaultCase {
        const char *name;
        unsigned mariadb_error;
    };

    static constexpr FaultCase cases[] = {
        {"space-write-timeout", k_cant_create_table},
        {"space-write-deadlock", k_cant_create_table},
        {"space-write-full", k_cant_create_table},
        {"space-write-error", 0},
    };

    for (const FaultCase &fault : cases) {
        const TestPaths paths = make_paths(fault.name);
        bootstrap_table(paths, "CREATE TABLE app.seed (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB");
        mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        assert(::setenv("MYLITE_OWNERLESS_TEST_FAULT", fault.name, 1) == 0);
        const unsigned error = exec_error(
            db,
            "CREATE TABLE app.rejected ("
            "id INT NOT NULL PRIMARY KEY, payload VARBINARY(4096) NOT NULL"
            ") ENGINE=InnoDB"
        );
        assert(::unsetenv("MYLITE_OWNERLESS_TEST_FAULT") == 0);
        if (error != fault.mariadb_error) {
            std::fprintf(
                stderr,
                "%s returned errno=%u, expected %u\n",
                fault.name,
                error,
                fault.mariadb_error
            );
            std::abort();
        }
        close_faulted_database(db);
        db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        assert(
            query_unsigned(
                db,
                "SELECT COUNT(*) FROM information_schema.tables "
                "WHERE table_schema='app' AND table_name='rejected'"
            ) == 0
        );
        exec_ok(
            db,
            "CREATE TABLE app.rejected ("
            "id INT NOT NULL PRIMARY KEY, payload VARBINARY(4096) NOT NULL"
            ") ENGINE=InnoDB"
        );
        exec_ok(db, "INSERT INTO app.rejected VALUES (1,REPEAT('x',4096))");
        close_ok(db);

        db = open_database(paths, MYLITE_OPEN_READWRITE);
        assert(query_unsigned(db, "SELECT COUNT(*) FROM app.rejected") == 1);
        close_ok(db);
        remove_paths(paths);
    }
}

void test_record_wait_publication_failures() {
    struct FaultCase {
        const char *name;
        unsigned expected_error;
    };

    static constexpr FaultCase cases[] = {
        {"record-wait-publish-timeout", k_lock_wait_timeout},
        {"record-wait-publish-deadlock", k_lock_deadlock},
        {"record-wait-publish-full", k_lock_table_full},
        {"record-wait-publish-error", k_storage_engine_error},
    };

    for (const FaultCase &test : cases) {
        const TestPaths paths = make_paths(test.name);
        bootstrap_table(
            paths,
            "CREATE TABLE app.items ("
            "id INT NOT NULL PRIMARY KEY, value INT NOT NULL"
            ") ENGINE=InnoDB"
        );
        mylite_db *holder = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        mylite_db *waiter = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        mylite_db *successor =
            open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        exec_ok(holder, "INSERT INTO app.items VALUES (1,10)");
        exec_ok(holder, "START TRANSACTION");
        exec_ok(holder, "UPDATE app.items SET value=11 WHERE id=1");
        exec_ok(waiter, "SET SESSION innodb_lock_wait_timeout=2");
        assert(::setenv("MYLITE_OWNERLESS_TEST_FAULT", test.name, 1) == 0);
        const unsigned error = exec_error(waiter, "UPDATE app.items SET value=12 WHERE id=1");
        assert(::unsetenv("MYLITE_OWNERLESS_TEST_FAULT") == 0);
        assert(error == test.expected_error);

        if (error == k_storage_engine_error) {
            mylite_ownerless_innodb_clear_coordination_error_for_recovery();
        }
        exec_ok(holder, "ROLLBACK");
        exec_ok(successor, "SET SESSION innodb_lock_wait_timeout=1");
        exec_ok(successor, "UPDATE app.items SET value=13 WHERE id=1");
        assert(query_unsigned(successor, "SELECT value FROM app.items WHERE id=1") == 13);
        close_ok(successor);
        close_ok(waiter);
        close_ok(holder);
        remove_paths(paths);
    }
}

void test_table_wait_publication_failures() {
    struct FaultCase {
        const char *name;
        unsigned expected_error;
    };

    static constexpr FaultCase cases[] = {
        {"table-wait-publish-timeout", k_autoinc_read_failed},
        {"table-wait-publish-deadlock", k_autoinc_read_failed},
        {"table-wait-publish-full", k_autoinc_read_failed},
        {"table-wait-publish-error", k_autoinc_read_failed},
    };

    for (const FaultCase &test : cases) {
        const TestPaths paths = make_paths(test.name);
        mylite_db *bootstrap = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);
        exec_ok(bootstrap, "CREATE DATABASE app");
        exec_ok(
            bootstrap,
            "CREATE TABLE app.parent ("
            "id INT NOT NULL PRIMARY KEY, value INT NOT NULL"
            ") ENGINE=InnoDB"
        );
        exec_ok(
            bootstrap,
            "CREATE TABLE app.items ("
            "id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, "
            "value INT NOT NULL, parent_id INT NOT NULL, "
            "FOREIGN KEY (parent_id) REFERENCES app.parent(id)"
            ") ENGINE=InnoDB"
        );
        exec_ok(bootstrap, "CREATE TABLE app.source (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB");
        exec_ok(bootstrap, "INSERT INTO app.source VALUES (1)");
        exec_ok(bootstrap, "INSERT INTO app.parent VALUES (1,0)");
        close_ok(bootstrap);

        mylite_db *blocker = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        mylite_db *waiter = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        mylite_db *successor =
            open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
        exec_ok(blocker, "START TRANSACTION");
        exec_ok(blocker, "UPDATE app.parent SET value=1 WHERE id=1");
        exec_ok(waiter, "SET SESSION innodb_lock_wait_timeout=2");
        std::thread holder_thread([paths] {
            mylite_db *holder =
                open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
            exec_ok(holder, "INSERT INTO app.items(value,parent_id) SELECT 10,1 FROM app.source");
            close_ok(holder);
            mysql_thread_end();
        });
        wait_for_innodb_waiter(paths);
        assert(::setenv("MYLITE_OWNERLESS_TEST_FAULT", test.name, 1) == 0);
        const unsigned error = exec_error(
            waiter,
            "INSERT INTO app.items(value,parent_id) SELECT 20,1 FROM app.source"
        );
        const bool fault_consumed = ::getenv("MYLITE_OWNERLESS_TEST_FAULT") == nullptr;
        assert(::unsetenv("MYLITE_OWNERLESS_TEST_FAULT") == 0);
        exec_ok(blocker, "ROLLBACK");
        holder_thread.join();
        assert(fault_consumed);
        if (error != test.expected_error) {
            std::fprintf(
                stderr,
                "%s returned errno=%u, expected %u\n",
                test.name,
                error,
                test.expected_error
            );
            std::abort();
        }

        if (error == k_storage_engine_error) {
            mylite_ownerless_innodb_clear_coordination_error_for_recovery();
        }
        exec_ok(successor, "INSERT INTO app.items(value,parent_id) VALUES (30,1)");
        assert(query_unsigned(successor, "SELECT COUNT(*) FROM app.items") == 2);
        close_ok(successor);
        close_ok(waiter);
        close_ok(blocker);
        remove_paths(paths);
    }
}

void write_signal(int fd, char value) {
    assert(::write(fd, &value, sizeof(value)) == sizeof(value));
}

char read_signal(int fd) {
    char value = 0;
    assert(::read(fd, &value, sizeof(value)) == sizeof(value));
    return value;
}

[[noreturn]] void run_structure_gate_peer(const TestPaths &paths, int command_fd, int result_fd) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout=1");
    assert(read_signal(command_fd) == 't');
    const unsigned first_error = exec_error(
        db,
        "INSERT INTO app.items VALUES "
        "(500,REPEAT('b',3000)),(501,REPEAT('c',3000)),"
        "(502,REPEAT('d',3000)),(503,REPEAT('e',3000))"
    );
    write_signal(result_fd, first_error == k_lock_wait_timeout ? 't' : 'x');
    assert(read_signal(command_fd) == 'r');
    exec_ok(
        db,
        "INSERT INTO app.items VALUES "
        "(500,REPEAT('b',3000)),(501,REPEAT('c',3000)),"
        "(502,REPEAT('d',3000)),(503,REPEAT('e',3000))"
    );
    write_signal(result_fd, 's');
    close_ok(db);
    _exit(0);
}

[[noreturn]] void run_record_lock_peer(const TestPaths &paths, int command_fd, int result_fd) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout=1");
    assert(read_signal(command_fd) == 't');
    const unsigned first_error =
        exec_error(db, "UPDATE app.items SET payload=REPEAT('u',3000) WHERE id=75");
    write_signal(result_fd, first_error == k_lock_wait_timeout ? 't' : 'x');
    assert(read_signal(command_fd) == 'r');
    exec_ok(db, "UPDATE app.items SET payload=REPEAT('u',3000) WHERE id=75");
    write_signal(result_fd, 's');
    close_ok(db);
    _exit(0);
}

void test_record_space_gate_is_transaction_retained() {
    const TestPaths paths = make_paths("record-space-gate");
    bootstrap_table(
        paths,
        "CREATE TABLE app.items ("
        "id INT NOT NULL PRIMARY KEY, payload VARBINARY(3100) NOT NULL"
        ") ENGINE=InnoDB"
    );
    mylite_db *seed = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    for (unsigned i = 0; i < 100; ++i) {
        exec_ok(
            seed,
            "INSERT INTO app.items VALUES (" + std::to_string(i) + ",REPEAT(CHAR(65 + (" +
                std::to_string(i) + " % 20)),3000))"
        );
    }
    close_ok(seed);

    int structure_command[2];
    int structure_result[2];
    int record_command[2];
    int record_result[2];
    assert(::pipe(structure_command) == 0);
    assert(::pipe(structure_result) == 0);
    assert(::pipe(record_command) == 0);
    assert(::pipe(record_result) == 0);

    const pid_t structure_child = ::fork();
    assert(structure_child >= 0);
    if (structure_child == 0) {
        assert(::close(structure_command[1]) == 0);
        assert(::close(structure_result[0]) == 0);
        assert(::close(record_command[0]) == 0);
        assert(::close(record_command[1]) == 0);
        assert(::close(record_result[0]) == 0);
        assert(::close(record_result[1]) == 0);
        run_structure_gate_peer(paths, structure_command[0], structure_result[1]);
    }
    const pid_t record_child = ::fork();
    assert(record_child >= 0);
    if (record_child == 0) {
        assert(::close(structure_command[0]) == 0);
        assert(::close(structure_command[1]) == 0);
        assert(::close(structure_result[0]) == 0);
        assert(::close(structure_result[1]) == 0);
        assert(::close(record_command[1]) == 0);
        assert(::close(record_result[0]) == 0);
        run_record_lock_peer(paths, record_command[0], record_result[1]);
    }

    assert(::close(structure_command[0]) == 0);
    assert(::close(structure_result[1]) == 0);
    assert(::close(record_command[0]) == 0);
    assert(::close(record_result[1]) == 0);

    mylite_db *holder = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW);
    exec_ok(holder, "START TRANSACTION");
    assert(query_unsigned(holder, "SELECT id FROM app.items WHERE id=75 FOR UPDATE") == 75);
    assert(active_space_read_gate_count(paths) != 0);

    write_signal(structure_command[1], 't');
    write_signal(record_command[1], 't');
    assert(read_signal(structure_result[0]) == 't');
    assert(read_signal(record_result[0]) == 't');

    exec_ok(holder, "COMMIT");
    assert(active_space_read_gate_count(paths) == 0);
    write_signal(record_command[1], 'r');
    assert(read_signal(record_result[0]) == 's');
    write_signal(structure_command[1], 'r');
    assert(read_signal(structure_result[0]) == 's');
    close_ok(holder);

    int status = 0;
    assert(::waitpid(structure_child, &status, 0) == structure_child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(::waitpid(record_child, &status, 0) == record_child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(::close(structure_command[1]) == 0);
    assert(::close(structure_result[0]) == 0);
    assert(::close(record_command[1]) == 0);
    assert(::close(record_result[0]) == 0);

    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(query_unsigned(db, "SELECT COUNT(*) FROM app.items") == 104);
    assert(query_unsigned(db, "SELECT LENGTH(payload) FROM app.items WHERE id=75") == 3000);
    exec_ok(db, "CHECK TABLE app.items EXTENDED");
    close_ok(db);
    remove_paths(paths);
}

} // namespace

int main() {
    test_memmove_is_persistent();
    test_space_write_failures();
    test_record_wait_publication_failures();
    test_table_wait_publication_failures();
    test_record_space_gate_is_transaction_retained();
    return 0;
}

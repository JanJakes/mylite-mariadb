#include <mylite/mylite.h>

#include "ownerless_page_log.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

extern "C" int mylite_ownerless_test_seed_saturated_checkpoint_records(int checkpoint_fd);
extern "C" int mylite_ownerless_test_update_saturated_checkpoint_lsn(int checkpoint_fd);
extern "C" int mylite_ownerless_test_update_checkpoint_lsn(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
);
extern "C" int mylite_ownerless_test_update_saturated_native_file_op(int checkpoint_fd);

constexpr off_t k_page_log_generation_offset = 24;
constexpr off_t k_page_log_append_lock_start = 0;
constexpr off_t k_page_log_checkpoint_lock_start = 1;
constexpr off_t k_checkpoint_state_lock_start = 0;
constexpr off_t k_dictionary_statement_entry_lock_start = 5;
constexpr auto k_bounded_wait_limit = std::chrono::seconds(2);

void write_exact(int fd, const void *buffer, std::size_t size, off_t offset) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t result = ::pwrite(fd, bytes + done, size - done, offset + done);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        assert(result > 0);
        done += static_cast<std::size_t>(result);
    }
}

std::vector<unsigned char> read_file(int fd) {
    struct stat file_stat = {};
    assert(::fstat(fd, &file_stat) == 0);
    assert(file_stat.st_size >= 0);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(file_stat.st_size));
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t result = ::pread(fd, bytes.data() + done, bytes.size() - done, done);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        assert(result > 0);
        done += static_cast<std::size_t>(result);
    }
    return bytes;
}

int open_file(const std::filesystem::path &path) {
    const std::string value = path.string();
    const int fd = ::open(value.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    return fd;
}

class LockHolder {
  public:
    LockHolder(const std::filesystem::path &path, off_t start) {
        int ready_pipe[2] = {-1, -1};
        int release_pipe[2] = {-1, -1};
        assert(::pipe(ready_pipe) == 0);
        assert(::pipe(release_pipe) == 0);
        const std::string path_value = path.string();
        pid_ = ::fork();
        assert(pid_ >= 0);
        if (pid_ == 0) {
            ::close(ready_pipe[0]);
            ::close(release_pipe[1]);
            const int fd = ::open(path_value.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                _exit(2);
            }
            struct flock lock = {};
            lock.l_type = F_WRLCK;
            lock.l_whence = SEEK_SET;
            lock.l_start = start;
            lock.l_len = 1;
            if (::fcntl(fd, F_SETLK, &lock) != 0 || ::write(ready_pipe[1], "x", 1) != 1) {
                _exit(3);
            }
            char release = 0;
            while (::read(release_pipe[0], &release, 1) < 0 && errno == EINTR) {}
            lock.l_type = F_UNLCK;
            static_cast<void>(::fcntl(fd, F_SETLK, &lock));
            ::close(fd);
            _exit(release == 'x' ? 0 : 4);
        }

        ::close(ready_pipe[1]);
        ::close(release_pipe[0]);
        release_fd_ = release_pipe[1];
        struct pollfd ready = {ready_pipe[0], POLLIN, 0};
        assert(::poll(&ready, 1, 2000) == 1);
        char value = 0;
        assert(::read(ready_pipe[0], &value, 1) == 1 && value == 'x');
        ::close(ready_pipe[0]);
    }

    LockHolder(const LockHolder &) = delete;
    LockHolder &operator=(const LockHolder &) = delete;

    ~LockHolder() {
        release();
    }

    void release() {
        if (pid_ <= 0) {
            return;
        }
        assert(::write(release_fd_, "x", 1) == 1);
        ::close(release_fd_);
        release_fd_ = -1;
        int status = 0;
        assert(::waitpid(pid_, &status, 0) == pid_);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        pid_ = -1;
    }

  private:
    pid_t pid_ = -1;
    int release_fd_ = -1;
};

struct TempRoot {
    char value[64] = "/tmp/mylite-teardown-lock-XXXXXX";

    TempRoot() {
        assert(::mkdtemp(value) != nullptr);
    }

    ~TempRoot() {
        std::error_code error;
        std::filesystem::remove_all(value, error);
    }

    std::filesystem::path path(const char *name) const {
        return std::filesystem::path(value) / name;
    }
};

template <typename Callback> void assert_bounded_failure(Callback callback) {
    const auto started = std::chrono::steady_clock::now();
    callback();
    assert(std::chrono::steady_clock::now() - started < k_bounded_wait_limit);
}

void set_page_log_generation_saturated(int fd) {
    std::array<unsigned char, sizeof(std::uint64_t)> generation = {};
    std::uint64_t value = std::numeric_limits<std::uint64_t>::max();
    for (unsigned index = 0; index < generation.size(); ++index) {
        generation[index] = static_cast<unsigned char>(value & 0xffU);
        value >>= 8U;
    }
    write_exact(fd, generation.data(), generation.size(), k_page_log_generation_offset);
}

void append_test_page(int fd) {
    std::array<unsigned char, 64> page = {};
    page.fill(0x5aU);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            7U,
            11U,
            100U,
            100U,
            page.data(),
            page.size(),
            nullptr
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
}

void test_page_log_file_locks_and_generation_saturation() {
    TempRoot root;
    const auto log_path = root.path("page-log");
    const auto log_stage_path = root.path("page-log-stage");
    const int fd = open_file(log_path);
    const int log_stage_fd = open_file(log_stage_path);
    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_register_checkpoint_stage(fd, log_stage_fd) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    {
        LockHolder holder(log_path, k_page_log_append_lock_start);
        std::array<unsigned char, 64> page = {};
        assert_bounded_failure([&] {
            assert(
                mylite_ownerless_page_log_append(
                    fd,
                    1U,
                    2U,
                    10U,
                    10U,
                    page.data(),
                    page.size(),
                    nullptr
                ) == MYLITE_OWNERLESS_PAGE_LOG_ERROR
            );
        });
    }
    append_test_page(fd);

    {
        LockHolder holder(log_path, k_page_log_checkpoint_lock_start);
        assert_bounded_failure([&] {
            assert(
                mylite_ownerless_page_log_checkpoint(
                    fd,
                    std::numeric_limits<std::uint64_t>::max(),
                    nullptr,
                    nullptr
                ) == MYLITE_OWNERLESS_PAGE_LOG_ERROR
            );
        });
    }
    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            std::numeric_limits<std::uint64_t>::max(),
            nullptr,
            nullptr
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_unregister_checkpoint_stage(fd);
    ::close(log_stage_fd);
    ::close(fd);

    const auto staged_log_path = root.path("staged-log");
    const auto stage_path = root.path("checkpoint-stage");
    const int staged_fd = open_file(staged_log_path);
    const int stage_fd = open_file(stage_path);
    assert(mylite_ownerless_page_log_initialize(staged_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    append_test_page(staged_fd);
    assert(
        mylite_ownerless_page_log_register_checkpoint_stage(staged_fd, stage_fd) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    set_page_log_generation_saturated(staged_fd);
    const auto staged_before = read_file(staged_fd);
    const auto stage_before = read_file(stage_fd);
    assert(
        mylite_ownerless_page_log_checkpoint(
            staged_fd,
            std::numeric_limits<std::uint64_t>::max(),
            nullptr,
            nullptr
        ) == MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(read_file(staged_fd) == staged_before);
    assert(read_file(stage_fd) == stage_before);
    mylite_ownerless_page_log_unregister_checkpoint_stage(staged_fd);
    ::close(stage_fd);
    ::close(staged_fd);

    const auto legacy_log_path = root.path("legacy-log");
    const int legacy_fd = open_file(legacy_log_path);
    assert(mylite_ownerless_page_log_initialize(legacy_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    append_test_page(legacy_fd);
    set_page_log_generation_saturated(legacy_fd);
    const auto legacy_before = read_file(legacy_fd);
    assert(
        mylite_ownerless_page_log_checkpoint(
            legacy_fd,
            std::numeric_limits<std::uint64_t>::max(),
            nullptr,
            nullptr
        ) == MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(read_file(legacy_fd) == legacy_before);
    ::close(legacy_fd);
}

void test_checkpoint_state_lock_and_generation_saturation() {
    TempRoot root;
    const auto lock_path = root.path("checkpoint-lock");
    const int lock_fd = open_file(lock_path);
    assert(mylite_ownerless_test_update_checkpoint_lsn(lock_fd, 1U, 1U) == 1);
    const auto before = read_file(lock_fd);
    {
        LockHolder holder(lock_path, k_checkpoint_state_lock_start);
        assert_bounded_failure([&] {
            assert(mylite_ownerless_test_update_checkpoint_lsn(lock_fd, 2U, 2U) == 0);
        });
        assert(read_file(lock_fd) == before);
    }
    assert(mylite_ownerless_test_update_checkpoint_lsn(lock_fd, 2U, 2U) == 1);
    ::close(lock_fd);

    const int saturated_fd = open_file(root.path("checkpoint-saturated"));
    assert(mylite_ownerless_test_seed_saturated_checkpoint_records(saturated_fd) == 1);
    const auto saturated_before = read_file(saturated_fd);
    assert(mylite_ownerless_test_update_saturated_checkpoint_lsn(saturated_fd) == 0);
    assert(read_file(saturated_fd) == saturated_before);
    assert(mylite_ownerless_test_update_saturated_native_file_op(saturated_fd) == 0);
    assert(read_file(saturated_fd) == saturated_before);
    ::close(saturated_fd);
}

void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = nullptr;
    const int result = mylite_exec(db, sql, nullptr, nullptr, &errmsg);
    if (result != MYLITE_OK) {
        std::fprintf(stderr, "SQL failed: %s: %s\n", sql, errmsg != nullptr ? errmsg : "");
    }
    mylite_free(errmsg);
    assert(result == MYLITE_OK);
}

mylite_db *open_ownerless(
    const std::filesystem::path &database_path,
    const std::filesystem::path &runtime_path,
    bool create
) {
    const std::string database = database_path.string();
    const std::string runtime = runtime_path.string();
    mylite_open_config config = {};
    config.size = sizeof(config);
    config.profile = MYLITE_PROFILE_DEFAULT;
    config.busy_timeout_ms = 0U;
    config.durability = MYLITE_DURABILITY_FULL;
    config.temp_directory = runtime.c_str();
    mylite_db *db = nullptr;
    unsigned flags = MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW;
    if (create) {
        flags |= MYLITE_OPEN_CREATE;
    }
    const int result = mylite_open(database.c_str(), &db, flags, &config);
    if (result != MYLITE_OK) {
        std::fprintf(stderr, "mylite_open failed: %d\n", result);
    }
    assert(result == MYLITE_OK && db != nullptr);
    return db;
}

mylite_db *open_exclusive(
    const std::filesystem::path &database_path,
    const std::filesystem::path &runtime_path,
    bool create
) {
    const std::string database = database_path.string();
    const std::string runtime = runtime_path.string();
    mylite_open_config config = {};
    config.size = sizeof(config);
    config.profile = MYLITE_PROFILE_DEFAULT;
    config.durability = MYLITE_DURABILITY_FULL;
    config.temp_directory = runtime.c_str();
    mylite_db *db = nullptr;
    unsigned flags = MYLITE_OPEN_READWRITE;
    if (create) {
        flags |= MYLITE_OPEN_CREATE;
    }
    assert(mylite_open(database.c_str(), &db, flags, &config) == MYLITE_OK);
    assert(db != nullptr);
    return db;
}

void test_ddl_lock_and_retryable_final_cleanup() {
    TempRoot root;
    const auto database_path = root.path("database");
    const auto runtime_path = root.path("runtime");
    assert(std::filesystem::create_directory(runtime_path));

    mylite_db *db = open_ownerless(database_path, runtime_path, true);
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, "SET SESSION lock_wait_timeout=0");
    const auto statement_lock_path = database_path / "concurrency" / "mylite-statements.lock";
    {
        LockHolder holder(statement_lock_path, k_dictionary_statement_entry_lock_start);
        assert_bounded_failure([&] {
            assert(
                mylite_exec(
                    db,
                    "CREATE TABLE app.locked_ddl (id INT PRIMARY KEY) ENGINE=InnoDB",
                    nullptr,
                    nullptr,
                    nullptr
                ) == MYLITE_BUSY
            );
        });
    }
    exec_ok(db, "CREATE TABLE app.locked_ddl (id INT PRIMARY KEY) ENGINE=InnoDB");

    assert(::setenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE", "owner-state", 1) == 0);
    assert(mylite_close(db) == MYLITE_IOERR);
    assert(mylite_errcode(db) == MYLITE_IOERR);
    assert(std::strstr(mylite_errmsg(db), "retry mylite_close") != nullptr);
    assert(mylite_exec(db, "SELECT 1", nullptr, nullptr, nullptr) == MYLITE_IOERR);
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE") == 0);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_ownerless(database_path, runtime_path, false);
    assert(::setenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE", "process-slot", 1) == 0);
    assert(mylite_close(db) == MYLITE_IOERR);
    assert(mylite_errcode(db) == MYLITE_IOERR);
    assert(std::strstr(mylite_errmsg(db), "retry mylite_close") != nullptr);
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE") == 0);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_ownerless(database_path, runtime_path, false);
    exec_ok(db, "INSERT INTO app.locked_ddl VALUES (1)");
    assert(::setenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE", "redo-prefix", 1) == 0);
    assert(mylite_close(db) == MYLITE_IOERR);
    assert(mylite_errcode(db) == MYLITE_IOERR);
    assert(std::strstr(mylite_errmsg(db), "retry mylite_close") != nullptr);
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE") == 0);
    assert(mylite_close(db) == MYLITE_OK);

    db = open_exclusive(database_path, runtime_path, false);
    assert(::setenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE", "redo-prefix", 1) == 0);
    assert(mylite_close(db) == MYLITE_IOERR);
    assert(mylite_errcode(db) == MYLITE_IOERR);
    assert(std::strstr(mylite_errmsg(db), "retry mylite_close") != nullptr);
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE") == 0);
    assert(mylite_close(db) == MYLITE_OK);
}

void test_failed_open_retries_pending_runtime_cleanup() {
    TempRoot root;
    const auto database_path = root.path("database");
    const auto runtime_path = root.path("runtime");
    assert(std::filesystem::create_directory(runtime_path));

    mylite_db *db = open_exclusive(database_path, runtime_path, true);
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, "CREATE TABLE app.unsupported_engine (id INT PRIMARY KEY) ENGINE=MyISAM");
    assert(mylite_close(db) == MYLITE_OK);

    const std::string database = database_path.string();
    const std::string runtime = runtime_path.string();
    mylite_open_config config = {};
    config.size = sizeof(config);
    config.profile = MYLITE_PROFILE_DEFAULT;
    config.durability = MYLITE_DURABILITY_FULL;
    config.temp_directory = runtime.c_str();
    db = nullptr;
    assert(::setenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE", "owner-state", 1) == 0);
    assert(
        mylite_open(
            database.c_str(),
            &db,
            MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
            &config
        ) == MYLITE_ERROR
    );
    assert(db == nullptr);
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE") == 0);

    db = open_exclusive(database_path, runtime_path, false);
    assert(mylite_close(db) == MYLITE_OK);
}

void test_startup_unmap_failure_is_retried_before_next_open() {
    TempRoot root;
    const auto database_path = root.path("database");
    const auto runtime_path = root.path("runtime");
    assert(std::filesystem::create_directory(runtime_path));

    mylite_db *db = open_ownerless(database_path, runtime_path, true);
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, "CREATE TABLE app.t (id INT PRIMARY KEY) ENGINE=InnoDB");
    assert(mylite_close(db) == MYLITE_OK);

    const std::string database = database_path.string();
    const std::string runtime = runtime_path.string();
    mylite_open_config config = {};
    config.size = sizeof(config);
    config.profile = MYLITE_PROFILE_DEFAULT;
    config.durability = MYLITE_DURABILITY_FULL;
    config.temp_directory = runtime.c_str();
    db = nullptr;
    assert(
        ::setenv("MYLITE_OWNERLESS_TEST_FAULT", "runtime-startup-after-mysql-server-init", 1) == 0
    );
    assert(::setenv("MYLITE_OWNERLESS_TEST_FAULT_COUNT", "1", 1) == 0);
    assert(::setenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE", "process-slot", 1) == 0);
    assert(
        mylite_open(
            database.c_str(),
            &db,
            MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW,
            &config
        ) == MYLITE_BUSY
    );
    assert(db == nullptr);
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FINAL_CLEANUP_FAILURE") == 0);
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FAULT_COUNT") == 0);
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FAULT") == 0);

    db = open_ownerless(database_path, runtime_path, false);
    assert(mylite_close(db) == MYLITE_OK);
}

void test_owner_cleanup_completes_applied_pending_releases() {
    TempRoot root;
    const auto database_path = root.path("database");
    const auto runtime_path = root.path("runtime");
    assert(std::filesystem::create_directory(runtime_path));

    mylite_db *db = open_ownerless(database_path, runtime_path, true);
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, "CREATE TABLE app.t (id INT PRIMARY KEY) ENGINE=InnoDB");
    assert(mylite_close(db) == MYLITE_OK);

    constexpr std::array<const char *, 6> registries =
        {"mdl", "innodb-lock", "page-write", "trx", "redo", "read-view"};
    for (const char *registry : registries) {
        db = open_ownerless(database_path, runtime_path, false);
        assert(::setenv("MYLITE_OWNERLESS_TEST_OWNER_CLEANUP_RELEASE_PENDING", registry, 1) == 0);
        assert(mylite_close(db) == MYLITE_OK);
        assert(::unsetenv("MYLITE_OWNERLESS_TEST_OWNER_CLEANUP_RELEASE_PENDING") == 0);

        db = open_ownerless(database_path, runtime_path, false);
        assert(mylite_close(db) == MYLITE_OK);
    }
}

} // namespace

int main() {
    const char *filter = std::getenv("MYLITE_OWNERLESS_TEARDOWN_TEST_FILTER");
    if (filter != nullptr && std::strcmp(filter, "page-log") == 0) {
        assert(::setenv("MYLITE_OWNERLESS_TEST_FILE_LOCK_TIMEOUT_MS", "50", 1) == 0);
        test_page_log_file_locks_and_generation_saturation();
        assert(::unsetenv("MYLITE_OWNERLESS_TEST_FILE_LOCK_TIMEOUT_MS") == 0);
        return 0;
    }
    if (filter != nullptr && std::strcmp(filter, "checkpoint-state") == 0) {
        assert(::setenv("MYLITE_OWNERLESS_TEST_FILE_LOCK_TIMEOUT_MS", "50", 1) == 0);
        test_checkpoint_state_lock_and_generation_saturation();
        assert(::unsetenv("MYLITE_OWNERLESS_TEST_FILE_LOCK_TIMEOUT_MS") == 0);
        return 0;
    }
    if (filter != nullptr && std::strcmp(filter, "ddl-cleanup") == 0) {
        test_ddl_lock_and_retryable_final_cleanup();
        return 0;
    }
    assert(::setenv("MYLITE_OWNERLESS_TEST_FILE_LOCK_TIMEOUT_MS", "50", 1) == 0);
    test_page_log_file_locks_and_generation_saturation();
    test_checkpoint_state_lock_and_generation_saturation();
    assert(::unsetenv("MYLITE_OWNERLESS_TEST_FILE_LOCK_TIMEOUT_MS") == 0);
    test_ddl_lock_and_retryable_final_cleanup();
    test_failed_open_retries_pending_runtime_cleanup();
    test_startup_unmap_failure_is_retried_before_next_open();
    test_owner_cleanup_completes_applied_pending_releases();
    return 0;
}

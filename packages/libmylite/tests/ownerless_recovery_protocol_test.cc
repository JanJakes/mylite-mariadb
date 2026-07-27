#include "ownerless_page_log.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

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

constexpr off_t k_page_log_ack_slots_offset = 4096;
constexpr off_t k_checkpoint_stage_ready_slot_offset = 8192;
constexpr off_t k_checkpoint_stage_format_offset = 8;
constexpr off_t k_checkpoint_stage_state_offset = 16;
constexpr off_t k_checkpoint_stage_target_size_offset = 64;
constexpr off_t k_checkpoint_stage_state_sequence_offset = 96;
constexpr std::uint32_t k_checkpoint_stage_format_v3 = 3;
constexpr std::uint32_t k_checkpoint_stage_state_ready = 2;
constexpr std::uint64_t k_checkpoint_stage_ready_sequence = 3;

std::uint64_t load64(const unsigned char *bytes) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

std::uint32_t load32(const unsigned char *bytes) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

void read_exact(int fd, void *buffer, std::size_t size, off_t offset) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t result = ::pread(fd, bytes + done, size - done, offset + done);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        assert(result > 0);
        done += static_cast<std::size_t>(result);
    }
}

int open_rw(const char *path) {
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    return fd;
}

void wait_for_byte(int fd) {
    char byte = 0;
    while (::read(fd, &byte, 1) < 0 && errno == EINTR) {}
    assert(byte == 'x');
}

void crash_checkpoint_at(int fd, int stage_fd, const char *fault) {
    int ready[2] = {-1, -1};
    assert(::pipe(ready) == 0);
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        ::close(ready[0]);
        char ready_fd[32] = {};
        assert(std::snprintf(ready_fd, sizeof(ready_fd), "%d", ready[1]) > 0);
        assert(::setenv("MYLITE_OWNERLESS_TEST_FAULT", fault, 1) == 0);
        assert(::setenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD", ready_fd, 1) == 0);
        assert(
            mylite_ownerless_page_log_register_checkpoint_stage(fd, stage_fd) ==
            MYLITE_OWNERLESS_PAGE_LOG_OK
        );
        static_cast<void>(mylite_ownerless_page_log_checkpoint(fd, 100, nullptr, nullptr));
        _exit(2);
    }
    ::close(ready[1]);
    wait_for_byte(ready[0]);
    ::close(ready[0]);
    assert(::kill(child, SIGKILL) == 0);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

struct Fixture {
    char root[64] = "/tmp/mylite-recovery-protocol-XXXXXX";
    char log_path[128] = {};
    char stage_path[160] = {};
    int log_fd = -1;
    int stage_fd = -1;

    Fixture() {
        assert(::mkdtemp(root) != nullptr);
        assert(std::snprintf(log_path, sizeof(log_path), "%s/page-log", root) > 0);
        assert(std::snprintf(stage_path, sizeof(stage_path), "%s/page-log.stage", root) > 0);
        log_fd = open_rw(log_path);
        stage_fd = open_rw(stage_path);
        assert(mylite_ownerless_page_log_initialize(log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
        std::array<unsigned char, 32> first{};
        std::array<unsigned char, 32> second{};
        first.fill(0x41);
        second.fill(0x42);
        assert(
            mylite_ownerless_page_log_append(
                log_fd,
                7,
                11,
                100,
                100,
                first.data(),
                first.size(),
                nullptr
            ) == MYLITE_OWNERLESS_PAGE_LOG_OK
        );
        assert(
            mylite_ownerless_page_log_append(
                log_fd,
                7,
                11,
                120,
                120,
                second.data(),
                second.size(),
                nullptr
            ) == MYLITE_OWNERLESS_PAGE_LOG_OK
        );
        assert(
            mylite_ownerless_page_log_register_checkpoint_stage(log_fd, stage_fd) ==
            MYLITE_OWNERLESS_PAGE_LOG_OK
        );
    }

    ~Fixture() {
        if (log_fd >= 0) {
            mylite_ownerless_page_log_unregister_checkpoint_stage(log_fd);
        }
        if (stage_fd >= 0) {
            ::close(stage_fd);
        }
        if (log_fd >= 0) {
            ::close(log_fd);
        }
        ::unlink(stage_path);
        ::unlink(log_path);
        ::rmdir(root);
    }
};

std::array<unsigned char, 128> read_stage_header(int fd) {
    std::array<unsigned char, 128> header{};
    read_exact(fd, header.data(), header.size(), k_checkpoint_stage_ready_slot_offset);
    assert(
        load32(header.data() + k_checkpoint_stage_format_offset) == k_checkpoint_stage_format_v3
    );
    assert(
        load32(header.data() + k_checkpoint_stage_state_offset) == k_checkpoint_stage_state_ready
    );
    assert(
        load64(header.data() + k_checkpoint_stage_state_sequence_offset) ==
        k_checkpoint_stage_ready_sequence
    );
    return header;
}

void test_recovery_normalizes_length_and_acknowledges() {
    Fixture fixture;
    crash_checkpoint_at(fixture.log_fd, fixture.stage_fd, "checkpoint-install-before-truncate");
    const auto stage_header = read_stage_header(fixture.stage_fd);
    const std::uint64_t target_size =
        load64(stage_header.data() + k_checkpoint_stage_target_size_offset);
    struct stat before{};
    assert(::fstat(fixture.log_fd, &before) == 0);
    assert(static_cast<std::uint64_t>(before.st_size) > target_size);

    std::array<unsigned char, 32> page{};
    std::uint32_t page_size = 0;
    std::uint64_t page_lsn = 0;
    std::uint64_t commit_lsn = 0;
    assert(
        mylite_ownerless_page_log_find_latest(
            fixture.log_fd,
            7,
            11,
            120,
            page.data(),
            page.size(),
            &page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(page_size == page.size() && page_lsn == 120 && commit_lsn == 120);
    for (unsigned char byte : page) {
        assert(byte == 0x42);
    }

    struct stat after{};
    struct stat stage_after{};
    assert(::fstat(fixture.log_fd, &after) == 0);
    assert(static_cast<std::uint64_t>(after.st_size) == target_size);
    assert(::fstat(fixture.stage_fd, &stage_after) == 0 && stage_after.st_size == 0);
    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE> log_header{};
    read_exact(fixture.log_fd, log_header.data(), log_header.size(), 0);
    const std::uint64_t ack0 = load64(log_header.data() + k_page_log_ack_slots_offset);
    const std::uint64_t ack1 = load64(log_header.data() + k_page_log_ack_slots_offset + 16);
    assert(ack0 == target_size || ack1 == target_size);
}

void test_ready_is_preserved_on_recovery_failure() {
    Fixture fixture;
    crash_checkpoint_at(fixture.log_fd, fixture.stage_fd, "checkpoint-stage-ready");
    static_cast<void>(read_stage_header(fixture.stage_fd));

    mylite_ownerless_page_log_unregister_checkpoint_stage(fixture.log_fd);
    assert(::close(fixture.log_fd) == 0);
    fixture.log_fd = ::open(fixture.log_path, O_RDONLY | O_CLOEXEC);
    assert(fixture.log_fd >= 0);
    assert(
        mylite_ownerless_page_log_register_checkpoint_stage(fixture.log_fd, fixture.stage_fd) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(mylite_ownerless_page_log_initialize(fixture.log_fd) == MYLITE_OWNERLESS_PAGE_LOG_ERROR);
    struct stat failed_stage{};
    assert(::fstat(fixture.stage_fd, &failed_stage) == 0 && failed_stage.st_size > 0);
    static_cast<void>(read_stage_header(fixture.stage_fd));

    mylite_ownerless_page_log_unregister_checkpoint_stage(fixture.log_fd);
    assert(::close(fixture.log_fd) == 0);
    fixture.log_fd = ::open(fixture.log_path, O_RDWR | O_CLOEXEC);
    assert(fixture.log_fd >= 0);
    assert(
        mylite_ownerless_page_log_register_checkpoint_stage(fixture.log_fd, fixture.stage_fd) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(mylite_ownerless_page_log_initialize(fixture.log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(::fstat(fixture.stage_fd, &failed_stage) == 0 && failed_stage.st_size == 0);
}

void test_rseg_history_reconciliation_policy() {
    constexpr int native = 1;
    constexpr int retained = 2;
    constexpr int error = 3;
    const auto choose = [](int native_valid,
                           std::uint32_t native_len,
                           std::uint32_t native_first,
                           std::uint32_t native_last,
                           std::uint64_t native_lsn,
                           std::uint32_t retained_len,
                           std::uint32_t retained_first,
                           std::uint32_t retained_last,
                           std::uint64_t retained_lsn,
                           int has_pair) {
        return mylite_ownerless_innodb_test_choose_startup_rseg_history(
            native_valid,
            native_len,
            native_first,
            128,
            native_last,
            256,
            native_lsn,
            retained_len,
            retained_first,
            128,
            retained_last,
            256,
            retained_lsn,
            has_pair
        );
    };

    assert(choose(1, 0, UINT32_MAX, UINT32_MAX, 200, 1, 17, 17, 100, 1) == native);
    assert(choose(1, 0, UINT32_MAX, UINT32_MAX, 200, 1, 17, 17, 100, 0) == native);
    assert(choose(1, 1, 17, 17, 100, 1, 17, 17, 120, 1) == retained);
    assert(choose(1, 1, 17, 17, 100, 1, 18, 18, 120, 1) == retained);
    assert(choose(1, 1, 17, 17, 100, 1, 18, 18, 120, 0) == error);
    assert(choose(1, 1, 17, 17, 200, 0, UINT32_MAX, UINT32_MAX, 220, 0) == retained);
    assert(choose(0, 0, UINT32_MAX, UINT32_MAX, 0, 1, 17, 17, 120, 1) == retained);

    assert(mylite_ownerless_innodb_test_required_undo_commit_matches(120, 0, 120));
    assert(!mylite_ownerless_innodb_test_required_undo_commit_matches(120, 0, 119));
    assert(!mylite_ownerless_innodb_test_required_undo_commit_matches(120, 1, 0));
    assert(!mylite_ownerless_innodb_test_required_undo_commit_matches(120, 4, 120));
}

} // namespace

int main() {
    if (!mylite_ownerless_page_log_test_faults_enabled()) {
        return 0;
    }
    test_recovery_normalizes_length_and_acknowledges();
    test_ready_is_preserved_on_recovery_failure();
    test_rseg_history_reconciliation_policy();
    return 0;
}

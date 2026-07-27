#include "ownerless_page_log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

constexpr off_t k_append_lock_start = 0;
constexpr off_t k_checkpoint_lock_start = 1;

bool child_can_lock(const char *path, off_t lock_start) {
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const int fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            _exit(2);
        }
        struct flock lock = {};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = lock_start;
        lock.l_len = 1;
#if defined(F_OFD_SETLK)
        constexpr int lock_command = F_OFD_SETLK;
#else
        constexpr int lock_command = F_SETLK;
#endif
        const bool acquired = ::fcntl(fd, lock_command, &lock) == 0;
        if (acquired) {
            lock.l_type = F_UNLCK;
            CHECK(::fcntl(fd, lock_command, &lock) == 0);
        }
        CHECK(::close(fd) == 0);
        _exit(acquired ? 0 : 1);
    }

    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    return WEXITSTATUS(status) == 0;
}

void test_append_session_unlock_failure_is_retryable(int fd, const char *path) {
    mylite_ownerless_page_log_append_session outer = {};
    mylite_ownerless_page_log_append_session inner = {};
    CHECK(
        mylite_ownerless_page_log_append_session_begin_initialized_at(fd, 0U, &outer) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    CHECK(
        mylite_ownerless_page_log_append_session_begin_initialized_at(fd, 0U, &inner) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_test_inject_unlock_failure_once();
    CHECK(mylite_ownerless_page_log_append_session_end(fd, &inner) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    CHECK(
        mylite_ownerless_page_log_append_session_end(fd, &outer) == MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    CHECK(outer.active != 0);
    CHECK(
        mylite_ownerless_page_log_append_session_append(
            fd,
            &outer,
            1U,
            1U,
            1U,
            1U,
            path,
            1U,
            nullptr
        ) == MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    CHECK(mylite_ownerless_page_log_retire_process_lock(fd) == MYLITE_OWNERLESS_PAGE_LOG_BUSY);
    CHECK(!child_can_lock(path, k_append_lock_start));
    CHECK(!child_can_lock(path, k_checkpoint_lock_start));

    CHECK(mylite_ownerless_page_log_append_session_end(fd, &outer) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    CHECK(outer.active == 0);
    CHECK(child_can_lock(path, k_append_lock_start));
    CHECK(child_can_lock(path, k_checkpoint_lock_start));
    CHECK(mylite_ownerless_page_log_append_session_end(fd, &outer) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    CHECK(mylite_ownerless_page_log_retire_process_lock(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
}

void test_read_unlock_failure_is_retryable(int fd, const char *path) {
    CHECK(mylite_ownerless_page_log_begin_read(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_test_inject_unlock_failure_once();
    CHECK(mylite_ownerless_page_log_end_read(fd) == MYLITE_OWNERLESS_PAGE_LOG_ERROR);
    CHECK(!child_can_lock(path, k_checkpoint_lock_start));
    CHECK(mylite_ownerless_page_log_end_read(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    CHECK(mylite_ownerless_page_log_end_read(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    CHECK(child_can_lock(path, k_checkpoint_lock_start));
    CHECK(mylite_ownerless_page_log_retire_process_lock(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
}

} // namespace

int main() {
    CHECK(mylite_ownerless_page_log_test_faults_enabled() == 1);
    char path[] = "/tmp/mylite-ownerless-page-log-unlock-XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);
    CHECK(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);

    test_append_session_unlock_failure_is_retryable(fd, path);
    test_read_unlock_failure_is_retryable(fd, path);

    CHECK(::close(fd) == 0);
    CHECK(::unlink(path) == 0);
    return 0;
}

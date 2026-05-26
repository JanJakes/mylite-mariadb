#include "ownerless_wait.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <ctime>
#include <thread>

#if defined(__linux__)
#  include <linux/futex.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

namespace {

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
#if defined(__linux__)
int wait_with_futex(mylite_ownerless_wait_word *word, std::uint32_t expected, unsigned timeout_ms);
#else
int wait_with_backoff(
    mylite_ownerless_wait_word *word,
    std::uint32_t expected,
    unsigned timeout_ms
);
#endif
#if defined(__linux__)
timespec timespec_from_milliseconds(unsigned timeout_ms);
#endif

} // namespace

std::uint32_t mylite_ownerless_wait_load(const mylite_ownerless_wait_word *word) {
    if (word == nullptr) {
        return 0U;
    }
    return __atomic_load_n(&word->value, __ATOMIC_ACQUIRE);
}

void mylite_ownerless_wait_store(mylite_ownerless_wait_word *word, std::uint32_t value) {
    if (word == nullptr) {
        return;
    }
    __atomic_store_n(&word->value, value, __ATOMIC_RELEASE);
}

int mylite_ownerless_wait_for_change(
    mylite_ownerless_wait_word *word,
    std::uint32_t expected,
    unsigned timeout_ms
) {
    if (word == nullptr) {
        return MYLITE_OWNERLESS_WAIT_ERROR;
    }
    if (mylite_ownerless_wait_load(word) != expected) {
        return MYLITE_OWNERLESS_WAIT_OK;
    }
    if (timeout_ms == 0U) {
        return MYLITE_OWNERLESS_WAIT_TIMEOUT;
    }
#if defined(__linux__)
    return wait_with_futex(word, expected, timeout_ms);
#else
    return wait_with_backoff(word, expected, timeout_ms);
#endif
}

int mylite_ownerless_wait_wake(mylite_ownerless_wait_word *word) {
    if (word == nullptr) {
        return MYLITE_OWNERLESS_WAIT_ERROR;
    }
#if defined(__linux__)
    const long result = syscall(SYS_futex, &word->value, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
    return result < 0 ? MYLITE_OWNERLESS_WAIT_ERROR : MYLITE_OWNERLESS_WAIT_OK;
#else
    return MYLITE_OWNERLESS_WAIT_OK;
#endif
}

const char *mylite_ownerless_wait_backend_name(void) {
#if defined(__linux__)
    return "linux-futex";
#else
    return "adaptive-backoff";
#endif
}

int mylite_ownerless_wait_backend_is_fast(void) {
#if defined(__linux__)
    return 1;
#else
    return 0;
#endif
}

namespace {

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

#if defined(__linux__)
int wait_with_futex(mylite_ownerless_wait_word *word, std::uint32_t expected, unsigned timeout_ms) {
    const auto deadline = wait_deadline(timeout_ms);

    while (mylite_ownerless_wait_load(word) == expected) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return MYLITE_OWNERLESS_WAIT_TIMEOUT;
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto remaining_ms =
            static_cast<unsigned>(std::max<std::chrono::milliseconds::rep>(remaining.count(), 1));
        timespec timeout = timespec_from_milliseconds(remaining_ms);
        const long result =
            syscall(SYS_futex, &word->value, FUTEX_WAIT, expected, &timeout, nullptr, 0);
        if (result == 0) {
            continue;
        }
        if (errno == EAGAIN) {
            return MYLITE_OWNERLESS_WAIT_OK;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ETIMEDOUT) {
            return mylite_ownerless_wait_load(word) == expected ? MYLITE_OWNERLESS_WAIT_TIMEOUT
                                                                : MYLITE_OWNERLESS_WAIT_OK;
        }
        return MYLITE_OWNERLESS_WAIT_ERROR;
    }
    return MYLITE_OWNERLESS_WAIT_OK;
}
#endif

#if !defined(__linux__)
int wait_with_backoff(
    mylite_ownerless_wait_word *word,
    std::uint32_t expected,
    unsigned timeout_ms
) {
    const auto deadline = wait_deadline(timeout_ms);
    auto sleep_time = std::chrono::microseconds(100);
    constexpr auto k_max_sleep_time = std::chrono::microseconds(2000);

    while (mylite_ownerless_wait_load(word) == expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return MYLITE_OWNERLESS_WAIT_TIMEOUT;
        }
        std::this_thread::sleep_for(sleep_time);
        sleep_time = std::min(sleep_time * 2, k_max_sleep_time);
    }
    return MYLITE_OWNERLESS_WAIT_OK;
}
#endif

#if defined(__linux__)
timespec timespec_from_milliseconds(unsigned timeout_ms) {
    timespec timeout = {};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000U);
    timeout.tv_nsec = static_cast<long>((timeout_ms % 1000U) * 1000000U);
    return timeout;
}
#endif

} // namespace

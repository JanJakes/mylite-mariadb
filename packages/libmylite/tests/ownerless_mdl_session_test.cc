#include "ownerless_latch.h"
#include "ownerless_lock_table.h"
#include "ownerless_mdl.h"
#include "ownerless_process_registry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <limits>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t k_active_count_offset = 16U;
constexpr std::size_t k_generation_offset = 8U;
constexpr std::size_t k_latch_offset = 24U;
constexpr std::size_t k_waiting_count_offset = 56U;
constexpr std::size_t k_wait_word_offset = 64U;
constexpr std::size_t k_entry_key_hash_offset = 0U;
constexpr std::size_t k_entry_owner_id_offset = 8U;
constexpr std::size_t k_entry_generation_offset = 24U;
constexpr std::size_t k_entry_state_offset = 12U;
constexpr std::size_t k_entry_mode_offset = 16U;
constexpr std::size_t k_entry_reference_count_offset = 32U;
constexpr std::size_t k_entry_session_id_offset = 48U;
constexpr std::uint32_t k_entry_count = 32U;
constexpr std::uint32_t k_waiting_state = 2U;
constexpr std::uint64_t k_key_one = 0x101U;
constexpr std::uint64_t k_key_two = 0x202U;
constexpr std::uint64_t k_key_three = 0x303U;

struct Identity {
    std::uint32_t owner;
    std::uint64_t generation;
    std::uint64_t session;
};

constexpr Identity k_a1{1U, 101U, 1001U};
constexpr Identity k_a2{1U, 101U, 1002U};
constexpr Identity k_b{2U, 202U, 2001U};
constexpr Identity k_c{3U, 303U, 3001U};

[[noreturn]] void fail(const char *expression, int line) {
    std::fprintf(stderr, "ownerless MDL session test failed at line %d: %s\n", line, expression);
    std::abort();
}

#define CHECK(expression) ((expression) ? static_cast<void>(0) : fail(#expression, __LINE__))

class LockTable {
  public:
    explicit LockTable(std::uint32_t entry_count = k_entry_count)
        : entry_count_(entry_count), bytes_(mylite_ownerless_lock_table_size(entry_count), 0U) {
        CHECK(
            mylite_ownerless_lock_table_initialize(bytes_.data(), bytes_.size(), entry_count) ==
            MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
    }

    void *data() {
        return bytes_.data();
    }

    std::size_t size() const {
        return bytes_.size();
    }

    std::uint32_t entry_count() const {
        return entry_count_;
    }

    std::uint64_t load64(std::size_t offset) const {
        const auto *value = reinterpret_cast<const std::uint64_t *>(bytes_.data() + offset);
        return __atomic_load_n(value, __ATOMIC_ACQUIRE);
    }

    std::uint32_t load32(std::size_t offset) const {
        const auto *value = reinterpret_cast<const std::uint32_t *>(bytes_.data() + offset);
        return __atomic_load_n(value, __ATOMIC_ACQUIRE);
    }

    void store64(std::size_t offset, std::uint64_t value) {
        auto *target = reinterpret_cast<std::uint64_t *>(bytes_.data() + offset);
        __atomic_store_n(target, value, __ATOMIC_RELEASE);
    }

    void store32(std::size_t offset, std::uint32_t value) {
        auto *target = reinterpret_cast<std::uint32_t *>(bytes_.data() + offset);
        __atomic_store_n(target, value, __ATOMIC_RELEASE);
    }

  private:
    std::uint32_t entry_count_;
    std::vector<unsigned char> bytes_;
};

int acquire(
    LockTable &table,
    std::uint64_t key,
    Identity identity,
    std::uint32_t mode,
    unsigned timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode_for_session(
        table.data(),
        table.size(),
        key,
        identity.owner,
        identity.generation,
        identity.session,
        mode,
        timeout_ms
    );
}

int acquire_with_options(
    LockTable &table,
    std::uint64_t key,
    Identity identity,
    std::uint32_t mode,
    int bypass_queued_waiters,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    unsigned timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode_for_session_with_options(
        table.data(),
        table.size(),
        key,
        identity.owner,
        identity.generation,
        identity.session,
        mode,
        bypass_queued_waiters,
        is_cancelled,
        cancel_context,
        timeout_ms
    );
}

int acquire_with_deadlock_weight(
    LockTable &table,
    std::uint64_t key,
    Identity identity,
    std::uint32_t mode,
    std::uint32_t deadlock_weight,
    unsigned timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode_for_session_with_deadlock_weight(
        table.data(),
        table.size(),
        key,
        identity.owner,
        identity.generation,
        identity.session,
        mode,
        0,
        deadlock_weight,
        nullptr,
        nullptr,
        timeout_ms
    );
}

int acquire_with_scheduling(
    LockTable &table,
    std::uint64_t key,
    Identity identity,
    std::uint32_t mode,
    std::uint32_t deadlock_weight,
    std::uint64_t max_write_lock_count,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode_for_session_with_scheduling(
        table.data(),
        table.size(),
        key,
        identity.owner,
        identity.generation,
        identity.session,
        mode,
        0,
        deadlock_weight,
        max_write_lock_count,
        is_cancelled,
        cancel_context,
        timeout_ms
    );
}

int release(LockTable &table, std::uint64_t key, Identity identity, std::uint32_t mode) {
    return mylite_ownerless_lock_table_release_mode_for_session(
        table.data(),
        table.size(),
        key,
        identity.owner,
        identity.generation,
        identity.session,
        mode
    );
}

std::uint32_t matching_active_entry_count(
    const LockTable &table,
    std::uint64_t key,
    Identity identity,
    std::uint32_t mode
) {
    std::uint32_t result = 0U;
    for (std::uint32_t index = 0U; index < table.entry_count(); ++index) {
        const std::size_t entry_offset = MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE +
                                         (index * MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE);
        if (table.load32(entry_offset + k_entry_state_offset) == 1U &&
            table.load64(entry_offset + k_entry_key_hash_offset) == key &&
            table.load32(entry_offset + k_entry_owner_id_offset) == identity.owner &&
            table.load64(entry_offset + k_entry_session_id_offset) == identity.session &&
            table.load32(entry_offset + k_entry_mode_offset) == mode) {
            ++result;
        }
    }
    return result;
}

void wait_for_waiters(const LockTable &table, std::uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (table.load64(k_waiting_count_offset) != expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("waiter publication", __LINE__);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int get_future(std::future<int> &future) {
    CHECK(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    return future.get();
}

std::uint64_t waiting_sequence_for_session(const LockTable &table, std::uint64_t session_id) {
    for (std::uint32_t index = 0U; index < table.entry_count(); ++index) {
        const std::size_t entry_offset = MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE +
                                         (index * MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE);
        if (table.load32(entry_offset + k_entry_state_offset) == k_waiting_state &&
            table.load64(entry_offset + k_entry_session_id_offset) == session_id) {
            return table.load64(entry_offset + k_entry_generation_offset);
        }
    }
    return 0U;
}

int cancellation_requested(void *context) {
    const auto *cancelled = static_cast<const std::atomic<bool> *>(context);
    return cancelled->load(std::memory_order_acquire) ? 1 : 0;
}

int cancellation_requested_after_wait(void *context) {
    auto *calls = static_cast<std::atomic<unsigned> *>(context);
    return calls->fetch_add(1U, std::memory_order_acq_rel) == 0U ? 0 : 1;
}

void test_same_process_sessions_are_distinct() {
    LockTable table;
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    std::uint32_t active_count = 0U;
    CHECK(
        mylite_ownerless_lock_table_owner_active_count(
            table.data(),
            table.size(),
            k_a1.owner,
            k_b.owner,
            k_b.generation,
            &active_count
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(active_count == 2U);

    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 20U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    CHECK(
        release(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_same_process_sessions_do_not_form_false_cycle() {
    LockTable table;
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    auto a2_wait = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_two, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 1000U);
    });
    wait_for_waiters(table, 1U);
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 50U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    CHECK(
        release(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(a2_wait) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_two, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_three_session_cycle_is_detected() {
    LockTable table;
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_two, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_three, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    auto a1_wait = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_three, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 1000U);
    });
    wait_for_waiters(table, 1U);
    auto b_wait = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 1000U);
    });
    wait_for_waiters(table, 2U);
    CHECK(
        acquire(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 1000U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_DEADLOCK
    );

    CHECK(
        release(table, k_key_two, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(b_wait) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_three, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(a1_wait) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_three, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_weighted_deadlock_signals_existing_victim() {
    LockTable table;
    constexpr std::uint32_t dml_weight = 1U;
    constexpr std::uint32_t ddl_weight = 100U;
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    auto low_weight_waiter = std::async(std::launch::async, [&table]() {
        return acquire_with_deadlock_weight(
            table,
            k_key_two,
            k_a1,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            dml_weight,
            2000U
        );
    });
    wait_for_waiters(table, 1U);
    auto high_weight_waiter = std::async(std::launch::async, [&table]() {
        return acquire_with_deadlock_weight(
            table,
            k_key_one,
            k_b,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            ddl_weight,
            2000U
        );
    });

    CHECK(get_future(low_weight_waiter) == MYLITE_OWNERLESS_LOCK_TABLE_DEADLOCK);
    wait_for_waiters(table, 1U);
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(high_weight_waiter) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_deadlock_detection_breaks_every_cycle_closed_by_request() {
    LockTable table;
    constexpr std::uint32_t first_victim_weight = 1U;
    constexpr std::uint32_t second_victim_weight = 2U;
    constexpr std::uint32_t requester_weight = 100U;

    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_two, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_three, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_three, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    auto first_cycle = std::async(std::launch::async, [&table]() {
        return acquire_with_deadlock_weight(
            table,
            k_key_one,
            k_b,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            first_victim_weight,
            2000U
        );
    });
    wait_for_waiters(table, 1U);
    auto second_cycle = std::async(std::launch::async, [&table]() {
        return acquire_with_deadlock_weight(
            table,
            k_key_two,
            k_c,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            second_victim_weight,
            2000U
        );
    });
    wait_for_waiters(table, 2U);
    auto requester = std::async(std::launch::async, [&table]() {
        return acquire_with_deadlock_weight(
            table,
            k_key_three,
            k_a1,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            requester_weight,
            2000U
        );
    });

    CHECK(get_future(first_cycle) == MYLITE_OWNERLESS_LOCK_TABLE_DEADLOCK);
    CHECK(get_future(second_cycle) == MYLITE_OWNERLESS_LOCK_TABLE_DEADLOCK);
    CHECK(
        release(table, k_key_three, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_three, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(requester) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_three, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_two, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_max_write_lock_count_breaks_priority_starvation() {
    LockTable table;
    constexpr std::uint64_t max_write_locks = 1U;
    CHECK(
        acquire(table, k_key_one, k_c, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    auto reader = std::async(std::launch::async, [&table]() {
        return acquire_with_scheduling(
            table,
            k_key_one,
            k_a1,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
            1U,
            max_write_locks,
            nullptr,
            nullptr,
            2000U
        );
    });
    wait_for_waiters(table, 1U);
    auto first_writer = std::async(std::launch::async, [&table]() {
        return acquire_with_scheduling(
            table,
            k_key_one,
            k_a2,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            1U,
            max_write_locks,
            nullptr,
            nullptr,
            2000U
        );
    });
    wait_for_waiters(table, 2U);
    auto second_writer = std::async(std::launch::async, [&table]() {
        return acquire_with_scheduling(
            table,
            k_key_one,
            k_b,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            1U,
            max_write_locks,
            nullptr,
            nullptr,
            2000U
        );
    });
    wait_for_waiters(table, 3U);

    CHECK(
        release(table, k_key_one, k_c, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(first_writer) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(reader.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(second_writer.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(
        release(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(reader) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(second_writer.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(second_writer) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_large_timeout_is_not_truncated_to_zero() {
    LockTable table;
    CHECK(
        acquire_with_scheduling(
            table,
            k_key_one,
            k_a1,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
            1U,
            0U,
            nullptr,
            nullptr,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_ERROR
    );
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    std::atomic<unsigned> cancellation_calls{0U};
    constexpr std::uint64_t timeout_ms =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
    CHECK(
        acquire_with_scheduling(
            table,
            k_key_one,
            k_a1,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
            1U,
            std::numeric_limits<std::uint64_t>::max(),
            cancellation_requested_after_wait,
            &cancellation_calls,
            timeout_ms
        ) == MYLITE_OWNERLESS_LOCK_TABLE_KILLED
    );
    CHECK(cancellation_calls.load(std::memory_order_acquire) >= 2U);
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_stale_session_and_generation_cannot_release() {
    LockTable table;
    constexpr Identity original{1U, 300U, 900U};
    constexpr Identity wrong_session{1U, 300U, 901U};
    constexpr Identity reused_token{1U, 301U, 900U};

    CHECK(
        acquire(table, k_key_one, original, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, wrong_session, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_NOT_FOUND
    );
    CHECK(
        release(table, k_key_one, reused_token, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_NOT_FOUND
    );
    CHECK(
        release(table, k_key_one, original, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    CHECK(
        acquire(table, k_key_one, reused_token, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, original, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_NOT_FOUND
    );
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 20U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    CHECK(
        release(table, k_key_one, reused_token, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_fifo_upgrade_blocks_late_reader() {
    LockTable table;
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    auto upgrade = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 1000U);
    });
    wait_for_waiters(table, 1U);
    CHECK(
        acquire(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 50U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(upgrade) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_native_wait_priority_grants_later_exclusive_first() {
    LockTable table;
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    auto reader = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 2000U);
    });
    wait_for_waiters(table, 1U);
    auto exclusive = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 2000U);
    });
    wait_for_waiters(table, 2U);

    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(exclusive) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    wait_for_waiters(table, 1U);
    CHECK(waiting_sequence_for_session(table, k_a1.session) != 0U);
    CHECK(
        release(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(reader) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_downgrade_bypasses_queued_exclusive() {
    LockTable table(k_entry_count + 1U);
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    auto writer = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 2000U);
    });
    wait_for_waiters(table, 1U);
    constexpr std::uint64_t filler_key = 0x30000U;
    const std::uint32_t filler_count = table.entry_count() - 2U;
    for (std::uint32_t index = 0U; index < filler_count; ++index) {
        CHECK(
            acquire(table, filler_key + index, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
            MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
    }
    CHECK(table.load64(k_active_count_offset) == table.entry_count() - 1U);
    CHECK(table.load64(k_waiting_count_offset) == 1U);

    CHECK(
        mylite_ownerless_lock_table_reclassify_mode_for_session(
            table.data(),
            table.size(),
            k_key_one,
            k_a1.owner,
            k_a1.generation,
            k_a1.session,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(table.load64(k_active_count_offset) == table.entry_count() - 1U);
    CHECK(table.load64(k_waiting_count_offset) == 1U);
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(writer) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    for (std::uint32_t index = 0U; index < filler_count; ++index) {
        CHECK(
            release(table, filler_key + index, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
            MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
    }
}

void test_downgrade_balances_existing_granted_mode() {
    LockTable table(4U);
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    auto writer = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 2000U);
    });
    wait_for_waiters(table, 1U);
    CHECK(
        acquire(table, k_key_two, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(table.load64(k_active_count_offset) == 3U);
    CHECK(table.load64(k_waiting_count_offset) == 1U);

    CHECK(
        mylite_ownerless_lock_table_reclassify_mode_for_session(
            table.data(),
            table.size(),
            k_key_one,
            k_a1.owner,
            k_a1.generation,
            k_a1.session,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(table.load64(k_active_count_offset) == 3U);
    CHECK(table.load64(k_waiting_count_offset) == 1U);

    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(table.load64(k_waiting_count_offset) == 1U);
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(writer) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_two, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_full_table_downgrade_keeps_balanced_references() {
    LockTable table(4U);
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_three, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(table.load64(k_active_count_offset) == table.entry_count());

    CHECK(
        mylite_ownerless_lock_table_reclassify_mode_for_session(
            table.data(),
            table.size(),
            k_key_one,
            k_a1.owner,
            k_a1.generation,
            k_a1.session,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(table.load64(k_active_count_offset) == table.entry_count());
    CHECK(
        matching_active_entry_count(
            table,
            k_key_one,
            k_a1,
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE
        ) == 1U
    );
    CHECK(
        matching_active_entry_count(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        1U
    );

    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 20U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_three, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_equal_priority_mixed_modes_follow_fifo() {
    LockTable table;
    CHECK(
        acquire(table, k_key_one, k_c, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    auto first = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE, 2000U);
    });
    wait_for_waiters(table, 1U);
    auto second = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE, 2000U);
    });
    wait_for_waiters(table, 2U);
    const std::uint64_t first_sequence = waiting_sequence_for_session(table, k_a1.session);
    const std::uint64_t second_sequence = waiting_sequence_for_session(table, k_b.session);
    CHECK(first_sequence != 0U);
    CHECK(first_sequence < second_sequence);

    CHECK(
        release(table, k_key_one, k_c, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(first) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    wait_for_waiters(table, 1U);
    CHECK(waiting_sequence_for_session(table, k_b.session) == second_sequence);
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(second) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_operation_deadline_bounds_latch_fault() {
    LockTable table;
    auto *latch = reinterpret_cast<mylite_ownerless_latch *>(
        static_cast<unsigned char *>(table.data()) + k_latch_offset
    );
    CHECK(
        mylite_ownerless_latch_acquire(latch, k_c.owner, k_c.generation, nullptr, nullptr, 0U) ==
        MYLITE_OWNERLESS_LATCH_OK
    );

    const auto start = std::chrono::steady_clock::now();
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 30U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_ERROR
    );
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    CHECK(
        mylite_ownerless_latch_release(latch, k_c.owner, k_c.generation) ==
        MYLITE_OWNERLESS_LATCH_OK
    );
}

void test_release_pending_retains_owner_cleanup_identity() {
    LockTable table;
    mylite_ownerless_latch_test_inject_release_pending_once();
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_APPLIED_RELEASE_PENDING
    );

    std::uint32_t active_count = 0U;
    CHECK(
        mylite_ownerless_lock_table_owner_active_count(
            table.data(),
            table.size(),
            k_a1.owner,
            k_a1.owner,
            k_a1.generation,
            &active_count
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(active_count == 1U);
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    mylite_ownerless_latch_test_inject_release_pending_once();
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_APPLIED_RELEASE_PENDING
    );
    CHECK(
        mylite_ownerless_lock_table_owner_active_count(
            table.data(),
            table.size(),
            k_a1.owner,
            k_a1.owner,
            k_a1.generation,
            &active_count
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(active_count == 0U);
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_owner_release_retry_is_idempotent() {
    LockTable table;
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_two, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    std::uint32_t released = 0U;
    mylite_ownerless_latch_test_inject_release_pending_once();
    CHECK(
        mylite_ownerless_lock_table_release_owner(
            table.data(),
            table.size(),
            k_a1.owner,
            k_a1.owner,
            k_a1.generation,
            &released
        ) == MYLITE_OWNERLESS_LOCK_TABLE_APPLIED_RELEASE_PENDING
    );
    CHECK(released == 2U);

    released = 17U;
    CHECK(
        mylite_ownerless_lock_table_release_owner(
            table.data(),
            table.size(),
            k_a1.owner,
            k_a1.owner,
            k_a1.generation,
            &released
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(released == 0U);
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_empty_owner_release_pending_retry_is_idempotent() {
    LockTable table;
    std::uint32_t released = 17U;

    mylite_ownerless_latch_test_inject_release_pending_once();
    CHECK(
        mylite_ownerless_lock_table_release_owner(
            table.data(),
            table.size(),
            k_a1.owner,
            k_a1.owner,
            k_a1.generation,
            &released
        ) == MYLITE_OWNERLESS_LOCK_TABLE_APPLIED_RELEASE_PENDING
    );
    CHECK(released == 0U);

    released = 17U;
    CHECK(
        mylite_ownerless_lock_table_release_owner(
            table.data(),
            table.size(),
            k_a1.owner,
            k_a1.owner,
            k_a1.generation,
            &released
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(released == 0U);
}

void test_unrecoverable_latch_fails_closed() {
    LockTable table;
    auto *latch = reinterpret_cast<mylite_ownerless_latch *>(
        static_cast<unsigned char *>(table.data()) + k_latch_offset
    );
    latch->owner_generation = k_c.generation;
    latch->state_owner =
        (static_cast<std::uint64_t>(k_c.owner) << 32U) | MYLITE_OWNERLESS_LATCH_STATE_OWNER_DEAD;

    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 10000U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_ERROR
    );
}

void test_cancellation_preserves_published_fifo_position() {
    LockTable table;
    const std::uint64_t key = mylite_ownerless_mdl_key_hash(
        MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
        "app",
        "cancelled_wait"
    );
    CHECK(
        acquire(table, key, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    std::atomic<bool> cancelled{false};
    auto cancelled_waiter = std::async(std::launch::async, [&table, &cancelled]() {
        const mylite_ownerless_mdl_wait_options options = {
            cancellation_requested,
            &cancelled,
            0U,
            MYLITE_OWNERLESS_LOCK_TABLE_DEFAULT_DEADLOCK_WEIGHT,
            0U,
            std::numeric_limits<std::uint64_t>::max(),
        };
        return mylite_ownerless_mdl_acquire_mode_for_session_with_options(
            table.data(),
            table.size(),
            k_a1.owner,
            k_a1.generation,
            k_a1.session,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "cancelled_wait",
            MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
            &options,
            5000U
        );
    });
    wait_for_waiters(table, 1U);
    const std::uint64_t initial_sequence = waiting_sequence_for_session(table, k_a1.session);
    CHECK(initial_sequence != 0U);

    auto later_waiter = std::async(std::launch::async, [&table, key]() {
        return acquire(table, key, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 2000U);
    });
    wait_for_waiters(table, 2U);
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    CHECK(waiting_sequence_for_session(table, k_a1.session) == initial_sequence);

    cancelled.store(true, std::memory_order_release);
    CHECK(get_future(cancelled_waiter) == MYLITE_OWNERLESS_LOCK_TABLE_KILLED);
    wait_for_waiters(table, 1U);
    CHECK(waiting_sequence_for_session(table, k_a1.session) == 0U);
    CHECK(waiting_sequence_for_session(table, k_a2.session) > initial_sequence);

    CHECK(
        release(table, key, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(later_waiter) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, key, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_high_priority_shared_bypasses_only_queued_writer() {
    LockTable table;
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    auto writer = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 2000U);
    });
    wait_for_waiters(table, 1U);

    CHECK(
        acquire(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 50U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    CHECK(
        acquire(table, k_key_one, k_a2, MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ, 50U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    CHECK(
        acquire_with_options(
            table,
            k_key_one,
            k_c,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
            1,
            nullptr,
            nullptr,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(writer) == MYLITE_OWNERLESS_LOCK_TABLE_OK);

    CHECK(
        acquire_with_options(
            table,
            k_key_one,
            k_c,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
            1,
            nullptr,
            nullptr,
            50U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_production_table_holds_129_distinct_table_keys() {
    constexpr std::uint32_t held_table_count = 129U;
    LockTable table(MYLITE_OWNERLESS_LOCK_TABLE_PRODUCTION_ENTRY_COUNT);
    std::vector<std::uint64_t> keys;
    keys.reserve(held_table_count);

    for (std::uint32_t index = 0U; index < held_table_count; ++index) {
        char table_name[32];
        CHECK(std::snprintf(table_name, sizeof(table_name), "table_%03u", index) > 0);
        const std::uint64_t key =
            mylite_ownerless_mdl_key_hash(MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE, "app", table_name);
        CHECK(key != 0U);
        keys.push_back(key);
        CHECK(
            acquire(table, key, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ, 0U) ==
            MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
    }
    CHECK(table.load64(k_active_count_offset) == held_table_count);

    const std::uint64_t peer_key =
        mylite_ownerless_mdl_key_hash(MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE, "app", "peer_table");
    CHECK(
        acquire(table, peer_key, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(table.load64(k_active_count_offset) == held_table_count + 1U);
    CHECK(
        release(table, peer_key, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    for (const std::uint64_t key : keys) {
        CHECK(
            release(table, key, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ) ==
            MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
    }
    CHECK(table.load64(k_active_count_offset) == 0U);
}

void test_capacity_exhaustion_returns_full() {
    LockTable table(2U);
    CHECK(
        acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        acquire(table, k_key_three, k_c, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 2000U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_FULL
    );
    CHECK(
        release(table, k_key_two, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

void test_dead_latch_recovery_preserves_waiter() {
    LockTable table;
    CHECK(
        acquire(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE, 0U) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    auto waiter = std::async(std::launch::async, [&table]() {
        return acquire(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED, 1000U);
    });
    wait_for_waiters(table, 1U);

    auto *latch = reinterpret_cast<mylite_ownerless_latch *>(
        static_cast<unsigned char *>(table.data()) + k_latch_offset
    );
    CHECK(
        mylite_ownerless_latch_acquire(latch, 3U, 303U, nullptr, nullptr, 1000U) ==
        MYLITE_OWNERLESS_LATCH_OK
    );
    table.store64(k_active_count_offset, 17U);
    table.store64(k_waiting_count_offset, 19U);
    table.store32(k_wait_word_offset, 41U);
    bool damaged_waiter = false;
    std::uint64_t max_entry_generation = 0U;
    for (std::uint32_t index = 0U; index < k_entry_count; ++index) {
        const std::size_t entry_offset = MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE +
                                         (index * MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE);
        const std::uint32_t state = table.load32(entry_offset + k_entry_state_offset);
        if (state != 0U) {
            const std::uint64_t generation = table.load64(entry_offset + k_entry_generation_offset);
            max_entry_generation = std::max(generation, max_entry_generation);
        }
        if (state == k_waiting_state) {
            table.store32(entry_offset + k_entry_reference_count_offset, 1U);
            damaged_waiter = true;
        }
    }
    CHECK(damaged_waiter);
    CHECK(max_entry_generation != 0U);
    table.store64(k_generation_offset, 0U);

    constexpr std::uint32_t process_slots = 8U;
    std::vector<unsigned char> registry(mylite_ownerless_process_registry_size(process_slots), 0U);
    CHECK(
        mylite_ownerless_process_registry_initialize(
            registry.data(),
            registry.size(),
            process_slots
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    mylite_ownerless_process_registry_liveness_context liveness =
        {registry.data(), registry.size(), nullptr, nullptr};
    CHECK(
        mylite_ownerless_lock_table_recover_dead_latch(
            table.data(),
            table.size(),
            4U,
            404U,
            &liveness
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(table.load64(k_active_count_offset) == 1U);
    CHECK(table.load64(k_waiting_count_offset) == 1U);
    CHECK(table.load64(k_generation_offset) > max_entry_generation);
    CHECK(table.load32(k_wait_word_offset) != 41U);

    CHECK(
        release(table, k_key_one, k_b, MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    CHECK(get_future(waiter) == MYLITE_OWNERLESS_LOCK_TABLE_OK);
    CHECK(
        release(table, k_key_one, k_a1, MYLITE_OWNERLESS_LOCK_TABLE_SHARED) ==
        MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

} // namespace

int main() {
    test_same_process_sessions_are_distinct();
    test_same_process_sessions_do_not_form_false_cycle();
    test_three_session_cycle_is_detected();
    test_weighted_deadlock_signals_existing_victim();
    test_deadlock_detection_breaks_every_cycle_closed_by_request();
    test_max_write_lock_count_breaks_priority_starvation();
    test_large_timeout_is_not_truncated_to_zero();
    test_stale_session_and_generation_cannot_release();
    test_fifo_upgrade_blocks_late_reader();
    test_native_wait_priority_grants_later_exclusive_first();
    test_downgrade_bypasses_queued_exclusive();
    test_downgrade_balances_existing_granted_mode();
    test_full_table_downgrade_keeps_balanced_references();
    test_equal_priority_mixed_modes_follow_fifo();
    test_operation_deadline_bounds_latch_fault();
    test_release_pending_retains_owner_cleanup_identity();
    test_owner_release_retry_is_idempotent();
    test_empty_owner_release_pending_retry_is_idempotent();
    test_unrecoverable_latch_fails_closed();
    test_cancellation_preserves_published_fifo_position();
    test_high_priority_shared_bypasses_only_queued_writer();
    test_production_table_holds_129_distinct_table_keys();
    test_capacity_exhaustion_returns_full();
    test_dead_latch_recovery_preserves_waiter();
    return 0;
}

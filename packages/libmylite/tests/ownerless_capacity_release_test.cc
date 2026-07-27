#include "ownerless_innodb_lock_registry.h"
#include "ownerless_latch.h"
#include "ownerless_read_view_registry.h"
#include "ownerless_trx_registry.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t k_owner_id = 7;
constexpr std::uint64_t k_owner_generation = 11;
constexpr std::size_t k_innodb_registry_latch_offset = 24;
constexpr std::uint64_t k_record_index_id = 17;
constexpr std::uint32_t k_record_space_id = 3;
constexpr std::uint32_t k_record_page_no = 5;
constexpr std::uint32_t k_record_heap_no = 7;

int acquire_record(
    std::vector<unsigned char> &mapping,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    unsigned timeout_ms
) {
    return mylite_ownerless_innodb_lock_registry_acquire_record(
        mapping.data(),
        mapping.size(),
        owner_id,
        static_cast<std::uint64_t>(owner_id) * 100U,
        trx_id,
        k_record_index_id,
        k_record_space_id,
        k_record_page_no,
        k_record_heap_no,
        MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
        timeout_ms
    );
}

int release_record(
    std::vector<unsigned char> &mapping,
    std::uint32_t owner_id,
    std::uint64_t trx_id
) {
    return mylite_ownerless_innodb_lock_registry_release_record(
        mapping.data(),
        mapping.size(),
        owner_id,
        static_cast<std::uint64_t>(owner_id) * 100U,
        trx_id,
        k_record_index_id,
        k_record_space_id,
        k_record_page_no,
        k_record_heap_no,
        MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
    );
}

int publish_record_wait(
    std::vector<unsigned char> &mapping,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint32_t blocker_owner_id,
    std::uint64_t blocker_trx_id
) {
    return mylite_ownerless_innodb_lock_registry_wait_for_record(
        mapping.data(),
        mapping.size(),
        owner_id,
        static_cast<std::uint64_t>(owner_id) * 100U,
        trx_id,
        k_record_index_id,
        k_record_space_id,
        k_record_page_no,
        k_record_heap_no,
        MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
        blocker_owner_id,
        blocker_trx_id
    );
}

void test_trx_capacity_and_release() {
    const std::size_t size = mylite_ownerless_trx_registry_size(1);
    std::vector<unsigned char> mapping(size);
    assert(
        mylite_ownerless_trx_registry_initialize(mapping.data(), mapping.size(), 1, 100) ==
        MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );

    std::uint64_t trx_id = 0;
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    assert(
        mylite_ownerless_trx_registry_begin(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            &trx_id,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );

    std::uint64_t full_id = 1;
    std::uint32_t full_slot = 1;
    std::uint64_t full_generation = 1;
    assert(
        mylite_ownerless_trx_registry_begin(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            &full_id,
            &full_slot,
            &full_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_FULL
    );
    assert(full_id == 0 && full_slot == 0 && full_generation == 0);

    assert(
        mylite_ownerless_trx_registry_end(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            slot,
            generation + 1
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND
    );
    assert(
        mylite_ownerless_trx_registry_end(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            slot,
            generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_end(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            slot,
            generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );

    std::uint64_t next_id = 0;
    std::uint32_t next_slot = 0;
    std::uint64_t next_generation = 0;
    assert(
        mylite_ownerless_trx_registry_begin(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            &next_id,
            &next_slot,
            &next_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(next_id > trx_id && next_generation != generation);
    assert(
        mylite_ownerless_trx_registry_end(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            slot,
            generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND
    );
}

void test_read_view_capacity_snapshot_and_release() {
    const std::size_t size = mylite_ownerless_read_view_registry_size(2);
    std::vector<unsigned char> mapping(size);
    assert(
        mylite_ownerless_read_view_registry_initialize(mapping.data(), mapping.size(), 2) ==
        MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );

    const std::uint64_t first_ids[] = {5, 7};
    const std::uint64_t second_ids[] = {5, 6};
    std::uint32_t first_slot = 0;
    std::uint64_t first_generation = 0;
    std::uint32_t second_slot = 0;
    std::uint64_t second_generation = 0;
    assert(
        mylite_ownerless_read_view_registry_open(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            10,
            9,
            first_ids,
            2,
            &first_slot,
            &first_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_open(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            8,
            7,
            second_ids,
            2,
            &second_slot,
            &second_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );

    std::uint32_t count = 0;
    std::uint64_t low_limit_id = 0;
    std::uint64_t low_limit_no = 0;
    assert(
        mylite_ownerless_read_view_registry_snapshot_oldest(
            mapping.data(),
            mapping.size(),
            nullptr,
            0,
            k_owner_id,
            k_owner_generation,
            &count,
            &low_limit_id,
            &low_limit_no
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL
    );
    assert(count == 3);
    std::vector<std::uint64_t> ids(count);
    assert(
        mylite_ownerless_read_view_registry_snapshot_oldest(
            mapping.data(),
            mapping.size(),
            ids.data(),
            static_cast<std::uint32_t>(ids.size()),
            k_owner_id,
            k_owner_generation,
            &count,
            &low_limit_id,
            &low_limit_no
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert((ids == std::vector<std::uint64_t>{5, 6, 7}));
    assert(low_limit_id == 8 && low_limit_no == 7);

    std::uint32_t full_slot = 1;
    std::uint64_t full_generation = 1;
    assert(
        mylite_ownerless_read_view_registry_open(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            12,
            11,
            nullptr,
            0,
            &full_slot,
            &full_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL
    );
    assert(full_slot == 0 && full_generation == 0);

    assert(
        mylite_ownerless_read_view_registry_close(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            first_slot,
            first_generation + 1
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_NOT_FOUND
    );
    assert(
        mylite_ownerless_read_view_registry_close(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_close(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );

    std::uint32_t reused_slot = 0;
    std::uint64_t reused_generation = 0;
    assert(
        mylite_ownerless_read_view_registry_open(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            12,
            11,
            nullptr,
            0,
            &reused_slot,
            &reused_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(reused_slot == first_slot && reused_generation != first_generation);
    assert(
        mylite_ownerless_read_view_registry_close(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_NOT_FOUND
    );
}

void test_trx_applied_release_pending_is_retryable() {
    const std::size_t size = mylite_ownerless_trx_registry_size(1);
    std::vector<unsigned char> mapping(size);
    assert(
        mylite_ownerless_trx_registry_initialize(mapping.data(), mapping.size(), 1, 200) ==
        MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );

    std::uint64_t trx_id = 0;
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    mylite_ownerless_latch_test_inject_release_pending_once();
    assert(
        mylite_ownerless_trx_registry_begin(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            &trx_id,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_APPLIED_RELEASE_PENDING
    );
    assert(trx_id == 200U && generation != 0U);
    assert(mylite_ownerless_trx_registry_active_count(mapping.data()) == 1U);

    std::uint64_t duplicate_id = 1;
    std::uint32_t duplicate_slot = 1;
    std::uint64_t duplicate_generation = 1;
    assert(
        mylite_ownerless_trx_registry_begin(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            &duplicate_id,
            &duplicate_slot,
            &duplicate_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_FULL
    );
    assert(duplicate_id == 0U && duplicate_slot == 0U && duplicate_generation == 0U);
    assert(mylite_ownerless_trx_registry_active_count(mapping.data()) == 1U);
    assert(
        mylite_ownerless_trx_registry_end(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            slot,
            generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );

    std::uint64_t reused_id = 0;
    std::uint32_t reused_slot = 0;
    std::uint64_t reused_generation = 0;
    assert(
        mylite_ownerless_trx_registry_begin(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            &reused_id,
            &reused_slot,
            &reused_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(reused_slot == slot && reused_generation != generation && reused_id > trx_id);

    mylite_ownerless_latch_test_inject_release_pending_once();
    assert(
        mylite_ownerless_trx_registry_end(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            reused_slot,
            reused_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_APPLIED_RELEASE_PENDING
    );
    assert(mylite_ownerless_trx_registry_active_count(mapping.data()) == 0U);
    assert(
        mylite_ownerless_trx_registry_end(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            reused_slot,
            reused_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_end(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            slot,
            generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND
    );
}

void test_read_view_applied_release_pending_is_retryable() {
    const std::size_t size = mylite_ownerless_read_view_registry_size(1);
    std::vector<unsigned char> mapping(size);
    assert(
        mylite_ownerless_read_view_registry_initialize(mapping.data(), mapping.size(), 1) ==
        MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );

    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    mylite_ownerless_latch_test_inject_release_pending_once();
    assert(
        mylite_ownerless_read_view_registry_open(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            20,
            19,
            nullptr,
            0,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_APPLIED_RELEASE_PENDING
    );
    assert(generation != 0U);
    assert(mylite_ownerless_read_view_registry_active_count(mapping.data()) == 1U);

    std::uint32_t duplicate_slot = 1;
    std::uint64_t duplicate_generation = 1;
    assert(
        mylite_ownerless_read_view_registry_open(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            21,
            20,
            nullptr,
            0,
            &duplicate_slot,
            &duplicate_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL
    );
    assert(duplicate_slot == 0U && duplicate_generation == 0U);
    assert(mylite_ownerless_read_view_registry_active_count(mapping.data()) == 1U);
    assert(
        mylite_ownerless_read_view_registry_close(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            slot,
            generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );

    std::uint32_t reused_slot = 0;
    std::uint64_t reused_generation = 0;
    assert(
        mylite_ownerless_read_view_registry_open(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            22,
            21,
            nullptr,
            0,
            &reused_slot,
            &reused_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(reused_slot == slot && reused_generation != generation);

    mylite_ownerless_latch_test_inject_release_pending_once();
    assert(
        mylite_ownerless_read_view_registry_close(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            reused_slot,
            reused_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_APPLIED_RELEASE_PENDING
    );
    assert(mylite_ownerless_read_view_registry_active_count(mapping.data()) == 0U);
    assert(
        mylite_ownerless_read_view_registry_close(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            reused_slot,
            reused_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_close(
            mapping.data(),
            mapping.size(),
            k_owner_id,
            k_owner_generation,
            slot,
            generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_NOT_FOUND
    );
}

void test_innodb_waiters_keep_generation_fifo() {
    const std::size_t size = mylite_ownerless_innodb_lock_registry_size(8);
    std::vector<unsigned char> mapping(size);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(mapping.data(), mapping.size(), 8) ==
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );

    assert(acquire_record(mapping, 1, 101, 0) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);
    assert(
        publish_record_wait(mapping, 2, 202, 1, 101) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        publish_record_wait(mapping, 3, 303, 1, 101) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );

    /* Refreshing the blocker must not move the oldest waiter to the queue tail. */
    assert(
        publish_record_wait(mapping, 2, 202, 1, 101) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(release_record(mapping, 1, 101) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);

    assert(
        mylite_ownerless_innodb_lock_registry_wait_until_record_available(
            mapping.data(),
            mapping.size(),
            2,
            200,
            202,
            k_record_index_id,
            k_record_space_id,
            k_record_page_no,
            k_record_heap_no,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(acquire_record(mapping, 3, 303, 0) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT);
    assert(acquire_record(mapping, 2, 202, 0) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);
    assert(release_record(mapping, 2, 202) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);
    assert(acquire_record(mapping, 3, 303, 0) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);
    assert(release_record(mapping, 3, 303) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);
    assert(mylite_ownerless_innodb_lock_registry_active_count(mapping.data()) == 0U);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(mapping.data()) == 0U);
}

void test_cross_registry_cycle_check_does_not_hold_primary_latch() {
    const std::size_t size = mylite_ownerless_innodb_lock_registry_size(8);
    std::vector<unsigned char> primary(size);
    std::vector<unsigned char> cycle(size);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(primary.data(), primary.size(), 8) ==
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(cycle.data(), cycle.size(), 8) ==
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(acquire_record(primary, 1, 101, 0) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);

    auto *cycle_latch =
        reinterpret_cast<mylite_ownerless_latch *>(cycle.data() + k_innodb_registry_latch_offset);
    auto *primary_latch =
        reinterpret_cast<mylite_ownerless_latch *>(primary.data() + k_innodb_registry_latch_offset);
    assert(
        mylite_ownerless_latch_acquire(cycle_latch, 9, 900, nullptr, nullptr, 0) ==
        MYLITE_OWNERLESS_LATCH_OK
    );

    std::atomic<int> wait_result{MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR};
    std::thread waiter([&]() {
        int result =
            mylite_ownerless_innodb_lock_registry_wait_until_record_available_with_cycle_registry(
                primary.data(),
                primary.size(),
                cycle.data(),
                cycle.size(),
                2,
                200,
                202,
                k_record_index_id,
                k_record_space_id,
                k_record_page_no,
                k_record_heap_no,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
                2000
            );
        if (result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            result = acquire_record(primary, 2, 202, 0);
        }
        if (result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            result = release_record(primary, 2, 202);
        }
        wait_result.store(result, std::memory_order_release);
    });

    for (unsigned iteration = 0;
         iteration < 1000 &&
         mylite_ownerless_innodb_lock_registry_waiting_count(primary.data()) == 0U;
         ++iteration) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(primary.data()) == 1U);

    assert(
        mylite_ownerless_latch_acquire(primary_latch, 8, 800, nullptr, nullptr, 100) ==
        MYLITE_OWNERLESS_LATCH_OK
    );
    assert(mylite_ownerless_latch_release(primary_latch, 8, 800) == MYLITE_OWNERLESS_LATCH_OK);
    assert(mylite_ownerless_latch_release(cycle_latch, 9, 900) == MYLITE_OWNERLESS_LATCH_OK);
    assert(release_record(primary, 1, 101) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);

    waiter.join();
    assert(wait_result.load(std::memory_order_acquire) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(primary.data()) == 0U);
}

void test_cross_registry_snapshot_detects_cycle() {
    const std::size_t size = mylite_ownerless_innodb_lock_registry_size(8);
    std::vector<unsigned char> row_registry(size);
    std::vector<unsigned char> page_registry(size);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            row_registry.data(),
            row_registry.size(),
            8
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            page_registry.data(),
            page_registry.size(),
            8
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(acquire_record(row_registry, 1, 101, 0) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);
    assert(
        publish_record_wait(page_registry, 1, 101, 2, 202) ==
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );

    assert(
        mylite_ownerless_innodb_lock_registry_wait_until_record_available_with_cycle_registry(
            row_registry.data(),
            row_registry.size(),
            page_registry.data(),
            page_registry.size(),
            2,
            200,
            202,
            k_record_index_id,
            k_record_space_id,
            k_record_page_no,
            k_record_heap_no,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            1000
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(row_registry.data()) == 0U);

    std::uint32_t cleared_waits = 0;
    assert(
        mylite_ownerless_innodb_lock_registry_clear_wait(
            page_registry.data(),
            page_registry.size(),
            1,
            100,
            101,
            &cleared_waits
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(cleared_waits == 1U);
    assert(release_record(row_registry, 1, 101) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK);
}

} // namespace

int main() {
    test_trx_capacity_and_release();
    test_read_view_capacity_snapshot_and_release();
    test_trx_applied_release_pending_is_retryable();
    test_read_view_applied_release_pending_is_retryable();
    test_innodb_waiters_keep_generation_fifo();
    test_cross_registry_cycle_check_does_not_hold_primary_latch();
    test_cross_registry_snapshot_detects_cycle();
    return 0;
}

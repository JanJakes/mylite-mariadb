#include "ownerless_redo_state.h"

#include "ownerless_latch.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t k_latch_offset = 0;
constexpr std::size_t k_latest_lsn_offset = 32;
constexpr std::size_t k_visible_lsn_offset = MYLITE_OWNERLESS_REDO_STATE_VISIBLE_LSN_OFFSET;
constexpr std::size_t k_refcount_offset = 48;
constexpr std::size_t k_reserved_lsn_offset = 56;
constexpr std::size_t k_durable_lsn_offset = 64;
constexpr std::size_t k_written_lsn_offset = 72;
constexpr std::size_t k_progress_latch_offset = 96;
constexpr std::size_t k_completed_range_slots_offset = 128;
constexpr std::size_t k_completed_range_slot_size = 16;
constexpr std::uint32_t k_completed_range_slot_count =
    (MYLITE_OWNERLESS_REDO_STATE_SIZE - k_completed_range_slots_offset) /
    k_completed_range_slot_size;
constexpr std::size_t k_completed_range_slot_start_offset = 0;
constexpr std::size_t k_completed_range_slot_end_offset = 8;

static_assert(
    k_latch_offset + MYLITE_OWNERLESS_LATCH_SIZE <= k_latest_lsn_offset,
    "redo state latch overlaps LSN fields"
);
static_assert(
    k_written_lsn_offset + sizeof(std::uint64_t) <= k_progress_latch_offset,
    "redo state LSN fields overlap progress latch"
);
static_assert(
    k_progress_latch_offset + MYLITE_OWNERLESS_LATCH_SIZE <= k_completed_range_slots_offset,
    "redo progress latch overlaps completed range slots"
);
static_assert(
    k_completed_range_slot_count >= 16U,
    "redo state needs enough completed range slots for process fan-out"
);

bool state_valid(const void *state, std::size_t state_size);
mylite_ownerless_latch *state_latch(void *state);
const mylite_ownerless_latch *state_latch(const void *state);
mylite_ownerless_latch *progress_latch(void *state);
std::uint64_t load64(const void *state, std::size_t offset);
std::uint32_t load32(const void *state, std::size_t offset);
void store64(void *state, std::size_t offset, std::uint64_t value);
void store32(void *state, std::size_t offset, std::uint32_t value);
bool reserve_range64(
    void *state,
    std::size_t offset,
    std::uint64_t minimum_start,
    std::uint64_t length,
    std::uint64_t *out_start,
    std::uint64_t *out_end
);
int acquire_progress_latch(void *state, std::uint32_t owner_id, std::uint64_t owner_generation);
bool record_completed_range(void *state, std::uint64_t start_lsn, std::uint64_t end_lsn);
std::uint64_t drain_completed_ranges(void *state, std::uint64_t written_lsn);
bool range_is_contiguous(std::uint64_t start_lsn, std::uint64_t written_lsn);
std::size_t completed_range_slot_offset(std::uint32_t slot_index);
std::uint64_t fetch_max64(void *state, std::size_t offset, std::uint64_t value);
std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
int latch_result_to_redo_state_result(int latch_result);

} // namespace

int mylite_ownerless_redo_state_initialize(
    void *state,
    std::size_t state_size,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
) {
    if (!state_valid(state, state_size)) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    std::memset(state, 0, MYLITE_OWNERLESS_REDO_STATE_SIZE);
    const std::uint64_t maximum_lsn = std::max(latest_lsn, visible_lsn);
    store64(state, k_latest_lsn_offset, maximum_lsn);
    store64(state, k_visible_lsn_offset, visible_lsn);
    store64(state, k_reserved_lsn_offset, maximum_lsn);
    store64(state, k_durable_lsn_offset, visible_lsn);
    store64(state, k_written_lsn_offset, maximum_lsn);
    return MYLITE_OWNERLESS_REDO_STATE_OK;
}

int mylite_ownerless_redo_state_enter(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    unsigned timeout_ms,
    std::uint64_t *out_latest_lsn
) {
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U ||
        out_latest_lsn == nullptr) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    auto *latch = state_latch(state);
    const auto deadline = wait_deadline(timeout_ms);
    for (;;) {
        std::uint32_t latch_state = 0U;
        std::uint32_t latch_owner_id = 0U;
        std::uint32_t waiter_count = 0U;
        std::uint64_t latch_owner_generation = 0U;
        std::uint64_t owner_death_count = 0U;
        if (mylite_ownerless_latch_snapshot(
                latch,
                &latch_state,
                &latch_owner_id,
                &latch_owner_generation,
                &waiter_count,
                &owner_death_count
            ) != MYLITE_OWNERLESS_LATCH_OK) {
            return MYLITE_OWNERLESS_REDO_STATE_ERROR;
        }
        if (latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED && latch_owner_id == owner_id &&
            latch_owner_generation == owner_generation) {
            __atomic_add_fetch(
                reinterpret_cast<std::uint32_t *>(
                    static_cast<unsigned char *>(state) + k_refcount_offset
                ),
                1U,
                __ATOMIC_ACQ_REL
            );
            if (mylite_ownerless_latch_snapshot(
                    latch,
                    &latch_state,
                    &latch_owner_id,
                    &latch_owner_generation,
                    &waiter_count,
                    &owner_death_count
                ) == MYLITE_OWNERLESS_LATCH_OK &&
                latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED && latch_owner_id == owner_id &&
                latch_owner_generation == owner_generation) {
                *out_latest_lsn = load64(state, k_latest_lsn_offset);
                return MYLITE_OWNERLESS_REDO_STATE_OK;
            }
            __atomic_sub_fetch(
                reinterpret_cast<std::uint32_t *>(
                    static_cast<unsigned char *>(state) + k_refcount_offset
                ),
                1U,
                __ATOMIC_ACQ_REL
            );
        }

        const unsigned timeout_remaining = remaining_timeout_ms(deadline);
        if (timeout_remaining == 0U) {
            return MYLITE_OWNERLESS_REDO_STATE_TIMEOUT;
        }
        const int latch_result = mylite_ownerless_latch_acquire(
            latch,
            owner_id,
            owner_generation,
            nullptr,
            nullptr,
            std::min(timeout_remaining, 100U)
        );
        if (latch_result == MYLITE_OWNERLESS_LATCH_OK) {
            store32(state, k_refcount_offset, 1U);
            *out_latest_lsn = load64(state, k_latest_lsn_offset);
            return MYLITE_OWNERLESS_REDO_STATE_OK;
        }
        if (latch_result != MYLITE_OWNERLESS_LATCH_TIMEOUT) {
            return latch_result_to_redo_state_result(latch_result);
        }
    }
}

int mylite_ownerless_redo_state_leave(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t latest_lsn,
    std::uint64_t *out_advanced_latest_lsn,
    std::uint32_t *out_remaining
) {
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    if (out_advanced_latest_lsn != nullptr) {
        *out_advanced_latest_lsn = 0U;
    }
    if (out_remaining != nullptr) {
        *out_remaining = 0U;
    }

    std::uint32_t latch_state = 0U;
    std::uint32_t latch_owner_id = 0U;
    std::uint32_t waiter_count = 0U;
    std::uint64_t latch_owner_generation = 0U;
    std::uint64_t owner_death_count = 0U;
    if (mylite_ownerless_latch_snapshot(
            state_latch(state),
            &latch_state,
            &latch_owner_id,
            &latch_owner_generation,
            &waiter_count,
            &owner_death_count
        ) != MYLITE_OWNERLESS_LATCH_OK ||
        latch_state != MYLITE_OWNERLESS_LATCH_STATE_LOCKED || latch_owner_id != owner_id ||
        latch_owner_generation != owner_generation) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    auto *refcount =
        reinterpret_cast<std::uint32_t *>(static_cast<unsigned char *>(state) + k_refcount_offset);
    if (__atomic_load_n(refcount, __ATOMIC_ACQUIRE) == 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    const std::uint64_t previous_lsn = load64(state, k_latest_lsn_offset);
    if (latest_lsn > previous_lsn) {
        store64(state, k_latest_lsn_offset, latest_lsn);
        fetch_max64(state, k_reserved_lsn_offset, latest_lsn);
        if (out_advanced_latest_lsn != nullptr) {
            *out_advanced_latest_lsn = latest_lsn;
        }
    }

    const std::uint32_t remaining = __atomic_sub_fetch(refcount, 1U, __ATOMIC_ACQ_REL);
    if (out_remaining != nullptr) {
        *out_remaining = remaining;
    }
    if (remaining != 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_OK;
    }

    return latch_result_to_redo_state_result(
        mylite_ownerless_latch_release(state_latch(state), owner_id, owner_generation)
    );
}

int mylite_ownerless_redo_state_reserve(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t minimum_start_lsn,
    std::uint64_t length,
    std::uint64_t *out_start_lsn,
    std::uint64_t *out_end_lsn
) {
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U ||
        length == 0U || out_start_lsn == nullptr || out_end_lsn == nullptr) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    *out_start_lsn = 0U;
    *out_end_lsn = 0U;

    auto *latch = state_latch(state);
    bool acquired_latch = false;
    std::uint32_t latch_state = 0U;
    std::uint32_t latch_owner_id = 0U;
    std::uint32_t waiter_count = 0U;
    std::uint64_t latch_owner_generation = 0U;
    std::uint64_t owner_death_count = 0U;
    if (mylite_ownerless_latch_snapshot(
            latch,
            &latch_state,
            &latch_owner_id,
            &latch_owner_generation,
            &waiter_count,
            &owner_death_count
        ) != MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    if (latch_state != MYLITE_OWNERLESS_LATCH_STATE_LOCKED || latch_owner_id != owner_id ||
        latch_owner_generation != owner_generation) {
        const int latch_result = mylite_ownerless_latch_acquire(
            latch,
            owner_id,
            owner_generation,
            nullptr,
            nullptr,
            5000U
        );
        if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
            return latch_result_to_redo_state_result(latch_result);
        }
        acquired_latch = true;
    }

    if (!reserve_range64(
            state,
            k_reserved_lsn_offset,
            minimum_start_lsn,
            length,
            out_start_lsn,
            out_end_lsn
        )) {
        if (acquired_latch) {
            static_cast<void>(mylite_ownerless_latch_release(latch, owner_id, owner_generation));
        }
        *out_start_lsn = 0U;
        *out_end_lsn = 0U;
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    if (*out_start_lsn != 0U && load64(state, k_written_lsn_offset) == 0U) {
        store64(state, k_written_lsn_offset, *out_start_lsn);
    }
    if (!acquired_latch) {
        return MYLITE_OWNERLESS_REDO_STATE_OK;
    }
    const int release_result = mylite_ownerless_latch_release(latch, owner_id, owner_generation);
    return latch_result_to_redo_state_result(release_result);
}

int mylite_ownerless_redo_state_complete_write(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t *out_written_lsn
) {
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U ||
        start_lsn == 0U || end_lsn <= start_lsn) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    if (out_written_lsn != nullptr) {
        *out_written_lsn = 0U;
    }

    const int latch_result = acquire_progress_latch(state, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }

    const std::uint64_t previous_written_lsn = load64(state, k_written_lsn_offset);
    std::uint64_t written_lsn = previous_written_lsn;
    int result = MYLITE_OWNERLESS_REDO_STATE_OK;
    if (written_lsn == 0U) {
        store64(state, k_written_lsn_offset, end_lsn);
        written_lsn = drain_completed_ranges(state, end_lsn);
    } else if (end_lsn <= written_lsn) {
        result = MYLITE_OWNERLESS_REDO_STATE_OK;
    } else if (range_is_contiguous(start_lsn, written_lsn)) {
        store64(state, k_written_lsn_offset, end_lsn);
        written_lsn = drain_completed_ranges(state, end_lsn);
    } else if (!record_completed_range(state, start_lsn, end_lsn)) {
        result = MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    const int release_result =
        mylite_ownerless_latch_release(progress_latch(state), owner_id, owner_generation);
    if (release_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(release_result);
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK && out_written_lsn != nullptr &&
        written_lsn > previous_written_lsn) {
        *out_written_lsn = written_lsn;
    }
    return result;
}

int mylite_ownerless_redo_state_publish_visible(
    void *state,
    std::size_t state_size,
    std::uint64_t visible_lsn,
    std::uint64_t *out_latest_lsn,
    std::uint64_t *out_visible_lsn
) {
    if (!state_valid(state, state_size) || visible_lsn == 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    const std::uint64_t latest_lsn = fetch_max64(state, k_latest_lsn_offset, visible_lsn);
    const std::uint64_t published_visible_lsn =
        fetch_max64(state, k_visible_lsn_offset, visible_lsn);
    fetch_max64(state, k_durable_lsn_offset, published_visible_lsn);
    if (out_latest_lsn != nullptr) {
        *out_latest_lsn = latest_lsn;
    }
    if (out_visible_lsn != nullptr) {
        *out_visible_lsn = published_visible_lsn;
    }
    return MYLITE_OWNERLESS_REDO_STATE_OK;
}

int mylite_ownerless_redo_state_cleanup_owner(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t *out_released
) {
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    if (out_released != nullptr) {
        *out_released = 0U;
    }

    mylite_ownerless_redo_state_snapshot snapshot = {};
    if (mylite_ownerless_redo_state_read_snapshot(state, state_size, &snapshot) !=
        MYLITE_OWNERLESS_REDO_STATE_OK) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    if (snapshot.latch_state != MYLITE_OWNERLESS_LATCH_STATE_LOCKED ||
        snapshot.latch_owner_id != owner_id ||
        snapshot.latch_owner_generation != owner_generation) {
        return MYLITE_OWNERLESS_REDO_STATE_OK;
    }

    store32(state, k_refcount_offset, 0U);
    const int release_result =
        mylite_ownerless_latch_release(state_latch(state), owner_id, owner_generation);
    if (release_result == MYLITE_OWNERLESS_LATCH_OK && out_released != nullptr) {
        *out_released = 1U;
    }
    return latch_result_to_redo_state_result(release_result);
}

int mylite_ownerless_redo_state_read_snapshot(
    const void *state,
    std::size_t state_size,
    mylite_ownerless_redo_state_snapshot *out_snapshot
) {
    if (!state_valid(state, state_size) || out_snapshot == nullptr) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    std::uint32_t waiter_count = 0U;
    std::uint64_t owner_death_count = 0U;
    if (mylite_ownerless_latch_snapshot(
            state_latch(state),
            &out_snapshot->latch_state,
            &out_snapshot->latch_owner_id,
            &out_snapshot->latch_owner_generation,
            &waiter_count,
            &owner_death_count
        ) != MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    out_snapshot->latest_lsn = load64(state, k_latest_lsn_offset);
    out_snapshot->visible_lsn = load64(state, k_visible_lsn_offset);
    out_snapshot->reserved_lsn = load64(state, k_reserved_lsn_offset);
    out_snapshot->durable_lsn = load64(state, k_durable_lsn_offset);
    out_snapshot->written_lsn = load64(state, k_written_lsn_offset);
    out_snapshot->refcount = load32(state, k_refcount_offset);
    return MYLITE_OWNERLESS_REDO_STATE_OK;
}

namespace {

bool state_valid(const void *state, std::size_t state_size) {
    return state != nullptr && state_size >= MYLITE_OWNERLESS_REDO_STATE_SIZE;
}

mylite_ownerless_latch *state_latch(void *state) {
    return reinterpret_cast<mylite_ownerless_latch *>(
        static_cast<unsigned char *>(state) + k_latch_offset
    );
}

const mylite_ownerless_latch *state_latch(const void *state) {
    return reinterpret_cast<const mylite_ownerless_latch *>(
        static_cast<const unsigned char *>(state) + k_latch_offset
    );
}

mylite_ownerless_latch *progress_latch(void *state) {
    return reinterpret_cast<mylite_ownerless_latch *>(
        static_cast<unsigned char *>(state) + k_progress_latch_offset
    );
}

std::uint64_t load64(const void *state, std::size_t offset) {
    const auto *value =
        reinterpret_cast<const std::uint64_t *>(static_cast<const unsigned char *>(state) + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

std::uint32_t load32(const void *state, std::size_t offset) {
    const auto *value =
        reinterpret_cast<const std::uint32_t *>(static_cast<const unsigned char *>(state) + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

void store64(void *state, std::size_t offset, std::uint64_t value) {
    auto *target = reinterpret_cast<std::uint64_t *>(static_cast<unsigned char *>(state) + offset);
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

void store32(void *state, std::size_t offset, std::uint32_t value) {
    auto *target = reinterpret_cast<std::uint32_t *>(static_cast<unsigned char *>(state) + offset);
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

bool reserve_range64(
    void *state,
    std::size_t offset,
    std::uint64_t minimum_start,
    std::uint64_t length,
    std::uint64_t *out_start,
    std::uint64_t *out_end
) {
    auto *target = reinterpret_cast<std::uint64_t *>(static_cast<unsigned char *>(state) + offset);
    std::uint64_t observed = __atomic_load_n(target, __ATOMIC_ACQUIRE);
    for (;;) {
        const std::uint64_t start = std::max(observed, minimum_start);
        if (length > std::numeric_limits<std::uint64_t>::max() - start) {
            return false;
        }
        const std::uint64_t end = start + length;
        if (__atomic_compare_exchange_n(
                target,
                &observed,
                end,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE
            )) {
            *out_start = start;
            *out_end = end;
            return true;
        }
    }
}

int acquire_progress_latch(void *state, std::uint32_t owner_id, std::uint64_t owner_generation) {
    return mylite_ownerless_latch_acquire(
        progress_latch(state),
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        5000U
    );
}

bool record_completed_range(void *state, std::uint64_t start_lsn, std::uint64_t end_lsn) {
    for (std::uint32_t slot_index = 0; slot_index < k_completed_range_slot_count; ++slot_index) {
        const std::size_t slot_offset = completed_range_slot_offset(slot_index);
        if (load64(state, slot_offset + k_completed_range_slot_start_offset) != 0U) {
            continue;
        }
        store64(state, slot_offset + k_completed_range_slot_end_offset, end_lsn);
        store64(state, slot_offset + k_completed_range_slot_start_offset, start_lsn);
        return true;
    }
    return false;
}

std::uint64_t drain_completed_ranges(void *state, std::uint64_t written_lsn) {
    for (;;) {
        bool advanced = false;
        for (std::uint32_t slot_index = 0; slot_index < k_completed_range_slot_count;
             ++slot_index) {
            const std::size_t slot_offset = completed_range_slot_offset(slot_index);
            const std::uint64_t start_lsn =
                load64(state, slot_offset + k_completed_range_slot_start_offset);
            if (start_lsn == 0U || !range_is_contiguous(start_lsn, written_lsn)) {
                continue;
            }

            const std::uint64_t end_lsn =
                load64(state, slot_offset + k_completed_range_slot_end_offset);
            store64(state, slot_offset + k_completed_range_slot_end_offset, 0U);
            store64(state, slot_offset + k_completed_range_slot_start_offset, 0U);
            if (end_lsn > written_lsn) {
                written_lsn = end_lsn;
                store64(state, k_written_lsn_offset, written_lsn);
                advanced = true;
            }
        }
        if (!advanced) {
            return written_lsn;
        }
    }
}

bool range_is_contiguous(std::uint64_t start_lsn, std::uint64_t written_lsn) {
    return start_lsn <= written_lsn || (written_lsn != std::numeric_limits<std::uint64_t>::max() &&
                                        start_lsn == written_lsn + 1U);
}

std::size_t completed_range_slot_offset(std::uint32_t slot_index) {
    return k_completed_range_slots_offset +
           (static_cast<std::size_t>(slot_index) * k_completed_range_slot_size);
}

std::uint64_t fetch_max64(void *state, std::size_t offset, std::uint64_t value) {
    auto *target = reinterpret_cast<std::uint64_t *>(static_cast<unsigned char *>(state) + offset);
    std::uint64_t observed = __atomic_load_n(target, __ATOMIC_ACQUIRE);
    while (observed < value && !__atomic_compare_exchange_n(
                                   target,
                                   &observed,
                                   value,
                                   false,
                                   __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE
                               )) {}
    return std::max(observed, value);
}

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<unsigned>(std::max<std::chrono::milliseconds::rep>(remaining.count(), 1));
}

int latch_result_to_redo_state_result(int latch_result) {
    if (latch_result == MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_REDO_STATE_OK;
    }
    if (latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT) {
        return MYLITE_OWNERLESS_REDO_STATE_TIMEOUT;
    }
    return MYLITE_OWNERLESS_REDO_STATE_ERROR;
}

} // namespace

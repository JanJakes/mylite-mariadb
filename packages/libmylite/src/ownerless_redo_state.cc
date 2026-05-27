#include "ownerless_redo_state.h"

#include "ownerless_latch.h"

#include <algorithm>
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
constexpr std::size_t k_active_reservation_slots_offset = 128;
constexpr std::size_t k_active_reservation_slot_size = 32;
constexpr std::uint32_t k_active_reservation_slot_count = 64;
constexpr std::size_t k_completed_range_slots_offset =
    k_active_reservation_slots_offset +
    (k_active_reservation_slot_count * k_active_reservation_slot_size);
constexpr std::size_t k_completed_range_slot_size = 16;
constexpr std::uint32_t k_completed_range_slot_count =
    (MYLITE_OWNERLESS_REDO_STATE_SIZE - k_completed_range_slots_offset) /
    k_completed_range_slot_size;
constexpr std::size_t k_active_reservation_slot_owner_id_offset = 0;
constexpr std::size_t k_active_reservation_slot_state_offset = 4;
constexpr std::size_t k_active_reservation_slot_owner_generation_offset = 8;
constexpr std::size_t k_active_reservation_slot_start_offset = 16;
constexpr std::size_t k_active_reservation_slot_end_offset = 24;
constexpr std::size_t k_active_entry_slot_refcount_offset = k_active_reservation_slot_start_offset;
constexpr std::size_t k_completed_range_slot_start_offset = 0;
constexpr std::size_t k_completed_range_slot_end_offset = 8;
constexpr std::uint32_t k_active_reservation_slot_state_free = 0;
constexpr std::uint32_t k_active_reservation_slot_state_active = 1;
constexpr std::uint32_t k_active_reservation_slot_state_entry = 2;
constexpr unsigned k_progress_latch_timeout_ms = 5000U;

static_assert(
    k_latch_offset + MYLITE_OWNERLESS_LATCH_SIZE <= k_latest_lsn_offset,
    "redo state latch overlaps LSN fields"
);
static_assert(
    k_written_lsn_offset + sizeof(std::uint64_t) <= k_progress_latch_offset,
    "redo state LSN fields overlap progress latch"
);
static_assert(
    k_progress_latch_offset + MYLITE_OWNERLESS_LATCH_SIZE <= k_active_reservation_slots_offset,
    "redo progress latch overlaps active reservation slots"
);
static_assert(
    k_completed_range_slots_offset <= MYLITE_OWNERLESS_REDO_STATE_SIZE,
    "redo active reservation slots exceed state segment"
);
static_assert(
    k_completed_range_slot_count >= 16U,
    "redo state needs enough completed range slots for process fan-out"
);

bool state_valid(const void *state, std::size_t state_size);
mylite_ownerless_latch *state_latch(void *state);
const mylite_ownerless_latch *state_latch(const void *state);
mylite_ownerless_latch *progress_latch(void *state);
const mylite_ownerless_latch *progress_latch(const void *state);
std::uint64_t load64(const void *state, std::size_t offset);
std::uint32_t load32(const void *state, std::size_t offset);
void store64(void *state, std::size_t offset, std::uint64_t value);
void store32(void *state, std::size_t offset, std::uint32_t value);
std::uint64_t entry_lsn(const void *state);
bool reserve_range64(
    void *state,
    std::size_t offset,
    std::uint64_t minimum_start,
    std::uint64_t length,
    std::uint64_t *out_start,
    std::uint64_t *out_end
);
int acquire_progress_latch(void *state, std::uint32_t owner_id, std::uint64_t owner_generation);
int acquire_progress_latch(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    unsigned timeout_ms
);
unsigned char *find_free_active_reservation_slot(void *state);
unsigned char *find_active_reservation_slot(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t start_lsn,
    std::uint64_t end_lsn
);
unsigned char *find_active_entry_slot(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
);
int enter_active_owner(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    unsigned timeout_ms
);
int leave_active_owner(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t latest_lsn,
    std::uint64_t *out_advanced_latest_lsn,
    std::uint32_t *out_remaining
);
void reserve_active_range(
    unsigned char *slot,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t start_lsn,
    std::uint64_t end_lsn
);
void clear_active_reservation_slot(unsigned char *slot);
int clear_owner_entry_slots_guarded(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
);
void clear_owner_entry_slots(void *state, std::uint32_t owner_id, std::uint64_t owner_generation);
std::uint32_t active_reservation_count(const void *state);
std::uint32_t owner_active_state_count(const void *state, std::uint32_t owner_id);
bool record_completed_range(void *state, std::uint64_t start_lsn, std::uint64_t end_lsn);
bool ranges_touch_or_overlap(
    std::uint64_t left_start,
    std::uint64_t left_end,
    std::uint64_t right_start,
    std::uint64_t right_end
);
std::uint64_t drain_completed_ranges(void *state, std::uint64_t written_lsn);
bool range_is_contiguous(std::uint64_t start_lsn, std::uint64_t written_lsn);
std::size_t completed_range_slot_offset(std::uint32_t slot_index);
std::uint64_t fetch_max64(void *state, std::size_t offset, std::uint64_t value);
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

    const int entry_result = enter_active_owner(state, owner_id, owner_generation, timeout_ms);
    if (entry_result != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return entry_result;
    }
    *out_latest_lsn = entry_lsn(state);
    return MYLITE_OWNERLESS_REDO_STATE_OK;
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

    const int leave_result = leave_active_owner(
        state,
        owner_id,
        owner_generation,
        latest_lsn,
        out_advanced_latest_lsn,
        out_remaining
    );
    if (leave_result != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return leave_result;
    }
    return MYLITE_OWNERLESS_REDO_STATE_OK;
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

    const int latch_result = acquire_progress_latch(state, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }

    unsigned char *slot = find_free_active_reservation_slot(state);
    if (slot == nullptr) {
        static_cast<void>(mylite_ownerless_latch_release(
            progress_latch(state),
            owner_id,
            owner_generation
        ));
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    if (!reserve_range64(
            state,
            k_reserved_lsn_offset,
            minimum_start_lsn,
            length,
            out_start_lsn,
            out_end_lsn
        )) {
        static_cast<void>(mylite_ownerless_latch_release(
            progress_latch(state),
            owner_id,
            owner_generation
        ));
        *out_start_lsn = 0U;
        *out_end_lsn = 0U;
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    reserve_active_range(slot, owner_id, owner_generation, *out_start_lsn, *out_end_lsn);
    if (*out_start_lsn != 0U && load64(state, k_written_lsn_offset) == 0U) {
        store64(state, k_written_lsn_offset, *out_start_lsn);
    }
    const int release_result =
        mylite_ownerless_latch_release(progress_latch(state), owner_id, owner_generation);
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
    unsigned char *slot = find_active_reservation_slot(
        state,
        owner_id,
        owner_generation,
        start_lsn,
        end_lsn
    );
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
    } else {
        written_lsn = drain_completed_ranges(state, written_lsn);
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK && slot != nullptr) {
        clear_active_reservation_slot(slot);
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

    const std::uint64_t written_lsn = load64(state, k_written_lsn_offset);
    if (written_lsn == 0U) {
        if (out_latest_lsn != nullptr) {
            *out_latest_lsn = load64(state, k_latest_lsn_offset);
        }
        if (out_visible_lsn != nullptr) {
            *out_visible_lsn = load64(state, k_visible_lsn_offset);
        }
        return MYLITE_OWNERLESS_REDO_STATE_OK;
    }

    const std::uint64_t safe_visible_lsn = std::min(visible_lsn, written_lsn);
    const std::uint64_t latest_lsn = fetch_max64(state, k_latest_lsn_offset, safe_visible_lsn);
    const std::uint64_t published_visible_lsn =
        fetch_max64(state, k_visible_lsn_offset, safe_visible_lsn);
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
        return clear_owner_entry_slots_guarded(state, owner_id, owner_generation);
    }

    const int clear_result = clear_owner_entry_slots_guarded(state, owner_id, owner_generation);
    if (clear_result != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return clear_result;
    }
    store32(state, k_refcount_offset, 0U);
    const int release_result =
        mylite_ownerless_latch_release(state_latch(state), owner_id, owner_generation);
    if (release_result == MYLITE_OWNERLESS_LATCH_OK && out_released != nullptr) {
        *out_released = 1U;
    }
    return latch_result_to_redo_state_result(release_result);
}

int mylite_ownerless_redo_state_owner_active_count(
    const void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint32_t *out_active_count
) {
    if (!state_valid(state, state_size) || owner_id == 0U || out_active_count == nullptr) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    *out_active_count = owner_active_state_count(state, owner_id);
    return MYLITE_OWNERLESS_REDO_STATE_OK;
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
    out_snapshot->active_reservation_count = active_reservation_count(state);
    if (mylite_ownerless_latch_snapshot(
            progress_latch(state),
            &out_snapshot->progress_latch_state,
            &out_snapshot->progress_latch_owner_id,
            &out_snapshot->progress_latch_owner_generation,
            &waiter_count,
            &owner_death_count
        ) != MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
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

const mylite_ownerless_latch *progress_latch(const void *state) {
    return reinterpret_cast<const mylite_ownerless_latch *>(
        static_cast<const unsigned char *>(state) + k_progress_latch_offset
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

std::uint64_t entry_lsn(const void *state) {
    return std::max(load64(state, k_latest_lsn_offset), load64(state, k_reserved_lsn_offset));
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
    return acquire_progress_latch(state, owner_id, owner_generation, k_progress_latch_timeout_ms);
}

int acquire_progress_latch(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    unsigned timeout_ms
) {
    return mylite_ownerless_latch_acquire(
        progress_latch(state),
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        timeout_ms
    );
}

unsigned char *find_free_active_reservation_slot(void *state) {
    auto *bytes = static_cast<unsigned char *>(state);
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count;
         ++slot_index) {
        unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        if (load32(slot, k_active_reservation_slot_state_offset) ==
            k_active_reservation_slot_state_free) {
            return slot;
        }
    }
    return nullptr;
}

unsigned char *find_active_reservation_slot(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t start_lsn,
    std::uint64_t end_lsn
) {
    auto *bytes = static_cast<unsigned char *>(state);
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count;
         ++slot_index) {
        unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        if (load32(slot, k_active_reservation_slot_state_offset) !=
                k_active_reservation_slot_state_active ||
            load32(slot, k_active_reservation_slot_owner_id_offset) != owner_id ||
            load64(slot, k_active_reservation_slot_owner_generation_offset) !=
                owner_generation ||
            load64(slot, k_active_reservation_slot_start_offset) != start_lsn ||
            load64(slot, k_active_reservation_slot_end_offset) != end_lsn) {
            continue;
        }
        return slot;
    }
    return nullptr;
}

unsigned char *find_active_entry_slot(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    auto *bytes = static_cast<unsigned char *>(state);
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count;
         ++slot_index) {
        unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        if (load32(slot, k_active_reservation_slot_state_offset) ==
                k_active_reservation_slot_state_entry &&
            load32(slot, k_active_reservation_slot_owner_id_offset) == owner_id &&
            load64(slot, k_active_reservation_slot_owner_generation_offset) ==
                owner_generation) {
            return slot;
        }
    }
    return nullptr;
}

int enter_active_owner(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    unsigned timeout_ms
) {
    const int latch_result = acquire_progress_latch(state, owner_id, owner_generation, timeout_ms);
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }

    unsigned char *slot = find_active_entry_slot(state, owner_id, owner_generation);
    if (slot == nullptr) {
        slot = find_free_active_reservation_slot(state);
        if (slot != nullptr) {
            store64(slot, k_active_entry_slot_refcount_offset, 0U);
            store64(slot, k_active_reservation_slot_owner_generation_offset, owner_generation);
            store32(slot, k_active_reservation_slot_owner_id_offset, owner_id);
            store32(slot, k_active_reservation_slot_state_offset,
                    k_active_reservation_slot_state_entry);
        }
    }
    if (slot == nullptr) {
        static_cast<void>(mylite_ownerless_latch_release(
            progress_latch(state),
            owner_id,
            owner_generation
        ));
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    const std::uint64_t refcount = load64(slot, k_active_entry_slot_refcount_offset);
    const std::uint32_t global_refcount = load32(state, k_refcount_offset);
    if (refcount == std::numeric_limits<std::uint64_t>::max() ||
        global_refcount == std::numeric_limits<std::uint32_t>::max()) {
        static_cast<void>(mylite_ownerless_latch_release(
            progress_latch(state),
            owner_id,
            owner_generation
        ));
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    store64(slot, k_active_entry_slot_refcount_offset, refcount + 1U);
    __atomic_add_fetch(
        reinterpret_cast<std::uint32_t *>(static_cast<unsigned char *>(state) + k_refcount_offset),
        1U,
        __ATOMIC_ACQ_REL
    );

    const int release_result =
        mylite_ownerless_latch_release(progress_latch(state), owner_id, owner_generation);
    return latch_result_to_redo_state_result(release_result);
}

int leave_active_owner(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t latest_lsn,
    std::uint64_t *out_advanced_latest_lsn,
    std::uint32_t *out_remaining
) {
    const int latch_result = acquire_progress_latch(state, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }

    unsigned char *slot = find_active_entry_slot(state, owner_id, owner_generation);
    if (slot == nullptr) {
        static_cast<void>(mylite_ownerless_latch_release(
            progress_latch(state),
            owner_id,
            owner_generation
        ));
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    const std::uint64_t owner_refcount = load64(slot, k_active_entry_slot_refcount_offset);
    const std::uint32_t global_refcount = load32(state, k_refcount_offset);
    if (owner_refcount == 0U || owner_refcount > global_refcount) {
        static_cast<void>(mylite_ownerless_latch_release(
            progress_latch(state),
            owner_id,
            owner_generation
        ));
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    const std::uint64_t previous_lsn = load64(state, k_latest_lsn_offset);
    if (latest_lsn > previous_lsn) {
        const std::uint64_t published_lsn = fetch_max64(state, k_latest_lsn_offset, latest_lsn);
        fetch_max64(state, k_reserved_lsn_offset, latest_lsn);
        if (out_advanced_latest_lsn != nullptr && published_lsn == latest_lsn) {
            *out_advanced_latest_lsn = latest_lsn;
        }
    }

    if (owner_refcount == 1U) {
        clear_active_reservation_slot(slot);
    } else {
        store64(slot, k_active_entry_slot_refcount_offset, owner_refcount - 1U);
    }
    __atomic_sub_fetch(
        reinterpret_cast<std::uint32_t *>(static_cast<unsigned char *>(state) + k_refcount_offset),
        1U,
        __ATOMIC_ACQ_REL
    );
    if (out_remaining != nullptr) {
        *out_remaining = load32(state, k_refcount_offset);
    }

    const int release_result =
        mylite_ownerless_latch_release(progress_latch(state), owner_id, owner_generation);
    return latch_result_to_redo_state_result(release_result);
}

void reserve_active_range(
    unsigned char *slot,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t start_lsn,
    std::uint64_t end_lsn
) {
    store64(slot, k_active_reservation_slot_end_offset, end_lsn);
    store64(slot, k_active_reservation_slot_start_offset, start_lsn);
    store64(slot, k_active_reservation_slot_owner_generation_offset, owner_generation);
    store32(slot, k_active_reservation_slot_owner_id_offset, owner_id);
    store32(slot, k_active_reservation_slot_state_offset, k_active_reservation_slot_state_active);
}

void clear_active_reservation_slot(unsigned char *slot) {
    store32(slot, k_active_reservation_slot_state_offset, k_active_reservation_slot_state_free);
    store64(slot, k_active_reservation_slot_end_offset, 0U);
    store64(slot, k_active_reservation_slot_start_offset, 0U);
    store64(slot, k_active_reservation_slot_owner_generation_offset, 0U);
    store32(slot, k_active_reservation_slot_owner_id_offset, 0U);
}

int clear_owner_entry_slots_guarded(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    const int latch_result = acquire_progress_latch(state, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }

    clear_owner_entry_slots(state, owner_id, owner_generation);

    const int release_result =
        mylite_ownerless_latch_release(progress_latch(state), owner_id, owner_generation);
    return latch_result_to_redo_state_result(release_result);
}

void clear_owner_entry_slots(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    while (unsigned char *slot = find_active_entry_slot(state, owner_id, owner_generation)) {
        const std::uint64_t owner_refcount = load64(slot, k_active_entry_slot_refcount_offset);
        const std::uint32_t global_refcount = load32(state, k_refcount_offset);
        store32(
            state,
            k_refcount_offset,
            owner_refcount >= global_refcount
                ? 0U
                : static_cast<std::uint32_t>(global_refcount - owner_refcount)
        );
        clear_active_reservation_slot(slot);
    }
}

std::uint32_t active_reservation_count(const void *state) {
    const auto *bytes = static_cast<const unsigned char *>(state);
    std::uint32_t count = 0;
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count;
         ++slot_index) {
        const unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        if (load32(slot, k_active_reservation_slot_state_offset) ==
            k_active_reservation_slot_state_active) {
            ++count;
        }
    }
    return count;
}

std::uint32_t owner_active_state_count(const void *state, std::uint32_t owner_id) {
    const auto *bytes = static_cast<const unsigned char *>(state);
    std::uint32_t count = 0;
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count;
         ++slot_index) {
        const unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        const std::uint32_t slot_state = load32(slot, k_active_reservation_slot_state_offset);
        if ((slot_state == k_active_reservation_slot_state_active ||
             (slot_state == k_active_reservation_slot_state_entry &&
              load64(slot, k_active_entry_slot_refcount_offset) != 0U)) &&
            load32(slot, k_active_reservation_slot_owner_id_offset) == owner_id) {
            ++count;
        }
    }
    return count;
}

bool record_completed_range(void *state, std::uint64_t start_lsn, std::uint64_t end_lsn) {
    std::uint64_t merged_start_lsn = start_lsn;
    std::uint64_t merged_end_lsn = end_lsn;
    for (;;) {
        bool merged = false;
        for (std::uint32_t slot_index = 0; slot_index < k_completed_range_slot_count;
             ++slot_index) {
            const std::size_t slot_offset = completed_range_slot_offset(slot_index);
            const std::uint64_t slot_start_lsn =
                load64(state, slot_offset + k_completed_range_slot_start_offset);
            if (slot_start_lsn == 0U) {
                continue;
            }
            const std::uint64_t slot_end_lsn =
                load64(state, slot_offset + k_completed_range_slot_end_offset);
            if (!ranges_touch_or_overlap(
                    merged_start_lsn,
                    merged_end_lsn,
                    slot_start_lsn,
                    slot_end_lsn
                )) {
                continue;
            }

            merged_start_lsn = std::min(merged_start_lsn, slot_start_lsn);
            merged_end_lsn = std::max(merged_end_lsn, slot_end_lsn);
            store64(state, slot_offset + k_completed_range_slot_end_offset, 0U);
            store64(state, slot_offset + k_completed_range_slot_start_offset, 0U);
            merged = true;
        }
        if (!merged) {
            break;
        }
    }

    for (std::uint32_t slot_index = 0; slot_index < k_completed_range_slot_count; ++slot_index) {
        const std::size_t slot_offset = completed_range_slot_offset(slot_index);
        if (load64(state, slot_offset + k_completed_range_slot_start_offset) != 0U) {
            continue;
        }
        store64(state, slot_offset + k_completed_range_slot_end_offset, merged_end_lsn);
        store64(state, slot_offset + k_completed_range_slot_start_offset, merged_start_lsn);
        return true;
    }
    return false;
}

bool ranges_touch_or_overlap(
    std::uint64_t left_start,
    std::uint64_t left_end,
    std::uint64_t right_start,
    std::uint64_t right_end
) {
    return range_is_contiguous(left_start, right_end) && range_is_contiguous(right_start, left_end);
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

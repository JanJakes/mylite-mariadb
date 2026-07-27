#include "ownerless_redo_state.h"

#include "ownerless_latch.h"
#include "ownerless_process_registry.h"

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
constexpr std::size_t k_visible_generation_offset =
    MYLITE_OWNERLESS_REDO_STATE_VISIBLE_GENERATION_OFFSET;
constexpr std::size_t k_active_reservation_count_offset = 88;
constexpr std::size_t k_completed_range_count_offset = 92;
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
    k_visible_generation_offset + sizeof(std::uint64_t) <= k_progress_latch_offset,
    "redo state visible generation overlaps progress latch"
);
static_assert(
    k_active_reservation_count_offset + sizeof(std::uint32_t) <= k_progress_latch_offset,
    "redo state active reservation count overlaps progress latch"
);
static_assert(
    k_completed_range_count_offset + sizeof(std::uint32_t) <= k_progress_latch_offset,
    "redo state completed range count overlaps progress latch"
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
int leave_active_owner_locked(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t latest_lsn,
    std::uint64_t *out_advanced_latest_lsn,
    std::uint32_t *out_remaining
);
int complete_write_locked(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t *out_written_lsn
);
void reserve_active_range(
    void *state,
    unsigned char *slot,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t start_lsn,
    std::uint64_t end_lsn
);
void clear_active_reservation_slot(void *state, unsigned char *slot);
void clear_redo_state_slot(unsigned char *slot);
int clear_owner_entry_slots_guarded(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
);
void clear_owner_entry_slots(void *state, std::uint32_t owner_id, std::uint64_t owner_generation);
bool repair_progress_state_locked(void *state);
bool owner_has_incomplete_reservation(
    const void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
);
std::uint32_t active_reservation_count(const void *state);
std::uint32_t completed_range_count(const void *state);
std::uint32_t owner_active_state_count(const void *state, std::uint32_t owner_id);
void increment_active_reservation_count(void *state);
void decrement_active_reservation_count(void *state);
void increment_completed_range_count(void *state);
void decrement_completed_range_count(void *state);
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
bool increment_visible_generation(void *state);
int latch_result_to_redo_state_result(int latch_result);
int finish_latch_operation(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
);

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
    store64(state, k_visible_generation_offset, visible_lsn == 0U ? 0U : 1U);
    return MYLITE_OWNERLESS_REDO_STATE_OK;
}

int mylite_ownerless_redo_state_seed_checkpoint(
    void *state,
    std::size_t state_size,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
) {
    if (!state_valid(state, state_size)) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    const std::uint64_t maximum_lsn = std::max(latest_lsn, visible_lsn);
    if (maximum_lsn == 0U && visible_lsn == 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_OK;
    }

    fetch_max64(state, k_latest_lsn_offset, maximum_lsn);
    fetch_max64(state, k_reserved_lsn_offset, maximum_lsn);
    fetch_max64(state, k_written_lsn_offset, maximum_lsn);
    fetch_max64(state, k_visible_lsn_offset, visible_lsn);
    fetch_max64(state, k_durable_lsn_offset, visible_lsn);
    if (visible_lsn != 0U && load64(state, k_visible_generation_offset) == 0U) {
        fetch_max64(state, k_visible_generation_offset, 1U);
    }
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
        return finish_latch_operation(
            progress_latch(state),
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_REDO_STATE_ERROR,
            false
        );
    }
    const std::uint64_t reserved_lsn = load64(state, k_reserved_lsn_offset);
    const std::uint64_t start_lsn = std::max(reserved_lsn, minimum_start_lsn);
    if (length > std::numeric_limits<std::uint64_t>::max() - start_lsn) {
        return finish_latch_operation(
            progress_latch(state),
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_REDO_STATE_ERROR,
            false
        );
    }
    const std::uint64_t end_lsn = start_lsn + length;
    /*
     * Publish the reservation record before advancing the allocator. Recovery
     * can raise reserved_lsn from this record; the opposite order leaves an
     * ownerless gap with no evidence describing it.
     */
    reserve_active_range(state, slot, owner_id, owner_generation, start_lsn, end_lsn);
    fetch_max64(state, k_reserved_lsn_offset, end_lsn);
    *out_start_lsn = start_lsn;
    *out_end_lsn = end_lsn;
    if (*out_start_lsn != 0U && load64(state, k_written_lsn_offset) == 0U) {
        store64(state, k_written_lsn_offset, *out_start_lsn);
    }
    return finish_latch_operation(
        progress_latch(state),
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_REDO_STATE_OK,
        true
    );
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

    const int result = complete_write_locked(
        state,
        owner_id,
        owner_generation,
        start_lsn,
        end_lsn,
        out_written_lsn
    );

    return finish_latch_operation(
        progress_latch(state),
        owner_id,
        owner_generation,
        result,
        result == MYLITE_OWNERLESS_REDO_STATE_OK
    );
}

int mylite_ownerless_redo_state_complete_write_and_leave(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t latest_lsn,
    std::uint64_t *out_written_lsn,
    std::uint64_t *out_advanced_latest_lsn,
    std::uint32_t *out_remaining
) {
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U ||
        start_lsn == 0U || end_lsn <= start_lsn) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    if (out_written_lsn != nullptr) {
        *out_written_lsn = 0U;
    }
    if (out_advanced_latest_lsn != nullptr) {
        *out_advanced_latest_lsn = 0U;
    }
    if (out_remaining != nullptr) {
        *out_remaining = 0U;
    }

    const int latch_result = acquire_progress_latch(state, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }

    int result = complete_write_locked(
        state,
        owner_id,
        owner_generation,
        start_lsn,
        end_lsn,
        out_written_lsn
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK) {
        result = leave_active_owner_locked(
            state,
            owner_id,
            owner_generation,
            latest_lsn,
            out_advanced_latest_lsn,
            out_remaining
        );
    }

    return finish_latch_operation(
        progress_latch(state),
        owner_id,
        owner_generation,
        result,
        result == MYLITE_OWNERLESS_REDO_STATE_OK
    );
}

int mylite_ownerless_redo_state_complete_write_and_leave_batch(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    const mylite_ownerless_redo_state_range *ranges,
    std::size_t range_count,
    std::uint64_t latest_lsn,
    std::uint64_t *out_written_lsn,
    std::uint64_t *out_advanced_latest_lsn,
    std::uint32_t *out_remaining,
    std::size_t *out_completed_count
) {
    if (out_written_lsn != nullptr) {
        *out_written_lsn = 0U;
    }
    if (out_advanced_latest_lsn != nullptr) {
        *out_advanced_latest_lsn = 0U;
    }
    if (out_remaining != nullptr) {
        *out_remaining = 0U;
    }
    if (out_completed_count != nullptr) {
        *out_completed_count = 0U;
    }
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U ||
        ranges == nullptr || range_count == 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    for (std::size_t index = 0; index < range_count; ++index) {
        if (ranges[index].start_lsn == 0U || ranges[index].end_lsn <= ranges[index].start_lsn) {
            return MYLITE_OWNERLESS_REDO_STATE_ERROR;
        }
    }

    const int latch_result = acquire_progress_latch(state, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }

    int result = MYLITE_OWNERLESS_REDO_STATE_OK;
    std::uint64_t written_lsn = 0U;
    std::uint64_t advanced_latest_lsn = 0U;
    std::uint32_t remaining = 0U;
    std::size_t completed_count = 0U;
    for (std::size_t index = 0; index < range_count; ++index) {
        std::uint64_t range_written_lsn = 0U;
        result = complete_write_locked(
            state,
            owner_id,
            owner_generation,
            ranges[index].start_lsn,
            ranges[index].end_lsn,
            &range_written_lsn
        );
        if (result != MYLITE_OWNERLESS_REDO_STATE_OK) {
            break;
        }

        const std::uint64_t range_latest_lsn = index + 1U == range_count ? latest_lsn : 0U;
        result = leave_active_owner_locked(
            state,
            owner_id,
            owner_generation,
            range_latest_lsn,
            &advanced_latest_lsn,
            &remaining
        );
        if (result != MYLITE_OWNERLESS_REDO_STATE_OK) {
            break;
        }

        ++completed_count;
        written_lsn = std::max(written_lsn, range_written_lsn);
    }

    if (out_written_lsn != nullptr) {
        *out_written_lsn = written_lsn;
    }
    if (out_advanced_latest_lsn != nullptr) {
        *out_advanced_latest_lsn = advanced_latest_lsn;
    }
    if (out_remaining != nullptr) {
        *out_remaining = remaining;
    }
    if (out_completed_count != nullptr) {
        *out_completed_count = completed_count;
    }

    return finish_latch_operation(
        progress_latch(state),
        owner_id,
        owner_generation,
        result,
        completed_count > 0U
    );
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
    if (!increment_visible_generation(state)) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
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
    if (out_released != nullptr) {
        *out_released = 1U;
    }
    return finish_latch_operation(
        state_latch(state),
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_REDO_STATE_OK,
        true
    );
}

int mylite_ownerless_redo_state_cleanup_owner_with_latch_owner(
    void *state,
    std::size_t state_size,
    std::uint32_t dead_owner_id,
    std::uint64_t dead_owner_generation,
    std::uint32_t latch_owner_id,
    std::uint64_t latch_owner_generation,
    std::uint32_t *out_released
) {
    if (!state_valid(state, state_size) || dead_owner_id == 0U || dead_owner_generation == 0U ||
        latch_owner_id == 0U || latch_owner_generation == 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    if (out_released != nullptr) {
        *out_released = 0U;
    }

    const int latch_result = acquire_progress_latch(state, latch_owner_id, latch_owner_generation);
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }
    const std::uint32_t before = owner_active_state_count(state, dead_owner_id);
    clear_owner_entry_slots(state, dead_owner_id, dead_owner_generation);
    const bool incomplete =
        owner_has_incomplete_reservation(state, dead_owner_id, dead_owner_generation);
    const std::uint32_t after = owner_active_state_count(state, dead_owner_id);
    if (out_released != nullptr) {
        *out_released = before - after;
    }
    return finish_latch_operation(
        progress_latch(state),
        latch_owner_id,
        latch_owner_generation,
        incomplete ? MYLITE_OWNERLESS_REDO_STATE_OWNER_DEAD : MYLITE_OWNERLESS_REDO_STATE_OK,
        before != after
    );
}

int mylite_ownerless_redo_state_recover_dead_latches(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    const mylite_ownerless_process_registry_liveness_context *liveness
) {
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U ||
        liveness == nullptr) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    mylite_ownerless_latch_dead_owner dead_owner = {};
    int latch_result = mylite_ownerless_latch_acquire_recoverable(
        state_latch(state),
        owner_id,
        owner_generation,
        mylite_ownerless_process_registry_latch_owner_is_alive,
        const_cast<mylite_ownerless_process_registry_liveness_context *>(liveness),
        k_progress_latch_timeout_ms,
        &dead_owner
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED) {
        /* The legacy outer latch has no current protected mutations. */
        if (mylite_ownerless_latch_mark_consistent(
                state_latch(state),
                owner_id,
                owner_generation
            ) != MYLITE_OWNERLESS_LATCH_OK) {
            return MYLITE_OWNERLESS_REDO_STATE_ERROR;
        }
    } else if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }
    const int state_release_result = finish_latch_operation(
        state_latch(state),
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_REDO_STATE_OK,
        latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED
    );
    if (state_release_result != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return state_release_result;
    }

    dead_owner = {};
    latch_result = mylite_ownerless_latch_acquire_recoverable(
        progress_latch(state),
        owner_id,
        owner_generation,
        mylite_ownerless_process_registry_latch_owner_is_alive,
        const_cast<mylite_ownerless_process_registry_liveness_context *>(liveness),
        k_progress_latch_timeout_ms,
        &dead_owner
    );
    bool owner_coordination_required = false;
    if (latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED) {
        if (!repair_progress_state_locked(state)) {
            static_cast<void>(mylite_ownerless_latch_mark_not_recoverable(
                progress_latch(state),
                owner_id,
                owner_generation
            ));
            return MYLITE_OWNERLESS_REDO_STATE_OWNER_DEAD;
        }
        /* The latch generation can be zero/stale in a dead publication window. */
        owner_coordination_required =
            owner_has_incomplete_reservation(state, dead_owner.owner_id, 0U);
        if (mylite_ownerless_latch_mark_consistent(
                progress_latch(state),
                owner_id,
                owner_generation
            ) != MYLITE_OWNERLESS_LATCH_OK) {
            return MYLITE_OWNERLESS_REDO_STATE_ERROR;
        }
    } else if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }
    return finish_latch_operation(
        progress_latch(state),
        owner_id,
        owner_generation,
        owner_coordination_required ? MYLITE_OWNERLESS_REDO_STATE_OWNER_DEAD
                                    : MYLITE_OWNERLESS_REDO_STATE_OK,
        latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED
    );
}

int mylite_ownerless_redo_state_finish_pending_progress_release(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    const int latch_result = acquire_progress_latch(state, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }
    return finish_latch_operation(
        progress_latch(state),
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_REDO_STATE_OK,
        false
    );
}

int mylite_ownerless_redo_state_finish_pending_state_release(
    void *state,
    std::size_t state_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (!state_valid(state, state_size) || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }
    const int latch_result = mylite_ownerless_latch_acquire(
        state_latch(state),
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        k_progress_latch_timeout_ms
    );
    if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_redo_state_result(latch_result);
    }
    return finish_latch_operation(
        state_latch(state),
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_REDO_STATE_OK,
        false
    );
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
    out_snapshot->visible_generation = load64(state, k_visible_generation_offset);
    out_snapshot->refcount = load32(state, k_refcount_offset);
    out_snapshot->active_reservation_count = active_reservation_count(state);
    out_snapshot->completed_range_count = completed_range_count(state);
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
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count; ++slot_index) {
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
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count; ++slot_index) {
        unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        if (load32(slot, k_active_reservation_slot_state_offset) !=
                k_active_reservation_slot_state_active ||
            load32(slot, k_active_reservation_slot_owner_id_offset) != owner_id ||
            load64(slot, k_active_reservation_slot_owner_generation_offset) != owner_generation ||
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
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count; ++slot_index) {
        unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        if (load32(slot, k_active_reservation_slot_state_offset) ==
                k_active_reservation_slot_state_entry &&
            load32(slot, k_active_reservation_slot_owner_id_offset) == owner_id &&
            load64(slot, k_active_reservation_slot_owner_generation_offset) == owner_generation) {
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
    bool new_slot = false;
    if (slot == nullptr) {
        slot = find_free_active_reservation_slot(state);
        new_slot = slot != nullptr;
    }
    if (slot == nullptr) {
        return finish_latch_operation(
            progress_latch(state),
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_REDO_STATE_ERROR,
            false
        );
    }

    const std::uint64_t refcount =
        new_slot ? 0U : load64(slot, k_active_entry_slot_refcount_offset);
    const std::uint32_t global_refcount = load32(state, k_refcount_offset);
    if (refcount == std::numeric_limits<std::uint64_t>::max() ||
        global_refcount == std::numeric_limits<std::uint32_t>::max()) {
        return finish_latch_operation(
            progress_latch(state),
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_REDO_STATE_ERROR,
            false
        );
    }
    store64(slot, k_active_entry_slot_refcount_offset, refcount + 1U);
    if (new_slot) {
        store64(slot, k_active_reservation_slot_owner_generation_offset, owner_generation);
        store32(slot, k_active_reservation_slot_owner_id_offset, owner_id);
    }
    __atomic_add_fetch(
        reinterpret_cast<std::uint32_t *>(static_cast<unsigned char *>(state) + k_refcount_offset),
        1U,
        __ATOMIC_ACQ_REL
    );
    if (new_slot) {
        store32(
            slot,
            k_active_reservation_slot_state_offset,
            k_active_reservation_slot_state_entry
        );
    }

    return finish_latch_operation(
        progress_latch(state),
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_REDO_STATE_OK,
        true
    );
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

    const int result = leave_active_owner_locked(
        state,
        owner_id,
        owner_generation,
        latest_lsn,
        out_advanced_latest_lsn,
        out_remaining
    );

    return finish_latch_operation(
        progress_latch(state),
        owner_id,
        owner_generation,
        result,
        result == MYLITE_OWNERLESS_REDO_STATE_OK
    );
}

int leave_active_owner_locked(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t latest_lsn,
    std::uint64_t *out_advanced_latest_lsn,
    std::uint32_t *out_remaining
) {
    unsigned char *slot = find_active_entry_slot(state, owner_id, owner_generation);
    if (slot == nullptr) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    const std::uint64_t owner_refcount = load64(slot, k_active_entry_slot_refcount_offset);
    const std::uint32_t global_refcount = load32(state, k_refcount_offset);
    if (owner_refcount == 0U || owner_refcount > global_refcount) {
        return MYLITE_OWNERLESS_REDO_STATE_ERROR;
    }

    const std::uint64_t previous_lsn = load64(state, k_latest_lsn_offset);
    if (latest_lsn > previous_lsn) {
        const std::uint64_t published_lsn = fetch_max64(state, k_latest_lsn_offset, latest_lsn);
        fetch_max64(state, k_reserved_lsn_offset, latest_lsn);
        if (active_reservation_count(state) == 0U) {
            const std::uint64_t written_lsn = load64(state, k_written_lsn_offset);
            if (latest_lsn > written_lsn) {
                store64(state, k_written_lsn_offset, latest_lsn);
                static_cast<void>(drain_completed_ranges(state, latest_lsn));
            }
        }
        if (out_advanced_latest_lsn != nullptr && published_lsn == latest_lsn) {
            *out_advanced_latest_lsn = latest_lsn;
        }
    }

    if (owner_refcount == 1U) {
        clear_active_reservation_slot(state, slot);
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
    return MYLITE_OWNERLESS_REDO_STATE_OK;
}

int complete_write_locked(
    void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t *out_written_lsn
) {
    const std::uint64_t previous_written_lsn = load64(state, k_written_lsn_offset);
    std::uint64_t written_lsn = previous_written_lsn;
    int result = MYLITE_OWNERLESS_REDO_STATE_OK;
    unsigned char *slot =
        find_active_reservation_slot(state, owner_id, owner_generation, start_lsn, end_lsn);
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
        clear_active_reservation_slot(state, slot);
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK && out_written_lsn != nullptr &&
        written_lsn > previous_written_lsn) {
        *out_written_lsn = written_lsn;
    }
    return result;
}

void reserve_active_range(
    void *state,
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
    increment_active_reservation_count(state);
    store32(slot, k_active_reservation_slot_state_offset, k_active_reservation_slot_state_active);
}

void clear_active_reservation_slot(void *state, unsigned char *slot) {
    const bool was_active = load32(slot, k_active_reservation_slot_state_offset) ==
                            k_active_reservation_slot_state_active;
    clear_redo_state_slot(slot);
    if (was_active) {
        decrement_active_reservation_count(state);
    }
}

void clear_redo_state_slot(unsigned char *slot) {
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

    const std::uint32_t before = owner_active_state_count(state, owner_id);
    clear_owner_entry_slots(state, owner_id, owner_generation);
    const std::uint32_t after = owner_active_state_count(state, owner_id);

    return finish_latch_operation(
        progress_latch(state),
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_REDO_STATE_OK,
        before != after
    );
}

void clear_owner_entry_slots(void *state, std::uint32_t owner_id, std::uint64_t owner_generation) {
    while (unsigned char *slot = find_active_entry_slot(state, owner_id, owner_generation)) {
        const std::uint64_t owner_refcount = load64(slot, k_active_entry_slot_refcount_offset);
        const std::uint32_t global_refcount = load32(state, k_refcount_offset);
        clear_redo_state_slot(slot);
        store32(
            state,
            k_refcount_offset,
            owner_refcount >= global_refcount
                ? 0U
                : static_cast<std::uint32_t>(global_refcount - owner_refcount)
        );
    }
}

bool repair_progress_state_locked(void *state) {
    auto *bytes = static_cast<unsigned char *>(state);
    std::uint64_t maximum_reserved_lsn = load64(state, k_reserved_lsn_offset);
    std::uint64_t total_refcount = 0U;

    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count; ++slot_index) {
        unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        const std::uint32_t slot_state = load32(slot, k_active_reservation_slot_state_offset);
        if (slot_state == k_active_reservation_slot_state_free) {
            continue;
        }
        const std::uint32_t slot_owner_id = load32(slot, k_active_reservation_slot_owner_id_offset);
        const std::uint64_t slot_owner_generation =
            load64(slot, k_active_reservation_slot_owner_generation_offset);
        if (slot_owner_id == 0U || slot_owner_generation == 0U) {
            return false;
        }
        if (slot_state == k_active_reservation_slot_state_active) {
            const std::uint64_t start_lsn = load64(slot, k_active_reservation_slot_start_offset);
            const std::uint64_t end_lsn = load64(slot, k_active_reservation_slot_end_offset);
            if (start_lsn == 0U || end_lsn <= start_lsn) {
                return false;
            }
            maximum_reserved_lsn = std::max(maximum_reserved_lsn, end_lsn);
            continue;
        }
        if (slot_state != k_active_reservation_slot_state_entry) {
            return false;
        }
        const std::uint64_t owner_refcount = load64(slot, k_active_entry_slot_refcount_offset);
        if (owner_refcount == 0U ||
            owner_refcount > std::numeric_limits<std::uint32_t>::max() - total_refcount) {
            return false;
        }
        total_refcount += owner_refcount;
    }

    std::uint32_t completed_count = 0U;
    const std::uint64_t initial_written_lsn = load64(state, k_written_lsn_offset);
    for (std::uint32_t slot_index = 0; slot_index < k_completed_range_slot_count; ++slot_index) {
        const std::size_t slot_offset = completed_range_slot_offset(slot_index);
        const std::uint64_t start_lsn =
            load64(state, slot_offset + k_completed_range_slot_start_offset);
        if (start_lsn == 0U) {
            store64(state, slot_offset + k_completed_range_slot_end_offset, 0U);
            continue;
        }
        const std::uint64_t end_lsn =
            load64(state, slot_offset + k_completed_range_slot_end_offset);
        if (end_lsn <= start_lsn) {
            return false;
        }
        if (end_lsn <= initial_written_lsn) {
            store64(state, slot_offset + k_completed_range_slot_start_offset, 0U);
            store64(state, slot_offset + k_completed_range_slot_end_offset, 0U);
            continue;
        }
        ++completed_count;
    }

    store64(state, k_reserved_lsn_offset, maximum_reserved_lsn);
    store32(state, k_refcount_offset, static_cast<std::uint32_t>(total_refcount));
    store32(state, k_completed_range_count_offset, completed_count);
    static_cast<void>(drain_completed_ranges(state, initial_written_lsn));

    const std::uint64_t repaired_written_lsn = load64(state, k_written_lsn_offset);
    std::uint32_t active_reservations = 0U;
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count; ++slot_index) {
        unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        if (load32(slot, k_active_reservation_slot_state_offset) !=
            k_active_reservation_slot_state_active) {
            continue;
        }
        if (load64(slot, k_active_reservation_slot_end_offset) <= repaired_written_lsn) {
            clear_redo_state_slot(slot);
            continue;
        }
        ++active_reservations;
    }
    completed_count = 0U;
    for (std::uint32_t slot_index = 0; slot_index < k_completed_range_slot_count; ++slot_index) {
        if (load64(
                state,
                completed_range_slot_offset(slot_index) + k_completed_range_slot_start_offset
            ) != 0U) {
            ++completed_count;
        }
    }
    store32(state, k_active_reservation_count_offset, active_reservations);
    store32(state, k_completed_range_count_offset, completed_count);
    return true;
}

bool owner_has_incomplete_reservation(
    const void *state,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    const auto *bytes = static_cast<const unsigned char *>(state);
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count; ++slot_index) {
        const unsigned char *slot =
            bytes + k_active_reservation_slots_offset +
            (static_cast<std::size_t>(slot_index) * k_active_reservation_slot_size);
        if (load32(slot, k_active_reservation_slot_state_offset) ==
                k_active_reservation_slot_state_active &&
            load32(slot, k_active_reservation_slot_owner_id_offset) == owner_id &&
            (owner_generation == 0U ||
             load64(slot, k_active_reservation_slot_owner_generation_offset) == owner_generation)) {
            return true;
        }
    }
    return false;
}

std::uint32_t active_reservation_count(const void *state) {
    return load32(state, k_active_reservation_count_offset);
}

std::uint32_t completed_range_count(const void *state) {
    return load32(state, k_completed_range_count_offset);
}

std::uint32_t owner_active_state_count(const void *state, std::uint32_t owner_id) {
    const auto *bytes = static_cast<const unsigned char *>(state);
    std::uint32_t count = 0;
    for (std::uint32_t slot_index = 0; slot_index < k_active_reservation_slot_count; ++slot_index) {
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

void increment_active_reservation_count(void *state) {
    const std::uint32_t count = load32(state, k_active_reservation_count_offset);
    if (count < k_active_reservation_slot_count) {
        store32(state, k_active_reservation_count_offset, count + 1U);
    }
}

void decrement_active_reservation_count(void *state) {
    const std::uint32_t count = load32(state, k_active_reservation_count_offset);
    if (count != 0U) {
        store32(state, k_active_reservation_count_offset, count - 1U);
    }
}

void increment_completed_range_count(void *state) {
    const std::uint32_t count = load32(state, k_completed_range_count_offset);
    if (count < k_completed_range_slot_count) {
        store32(state, k_completed_range_count_offset, count + 1U);
    }
}

void decrement_completed_range_count(void *state) {
    const std::uint32_t count = load32(state, k_completed_range_count_offset);
    if (count != 0U) {
        store32(state, k_completed_range_count_offset, count - 1U);
    }
}

bool record_completed_range(void *state, std::uint64_t start_lsn, std::uint64_t end_lsn) {
    std::uint64_t merged_start_lsn = start_lsn;
    std::uint64_t merged_end_lsn = end_lsn;
    bool expanded = true;
    while (expanded) {
        expanded = false;
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
            const std::uint64_t next_start = std::min(merged_start_lsn, slot_start_lsn);
            const std::uint64_t next_end = std::max(merged_end_lsn, slot_end_lsn);
            expanded = expanded || next_start != merged_start_lsn || next_end != merged_end_lsn;
            merged_start_lsn = next_start;
            merged_end_lsn = next_end;
        }
    }

    std::size_t publication_offset = MYLITE_OWNERLESS_REDO_STATE_SIZE;
    for (std::uint32_t slot_index = 0; slot_index < k_completed_range_slot_count; ++slot_index) {
        const std::size_t slot_offset = completed_range_slot_offset(slot_index);
        if (load64(state, slot_offset + k_completed_range_slot_start_offset) == 0U) {
            publication_offset = slot_offset;
            break;
        }
    }
    if (publication_offset == MYLITE_OWNERLESS_REDO_STATE_SIZE) {
        return false;
    }

    /* Publish the replacement first. A crash can leave duplicates, never a hole. */
    store64(state, publication_offset + k_completed_range_slot_end_offset, merged_end_lsn);
    increment_completed_range_count(state);
    store64(state, publication_offset + k_completed_range_slot_start_offset, merged_start_lsn);

    for (std::uint32_t slot_index = 0; slot_index < k_completed_range_slot_count; ++slot_index) {
        const std::size_t slot_offset = completed_range_slot_offset(slot_index);
        if (slot_offset == publication_offset) {
            continue;
        }
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
        store64(state, slot_offset + k_completed_range_slot_start_offset, 0U);
        store64(state, slot_offset + k_completed_range_slot_end_offset, 0U);
        decrement_completed_range_count(state);
    }
    return true;
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
            if (end_lsn > written_lsn) {
                written_lsn = end_lsn;
                store64(state, k_written_lsn_offset, written_lsn);
                advanced = true;
            }
            /* Clear the publication word only after written_lsn covers it. */
            store64(state, slot_offset + k_completed_range_slot_start_offset, 0U);
            store64(state, slot_offset + k_completed_range_slot_end_offset, 0U);
            decrement_completed_range_count(state);
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

bool increment_visible_generation(void *state) {
    auto *generation = reinterpret_cast<std::uint64_t *>(
        static_cast<unsigned char *>(state) + k_visible_generation_offset
    );
    std::uint64_t observed = __atomic_load_n(generation, __ATOMIC_ACQUIRE);
    for (;;) {
        if (observed == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        if (__atomic_compare_exchange_n(
                generation,
                &observed,
                observed + 1U,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE
            )) {
            return true;
        }
    }
}

int latch_result_to_redo_state_result(int latch_result) {
    if (latch_result == MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_REDO_STATE_OK;
    }
    if (latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT) {
        return MYLITE_OWNERLESS_REDO_STATE_TIMEOUT;
    }
    if (latch_result == MYLITE_OWNERLESS_LATCH_OWNER_DEAD ||
        latch_result == MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE) {
        return MYLITE_OWNERLESS_REDO_STATE_OWNER_DEAD;
    }
    return MYLITE_OWNERLESS_REDO_STATE_ERROR;
}

int finish_latch_operation(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
) {
    const int release_result = mylite_ownerless_latch_release(latch, owner_id, owner_generation);
    if (release_result == MYLITE_OWNERLESS_LATCH_OK) {
        return operation_result;
    }
    if (!operation_applied && release_result == MYLITE_OWNERLESS_LATCH_RELEASE_PENDING &&
        mylite_ownerless_latch_acquire(
            latch,
            owner_id,
            owner_generation,
            nullptr,
            nullptr,
            k_progress_latch_timeout_ms
        ) == MYLITE_OWNERLESS_LATCH_OK &&
        mylite_ownerless_latch_release(latch, owner_id, owner_generation) ==
            MYLITE_OWNERLESS_LATCH_OK) {
        return operation_result;
    }
    return operation_applied ? MYLITE_OWNERLESS_REDO_STATE_APPLIED_RELEASE_PENDING
                             : MYLITE_OWNERLESS_REDO_STATE_ERROR;
}

} // namespace

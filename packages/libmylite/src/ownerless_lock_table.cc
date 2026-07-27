#include "ownerless_lock_table.h"

#include "ownerless_latch.h"
#include "ownerless_process_registry.h"
#include "ownerless_wait.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace {

constexpr std::size_t k_header_entry_count_offset = 0;
constexpr std::size_t k_header_entry_size_offset = 4;
constexpr std::size_t k_header_generation_offset = 8;
constexpr std::size_t k_header_active_count_offset = 16;
constexpr std::size_t k_header_latch_offset = 24;
constexpr std::size_t k_header_waiting_count_offset = 56;
constexpr std::size_t k_header_wait_word_offset = 64;
constexpr std::size_t k_entry_key_hash_offset = 0;
constexpr std::size_t k_entry_owner_id_offset = 8;
constexpr std::size_t k_entry_state_offset = 12;
constexpr std::size_t k_entry_mode_offset = 16;
constexpr std::size_t k_entry_generation_offset = 24;
constexpr std::size_t k_entry_reference_count_offset = 32;
constexpr std::size_t k_entry_owner_generation_offset = 40;
constexpr std::size_t k_entry_session_id_offset = 48;
constexpr std::size_t k_entry_flags_offset = 56;
constexpr std::size_t k_entry_hog_lock_count_offset = 64;
constexpr std::size_t k_entry_max_write_lock_count_offset = 72;
constexpr std::uint32_t k_entry_state_free = 0;
constexpr std::uint32_t k_entry_state_active = 1;
constexpr std::uint32_t k_entry_state_waiting = 2;
constexpr std::uint64_t k_entry_flag_bypass_queued_waiters = 1U;
constexpr std::uint64_t k_entry_flag_deadlock_victim = 1U << 1U;
constexpr unsigned k_entry_deadlock_weight_shift = 32U;
constexpr std::uint64_t k_entry_deadlock_weight_mask = 0xffffffffULL
                                                       << k_entry_deadlock_weight_shift;
constexpr std::uint64_t k_entry_known_flags = k_entry_flag_bypass_queued_waiters |
                                              k_entry_flag_deadlock_victim |
                                              k_entry_deadlock_weight_mask;
constexpr unsigned k_lock_table_latch_timeout_ms = 5000U;
constexpr unsigned k_cancellation_wait_slice_ms = 50U;

static_assert(k_header_latch_offset + MYLITE_OWNERLESS_LATCH_SIZE <= k_header_waiting_count_offset);
static_assert(
    k_header_wait_word_offset + sizeof(mylite_ownerless_wait_word) <=
    MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE
);
static_assert(
    k_entry_session_id_offset + sizeof(std::uint64_t) <= MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE
);
static_assert(
    k_entry_max_write_lock_count_offset + sizeof(std::uint64_t) <=
    MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE
);

struct OwnerIdentity {
    std::uint32_t id = 0U;
    std::uint64_t generation = 0U;
    std::uint64_t session_id = 0U;
};

constexpr std::uint64_t k_legacy_session_id = std::numeric_limits<std::uint64_t>::max();

struct RequestSearchResult {
    unsigned char *own_active_entry = nullptr;
    unsigned char *waiting_entry = nullptr;
    unsigned char *free_entry = nullptr;
    bool active_conflict = false;
    bool waiting_priority_conflict = false;
    bool deadlock_victim = false;
    bool invalid_entry = false;
};

enum class CycleCheckResult { none, victim_is_requester, victim_is_other, error };

struct WaitGraphNode {
    OwnerIdentity owner;
    std::size_t parent_index = 0U;
    unsigned char *incoming_waiting_entry = nullptr;
};

std::chrono::steady_clock::time_point wait_deadline(std::uint64_t timeout_ms);
int acquire_table_latch(
    unsigned char *table,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
);
int release_table_latch(
    unsigned char *table,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
);
int finish_table_operation(
    unsigned char *table,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
);
int acquire_lock_until(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    bool bypass_queued_waiters,
    std::uint32_t deadlock_weight,
    std::uint64_t max_write_lock_count,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    std::chrono::steady_clock::time_point deadline
);
int wait_for_entry_change(
    mylite_ownerless_wait_word *wait_word,
    std::uint32_t observed,
    bool cancellation_enabled,
    std::chrono::steady_clock::time_point deadline
);
int finish_waiting_request(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    bool bypass_queued_waiters,
    std::uint64_t waiter_sequence,
    std::uint64_t max_write_lock_count,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    int wait_result
);
int release_lock_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode
);
int reclassify_lock_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t old_mode,
    std::uint32_t new_mode
);
int release_owner_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_entries
);
std::uint32_t owner_active_count_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint32_t owner_id
);
std::uint32_t owner_entry_count_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint32_t owner_id
);
RequestSearchResult find_request_state(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    bool bypass_queued_waiters,
    std::uint64_t waiter_sequence,
    std::uint64_t max_write_lock_count
);
unsigned char *find_active_entry(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode
);
int initialize_active_entry(
    unsigned char *table,
    unsigned char *entry,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    std::uint64_t flags
);
int initialize_waiting_entry(
    unsigned char *table,
    unsigned char *entry,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    bool bypass_queued_waiters,
    std::uint32_t deadlock_weight,
    std::uint64_t hog_lock_count,
    std::uint64_t max_write_lock_count,
    std::uint64_t *out_sequence
);
int grant_waiting_entry(unsigned char *table, std::size_t mapping_size, unsigned char *entry);
int clear_lock_entry(unsigned char *table, unsigned char *entry);
CycleCheckResult wait_cycle_exists(
    unsigned char *table,
    std::size_t mapping_size,
    OwnerIdentity start,
    unsigned char *start_waiting_entry
);
bool waiting_entry_is_blocked_by(
    unsigned char *table,
    std::size_t mapping_size,
    const unsigned char *waiting_entry,
    const unsigned char *candidate
);
bool owner_identity_equal(OwnerIdentity left, OwnerIdentity right);
OwnerIdentity entry_owner(const unsigned char *entry);
bool entry_is_valid(const unsigned char *entry);
bool repair_lock_table_locked(unsigned char *table, std::size_t mapping_size);
bool lock_mode_is_valid(std::uint32_t mode);
bool lock_modes_conflict(std::uint32_t requested_mode, std::uint32_t active_mode);
bool waiting_mode_blocks_request(std::uint32_t requested_mode, std::uint32_t waiting_mode);
bool waiting_modes_have_equal_priority(std::uint32_t left_mode, std::uint32_t right_mode);
bool mode_is_hog_lock(std::uint32_t mode);
std::uint64_t waiting_hog_lock_count_for_key(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash
);
bool key_has_low_priority_waiter(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash
);
void update_hog_lock_count_after_grant(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t granted_mode
);
void normalize_hog_lock_count_for_key(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash
);
bool entry_is_deadlock_victim(const unsigned char *entry);
std::uint32_t entry_deadlock_weight(const unsigned char *entry);
void consider_deadlock_victim(unsigned char *entry, unsigned char **victim);
bool cancellation_requested(
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context
);
std::uint64_t advance_generation(unsigned char *table);
void wake_table_waiters(unsigned char *table);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
std::chrono::steady_clock::time_point bounded_latch_deadline(
    std::chrono::steady_clock::time_point operation_deadline
);
bool lock_table_size_fits(std::uint32_t entry_count);
bool mapping_can_hold_table(const void *mapping, std::size_t mapping_size);
std::uint32_t entry_count(const unsigned char *table);
unsigned char *entry_at(unsigned char *table, std::uint32_t index);
mylite_ownerless_latch *table_latch(unsigned char *table);
mylite_ownerless_wait_word *table_wait_word(unsigned char *table);
std::uint32_t load32(const unsigned char *base, std::size_t offset);
std::uint64_t load64(const unsigned char *base, std::size_t offset);
void store32(unsigned char *base, std::size_t offset, std::uint32_t value);
void store64(unsigned char *base, std::size_t offset, std::uint64_t value);

} // namespace

std::size_t mylite_ownerless_lock_table_size(std::uint32_t entry_count) {
    if (!lock_table_size_fits(entry_count)) {
        return 0U;
    }
    return MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE +
           (static_cast<std::size_t>(entry_count) * MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE);
}

int mylite_ownerless_lock_table_initialize(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t entry_count
) {
    const std::size_t table_size = mylite_ownerless_lock_table_size(entry_count);
    if (mapping == nullptr || table_size == 0U || mapping_size < table_size) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    auto *table = static_cast<unsigned char *>(mapping);
    std::memset(table, 0, table_size);
    store32(table, k_header_entry_count_offset, entry_count);
    store32(table, k_header_entry_size_offset, MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE);
    return MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

int mylite_ownerless_lock_table_acquire_exclusive(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
        timeout_ms
    );
}

int mylite_ownerless_lock_table_acquire_shared(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
        timeout_ms
    );
}

int mylite_ownerless_lock_table_acquire_upgradable(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE,
        timeout_ms
    );
}

int mylite_ownerless_lock_table_acquire_mode(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mode,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode_for_session(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        k_legacy_session_id,
        mode,
        timeout_ms
    );
}

int mylite_ownerless_lock_table_acquire_mode_for_session(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode_for_session_with_options(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        mode,
        0,
        nullptr,
        nullptr,
        timeout_ms
    );
}

int mylite_ownerless_lock_table_acquire_mode_for_session_with_options(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    int bypass_queued_waiters,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode_for_session_with_deadlock_weight(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        mode,
        bypass_queued_waiters,
        MYLITE_OWNERLESS_LOCK_TABLE_DEFAULT_DEADLOCK_WEIGHT,
        is_cancelled,
        cancel_context,
        timeout_ms
    );
}

int mylite_ownerless_lock_table_acquire_mode_for_session_with_deadlock_weight(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    int bypass_queued_waiters,
    std::uint32_t deadlock_weight,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_lock_table_acquire_mode_for_session_with_scheduling(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        mode,
        bypass_queued_waiters,
        deadlock_weight,
        std::numeric_limits<std::uint64_t>::max(),
        is_cancelled,
        cancel_context,
        timeout_ms
    );
}

int mylite_ownerless_lock_table_acquire_mode_for_session_with_scheduling(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    int bypass_queued_waiters,
    std::uint32_t deadlock_weight,
    std::uint64_t max_write_lock_count,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    std::uint64_t timeout_ms
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || key_hash == 0U || owner_id == 0U ||
        owner_generation == 0U || session_id == 0U || !lock_mode_is_valid(mode) ||
        max_write_lock_count == 0U || (bypass_queued_waiters != 0 && bypass_queued_waiters != 1)) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        mode,
        bypass_queued_waiters != 0,
        deadlock_weight,
        max_write_lock_count,
        is_cancelled,
        cancel_context,
        wait_deadline(timeout_ms)
    );
}

int mylite_ownerless_lock_table_release_exclusive(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    return mylite_ownerless_lock_table_release_mode(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE
    );
}

int mylite_ownerless_lock_table_release_shared(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    return mylite_ownerless_lock_table_release_mode(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_LOCK_TABLE_SHARED
    );
}

int mylite_ownerless_lock_table_release_upgradable(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    return mylite_ownerless_lock_table_release_mode(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE
    );
}

int mylite_ownerless_lock_table_release_mode(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mode
) {
    return mylite_ownerless_lock_table_release_mode_for_session(
        mapping,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        k_legacy_session_id,
        mode
    );
}

int mylite_ownerless_lock_table_release_mode_for_session(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || key_hash == 0U || owner_id == 0U ||
        owner_generation == 0U || session_id == 0U || !lock_mode_is_valid(mode)) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    auto *table = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_table_latch(
        table,
        owner_id,
        owner_generation,
        wait_deadline(k_lock_table_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return latch_result;
    }
    const int release_result = release_lock_locked(
        table,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        mode
    );
    return finish_table_operation(
        table,
        owner_id,
        owner_generation,
        release_result,
        release_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

int mylite_ownerless_lock_table_reclassify_mode_for_session(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t old_mode,
    std::uint32_t new_mode
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || key_hash == 0U || owner_id == 0U ||
        owner_generation == 0U || session_id == 0U || !lock_mode_is_valid(old_mode) ||
        !lock_mode_is_valid(new_mode)) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    auto *table = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_table_latch(
        table,
        owner_id,
        owner_generation,
        wait_deadline(k_lock_table_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return latch_result;
    }
    const int reclassify_result = reclassify_lock_locked(
        table,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        old_mode,
        new_mode
    );
    return finish_table_operation(
        table,
        owner_id,
        owner_generation,
        reclassify_result,
        reclassify_result == MYLITE_OWNERLESS_LOCK_TABLE_OK && old_mode != new_mode
    );
}

int mylite_ownerless_lock_table_release_owner(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t latch_owner_id,
    std::uint64_t latch_owner_generation,
    std::uint32_t *out_released_entries
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || owner_id == 0U || latch_owner_id == 0U ||
        latch_owner_generation == 0U || out_released_entries == nullptr) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    auto *table = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_table_latch(
        table,
        latch_owner_id,
        latch_owner_generation,
        wait_deadline(k_lock_table_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return latch_result;
    }
    const int release_result =
        release_owner_locked(table, mapping_size, owner_id, out_released_entries);
    return finish_table_operation(
        table,
        latch_owner_id,
        latch_owner_generation,
        release_result,
        release_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

int mylite_ownerless_lock_table_owner_active_count(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t latch_owner_id,
    std::uint64_t latch_owner_generation,
    std::uint32_t *out_active_count
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || owner_id == 0U || latch_owner_id == 0U ||
        latch_owner_generation == 0U || out_active_count == nullptr) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    auto *table = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_table_latch(
        table,
        latch_owner_id,
        latch_owner_generation,
        wait_deadline(k_lock_table_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return latch_result;
    }
    *out_active_count = owner_active_count_locked(table, mapping_size, owner_id);
    return finish_table_operation(
        table,
        latch_owner_id,
        latch_owner_generation,
        MYLITE_OWNERLESS_LOCK_TABLE_OK,
        false
    );
}

int mylite_ownerless_lock_table_recover_dead_latch(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    const mylite_ownerless_process_registry_liveness_context *liveness
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || liveness == nullptr) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    auto *table = static_cast<unsigned char *>(mapping);
    mylite_ownerless_latch_dead_owner dead_owner = {};
    const int latch_result = mylite_ownerless_latch_acquire_recoverable(
        table_latch(table),
        owner_id,
        owner_generation,
        mylite_ownerless_process_registry_latch_owner_is_alive,
        const_cast<mylite_ownerless_process_registry_liveness_context *>(liveness),
        k_lock_table_latch_timeout_ms,
        &dead_owner
    );
    bool owner_cleanup_required = false;
    if (latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED) {
        if (!repair_lock_table_locked(table, mapping_size)) {
            static_cast<void>(mylite_ownerless_latch_mark_not_recoverable(
                table_latch(table),
                owner_id,
                owner_generation
            ));
            return MYLITE_OWNERLESS_LOCK_TABLE_OWNER_DEAD;
        }
        owner_cleanup_required =
            owner_entry_count_locked(table, mapping_size, dead_owner.owner_id) != 0U;
        if (mylite_ownerless_latch_mark_consistent(
                table_latch(table),
                owner_id,
                owner_generation
            ) != MYLITE_OWNERLESS_LATCH_OK) {
            return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
        }
    } else if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        if (latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT) {
            return MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT;
        }
        return latch_result == MYLITE_OWNERLESS_LATCH_OWNER_DEAD ||
                       latch_result == MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE
                   ? MYLITE_OWNERLESS_LOCK_TABLE_OWNER_DEAD
                   : MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    if (mylite_ownerless_latch_release(table_latch(table), owner_id, owner_generation) !=
        MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    return owner_cleanup_required ? MYLITE_OWNERLESS_LOCK_TABLE_OWNER_DEAD
                                  : MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

namespace {

std::chrono::steady_clock::time_point wait_deadline(std::uint64_t timeout_ms) {
    const auto now = std::chrono::steady_clock::now();
    const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::time_point::max() - now
    )
                               .count();
    if (available <= 0 || timeout_ms >= static_cast<std::uint64_t>(available)) {
        return std::chrono::steady_clock::time_point::max();
    }
    return now + std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

int acquire_table_latch(
    unsigned char *table,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
) {
    const auto operation_deadline = deadline;
    const bool nonblocking_attempt = std::chrono::steady_clock::now() >= operation_deadline;
    deadline = nonblocking_attempt ? std::chrono::steady_clock::now() +
                                         std::chrono::milliseconds(k_lock_table_latch_timeout_ms)
                                   : bounded_latch_deadline(operation_deadline);
    const int latch_result = mylite_ownerless_latch_acquire(
        table_latch(table),
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        remaining_timeout_ms(deadline)
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_LOCK_TABLE_OK;
    }
    /*
     * The SQL timeout applies to lock conflicts, not entry into this internal
     * critical section. Even a nonblocking lock request must wait for an
     * in-flight table mutation to finish. Normal operations do not own the
     * process-registry context required to prove latch-owner death, so a latch
     * timeout is a coordination fault. The runtime later calls
     * recover_dead_latch() with authoritative liveness state.
     */
    return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
}

int release_table_latch(
    unsigned char *table,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    const int release_result =
        mylite_ownerless_latch_release(table_latch(table), owner_id, owner_generation);
    if (release_result == MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_LOCK_TABLE_OK;
    }
    return release_result == MYLITE_OWNERLESS_LATCH_RELEASE_PENDING
               ? MYLITE_OWNERLESS_LOCK_TABLE_APPLIED_RELEASE_PENDING
               : MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
}

int finish_table_operation(
    unsigned char *table,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
) {
    const int release_result = release_table_latch(table, owner_id, owner_generation);
    if (release_result == MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return operation_result;
    }
    return operation_applied ? MYLITE_OWNERLESS_LOCK_TABLE_APPLIED_RELEASE_PENDING
                             : MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
}

int acquire_lock_until(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    bool bypass_queued_waiters,
    std::uint32_t deadlock_weight,
    std::uint64_t max_write_lock_count,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    std::chrono::steady_clock::time_point deadline
) {
    std::uint64_t waiter_sequence = 0U;

    for (;;) {
        const int latch_result = acquire_table_latch(table, owner_id, owner_generation, deadline);
        if (latch_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
            if (waiter_sequence != 0U) {
                return finish_waiting_request(
                    table,
                    mapping_size,
                    key_hash,
                    owner_id,
                    owner_generation,
                    session_id,
                    mode,
                    bypass_queued_waiters,
                    waiter_sequence,
                    max_write_lock_count,
                    is_cancelled,
                    cancel_context,
                    latch_result
                );
            }
            return latch_result;
        }

        RequestSearchResult search = find_request_state(
            table,
            mapping_size,
            key_hash,
            owner_id,
            owner_generation,
            session_id,
            mode,
            bypass_queued_waiters,
            waiter_sequence,
            max_write_lock_count
        );
        if (search.invalid_entry) {
            return finish_table_operation(
                table,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_LOCK_TABLE_ERROR,
                waiter_sequence != 0U
            );
        }
        if (waiter_sequence != 0U && search.waiting_entry == nullptr) {
            return finish_table_operation(
                table,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_LOCK_TABLE_KILLED,
                false
            );
        }
        if (search.deadlock_victim) {
            const int clear_result = clear_lock_entry(table, search.waiting_entry);
            return finish_table_operation(
                table,
                owner_id,
                owner_generation,
                clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
                    ? MYLITE_OWNERLESS_LOCK_TABLE_DEADLOCK
                    : clear_result,
                clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
            );
        }
        if (search.own_active_entry != nullptr) {
            const bool cleared_waiter = search.waiting_entry != nullptr;
            unsigned char *new_entry =
                search.waiting_entry != nullptr ? search.waiting_entry : search.free_entry;
            if (search.waiting_entry != nullptr &&
                clear_lock_entry(table, search.waiting_entry) != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
                return finish_table_operation(
                    table,
                    owner_id,
                    owner_generation,
                    MYLITE_OWNERLESS_LOCK_TABLE_ERROR,
                    false
                );
            }
            const int increment_result = new_entry == nullptr ? MYLITE_OWNERLESS_LOCK_TABLE_FULL
                                                              : initialize_active_entry(
                                                                    table,
                                                                    new_entry,
                                                                    key_hash,
                                                                    owner_id,
                                                                    owner_generation,
                                                                    session_id,
                                                                    mode,
                                                                    0U
                                                                );
            return finish_table_operation(
                table,
                owner_id,
                owner_generation,
                increment_result,
                cleared_waiter || increment_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
            );
        }
        const bool can_grant = !search.active_conflict && !search.waiting_priority_conflict;
        if (can_grant && (search.waiting_entry != nullptr || search.free_entry != nullptr)) {
            int grant_result;
            if (search.waiting_entry != nullptr) {
                grant_result = grant_waiting_entry(table, mapping_size, search.waiting_entry);
            } else {
                grant_result = initialize_active_entry(
                    table,
                    search.free_entry,
                    key_hash,
                    owner_id,
                    owner_generation,
                    session_id,
                    mode,
                    0U
                );
                if (grant_result == MYLITE_OWNERLESS_LOCK_TABLE_OK) {
                    update_hog_lock_count_after_grant(table, mapping_size, key_hash, mode);
                }
            }
            return finish_table_operation(
                table,
                owner_id,
                owner_generation,
                grant_result,
                grant_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
            );
        }
        if (cancellation_requested(is_cancelled, cancel_context)) {
            int clear_result = MYLITE_OWNERLESS_LOCK_TABLE_OK;
            if (search.waiting_entry != nullptr) {
                clear_result = clear_lock_entry(table, search.waiting_entry);
            }
            return finish_table_operation(
                table,
                owner_id,
                owner_generation,
                clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK ? MYLITE_OWNERLESS_LOCK_TABLE_KILLED
                                                               : clear_result,
                search.waiting_entry != nullptr && clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
            );
        }

        if (search.waiting_entry == nullptr) {
            if (search.free_entry == nullptr) {
                return finish_table_operation(
                    table,
                    owner_id,
                    owner_generation,
                    MYLITE_OWNERLESS_LOCK_TABLE_FULL,
                    false
                );
            }
            if (remaining_timeout_ms(deadline) == 0U) {
                return finish_table_operation(
                    table,
                    owner_id,
                    owner_generation,
                    MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT,
                    false
                );
            }
            const int publish_result = initialize_waiting_entry(
                table,
                search.free_entry,
                key_hash,
                owner_id,
                owner_generation,
                session_id,
                mode,
                bypass_queued_waiters,
                deadlock_weight,
                waiting_hog_lock_count_for_key(table, mapping_size, key_hash),
                max_write_lock_count,
                &waiter_sequence
            );
            if (publish_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
                return finish_table_operation(
                    table,
                    owner_id,
                    owner_generation,
                    publish_result,
                    false
                );
            }
            search.waiting_entry = search.free_entry;

            CycleCheckResult cycle_result = CycleCheckResult::none;
            do {
                cycle_result = wait_cycle_exists(
                    table,
                    mapping_size,
                    OwnerIdentity{owner_id, owner_generation, session_id},
                    search.waiting_entry
                );
            } while (cycle_result == CycleCheckResult::victim_is_other);
            if (cycle_result == CycleCheckResult::victim_is_requester ||
                cycle_result == CycleCheckResult::error) {
                const int clear_result = clear_lock_entry(table, search.waiting_entry);
                int operation_result = clear_result;
                if (clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK) {
                    operation_result = cycle_result == CycleCheckResult::victim_is_requester
                                           ? MYLITE_OWNERLESS_LOCK_TABLE_DEADLOCK
                                           : MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
                }
                return finish_table_operation(
                    table,
                    owner_id,
                    owner_generation,
                    operation_result,
                    clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
                );
            }
        }

        mylite_ownerless_wait_word *wait_word = table_wait_word(table);
        const std::uint32_t observed = mylite_ownerless_wait_load(wait_word);
        const int latch_release_result = finish_table_operation(
            table,
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_LOCK_TABLE_OK,
            waiter_sequence != 0U
        );
        if (latch_release_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
            return latch_release_result;
        }
        const int wait_result =
            wait_for_entry_change(wait_word, observed, is_cancelled != nullptr, deadline);
        if (wait_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
            return finish_waiting_request(
                table,
                mapping_size,
                key_hash,
                owner_id,
                owner_generation,
                session_id,
                mode,
                bypass_queued_waiters,
                waiter_sequence,
                max_write_lock_count,
                is_cancelled,
                cancel_context,
                wait_result
            );
        }
    }
}

int wait_for_entry_change(
    mylite_ownerless_wait_word *wait_word,
    std::uint32_t observed,
    bool cancellation_enabled,
    std::chrono::steady_clock::time_point deadline
) {
    const unsigned remaining_ms = remaining_timeout_ms(deadline);
    if (remaining_ms == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT;
    }
    const unsigned timeout_ms =
        cancellation_enabled ? std::min(remaining_ms, k_cancellation_wait_slice_ms) : remaining_ms;
    const int wait_result = mylite_ownerless_wait_for_change(wait_word, observed, timeout_ms);
    if (wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT) {
        if (cancellation_enabled && timeout_ms < remaining_ms) {
            return MYLITE_OWNERLESS_LOCK_TABLE_OK;
        }
        return MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT;
    }
    return wait_result == MYLITE_OWNERLESS_WAIT_OK ? MYLITE_OWNERLESS_LOCK_TABLE_OK
                                                   : MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
}

int finish_waiting_request(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    bool bypass_queued_waiters,
    std::uint64_t waiter_sequence,
    std::uint64_t max_write_lock_count,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    int wait_result
) {
    const int latch_result = acquire_table_latch(
        table,
        owner_id,
        owner_generation,
        wait_deadline(k_lock_table_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    const RequestSearchResult search = find_request_state(
        table,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        mode,
        bypass_queued_waiters,
        waiter_sequence,
        max_write_lock_count
    );
    if (search.invalid_entry) {
        return finish_table_operation(
            table,
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_LOCK_TABLE_ERROR,
            true
        );
    }
    if (search.waiting_entry == nullptr) {
        return finish_table_operation(
            table,
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_LOCK_TABLE_KILLED,
            false
        );
    }
    if (search.deadlock_victim) {
        const int clear_result = clear_lock_entry(table, search.waiting_entry);
        return finish_table_operation(
            table,
            owner_id,
            owner_generation,
            clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK ? MYLITE_OWNERLESS_LOCK_TABLE_DEADLOCK
                                                           : clear_result,
            clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
    }
    if (search.own_active_entry != nullptr) {
        if (clear_lock_entry(table, search.waiting_entry) != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
            return finish_table_operation(
                table,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_LOCK_TABLE_ERROR,
                false
            );
        }
        const RequestSearchResult refreshed = find_request_state(
            table,
            mapping_size,
            key_hash,
            owner_id,
            owner_generation,
            session_id,
            mode,
            bypass_queued_waiters,
            0U,
            max_write_lock_count
        );
        const int increment_result = refreshed.free_entry == nullptr
                                         ? MYLITE_OWNERLESS_LOCK_TABLE_FULL
                                         : initialize_active_entry(
                                               table,
                                               refreshed.free_entry,
                                               key_hash,
                                               owner_id,
                                               owner_generation,
                                               session_id,
                                               mode,
                                               0U
                                           );
        return finish_table_operation(table, owner_id, owner_generation, increment_result, true);
    }
    if (!search.active_conflict && !search.waiting_priority_conflict) {
        const int grant_result = grant_waiting_entry(table, mapping_size, search.waiting_entry);
        return finish_table_operation(
            table,
            owner_id,
            owner_generation,
            grant_result,
            grant_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
    }

    const bool cancelled = cancellation_requested(is_cancelled, cancel_context);
    const int clear_result = clear_lock_entry(table, search.waiting_entry);
    int operation_result = clear_result;
    if (clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        operation_result = cancelled ? MYLITE_OWNERLESS_LOCK_TABLE_KILLED : wait_result;
    }
    return finish_table_operation(
        table,
        owner_id,
        owner_generation,
        operation_result,
        clear_result == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
}

int release_lock_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode
) {
    unsigned char *entry = find_active_entry(
        table,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        mode
    );
    if (entry == nullptr) {
        return MYLITE_OWNERLESS_LOCK_TABLE_NOT_FOUND;
    }

    const std::uint32_t reference_count = load32(entry, k_entry_reference_count_offset);
    if (reference_count > 1U) {
        store32(entry, k_entry_reference_count_offset, reference_count - 1U);
        return MYLITE_OWNERLESS_LOCK_TABLE_OK;
    }
    return clear_lock_entry(table, entry);
}

int reclassify_lock_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t old_mode,
    std::uint32_t new_mode
) {
    unsigned char *old_entry = find_active_entry(
        table,
        mapping_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        old_mode
    );
    if (old_entry == nullptr) {
        return MYLITE_OWNERLESS_LOCK_TABLE_NOT_FOUND;
    }
    if (old_mode == new_mode) {
        return MYLITE_OWNERLESS_LOCK_TABLE_OK;
    }
    if (load64(table, k_header_generation_offset) == std::numeric_limits<std::uint64_t>::max() ||
        load64(table, k_header_active_count_offset) == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    const OwnerIdentity owner{owner_id, owner_generation, session_id};
    const std::uint32_t count = entry_count(table);
    for (std::uint32_t index = 0U; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
        }
        const std::uint32_t state = load32(entry, k_entry_state_offset);
        if (state == k_entry_state_free) {
            continue;
        }
        if (!entry_is_valid(entry)) {
            return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
        }
        if (state == k_entry_state_active && load64(entry, k_entry_key_hash_offset) == key_hash &&
            !owner_identity_equal(entry_owner(entry), owner) &&
            lock_modes_conflict(new_mode, load32(entry, k_entry_mode_offset))) {
            return MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT;
        }
    }

    const std::uint64_t generation = advance_generation(table);
    if (generation == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    store32(old_entry, k_entry_mode_offset, new_mode);
    store64(old_entry, k_entry_generation_offset, generation);
    wake_table_waiters(table);
    return MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

int release_owner_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_entries
) {
    std::uint32_t released_entries = 0;
    const std::uint32_t count = entry_count(table);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            break;
        }
        if (load32(entry, k_entry_state_offset) == k_entry_state_free ||
            load32(entry, k_entry_owner_id_offset) != owner_id) {
            continue;
        }

        if (clear_lock_entry(table, entry) != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
            *out_released_entries = released_entries;
            return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
        }
        ++released_entries;
    }

    *out_released_entries = released_entries;
    return MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

std::uint32_t owner_active_count_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint32_t owner_id
) {
    std::uint32_t active_count = 0U;
    const std::uint32_t count = entry_count(table);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            break;
        }
        if (load32(entry, k_entry_state_offset) == k_entry_state_active &&
            load32(entry, k_entry_owner_id_offset) == owner_id) {
            ++active_count;
        }
    }
    return active_count;
}

std::uint32_t owner_entry_count_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint32_t owner_id
) {
    std::uint32_t owned_count = 0U;
    const std::uint32_t count = entry_count(table);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            break;
        }
        const std::uint32_t state = load32(entry, k_entry_state_offset);
        if ((state == k_entry_state_active || state == k_entry_state_waiting) &&
            load32(entry, k_entry_owner_id_offset) == owner_id) {
            ++owned_count;
        }
    }
    return owned_count;
}

RequestSearchResult find_request_state(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    bool bypass_queued_waiters,
    std::uint64_t waiter_sequence,
    std::uint64_t max_write_lock_count
) {
    RequestSearchResult result;
    const OwnerIdentity owner{owner_id, owner_generation, session_id};
    const std::uint32_t count = entry_count(table);
    const bool starvation_breaker_active =
        max_write_lock_count != std::numeric_limits<std::uint64_t>::max() &&
        waiting_hog_lock_count_for_key(table, mapping_size, key_hash) >= max_write_lock_count &&
        key_has_low_priority_waiter(table, mapping_size, key_hash);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            result.invalid_entry = true;
            break;
        }
        const std::uint32_t state = load32(entry, k_entry_state_offset);
        if (state == k_entry_state_free) {
            if (result.free_entry == nullptr) {
                result.free_entry = entry;
            }
            continue;
        }
        if (!entry_is_valid(entry)) {
            result.invalid_entry = true;
            continue;
        }
        const OwnerIdentity entry_identity = entry_owner(entry);
        if (load64(entry, k_entry_key_hash_offset) != key_hash) {
            continue;
        }

        const std::uint32_t entry_mode = load32(entry, k_entry_mode_offset);
        if (state == k_entry_state_active) {
            if (owner_identity_equal(entry_identity, owner)) {
                if (entry_mode == mode) {
                    result.own_active_entry = entry;
                }
            } else if (lock_modes_conflict(mode, entry_mode)) {
                result.active_conflict = true;
            }
            continue;
        }

        const std::uint64_t sequence = load64(entry, k_entry_generation_offset);
        if (waiter_sequence != 0U && sequence == waiter_sequence &&
            owner_identity_equal(entry_identity, owner)) {
            result.waiting_entry = entry;
            result.deadlock_victim = entry_is_deadlock_victim(entry);
            continue;
        }
        if (!bypass_queued_waiters && !owner_identity_equal(entry_identity, owner)) {
            bool priority_conflict = waiting_mode_blocks_request(mode, entry_mode);
            if (starvation_breaker_active) {
                priority_conflict = mode_is_hog_lock(mode)
                                        ? !mode_is_hog_lock(entry_mode)
                                        : (!mode_is_hog_lock(entry_mode) &&
                                           waiting_mode_blocks_request(mode, entry_mode));
            }
            const bool fifo_conflict = waiting_modes_have_equal_priority(mode, entry_mode) &&
                                       lock_modes_conflict(mode, entry_mode) &&
                                       (waiter_sequence == 0U || sequence < waiter_sequence);
            if (priority_conflict || fifo_conflict) {
                result.waiting_priority_conflict = true;
            }
        }
    }
    return result;
}

unsigned char *find_active_entry(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode
) {
    const std::uint32_t count = entry_count(table);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            return nullptr;
        }
        if (load32(entry, k_entry_state_offset) == k_entry_state_active &&
            load64(entry, k_entry_key_hash_offset) == key_hash &&
            load32(entry, k_entry_owner_id_offset) == owner_id &&
            load64(entry, k_entry_owner_generation_offset) == owner_generation &&
            load64(entry, k_entry_session_id_offset) == session_id &&
            load32(entry, k_entry_mode_offset) == mode) {
            return entry;
        }
    }
    return nullptr;
}

int initialize_active_entry(
    unsigned char *table,
    unsigned char *entry,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    std::uint64_t flags
) {
    if (load64(table, k_header_active_count_offset) == std::numeric_limits<std::uint64_t>::max()) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    const std::uint64_t generation = advance_generation(table);
    if (generation == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    store64(entry, k_entry_key_hash_offset, key_hash);
    store32(entry, k_entry_owner_id_offset, owner_id);
    store32(entry, k_entry_mode_offset, mode);
    store32(entry, k_entry_reference_count_offset, 1U);
    store64(entry, k_entry_owner_generation_offset, owner_generation);
    store64(entry, k_entry_session_id_offset, session_id);
    store64(entry, k_entry_flags_offset, flags);
    store64(entry, k_entry_hog_lock_count_offset, 0U);
    store64(entry, k_entry_max_write_lock_count_offset, 0U);
    store64(entry, k_entry_generation_offset, generation);
    store64(table, k_header_active_count_offset, load64(table, k_header_active_count_offset) + 1U);
    store32(entry, k_entry_state_offset, k_entry_state_active);
    return MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

int initialize_waiting_entry(
    unsigned char *table,
    unsigned char *entry,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mode,
    bool bypass_queued_waiters,
    std::uint32_t deadlock_weight,
    std::uint64_t hog_lock_count,
    std::uint64_t max_write_lock_count,
    std::uint64_t *out_sequence
) {
    if (out_sequence == nullptr ||
        load64(table, k_header_waiting_count_offset) == std::numeric_limits<std::uint64_t>::max()) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    const std::uint64_t sequence = advance_generation(table);
    if (sequence == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    store64(entry, k_entry_key_hash_offset, key_hash);
    store32(entry, k_entry_owner_id_offset, owner_id);
    store32(entry, k_entry_mode_offset, mode);
    store32(entry, k_entry_reference_count_offset, 0U);
    store64(entry, k_entry_owner_generation_offset, owner_generation);
    store64(entry, k_entry_session_id_offset, session_id);
    store64(
        entry,
        k_entry_flags_offset,
        (bypass_queued_waiters ? k_entry_flag_bypass_queued_waiters : 0U) |
            (static_cast<std::uint64_t>(deadlock_weight) << k_entry_deadlock_weight_shift)
    );
    store64(entry, k_entry_hog_lock_count_offset, hog_lock_count);
    store64(entry, k_entry_max_write_lock_count_offset, max_write_lock_count);
    store64(entry, k_entry_generation_offset, sequence);
    store64(
        table,
        k_header_waiting_count_offset,
        load64(table, k_header_waiting_count_offset) + 1U
    );
    store32(entry, k_entry_state_offset, k_entry_state_waiting);
    *out_sequence = sequence;
    return MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

int grant_waiting_entry(unsigned char *table, std::size_t mapping_size, unsigned char *entry) {
    if (load32(entry, k_entry_state_offset) != k_entry_state_waiting ||
        load64(table, k_header_waiting_count_offset) == 0U ||
        load64(table, k_header_active_count_offset) == std::numeric_limits<std::uint64_t>::max()) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    const std::uint64_t generation = advance_generation(table);
    if (generation == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    store32(entry, k_entry_reference_count_offset, 1U);
    store64(
        table,
        k_header_waiting_count_offset,
        load64(table, k_header_waiting_count_offset) - 1U
    );
    store64(table, k_header_active_count_offset, load64(table, k_header_active_count_offset) + 1U);
    store32(entry, k_entry_state_offset, k_entry_state_active);
    update_hog_lock_count_after_grant(
        table,
        mapping_size,
        load64(entry, k_entry_key_hash_offset),
        load32(entry, k_entry_mode_offset)
    );
    store64(entry, k_entry_hog_lock_count_offset, 0U);
    store64(entry, k_entry_max_write_lock_count_offset, 0U);
    wake_table_waiters(table);
    return MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

int clear_lock_entry(unsigned char *table, unsigned char *entry) {
    const std::uint32_t state = load32(entry, k_entry_state_offset);
    const std::uint64_t key_hash = load64(entry, k_entry_key_hash_offset);
    if (state != k_entry_state_active && state != k_entry_state_waiting) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    if ((state == k_entry_state_active && load64(table, k_header_active_count_offset) == 0U) ||
        (state == k_entry_state_waiting && load64(table, k_header_waiting_count_offset) == 0U)) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    const std::uint64_t generation = load64(table, k_header_generation_offset);
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    store32(entry, k_entry_state_offset, k_entry_state_free);
    store32(entry, k_entry_owner_id_offset, 0U);
    store32(entry, k_entry_mode_offset, 0U);
    store32(entry, k_entry_reference_count_offset, 0U);
    store64(entry, k_entry_key_hash_offset, 0U);
    store64(entry, k_entry_owner_generation_offset, 0U);
    store64(entry, k_entry_session_id_offset, 0U);
    store64(entry, k_entry_flags_offset, 0U);
    store64(entry, k_entry_hog_lock_count_offset, 0U);
    store64(entry, k_entry_max_write_lock_count_offset, 0U);
    store64(entry, k_entry_generation_offset, 0U);
    store64(table, k_header_generation_offset, generation + 1U);
    if (state == k_entry_state_active) {
        store64(
            table,
            k_header_active_count_offset,
            load64(table, k_header_active_count_offset) - 1U
        );
    } else {
        store64(
            table,
            k_header_waiting_count_offset,
            load64(table, k_header_waiting_count_offset) - 1U
        );
    }
    normalize_hog_lock_count_for_key(
        table,
        mylite_ownerless_lock_table_size(entry_count(table)),
        key_hash
    );
    wake_table_waiters(table);
    return MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

CycleCheckResult wait_cycle_exists(
    unsigned char *table,
    std::size_t mapping_size,
    OwnerIdentity start,
    unsigned char *start_waiting_entry
) {
    const std::uint32_t count = entry_count(table);
    std::unique_ptr<WaitGraphNode[]> nodes(new (std::nothrow) WaitGraphNode[count]);
    if (nodes == nullptr || start_waiting_entry == nullptr ||
        load32(start_waiting_entry, k_entry_state_offset) != k_entry_state_waiting) {
        return CycleCheckResult::error;
    }
    nodes[0].owner = start;
    std::size_t node_count = 1U;

    for (std::size_t node_index = 0U; node_index < node_count; ++node_index) {
        for (std::uint32_t waiter_index = 0; waiter_index < count; ++waiter_index) {
            unsigned char *waiting_entry = entry_at(table, waiter_index);
            if (static_cast<std::size_t>(
                    waiting_entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table
                ) > mapping_size ||
                load32(waiting_entry, k_entry_state_offset) != k_entry_state_waiting ||
                entry_is_deadlock_victim(waiting_entry) ||
                !owner_identity_equal(entry_owner(waiting_entry), nodes[node_index].owner)) {
                continue;
            }

            for (std::uint32_t candidate_index = 0; candidate_index < count; ++candidate_index) {
                unsigned char *candidate = entry_at(table, candidate_index);
                if (static_cast<std::size_t>(
                        candidate + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table
                    ) > mapping_size ||
                    !waiting_entry_is_blocked_by(table, mapping_size, waiting_entry, candidate)) {
                    continue;
                }

                const OwnerIdentity candidate_owner = entry_owner(candidate);
                if (owner_identity_equal(candidate_owner, start)) {
                    unsigned char *victim = start_waiting_entry;
                    consider_deadlock_victim(waiting_entry, &victim);
                    std::size_t path_index = node_index;
                    while (path_index != 0U) {
                        consider_deadlock_victim(nodes[path_index].incoming_waiting_entry, &victim);
                        path_index = nodes[path_index].parent_index;
                    }
                    consider_deadlock_victim(start_waiting_entry, &victim);
                    store64(
                        victim,
                        k_entry_flags_offset,
                        load64(victim, k_entry_flags_offset) | k_entry_flag_deadlock_victim
                    );
                    wake_table_waiters(table);
                    return victim == start_waiting_entry ? CycleCheckResult::victim_is_requester
                                                         : CycleCheckResult::victim_is_other;
                }
                bool already_visited = false;
                for (std::size_t visited_index = 0; visited_index < node_count; ++visited_index) {
                    if (owner_identity_equal(nodes[visited_index].owner, candidate_owner)) {
                        already_visited = true;
                        break;
                    }
                }
                if (!already_visited) {
                    if (node_count == count) {
                        return CycleCheckResult::error;
                    }
                    nodes[node_count].owner = candidate_owner;
                    nodes[node_count].parent_index = node_index;
                    nodes[node_count].incoming_waiting_entry = waiting_entry;
                    ++node_count;
                }
            }
        }
    }
    return CycleCheckResult::none;
}

bool waiting_entry_is_blocked_by(
    unsigned char *table,
    std::size_t mapping_size,
    const unsigned char *waiting_entry,
    const unsigned char *candidate
) {
    const std::uint32_t candidate_state = load32(candidate, k_entry_state_offset);
    if ((candidate_state != k_entry_state_active && candidate_state != k_entry_state_waiting) ||
        entry_is_deadlock_victim(waiting_entry) ||
        (candidate_state == k_entry_state_waiting && entry_is_deadlock_victim(candidate)) ||
        owner_identity_equal(entry_owner(candidate), entry_owner(waiting_entry))) {
        return false;
    }
    if (load64(candidate, k_entry_key_hash_offset) !=
        load64(waiting_entry, k_entry_key_hash_offset)) {
        return false;
    }
    const std::uint32_t requested_mode = load32(waiting_entry, k_entry_mode_offset);
    const std::uint32_t candidate_mode = load32(candidate, k_entry_mode_offset);
    if (candidate_state == k_entry_state_active) {
        return lock_modes_conflict(requested_mode, candidate_mode);
    }
    if ((load64(waiting_entry, k_entry_flags_offset) & k_entry_flag_bypass_queued_waiters) != 0U) {
        return false;
    }
    const std::uint64_t max_write_lock_count =
        load64(waiting_entry, k_entry_max_write_lock_count_offset);
    const bool starvation_breaker_active =
        max_write_lock_count != std::numeric_limits<std::uint64_t>::max() &&
        load64(waiting_entry, k_entry_hog_lock_count_offset) >= max_write_lock_count &&
        key_has_low_priority_waiter(
            table,
            mapping_size,
            load64(waiting_entry, k_entry_key_hash_offset)
        );
    bool priority_conflict = waiting_mode_blocks_request(requested_mode, candidate_mode);
    if (starvation_breaker_active) {
        priority_conflict = mode_is_hog_lock(requested_mode)
                                ? !mode_is_hog_lock(candidate_mode)
                                : (!mode_is_hog_lock(candidate_mode) &&
                                   waiting_mode_blocks_request(requested_mode, candidate_mode));
    }
    return priority_conflict ||
           (waiting_modes_have_equal_priority(requested_mode, candidate_mode) &&
            lock_modes_conflict(requested_mode, candidate_mode) &&
            load64(candidate, k_entry_generation_offset) <
                load64(waiting_entry, k_entry_generation_offset));
}

bool owner_identity_equal(OwnerIdentity left, OwnerIdentity right) {
    return left.id == right.id && left.generation == right.generation &&
           left.session_id == right.session_id;
}

OwnerIdentity entry_owner(const unsigned char *entry) {
    return OwnerIdentity{
        load32(entry, k_entry_owner_id_offset),
        load64(entry, k_entry_owner_generation_offset),
        load64(entry, k_entry_session_id_offset),
    };
}

bool entry_is_valid(const unsigned char *entry) {
    const std::uint32_t state = load32(entry, k_entry_state_offset);
    const std::uint32_t reference_count = load32(entry, k_entry_reference_count_offset);
    return (state == k_entry_state_active || state == k_entry_state_waiting) &&
           load64(entry, k_entry_key_hash_offset) != 0U &&
           load32(entry, k_entry_owner_id_offset) != 0U &&
           load64(entry, k_entry_owner_generation_offset) != 0U &&
           load64(entry, k_entry_session_id_offset) != 0U &&
           (load64(entry, k_entry_flags_offset) & ~k_entry_known_flags) == 0U &&
           lock_mode_is_valid(load32(entry, k_entry_mode_offset)) &&
           load64(entry, k_entry_generation_offset) != 0U &&
           ((state == k_entry_state_active && reference_count == 1U &&
             load64(entry, k_entry_max_write_lock_count_offset) == 0U) ||
            (state == k_entry_state_waiting && reference_count == 0U &&
             load64(entry, k_entry_max_write_lock_count_offset) != 0U));
}

bool repair_lock_table_locked(unsigned char *table, std::size_t mapping_size) {
    std::uint64_t generation = load64(table, k_header_generation_offset);
    std::uint64_t active_count = 0U;
    std::uint64_t waiting_count = 0U;
    const std::uint32_t count = entry_count(table);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            return false;
        }
        const std::uint32_t state = load32(entry, k_entry_state_offset);
        if (state == k_entry_state_free) {
            continue;
        }
        const std::uint64_t entry_generation = load64(entry, k_entry_generation_offset);
        const std::uint32_t reference_count = load32(entry, k_entry_reference_count_offset);
        if ((state != k_entry_state_active && state != k_entry_state_waiting) ||
            load64(entry, k_entry_key_hash_offset) == 0U ||
            load32(entry, k_entry_owner_id_offset) == 0U ||
            load64(entry, k_entry_owner_generation_offset) == 0U ||
            load64(entry, k_entry_session_id_offset) == 0U ||
            (load64(entry, k_entry_flags_offset) & ~k_entry_known_flags) != 0U ||
            !lock_mode_is_valid(load32(entry, k_entry_mode_offset)) || entry_generation == 0U ||
            (state == k_entry_state_active &&
             (reference_count != 1U || load64(entry, k_entry_max_write_lock_count_offset) != 0U)) ||
            (state == k_entry_state_waiting &&
             load64(entry, k_entry_max_write_lock_count_offset) == 0U)) {
            return false;
        }
        for (std::uint32_t prior_index = 0; prior_index < index; ++prior_index) {
            unsigned char *prior = entry_at(table, prior_index);
            const std::uint32_t prior_state = load32(prior, k_entry_state_offset);
            if (prior_state == k_entry_state_free) {
                continue;
            }
            if (load64(prior, k_entry_generation_offset) == entry_generation) {
                return false;
            }
        }
        generation = std::max(generation, entry_generation);
        if (state == k_entry_state_active) {
            ++active_count;
        } else {
            ++waiting_count;
        }
    }
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        const std::uint32_t state = load32(entry, k_entry_state_offset);
        if (state == k_entry_state_free) {
            store64(entry, k_entry_key_hash_offset, 0U);
            store32(entry, k_entry_owner_id_offset, 0U);
            store32(entry, k_entry_mode_offset, 0U);
            store64(entry, k_entry_generation_offset, 0U);
            store32(entry, k_entry_reference_count_offset, 0U);
            store64(entry, k_entry_owner_generation_offset, 0U);
            store64(entry, k_entry_session_id_offset, 0U);
            store64(entry, k_entry_flags_offset, 0U);
            store64(entry, k_entry_hog_lock_count_offset, 0U);
            store64(entry, k_entry_max_write_lock_count_offset, 0U);
        } else if (state == k_entry_state_waiting) {
            /* A crashed waiting-to-active conversion remains a waiter. */
            store32(entry, k_entry_reference_count_offset, 0U);
        } else {
            store64(entry, k_entry_hog_lock_count_offset, 0U);
            store64(entry, k_entry_max_write_lock_count_offset, 0U);
        }
    }
    store64(table, k_header_generation_offset, generation + 1U);
    store64(table, k_header_active_count_offset, active_count);
    store64(table, k_header_waiting_count_offset, waiting_count);
    wake_table_waiters(table);
    return true;
}

bool lock_mode_is_valid(std::uint32_t mode) {
    return mode >= MYLITE_OWNERLESS_LOCK_TABLE_SHARED &&
           mode <= MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE;
}

bool lock_modes_conflict(std::uint32_t requested_mode, std::uint32_t active_mode) {
    if (requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE ||
        active_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE) {
        return true;
    }
    if (requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE ||
        active_mode == MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE) {
        return requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED ||
               active_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED;
    }
    if (requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE ||
        active_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE) {
        return requested_mode != MYLITE_OWNERLESS_LOCK_TABLE_SHARED &&
               active_mode != MYLITE_OWNERLESS_LOCK_TABLE_SHARED;
    }
    if (requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE ||
        active_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE) {
        const std::uint32_t other_mode =
            requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE ? active_mode
                                                                          : requested_mode;
        return other_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE ||
               other_mode == MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE ||
               other_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE;
    }
    if (requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY ||
        active_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY) {
        return requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE ||
               active_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE;
    }
    return requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE &&
           active_mode == MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE;
}

bool waiting_mode_blocks_request(std::uint32_t requested_mode, std::uint32_t waiting_mode) {
    /* Mirrors MDL_object_lock/MDL_scoped_lock::m_waiting_incompatible. */
    switch (requested_mode) {
    case MYLITE_OWNERLESS_LOCK_TABLE_SHARED:
        return waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE;
    case MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ:
        return waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE ||
               waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE;
    case MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE:
        return waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE ||
               waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE ||
               waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE;
    case MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE:
    case MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE:
    case MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE:
        return waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE;
    case MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY:
        return waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE ||
               waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE ||
               waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE;
    case MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE:
        return waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED ||
               waiting_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE;
    case MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE:
        return false;
    default:
        return true;
    }
}

bool waiting_modes_have_equal_priority(std::uint32_t left_mode, std::uint32_t right_mode) {
    return !waiting_mode_blocks_request(left_mode, right_mode) &&
           !waiting_mode_blocks_request(right_mode, left_mode);
}

bool mode_is_hog_lock(std::uint32_t mode) {
    return mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE ||
           mode == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE ||
           mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE;
}

std::uint64_t waiting_hog_lock_count_for_key(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash
) {
    std::uint64_t hog_lock_count = 0U;
    const std::uint32_t count = entry_count(table);
    for (std::uint32_t index = 0U; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            break;
        }
        if (load32(entry, k_entry_state_offset) == k_entry_state_waiting &&
            !entry_is_deadlock_victim(entry) &&
            load64(entry, k_entry_key_hash_offset) == key_hash) {
            hog_lock_count = std::max(hog_lock_count, load64(entry, k_entry_hog_lock_count_offset));
        }
    }
    return hog_lock_count;
}

bool key_has_low_priority_waiter(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash
) {
    const std::uint32_t count = entry_count(table);
    for (std::uint32_t index = 0U; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            break;
        }
        if (load32(entry, k_entry_state_offset) == k_entry_state_waiting &&
            !entry_is_deadlock_victim(entry) &&
            load64(entry, k_entry_key_hash_offset) == key_hash &&
            !mode_is_hog_lock(load32(entry, k_entry_mode_offset))) {
            return true;
        }
    }
    return false;
}

void update_hog_lock_count_after_grant(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t granted_mode
) {
    const bool low_priority_waiter_remains =
        key_has_low_priority_waiter(table, mapping_size, key_hash);
    std::uint64_t next_hog_lock_count = 0U;
    if (mode_is_hog_lock(granted_mode) && low_priority_waiter_remains) {
        const std::uint64_t current = waiting_hog_lock_count_for_key(table, mapping_size, key_hash);
        next_hog_lock_count =
            current == std::numeric_limits<std::uint64_t>::max() ? current : current + 1U;
    }

    const std::uint32_t count = entry_count(table);
    for (std::uint32_t index = 0U; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            break;
        }
        if (load32(entry, k_entry_state_offset) == k_entry_state_waiting &&
            load64(entry, k_entry_key_hash_offset) == key_hash) {
            store64(entry, k_entry_hog_lock_count_offset, next_hog_lock_count);
        }
    }
}

void normalize_hog_lock_count_for_key(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash
) {
    if (key_hash == 0U || key_has_low_priority_waiter(table, mapping_size, key_hash)) {
        return;
    }
    const std::uint32_t count = entry_count(table);
    for (std::uint32_t index = 0U; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            break;
        }
        if (load32(entry, k_entry_state_offset) == k_entry_state_waiting &&
            load64(entry, k_entry_key_hash_offset) == key_hash) {
            store64(entry, k_entry_hog_lock_count_offset, 0U);
        }
    }
}

bool entry_is_deadlock_victim(const unsigned char *entry) {
    return (load64(entry, k_entry_flags_offset) & k_entry_flag_deadlock_victim) != 0U;
}

std::uint32_t entry_deadlock_weight(const unsigned char *entry) {
    return static_cast<std::uint32_t>(
        (load64(entry, k_entry_flags_offset) & k_entry_deadlock_weight_mask) >>
        k_entry_deadlock_weight_shift
    );
}

void consider_deadlock_victim(unsigned char *entry, unsigned char **victim) {
    if (entry != nullptr && victim != nullptr && *victim != nullptr &&
        entry_deadlock_weight(entry) < entry_deadlock_weight(*victim)) {
        *victim = entry;
    }
}

bool cancellation_requested(
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context
) {
    return is_cancelled != nullptr && is_cancelled(cancel_context) != 0;
}

std::uint64_t advance_generation(unsigned char *table) {
    const std::uint64_t generation = load64(table, k_header_generation_offset);
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        return 0U;
    }
    store64(table, k_header_generation_offset, generation + 1U);
    return generation + 1U;
}

void wake_table_waiters(unsigned char *table) {
    mylite_ownerless_wait_word *wait_word = table_wait_word(table);
    mylite_ownerless_wait_store(wait_word, mylite_ownerless_wait_load(wait_word) + 1U);
    static_cast<void>(mylite_ownerless_wait_wake(wait_word));
}

unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto bounded = std::min<std::chrono::milliseconds::rep>(
        std::max<std::chrono::milliseconds::rep>(remaining.count(), 1),
        static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<unsigned>::max())
    );
    return static_cast<unsigned>(bounded);
}

std::chrono::steady_clock::time_point bounded_latch_deadline(
    std::chrono::steady_clock::time_point operation_deadline
) {
    return std::min(
        operation_deadline,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(k_lock_table_latch_timeout_ms)
    );
}

bool lock_table_size_fits(std::uint32_t entry_count) {
    const std::size_t max_entries =
        (std::numeric_limits<std::size_t>::max() - MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE) /
        MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE;
    return static_cast<std::size_t>(entry_count) <= max_entries;
}

bool mapping_can_hold_table(const void *mapping, std::size_t mapping_size) {
    if (mapping == nullptr || mapping_size < MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE) {
        return false;
    }
    const auto *table = static_cast<const unsigned char *>(mapping);
    const std::uint32_t count = entry_count(table);
    const std::size_t table_size = mylite_ownerless_lock_table_size(count);
    return count > 0U &&
           load32(table, k_header_entry_size_offset) == MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE &&
           table_size > 0U && mapping_size >= table_size;
}

std::uint32_t entry_count(const unsigned char *table) {
    return load32(table, k_header_entry_count_offset);
}

unsigned char *entry_at(unsigned char *table, std::uint32_t index) {
    return table + MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE +
           (static_cast<std::size_t>(index) * MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE);
}

mylite_ownerless_latch *table_latch(unsigned char *table) {
    return reinterpret_cast<mylite_ownerless_latch *>(table + k_header_latch_offset);
}

mylite_ownerless_wait_word *table_wait_word(unsigned char *table) {
    return reinterpret_cast<mylite_ownerless_wait_word *>(table + k_header_wait_word_offset);
}

std::uint32_t load32(const unsigned char *base, std::size_t offset) {
    const auto *value = reinterpret_cast<const std::uint32_t *>(base + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

std::uint64_t load64(const unsigned char *base, std::size_t offset) {
    const auto *value = reinterpret_cast<const std::uint64_t *>(base + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

void store32(unsigned char *base, std::size_t offset, std::uint32_t value) {
    auto *target = reinterpret_cast<std::uint32_t *>(base + offset);
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

void store64(unsigned char *base, std::size_t offset, std::uint64_t value) {
    auto *target = reinterpret_cast<std::uint64_t *>(base + offset);
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

} // namespace

#include "ownerless_autoinc_registry.h"

#include "ownerless_latch.h"
#include "ownerless_process_registry.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t k_header_slot_count_offset = 0;
constexpr std::size_t k_header_slot_size_offset = 4;
constexpr std::size_t k_header_checkpoint_pending_offset = 8;
/*
 * These fields consume bytes that were reserved and zero in the original
 * 64-byte header/32-byte slot ABI. Generation zero is a legacy entry and is
 * upgraded while latched; segment sizes and the .shm format stay compatible.
 */
constexpr std::size_t k_header_entry_generation_offset = 16;
constexpr std::size_t k_header_mutation_marker_offset = 24;
constexpr std::size_t k_header_latch_offset = 32;
constexpr std::size_t k_slot_table_id_offset = 0;
constexpr std::size_t k_slot_next_value_offset = 8;
constexpr std::size_t k_slot_state_offset = 16;
constexpr std::size_t k_slot_entry_generation_offset = 20;
constexpr std::size_t k_slot_persistent_value_offset = 24;
constexpr std::uint32_t k_slot_state_free = 0;
constexpr std::uint32_t k_slot_state_active = 1;
constexpr std::uint64_t k_mutation_remove_flag = 1ULL << 63U;
constexpr std::uint64_t k_mutation_slot_mask = 0x7fffffffULL;
constexpr unsigned k_registry_latch_timeout_ms = 5000U;

bool registry_size_fits(std::uint32_t slot_count);
bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size);
std::uint32_t slot_count(const unsigned char *registry);
unsigned char *slot_at(unsigned char *registry, std::uint32_t index);
mylite_ownerless_latch *registry_latch(unsigned char *registry);
int acquire_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
);
int finish_registry_operation(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
);
bool repair_registry_locked(unsigned char *registry, std::size_t mapping_size);
bool next_entry_generation(unsigned char *registry, std::uint32_t *out_generation);
bool ensure_slot_generation(unsigned char *registry, unsigned char *slot);
bool begin_slot_mutation(
    unsigned char *registry,
    const unsigned char *slot,
    std::uint32_t entry_generation,
    bool remove
);
void finish_slot_mutation(unsigned char *registry);
bool repair_slot_mutation(unsigned char *registry, std::size_t mapping_size);
unsigned char *find_slot(unsigned char *registry, std::size_t mapping_size, std::uint64_t table_id);
unsigned char *find_retired_slot(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t table_id
);
unsigned char *find_free_slot(unsigned char *registry, std::size_t mapping_size);
std::uint32_t load32(const unsigned char *base, std::size_t offset);
std::uint64_t load64(const unsigned char *base, std::size_t offset);
void store32(unsigned char *base, std::size_t offset, std::uint32_t value);
void store64(unsigned char *base, std::size_t offset, std::uint64_t value);

} // namespace

std::size_t mylite_ownerless_autoinc_registry_size(std::uint32_t slot_count) {
    return MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(slot_count) * MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE);
}

int mylite_ownerless_autoinc_registry_initialize(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_count
) {
    if (mapping == nullptr || !registry_size_fits(slot_count) ||
        mapping_size < mylite_ownerless_autoinc_registry_size(slot_count)) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    std::memset(registry, 0, mylite_ownerless_autoinc_registry_size(slot_count));
    store32(registry, k_header_slot_count_offset, slot_count);
    store32(registry, k_header_slot_size_offset, MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE);
    mylite_ownerless_latch_initialize(registry_latch(registry));
    return MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK;
}

int mylite_ownerless_autoinc_registry_read_or_seed(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t table_id,
    std::uint64_t seed_next_value,
    std::uint64_t *out_next_value
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || table_id == 0U || seed_next_value == 0U ||
        out_next_value == nullptr) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return latch_result;
    }

    unsigned char *slot = find_slot(registry, mapping_size, table_id);
    if (slot == nullptr) {
        if (find_retired_slot(registry, mapping_size, table_id) != nullptr) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_STALE,
                false
            );
        }
        slot = find_free_slot(registry, mapping_size);
        if (slot == nullptr) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL,
                false
            );
        }
        std::uint32_t entry_generation = 0U;
        if (!next_entry_generation(registry, &entry_generation)) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
                false
            );
        }
        if (!begin_slot_mutation(registry, slot, entry_generation, false)) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
                true
            );
        }
        store32(slot, k_slot_entry_generation_offset, entry_generation);
        store64(slot, k_slot_table_id_offset, table_id);
        store64(slot, k_slot_next_value_offset, seed_next_value);
        store64(slot, k_slot_persistent_value_offset, 0U);
        store32(slot, k_slot_state_offset, k_slot_state_active);
        finish_slot_mutation(registry);
        *out_next_value = seed_next_value;
    } else {
        if (!ensure_slot_generation(registry, slot)) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
                false
            );
        }
        const std::uint64_t stored = load64(slot, k_slot_next_value_offset);
        const std::uint64_t next_value = std::max(stored, seed_next_value);
        store64(slot, k_slot_next_value_offset, next_value);
        *out_next_value = next_value;
    }

    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK,
        true
    );
}

int mylite_ownerless_autoinc_registry_publish(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t table_id,
    std::uint64_t next_value,
    std::uint64_t persistent_value
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || table_id == 0U || next_value == 0U) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return latch_result;
    }

    unsigned char *slot = find_slot(registry, mapping_size, table_id);
    bool advanced = false;
    if (slot == nullptr) {
        if (find_retired_slot(registry, mapping_size, table_id) != nullptr) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_STALE,
                false
            );
        }
        slot = find_free_slot(registry, mapping_size);
        if (slot == nullptr) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL,
                false
            );
        }
        std::uint32_t entry_generation = 0U;
        if (!next_entry_generation(registry, &entry_generation)) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
                false
            );
        }
        if (!begin_slot_mutation(registry, slot, entry_generation, false)) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
                true
            );
        }
        store32(slot, k_slot_entry_generation_offset, entry_generation);
        store64(slot, k_slot_table_id_offset, table_id);
        store64(slot, k_slot_next_value_offset, next_value);
        store64(slot, k_slot_persistent_value_offset, persistent_value);
        store64(registry, k_header_checkpoint_pending_offset, 1U);
        store32(slot, k_slot_state_offset, k_slot_state_active);
        finish_slot_mutation(registry);
        advanced = true;
    } else {
        if (!ensure_slot_generation(registry, slot)) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
                false
            );
        }
        if (next_value > load64(slot, k_slot_next_value_offset)) {
            store64(slot, k_slot_next_value_offset, next_value);
            advanced = true;
        }
        if (persistent_value > load64(slot, k_slot_persistent_value_offset)) {
            store64(slot, k_slot_persistent_value_offset, persistent_value);
            advanced = true;
        }
    }
    if (advanced) {
        store64(registry, k_header_checkpoint_pending_offset, 1U);
    }

    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK,
        advanced
    );
}

int mylite_ownerless_autoinc_registry_entry_generation(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t table_id,
    std::uint64_t *out_entry_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || table_id == 0U || out_entry_generation == nullptr) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }
    *out_entry_generation = 0U;

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return latch_result;
    }
    unsigned char *slot = find_slot(registry, mapping_size, table_id);
    if (slot == nullptr || !ensure_slot_generation(registry, slot)) {
        return finish_registry_operation(
            registry,
            owner_id,
            owner_generation,
            slot == nullptr ? MYLITE_OWNERLESS_AUTOINC_REGISTRY_STALE
                            : MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
            slot != nullptr
        );
    }
    *out_entry_generation = load32(slot, k_slot_entry_generation_offset);
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK,
        true
    );
}

int mylite_ownerless_autoinc_registry_remove(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t table_id,
    std::uint64_t entry_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || table_id == 0U || entry_generation == 0U ||
        entry_generation > std::numeric_limits<std::uint32_t>::max()) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return latch_result;
    }
    unsigned char *slot = find_slot(registry, mapping_size, table_id);
    if (slot == nullptr) {
        slot = find_retired_slot(registry, mapping_size, table_id);
        if (slot == nullptr) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_STALE,
                false
            );
        }
    }
    if (load32(slot, k_slot_entry_generation_offset) != entry_generation) {
        return finish_registry_operation(
            registry,
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_AUTOINC_REGISTRY_STALE,
            false
        );
    }
    if (load32(slot, k_slot_state_offset) == k_slot_state_free) {
        return finish_registry_operation(
            registry,
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK,
            false
        );
    }
    if (!begin_slot_mutation(registry, slot, static_cast<std::uint32_t>(entry_generation), true)) {
        return finish_registry_operation(
            registry,
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
            false
        );
    }
    /* A crash after this point must not hide the need to persist reclamation. */
    store64(registry, k_header_checkpoint_pending_offset, 1U);
    /* State is cleared first; the retained ID/generation is the idempotence tombstone. */
    store32(slot, k_slot_state_offset, k_slot_state_free);
    store64(slot, k_slot_next_value_offset, 0U);
    store64(slot, k_slot_persistent_value_offset, 0U);
    finish_slot_mutation(registry);
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK,
        true
    );
}

int mylite_ownerless_autoinc_registry_checkpoint_pending(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int *out_pending
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || out_pending == nullptr) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return latch_result;
    }

    *out_pending = load64(registry, k_header_checkpoint_pending_offset) != 0U ? 1 : 0;
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_autoinc_registry_clear_checkpoint_pending(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return latch_result;
    }

    store64(registry, k_header_checkpoint_pending_offset, 0U);
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK,
        true
    );
}

int mylite_ownerless_autoinc_registry_snapshot(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    mylite_ownerless_autoinc_registry_entry *entries,
    std::size_t entry_capacity,
    std::size_t *out_entry_count
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || out_entry_count == nullptr ||
        (entry_capacity > 0U && entries == nullptr)) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return latch_result;
    }

    std::size_t entry_count = 0;
    bool truncated = false;
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
                false
            );
        }
        if (load32(slot, k_slot_state_offset) != k_slot_state_active) {
            continue;
        }
        if (!ensure_slot_generation(registry, slot)) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR,
                true
            );
        }

        const std::uint64_t table_id = load64(slot, k_slot_table_id_offset);
        const std::uint64_t next_value = load64(slot, k_slot_next_value_offset);
        const std::uint64_t persistent_value = load64(slot, k_slot_persistent_value_offset);
        if (table_id == 0U || next_value == 0U) {
            continue;
        }

        if (entry_count < entry_capacity) {
            entries[entry_count].table_id = table_id;
            entries[entry_count].next_value = next_value;
            entries[entry_count].persistent_value = persistent_value;
            entries[entry_count].entry_generation = load32(slot, k_slot_entry_generation_offset);
        } else {
            truncated = true;
        }
        ++entry_count;
    }

    *out_entry_count = entry_count;
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        truncated ? MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL : MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK,
        true
    );
}

int mylite_ownerless_autoinc_registry_finish_pending_release(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }
    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, owner_id, owner_generation);
    if (latch_result != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return latch_result;
    }
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_autoinc_registry_recover_dead_latch(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    const mylite_ownerless_process_registry_liveness_context *liveness
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || liveness == nullptr) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
    }
    auto *registry = static_cast<unsigned char *>(mapping);
    mylite_ownerless_latch_dead_owner dead_owner = {};
    const int latch_result = mylite_ownerless_latch_acquire_recoverable(
        registry_latch(registry),
        owner_id,
        owner_generation,
        mylite_ownerless_process_registry_latch_owner_is_alive,
        const_cast<mylite_ownerless_process_registry_liveness_context *>(liveness),
        k_registry_latch_timeout_ms,
        &dead_owner
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED) {
        if (!repair_registry_locked(registry, mapping_size)) {
            static_cast<void>(mylite_ownerless_latch_mark_not_recoverable(
                registry_latch(registry),
                owner_id,
                owner_generation
            ));
            return MYLITE_OWNERLESS_AUTOINC_REGISTRY_OWNER_DEAD;
        }
        if (mylite_ownerless_latch_mark_consistent(
                registry_latch(registry),
                owner_id,
                owner_generation
            ) != MYLITE_OWNERLESS_LATCH_OK) {
            return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
        }
    } else if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT
                   ? MYLITE_OWNERLESS_AUTOINC_REGISTRY_TIMEOUT
                   : MYLITE_OWNERLESS_AUTOINC_REGISTRY_OWNER_DEAD;
    }
    return mylite_ownerless_latch_release(registry_latch(registry), owner_id, owner_generation) ==
                   MYLITE_OWNERLESS_LATCH_OK
               ? MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
               : MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
}

namespace {

bool registry_size_fits(std::uint32_t count) {
    const std::size_t max_slots =
        (std::numeric_limits<std::size_t>::max() - MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE) /
        MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE;
    return count > 0U && count <= k_mutation_slot_mask &&
           static_cast<std::size_t>(count) <= max_slots;
}

bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size) {
    if (mapping == nullptr || mapping_size < MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE) {
        return false;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    const std::uint32_t count = load32(registry, k_header_slot_count_offset);
    return registry_size_fits(count) &&
           load32(registry, k_header_slot_size_offset) ==
               MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE &&
           mapping_size >= mylite_ownerless_autoinc_registry_size(count);
}

std::uint32_t slot_count(const unsigned char *registry) {
    return load32(registry, k_header_slot_count_offset);
}

unsigned char *slot_at(unsigned char *registry, std::uint32_t index) {
    return registry + MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(index) * MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE);
}

mylite_ownerless_latch *registry_latch(unsigned char *registry) {
    return reinterpret_cast<mylite_ownerless_latch *>(registry + k_header_latch_offset);
}

int acquire_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    const int latch_result = mylite_ownerless_latch_acquire(
        registry_latch(registry),
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        k_registry_latch_timeout_ms
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK;
    }
    if (latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT) {
        return MYLITE_OWNERLESS_AUTOINC_REGISTRY_TIMEOUT;
    }
    return latch_result == MYLITE_OWNERLESS_LATCH_OWNER_DEAD ||
                   latch_result == MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE
               ? MYLITE_OWNERLESS_AUTOINC_REGISTRY_OWNER_DEAD
               : MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
}

bool repair_registry_locked(unsigned char *registry, std::size_t mapping_size) {
    if (!repair_slot_mutation(registry, mapping_size)) {
        return false;
    }
    const std::uint64_t stored_generation = load64(registry, k_header_entry_generation_offset);
    if (stored_generation > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::uint32_t maximum_generation = static_cast<std::uint32_t>(stored_generation);
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            return false;
        }
        const std::uint32_t state = load32(slot, k_slot_state_offset);
        if (state != k_slot_state_free && state != k_slot_state_active) {
            return false;
        }
        if (state == k_slot_state_free) {
            const std::uint64_t table_id = load64(slot, k_slot_table_id_offset);
            const std::uint64_t next_value = load64(slot, k_slot_next_value_offset);
            const std::uint64_t persistent_value = load64(slot, k_slot_persistent_value_offset);
            const std::uint32_t generation = load32(slot, k_slot_entry_generation_offset);
            const bool unused =
                table_id == 0U && next_value == 0U && persistent_value == 0U && generation == 0U;
            const bool tombstone =
                table_id != 0U && next_value == 0U && persistent_value == 0U && generation != 0U;
            if (!unused && !tombstone) {
                return false;
            }
            maximum_generation = std::max(maximum_generation, generation);
            continue;
        }
        if (load64(slot, k_slot_table_id_offset) == 0U ||
            load64(slot, k_slot_next_value_offset) == 0U) {
            return false;
        }
        std::uint32_t generation = load32(slot, k_slot_entry_generation_offset);
        if (generation == 0U) {
            if (maximum_generation == std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            generation = ++maximum_generation;
            store32(slot, k_slot_entry_generation_offset, generation);
        } else {
            maximum_generation = std::max(maximum_generation, generation);
        }
        for (std::uint32_t prior = 0; prior < index; ++prior) {
            unsigned char *prior_slot = slot_at(registry, prior);
            if (load32(prior_slot, k_slot_state_offset) == k_slot_state_active &&
                load64(prior_slot, k_slot_table_id_offset) ==
                    load64(slot, k_slot_table_id_offset)) {
                return false;
            }
        }
    }
    store64(registry, k_header_entry_generation_offset, maximum_generation);
    /* A dead mutation may have advanced a value before publishing this bit. */
    store64(registry, k_header_checkpoint_pending_offset, 1U);
    finish_slot_mutation(registry);
    return true;
}

bool next_entry_generation(unsigned char *registry, std::uint32_t *out_generation) {
    const std::uint64_t current = load64(registry, k_header_entry_generation_offset);
    if (out_generation == nullptr || current >= std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *out_generation = static_cast<std::uint32_t>(current + 1U);
    /* Gaps are safe; publishing the allocator before slot state prevents reuse. */
    store64(registry, k_header_entry_generation_offset, *out_generation);
    return true;
}

bool ensure_slot_generation(unsigned char *registry, unsigned char *slot) {
    if (load32(slot, k_slot_entry_generation_offset) != 0U) {
        return true;
    }
    std::uint32_t generation = 0U;
    if (!next_entry_generation(registry, &generation)) {
        return false;
    }
    store32(slot, k_slot_entry_generation_offset, generation);
    return true;
}

bool begin_slot_mutation(
    unsigned char *registry,
    const unsigned char *slot,
    std::uint32_t entry_generation,
    bool remove
) {
    if (entry_generation == 0U || load64(registry, k_header_mutation_marker_offset) != 0U ||
        slot < registry) {
        return false;
    }
    const std::size_t byte_offset = static_cast<std::size_t>(slot - registry);
    if (byte_offset < MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE) {
        return false;
    }
    const std::size_t slot_offset = byte_offset - MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE;
    if (slot_offset % MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE != 0U) {
        return false;
    }
    const std::uint64_t index_plus_one =
        (slot_offset / MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE) + 1U;
    if (index_plus_one > k_mutation_slot_mask) {
        return false;
    }
    const std::uint64_t marker =
        (remove ? k_mutation_remove_flag : 0U) | (index_plus_one << 32U) | entry_generation;
    store64(registry, k_header_mutation_marker_offset, marker);
    return true;
}

void finish_slot_mutation(unsigned char *registry) {
    store64(registry, k_header_mutation_marker_offset, 0U);
}

bool repair_slot_mutation(unsigned char *registry, std::size_t mapping_size) {
    const std::uint64_t marker = load64(registry, k_header_mutation_marker_offset);
    if (marker == 0U) {
        return true;
    }
    const bool remove = (marker & k_mutation_remove_flag) != 0U;
    const std::uint64_t index_plus_one = (marker >> 32U) & k_mutation_slot_mask;
    const std::uint32_t entry_generation = static_cast<std::uint32_t>(marker);
    if (index_plus_one == 0U || index_plus_one > slot_count(registry) || entry_generation == 0U) {
        return false;
    }
    unsigned char *slot = slot_at(registry, static_cast<std::uint32_t>(index_plus_one - 1U));
    if (static_cast<std::size_t>(slot + MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE - registry) >
        mapping_size) {
        return false;
    }
    const std::uint32_t state = load32(slot, k_slot_state_offset);
    if (state != k_slot_state_free && state != k_slot_state_active) {
        return false;
    }
    if (!remove) {
        if (state == k_slot_state_free) {
            std::memset(slot, 0, MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE);
            return true;
        }
        return load32(slot, k_slot_entry_generation_offset) == entry_generation;
    }
    if (load64(slot, k_slot_table_id_offset) == 0U ||
        load32(slot, k_slot_entry_generation_offset) != entry_generation) {
        return false;
    }
    store64(registry, k_header_checkpoint_pending_offset, 1U);
    store32(slot, k_slot_state_offset, k_slot_state_free);
    store64(slot, k_slot_next_value_offset, 0U);
    store64(slot, k_slot_persistent_value_offset, 0U);
    return true;
}

int finish_registry_operation(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
) {
    const int release_result =
        mylite_ownerless_latch_release(registry_latch(registry), owner_id, owner_generation);
    if (release_result == MYLITE_OWNERLESS_LATCH_OK) {
        return operation_result;
    }
    if (!operation_applied && release_result == MYLITE_OWNERLESS_LATCH_RELEASE_PENDING &&
        mylite_ownerless_latch_acquire(
            registry_latch(registry),
            owner_id,
            owner_generation,
            nullptr,
            nullptr,
            k_registry_latch_timeout_ms
        ) == MYLITE_OWNERLESS_LATCH_OK &&
        mylite_ownerless_latch_release(registry_latch(registry), owner_id, owner_generation) ==
            MYLITE_OWNERLESS_LATCH_OK) {
        return operation_result;
    }
    return operation_applied ? MYLITE_OWNERLESS_AUTOINC_REGISTRY_APPLIED_RELEASE_PENDING
                             : MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
}

unsigned char *find_slot(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t table_id
) {
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            return nullptr;
        }
        if (load32(slot, k_slot_state_offset) == k_slot_state_active &&
            load64(slot, k_slot_table_id_offset) == table_id) {
            return slot;
        }
    }
    return nullptr;
}

unsigned char *find_free_slot(unsigned char *registry, std::size_t mapping_size) {
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            return nullptr;
        }
        if (load32(slot, k_slot_state_offset) == k_slot_state_free) {
            return slot;
        }
    }
    return nullptr;
}

unsigned char *find_retired_slot(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t table_id
) {
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            return nullptr;
        }
        if (load32(slot, k_slot_state_offset) == k_slot_state_free &&
            load64(slot, k_slot_table_id_offset) == table_id &&
            load32(slot, k_slot_entry_generation_offset) != 0U) {
            return slot;
        }
    }
    return nullptr;
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

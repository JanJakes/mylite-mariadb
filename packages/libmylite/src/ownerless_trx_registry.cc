#include "ownerless_trx_registry.h"

#include "ownerless_wait.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t k_header_slot_count_offset = 0;
constexpr std::size_t k_header_slot_size_offset = 4;
constexpr std::size_t k_header_generation_offset = 8;
constexpr std::size_t k_header_active_count_offset = 16;
constexpr std::size_t k_header_next_trx_id_offset = 24;
constexpr std::size_t k_header_latch_offset = 32;
constexpr std::size_t k_header_oldest_active_trx_id_offset = 40;
constexpr std::size_t k_slot_generation_offset = 0;
constexpr std::size_t k_slot_trx_id_offset = 8;
constexpr std::size_t k_slot_owner_id_offset = 16;
constexpr std::size_t k_slot_state_offset = 20;
constexpr std::size_t k_slot_trx_no_offset = 24;
constexpr std::uint32_t k_slot_state_free = 0;

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
int acquire_registry_latch(unsigned char *registry, std::chrono::steady_clock::time_point deadline);
void release_registry_latch(unsigned char *registry);
int begin_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t *out_trx_id,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
);
int allocate_id_locked(unsigned char *registry, std::uint64_t *out_trx_id);
int assign_no_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t trx_id,
    std::uint64_t trx_no
);
int assign_new_no_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t trx_id,
    std::uint64_t *out_trx_no
);
int end_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
);
int release_owner_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_transactions
);
int snapshot_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t *out_trx_ids,
    std::uint32_t trx_id_capacity,
    std::uint32_t *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_oldest_active_trx_id
);
int snapshot_read_view_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t *out_trx_ids,
    std::uint32_t trx_id_capacity,
    std::uint32_t *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_min_trx_no
);
void recompute_oldest_active_trx_id_locked(unsigned char *registry, std::size_t mapping_size);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
bool registry_size_fits(std::uint32_t slot_count);
bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size);
std::uint32_t slot_count(const unsigned char *registry);
unsigned char *slot_at(unsigned char *registry, std::uint32_t index);
mylite_ownerless_wait_word *registry_latch(unsigned char *registry);
std::uint32_t load32(const unsigned char *base, std::size_t offset);
std::uint64_t load64(const unsigned char *base, std::size_t offset);
void store32(unsigned char *base, std::size_t offset, std::uint32_t value);
void store64(unsigned char *base, std::size_t offset, std::uint64_t value);
bool cas_wait_word(mylite_ownerless_wait_word *word, std::uint32_t *expected, std::uint32_t value);

} // namespace

std::size_t mylite_ownerless_trx_registry_size(std::uint32_t slot_count) {
    if (!registry_size_fits(slot_count)) {
        return 0U;
    }
    return MYLITE_OWNERLESS_TRX_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(slot_count) * MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE);
}

int mylite_ownerless_trx_registry_initialize(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_count,
    std::uint64_t next_trx_id
) {
    const std::size_t registry_size = mylite_ownerless_trx_registry_size(slot_count);
    if (mapping == nullptr || registry_size == 0U || mapping_size < registry_size ||
        slot_count == 0U || next_trx_id == 0U) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    std::memset(registry, 0, registry_size);
    store32(registry, k_header_slot_count_offset, slot_count);
    store32(registry, k_header_slot_size_offset, MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE);
    store64(registry, k_header_next_trx_id_offset, next_trx_id);
    return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
}

int mylite_ownerless_trx_registry_begin(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t *out_trx_id,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        out_trx_id == nullptr || out_slot_index == nullptr ||
        out_slot_generation == nullptr) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return latch_result;
    }
    const int begin_result = begin_locked(
        registry,
        mapping_size,
        owner_id,
        out_trx_id,
        out_slot_index,
        out_slot_generation
    );
    release_registry_latch(registry);
    return begin_result;
}

int mylite_ownerless_trx_registry_allocate_id(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t *out_trx_id
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || out_trx_id == nullptr) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return latch_result;
    }
    const int allocate_result = allocate_id_locked(registry, out_trx_id);
    release_registry_latch(registry);
    return allocate_result;
}

int mylite_ownerless_trx_registry_assign_no(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t trx_id,
    std::uint64_t trx_no
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || trx_id == 0U || trx_no == 0U ||
        trx_no == MYLITE_OWNERLESS_TRX_REGISTRY_UNASSIGNED_NO) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return latch_result;
    }
    const int assign_result = assign_no_locked(registry, mapping_size, trx_id, trx_no);
    release_registry_latch(registry);
    return assign_result;
}

int mylite_ownerless_trx_registry_assign_new_no(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t trx_id,
    std::uint64_t *out_trx_no
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || trx_id == 0U ||
        out_trx_no == nullptr) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return latch_result;
    }
    const int assign_result = assign_new_no_locked(registry, mapping_size, trx_id, out_trx_no);
    release_registry_latch(registry);
    return assign_result;
}

int mylite_ownerless_trx_registry_end(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || slot_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return latch_result;
    }
    const int end_result = end_locked(registry, mapping_size, slot_index, slot_generation);
    release_registry_latch(registry);
    return end_result;
}

int mylite_ownerless_trx_registry_release_owner(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_transactions
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        out_released_transactions == nullptr) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return latch_result;
    }
    const int release_result =
        release_owner_locked(registry, mapping_size, owner_id, out_released_transactions);
    release_registry_latch(registry);
    return release_result;
}

int mylite_ownerless_trx_registry_snapshot(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t *out_trx_ids,
    std::uint32_t trx_id_capacity,
    std::uint32_t *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_oldest_active_trx_id
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) ||
        (trx_id_capacity > 0U && out_trx_ids == nullptr) || out_trx_id_count == nullptr ||
        out_next_trx_id == nullptr || out_oldest_active_trx_id == nullptr) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return latch_result;
    }
    const int snapshot_result = snapshot_locked(
        registry,
        mapping_size,
        out_trx_ids,
        trx_id_capacity,
        out_trx_id_count,
        out_next_trx_id,
        out_oldest_active_trx_id
    );
    release_registry_latch(registry);
    return snapshot_result;
}

int mylite_ownerless_trx_registry_snapshot_read_view(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t *out_trx_ids,
    std::uint32_t trx_id_capacity,
    std::uint32_t *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_min_trx_no
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) ||
        (trx_id_capacity > 0U && out_trx_ids == nullptr) || out_trx_id_count == nullptr ||
        out_next_trx_id == nullptr || out_min_trx_no == nullptr) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return latch_result;
    }
    const int snapshot_result = snapshot_read_view_locked(
        registry,
        mapping_size,
        out_trx_ids,
        trx_id_capacity,
        out_trx_id_count,
        out_next_trx_id,
        out_min_trx_no
    );
    release_registry_latch(registry);
    return snapshot_result;
}

std::uint64_t mylite_ownerless_trx_registry_active_count(const void *mapping) {
    if (mapping == nullptr) {
        return 0U;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_active_count_offset);
}

std::uint64_t mylite_ownerless_trx_registry_oldest_active_trx_id(
    const void *mapping,
    std::size_t mapping_size
) {
    if (!mapping_can_hold_registry(mapping, mapping_size)) {
        return 0U;
    }

    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_oldest_active_trx_id_offset);
}

std::uint64_t mylite_ownerless_trx_registry_next_trx_id(const void *mapping) {
    if (mapping == nullptr) {
        return 0U;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_next_trx_id_offset);
}

namespace {

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

int acquire_registry_latch(
    unsigned char *registry,
    std::chrono::steady_clock::time_point deadline
) {
    mylite_ownerless_wait_word *latch = registry_latch(registry);

    for (;;) {
        std::uint32_t observed = mylite_ownerless_wait_load(latch);
        if ((observed & 1U) == 0U) {
            std::uint32_t expected = observed;
            if (cas_wait_word(latch, &expected, observed + 1U)) {
                return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
            }
            continue;
        }

        const unsigned timeout_ms = remaining_timeout_ms(deadline);
        if (timeout_ms == 0U) {
            return MYLITE_OWNERLESS_TRX_REGISTRY_TIMEOUT;
        }
        const int wait_result =
            mylite_ownerless_wait_for_change(latch, observed, timeout_ms);
        if (wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT) {
            return MYLITE_OWNERLESS_TRX_REGISTRY_TIMEOUT;
        }
        if (wait_result != MYLITE_OWNERLESS_WAIT_OK) {
            return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
        }
    }
}

void release_registry_latch(unsigned char *registry) {
    mylite_ownerless_wait_word *latch = registry_latch(registry);
    const std::uint32_t observed = mylite_ownerless_wait_load(latch);
    mylite_ownerless_wait_store(latch, observed + 1U);
    static_cast<void>(mylite_ownerless_wait_wake(latch));
}

int begin_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t *out_trx_id,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    const std::uint64_t trx_id = load64(registry, k_header_next_trx_id_offset);
    if (trx_id == std::numeric_limits<std::uint64_t>::max()) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != k_slot_state_free) {
            continue;
        }

        const std::uint64_t current_generation = load64(registry, k_header_generation_offset);
        if (current_generation == std::numeric_limits<std::uint64_t>::max()) {
            return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
        }
        const std::uint64_t active_count = load64(registry, k_header_active_count_offset);
        if (active_count == std::numeric_limits<std::uint64_t>::max()) {
            return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
        }
        const std::uint64_t generation = current_generation + 1U;
        store64(slot, k_slot_generation_offset, generation);
        store64(slot, k_slot_trx_id_offset, trx_id);
        store32(slot, k_slot_owner_id_offset, owner_id);
        store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_TRX_STATE_ACTIVE);
        store64(slot, k_slot_trx_no_offset, MYLITE_OWNERLESS_TRX_REGISTRY_UNASSIGNED_NO);
        store64(registry, k_header_next_trx_id_offset, trx_id + 1U);
        store64(registry, k_header_generation_offset, generation);
        store64(
            registry,
            k_header_active_count_offset,
            active_count + 1U
        );
        const std::uint64_t oldest_active =
            load64(registry, k_header_oldest_active_trx_id_offset);
        if (oldest_active == 0U || trx_id < oldest_active) {
            store64(registry, k_header_oldest_active_trx_id_offset, trx_id);
        }
        *out_trx_id = trx_id;
        *out_slot_index = index;
        *out_slot_generation = generation;
        return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
    }
    return MYLITE_OWNERLESS_TRX_REGISTRY_FULL;
}

int allocate_id_locked(unsigned char *registry, std::uint64_t *out_trx_id) {
    const std::uint64_t trx_id = load64(registry, k_header_next_trx_id_offset);
    if (trx_id == std::numeric_limits<std::uint64_t>::max()) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    store64(registry, k_header_next_trx_id_offset, trx_id + 1U);
    *out_trx_id = trx_id;
    return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
}

int assign_no_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t trx_id,
    std::uint64_t trx_no
) {
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_TRX_STATE_ACTIVE &&
            load64(slot, k_slot_trx_id_offset) == trx_id) {
            store64(slot, k_slot_trx_no_offset, trx_no);
            return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
        }
    }
    return MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND;
}

int assign_new_no_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t trx_id,
    std::uint64_t *out_trx_no
) {
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_TRX_STATE_ACTIVE &&
            load64(slot, k_slot_trx_id_offset) == trx_id) {
            const int allocate_result = allocate_id_locked(registry, out_trx_no);
            if (allocate_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
                return allocate_result;
            }
            store64(slot, k_slot_trx_no_offset, *out_trx_no);
            return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
        }
    }
    return MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND;
}

int end_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (slot_index >= slot_count(registry)) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND;
    }

    unsigned char *slot = slot_at(registry, slot_index);
    if (static_cast<std::size_t>(
            slot + MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE - registry
        ) > mapping_size ||
        load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_TRX_STATE_ACTIVE ||
        load64(slot, k_slot_generation_offset) != slot_generation) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND;
    }

    const std::uint64_t current_generation = load64(registry, k_header_generation_offset);
    if (current_generation == std::numeric_limits<std::uint64_t>::max()) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }
    const std::uint64_t active_count = load64(registry, k_header_active_count_offset);
    if (active_count == 0U) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }
    const std::uint64_t generation = current_generation + 1U;
    std::memset(slot, 0, MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE);
    store64(slot, k_slot_generation_offset, generation);
    store64(registry, k_header_generation_offset, generation);
    store64(
        registry,
        k_header_active_count_offset,
        active_count - 1U
    );
    recompute_oldest_active_trx_id_locked(registry, mapping_size);
    return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
}

int release_owner_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_transactions
) {
    std::uint32_t released_transactions = 0U;
    std::uint64_t generation = load64(registry, k_header_generation_offset);
    std::uint64_t active_count = load64(registry, k_header_active_count_offset);
    const std::uint32_t count = slot_count(registry);

    if (generation > std::numeric_limits<std::uint64_t>::max() - active_count) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_TRX_STATE_ACTIVE ||
            load32(slot, k_slot_owner_id_offset) != owner_id) {
            continue;
        }
        if (active_count == 0U) {
            return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
        }

        ++generation;
        std::memset(slot, 0, MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE);
        store64(slot, k_slot_generation_offset, generation);
        --active_count;
        ++released_transactions;
    }

    if (released_transactions > 0U) {
        store64(registry, k_header_generation_offset, generation);
        store64(registry, k_header_active_count_offset, active_count);
        recompute_oldest_active_trx_id_locked(registry, mapping_size);
    }

    *out_released_transactions = released_transactions;
    return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
}

int snapshot_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t *out_trx_ids,
    std::uint32_t trx_id_capacity,
    std::uint32_t *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_oldest_active_trx_id
) {
    const std::uint64_t active_count = load64(registry, k_header_active_count_offset);
    if (active_count > std::numeric_limits<std::uint32_t>::max()) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    *out_trx_id_count = static_cast<std::uint32_t>(active_count);
    *out_next_trx_id = load64(registry, k_header_next_trx_id_offset);
    *out_oldest_active_trx_id = load64(registry, k_header_oldest_active_trx_id_offset);
    if (active_count > trx_id_capacity) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_FULL;
    }

    std::uint32_t copied = 0U;
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        const unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_TRX_STATE_ACTIVE) {
            continue;
        }
        if (copied >= trx_id_capacity) {
            return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
        }
        out_trx_ids[copied] = load64(slot, k_slot_trx_id_offset);
        ++copied;
    }

    if (copied != active_count) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    if (copied > 1U) {
        std::sort(out_trx_ids, out_trx_ids + copied);
    }
    return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
}

int snapshot_read_view_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t *out_trx_ids,
    std::uint32_t trx_id_capacity,
    std::uint32_t *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_min_trx_no
) {
    const std::uint64_t active_count = load64(registry, k_header_active_count_offset);
    if (active_count > std::numeric_limits<std::uint32_t>::max()) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    const std::uint64_t next_trx_id = load64(registry, k_header_next_trx_id_offset);
    std::uint64_t min_trx_no = next_trx_id;
    const bool snapshot_full = active_count > trx_id_capacity;
    *out_trx_id_count = static_cast<std::uint32_t>(active_count);
    *out_next_trx_id = next_trx_id;

    std::uint32_t copied = 0U;
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        const unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_TRX_STATE_ACTIVE) {
            continue;
        }

        const std::uint64_t trx_no = load64(slot, k_slot_trx_no_offset);
        if (trx_no < min_trx_no) {
            min_trx_no = trx_no;
        }
        if (snapshot_full) {
            continue;
        }
        if (copied >= trx_id_capacity) {
            return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
        }
        out_trx_ids[copied] = load64(slot, k_slot_trx_id_offset);
        ++copied;
    }

    if (!snapshot_full && copied != active_count) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_ERROR;
    }

    *out_min_trx_no = min_trx_no;
    if (snapshot_full) {
        return MYLITE_OWNERLESS_TRX_REGISTRY_FULL;
    }
    if (copied > 1U) {
        std::sort(out_trx_ids, out_trx_ids + copied);
    }
    return MYLITE_OWNERLESS_TRX_REGISTRY_OK;
}

void recompute_oldest_active_trx_id_locked(
    unsigned char *registry,
    std::size_t mapping_size
) {
    std::uint64_t oldest_trx_id = 0U;
    const std::uint32_t count = slot_count(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        const unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_TRX_STATE_ACTIVE) {
            continue;
        }

        const std::uint64_t trx_id = load64(slot, k_slot_trx_id_offset);
        if (oldest_trx_id == 0U || trx_id < oldest_trx_id) {
            oldest_trx_id = trx_id;
        }
    }

    store64(registry, k_header_oldest_active_trx_id_offset, oldest_trx_id);
}

unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<unsigned>(std::max<std::chrono::milliseconds::rep>(remaining.count(), 1));
}

bool registry_size_fits(std::uint32_t slot_count) {
    const std::size_t max_slots =
        (std::numeric_limits<std::size_t>::max() -
         MYLITE_OWNERLESS_TRX_REGISTRY_HEADER_SIZE) /
        MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE;
    return static_cast<std::size_t>(slot_count) <= max_slots;
}

bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size) {
    if (mapping == nullptr || mapping_size < MYLITE_OWNERLESS_TRX_REGISTRY_HEADER_SIZE) {
        return false;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    const std::uint32_t count = slot_count(registry);
    const std::size_t registry_size = mylite_ownerless_trx_registry_size(count);
    return count > 0U &&
           load32(registry, k_header_slot_size_offset) ==
               MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE &&
           registry_size > 0U && mapping_size >= registry_size;
}

std::uint32_t slot_count(const unsigned char *registry) {
    return load32(registry, k_header_slot_count_offset);
}

unsigned char *slot_at(unsigned char *registry, std::uint32_t index) {
    return registry + MYLITE_OWNERLESS_TRX_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(index) * MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE);
}

mylite_ownerless_wait_word *registry_latch(unsigned char *registry) {
    return reinterpret_cast<mylite_ownerless_wait_word *>(registry + k_header_latch_offset);
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

bool cas_wait_word(
    mylite_ownerless_wait_word *word,
    std::uint32_t *expected,
    std::uint32_t value
) {
    return __atomic_compare_exchange_n(
        &word->value,
        expected,
        value,
        false,
        __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED
    );
}

} // namespace

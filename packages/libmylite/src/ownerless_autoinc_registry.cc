#include "ownerless_autoinc_registry.h"

#include "ownerless_latch.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t k_header_slot_count_offset = 0;
constexpr std::size_t k_header_slot_size_offset = 4;
constexpr std::size_t k_header_latch_offset = 32;
constexpr std::size_t k_slot_table_id_offset = 0;
constexpr std::size_t k_slot_next_value_offset = 8;
constexpr std::size_t k_slot_state_offset = 16;
constexpr std::uint32_t k_slot_state_free = 0;
constexpr std::uint32_t k_slot_state_active = 1;
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
void release_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
);
unsigned char *find_slot(unsigned char *registry, std::size_t mapping_size, std::uint64_t table_id);
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
        slot = find_free_slot(registry, mapping_size);
        if (slot == nullptr) {
            release_registry_latch(registry, owner_id, owner_generation);
            return MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL;
        }
        store64(slot, k_slot_table_id_offset, table_id);
        store64(slot, k_slot_next_value_offset, seed_next_value);
        store32(slot, k_slot_state_offset, k_slot_state_active);
        *out_next_value = seed_next_value;
    } else {
        const std::uint64_t stored = load64(slot, k_slot_next_value_offset);
        const std::uint64_t next_value = std::max(stored, seed_next_value);
        store64(slot, k_slot_next_value_offset, next_value);
        *out_next_value = next_value;
    }

    release_registry_latch(registry, owner_id, owner_generation);
    return MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK;
}

int mylite_ownerless_autoinc_registry_publish(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t table_id,
    std::uint64_t next_value
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
    if (slot == nullptr) {
        slot = find_free_slot(registry, mapping_size);
        if (slot == nullptr) {
            release_registry_latch(registry, owner_id, owner_generation);
            return MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL;
        }
        store64(slot, k_slot_table_id_offset, table_id);
        store64(slot, k_slot_next_value_offset, next_value);
        store32(slot, k_slot_state_offset, k_slot_state_active);
    } else if (next_value > load64(slot, k_slot_next_value_offset)) {
        store64(slot, k_slot_next_value_offset, next_value);
    }

    release_registry_latch(registry, owner_id, owner_generation);
    return MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK;
}

namespace {

bool registry_size_fits(std::uint32_t count) {
    return count > 0U && count <= (std::numeric_limits<std::size_t>::max() -
                                   MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE) /
                                      MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE;
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
    return MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR;
}

void release_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    static_cast<void>(
        mylite_ownerless_latch_release(registry_latch(registry), owner_id, owner_generation)
    );
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

std::uint32_t load32(const unsigned char *base, std::size_t offset) {
    std::uint32_t value;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

std::uint64_t load64(const unsigned char *base, std::size_t offset) {
    std::uint64_t value;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

void store32(unsigned char *base, std::size_t offset, std::uint32_t value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

void store64(unsigned char *base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

} // namespace

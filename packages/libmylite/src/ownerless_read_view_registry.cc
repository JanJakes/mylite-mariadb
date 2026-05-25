#include "ownerless_read_view_registry.h"

#include "ownerless_wait.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr std::size_t k_header_slot_count_offset = 0;
constexpr std::size_t k_header_slot_size_offset = 4;
constexpr std::size_t k_header_generation_offset = 8;
constexpr std::size_t k_header_active_count_offset = 16;
constexpr std::size_t k_header_latch_offset = 24;
constexpr std::size_t k_slot_generation_offset = 0;
constexpr std::size_t k_slot_owner_id_offset = 8;
constexpr std::size_t k_slot_state_offset = 12;
constexpr std::size_t k_slot_low_limit_id_offset = 16;
constexpr std::size_t k_slot_low_limit_no_offset = 24;
constexpr std::size_t k_slot_trx_id_count_offset = 32;
constexpr std::size_t k_slot_trx_ids_offset = 64;
constexpr std::uint32_t k_slot_state_free = 0;

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
int acquire_registry_latch(unsigned char *registry, std::chrono::steady_clock::time_point deadline);
void release_registry_latch(unsigned char *registry);
int open_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t low_limit_id,
    std::uint64_t low_limit_no,
    const std::uint64_t *trx_ids,
    std::uint32_t trx_id_count,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
);
int close_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
);
int release_owner_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_views
);
std::uint32_t owner_active_count_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id
);
int snapshot_oldest_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t *out_trx_ids,
    std::uint32_t trx_id_capacity,
    std::uint32_t *out_trx_id_count,
    std::uint64_t *out_low_limit_id,
    std::uint64_t *out_low_limit_no
);
void clear_active_slot_locked(unsigned char *registry, unsigned char *slot);
void append_slot_ids_below_limit(
    const unsigned char *slot,
    std::uint64_t low_limit_id,
    std::vector<std::uint64_t> &trx_ids
);
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

std::size_t mylite_ownerless_read_view_registry_size(std::uint32_t slot_count) {
    if (!registry_size_fits(slot_count)) {
        return 0U;
    }
    return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(slot_count) *
            MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE);
}

int mylite_ownerless_read_view_registry_initialize(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_count
) {
    const std::size_t registry_size = mylite_ownerless_read_view_registry_size(slot_count);
    if (mapping == nullptr || registry_size == 0U || mapping_size < registry_size ||
        slot_count == 0U) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    std::memset(registry, 0, registry_size);
    store32(registry, k_header_slot_count_offset, slot_count);
    store32(registry, k_header_slot_size_offset, MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE);
    return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK;
}

int mylite_ownerless_read_view_registry_open(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t low_limit_id,
    std::uint64_t low_limit_no,
    const std::uint64_t *trx_ids,
    std::uint32_t trx_id_count,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        low_limit_id == 0U || low_limit_no == 0U ||
        (trx_id_count > 0U && trx_ids == nullptr) || out_slot_index == nullptr ||
        out_slot_generation == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ERROR;
    }
    if (trx_id_count > MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ID_CAPACITY) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK) {
        return latch_result;
    }
    const int open_result = open_locked(
        registry,
        mapping_size,
        owner_id,
        low_limit_id,
        low_limit_no,
        trx_ids,
        trx_id_count,
        out_slot_index,
        out_slot_generation
    );
    release_registry_latch(registry);
    return open_result;
}

int mylite_ownerless_read_view_registry_close(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        slot_generation == 0U) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK) {
        return latch_result;
    }
    const int close_result =
        close_locked(registry, mapping_size, owner_id, slot_index, slot_generation);
    release_registry_latch(registry);
    return close_result;
}

int mylite_ownerless_read_view_registry_release_owner(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_views
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        out_released_views == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK) {
        return latch_result;
    }
    const int release_result =
        release_owner_locked(registry, mapping_size, owner_id, out_released_views);
    release_registry_latch(registry);
    return release_result;
}

int mylite_ownerless_read_view_registry_snapshot_oldest(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t *out_trx_ids,
    std::uint32_t trx_id_capacity,
    std::uint32_t *out_trx_id_count,
    std::uint64_t *out_low_limit_id,
    std::uint64_t *out_low_limit_no
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) ||
        (trx_id_capacity > 0U && out_trx_ids == nullptr) ||
        out_trx_id_count == nullptr || out_low_limit_id == nullptr ||
        out_low_limit_no == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK) {
        return latch_result;
    }
    const int snapshot_result = snapshot_oldest_locked(
        registry,
        mapping_size,
        out_trx_ids,
        trx_id_capacity,
        out_trx_id_count,
        out_low_limit_id,
        out_low_limit_no
    );
    release_registry_latch(registry);
    return snapshot_result;
}

std::uint64_t mylite_ownerless_read_view_registry_active_count(const void *mapping) {
    if (mapping == nullptr) {
        return 0U;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_active_count_offset);
}

int mylite_ownerless_read_view_registry_owner_active_count(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_active_count
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        out_active_count == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK) {
        return latch_result;
    }
    *out_active_count = owner_active_count_locked(registry, mapping_size, owner_id);
    release_registry_latch(registry);
    return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK;
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
                return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK;
            }
            continue;
        }

        const unsigned timeout_ms = remaining_timeout_ms(deadline);
        if (timeout_ms == 0U) {
            return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_TIMEOUT;
        }
        const int wait_result = mylite_ownerless_wait_for_change(latch, observed, timeout_ms);
        if (wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT) {
            return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_TIMEOUT;
        }
        if (wait_result != MYLITE_OWNERLESS_WAIT_OK) {
            return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ERROR;
        }
    }
}

void release_registry_latch(unsigned char *registry) {
    mylite_ownerless_wait_word *latch = registry_latch(registry);
    const std::uint32_t observed = mylite_ownerless_wait_load(latch);
    mylite_ownerless_wait_store(latch, observed + 1U);
    static_cast<void>(mylite_ownerless_wait_wake(latch));
}

int open_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t low_limit_id,
    std::uint64_t low_limit_no,
    const std::uint64_t *trx_ids,
    std::uint32_t trx_id_count,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(slot + MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE -
                                     registry) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != k_slot_state_free) {
            continue;
        }

        const std::uint64_t generation = load64(slot, k_slot_generation_offset) + 1U;
        store64(slot, k_slot_generation_offset, generation);
        store32(slot, k_slot_owner_id_offset, owner_id);
        store64(slot, k_slot_low_limit_id_offset, low_limit_id);
        store64(slot, k_slot_low_limit_no_offset, low_limit_no);
        store32(slot, k_slot_trx_id_count_offset, trx_id_count);
        for (std::uint32_t id_index = 0; id_index < trx_id_count; ++id_index) {
            store64(slot, k_slot_trx_ids_offset + (id_index * sizeof(std::uint64_t)),
                    trx_ids[id_index]);
        }
        store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_READ_VIEW_STATE_ACTIVE);
        store64(registry, k_header_generation_offset,
                load64(registry, k_header_generation_offset) + 1U);
        store64(registry, k_header_active_count_offset,
                load64(registry, k_header_active_count_offset) + 1U);
        *out_slot_index = index;
        *out_slot_generation = generation;
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK;
    }
    return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL;
}

int close_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (slot_index >= slot_count(registry)) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_NOT_FOUND;
    }
    unsigned char *slot = slot_at(registry, slot_index);
    if (static_cast<std::size_t>(slot + MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE -
                                 registry) > mapping_size ||
        load32(slot, k_slot_state_offset) == k_slot_state_free ||
        load32(slot, k_slot_owner_id_offset) != owner_id ||
        load64(slot, k_slot_generation_offset) != slot_generation) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_NOT_FOUND;
    }

    clear_active_slot_locked(registry, slot);
    return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK;
}

int release_owner_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_views
) {
    std::uint32_t released_views = 0;
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(slot + MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE -
                                     registry) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == k_slot_state_free ||
            load32(slot, k_slot_owner_id_offset) != owner_id) {
            continue;
        }

        clear_active_slot_locked(registry, slot);
        ++released_views;
    }

    *out_released_views = released_views;
    return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK;
}

std::uint32_t owner_active_count_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id
) {
    std::uint32_t active_count = 0U;
    const std::uint32_t count = slot_count(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_READ_VIEW_STATE_ACTIVE &&
            load32(slot, k_slot_owner_id_offset) == owner_id) {
            ++active_count;
        }
    }
    return active_count;
}

int snapshot_oldest_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t *out_trx_ids,
    std::uint32_t trx_id_capacity,
    std::uint32_t *out_trx_id_count,
    std::uint64_t *out_low_limit_id,
    std::uint64_t *out_low_limit_no
) {
    std::uint64_t low_limit_id = 0;
    std::uint64_t low_limit_no = 0;
    const std::uint32_t count = slot_count(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        const unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(slot + MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE -
                                     registry) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == k_slot_state_free) {
            continue;
        }

        const std::uint64_t slot_low_limit_id = load64(slot, k_slot_low_limit_id_offset);
        const std::uint64_t slot_low_limit_no = load64(slot, k_slot_low_limit_no_offset);
        low_limit_id = low_limit_id == 0U ? slot_low_limit_id
                                          : std::min(low_limit_id, slot_low_limit_id);
        low_limit_no = low_limit_no == 0U ? slot_low_limit_no
                                          : std::min(low_limit_no, slot_low_limit_no);
    }

    std::vector<std::uint64_t> trx_ids;
    if (low_limit_id != 0U) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const unsigned char *slot = slot_at(registry, index);
            if (static_cast<std::size_t>(slot + MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE -
                                         registry) > mapping_size) {
                break;
            }
            if (load32(slot, k_slot_state_offset) == k_slot_state_free) {
                continue;
            }
            append_slot_ids_below_limit(slot, low_limit_id, trx_ids);
        }
    }

    std::sort(trx_ids.begin(), trx_ids.end());
    trx_ids.erase(std::unique(trx_ids.begin(), trx_ids.end()), trx_ids.end());

    *out_trx_id_count = static_cast<std::uint32_t>(trx_ids.size());
    *out_low_limit_id = low_limit_id;
    *out_low_limit_no = low_limit_no;
    if (trx_ids.size() > trx_id_capacity) {
        return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL;
    }
    for (std::size_t index = 0; index < trx_ids.size(); ++index) {
        out_trx_ids[index] = trx_ids[index];
    }
    return MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK;
}

void clear_active_slot_locked(unsigned char *registry, unsigned char *slot) {
    store32(slot, k_slot_state_offset, k_slot_state_free);
    store32(slot, k_slot_owner_id_offset, 0U);
    store64(slot, k_slot_low_limit_id_offset, 0U);
    store64(slot, k_slot_low_limit_no_offset, 0U);
    store32(slot, k_slot_trx_id_count_offset, 0U);
    store64(registry, k_header_generation_offset,
            load64(registry, k_header_generation_offset) + 1U);
    store64(registry, k_header_active_count_offset,
            load64(registry, k_header_active_count_offset) - 1U);
}

void append_slot_ids_below_limit(
    const unsigned char *slot,
    std::uint64_t low_limit_id,
    std::vector<std::uint64_t> &trx_ids
) {
    const std::uint32_t count = load32(slot, k_slot_trx_id_count_offset);
    const std::uint32_t capped_count =
        std::min<std::uint32_t>(count, MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ID_CAPACITY);
    for (std::uint32_t index = 0; index < capped_count; ++index) {
        const std::uint64_t trx_id =
            load64(slot, k_slot_trx_ids_offset + (index * sizeof(std::uint64_t)));
        if (trx_id != 0U && trx_id < low_limit_id) {
            trx_ids.push_back(trx_id);
        }
    }
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
         MYLITE_OWNERLESS_READ_VIEW_REGISTRY_HEADER_SIZE) /
        MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE;
    return static_cast<std::size_t>(slot_count) <= max_slots;
}

bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size) {
    if (mapping == nullptr || mapping_size < MYLITE_OWNERLESS_READ_VIEW_REGISTRY_HEADER_SIZE) {
        return false;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    const std::uint32_t count = slot_count(registry);
    const std::size_t registry_size = mylite_ownerless_read_view_registry_size(count);
    return count > 0U &&
           load32(registry, k_header_slot_size_offset) ==
               MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE &&
           registry_size > 0U && mapping_size >= registry_size;
}

std::uint32_t slot_count(const unsigned char *registry) {
    return load32(registry, k_header_slot_count_offset);
}

unsigned char *slot_at(unsigned char *registry, std::uint32_t index) {
    return registry + MYLITE_OWNERLESS_READ_VIEW_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(index) *
            MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE);
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

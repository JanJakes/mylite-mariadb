#include "ownerless_process_registry.h"

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
constexpr std::size_t k_header_latch_offset = 24;
constexpr std::size_t k_slot_generation_offset = 0;
constexpr std::size_t k_slot_state_offset = 8;
constexpr std::size_t k_slot_open_mode_offset = 12;
constexpr std::size_t k_slot_pid_offset = 16;
constexpr std::size_t k_slot_heartbeat_offset = 24;
constexpr std::size_t k_slot_shm_generation_offset = 32;

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
int acquire_registry_latch(unsigned char *registry, std::chrono::steady_clock::time_point deadline);
void release_registry_latch(unsigned char *registry);
int allocate_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t pid,
    std::uint32_t open_mode,
    std::uint64_t shm_generation,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
);
int release_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
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

std::size_t mylite_ownerless_process_registry_size(std::uint32_t slot_count) {
    if (!registry_size_fits(slot_count)) {
        return 0U;
    }
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(slot_count) * MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE);
}

int mylite_ownerless_process_registry_initialize(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_count
) {
    const std::size_t registry_size = mylite_ownerless_process_registry_size(slot_count);
    if (mapping == nullptr || registry_size == 0U || mapping_size < registry_size) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    std::memset(registry, 0, registry_size);
    store32(registry, k_header_slot_count_offset, slot_count);
    store32(registry, k_header_slot_size_offset, MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE);
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
}

int mylite_ownerless_process_registry_allocate(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t pid,
    std::uint32_t open_mode,
    std::uint64_t shm_generation,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || pid == 0U ||
        open_mode == 0U || out_slot_index == nullptr || out_slot_generation == nullptr) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    const int allocate_result = allocate_locked(
        registry,
        mapping_size,
        pid,
        open_mode,
        shm_generation,
        out_slot_index,
        out_slot_generation
    );
    release_registry_latch(registry);
    return allocate_result;
}

int mylite_ownerless_process_registry_release(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || slot_generation == 0U) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    const int release_result = release_locked(registry, mapping_size, slot_index, slot_generation);
    release_registry_latch(registry);
    return release_result;
}

std::uint64_t mylite_ownerless_process_registry_active_count(const void *mapping) {
    if (mapping == nullptr) {
        return 0U;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_active_count_offset);
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
                return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
            }
            continue;
        }

        const unsigned timeout_ms = remaining_timeout_ms(deadline);
        if (timeout_ms == 0U) {
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_TIMEOUT;
        }
        const int wait_result =
            mylite_ownerless_wait_for_change(latch, observed, timeout_ms);
        if (wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT) {
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_TIMEOUT;
        }
        if (wait_result != MYLITE_OWNERLESS_WAIT_OK) {
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
        }
    }
}

void release_registry_latch(unsigned char *registry) {
    mylite_ownerless_wait_word *latch = registry_latch(registry);
    const std::uint32_t observed = mylite_ownerless_wait_load(latch);
    mylite_ownerless_wait_store(latch, observed + 1U);
    static_cast<void>(mylite_ownerless_wait_wake(latch));
}

int allocate_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint64_t pid,
    std::uint32_t open_mode,
    std::uint64_t shm_generation,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != 0U) {
            continue;
        }

        const std::uint64_t generation = load64(registry, k_header_generation_offset) + 1U;
        store64(slot, k_slot_generation_offset, generation);
        store32(slot, k_slot_open_mode_offset, open_mode);
        store64(slot, k_slot_pid_offset, pid);
        store64(slot, k_slot_heartbeat_offset, generation);
        store64(slot, k_slot_shm_generation_offset, shm_generation);
        store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE);
        store64(registry, k_header_generation_offset, generation);
        store64(
            registry,
            k_header_active_count_offset,
            load64(registry, k_header_active_count_offset) + 1U
        );
        *out_slot_index = index;
        *out_slot_generation = generation;
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
    }
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_FULL;
}

int release_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (slot_index >= slot_count(registry)) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND;
    }
    unsigned char *slot = slot_at(registry, slot_index);
    if (static_cast<std::size_t>(
            slot + MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE - registry
        ) > mapping_size ||
        load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE ||
        load64(slot, k_slot_generation_offset) != slot_generation) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND;
    }

    const std::uint64_t generation = load64(registry, k_header_generation_offset) + 1U;
    std::memset(slot, 0, MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE);
    store64(slot, k_slot_generation_offset, generation);
    store64(registry, k_header_generation_offset, generation);
    store64(
        registry,
        k_header_active_count_offset,
        load64(registry, k_header_active_count_offset) - 1U
    );
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
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
         MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE) /
        MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE;
    return static_cast<std::size_t>(slot_count) <= max_slots;
}

bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size) {
    if (mapping == nullptr || mapping_size < MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE) {
        return false;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    const std::uint32_t count = slot_count(registry);
    const std::size_t registry_size = mylite_ownerless_process_registry_size(count);
    return count > 0U &&
           load32(registry, k_header_slot_size_offset) ==
               MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE &&
           registry_size > 0U && mapping_size >= registry_size;
}

std::uint32_t slot_count(const unsigned char *registry) {
    return load32(registry, k_header_slot_count_offset);
}

unsigned char *slot_at(unsigned char *registry, std::uint32_t index) {
    return registry + MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(index) * MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE);
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

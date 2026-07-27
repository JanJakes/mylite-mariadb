#include "ownerless_page_pin_registry.h"

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
constexpr std::size_t k_header_generation_offset = 8;
constexpr std::size_t k_header_active_count_offset = 16;
constexpr std::size_t k_header_latch_offset = 24;
constexpr std::size_t k_slot_generation_offset = 0;
constexpr std::size_t k_slot_owner_id_offset = 8;
constexpr std::size_t k_slot_state_offset = 12;
constexpr std::size_t k_slot_read_lsn_offset = 16;
constexpr std::uint32_t k_slot_state_free = 0;
constexpr unsigned k_latch_timeout_ms = 5000;

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
int acquire_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
);
int finish_registry_operation(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
);
bool registry_size_fits(std::uint32_t slot_count);
bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size);
std::uint32_t slot_count(const unsigned char *registry);
unsigned char *slot_at(unsigned char *registry, std::uint32_t index);
mylite_ownerless_latch *registry_latch(unsigned char *registry);
void clear_active_slot_locked(unsigned char *registry, unsigned char *slot);
bool generation_can_advance(const unsigned char *registry, std::uint64_t amount);
bool repair_registry_locked(unsigned char *registry, std::size_t mapping_size);
std::uint32_t owner_active_count_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id
);
int snapshot_oldest_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
);
int snapshot_oldest_excluding_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t exclude_owner_id,
    std::uint32_t exclude_slot_index,
    std::uint64_t exclude_slot_generation,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
);
std::uint32_t load32(const unsigned char *base, std::size_t offset);
std::uint64_t load64(const unsigned char *base, std::size_t offset);
void store32(unsigned char *base, std::size_t offset, std::uint32_t value);
void store64(unsigned char *base, std::size_t offset, std::uint64_t value);

} // namespace

std::size_t mylite_ownerless_page_pin_registry_size(std::uint32_t slot_count) {
    if (!registry_size_fits(slot_count)) {
        return 0U;
    }
    return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(slot_count) * MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE);
}

int mylite_ownerless_page_pin_registry_initialize(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_count
) {
    const std::size_t registry_size = mylite_ownerless_page_pin_registry_size(slot_count);
    if (mapping == nullptr || registry_size == 0U || mapping_size < registry_size ||
        slot_count == 0U) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    std::memset(registry, 0, registry_size);
    mylite_ownerless_latch_initialize(registry_latch(registry));
    store32(registry, k_header_slot_count_offset, slot_count);
    store32(registry, k_header_slot_size_offset, MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE);
    return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK;
}

int mylite_ownerless_page_pin_registry_open(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t read_lsn,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || read_lsn == 0U || out_slot_index == nullptr ||
        out_slot_generation == nullptr) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }
    *out_slot_index = 0U;
    *out_slot_generation = 0U;

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        owner_id,
        owner_generation,
        wait_deadline(k_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return latch_result;
    }

    int result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_FULL;
    bool applied = false;
    if (!generation_can_advance(registry, 1U)) {
        return finish_registry_operation(
            registry,
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR,
            false
        );
    }
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != k_slot_state_free) {
            continue;
        }

        const std::uint64_t slot_generation = load64(slot, k_slot_generation_offset);
        if (slot_generation == std::numeric_limits<std::uint64_t>::max()) {
            result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
            continue;
        }
        const std::uint64_t generation = slot_generation + 1U;
        store64(slot, k_slot_generation_offset, generation);
        store32(slot, k_slot_owner_id_offset, owner_id);
        store64(slot, k_slot_read_lsn_offset, read_lsn);
        store64(
            registry,
            k_header_generation_offset,
            load64(registry, k_header_generation_offset) + 1U
        );
        store64(
            registry,
            k_header_active_count_offset,
            load64(registry, k_header_active_count_offset) + 1U
        );
        store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_PAGE_PIN_STATE_ACTIVE);
        *out_slot_index = index;
        *out_slot_generation = generation;
        result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK;
        applied = true;
        break;
    }

    return finish_registry_operation(registry, owner_id, owner_generation, result, applied);
}

int mylite_ownerless_page_pin_registry_replace(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t old_slot_index,
    std::uint64_t old_slot_generation,
    std::uint64_t read_lsn,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || old_slot_generation == 0U || read_lsn == 0U ||
        out_slot_index == nullptr || out_slot_generation == nullptr) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }
    *out_slot_index = 0U;
    *out_slot_generation = 0U;

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        owner_id,
        owner_generation,
        wait_deadline(k_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return latch_result;
    }

    int result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_NOT_FOUND;
    bool applied = false;
    if (old_slot_index < slot_count(registry)) {
        unsigned char *slot = slot_at(registry, old_slot_index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE - registry
            ) <= mapping_size &&
            load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_PAGE_PIN_STATE_ACTIVE &&
            load32(slot, k_slot_owner_id_offset) == owner_id &&
            load64(slot, k_slot_generation_offset) == old_slot_generation) {
            if (old_slot_generation == std::numeric_limits<std::uint64_t>::max() ||
                !generation_can_advance(registry, 1U)) {
                result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
            } else {
                const std::uint64_t replacement_generation = old_slot_generation + 1U;
                store64(slot, k_slot_read_lsn_offset, read_lsn);
                store64(slot, k_slot_generation_offset, replacement_generation);
                store64(
                    registry,
                    k_header_generation_offset,
                    load64(registry, k_header_generation_offset) + 1U
                );
                *out_slot_index = old_slot_index;
                *out_slot_generation = replacement_generation;
                result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK;
                applied = true;
            }
        }
    }

    return finish_registry_operation(registry, owner_id, owner_generation, result, applied);
}

int mylite_ownerless_page_pin_registry_close(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || slot_generation == 0U) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        owner_id,
        owner_generation,
        wait_deadline(k_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return latch_result;
    }

    int result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_NOT_FOUND;
    bool applied = false;
    if (slot_index < slot_count(registry)) {
        unsigned char *slot = slot_at(registry, slot_index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE - registry
            ) <= mapping_size &&
            load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_PAGE_PIN_STATE_ACTIVE &&
            load32(slot, k_slot_owner_id_offset) == owner_id &&
            load64(slot, k_slot_generation_offset) == slot_generation) {
            if (generation_can_advance(registry, 1U)) {
                clear_active_slot_locked(registry, slot);
                result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK;
                applied = true;
            } else {
                result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
            }
        }
    }

    return finish_registry_operation(registry, owner_id, owner_generation, result, applied);
}

int mylite_ownerless_page_pin_registry_release_owner(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t latch_owner_id,
    std::uint64_t latch_owner_generation,
    std::uint32_t *out_released_pins
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        latch_owner_id == 0U || latch_owner_generation == 0U || out_released_pins == nullptr) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        latch_owner_id,
        latch_owner_generation,
        wait_deadline(k_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return latch_result;
    }

    std::uint32_t released_pins = 0;
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            return finish_registry_operation(
                registry,
                latch_owner_id,
                latch_owner_generation,
                MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR,
                false
            );
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_PAGE_PIN_STATE_ACTIVE &&
            load32(slot, k_slot_owner_id_offset) == owner_id) {
            ++released_pins;
        }
    }
    if (!generation_can_advance(registry, released_pins)) {
        return finish_registry_operation(
            registry,
            latch_owner_id,
            latch_owner_generation,
            MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR,
            false
        );
    }
    released_pins = 0U;
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_PAGE_PIN_STATE_ACTIVE &&
            load32(slot, k_slot_owner_id_offset) == owner_id) {
            clear_active_slot_locked(registry, slot);
            ++released_pins;
        }
    }

    *out_released_pins = released_pins;
    return finish_registry_operation(
        registry,
        latch_owner_id,
        latch_owner_generation,
        MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK,
        released_pins > 0U
    );
}

int mylite_ownerless_page_pin_registry_snapshot_oldest(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || out_active_count == nullptr || out_oldest_read_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        owner_id,
        owner_generation,
        wait_deadline(k_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return latch_result;
    }
    const int snapshot_result =
        snapshot_oldest_locked(registry, mapping_size, out_active_count, out_oldest_read_lsn);
    return finish_registry_operation(registry, owner_id, owner_generation, snapshot_result, false);
}

int mylite_ownerless_page_pin_registry_snapshot_oldest_excluding(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t latch_owner_id,
    std::uint64_t latch_owner_generation,
    std::uint32_t exclude_owner_id,
    std::uint32_t exclude_slot_index,
    std::uint64_t exclude_slot_generation,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || latch_owner_id == 0U ||
        latch_owner_generation == 0U || exclude_owner_id == 0U || exclude_slot_generation == 0U ||
        out_active_count == nullptr || out_oldest_read_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        latch_owner_id,
        latch_owner_generation,
        wait_deadline(k_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return latch_result;
    }
    const int snapshot_result = snapshot_oldest_excluding_locked(
        registry,
        mapping_size,
        exclude_owner_id,
        exclude_slot_index,
        exclude_slot_generation,
        out_active_count,
        out_oldest_read_lsn
    );
    return finish_registry_operation(
        registry,
        latch_owner_id,
        latch_owner_generation,
        snapshot_result,
        false
    );
}

std::uint64_t mylite_ownerless_page_pin_registry_active_count(const void *mapping) {
    if (mapping == nullptr) {
        return 0U;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_active_count_offset);
}

int mylite_ownerless_page_pin_registry_owner_active_count(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t latch_owner_id,
    std::uint64_t latch_owner_generation,
    std::uint32_t *out_active_count
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        latch_owner_id == 0U || latch_owner_generation == 0U || out_active_count == nullptr) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        latch_owner_id,
        latch_owner_generation,
        wait_deadline(k_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return latch_result;
    }
    *out_active_count = owner_active_count_locked(registry, mapping_size, owner_id);
    return finish_registry_operation(
        registry,
        latch_owner_id,
        latch_owner_generation,
        MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_page_pin_registry_finish_pending_release(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }
    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        owner_id,
        owner_generation,
        wait_deadline(k_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return latch_result;
    }
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_page_pin_registry_recover_dead_latch(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    const mylite_ownerless_process_registry_liveness_context *liveness
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || liveness == nullptr) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }
    auto *registry = static_cast<unsigned char *>(mapping);
    mylite_ownerless_latch_dead_owner dead_owner = {};
    bool repaired = false;
    const int latch_result = mylite_ownerless_latch_acquire_recoverable(
        registry_latch(registry),
        owner_id,
        owner_generation,
        mylite_ownerless_process_registry_latch_owner_is_alive,
        const_cast<mylite_ownerless_process_registry_liveness_context *>(liveness),
        k_latch_timeout_ms,
        &dead_owner
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED) {
        if (!repair_registry_locked(registry, mapping_size)) {
            static_cast<void>(mylite_ownerless_latch_mark_not_recoverable(
                registry_latch(registry),
                owner_id,
                owner_generation
            ));
            return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OWNER_DEAD;
        }
        if (mylite_ownerless_latch_mark_consistent(
                registry_latch(registry),
                owner_id,
                owner_generation
            ) != MYLITE_OWNERLESS_LATCH_OK) {
            return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
        }
        repaired = true;
    } else if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        if (latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT) {
            return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_TIMEOUT;
        }
        return latch_result == MYLITE_OWNERLESS_LATCH_OWNER_DEAD ||
                       latch_result == MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE
                   ? MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OWNER_DEAD
                   : MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
    }
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK,
        repaired
    );
}

namespace {

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

int acquire_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
) {
    const int latch_result = mylite_ownerless_latch_acquire(
        registry_latch(registry),
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        remaining_timeout_ms(deadline)
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK;
    }
    if (latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_TIMEOUT;
    }
    if (latch_result == MYLITE_OWNERLESS_LATCH_OWNER_DEAD ||
        latch_result == MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE) {
        return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OWNER_DEAD;
    }
    return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
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
    if (release_result == MYLITE_OWNERLESS_LATCH_RELEASE_PENDING) {
        return operation_applied ? MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_APPLIED_RELEASE_PENDING
                                 : MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_RELEASE_PENDING;
    }
    return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR;
}

bool registry_size_fits(std::uint32_t slot_count) {
    const std::size_t max_slots =
        (std::numeric_limits<std::size_t>::max() - MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_HEADER_SIZE) /
        MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE;
    return static_cast<std::size_t>(slot_count) <= max_slots;
}

bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size) {
    if (mapping == nullptr || mapping_size < MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_HEADER_SIZE) {
        return false;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    const std::uint32_t count = slot_count(registry);
    const std::size_t registry_size = mylite_ownerless_page_pin_registry_size(count);
    return count > 0U &&
           load32(registry, k_header_slot_size_offset) ==
               MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE &&
           registry_size > 0U && mapping_size >= registry_size;
}

std::uint32_t slot_count(const unsigned char *registry) {
    return load32(registry, k_header_slot_count_offset);
}

unsigned char *slot_at(unsigned char *registry, std::uint32_t index) {
    return registry + MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(index) * MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE);
}

mylite_ownerless_latch *registry_latch(unsigned char *registry) {
    return reinterpret_cast<mylite_ownerless_latch *>(registry + k_header_latch_offset);
}

void clear_active_slot_locked(unsigned char *registry, unsigned char *slot) {
    store32(slot, k_slot_state_offset, k_slot_state_free);
    store32(slot, k_slot_owner_id_offset, 0U);
    store64(slot, k_slot_read_lsn_offset, 0U);
    store64(
        registry,
        k_header_generation_offset,
        load64(registry, k_header_generation_offset) + 1U
    );
    store64(
        registry,
        k_header_active_count_offset,
        load64(registry, k_header_active_count_offset) - 1U
    );
}

bool generation_can_advance(const unsigned char *registry, std::uint64_t amount) {
    return amount <=
           std::numeric_limits<std::uint64_t>::max() - load64(registry, k_header_generation_offset);
}

bool repair_registry_locked(unsigned char *registry, std::size_t mapping_size) {
    std::uint64_t generation = load64(registry, k_header_generation_offset);
    std::uint64_t active_count = 0U;
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            return false;
        }
        const std::uint32_t state = load32(slot, k_slot_state_offset);
        generation = std::max(generation, load64(slot, k_slot_generation_offset));
        if (state == k_slot_state_free) {
            continue;
        }
        if (state != MYLITE_OWNERLESS_PAGE_PIN_STATE_ACTIVE ||
            load64(slot, k_slot_generation_offset) == 0U ||
            load32(slot, k_slot_owner_id_offset) == 0U ||
            load64(slot, k_slot_read_lsn_offset) == 0U) {
            return false;
        }
        ++active_count;
    }
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    store64(registry, k_header_generation_offset, generation + 1U);
    store64(registry, k_header_active_count_offset, active_count);
    return true;
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
                slot + MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_PAGE_PIN_STATE_ACTIVE &&
            load32(slot, k_slot_owner_id_offset) == owner_id) {
            ++active_count;
        }
    }
    return active_count;
}

int snapshot_oldest_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
) {
    return snapshot_oldest_excluding_locked(
        registry,
        mapping_size,
        0U,
        0U,
        0U,
        out_active_count,
        out_oldest_read_lsn
    );
}

int snapshot_oldest_excluding_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t exclude_owner_id,
    std::uint32_t exclude_slot_index,
    std::uint64_t exclude_slot_generation,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
) {
    std::uint32_t active_count = 0U;
    std::uint64_t oldest_read_lsn = 0U;
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_PAGE_PIN_STATE_ACTIVE) {
            continue;
        }
        if (exclude_slot_generation != 0U && index == exclude_slot_index &&
            load32(slot, k_slot_owner_id_offset) == exclude_owner_id &&
            load64(slot, k_slot_generation_offset) == exclude_slot_generation) {
            continue;
        }
        const std::uint64_t read_lsn = load64(slot, k_slot_read_lsn_offset);
        if (read_lsn == 0U) {
            continue;
        }
        oldest_read_lsn = oldest_read_lsn == 0U ? read_lsn : std::min(oldest_read_lsn, read_lsn);
        ++active_count;
    }
    *out_active_count = active_count;
    *out_oldest_read_lsn = oldest_read_lsn;
    return MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK;
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

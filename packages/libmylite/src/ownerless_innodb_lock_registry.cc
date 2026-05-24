#include "ownerless_innodb_lock_registry.h"

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
constexpr std::size_t k_header_waiting_count_offset = 32;
constexpr std::size_t k_slot_generation_offset = 0;
constexpr std::size_t k_slot_owner_id_offset = 8;
constexpr std::size_t k_slot_state_offset = 12;
constexpr std::size_t k_slot_kind_offset = 16;
constexpr std::size_t k_slot_mode_offset = 20;
constexpr std::size_t k_slot_flags_offset = 24;
constexpr std::size_t k_slot_wait_word_offset = 28;
constexpr std::size_t k_slot_trx_id_offset = 32;
constexpr std::size_t k_slot_table_id_offset = 40;
constexpr std::size_t k_slot_index_id_offset = 48;
constexpr std::size_t k_slot_space_id_offset = 56;
constexpr std::size_t k_slot_page_no_offset = 60;
constexpr std::size_t k_slot_heap_no_offset = 64;
constexpr std::size_t k_slot_reference_count_offset = 68;
constexpr std::size_t k_slot_blocker_owner_id_offset = 72;
constexpr std::size_t k_slot_blocker_trx_id_offset = 80;
constexpr std::size_t k_slot_blocker_generation_offset = 88;
constexpr std::uint32_t k_slot_state_free = 0;

struct LockRequest {
    std::uint32_t owner_id = 0;
    std::uint64_t trx_id = 0;
    std::uint32_t kind = 0;
    std::uint32_t mode = 0;
    std::uint32_t flags = 0;
    std::uint64_t table_id = 0;
    std::uint64_t index_id = 0;
    std::uint32_t space_id = 0;
    std::uint32_t page_no = 0;
    std::uint32_t heap_no = 0;
};

struct LockSearchResult {
    unsigned char *own_slot = nullptr;
    unsigned char *own_waiting_slot = nullptr;
    unsigned char *conflicting_slot = nullptr;
    unsigned char *free_slot = nullptr;
};

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
int acquire_registry_latch(unsigned char *registry, std::chrono::steady_clock::time_point deadline);
void release_registry_latch(unsigned char *registry);
int acquire_lock_until(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::chrono::steady_clock::time_point deadline,
    bool increment_existing
);
int wait_until_lock_available(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::chrono::steady_clock::time_point deadline
);
int publish_wait_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    const unsigned char *blocker_slot
);
int publish_wait_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::uint32_t blocker_owner_id,
    std::uint64_t blocker_trx_id
);
int clear_wait_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint32_t *out_cleared_waits
);
bool wait_cycle_exists(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
);
int wait_for_slot_change(
    mylite_ownerless_wait_word *wait_word,
    std::uint32_t observed,
    std::chrono::steady_clock::time_point deadline
);
int release_lock_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request
);
int release_owner_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_locks
);
LockSearchResult find_lock_slot(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request
);
unsigned char *find_waiting_slot_by_transaction(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
);
void initialize_lock_slot(unsigned char *registry, unsigned char *slot, const LockRequest &request);
void initialize_waiting_slot(
    unsigned char *registry,
    unsigned char *slot,
    const LockRequest &request,
    std::uint32_t blocker_owner_id,
    std::uint64_t blocker_trx_id,
    std::uint64_t blocker_generation
);
int increment_lock_slot_reference_count(unsigned char *slot);
void clear_active_slot(unsigned char *registry, unsigned char *slot);
void clear_waiting_slot(unsigned char *registry, unsigned char *slot);
void clear_slot_fields(unsigned char *registry, unsigned char *slot);
bool same_lock(const unsigned char *slot, const LockRequest &request);
bool locks_conflict(const unsigned char *slot, const LockRequest &request);
bool table_locks_conflict(std::uint32_t requested_mode, std::uint32_t active_mode);
bool record_locks_conflict(std::uint32_t requested_mode, std::uint32_t requested_flags,
                           std::uint32_t active_mode, std::uint32_t active_flags);
bool table_mode_valid(std::uint32_t mode);
bool record_mode_valid(std::uint32_t mode);
bool record_flags_valid(std::uint32_t flags);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
bool registry_size_fits(std::uint32_t slot_count);
bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size);
std::uint32_t slot_count(const unsigned char *registry);
unsigned char *slot_at(unsigned char *registry, std::uint32_t index);
mylite_ownerless_wait_word *registry_latch(unsigned char *registry);
mylite_ownerless_wait_word *slot_wait_word(unsigned char *slot);
std::uint32_t load32(const unsigned char *base, std::size_t offset);
std::uint64_t load64(const unsigned char *base, std::size_t offset);
void store32(unsigned char *base, std::size_t offset, std::uint32_t value);
void store64(unsigned char *base, std::size_t offset, std::uint64_t value);
bool cas_wait_word(mylite_ownerless_wait_word *word, std::uint32_t *expected, std::uint32_t value);

} // namespace

std::size_t mylite_ownerless_innodb_lock_registry_size(std::uint32_t slot_count) {
    if (!registry_size_fits(slot_count)) {
        return 0U;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(slot_count) *
            MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE);
}

int mylite_ownerless_innodb_lock_registry_initialize(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_count
) {
    const std::size_t registry_size = mylite_ownerless_innodb_lock_registry_size(slot_count);
    if (mapping == nullptr || registry_size == 0U || mapping_size < registry_size ||
        slot_count == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    std::memset(registry, 0, registry_size);
    store32(registry, k_header_slot_count_offset, slot_count);
    store32(registry, k_header_slot_size_offset, MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE);
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

int mylite_ownerless_innodb_lock_registry_acquire_table(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || table_id == 0U || !table_mode_valid(mode)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE, mode, 0U,
        table_id, 0U, 0U, 0U, 0U
    };
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        wait_deadline(timeout_ms),
        true
    );
}

int mylite_ownerless_innodb_lock_registry_reserve_table(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || table_id == 0U || !table_mode_valid(mode)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE, mode, 0U,
        table_id, 0U, 0U, 0U, 0U
    };
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        wait_deadline(timeout_ms),
        false
    );
}

int mylite_ownerless_innodb_lock_registry_release_table(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || table_id == 0U || !table_mode_valid(mode)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE, mode, 0U,
        table_id, 0U, 0U, 0U, 0U
    };
    const int release_result = release_lock_locked(registry, mapping_size, request);
    release_registry_latch(registry);
    return release_result;
}

int mylite_ownerless_innodb_lock_registry_wait_for_table(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    std::uint32_t blocker_owner_id,
    std::uint64_t blocker_trx_id
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || table_id == 0U || !table_mode_valid(mode) ||
        blocker_owner_id == 0U || blocker_trx_id == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE, mode, 0U,
        table_id, 0U, 0U, 0U, 0U
    };
    const int wait_result =
        publish_wait_locked(registry, mapping_size, request, blocker_owner_id, blocker_trx_id);
    release_registry_latch(registry);
    return wait_result;
}

int mylite_ownerless_innodb_lock_registry_wait_until_table_available(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || table_id == 0U || !table_mode_valid(mode)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE, mode, 0U,
        table_id, 0U, 0U, 0U, 0U
    };
    return wait_until_lock_available(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        wait_deadline(timeout_ms)
    );
}

int mylite_ownerless_innodb_lock_registry_acquire_record(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || index_id == 0U || !record_mode_valid(mode) ||
        !record_flags_valid(flags)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD, mode, flags,
        0U, index_id, space_id, page_no, heap_no
    };
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        wait_deadline(timeout_ms),
        true
    );
}

int mylite_ownerless_innodb_lock_registry_reserve_record(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || index_id == 0U || !record_mode_valid(mode) ||
        !record_flags_valid(flags)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD, mode, flags,
        0U, index_id, space_id, page_no, heap_no
    };
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        wait_deadline(timeout_ms),
        false
    );
}

int mylite_ownerless_innodb_lock_registry_release_record(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || index_id == 0U || !record_mode_valid(mode) ||
        !record_flags_valid(flags)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD, mode, flags,
        0U, index_id, space_id, page_no, heap_no
    };
    const int release_result = release_lock_locked(registry, mapping_size, request);
    release_registry_latch(registry);
    return release_result;
}

int mylite_ownerless_innodb_lock_registry_wait_for_record(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    std::uint32_t blocker_owner_id,
    std::uint64_t blocker_trx_id
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || index_id == 0U || !record_mode_valid(mode) ||
        !record_flags_valid(flags) || blocker_owner_id == 0U ||
        blocker_trx_id == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD, mode, flags,
        0U, index_id, space_id, page_no, heap_no
    };
    const int wait_result =
        publish_wait_locked(registry, mapping_size, request, blocker_owner_id, blocker_trx_id);
    release_registry_latch(registry);
    return wait_result;
}

int mylite_ownerless_innodb_lock_registry_wait_until_record_available(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || index_id == 0U || !record_mode_valid(mode) ||
        !record_flags_valid(flags)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id, trx_id, MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD, mode, flags,
        0U, index_id, space_id, page_no, heap_no
    };
    return wait_until_lock_available(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        wait_deadline(timeout_ms)
    );
}

int mylite_ownerless_innodb_lock_registry_clear_wait(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint32_t *out_cleared_waits
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        trx_id == 0U || out_cleared_waits == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const int clear_result =
        clear_wait_locked(registry, mapping_size, owner_id, trx_id, out_cleared_waits);
    release_registry_latch(registry);
    return clear_result;
}

int mylite_ownerless_innodb_lock_registry_release_owner(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_locks
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        out_released_locks == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const int release_result =
        release_owner_locked(registry, mapping_size, owner_id, out_released_locks);
    release_registry_latch(registry);
    return release_result;
}

std::uint64_t mylite_ownerless_innodb_lock_registry_active_count(const void *mapping) {
    if (mapping == nullptr) {
        return 0U;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_active_count_offset);
}

std::uint64_t mylite_ownerless_innodb_lock_registry_waiting_count(const void *mapping) {
    if (mapping == nullptr) {
        return 0U;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_waiting_count_offset);
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
                return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
            }
            continue;
        }

        const unsigned timeout_ms = remaining_timeout_ms(deadline);
        if (timeout_ms == 0U) {
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT;
        }
        const int wait_result = mylite_ownerless_wait_for_change(latch, observed, timeout_ms);
        if (wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT) {
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT;
        }
        if (wait_result != MYLITE_OWNERLESS_WAIT_OK) {
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
        }
    }
}

void release_registry_latch(unsigned char *registry) {
    mylite_ownerless_wait_word *latch = registry_latch(registry);
    const std::uint32_t observed = mylite_ownerless_wait_load(latch);
    mylite_ownerless_wait_store(latch, observed + 1U);
    static_cast<void>(mylite_ownerless_wait_wake(latch));
}

int acquire_lock_until(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::chrono::steady_clock::time_point deadline,
    bool increment_existing
) {
    for (;;) {
        const int latch_result = acquire_registry_latch(registry, deadline);
        if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return latch_result;
        }

        const LockSearchResult search = find_lock_slot(registry, mapping_size, request);
        if (search.own_slot != nullptr) {
            std::uint32_t cleared_waits = 0;
            clear_wait_locked(
                registry,
                mapping_size,
                request.owner_id,
                request.trx_id,
                &cleared_waits
            );
            const int increment_result =
                increment_existing
                    ? increment_lock_slot_reference_count(search.own_slot)
                    : MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
            release_registry_latch(registry);
            return increment_result;
        }
        if (search.conflicting_slot == nullptr) {
            unsigned char *grant_slot =
                search.free_slot != nullptr ? search.free_slot : search.own_waiting_slot;
            if (grant_slot == nullptr) {
                release_registry_latch(registry);
                return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_FULL;
            }
            std::uint32_t cleared_waits = 0;
            clear_wait_locked(
                registry,
                mapping_size,
                request.owner_id,
                request.trx_id,
                &cleared_waits
            );
            initialize_lock_slot(registry, grant_slot, request);
            release_registry_latch(registry);
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
        }

        mylite_ownerless_wait_word *wait_word = slot_wait_word(search.conflicting_slot);
        const std::uint32_t observed = mylite_ownerless_wait_load(wait_word);
        const int publish_result =
            publish_wait_locked(registry, mapping_size, request, search.conflicting_slot);
        if (publish_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            release_registry_latch(registry);
            return publish_result;
        }
        release_registry_latch(registry);
        const int wait_result = wait_for_slot_change(wait_word, observed, deadline);
        if (wait_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            const int clear_latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
            if (clear_latch_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                std::uint32_t cleared_waits = 0;
                clear_wait_locked(
                    registry,
                    mapping_size,
                    request.owner_id,
                    request.trx_id,
                    &cleared_waits
                );
                release_registry_latch(registry);
            }
            return wait_result;
        }
    }
}

int wait_until_lock_available(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::chrono::steady_clock::time_point deadline
) {
    for (;;) {
        const int latch_result = acquire_registry_latch(registry, deadline);
        if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return latch_result;
        }

        const LockSearchResult search = find_lock_slot(registry, mapping_size, request);
        if (search.own_slot != nullptr || search.conflicting_slot == nullptr) {
            std::uint32_t cleared_waits = 0;
            clear_wait_locked(
                registry,
                mapping_size,
                request.owner_id,
                request.trx_id,
                &cleared_waits
            );
            release_registry_latch(registry);
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
        }

        mylite_ownerless_wait_word *wait_word = slot_wait_word(search.conflicting_slot);
        const std::uint32_t observed = mylite_ownerless_wait_load(wait_word);
        const int publish_result =
            publish_wait_locked(registry, mapping_size, request, search.conflicting_slot);
        if (publish_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            release_registry_latch(registry);
            return publish_result;
        }
        release_registry_latch(registry);

        const int wait_result = wait_for_slot_change(wait_word, observed, deadline);
        if (wait_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            const int clear_latch_result = acquire_registry_latch(registry, wait_deadline(5000U));
            if (clear_latch_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                std::uint32_t cleared_waits = 0;
                clear_wait_locked(
                    registry,
                    mapping_size,
                    request.owner_id,
                    request.trx_id,
                    &cleared_waits
                );
                release_registry_latch(registry);
            }
            return wait_result;
        }
    }
}

int publish_wait_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    const unsigned char *blocker_slot
) {
    return publish_wait_locked(
        registry,
        mapping_size,
        request,
        load32(blocker_slot, k_slot_owner_id_offset),
        load64(blocker_slot, k_slot_trx_id_offset)
    );
}

int publish_wait_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::uint32_t blocker_owner_id,
    std::uint64_t blocker_trx_id
) {
    const LockSearchResult search = find_lock_slot(registry, mapping_size, request);
    unsigned char *wait_slot = search.own_waiting_slot != nullptr ? search.own_waiting_slot
                                                                  : search.free_slot;
    if (wait_slot == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_FULL;
    }

    initialize_waiting_slot(
        registry,
        wait_slot,
        request,
        blocker_owner_id,
        blocker_trx_id,
        search.conflicting_slot != nullptr
            ? load64(search.conflicting_slot, k_slot_generation_offset)
            : 0U
    );
    if (wait_cycle_exists(registry, mapping_size, request.owner_id, request.trx_id)) {
        clear_waiting_slot(registry, wait_slot);
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

int clear_wait_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint32_t *out_cleared_waits
) {
    std::uint32_t cleared_waits = 0;
    const std::uint32_t count = slot_count(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING ||
            load32(slot, k_slot_owner_id_offset) != owner_id ||
            load64(slot, k_slot_trx_id_offset) != trx_id) {
            continue;
        }

        clear_waiting_slot(registry, slot);
        ++cleared_waits;
    }

    *out_cleared_waits = cleared_waits;
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

bool wait_cycle_exists(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
) {
    std::uint32_t next_owner_id = owner_id;
    std::uint64_t next_trx_id = trx_id;
    const std::uint32_t count = slot_count(registry);

    for (std::uint32_t depth = 0; depth < count; ++depth) {
        unsigned char *wait_slot =
            find_waiting_slot_by_transaction(registry, mapping_size, next_owner_id, next_trx_id);
        if (wait_slot == nullptr) {
            return false;
        }
        next_owner_id = load32(wait_slot, k_slot_blocker_owner_id_offset);
        next_trx_id = load64(wait_slot, k_slot_blocker_trx_id_offset);
        if (next_owner_id == owner_id && next_trx_id == trx_id) {
            return true;
        }
        if (next_owner_id == 0U || next_trx_id == 0U) {
            return false;
        }
    }
    return false;
}

int wait_for_slot_change(
    mylite_ownerless_wait_word *wait_word,
    std::uint32_t observed,
    std::chrono::steady_clock::time_point deadline
) {
    const unsigned timeout_ms = remaining_timeout_ms(deadline);
    if (timeout_ms == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT;
    }
    const int wait_result = mylite_ownerless_wait_for_change(wait_word, observed, timeout_ms);
    if (wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT;
    }
    return wait_result == MYLITE_OWNERLESS_WAIT_OK
               ? MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
               : MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
}

int release_lock_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request
) {
    const LockSearchResult search = find_lock_slot(registry, mapping_size, request);
    if (search.own_slot == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_NOT_FOUND;
    }

    const std::uint32_t reference_count = load32(search.own_slot, k_slot_reference_count_offset);
    if (reference_count > 1U) {
        store32(search.own_slot, k_slot_reference_count_offset, reference_count - 1U);
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
    }
    clear_active_slot(registry, search.own_slot);
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

int release_owner_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_locks
) {
    std::uint32_t released_locks = 0;
    const std::uint32_t count = slot_count(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == k_slot_state_free ||
            load32(slot, k_slot_owner_id_offset) != owner_id) {
            continue;
        }

        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE) {
            clear_active_slot(registry, slot);
        } else {
            clear_waiting_slot(registry, slot);
        }
        ++released_locks;
    }

    *out_released_locks = released_locks;
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

LockSearchResult find_lock_slot(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request
) {
    LockSearchResult result;
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == k_slot_state_free) {
            if (result.free_slot == nullptr) {
                result.free_slot = slot;
            }
            continue;
        }
        if (load32(slot, k_slot_state_offset) ==
            MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING) {
            if (load32(slot, k_slot_owner_id_offset) == request.owner_id &&
                load64(slot, k_slot_trx_id_offset) == request.trx_id) {
                result.own_waiting_slot = slot;
            }
            continue;
        }
        if (same_lock(slot, request)) {
            result.own_slot = slot;
            return result;
        }
        if (locks_conflict(slot, request) && result.conflicting_slot == nullptr) {
            result.conflicting_slot = slot;
        }
    }
    return result;
}

unsigned char *find_waiting_slot_by_transaction(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
) {
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) ==
                MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING &&
            load32(slot, k_slot_owner_id_offset) == owner_id &&
            load64(slot, k_slot_trx_id_offset) == trx_id) {
            return slot;
        }
    }
    return nullptr;
}

void initialize_lock_slot(
    unsigned char *registry,
    unsigned char *slot,
    const LockRequest &request
) {
    store64(slot, k_slot_generation_offset, load64(registry, k_header_generation_offset) + 1U);
    store32(slot, k_slot_owner_id_offset, request.owner_id);
    store32(slot, k_slot_kind_offset, request.kind);
    store32(slot, k_slot_mode_offset, request.mode);
    store32(slot, k_slot_flags_offset, request.flags);
    store64(slot, k_slot_trx_id_offset, request.trx_id);
    store64(slot, k_slot_table_id_offset, request.table_id);
    store64(slot, k_slot_index_id_offset, request.index_id);
    store32(slot, k_slot_space_id_offset, request.space_id);
    store32(slot, k_slot_page_no_offset, request.page_no);
    store32(slot, k_slot_heap_no_offset, request.heap_no);
    store32(slot, k_slot_reference_count_offset, 1U);
    store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE);
    store64(registry, k_header_generation_offset, load64(registry, k_header_generation_offset) + 1U);
    store64(registry, k_header_active_count_offset, load64(registry, k_header_active_count_offset) + 1U);
}

void initialize_waiting_slot(
    unsigned char *registry,
    unsigned char *slot,
    const LockRequest &request,
    std::uint32_t blocker_owner_id,
    std::uint64_t blocker_trx_id,
    std::uint64_t blocker_generation
) {
    const bool new_wait =
        load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING;

    store64(slot, k_slot_generation_offset, load64(registry, k_header_generation_offset) + 1U);
    store32(slot, k_slot_owner_id_offset, request.owner_id);
    store32(slot, k_slot_kind_offset, request.kind);
    store32(slot, k_slot_mode_offset, request.mode);
    store32(slot, k_slot_flags_offset, request.flags);
    store64(slot, k_slot_trx_id_offset, request.trx_id);
    store64(slot, k_slot_table_id_offset, request.table_id);
    store64(slot, k_slot_index_id_offset, request.index_id);
    store32(slot, k_slot_space_id_offset, request.space_id);
    store32(slot, k_slot_page_no_offset, request.page_no);
    store32(slot, k_slot_heap_no_offset, request.heap_no);
    store32(slot, k_slot_reference_count_offset, 0U);
    store32(slot, k_slot_blocker_owner_id_offset, blocker_owner_id);
    store64(slot, k_slot_blocker_trx_id_offset, blocker_trx_id);
    store64(slot, k_slot_blocker_generation_offset, blocker_generation);
    store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING);
    store64(registry, k_header_generation_offset, load64(registry, k_header_generation_offset) + 1U);
    if (new_wait) {
        store64(
            registry,
            k_header_waiting_count_offset,
            load64(registry, k_header_waiting_count_offset) + 1U
        );
    }
}

int increment_lock_slot_reference_count(unsigned char *slot) {
    const std::uint32_t reference_count = load32(slot, k_slot_reference_count_offset);
    if (reference_count == std::numeric_limits<std::uint32_t>::max()) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    store32(slot, k_slot_reference_count_offset, reference_count + 1U);
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

void clear_active_slot(unsigned char *registry, unsigned char *slot) {
    clear_slot_fields(registry, slot);
    store64(registry, k_header_active_count_offset, load64(registry, k_header_active_count_offset) - 1U);
}

void clear_waiting_slot(unsigned char *registry, unsigned char *slot) {
    clear_slot_fields(registry, slot);
    store64(
        registry,
        k_header_waiting_count_offset,
        load64(registry, k_header_waiting_count_offset) - 1U
    );
}

void clear_slot_fields(unsigned char *registry, unsigned char *slot) {
    mylite_ownerless_wait_word *wait_word = slot_wait_word(slot);
    const std::uint32_t wait_generation = mylite_ownerless_wait_load(wait_word);

    store32(slot, k_slot_state_offset, k_slot_state_free);
    store32(slot, k_slot_owner_id_offset, 0U);
    store32(slot, k_slot_kind_offset, 0U);
    store32(slot, k_slot_mode_offset, 0U);
    store32(slot, k_slot_flags_offset, 0U);
    store64(slot, k_slot_trx_id_offset, 0U);
    store64(slot, k_slot_table_id_offset, 0U);
    store64(slot, k_slot_index_id_offset, 0U);
    store32(slot, k_slot_space_id_offset, 0U);
    store32(slot, k_slot_page_no_offset, 0U);
    store32(slot, k_slot_heap_no_offset, 0U);
    store32(slot, k_slot_reference_count_offset, 0U);
    store32(slot, k_slot_blocker_owner_id_offset, 0U);
    store64(slot, k_slot_blocker_trx_id_offset, 0U);
    store64(slot, k_slot_blocker_generation_offset, 0U);
    store64(slot, k_slot_generation_offset, load64(registry, k_header_generation_offset) + 1U);
    mylite_ownerless_wait_store(wait_word, wait_generation + 1U);
    store64(registry, k_header_generation_offset, load64(registry, k_header_generation_offset) + 1U);
    static_cast<void>(mylite_ownerless_wait_wake(wait_word));
}

bool same_lock(const unsigned char *slot, const LockRequest &request) {
    if (load32(slot, k_slot_owner_id_offset) != request.owner_id ||
        load64(slot, k_slot_trx_id_offset) != request.trx_id ||
        load32(slot, k_slot_kind_offset) != request.kind ||
        load32(slot, k_slot_mode_offset) != request.mode ||
        load32(slot, k_slot_flags_offset) != request.flags) {
        return false;
    }
    if (request.kind == MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE) {
        return load64(slot, k_slot_table_id_offset) == request.table_id;
    }
    return load64(slot, k_slot_index_id_offset) == request.index_id &&
           load32(slot, k_slot_space_id_offset) == request.space_id &&
           load32(slot, k_slot_page_no_offset) == request.page_no &&
           load32(slot, k_slot_heap_no_offset) == request.heap_no;
}

bool locks_conflict(const unsigned char *slot, const LockRequest &request) {
    if (load32(slot, k_slot_kind_offset) != request.kind ||
        (load32(slot, k_slot_owner_id_offset) == request.owner_id &&
         load64(slot, k_slot_trx_id_offset) == request.trx_id)) {
        return false;
    }
    if (request.kind == MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE) {
        return load64(slot, k_slot_table_id_offset) == request.table_id &&
               table_locks_conflict(request.mode, load32(slot, k_slot_mode_offset));
    }
    if (load64(slot, k_slot_index_id_offset) != request.index_id ||
        load32(slot, k_slot_space_id_offset) != request.space_id ||
        load32(slot, k_slot_page_no_offset) != request.page_no) {
        return false;
    }

    return load32(slot, k_slot_heap_no_offset) == request.heap_no &&
           record_locks_conflict(
               request.mode,
               request.flags,
               load32(slot, k_slot_mode_offset),
               load32(slot, k_slot_flags_offset)
           );
}

bool table_locks_conflict(std::uint32_t requested_mode, std::uint32_t active_mode) {
    static constexpr bool compatibility[5][5] = {
        {true, true, true, false, true},
        {true, true, false, false, true},
        {true, false, true, false, false},
        {false, false, false, false, false},
        {true, true, false, false, false},
    };
    return !compatibility[requested_mode][active_mode];
}

bool record_locks_conflict(
    std::uint32_t requested_mode,
    std::uint32_t requested_flags,
    std::uint32_t active_mode,
    std::uint32_t active_flags
) {
    if (requested_mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_S &&
        active_mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_S) {
        return false;
    }

    const bool requested_gap = (requested_flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP) != 0U;
    const bool requested_insert =
        (requested_flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION) != 0U;
    const bool requested_supremum =
        (requested_flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_SUPREMUM) != 0U;
    const bool active_gap = (active_flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP) != 0U;
    const bool active_record_not_gap =
        (active_flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP) != 0U;
    const bool active_insert =
        (active_flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION) != 0U;

    if ((requested_supremum || requested_gap) && !requested_insert) {
        return false;
    }
    if (!requested_insert && active_gap) {
        return false;
    }
    if (requested_gap && active_record_not_gap) {
        return false;
    }
    if (active_insert) {
        return false;
    }
    return true;
}

bool table_mode_valid(std::uint32_t mode) {
    return mode <= MYLITE_OWNERLESS_INNODB_LOCK_MODE_AUTO_INC;
}

bool record_mode_valid(std::uint32_t mode) {
    return mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_S ||
           mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_X;
}

bool record_flags_valid(std::uint32_t flags) {
    constexpr std::uint32_t known_flags =
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP |
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP |
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION |
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_SUPREMUM;
    const bool gap = (flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP) != 0U;
    const bool record_not_gap =
        (flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP) != 0U;
    const bool insert_intention =
        (flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION) != 0U;
    const bool supremum = (flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_SUPREMUM) != 0U;
    return (flags & ~known_flags) == 0U && !(gap && record_not_gap) &&
           !(supremum && record_not_gap) && (!insert_intention || gap || supremum);
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
         MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE) /
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE;
    return static_cast<std::size_t>(slot_count) <= max_slots;
}

bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size) {
    if (mapping == nullptr || mapping_size < MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE) {
        return false;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    const std::uint32_t count = slot_count(registry);
    const std::size_t registry_size = mylite_ownerless_innodb_lock_registry_size(count);
    return count > 0U &&
           load32(registry, k_header_slot_size_offset) ==
               MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE &&
           registry_size > 0U && mapping_size >= registry_size;
}

std::uint32_t slot_count(const unsigned char *registry) {
    return load32(registry, k_header_slot_count_offset);
}

unsigned char *slot_at(unsigned char *registry, std::uint32_t index) {
    return registry + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(index) *
            MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE);
}

mylite_ownerless_wait_word *registry_latch(unsigned char *registry) {
    return reinterpret_cast<mylite_ownerless_wait_word *>(registry + k_header_latch_offset);
}

mylite_ownerless_wait_word *slot_wait_word(unsigned char *slot) {
    return reinterpret_cast<mylite_ownerless_wait_word *>(slot + k_slot_wait_word_offset);
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

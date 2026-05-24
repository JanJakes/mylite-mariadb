#include "ownerless_lock_table.h"

#include "ownerless_wait.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t k_header_entry_count_offset = 0;
constexpr std::size_t k_header_entry_size_offset = 4;
constexpr std::size_t k_header_generation_offset = 8;
constexpr std::size_t k_header_active_count_offset = 16;
constexpr std::size_t k_header_latch_offset = 24;
constexpr std::size_t k_entry_key_hash_offset = 0;
constexpr std::size_t k_entry_owner_id_offset = 8;
constexpr std::size_t k_entry_state_offset = 12;
constexpr std::size_t k_entry_mode_offset = 16;
constexpr std::size_t k_entry_wait_word_offset = 20;
constexpr std::size_t k_entry_generation_offset = 24;
constexpr std::size_t k_entry_reference_count_offset = 32;
constexpr std::uint32_t k_entry_state_free = 0;
constexpr std::uint32_t k_entry_state_active = 1;

struct LockSearchResult {
    unsigned char *own_entry = nullptr;
    unsigned char *incompatible_own_entry = nullptr;
    unsigned char *conflicting_entry = nullptr;
    unsigned char *free_entry = nullptr;
};

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
int acquire_table_latch(unsigned char *table, std::chrono::steady_clock::time_point deadline);
void release_table_latch(unsigned char *table);
int acquire_lock_until(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint32_t mode,
    std::chrono::steady_clock::time_point deadline
);
int wait_for_entry_change(
    mylite_ownerless_wait_word *wait_word,
    std::uint32_t observed,
    std::chrono::steady_clock::time_point deadline
);
int release_lock_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint32_t mode
);
LockSearchResult find_lock_entry(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint32_t mode
);
void initialize_lock_entry(
    unsigned char *table,
    unsigned char *entry,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint32_t mode
);
int increment_lock_entry_reference_count(unsigned char *entry);
void clear_lock_entry(unsigned char *table, unsigned char *entry);
bool lock_modes_conflict(std::uint32_t requested_mode, std::uint32_t active_mode);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
bool lock_table_size_fits(std::uint32_t entry_count);
bool mapping_can_hold_table(const void *mapping, std::size_t mapping_size);
std::uint32_t entry_count(const unsigned char *table);
unsigned char *entry_at(unsigned char *table, std::uint32_t index);
mylite_ownerless_wait_word *table_latch(unsigned char *table);
mylite_ownerless_wait_word *entry_wait_word(unsigned char *entry);
std::uint32_t load32(const unsigned char *base, std::size_t offset);
std::uint64_t load64(const unsigned char *base, std::size_t offset);
void store32(unsigned char *base, std::size_t offset, std::uint32_t value);
void store64(unsigned char *base, std::size_t offset, std::uint64_t value);
bool cas_wait_word(mylite_ownerless_wait_word *word, std::uint32_t *expected, std::uint32_t value);

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
    unsigned timeout_ms
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || key_hash == 0U || owner_id == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        key_hash,
        owner_id,
        MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
        wait_deadline(timeout_ms)
    );
}

int mylite_ownerless_lock_table_acquire_shared(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || key_hash == 0U || owner_id == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        key_hash,
        owner_id,
        MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
        wait_deadline(timeout_ms)
    );
}

int mylite_ownerless_lock_table_release_exclusive(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || key_hash == 0U || owner_id == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    auto *table = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_table_latch(table, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return latch_result;
    }
    const int release_result = release_lock_locked(
        table,
        mapping_size,
        key_hash,
        owner_id,
        MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE
    );
    release_table_latch(table);
    return release_result;
}

int mylite_ownerless_lock_table_release_shared(
    void *mapping,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id
) {
    if (!mapping_can_hold_table(mapping, mapping_size) || key_hash == 0U || owner_id == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }

    auto *table = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_table_latch(table, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return latch_result;
    }
    const int release_result = release_lock_locked(
        table,
        mapping_size,
        key_hash,
        owner_id,
        MYLITE_OWNERLESS_LOCK_TABLE_SHARED
    );
    release_table_latch(table);
    return release_result;
}

namespace {

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

int acquire_table_latch(
    unsigned char *table,
    std::chrono::steady_clock::time_point deadline
) {
    mylite_ownerless_wait_word *latch = table_latch(table);

    for (;;) {
        std::uint32_t observed = mylite_ownerless_wait_load(latch);
        if ((observed & 1U) == 0U) {
            std::uint32_t expected = observed;
            if (cas_wait_word(latch, &expected, observed + 1U)) {
                return MYLITE_OWNERLESS_LOCK_TABLE_OK;
            }
            continue;
        }

        const unsigned timeout_ms = remaining_timeout_ms(deadline);
        if (timeout_ms == 0U) {
            return MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT;
        }
        const int wait_result =
            mylite_ownerless_wait_for_change(latch, observed, timeout_ms);
        if (wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT) {
            return MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT;
        }
        if (wait_result != MYLITE_OWNERLESS_WAIT_OK) {
            return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
        }
    }
}

void release_table_latch(unsigned char *table) {
    mylite_ownerless_wait_word *latch = table_latch(table);
    const std::uint32_t observed = mylite_ownerless_wait_load(latch);
    mylite_ownerless_wait_store(latch, observed + 1U);
    static_cast<void>(mylite_ownerless_wait_wake(latch));
}

int acquire_lock_until(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint32_t mode,
    std::chrono::steady_clock::time_point deadline
) {
    for (;;) {
        const int latch_result = acquire_table_latch(table, deadline);
        if (latch_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
            return latch_result;
        }

        const LockSearchResult search = find_lock_entry(
            table,
            mapping_size,
            key_hash,
            owner_id,
            mode
        );
        if (search.incompatible_own_entry != nullptr) {
            release_table_latch(table);
            return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
        }
        if (search.own_entry != nullptr) {
            const int increment_result =
                increment_lock_entry_reference_count(search.own_entry);
            release_table_latch(table);
            return increment_result;
        }
        if (search.conflicting_entry == nullptr) {
            if (search.free_entry == nullptr) {
                release_table_latch(table);
                return MYLITE_OWNERLESS_LOCK_TABLE_FULL;
            }
            initialize_lock_entry(table, search.free_entry, key_hash, owner_id, mode);
            release_table_latch(table);
            return MYLITE_OWNERLESS_LOCK_TABLE_OK;
        }

        mylite_ownerless_wait_word *wait_word = entry_wait_word(search.conflicting_entry);
        const std::uint32_t observed = mylite_ownerless_wait_load(wait_word);
        release_table_latch(table);
        const int wait_result = wait_for_entry_change(wait_word, observed, deadline);
        if (wait_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
            return wait_result;
        }
    }
}

int wait_for_entry_change(
    mylite_ownerless_wait_word *wait_word,
    std::uint32_t observed,
    std::chrono::steady_clock::time_point deadline
) {
    const unsigned timeout_ms = remaining_timeout_ms(deadline);
    if (timeout_ms == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT;
    }
    const int wait_result = mylite_ownerless_wait_for_change(wait_word, observed, timeout_ms);
    if (wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT) {
        return MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT;
    }
    return wait_result == MYLITE_OWNERLESS_WAIT_OK ? MYLITE_OWNERLESS_LOCK_TABLE_OK
                                                   : MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
}

int release_lock_locked(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint32_t mode
) {
    const LockSearchResult search = find_lock_entry(
        table,
        mapping_size,
        key_hash,
        owner_id,
        mode
    );
    if (search.own_entry == nullptr) {
        return MYLITE_OWNERLESS_LOCK_TABLE_NOT_FOUND;
    }

    const std::uint32_t reference_count = load32(search.own_entry, k_entry_reference_count_offset);
    if (reference_count > 1U) {
        store32(search.own_entry, k_entry_reference_count_offset, reference_count - 1U);
        return MYLITE_OWNERLESS_LOCK_TABLE_OK;
    }
    clear_lock_entry(table, search.own_entry);
    return MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

LockSearchResult find_lock_entry(
    unsigned char *table,
    std::size_t mapping_size,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint32_t mode
) {
    LockSearchResult result;
    const std::uint32_t count = entry_count(table);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *entry = entry_at(table, index);
        if (static_cast<std::size_t>(entry + MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE - table) >
            mapping_size) {
            break;
        }
        if (load32(entry, k_entry_state_offset) == k_entry_state_free) {
            if (result.free_entry == nullptr) {
                result.free_entry = entry;
            }
            continue;
        }
        if (load64(entry, k_entry_key_hash_offset) != key_hash) {
            continue;
        }

        const std::uint32_t active_owner_id = load32(entry, k_entry_owner_id_offset);
        const std::uint32_t active_mode = load32(entry, k_entry_mode_offset);
        if (active_owner_id == owner_id) {
            if (active_mode == mode) {
                result.own_entry = entry;
                return result;
            }
            continue;
        }
        if (lock_modes_conflict(mode, active_mode) && result.conflicting_entry == nullptr) {
            result.conflicting_entry = entry;
        }
    }
    return result;
}

void initialize_lock_entry(
    unsigned char *table,
    unsigned char *entry,
    std::uint64_t key_hash,
    std::uint32_t owner_id,
    std::uint32_t mode
) {
    store64(entry, k_entry_key_hash_offset, key_hash);
    store32(entry, k_entry_owner_id_offset, owner_id);
    store32(entry, k_entry_mode_offset, mode);
    store32(entry, k_entry_reference_count_offset, 1U);
    store64(entry, k_entry_generation_offset, load64(table, k_header_generation_offset) + 1U);
    store32(entry, k_entry_state_offset, k_entry_state_active);
    store64(table, k_header_generation_offset, load64(table, k_header_generation_offset) + 1U);
    store64(table, k_header_active_count_offset, load64(table, k_header_active_count_offset) + 1U);
}

int increment_lock_entry_reference_count(unsigned char *entry) {
    const std::uint32_t reference_count = load32(entry, k_entry_reference_count_offset);
    if (reference_count == std::numeric_limits<std::uint32_t>::max()) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    store32(entry, k_entry_reference_count_offset, reference_count + 1U);
    return MYLITE_OWNERLESS_LOCK_TABLE_OK;
}

void clear_lock_entry(unsigned char *table, unsigned char *entry) {
    mylite_ownerless_wait_word *wait_word = entry_wait_word(entry);
    const std::uint32_t wait_generation = mylite_ownerless_wait_load(wait_word);

    store32(entry, k_entry_state_offset, k_entry_state_free);
    store32(entry, k_entry_owner_id_offset, 0U);
    store32(entry, k_entry_mode_offset, 0U);
    store32(entry, k_entry_reference_count_offset, 0U);
    store64(entry, k_entry_key_hash_offset, 0U);
    store64(entry, k_entry_generation_offset, load64(table, k_header_generation_offset) + 1U);
    mylite_ownerless_wait_store(wait_word, wait_generation + 1U);
    store64(table, k_header_generation_offset, load64(table, k_header_generation_offset) + 1U);
    store64(table, k_header_active_count_offset, load64(table, k_header_active_count_offset) - 1U);
    static_cast<void>(mylite_ownerless_wait_wake(wait_word));
}

bool lock_modes_conflict(std::uint32_t requested_mode, std::uint32_t active_mode) {
    return requested_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE ||
           active_mode == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE;
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

mylite_ownerless_wait_word *table_latch(unsigned char *table) {
    return reinterpret_cast<mylite_ownerless_wait_word *>(table + k_header_latch_offset);
}

mylite_ownerless_wait_word *entry_wait_word(unsigned char *entry) {
    return reinterpret_cast<mylite_ownerless_wait_word *>(entry + k_entry_wait_word_offset);
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

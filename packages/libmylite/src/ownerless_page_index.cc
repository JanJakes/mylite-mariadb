#include "ownerless_page_index.h"

#include "ownerless_process_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t k_header_latch_offset = 0;
constexpr std::size_t k_header_entry_count_offset = 32;
constexpr std::size_t k_header_entry_size_offset = 36;
constexpr std::size_t k_header_active_count_offset = 40;
constexpr std::size_t k_header_wal_scan_required_offset = 44;
constexpr std::size_t k_header_generation_offset = 48;
constexpr std::size_t k_header_wal_scan_entries_trusted_offset = 56;
constexpr std::size_t k_entry_state_offset = 0;
constexpr std::size_t k_entry_space_id_offset = 4;
constexpr std::size_t k_entry_page_no_offset = 8;
constexpr std::size_t k_entry_commit_lsn_offset = 16;
constexpr std::size_t k_entry_page_lsn_offset = 24;
constexpr std::size_t k_entry_record_offset_offset = 32;
constexpr std::size_t k_entry_generation_offset = 40;
constexpr std::uint32_t k_entry_state_empty = 0;
constexpr std::uint32_t k_entry_state_active = 1;
constexpr unsigned k_latch_timeout_ms = 5000;

std::uint32_t load32(const unsigned char *bytes, std::size_t offset);
std::uint64_t load64(const unsigned char *bytes, std::size_t offset);
void store32(unsigned char *bytes, std::size_t offset, std::uint32_t value);
void store64(unsigned char *bytes, std::size_t offset, std::uint64_t value);
bool index_required_size(std::uint32_t entry_count, std::size_t *out_size);
bool index_valid(unsigned char *index, std::size_t index_size);
mylite_ownerless_latch *index_latch(unsigned char *index);
unsigned char *entry_at(unsigned char *index, std::uint32_t entry_index);
std::uint32_t entry_count(unsigned char *index);
std::uint32_t hash_page(std::uint32_t space_id, std::uint32_t page_no);
bool wal_scan_required(unsigned char *index);
bool wal_scan_entries_trusted(unsigned char *index);
void require_wal_scan(unsigned char *index);
void require_wal_scan_for_overflow(unsigned char *index);
void trust_wal_scan_entries(unsigned char *index);
void clear_entries_locked(unsigned char *index, std::uint32_t count);
void invalidate_entries_after_owner_death(unsigned char *index, std::uint32_t count);
int publish_entry_locked(
    unsigned char *index,
    std::uint32_t count,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t commit_lsn,
    std::uint64_t page_lsn,
    std::uint64_t record_offset
);
bool entry_matches_page(unsigned char *entry, std::uint32_t space_id, std::uint32_t page_no);
bool version_is_at_least(unsigned char *entry, std::uint64_t commit_lsn, std::uint64_t page_lsn);
bool version_is_newer_or_same_offset(
    unsigned char *entry,
    std::uint64_t commit_lsn,
    std::uint64_t page_lsn,
    std::uint64_t record_offset
);
bool entry_visible_for_page(
    unsigned char *entry,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn
);
bool entry_is_better_version(
    unsigned char *candidate,
    std::uint64_t best_commit_lsn,
    std::uint64_t best_page_lsn,
    std::uint64_t best_record_offset
);
int latch_result_to_index_result(int result);
int finish_index_operation(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
);
bool generation_can_advance(const unsigned char *index, std::uint64_t amount);

} // namespace

int mylite_ownerless_page_index_initialize(
    void *index,
    std::size_t index_size,
    std::uint32_t entry_count
) {
    std::size_t required_size = 0;
    if (index == nullptr || entry_count == 0U ||
        !index_required_size(entry_count, &required_size) || index_size < required_size) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    auto *bytes = static_cast<unsigned char *>(index);
    std::memset(bytes, 0, required_size);
    mylite_ownerless_latch_initialize(index_latch(bytes));
    store32(bytes, k_header_entry_count_offset, entry_count);
    store32(bytes, k_header_entry_size_offset, MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    store64(bytes, k_header_generation_offset, 1U);
    return MYLITE_OWNERLESS_PAGE_INDEX_OK;
}

int mylite_ownerless_page_index_publish(
    void *index,
    std::size_t index_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t commit_lsn,
    std::uint64_t page_lsn,
    std::uint64_t record_offset
) {
    if (index == nullptr || owner_id == 0U || owner_generation == 0U || commit_lsn == 0U ||
        page_lsn == 0U || record_offset == 0U) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    auto *bytes = static_cast<unsigned char *>(index);
    if (!index_valid(bytes, index_size)) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    mylite_ownerless_latch *latch = index_latch(bytes);
    const int acquire_result = mylite_ownerless_latch_acquire(
        latch,
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        k_latch_timeout_ms
    );
    if (acquire_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_index_result(acquire_result);
    }

    const std::uint32_t count = entry_count(bytes);
    const int result = generation_can_advance(bytes, 1U) ? publish_entry_locked(
                                                               bytes,
                                                               count,
                                                               space_id,
                                                               page_no,
                                                               commit_lsn,
                                                               page_lsn,
                                                               record_offset
                                                           )
                                                         : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;

    return finish_index_operation(
        latch,
        owner_id,
        owner_generation,
        result,
        result == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
}

int mylite_ownerless_page_index_require_wal_scan(
    void *index,
    std::size_t index_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (index == nullptr || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    auto *bytes = static_cast<unsigned char *>(index);
    if (!index_valid(bytes, index_size)) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    mylite_ownerless_latch *latch = index_latch(bytes);
    const int acquire_result = mylite_ownerless_latch_acquire(
        latch,
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        k_latch_timeout_ms
    );
    if (acquire_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_index_result(acquire_result);
    }

    const int result = generation_can_advance(bytes, 1U) ? MYLITE_OWNERLESS_PAGE_INDEX_OK
                                                         : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    if (result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        require_wal_scan(bytes);
    }

    return finish_index_operation(
        latch,
        owner_id,
        owner_generation,
        result,
        result == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
}

int mylite_ownerless_page_index_clear(
    void *index,
    std::size_t index_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (index == nullptr || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    auto *bytes = static_cast<unsigned char *>(index);
    if (!index_valid(bytes, index_size)) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    mylite_ownerless_latch *latch = index_latch(bytes);
    const int acquire_result = mylite_ownerless_latch_acquire(
        latch,
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        k_latch_timeout_ms
    );
    if (acquire_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_index_result(acquire_result);
    }

    const int result = generation_can_advance(bytes, 1U) ? MYLITE_OWNERLESS_PAGE_INDEX_OK
                                                         : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    if (result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        clear_entries_locked(bytes, entry_count(bytes));
    }

    return finish_index_operation(
        latch,
        owner_id,
        owner_generation,
        result,
        result == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
}

int mylite_ownerless_page_index_replace(
    void *index,
    std::size_t index_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    const mylite_ownerless_page_index_record *records,
    std::size_t record_count
) {
    if (index == nullptr || owner_id == 0U || owner_generation == 0U ||
        (records == nullptr && record_count != 0U)) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }
    for (std::size_t record_index = 0; record_index < record_count; ++record_index) {
        const mylite_ownerless_page_index_record &record = records[record_index];
        if (record.commit_lsn == 0U || record.page_lsn == 0U || record.record_offset == 0U) {
            return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
        }
    }

    auto *bytes = static_cast<unsigned char *>(index);
    if (!index_valid(bytes, index_size)) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    mylite_ownerless_latch *latch = index_latch(bytes);
    const int acquire_result = mylite_ownerless_latch_acquire(
        latch,
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        k_latch_timeout_ms
    );
    if (acquire_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_index_result(acquire_result);
    }

    const std::uint32_t count = entry_count(bytes);
    const std::uint64_t required_generations =
        record_count >= std::numeric_limits<std::uint64_t>::max() - 2U
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(record_count) + 2U;
    int result = generation_can_advance(bytes, required_generations)
                     ? MYLITE_OWNERLESS_PAGE_INDEX_OK
                     : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    if (result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        clear_entries_locked(bytes, count);
    }
    for (std::size_t record_index = 0; record_index < record_count; ++record_index) {
        if (result != MYLITE_OWNERLESS_PAGE_INDEX_OK) {
            break;
        }
        const mylite_ownerless_page_index_record &record = records[record_index];
        result = publish_entry_locked(
            bytes,
            count,
            record.space_id,
            record.page_no,
            record.commit_lsn,
            record.page_lsn,
            record.record_offset
        );
        if (result != MYLITE_OWNERLESS_PAGE_INDEX_OK) {
            break;
        }
    }
    if (result != MYLITE_OWNERLESS_PAGE_INDEX_OK && generation_can_advance(bytes, 1U)) {
        require_wal_scan(bytes);
    } else if (wal_scan_required(bytes)) {
        trust_wal_scan_entries(bytes);
    }

    return finish_index_operation(latch, owner_id, owner_generation, result, true);
}

int mylite_ownerless_page_index_find(
    void *index,
    std::size_t index_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    std::uint64_t *out_record_offset,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    return mylite_ownerless_page_index_find_with_generation(
        index,
        index_size,
        owner_id,
        owner_generation,
        space_id,
        page_no,
        max_commit_lsn,
        out_record_offset,
        out_page_lsn,
        out_commit_lsn,
        nullptr
    );
}

int mylite_ownerless_page_index_find_with_generation(
    void *index,
    std::size_t index_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    std::uint64_t *out_record_offset,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint64_t *out_index_generation
) {
    if (index == nullptr || owner_id == 0U || owner_generation == 0U || max_commit_lsn == 0U ||
        out_record_offset == nullptr || out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    auto *bytes = static_cast<unsigned char *>(index);
    if (!index_valid(bytes, index_size)) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    mylite_ownerless_latch *latch = index_latch(bytes);
    const int acquire_result = mylite_ownerless_latch_acquire(
        latch,
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        k_latch_timeout_ms
    );
    if (acquire_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_index_result(acquire_result);
    }

    const std::uint32_t count = entry_count(bytes);
    const std::uint32_t first = hash_page(space_id, page_no) % count;
    const std::uint64_t index_generation = load64(bytes, k_header_generation_offset);
    if (out_index_generation != nullptr) {
        *out_index_generation = index_generation;
    }
    int result = MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND;
    unsigned char *best = nullptr;
    std::uint64_t best_commit_lsn = 0;
    std::uint64_t best_page_lsn = 0;
    std::uint64_t best_record_offset = 0;
    const bool scan_required = wal_scan_required(bytes);
    const bool scan_entries_trusted = wal_scan_entries_trusted(bytes);

    bool page_present = false;
    for (std::uint32_t probe = 0; probe < count; ++probe) {
        unsigned char *entry = entry_at(bytes, (first + probe) % count);
        const std::uint32_t state = load32(entry, k_entry_state_offset);
        if (state == k_entry_state_empty) {
            break;
        }
        if (state != k_entry_state_active) {
            result = MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
            break;
        }
        if (entry_matches_page(entry, space_id, page_no)) {
            page_present = true;
        }
        if (!entry_visible_for_page(entry, space_id, page_no, max_commit_lsn)) {
            continue;
        }
        if (best == nullptr ||
            entry_is_better_version(entry, best_commit_lsn, best_page_lsn, best_record_offset)) {
            best = entry;
            best_commit_lsn = load64(entry, k_entry_commit_lsn_offset);
            best_page_lsn = load64(entry, k_entry_page_lsn_offset);
            best_record_offset = load64(entry, k_entry_record_offset_offset);
        }
    }

    if (result != MYLITE_OWNERLESS_PAGE_INDEX_ERROR && scan_required && !scan_entries_trusted) {
        result = MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED;
    } else if (result != MYLITE_OWNERLESS_PAGE_INDEX_ERROR && best != nullptr) {
        *out_record_offset = load64(best, k_entry_record_offset_offset);
        *out_page_lsn = best_page_lsn;
        *out_commit_lsn = best_commit_lsn;
        result = MYLITE_OWNERLESS_PAGE_INDEX_OK;
    } else if (result != MYLITE_OWNERLESS_PAGE_INDEX_ERROR && (page_present || scan_required)) {
        result = MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED;
    }

    return finish_index_operation(latch, owner_id, owner_generation, result, false);
}

int mylite_ownerless_page_index_generation(
    void *index,
    std::size_t index_size,
    std::uint64_t *out_index_generation
) {
    if (index == nullptr || out_index_generation == nullptr) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    auto *bytes = static_cast<unsigned char *>(index);
    if (!index_valid(bytes, index_size)) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    *out_index_generation = load64(bytes, k_header_generation_offset);
    return MYLITE_OWNERLESS_PAGE_INDEX_OK;
}

int mylite_ownerless_page_index_finish_pending_release(
    void *index,
    std::size_t index_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (index == nullptr || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }
    auto *bytes = static_cast<unsigned char *>(index);
    if (!index_valid(bytes, index_size)) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }
    mylite_ownerless_latch *latch = index_latch(bytes);
    const int acquire_result = mylite_ownerless_latch_acquire(
        latch,
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        k_latch_timeout_ms
    );
    if (acquire_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_index_result(acquire_result);
    }
    return finish_index_operation(
        latch,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_PAGE_INDEX_OK,
        false
    );
}

int mylite_ownerless_page_index_recover_dead_latch(
    void *index,
    std::size_t index_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    const mylite_ownerless_process_registry_liveness_context *liveness
) {
    if (index == nullptr || owner_id == 0U || owner_generation == 0U || liveness == nullptr) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }
    auto *bytes = static_cast<unsigned char *>(index);
    if (!index_valid(bytes, index_size)) {
        return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    mylite_ownerless_latch_dead_owner dead_owner = {};
    const int latch_result = mylite_ownerless_latch_acquire_recoverable(
        index_latch(bytes),
        owner_id,
        owner_generation,
        mylite_ownerless_process_registry_latch_owner_is_alive,
        const_cast<mylite_ownerless_process_registry_liveness_context *>(liveness),
        k_latch_timeout_ms,
        &dead_owner
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED) {
        /*
         * Existing-entry publication changes three words. Their intermediate
         * combinations are not interpretable, but this index is rebuildable;
         * invalidate it and force the durable WAL scan instead of guessing.
         */
        invalidate_entries_after_owner_death(bytes, entry_count(bytes));
        if (mylite_ownerless_latch_mark_consistent(
                index_latch(bytes),
                owner_id,
                owner_generation
            ) != MYLITE_OWNERLESS_LATCH_OK) {
            return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
        }
    } else if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        return latch_result_to_index_result(latch_result);
    }
    return finish_index_operation(
        index_latch(bytes),
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_PAGE_INDEX_OK,
        latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED
    );
}

namespace {

std::uint32_t load32(const unsigned char *bytes, std::size_t offset) {
    const auto *value = reinterpret_cast<const std::uint32_t *>(bytes + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

std::uint64_t load64(const unsigned char *bytes, std::size_t offset) {
    const auto *value = reinterpret_cast<const std::uint64_t *>(bytes + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

void store32(unsigned char *bytes, std::size_t offset, std::uint32_t value) {
    auto *target = reinterpret_cast<std::uint32_t *>(bytes + offset);
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

void store64(unsigned char *bytes, std::size_t offset, std::uint64_t value) {
    auto *target = reinterpret_cast<std::uint64_t *>(bytes + offset);
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

bool index_required_size(std::uint32_t entry_count, std::size_t *out_size) {
    if (out_size == nullptr) {
        return false;
    }
    constexpr std::size_t max_size = std::numeric_limits<std::size_t>::max();
    const std::size_t requested_entries = entry_count;
    if (requested_entries > (max_size - MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE) /
                                MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE) {
        return false;
    }
    *out_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                (requested_entries * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    return true;
}

bool index_valid(unsigned char *index, std::size_t index_size) {
    if (index == nullptr || index_size < MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE) {
        return false;
    }
    const std::uint32_t count = entry_count(index);
    std::size_t required_size = 0;
    return count > 0U &&
           load32(index, k_header_entry_size_offset) == MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE &&
           index_required_size(count, &required_size) && index_size >= required_size;
}

mylite_ownerless_latch *index_latch(unsigned char *index) {
    return reinterpret_cast<mylite_ownerless_latch *>(index + k_header_latch_offset);
}

unsigned char *entry_at(unsigned char *index, std::uint32_t entry_index) {
    return index + MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
           (static_cast<std::size_t>(entry_index) * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
}

std::uint32_t entry_count(unsigned char *index) {
    return load32(index, k_header_entry_count_offset);
}

std::uint32_t hash_page(std::uint32_t space_id, std::uint32_t page_no) {
    std::uint32_t hash = 2166136261U;
    hash = (hash ^ space_id) * 16777619U;
    return (hash ^ page_no) * 16777619U;
}

bool wal_scan_required(unsigned char *index) {
    return load32(index, k_header_wal_scan_required_offset) != 0U;
}

bool wal_scan_entries_trusted(unsigned char *index) {
    return load32(index, k_header_wal_scan_entries_trusted_offset) != 0U;
}

void require_wal_scan(unsigned char *index) {
    store32(index, k_header_wal_scan_required_offset, 1U);
    store32(index, k_header_wal_scan_entries_trusted_offset, 0U);
    store64(index, k_header_generation_offset, load64(index, k_header_generation_offset) + 1U);
}

void require_wal_scan_for_overflow(unsigned char *index) {
    const bool entries_trusted = wal_scan_entries_trusted(index) || !wal_scan_required(index);
    store32(index, k_header_wal_scan_required_offset, 1U);
    store32(index, k_header_wal_scan_entries_trusted_offset, entries_trusted ? 1U : 0U);
    store64(index, k_header_generation_offset, load64(index, k_header_generation_offset) + 1U);
}

void trust_wal_scan_entries(unsigned char *index) {
    store32(index, k_header_wal_scan_entries_trusted_offset, 1U);
}

void clear_entries_locked(unsigned char *index, std::uint32_t count) {
    for (std::uint32_t entry_index = 0; entry_index < count; ++entry_index) {
        unsigned char *entry = entry_at(index, entry_index);
        store32(entry, k_entry_state_offset, k_entry_state_empty);
        std::memset(entry, 0, MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    }
    store32(index, k_header_active_count_offset, 0U);
    store32(index, k_header_wal_scan_required_offset, 0U);
    store32(index, k_header_wal_scan_entries_trusted_offset, 0U);
    store64(index, k_header_generation_offset, load64(index, k_header_generation_offset) + 1U);
}

void invalidate_entries_after_owner_death(unsigned char *index, std::uint32_t count) {
    std::uint64_t generation = load64(index, k_header_generation_offset);
    for (std::uint32_t entry_index = 0; entry_index < count; ++entry_index) {
        unsigned char *entry = entry_at(index, entry_index);
        generation = std::max(generation, load64(entry, k_entry_generation_offset));
        store32(entry, k_entry_state_offset, k_entry_state_empty);
    }
    for (std::uint32_t entry_index = 0; entry_index < count; ++entry_index) {
        std::memset(entry_at(index, entry_index), 0, MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    }
    store32(index, k_header_active_count_offset, 0U);
    store32(index, k_header_wal_scan_entries_trusted_offset, 0U);
    store32(index, k_header_wal_scan_required_offset, 1U);
    store64(
        index,
        k_header_generation_offset,
        generation == std::numeric_limits<std::uint64_t>::max() ? generation : generation + 1U
    );
}

int publish_entry_locked(
    unsigned char *index,
    std::uint32_t count,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t commit_lsn,
    std::uint64_t page_lsn,
    std::uint64_t record_offset
) {
    const std::uint32_t first = hash_page(space_id, page_no) % count;
    int result = MYLITE_OWNERLESS_PAGE_INDEX_FULL;
    for (std::uint32_t probe = 0; probe < count; ++probe) {
        unsigned char *entry = entry_at(index, (first + probe) % count);
        const std::uint32_t state = load32(entry, k_entry_state_offset);
        if (state == k_entry_state_empty) {
            store32(entry, k_entry_space_id_offset, space_id);
            store32(entry, k_entry_page_no_offset, page_no);
            store64(entry, k_entry_commit_lsn_offset, commit_lsn);
            store64(entry, k_entry_page_lsn_offset, page_lsn);
            store64(entry, k_entry_record_offset_offset, record_offset);
            store64(
                entry,
                k_entry_generation_offset,
                load64(index, k_header_generation_offset) + 1U
            );
            store32(
                index,
                k_header_active_count_offset,
                load32(index, k_header_active_count_offset) + 1U
            );
            store64(
                index,
                k_header_generation_offset,
                load64(index, k_header_generation_offset) + 1U
            );
            store32(entry, k_entry_state_offset, k_entry_state_active);
            result = MYLITE_OWNERLESS_PAGE_INDEX_OK;
            break;
        }
        if (state != k_entry_state_active) {
            result = MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
            break;
        }
        if (entry_matches_page(entry, space_id, page_no)) {
            if (version_is_newer_or_same_offset(entry, commit_lsn, page_lsn, record_offset)) {
                store64(entry, k_entry_commit_lsn_offset, commit_lsn);
                store64(entry, k_entry_page_lsn_offset, page_lsn);
                store64(entry, k_entry_record_offset_offset, record_offset);
                store64(
                    entry,
                    k_entry_generation_offset,
                    load64(index, k_header_generation_offset) + 1U
                );
                store64(
                    index,
                    k_header_generation_offset,
                    load64(index, k_header_generation_offset) + 1U
                );
            }
            result = MYLITE_OWNERLESS_PAGE_INDEX_OK;
            break;
        }
    }

    if (result == MYLITE_OWNERLESS_PAGE_INDEX_FULL) {
        require_wal_scan_for_overflow(index);
        result = MYLITE_OWNERLESS_PAGE_INDEX_OK;
    }
    return result;
}

bool entry_matches_page(unsigned char *entry, std::uint32_t space_id, std::uint32_t page_no) {
    return load32(entry, k_entry_space_id_offset) == space_id &&
           load32(entry, k_entry_page_no_offset) == page_no;
}

bool version_is_at_least(unsigned char *entry, std::uint64_t commit_lsn, std::uint64_t page_lsn) {
    const std::uint64_t existing_commit_lsn = load64(entry, k_entry_commit_lsn_offset);
    const std::uint64_t existing_page_lsn = load64(entry, k_entry_page_lsn_offset);
    return commit_lsn > existing_commit_lsn ||
           (commit_lsn == existing_commit_lsn && page_lsn > existing_page_lsn);
}

bool version_is_newer_or_same_offset(
    unsigned char *entry,
    std::uint64_t commit_lsn,
    std::uint64_t page_lsn,
    std::uint64_t record_offset
) {
    if (version_is_at_least(entry, commit_lsn, page_lsn)) {
        return true;
    }
    const std::uint64_t existing_commit_lsn = load64(entry, k_entry_commit_lsn_offset);
    const std::uint64_t existing_page_lsn = load64(entry, k_entry_page_lsn_offset);
    const std::uint64_t existing_record_offset = load64(entry, k_entry_record_offset_offset);
    return commit_lsn == existing_commit_lsn && page_lsn == existing_page_lsn &&
           record_offset > existing_record_offset;
}

bool entry_visible_for_page(
    unsigned char *entry,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn
) {
    return load32(entry, k_entry_space_id_offset) == space_id &&
           load32(entry, k_entry_page_no_offset) == page_no &&
           load64(entry, k_entry_commit_lsn_offset) <= max_commit_lsn;
}

bool entry_is_better_version(
    unsigned char *candidate,
    std::uint64_t best_commit_lsn,
    std::uint64_t best_page_lsn,
    std::uint64_t best_record_offset
) {
    const std::uint64_t candidate_commit_lsn = load64(candidate, k_entry_commit_lsn_offset);
    const std::uint64_t candidate_page_lsn = load64(candidate, k_entry_page_lsn_offset);
    const std::uint64_t candidate_record_offset = load64(candidate, k_entry_record_offset_offset);
    return candidate_commit_lsn > best_commit_lsn ||
           (candidate_commit_lsn == best_commit_lsn &&
            (candidate_page_lsn > best_page_lsn || (candidate_page_lsn == best_page_lsn &&
                                                    candidate_record_offset > best_record_offset)));
}

int latch_result_to_index_result(int result) {
    if (result == MYLITE_OWNERLESS_LATCH_TIMEOUT) {
        return MYLITE_OWNERLESS_PAGE_INDEX_TIMEOUT;
    }
    if (result == MYLITE_OWNERLESS_LATCH_OWNER_DEAD ||
        result == MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE) {
        return MYLITE_OWNERLESS_PAGE_INDEX_OWNER_DEAD;
    }
    return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
}

int finish_index_operation(
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
            k_latch_timeout_ms
        ) == MYLITE_OWNERLESS_LATCH_OK &&
        mylite_ownerless_latch_release(latch, owner_id, owner_generation) ==
            MYLITE_OWNERLESS_LATCH_OK) {
        return operation_result;
    }
    return operation_applied ? MYLITE_OWNERLESS_PAGE_INDEX_APPLIED_RELEASE_PENDING
                             : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
}

bool generation_can_advance(const unsigned char *index, std::uint64_t amount) {
    return amount <=
           std::numeric_limits<std::uint64_t>::max() - load64(index, k_header_generation_offset);
}

} // namespace

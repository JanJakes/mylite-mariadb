#include "ownerless_page_index.h"

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
void require_wal_scan(unsigned char *index);
bool entry_matches_version(
    unsigned char *entry,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t commit_lsn,
    std::uint64_t page_lsn
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
    std::uint64_t best_page_lsn
);
int latch_result_to_index_result(int result);

} // namespace

int mylite_ownerless_page_index_initialize(
    void *index,
    std::size_t index_size,
    std::uint32_t entry_count
) {
    std::size_t required_size = 0;
    if (index == nullptr || entry_count == 0U ||
        !index_required_size(entry_count, &required_size) ||
        index_size < required_size) {
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
    if (index == nullptr || owner_id == 0U || owner_generation == 0U ||
        commit_lsn == 0U || page_lsn == 0U || record_offset == 0U) {
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
    int result = MYLITE_OWNERLESS_PAGE_INDEX_OK;
    if (wal_scan_required(bytes)) {
        const int release_result =
            mylite_ownerless_latch_release(latch, owner_id, owner_generation);
        return release_result == MYLITE_OWNERLESS_LATCH_OK
                   ? result
                   : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

    result = MYLITE_OWNERLESS_PAGE_INDEX_FULL;
    for (std::uint32_t probe = 0; probe < count; ++probe) {
        unsigned char *entry = entry_at(bytes, (first + probe) % count);
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
                load64(bytes, k_header_generation_offset) + 1U
            );
            store32(entry, k_entry_state_offset, k_entry_state_active);
            store32(
                bytes,
                k_header_active_count_offset,
                load32(bytes, k_header_active_count_offset) + 1U
            );
            store64(
                bytes,
                k_header_generation_offset,
                load64(bytes, k_header_generation_offset) + 1U
            );
            result = MYLITE_OWNERLESS_PAGE_INDEX_OK;
            break;
        }
        if (state != k_entry_state_active) {
            result = MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
            break;
        }
        if (entry_matches_version(entry, space_id, page_no, commit_lsn, page_lsn)) {
            store64(entry, k_entry_record_offset_offset, record_offset);
            store64(
                entry,
                k_entry_generation_offset,
                load64(bytes, k_header_generation_offset) + 1U
            );
            store64(
                bytes,
                k_header_generation_offset,
                load64(bytes, k_header_generation_offset) + 1U
            );
            result = MYLITE_OWNERLESS_PAGE_INDEX_OK;
            break;
        }
    }

    if (result == MYLITE_OWNERLESS_PAGE_INDEX_FULL) {
        require_wal_scan(bytes);
        result = MYLITE_OWNERLESS_PAGE_INDEX_OK;
    }

    const int release_result = mylite_ownerless_latch_release(latch, owner_id, owner_generation);
    return release_result == MYLITE_OWNERLESS_LATCH_OK
               ? result
               : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
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

    require_wal_scan(bytes);

    const int release_result = mylite_ownerless_latch_release(latch, owner_id, owner_generation);
    return release_result == MYLITE_OWNERLESS_LATCH_OK
               ? MYLITE_OWNERLESS_PAGE_INDEX_OK
               : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
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
    if (index == nullptr || owner_id == 0U || owner_generation == 0U ||
        max_commit_lsn == 0U || out_record_offset == nullptr || out_page_lsn == nullptr ||
        out_commit_lsn == nullptr) {
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
    int result = MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND;
    unsigned char *best = nullptr;
    std::uint64_t best_commit_lsn = 0;
    std::uint64_t best_page_lsn = 0;
    if (wal_scan_required(bytes)) {
        const int release_result =
            mylite_ownerless_latch_release(latch, owner_id, owner_generation);
        return release_result == MYLITE_OWNERLESS_LATCH_OK
                   ? result
                   : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
    }

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
        if (!entry_visible_for_page(entry, space_id, page_no, max_commit_lsn)) {
            continue;
        }
        if (best == nullptr || entry_is_better_version(entry, best_commit_lsn, best_page_lsn)) {
            best = entry;
            best_commit_lsn = load64(entry, k_entry_commit_lsn_offset);
            best_page_lsn = load64(entry, k_entry_page_lsn_offset);
        }
    }

    if (result != MYLITE_OWNERLESS_PAGE_INDEX_ERROR && best != nullptr) {
        *out_record_offset = load64(best, k_entry_record_offset_offset);
        *out_page_lsn = best_page_lsn;
        *out_commit_lsn = best_commit_lsn;
        result = MYLITE_OWNERLESS_PAGE_INDEX_OK;
    }

    const int release_result = mylite_ownerless_latch_release(latch, owner_id, owner_generation);
    return release_result == MYLITE_OWNERLESS_LATCH_OK
               ? result
               : MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
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

void require_wal_scan(unsigned char *index) {
    store32(index, k_header_wal_scan_required_offset, 1U);
    store64(index, k_header_generation_offset, load64(index, k_header_generation_offset) + 1U);
}

bool entry_matches_version(
    unsigned char *entry,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t commit_lsn,
    std::uint64_t page_lsn
) {
    return load32(entry, k_entry_space_id_offset) == space_id &&
           load32(entry, k_entry_page_no_offset) == page_no &&
           load64(entry, k_entry_commit_lsn_offset) == commit_lsn &&
           load64(entry, k_entry_page_lsn_offset) == page_lsn;
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
    std::uint64_t best_page_lsn
) {
    const std::uint64_t candidate_commit_lsn = load64(candidate, k_entry_commit_lsn_offset);
    const std::uint64_t candidate_page_lsn = load64(candidate, k_entry_page_lsn_offset);
    return candidate_commit_lsn > best_commit_lsn ||
           (candidate_commit_lsn == best_commit_lsn && candidate_page_lsn > best_page_lsn);
}

int latch_result_to_index_result(int result) {
    (void)result;
    return MYLITE_OWNERLESS_PAGE_INDEX_ERROR;
}

} // namespace

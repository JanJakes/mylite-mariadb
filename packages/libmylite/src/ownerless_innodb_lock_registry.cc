#include "ownerless_innodb_lock_registry.h"

#include "ownerless_latch.h"
#include "ownerless_process_registry.h"
#include "ownerless_wait.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr unsigned k_registry_latch_timeout_ms = 5000U;
constexpr std::size_t k_header_slot_count_offset = 0;
constexpr std::size_t k_header_slot_size_offset = 4;
constexpr std::size_t k_header_generation_offset = 8;
constexpr std::size_t k_header_active_count_offset = 16;
constexpr std::size_t k_header_latch_offset = 24;
constexpr std::size_t k_header_waiting_count_offset = 64;
constexpr std::size_t k_header_occupied_limit_offset = 72;
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
constexpr std::size_t k_slot_finalize_space_id_offset = 96;
constexpr std::size_t k_slot_finalize_page_no_offset = 100;
constexpr std::size_t k_slot_finalize_heap_no_offset = 104;
constexpr std::size_t k_slot_finalize_flags_offset = 108;
constexpr std::size_t k_slot_finalize_generation_offset = 112;
constexpr std::uint32_t k_slot_state_free = 0;
constexpr std::uint64_t k_page_write_index_id = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t k_page_write_heap_no = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t k_page_write_global_gate_space_id =
    std::numeric_limits<std::uint32_t>::max() - 2U;
constexpr std::uint32_t k_page_write_global_gate_page_no =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t k_page_write_space_gate_page_no =
    std::numeric_limits<std::uint32_t>::max() - 1U;
constexpr std::uint32_t k_page_write_space_read_gate_page_no =
    std::numeric_limits<std::uint32_t>::max() - 2U;
constexpr std::uint32_t k_embedded_default_undo_space_count = 3U;

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
    unsigned char *queued_slot = nullptr;
    unsigned char *free_slot = nullptr;
};

struct WaitEdge {
    std::uint32_t owner_id = 0;
    std::uint64_t trx_id = 0;
    std::uint32_t blocker_owner_id = 0;
    std::uint64_t blocker_trx_id = 0;
};

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
int acquire_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
);
int registry_latch_owner_liveness_unknown(
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    void *ctx
);
int finish_registry_operation(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
);
int acquire_lock_until(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline,
    bool nonblocking_lock_wait,
    bool increment_existing,
    std::uint32_t *out_acquire_flags
);
int wait_until_lock_available(
    unsigned char *registry,
    std::size_t mapping_size,
    unsigned char *cycle_registry,
    std::size_t cycle_mapping_size,
    const LockRequest &request,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
);
bool lock_request_available(const LockSearchResult &search);
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
int clear_wait_after_wait_result_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    int wait_result,
    std::uint32_t *out_cleared_waits
);
bool wait_cycle_exists(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
);
int snapshot_wait_edges(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline,
    std::vector<WaitEdge> &edges,
    std::uint64_t *out_generation
);
int registry_generation_matches(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline,
    std::uint64_t expected_generation,
    bool *out_matches
);
bool wait_edge_cycle_exists(
    const std::vector<WaitEdge> &first_edges,
    const std::vector<WaitEdge> &second_edges,
    std::uint32_t owner_id,
    std::uint64_t trx_id
);
int combined_wait_cycle_exists(
    unsigned char *first_registry,
    std::size_t first_mapping_size,
    unsigned char *second_registry,
    std::size_t second_mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::chrono::steady_clock::time_point deadline,
    bool *out_cycle
);
void notify_transaction_slots_changed_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    const unsigned char *excluded_slot
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
int release_transaction_records_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::uint32_t *out_released_locks
);
int release_transaction_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint32_t *out_released_locks
);
int finalize_insert_reservation_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request
);
void clear_transaction_insert_reservations_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
);
int release_owner_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_locks
);
std::uint32_t owner_active_count_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id
);
bool owner_blocks_waiting_lock_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id
);
bool repair_registry_locked(unsigned char *registry, std::size_t mapping_size);
bool registry_generation_has_headroom(const unsigned char *registry);
std::uint32_t scan_slot_limit(const unsigned char *registry);
void extend_scan_slot_limit(unsigned char *registry, unsigned char *slot);
void shrink_scan_slot_limit(unsigned char *registry, std::size_t mapping_size, unsigned char *slot);
LockSearchResult find_lock_slot(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request
);
bool request_owner_blocks_waiting_lock(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    const unsigned char *waiting_slot
);
bool queued_waiter_precedes_request(const LockSearchResult &search);
unsigned char *find_waiting_slot_by_transaction(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
);
LockRequest lock_request_from_slot(const unsigned char *slot);
void initialize_lock_slot(unsigned char *registry, unsigned char *slot, const LockRequest &request);
void replace_active_lock_slot(
    unsigned char *registry,
    unsigned char *slot,
    const LockRequest &request
);
void clear_finalize_fields(unsigned char *slot);
void canonicalize_free_slot(unsigned char *slot);
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
void notify_slot_changed(unsigned char *slot);
bool same_lock(const unsigned char *slot, const LockRequest &request);
bool locks_conflict(const unsigned char *slot, const LockRequest &request);
bool insert_reservation_slot(const unsigned char *slot);
bool insert_reservation_request(const LockRequest &request);
bool finalize_insert_reservation_request(const LockRequest &request);
bool page_write_lock_slot(const unsigned char *slot);
bool page_write_lock_request(const LockRequest &request);
bool page_write_global_gate(std::uint32_t space_id, std::uint32_t page_no);
bool page_write_space_gate(std::uint32_t page_no);
bool page_write_space_read_gate(std::uint32_t page_no);
bool page_write_space_write(std::uint32_t page_no);
bool page_write_default_undo_space(std::uint32_t space_id);
bool page_write_locks_conflict(const unsigned char *slot, const LockRequest &request);
bool table_locks_conflict(std::uint32_t requested_mode, std::uint32_t active_mode);
bool record_locks_conflict(
    std::uint32_t requested_mode,
    std::uint32_t requested_flags,
    std::uint32_t active_mode,
    std::uint32_t active_flags
);
bool record_page_locks_conflict(
    std::uint32_t requested_mode,
    std::uint32_t requested_flags,
    std::uint32_t active_mode,
    std::uint32_t active_flags
);
bool table_mode_valid(std::uint32_t mode);
bool record_mode_valid(std::uint32_t mode);
bool record_flags_valid(std::uint32_t flags);
bool record_request_valid(std::uint32_t mode, std::uint32_t flags);
bool committed_slot_valid(const unsigned char *slot);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
bool registry_size_fits(std::uint32_t slot_count);
bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size);
std::uint32_t slot_count(const unsigned char *registry);
std::uint32_t slot_index(const unsigned char *registry, const unsigned char *slot);
unsigned char *slot_at(unsigned char *registry, std::uint32_t index);
mylite_ownerless_latch *registry_latch(unsigned char *registry);
mylite_ownerless_wait_word *slot_wait_word(unsigned char *slot);
std::uint32_t load32(const unsigned char *base, std::size_t offset);
std::uint64_t load64(const unsigned char *base, std::size_t offset);
void store32(unsigned char *base, std::size_t offset, std::uint32_t value);
void store64(unsigned char *base, std::size_t offset, std::uint64_t value);

} // namespace

std::size_t mylite_ownerless_innodb_lock_registry_size(std::uint32_t slot_count) {
    if (!registry_size_fits(slot_count)) {
        return 0U;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(slot_count) * MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE);
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
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || table_id == 0U || !table_mode_valid(mode)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE,
        mode,
        0U,
        table_id,
        0U,
        0U,
        0U,
        0U
    };
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        owner_generation,
        wait_deadline(timeout_ms),
        timeout_ms == 0U,
        true,
        nullptr
    );
}

int mylite_ownerless_innodb_lock_registry_reserve_table(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || table_id == 0U || !table_mode_valid(mode)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE,
        mode,
        0U,
        table_id,
        0U,
        0U,
        0U,
        0U
    };
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        owner_generation,
        wait_deadline(timeout_ms),
        timeout_ms == 0U,
        false,
        nullptr
    );
}

int mylite_ownerless_innodb_lock_registry_release_table(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || table_id == 0U || !table_mode_valid(mode)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result =
        acquire_registry_latch(registry, owner_id, owner_generation, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE,
        mode,
        0U,
        table_id,
        0U,
        0U,
        0U,
        0U
    };
    const int release_result = release_lock_locked(registry, mapping_size, request);
    if (release_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        clear_transaction_insert_reservations_locked(registry, mapping_size, owner_id, trx_id);
    }
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        release_result,
        release_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
}

int mylite_ownerless_innodb_lock_registry_wait_for_table(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    std::uint32_t blocker_owner_id,
    std::uint64_t blocker_trx_id
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || table_id == 0U || !table_mode_valid(mode) ||
        blocker_owner_id == 0U || blocker_trx_id == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result =
        acquire_registry_latch(registry, owner_id, owner_generation, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE,
        mode,
        0U,
        table_id,
        0U,
        0U,
        0U,
        0U
    };
    const int wait_result =
        publish_wait_locked(registry, mapping_size, request, blocker_owner_id, blocker_trx_id);
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        wait_result,
        wait_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
}

int mylite_ownerless_innodb_lock_registry_wait_until_table_available(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || table_id == 0U || !table_mode_valid(mode)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE,
        mode,
        0U,
        table_id,
        0U,
        0U,
        0U,
        0U
    };
    return wait_until_lock_available(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        nullptr,
        0U,
        request,
        owner_generation,
        wait_deadline(timeout_ms)
    );
}

int mylite_ownerless_innodb_lock_registry_acquire_record(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned timeout_ms
) {
    return mylite_ownerless_innodb_lock_registry_acquire_record_with_flags(
        mapping,
        mapping_size,
        owner_id,
        owner_generation,
        trx_id,
        index_id,
        space_id,
        page_no,
        heap_no,
        mode,
        flags,
        timeout_ms,
        nullptr
    );
}

int mylite_ownerless_innodb_lock_registry_acquire_record_with_flags(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned timeout_ms,
    std::uint32_t *out_acquire_flags
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || index_id == 0U ||
        !record_request_valid(mode, flags)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    if (out_acquire_flags != nullptr) {
        *out_acquire_flags = 0U;
    }

    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD,
        mode,
        flags,
        0U,
        index_id,
        space_id,
        page_no,
        heap_no
    };
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        owner_generation,
        wait_deadline(timeout_ms),
        timeout_ms == 0U,
        true,
        out_acquire_flags
    );
}

int mylite_ownerless_innodb_lock_registry_reserve_record(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
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
        owner_generation == 0U || trx_id == 0U || index_id == 0U ||
        !record_request_valid(mode, flags)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD,
        mode,
        flags,
        0U,
        index_id,
        space_id,
        page_no,
        heap_no
    };
    if (finalize_insert_reservation_request(request)) {
        auto *registry = static_cast<unsigned char *>(mapping);
        const int latch_result = acquire_registry_latch(
            registry,
            owner_id,
            owner_generation,
            wait_deadline(k_registry_latch_timeout_ms)
        );
        if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return latch_result;
        }
        const int finalize_result =
            finalize_insert_reservation_locked(registry, mapping_size, request);
        return finish_registry_operation(
            registry,
            owner_id,
            owner_generation,
            finalize_result,
            finalize_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
    }
    return acquire_lock_until(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        request,
        owner_generation,
        wait_deadline(timeout_ms),
        timeout_ms == 0U,
        false,
        nullptr
    );
}

int mylite_ownerless_innodb_lock_registry_release_record(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || index_id == 0U ||
        !record_request_valid(mode, flags)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result =
        acquire_registry_latch(registry, owner_id, owner_generation, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD,
        mode,
        flags,
        0U,
        index_id,
        space_id,
        page_no,
        heap_no
    };
    const int release_result = release_lock_locked(registry, mapping_size, request);
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        release_result,
        release_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
}

int mylite_ownerless_innodb_lock_registry_release_transaction_records(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    std::uint32_t *out_released_locks
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || index_id == 0U ||
        !record_request_valid(mode, flags) || out_released_locks == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result =
        acquire_registry_latch(registry, owner_id, owner_generation, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD,
        mode,
        flags,
        0U,
        index_id,
        0U,
        0U,
        heap_no
    };
    const int release_result =
        release_transaction_records_locked(registry, mapping_size, request, out_released_locks);
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        release_result,
        release_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
}

int mylite_ownerless_innodb_lock_registry_release_transaction(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint32_t *out_released_locks
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || out_released_locks == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result =
        acquire_registry_latch(registry, owner_id, owner_generation, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const int release_result =
        release_transaction_locked(registry, mapping_size, owner_id, trx_id, out_released_locks);
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        release_result,
        release_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK && *out_released_locks > 0U
    );
}

int mylite_ownerless_innodb_lock_registry_wait_for_record(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
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
        owner_generation == 0U || trx_id == 0U || index_id == 0U ||
        !record_request_valid(mode, flags) || blocker_owner_id == 0U || blocker_trx_id == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result =
        acquire_registry_latch(registry, owner_id, owner_generation, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD,
        mode,
        flags,
        0U,
        index_id,
        space_id,
        page_no,
        heap_no
    };
    const int wait_result =
        publish_wait_locked(registry, mapping_size, request, blocker_owner_id, blocker_trx_id);
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        wait_result,
        wait_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
}

int mylite_ownerless_innodb_lock_registry_wait_until_record_available(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
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
        owner_generation == 0U || trx_id == 0U || index_id == 0U ||
        !record_request_valid(mode, flags)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD,
        mode,
        flags,
        0U,
        index_id,
        space_id,
        page_no,
        heap_no
    };
    return wait_until_lock_available(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        nullptr,
        0U,
        request,
        owner_generation,
        wait_deadline(timeout_ms)
    );
}

int mylite_ownerless_innodb_lock_registry_wait_until_record_available_with_cycle_registry(
    void *mapping,
    std::size_t mapping_size,
    void *cycle_mapping,
    std::size_t cycle_mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned timeout_ms
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) ||
        !mapping_can_hold_registry(cycle_mapping, cycle_mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || index_id == 0U ||
        !record_request_valid(mode, flags)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD,
        mode,
        flags,
        0U,
        index_id,
        space_id,
        page_no,
        heap_no
    };
    return wait_until_lock_available(
        static_cast<unsigned char *>(mapping),
        mapping_size,
        static_cast<unsigned char *>(cycle_mapping),
        cycle_mapping_size,
        request,
        owner_generation,
        wait_deadline(timeout_ms)
    );
}

int mylite_ownerless_innodb_lock_registry_record_available_now(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    int *out_available
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || index_id == 0U ||
        !record_request_valid(mode, flags) || out_available == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    *out_available = 0;
    auto *registry = static_cast<unsigned char *>(mapping);
    if (mylite_ownerless_innodb_lock_registry_waiting_count(mapping) != 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
    }
    if (mylite_ownerless_innodb_lock_registry_active_count(mapping) == 0U) {
        *out_available = 1;
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
    }

    const std::uint32_t count = scan_slot_limit(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        const unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE) {
            continue;
        }
        if (load32(slot, k_slot_owner_id_offset) == owner_id) {
            *out_available = 1;
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
        }
        break;
    }

    const int latch_result =
        acquire_registry_latch(registry, owner_id, owner_generation, wait_deadline(0U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }

    const LockRequest request{
        owner_id,
        trx_id,
        MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD,
        mode,
        flags,
        0U,
        index_id,
        space_id,
        page_no,
        heap_no
    };
    const LockSearchResult search = find_lock_slot(registry, mapping_size, request);
    *out_available = lock_request_available(search) ? 1 : 0;
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_innodb_lock_registry_record_active_now(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    int *out_active
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || index_id == 0U || !record_request_valid(mode, flags) ||
        out_active == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    *out_active = 0;
    auto *registry = static_cast<unsigned char *>(mapping);
    if (mylite_ownerless_innodb_lock_registry_active_count(mapping) == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
    }

    const int latch_result =
        acquire_registry_latch(registry, owner_id, owner_generation, wait_deadline(0U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }

    const std::uint32_t count = scan_slot_limit(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        const unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE ||
            load32(slot, k_slot_kind_offset) != MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD ||
            load32(slot, k_slot_mode_offset) != mode ||
            load32(slot, k_slot_flags_offset) != flags ||
            load64(slot, k_slot_index_id_offset) != index_id ||
            load32(slot, k_slot_space_id_offset) != space_id ||
            load32(slot, k_slot_page_no_offset) != page_no ||
            load32(slot, k_slot_heap_no_offset) != heap_no) {
            continue;
        }
        *out_active = 1;
        break;
    }

    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_innodb_lock_registry_clear_wait(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::uint32_t *out_cleared_waits
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || trx_id == 0U || out_cleared_waits == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result =
        acquire_registry_latch(registry, owner_id, owner_generation, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const int clear_result =
        clear_wait_locked(registry, mapping_size, owner_id, trx_id, out_cleared_waits);
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        clear_result,
        clear_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK && *out_cleared_waits > 0U
    );
}

int mylite_ownerless_innodb_lock_registry_release_owner(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t latch_owner_id,
    std::uint64_t latch_owner_generation,
    std::uint32_t *out_released_locks
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        latch_owner_id == 0U || latch_owner_generation == 0U || out_released_locks == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        latch_owner_id,
        latch_owner_generation,
        wait_deadline(5000U)
    );
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    const int release_result =
        release_owner_locked(registry, mapping_size, owner_id, out_released_locks);
    return finish_registry_operation(
        registry,
        latch_owner_id,
        latch_owner_generation,
        release_result,
        release_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK && *out_released_locks > 0U
    );
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

int mylite_ownerless_innodb_lock_registry_owner_active_count(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t latch_owner_id,
    std::uint64_t latch_owner_generation,
    std::uint32_t *out_active_count
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        latch_owner_id == 0U || latch_owner_generation == 0U || out_active_count == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        latch_owner_id,
        latch_owner_generation,
        wait_deadline(5000U)
    );
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    *out_active_count = owner_active_count_locked(registry, mapping_size, owner_id);
    return finish_registry_operation(
        registry,
        latch_owner_id,
        latch_owner_generation,
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_innodb_lock_registry_owner_blocks_waiting_lock(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t latch_owner_id,
    std::uint64_t latch_owner_generation,
    int *out_blocks_waiting_lock
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        latch_owner_id == 0U || latch_owner_generation == 0U ||
        out_blocks_waiting_lock == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    *out_blocks_waiting_lock = 0;
    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        latch_owner_id,
        latch_owner_generation,
        wait_deadline(5000U)
    );
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    *out_blocks_waiting_lock =
        owner_blocks_waiting_lock_locked(registry, mapping_size, owner_id) ? 1 : 0;
    return finish_registry_operation(
        registry,
        latch_owner_id,
        latch_owner_generation,
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_innodb_lock_registry_finish_pending_release(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        owner_id,
        owner_generation,
        wait_deadline(k_registry_latch_timeout_ms)
    );
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_innodb_lock_registry_recover_dead_latch(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    const mylite_ownerless_process_registry_liveness_context *liveness
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || liveness == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
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
    bool owner_coordination_required = false;
    if (latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED) {
        if (!repair_registry_locked(registry, mapping_size)) {
            static_cast<void>(mylite_ownerless_latch_mark_not_recoverable(
                registry_latch(registry),
                owner_id,
                owner_generation
            ));
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OWNER_DEAD;
        }
        owner_coordination_required =
            owner_active_count_locked(registry, mapping_size, dead_owner.owner_id) != 0U;
        if (mylite_ownerless_latch_mark_consistent(
                registry_latch(registry),
                owner_id,
                owner_generation
            ) != MYLITE_OWNERLESS_LATCH_OK) {
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
        }
    } else if (latch_result != MYLITE_OWNERLESS_LATCH_OK) {
        if (latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT) {
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT;
        }
        return latch_result == MYLITE_OWNERLESS_LATCH_OWNER_DEAD ||
                       latch_result == MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE
                   ? MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OWNER_DEAD
                   : MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    /* Active locks and reservations remain transaction/native-recovery evidence. */
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        owner_coordination_required ? MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OWNER_DEAD
                                    : MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
        latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED
    );
}

namespace {

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

int acquire_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
) {
    const unsigned timeout_ms = remaining_timeout_ms(deadline);
    const int latch_result = mylite_ownerless_latch_acquire(
        registry_latch(registry),
        owner_id,
        owner_generation,
        registry_latch_owner_liveness_unknown,
        nullptr,
        timeout_ms
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
    }
    if (latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT) {
        if (timeout_ms >= k_registry_latch_timeout_ms - 1U) {
            std::uint32_t state = 0U;
            std::uint32_t latch_owner_id = 0U;
            std::uint64_t latch_owner_generation = 0U;
            std::uint32_t waiter_count = 0U;
            std::uint64_t owner_death_count = 0U;
            if (mylite_ownerless_latch_snapshot(
                    registry_latch(registry),
                    &state,
                    &latch_owner_id,
                    &latch_owner_generation,
                    &waiter_count,
                    &owner_death_count
                ) == MYLITE_OWNERLESS_LATCH_OK &&
                state != MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED && latch_owner_id != 0U &&
                (latch_owner_id != owner_id || latch_owner_generation != owner_generation)) {
                return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OWNER_DEAD;
            }
        }
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT;
    }
    return latch_result == MYLITE_OWNERLESS_LATCH_OWNER_DEAD ||
                   latch_result == MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE
               ? MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OWNER_DEAD
               : MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
}

int registry_latch_owner_liveness_unknown(
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    void *ctx
) {
    (void)owner_id;
    (void)owner_generation;
    (void)ctx;
    /* Only recover_dead_latch() has enough directory context to prove death. */
    return -1;
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
    return operation_applied ? MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_APPLIED_RELEASE_PENDING
                             : MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
}

int acquire_lock_until(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline,
    bool nonblocking_lock_wait,
    bool increment_existing,
    std::uint32_t *out_acquire_flags
) {
    bool waited = false;

    for (;;) {
        const auto latch_deadline =
            nonblocking_lock_wait ? wait_deadline(k_registry_latch_timeout_ms) : deadline;
        const int latch_result =
            acquire_registry_latch(registry, request.owner_id, owner_generation, latch_deadline);
        if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return latch_result;
        }
        if (!registry_generation_has_headroom(registry)) {
            return finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR,
                false
            );
        }

        const LockSearchResult search = find_lock_slot(registry, mapping_size, request);
        if (search.own_slot != nullptr) {
            std::uint32_t cleared_waits = 0;
            const int clear_result = clear_wait_locked(
                registry,
                mapping_size,
                request.owner_id,
                request.trx_id,
                &cleared_waits
            );
            int increment_result = clear_result;
            if (clear_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                increment_result = increment_existing
                                       ? increment_lock_slot_reference_count(search.own_slot)
                                       : MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
            }
            if (increment_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK &&
                out_acquire_flags != nullptr && waited) {
                *out_acquire_flags |= MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ACQUIRE_WAITED;
            }
            return finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                increment_result,
                cleared_waits > 0U || (increment_existing &&
                                       increment_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK)
            );
        }
        if (search.conflicting_slot == nullptr && !queued_waiter_precedes_request(search)) {
            unsigned char *grant_slot =
                search.free_slot != nullptr ? search.free_slot : search.own_waiting_slot;
            if (grant_slot == nullptr) {
                return finish_registry_operation(
                    registry,
                    request.owner_id,
                    owner_generation,
                    MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_FULL,
                    false
                );
            }
            std::uint32_t cleared_waits = 0;
            const int clear_result = clear_wait_locked(
                registry,
                mapping_size,
                request.owner_id,
                request.trx_id,
                &cleared_waits
            );
            if (clear_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                return finish_registry_operation(
                    registry,
                    request.owner_id,
                    owner_generation,
                    clear_result,
                    false
                );
            }
            initialize_lock_slot(registry, grant_slot, request);
            if (out_acquire_flags != nullptr && waited) {
                *out_acquire_flags |= MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ACQUIRE_WAITED;
            }
            return finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
                true
            );
        }

        if (nonblocking_lock_wait) {
            return finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT,
                false
            );
        }

        if (search.conflicting_slot == nullptr) {
            mylite_ownerless_wait_word *wait_word = slot_wait_word(search.queued_slot);
            const std::uint32_t observed = mylite_ownerless_wait_load(wait_word);
            const int release_result = finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
                false
            );
            if (release_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                return release_result;
            }
            const int wait_result = wait_for_slot_change(wait_word, observed, deadline);
            if (wait_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                return wait_result;
            }
            waited = true;
            continue;
        }

        mylite_ownerless_wait_word *wait_word = slot_wait_word(search.conflicting_slot);
        const std::uint32_t observed = mylite_ownerless_wait_load(wait_word);
        const int publish_result =
            publish_wait_locked(registry, mapping_size, request, search.conflicting_slot);
        if (publish_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                publish_result,
                false
            );
        }
        const int publish_release_result = finish_registry_operation(
            registry,
            request.owner_id,
            owner_generation,
            MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
            true
        );
        if (publish_release_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return publish_release_result;
        }
        const int wait_result = wait_for_slot_change(wait_word, observed, deadline);
        if (wait_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            const int clear_latch_result = acquire_registry_latch(
                registry,
                request.owner_id,
                owner_generation,
                wait_deadline(5000U)
            );
            if (clear_latch_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                std::uint32_t cleared_waits = 0;
                const LockSearchResult final_search =
                    find_lock_slot(registry, mapping_size, request);
                if (wait_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT &&
                    lock_request_available(final_search)) {
                    const int clear_result = clear_wait_locked(
                        registry,
                        mapping_size,
                        request.owner_id,
                        request.trx_id,
                        &cleared_waits
                    );
                    return finish_registry_operation(
                        registry,
                        request.owner_id,
                        owner_generation,
                        clear_result,
                        cleared_waits > 0U
                    );
                }
                const int final_wait_result = clear_wait_after_wait_result_locked(
                    registry,
                    mapping_size,
                    request,
                    wait_result,
                    &cleared_waits
                );
                return finish_registry_operation(
                    registry,
                    request.owner_id,
                    owner_generation,
                    final_wait_result,
                    cleared_waits > 0U
                );
            }
            return wait_result;
        }
        waited = true;
    }
}

int wait_until_lock_available(
    unsigned char *registry,
    std::size_t mapping_size,
    unsigned char *cycle_registry,
    std::size_t cycle_mapping_size,
    const LockRequest &request,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
) {
    for (;;) {
        const int latch_result =
            acquire_registry_latch(registry, request.owner_id, owner_generation, deadline);
        if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return latch_result;
        }
        if (!registry_generation_has_headroom(registry)) {
            return finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR,
                false
            );
        }

        const LockSearchResult search = find_lock_slot(registry, mapping_size, request);
        if (lock_request_available(search)) {
            if (search.own_waiting_slot != nullptr) {
                return finish_registry_operation(
                    registry,
                    request.owner_id,
                    owner_generation,
                    MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
                    false
                );
            }
            std::uint32_t cleared_waits = 0;
            const int clear_result = clear_wait_locked(
                registry,
                mapping_size,
                request.owner_id,
                request.trx_id,
                &cleared_waits
            );
            return finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                clear_result,
                cleared_waits > 0U
            );
        }

        if (search.conflicting_slot == nullptr) {
            mylite_ownerless_wait_word *wait_word = slot_wait_word(search.queued_slot);
            const std::uint32_t observed = mylite_ownerless_wait_load(wait_word);
            const int release_result = finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
                false
            );
            if (release_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                return release_result;
            }
            const int wait_result = wait_for_slot_change(wait_word, observed, deadline);
            if (wait_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                return wait_result;
            }
            continue;
        }

        mylite_ownerless_wait_word *wait_word = slot_wait_word(search.conflicting_slot);
        const std::uint32_t observed = mylite_ownerless_wait_load(wait_word);
        const int publish_result =
            publish_wait_locked(registry, mapping_size, request, search.conflicting_slot);
        if (publish_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return finish_registry_operation(
                registry,
                request.owner_id,
                owner_generation,
                publish_result,
                false
            );
        }
        const int publish_release_result = finish_registry_operation(
            registry,
            request.owner_id,
            owner_generation,
            MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
            true
        );
        if (publish_release_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return publish_release_result;
        }

        if (cycle_registry != nullptr) {
            bool cycle = false;
            const int cycle_result = combined_wait_cycle_exists(
                registry,
                mapping_size,
                cycle_registry,
                cycle_mapping_size,
                request.owner_id,
                owner_generation,
                request.trx_id,
                deadline,
                &cycle
            );
            if (cycle_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK || cycle) {
                const int clear_latch_result = acquire_registry_latch(
                    registry,
                    request.owner_id,
                    owner_generation,
                    wait_deadline(k_registry_latch_timeout_ms)
                );
                if (clear_latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                    return clear_latch_result;
                }
                std::uint32_t cleared_waits = 0;
                const int clear_result = clear_wait_locked(
                    registry,
                    mapping_size,
                    request.owner_id,
                    request.trx_id,
                    &cleared_waits
                );
                int result = clear_result;
                if (clear_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                    result = cycle ? MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK : cycle_result;
                }
                return finish_registry_operation(
                    registry,
                    request.owner_id,
                    owner_generation,
                    result,
                    cleared_waits > 0U
                );
            }
        }

        const int wait_result = wait_for_slot_change(wait_word, observed, deadline);
        if (wait_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            const int clear_latch_result = acquire_registry_latch(
                registry,
                request.owner_id,
                owner_generation,
                wait_deadline(5000U)
            );
            if (clear_latch_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                std::uint32_t cleared_waits = 0;
                const LockSearchResult final_search =
                    find_lock_slot(registry, mapping_size, request);
                if (wait_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT &&
                    lock_request_available(final_search)) {
                    if (final_search.own_waiting_slot != nullptr) {
                        return finish_registry_operation(
                            registry,
                            request.owner_id,
                            owner_generation,
                            MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
                            false
                        );
                    }
                    const int clear_result = clear_wait_locked(
                        registry,
                        mapping_size,
                        request.owner_id,
                        request.trx_id,
                        &cleared_waits
                    );
                    return finish_registry_operation(
                        registry,
                        request.owner_id,
                        owner_generation,
                        clear_result,
                        cleared_waits > 0U
                    );
                }
                const int final_wait_result = clear_wait_after_wait_result_locked(
                    registry,
                    mapping_size,
                    request,
                    wait_result,
                    &cleared_waits
                );
                return finish_registry_operation(
                    registry,
                    request.owner_id,
                    owner_generation,
                    final_wait_result,
                    cleared_waits > 0U
                );
            }
            return wait_result;
        }
    }
}

bool lock_request_available(const LockSearchResult &search) {
    return search.own_slot != nullptr ||
           (search.conflicting_slot == nullptr && !queued_waiter_precedes_request(search));
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
    if (!registry_generation_has_headroom(registry)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    const LockSearchResult search = find_lock_slot(registry, mapping_size, request);
    unsigned char *wait_slot =
        search.own_waiting_slot != nullptr ? search.own_waiting_slot : search.free_slot;
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
    notify_transaction_slots_changed_locked(
        registry,
        mapping_size,
        request.owner_id,
        request.trx_id,
        wait_slot
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
    *out_cleared_waits = 0U;
    if (!registry_generation_has_headroom(registry)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    const std::uint32_t count = scan_slot_limit(registry);

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

int clear_wait_after_wait_result_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    int wait_result,
    std::uint32_t *out_cleared_waits
) {
    int final_wait_result = wait_result;
    if (wait_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT &&
        wait_cycle_exists(registry, mapping_size, request.owner_id, request.trx_id)) {
        final_wait_result = MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK;
    }

    const int clear_result = clear_wait_locked(
        registry,
        mapping_size,
        request.owner_id,
        request.trx_id,
        out_cleared_waits
    );
    return clear_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK ? final_wait_result
                                                                    : clear_result;
}

bool wait_cycle_exists(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
) {
    std::uint32_t next_owner_id = owner_id;
    std::uint64_t next_trx_id = trx_id;
    const std::uint32_t count = scan_slot_limit(registry);

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

int snapshot_wait_edges(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline,
    std::vector<WaitEdge> &edges,
    std::uint64_t *out_generation
) {
    for (;;) {
        edges.clear();
        const int latch_result =
            acquire_registry_latch(registry, owner_id, owner_generation, deadline);
        if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return latch_result;
        }

        const std::uint64_t waiting_count = load64(registry, k_header_waiting_count_offset);
        if (waiting_count > slot_count(registry)) {
            return finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR,
                false
            );
        }
        if (waiting_count > edges.capacity()) {
            const int release_result = finish_registry_operation(
                registry,
                owner_id,
                owner_generation,
                MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
                false
            );
            if (release_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
                return release_result;
            }
            edges.reserve(static_cast<std::size_t>(waiting_count));
            continue;
        }

        const std::uint32_t count = scan_slot_limit(registry);
        for (std::uint32_t index = 0; index < count; ++index) {
            const unsigned char *slot = slot_at(registry, index);
            if (static_cast<std::size_t>(
                    slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
                ) > mapping_size) {
                break;
            }
            if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING) {
                continue;
            }
            edges.push_back(
                WaitEdge{
                    load32(slot, k_slot_owner_id_offset),
                    load64(slot, k_slot_trx_id_offset),
                    load32(slot, k_slot_blocker_owner_id_offset),
                    load64(slot, k_slot_blocker_trx_id_offset),
                }
            );
        }
        if (out_generation != nullptr) {
            *out_generation = load64(registry, k_header_generation_offset);
        }
        return finish_registry_operation(
            registry,
            owner_id,
            owner_generation,
            MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
            false
        );
    }
}

int registry_generation_matches(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline,
    std::uint64_t expected_generation,
    bool *out_matches
) {
    const int latch_result = acquire_registry_latch(registry, owner_id, owner_generation, deadline);
    if (latch_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return latch_result;
    }
    *out_matches = load64(registry, k_header_generation_offset) == expected_generation;
    return finish_registry_operation(
        registry,
        owner_id,
        owner_generation,
        MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK,
        false
    );
}

const WaitEdge *find_wait_edge(
    const std::vector<WaitEdge> &edges,
    std::uint32_t owner_id,
    std::uint64_t trx_id
) {
    for (const WaitEdge &edge : edges) {
        if (edge.owner_id == owner_id && edge.trx_id == trx_id) {
            return &edge;
        }
    }
    return nullptr;
}

bool wait_edge_cycle_exists(
    const std::vector<WaitEdge> &first_edges,
    const std::vector<WaitEdge> &second_edges,
    std::uint32_t owner_id,
    std::uint64_t trx_id
) {
    std::uint32_t next_owner_id = owner_id;
    std::uint64_t next_trx_id = trx_id;
    const std::size_t count = first_edges.size() + second_edges.size();

    for (std::size_t depth = 0; depth < count; ++depth) {
        const WaitEdge *edge = find_wait_edge(first_edges, next_owner_id, next_trx_id);
        if (edge == nullptr) {
            edge = find_wait_edge(second_edges, next_owner_id, next_trx_id);
        }
        if (edge == nullptr) {
            return false;
        }
        next_owner_id = edge->blocker_owner_id;
        next_trx_id = edge->blocker_trx_id;
        if (next_owner_id == owner_id && next_trx_id == trx_id) {
            return true;
        }
        if (next_owner_id == 0U || next_trx_id == 0U) {
            return false;
        }
    }
    return false;
}

int combined_wait_cycle_exists(
    unsigned char *first_registry,
    std::size_t first_mapping_size,
    unsigned char *second_registry,
    std::size_t second_mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t trx_id,
    std::chrono::steady_clock::time_point deadline,
    bool *out_cycle
) {
    std::vector<WaitEdge> first_edges;
    std::vector<WaitEdge> second_edges;
    *out_cycle = false;

    for (;;) {
        std::uint64_t first_generation = 0U;
        int result = snapshot_wait_edges(
            first_registry,
            first_mapping_size,
            owner_id,
            owner_generation,
            deadline,
            first_edges,
            &first_generation
        );
        if (result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return result;
        }
        result = snapshot_wait_edges(
            second_registry,
            second_mapping_size,
            owner_id,
            owner_generation,
            deadline,
            second_edges,
            nullptr
        );
        if (result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return result;
        }
        bool first_unchanged = false;
        result = registry_generation_matches(
            first_registry,
            owner_id,
            owner_generation,
            deadline,
            first_generation,
            &first_unchanged
        );
        if (result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            return result;
        }
        if (first_unchanged) {
            *out_cycle = wait_edge_cycle_exists(first_edges, second_edges, owner_id, trx_id);
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
        }
        if (remaining_timeout_ms(deadline) == 0U) {
            return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT;
        }
    }
}

void notify_transaction_slots_changed_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    const unsigned char *excluded_slot
) {
    const std::uint32_t count = scan_slot_limit(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (slot == excluded_slot || load32(slot, k_slot_state_offset) == k_slot_state_free ||
            load32(slot, k_slot_owner_id_offset) != owner_id ||
            load64(slot, k_slot_trx_id_offset) != trx_id) {
            continue;
        }

        notify_slot_changed(slot);
    }
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
    return wait_result == MYLITE_OWNERLESS_WAIT_OK ? MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
                                                   : MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
}

int release_lock_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request
) {
    if (!registry_generation_has_headroom(registry)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    unsigned char *own_slot = nullptr;
    if (insert_reservation_request(request)) {
        const std::uint32_t count = scan_slot_limit(registry);
        for (std::uint32_t index = 0; index < count; ++index) {
            unsigned char *slot = slot_at(registry, index);
            if (static_cast<std::size_t>(
                    slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
                ) > mapping_size) {
                break;
            }
            if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE &&
                same_lock(slot, request) &&
                (own_slot == nullptr || load64(slot, k_slot_generation_offset) >
                                            load64(own_slot, k_slot_generation_offset))) {
                own_slot = slot;
            }
        }
    } else {
        own_slot = find_lock_slot(registry, mapping_size, request).own_slot;
    }
    if (own_slot == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_NOT_FOUND;
    }

    const std::uint32_t reference_count = load32(own_slot, k_slot_reference_count_offset);
    if (reference_count > 1U) {
        store32(own_slot, k_slot_reference_count_offset, reference_count - 1U);
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
    }
    clear_active_slot(registry, own_slot);
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

int release_transaction_records_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    std::uint32_t *out_released_locks
) {
    std::uint32_t released_locks = 0;
    *out_released_locks = 0U;
    if (!registry_generation_has_headroom(registry)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    const std::uint32_t count = scan_slot_limit(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE ||
            load32(slot, k_slot_owner_id_offset) != request.owner_id ||
            load64(slot, k_slot_trx_id_offset) != request.trx_id ||
            load32(slot, k_slot_kind_offset) != MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD ||
            load32(slot, k_slot_mode_offset) != request.mode ||
            load32(slot, k_slot_flags_offset) != request.flags ||
            load64(slot, k_slot_index_id_offset) != request.index_id ||
            load32(slot, k_slot_heap_no_offset) != request.heap_no) {
            continue;
        }

        clear_active_slot(registry, slot);
        ++released_locks;
    }

    *out_released_locks = released_locks;
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

int release_transaction_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id,
    std::uint32_t *out_released_locks
) {
    std::uint32_t released_locks = 0U;
    *out_released_locks = 0U;
    if (!registry_generation_has_headroom(registry)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    const std::uint32_t count = scan_slot_limit(registry);

    for (std::uint32_t index = 0U; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        const std::uint32_t state = load32(slot, k_slot_state_offset);
        if (state == k_slot_state_free || load32(slot, k_slot_owner_id_offset) != owner_id ||
            load64(slot, k_slot_trx_id_offset) != trx_id ||
            load32(slot, k_slot_kind_offset) != MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD) {
            continue;
        }

        if (state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE) {
            clear_active_slot(registry, slot);
        } else {
            clear_waiting_slot(registry, slot);
        }
        ++released_locks;
    }

    *out_released_locks = released_locks;
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

int finalize_insert_reservation_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request
) {
    if (!registry_generation_has_headroom(registry)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    LockRequest final_request = request;
    final_request.flags &= ~MYLITE_OWNERLESS_INNODB_RECORD_LOCK_FINALIZE_INSERT_RESERVATION;

    unsigned char *reservation_slot = nullptr;
    const std::uint32_t count = scan_slot_limit(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE &&
            load32(slot, k_slot_owner_id_offset) == request.owner_id &&
            load64(slot, k_slot_trx_id_offset) == request.trx_id &&
            load64(slot, k_slot_index_id_offset) == request.index_id &&
            insert_reservation_slot(slot) &&
            (reservation_slot == nullptr ||
             load64(slot, k_slot_generation_offset) >
                 load64(reservation_slot, k_slot_generation_offset))) {
            reservation_slot = slot;
        }
    }
    if (reservation_slot == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }

    const LockSearchResult search = find_lock_slot(registry, mapping_size, final_request);
    if (search.conflicting_slot != nullptr) {
        /* The page can be reused before another transaction has finished
           undoing a failed insert at the same heap number. The inserted row
           must be rolled back and retried, but the registry remains sound. */
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT;
    }
    if (search.own_slot != nullptr) {
        clear_active_slot(registry, reservation_slot);
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
    }

    replace_active_lock_slot(registry, reservation_slot, final_request);
    return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK;
}

void clear_transaction_insert_reservations_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
) {
    const std::uint32_t count = scan_slot_limit(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE &&
            load32(slot, k_slot_owner_id_offset) == owner_id &&
            load64(slot, k_slot_trx_id_offset) == trx_id && insert_reservation_slot(slot)) {
            clear_active_slot(registry, slot);
        }
    }
}

int release_owner_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_released_locks
) {
    std::uint32_t released_locks = 0;
    *out_released_locks = 0U;
    if (!registry_generation_has_headroom(registry)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR;
    }
    const std::uint32_t count = scan_slot_limit(registry);

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

std::uint32_t owner_active_count_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id
) {
    std::uint32_t active_count = 0U;
    const std::uint32_t count = scan_slot_limit(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != k_slot_state_free &&
            load32(slot, k_slot_owner_id_offset) == owner_id) {
            ++active_count;
        }
    }
    return active_count;
}

bool owner_blocks_waiting_lock_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id
) {
    const std::uint32_t count = scan_slot_limit(registry);
    for (std::uint32_t waiting_index = 0; waiting_index < count; ++waiting_index) {
        unsigned char *waiting_slot = slot_at(registry, waiting_index);
        if (static_cast<std::size_t>(
                waiting_slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(waiting_slot, k_slot_state_offset) !=
                MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING ||
            load32(waiting_slot, k_slot_owner_id_offset) == owner_id ||
            load32(waiting_slot, k_slot_blocker_owner_id_offset) != owner_id) {
            continue;
        }

        const LockRequest waiting_request = lock_request_from_slot(waiting_slot);
        for (std::uint32_t active_index = 0; active_index < count; ++active_index) {
            unsigned char *active_slot = slot_at(registry, active_index);
            if (static_cast<std::size_t>(
                    active_slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
                ) > mapping_size) {
                break;
            }
            if (load32(active_slot, k_slot_state_offset) !=
                    MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE ||
                load32(active_slot, k_slot_owner_id_offset) != owner_id) {
                continue;
            }
            if (locks_conflict(active_slot, waiting_request)) {
                return true;
            }
        }
    }
    return false;
}

bool repair_registry_locked(unsigned char *registry, std::size_t mapping_size) {
    const std::uint32_t count = slot_count(registry);

    /* Complete conversions whose target was durably published before the latch owner died. */
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            return false;
        }
        const std::uint32_t state = load32(slot, k_slot_state_offset);
        if (state == k_slot_state_free || state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE ||
            state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING) {
            continue;
        }
        if (state != MYLITE_OWNERLESS_INNODB_LOCK_STATE_FINALIZING_INSERT ||
            load32(slot, k_slot_owner_id_offset) == 0U ||
            load64(slot, k_slot_trx_id_offset) == 0U ||
            load32(slot, k_slot_kind_offset) != MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD ||
            load32(slot, k_slot_mode_offset) != MYLITE_OWNERLESS_INNODB_LOCK_MODE_X ||
            load64(slot, k_slot_table_id_offset) != 0U ||
            load64(slot, k_slot_index_id_offset) == 0U ||
            load32(slot, k_slot_finalize_flags_offset) !=
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP ||
            load64(slot, k_slot_finalize_generation_offset) == 0U) {
            return false;
        }

        store32(slot, k_slot_space_id_offset, load32(slot, k_slot_finalize_space_id_offset));
        store32(slot, k_slot_page_no_offset, load32(slot, k_slot_finalize_page_no_offset));
        store32(slot, k_slot_heap_no_offset, load32(slot, k_slot_finalize_heap_no_offset));
        store32(slot, k_slot_flags_offset, load32(slot, k_slot_finalize_flags_offset));
        store64(slot, k_slot_generation_offset, load64(slot, k_slot_finalize_generation_offset));
        store32(slot, k_slot_reference_count_offset, 1U);
        store32(slot, k_slot_blocker_owner_id_offset, 0U);
        store64(slot, k_slot_blocker_trx_id_offset, 0U);
        store64(slot, k_slot_blocker_generation_offset, 0U);
        store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE);
        clear_finalize_fields(slot);
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        const std::uint32_t state = load32(slot, k_slot_state_offset);
        if (state == k_slot_state_free) {
            continue;
        }
        if (!committed_slot_valid(slot)) {
            return false;
        }
        const LockRequest request = lock_request_from_slot(slot);
        for (std::uint32_t prior_index = 0; prior_index < index; ++prior_index) {
            unsigned char *prior = slot_at(registry, prior_index);
            const std::uint32_t prior_state = load32(prior, k_slot_state_offset);
            if (prior_state == k_slot_state_free) {
                continue;
            }
            if (load64(prior, k_slot_generation_offset) == load64(slot, k_slot_generation_offset)) {
                return false;
            }
            if (state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE &&
                prior_state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE &&
                same_lock(prior, request) && !insert_reservation_slot(slot)) {
                return false;
            }
            if (state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING &&
                prior_state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING &&
                load32(prior, k_slot_owner_id_offset) == request.owner_id &&
                load64(prior, k_slot_trx_id_offset) == request.trx_id) {
                return false;
            }
        }
    }

    /* Rebuild each wait edge from committed active locks. Unblocked waiters retry. */
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *waiting_slot = slot_at(registry, index);
        if (load32(waiting_slot, k_slot_state_offset) !=
            MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING) {
            continue;
        }
        const LockRequest request = lock_request_from_slot(waiting_slot);
        unsigned char *blocker = nullptr;
        for (std::uint32_t active_index = 0; active_index < count; ++active_index) {
            unsigned char *active_slot = slot_at(registry, active_index);
            if (load32(active_slot, k_slot_state_offset) ==
                    MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE &&
                locks_conflict(active_slot, request)) {
                blocker = active_slot;
                break;
            }
        }
        if (blocker == nullptr) {
            canonicalize_free_slot(waiting_slot);
            notify_slot_changed(waiting_slot);
            continue;
        }
        store32(
            waiting_slot,
            k_slot_blocker_owner_id_offset,
            load32(blocker, k_slot_owner_id_offset)
        );
        store64(waiting_slot, k_slot_blocker_trx_id_offset, load64(blocker, k_slot_trx_id_offset));
        store64(
            waiting_slot,
            k_slot_blocker_generation_offset,
            load64(blocker, k_slot_generation_offset)
        );
    }

    std::uint64_t generation = load64(registry, k_header_generation_offset);
    std::uint64_t active_count = 0U;
    std::uint64_t waiting_count = 0U;
    std::uint32_t occupied_limit = 0U;
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        const std::uint32_t state = load32(slot, k_slot_state_offset);
        if (state == k_slot_state_free) {
            canonicalize_free_slot(slot);
            continue;
        }
        clear_finalize_fields(slot);
        generation = std::max(generation, load64(slot, k_slot_generation_offset));
        occupied_limit = index + 1U;
        if (state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE) {
            ++active_count;
        } else {
            ++waiting_count;
        }
        notify_slot_changed(slot);
    }
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    store64(registry, k_header_generation_offset, generation + 1U);
    store64(registry, k_header_active_count_offset, active_count);
    store64(registry, k_header_waiting_count_offset, waiting_count);
    store32(registry, k_header_occupied_limit_offset, occupied_limit);
    return true;
}

bool registry_generation_has_headroom(const unsigned char *registry) {
    const std::uint64_t required = static_cast<std::uint64_t>(slot_count(registry)) + 2U;
    return load64(registry, k_header_generation_offset) <=
           std::numeric_limits<std::uint64_t>::max() - required;
}

std::uint32_t scan_slot_limit(const unsigned char *registry) {
    const std::uint32_t count = slot_count(registry);
    const std::uint32_t occupied_limit = load32(registry, k_header_occupied_limit_offset);
    if (occupied_limit == 0U && (load64(registry, k_header_active_count_offset) != 0U ||
                                 load64(registry, k_header_waiting_count_offset) != 0U)) {
        return count;
    }
    return std::min(occupied_limit, count);
}

void extend_scan_slot_limit(unsigned char *registry, unsigned char *slot) {
    const std::uint32_t new_limit = slot_index(registry, slot) + 1U;
    if (new_limit > scan_slot_limit(registry)) {
        store32(registry, k_header_occupied_limit_offset, new_limit);
    }
}

void shrink_scan_slot_limit(
    unsigned char *registry,
    std::size_t mapping_size,
    unsigned char *slot
) {
    std::uint32_t limit = scan_slot_limit(registry);
    if (limit == 0U || slot_index(registry, slot) + 1U < limit) {
        return;
    }

    while (limit > 0U) {
        unsigned char *candidate = slot_at(registry, limit - 1U);
        if (static_cast<std::size_t>(
                candidate + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size ||
            load32(candidate, k_slot_state_offset) != k_slot_state_free) {
            break;
        }
        --limit;
    }
    store32(registry, k_header_occupied_limit_offset, limit);
}

LockSearchResult find_lock_slot(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request
) {
    LockSearchResult result;
    const std::uint32_t count = slot_count(registry);
    const std::uint32_t limit = scan_slot_limit(registry);
    for (std::uint32_t index = 0; index < limit; ++index) {
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
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING) {
            if (load32(slot, k_slot_owner_id_offset) == request.owner_id &&
                load64(slot, k_slot_trx_id_offset) == request.trx_id) {
                result.own_waiting_slot = slot;
            } else if (locks_conflict(slot, request)) {
                const bool owner_blocks_waiting_lock =
                    request_owner_blocks_waiting_lock(registry, mapping_size, request, slot);
                if (!owner_blocks_waiting_lock &&
                    (result.queued_slot == nullptr ||
                     load64(slot, k_slot_generation_offset) <
                         load64(result.queued_slot, k_slot_generation_offset))) {
                    result.queued_slot = slot;
                }
            }
            continue;
        }
        if (same_lock(slot, request) && !insert_reservation_request(request)) {
            result.own_slot = slot;
            return result;
        }
        if (locks_conflict(slot, request) && result.conflicting_slot == nullptr) {
            result.conflicting_slot = slot;
        }
    }
    if (result.free_slot == nullptr && limit < count) {
        result.free_slot = slot_at(registry, limit);
    }
    return result;
}

bool queued_waiter_precedes_request(const LockSearchResult &search) {
    if (search.queued_slot == nullptr) {
        return false;
    }
    if (search.own_waiting_slot == nullptr) {
        return true;
    }
    return load64(search.queued_slot, k_slot_generation_offset) <
           load64(search.own_waiting_slot, k_slot_generation_offset);
}

bool request_owner_blocks_waiting_lock(
    unsigned char *registry,
    std::size_t mapping_size,
    const LockRequest &request,
    const unsigned char *waiting_slot
) {
    if (!page_write_lock_request(request) || !page_write_lock_slot(waiting_slot)) {
        return false;
    }

    const LockRequest waiting_request = lock_request_from_slot(waiting_slot);
    const std::uint32_t count = scan_slot_limit(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE ||
            load32(slot, k_slot_owner_id_offset) != request.owner_id ||
            !page_write_lock_slot(slot)) {
            continue;
        }

        if (locks_conflict(slot, waiting_request)) {
            return true;
        }
    }
    return false;
}

unsigned char *find_waiting_slot_by_transaction(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t trx_id
) {
    const std::uint32_t count = scan_slot_limit(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING &&
            load32(slot, k_slot_owner_id_offset) == owner_id &&
            load64(slot, k_slot_trx_id_offset) == trx_id) {
            return slot;
        }
    }
    return nullptr;
}

LockRequest lock_request_from_slot(const unsigned char *slot) {
    return LockRequest{
        load32(slot, k_slot_owner_id_offset),
        load64(slot, k_slot_trx_id_offset),
        load32(slot, k_slot_kind_offset),
        load32(slot, k_slot_mode_offset),
        load32(slot, k_slot_flags_offset),
        load64(slot, k_slot_table_id_offset),
        load64(slot, k_slot_index_id_offset),
        load32(slot, k_slot_space_id_offset),
        load32(slot, k_slot_page_no_offset),
        load32(slot, k_slot_heap_no_offset),
    };
}

void initialize_lock_slot(
    unsigned char *registry,
    unsigned char *slot,
    const LockRequest &request
) {
    extend_scan_slot_limit(registry, slot);
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
    store32(slot, k_slot_blocker_owner_id_offset, 0U);
    store64(slot, k_slot_blocker_trx_id_offset, 0U);
    store64(slot, k_slot_blocker_generation_offset, 0U);
    clear_finalize_fields(slot);
    store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE);
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
}

void replace_active_lock_slot(
    unsigned char *registry,
    unsigned char *slot,
    const LockRequest &request
) {
    const std::uint64_t generation = load64(registry, k_header_generation_offset) + 1U;

    /*
      The owner, transaction, index and X mode are identical in the reserved
      and final lock. Publish the changing coordinates in otherwise-unused
      slot storage before marking the conversion recoverable.
    */
    store32(slot, k_slot_finalize_space_id_offset, request.space_id);
    store32(slot, k_slot_finalize_page_no_offset, request.page_no);
    store32(slot, k_slot_finalize_heap_no_offset, request.heap_no);
    store32(slot, k_slot_finalize_flags_offset, request.flags);
    store64(slot, k_slot_finalize_generation_offset, generation);
    store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_INNODB_LOCK_STATE_FINALIZING_INSERT);

    store32(slot, k_slot_space_id_offset, request.space_id);
    store32(slot, k_slot_page_no_offset, request.page_no);
    store32(slot, k_slot_heap_no_offset, request.heap_no);
    store32(slot, k_slot_flags_offset, request.flags);
    store64(slot, k_slot_generation_offset, generation);
    store32(slot, k_slot_reference_count_offset, 1U);
    store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE);
    clear_finalize_fields(slot);
    notify_slot_changed(slot);
    store64(registry, k_header_generation_offset, generation);
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
    const std::uint64_t wait_generation = new_wait
                                              ? load64(registry, k_header_generation_offset) + 1U
                                              : load64(slot, k_slot_generation_offset);

    extend_scan_slot_limit(registry, slot);
    store64(slot, k_slot_generation_offset, wait_generation);
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
    clear_finalize_fields(slot);
    store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING);
    store64(
        registry,
        k_header_generation_offset,
        load64(registry, k_header_generation_offset) + 1U
    );
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
    shrink_scan_slot_limit(
        registry,
        mylite_ownerless_innodb_lock_registry_size(slot_count(registry)),
        slot
    );
    store64(
        registry,
        k_header_active_count_offset,
        load64(registry, k_header_active_count_offset) - 1U
    );
}

void clear_waiting_slot(unsigned char *registry, unsigned char *slot) {
    clear_slot_fields(registry, slot);
    shrink_scan_slot_limit(
        registry,
        mylite_ownerless_innodb_lock_registry_size(slot_count(registry)),
        slot
    );
    store64(
        registry,
        k_header_waiting_count_offset,
        load64(registry, k_header_waiting_count_offset) - 1U
    );
}

void clear_slot_fields(unsigned char *registry, unsigned char *slot) {
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
    clear_finalize_fields(slot);
    store64(slot, k_slot_generation_offset, load64(registry, k_header_generation_offset) + 1U);
    notify_slot_changed(slot);
    store64(
        registry,
        k_header_generation_offset,
        load64(registry, k_header_generation_offset) + 1U
    );
}

void clear_finalize_fields(unsigned char *slot) {
    store32(slot, k_slot_finalize_space_id_offset, 0U);
    store32(slot, k_slot_finalize_page_no_offset, 0U);
    store32(slot, k_slot_finalize_heap_no_offset, 0U);
    store32(slot, k_slot_finalize_flags_offset, 0U);
    store64(slot, k_slot_finalize_generation_offset, 0U);
}

void canonicalize_free_slot(unsigned char *slot) {
    store32(slot, k_slot_state_offset, k_slot_state_free);
    store64(slot, k_slot_generation_offset, 0U);
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
    clear_finalize_fields(slot);
}

void notify_slot_changed(unsigned char *slot) {
    mylite_ownerless_wait_word *wait_word = slot_wait_word(slot);
    const std::uint32_t wait_generation = mylite_ownerless_wait_load(wait_word);

    mylite_ownerless_wait_store(wait_word, wait_generation + 1U);
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
    if (insert_reservation_slot(slot) || insert_reservation_request(request)) {
        return false;
    }
    if (load32(slot, k_slot_kind_offset) != request.kind ||
        (load32(slot, k_slot_owner_id_offset) == request.owner_id &&
         load64(slot, k_slot_trx_id_offset) == request.trx_id)) {
        return false;
    }
    if (load32(slot, k_slot_owner_id_offset) == request.owner_id) {
        return false;
    }
    if (request.kind == MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE) {
        if (load64(slot, k_slot_table_id_offset) != request.table_id) {
            return false;
        }
        return table_locks_conflict(request.mode, load32(slot, k_slot_mode_offset));
    }
    if (page_write_lock_slot(slot) && page_write_lock_request(request)) {
        return page_write_locks_conflict(slot, request);
    }
    if (load64(slot, k_slot_index_id_offset) != request.index_id ||
        load32(slot, k_slot_space_id_offset) != request.space_id ||
        load32(slot, k_slot_page_no_offset) != request.page_no) {
        return false;
    }

    if (load32(slot, k_slot_heap_no_offset) == request.heap_no) {
        return record_locks_conflict(
            request.mode,
            request.flags,
            load32(slot, k_slot_mode_offset),
            load32(slot, k_slot_flags_offset)
        );
    }
    if (load32(slot, k_slot_owner_id_offset) == request.owner_id) {
        return false;
    }

    return record_page_locks_conflict(
        request.mode,
        request.flags,
        load32(slot, k_slot_mode_offset),
        load32(slot, k_slot_flags_offset)
    );
}

bool insert_reservation_slot(const unsigned char *slot) {
    return load32(slot, k_slot_kind_offset) == MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD &&
           load32(slot, k_slot_mode_offset) == MYLITE_OWNERLESS_INNODB_LOCK_MODE_X &&
           load32(slot, k_slot_flags_offset) ==
               MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_RESERVATION;
}

bool insert_reservation_request(const LockRequest &request) {
    return request.kind == MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD &&
           request.mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_X &&
           request.flags == MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_RESERVATION;
}

bool finalize_insert_reservation_request(const LockRequest &request) {
    return request.kind == MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD &&
           request.mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_X &&
           request.flags == (MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP |
                             MYLITE_OWNERLESS_INNODB_RECORD_LOCK_FINALIZE_INSERT_RESERVATION);
}

bool page_write_lock_slot(const unsigned char *slot) {
    const std::uint32_t mode = load32(slot, k_slot_mode_offset);
    const std::uint32_t page_no = load32(slot, k_slot_page_no_offset);
    return load32(slot, k_slot_kind_offset) == MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD &&
           (mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_X ||
            (mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_S && page_write_space_read_gate(page_no))) &&
           load32(slot, k_slot_flags_offset) == 0U &&
           load64(slot, k_slot_index_id_offset) == k_page_write_index_id &&
           load32(slot, k_slot_heap_no_offset) == k_page_write_heap_no;
}

bool page_write_lock_request(const LockRequest &request) {
    return request.kind == MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD &&
           (request.mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_X ||
            (request.mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_S &&
             page_write_space_read_gate(request.page_no))) &&
           request.flags == 0U && request.index_id == k_page_write_index_id &&
           request.heap_no == k_page_write_heap_no;
}

bool page_write_global_gate(std::uint32_t space_id, std::uint32_t page_no) {
    return space_id == k_page_write_global_gate_space_id &&
           page_no == k_page_write_global_gate_page_no;
}

bool page_write_space_gate(std::uint32_t page_no) {
    return page_no == k_page_write_space_gate_page_no;
}

bool page_write_space_read_gate(std::uint32_t page_no) {
    return page_no == k_page_write_space_read_gate_page_no;
}

bool page_write_space_write(std::uint32_t page_no) {
    return page_no == k_page_write_global_gate_page_no;
}

bool page_write_default_undo_space(std::uint32_t space_id) {
    return space_id > 0U && space_id <= k_embedded_default_undo_space_count;
}

bool page_write_locks_conflict(const unsigned char *slot, const LockRequest &request) {
    const std::uint32_t active_space_id = load32(slot, k_slot_space_id_offset);
    const std::uint32_t active_page_no = load32(slot, k_slot_page_no_offset);

    if (page_write_global_gate(active_space_id, active_page_no) ||
        page_write_global_gate(request.space_id, request.page_no)) {
        return true;
    }
    if (page_write_space_gate(active_page_no)) {
        return active_space_id == request.space_id;
    }
    if (page_write_space_gate(request.page_no)) {
        return request.space_id == active_space_id;
    }
    if (page_write_space_read_gate(active_page_no)) {
        return active_space_id == request.space_id &&
               (page_write_space_gate(request.page_no) || page_write_space_write(request.page_no));
    }
    if (page_write_space_read_gate(request.page_no)) {
        return request.space_id == active_space_id &&
               (page_write_space_gate(active_page_no) || page_write_space_write(active_page_no));
    }
    if (active_space_id == request.space_id && page_write_default_undo_space(active_space_id) &&
        (page_write_space_write(active_page_no) || page_write_space_write(request.page_no))) {
        return true;
    }
    if (active_space_id != request.space_id || active_page_no != request.page_no) {
        return false;
    }

    return record_page_locks_conflict(
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

bool record_page_locks_conflict(
    std::uint32_t requested_mode,
    std::uint32_t requested_flags,
    std::uint32_t active_mode,
    std::uint32_t active_flags
) {
    if (requested_mode != MYLITE_OWNERLESS_INNODB_LOCK_MODE_X ||
        active_mode != MYLITE_OWNERLESS_INNODB_LOCK_MODE_X) {
        return false;
    }

    /* Separate process-local buffer pools cannot safely merge same-page writes yet. */
    const bool requested_physical = requested_flags == 0U;
    const bool active_physical = active_flags == 0U;
    return requested_physical && active_physical;
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
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP | MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP |
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION |
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_SUPREMUM |
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_RESERVATION |
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_FINALIZE_INSERT_RESERVATION;
    if (flags == MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_RESERVATION ||
        flags == (MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP |
                  MYLITE_OWNERLESS_INNODB_RECORD_LOCK_FINALIZE_INSERT_RESERVATION)) {
        return true;
    }
    const bool gap = (flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP) != 0U;
    const bool record_not_gap = (flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP) != 0U;
    const bool insert_intention =
        (flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION) != 0U;
    const bool supremum = (flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_SUPREMUM) != 0U;
    return (flags & ~known_flags) == 0U && !(gap && record_not_gap) &&
           !(supremum && record_not_gap) && (!insert_intention || gap || supremum);
}

bool record_request_valid(std::uint32_t mode, std::uint32_t flags) {
    if (!record_mode_valid(mode) || !record_flags_valid(flags)) {
        return false;
    }
    const std::uint32_t internal_flags =
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_RESERVATION |
        MYLITE_OWNERLESS_INNODB_RECORD_LOCK_FINALIZE_INSERT_RESERVATION;
    return (flags & internal_flags) == 0U || mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_X;
}

bool committed_slot_valid(const unsigned char *slot) {
    const std::uint32_t state = load32(slot, k_slot_state_offset);
    const std::uint32_t kind = load32(slot, k_slot_kind_offset);
    const std::uint32_t mode = load32(slot, k_slot_mode_offset);
    const std::uint32_t flags = load32(slot, k_slot_flags_offset);
    const std::uint32_t reference_count = load32(slot, k_slot_reference_count_offset);
    if ((state != MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE &&
         state != MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING) ||
        load64(slot, k_slot_generation_offset) == 0U ||
        load32(slot, k_slot_owner_id_offset) == 0U || load64(slot, k_slot_trx_id_offset) == 0U ||
        (state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE && reference_count == 0U) ||
        (state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING && reference_count != 0U)) {
        return false;
    }

    if (kind == MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE) {
        return table_mode_valid(mode) && flags == 0U &&
               load64(slot, k_slot_table_id_offset) != 0U &&
               load64(slot, k_slot_index_id_offset) == 0U &&
               load32(slot, k_slot_space_id_offset) == 0U &&
               load32(slot, k_slot_page_no_offset) == 0U &&
               load32(slot, k_slot_heap_no_offset) == 0U;
    }
    if (kind != MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD || !record_request_valid(mode, flags) ||
        load64(slot, k_slot_table_id_offset) != 0U || load64(slot, k_slot_index_id_offset) == 0U ||
        (flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_FINALIZE_INSERT_RESERVATION) != 0U) {
        return false;
    }

    const bool reservation = insert_reservation_slot(slot);
    if ((flags & MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_RESERVATION) != 0U && !reservation) {
        return false;
    }
    if (reservation) {
        return state == MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE &&
               load32(slot, k_slot_space_id_offset) == 0U &&
               load32(slot, k_slot_page_no_offset) == 0U &&
               load32(slot, k_slot_heap_no_offset) == 0U;
    }
    return true;
}

unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<unsigned>(std::max<std::chrono::milliseconds::rep>(remaining.count(), 1));
}

bool registry_size_fits(std::uint32_t slot_count) {
    const std::size_t max_slots = (std::numeric_limits<std::size_t>::max() -
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

std::uint32_t slot_index(const unsigned char *registry, const unsigned char *slot) {
    const std::size_t offset = static_cast<std::size_t>(
        slot - registry - MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE
    );
    return static_cast<std::uint32_t>(offset / MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE);
}

unsigned char *slot_at(unsigned char *registry, std::uint32_t index) {
    return registry + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(index) * MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE);
}

mylite_ownerless_latch *registry_latch(unsigned char *registry) {
    return reinterpret_cast<mylite_ownerless_latch *>(registry + k_header_latch_offset);
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

} // namespace

#ifndef MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_H
#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK 0
#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_FULL 1
#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_NOT_FOUND 2
#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_TIMEOUT 3
#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_ERROR 4
#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OWNER_DEAD 5
#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_APPLIED_RELEASE_PENDING 6
#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_RELEASE_PENDING 7

#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_HEADER_SIZE 96U
#define MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE 64U
#define MYLITE_OWNERLESS_PAGE_PIN_STATE_ACTIVE 1U

size_t mylite_ownerless_page_pin_registry_size(uint32_t slot_count);
int mylite_ownerless_page_pin_registry_initialize(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_count
);
int mylite_ownerless_page_pin_registry_open(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t read_lsn,
    uint32_t *out_slot_index,
    uint64_t *out_slot_generation
);
/*
 * Atomically replace an active pin. APPLIED_RELEASE_PENDING means the old
 * token is stale and the returned replacement token must be retained.
 */
int mylite_ownerless_page_pin_registry_replace(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t old_slot_index,
    uint64_t old_slot_generation,
    uint64_t read_lsn,
    uint32_t *out_slot_index,
    uint64_t *out_slot_generation
);
int mylite_ownerless_page_pin_registry_close(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t slot_index,
    uint64_t slot_generation
);
int mylite_ownerless_page_pin_registry_release_owner(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t latch_owner_id,
    uint64_t latch_owner_generation,
    uint32_t *out_released_pins
);
int mylite_ownerless_page_pin_registry_snapshot_oldest(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t *out_active_count,
    uint64_t *out_oldest_read_lsn
);
int mylite_ownerless_page_pin_registry_snapshot_oldest_excluding(
    void *mapping,
    size_t mapping_size,
    uint32_t latch_owner_id,
    uint64_t latch_owner_generation,
    uint32_t exclude_owner_id,
    uint32_t exclude_slot_index,
    uint64_t exclude_slot_generation,
    uint32_t *out_active_count,
    uint64_t *out_oldest_read_lsn
);
uint64_t mylite_ownerless_page_pin_registry_active_count(const void *mapping);
int mylite_ownerless_page_pin_registry_owner_active_count(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t latch_owner_id,
    uint64_t latch_owner_generation,
    uint32_t *out_active_count
);
/* Idempotently finish either pending-release result for the same latch owner. */
int mylite_ownerless_page_pin_registry_finish_pending_release(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation
);
struct mylite_ownerless_process_registry_liveness_context;
int mylite_ownerless_page_pin_registry_recover_dead_latch(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    const struct mylite_ownerless_process_registry_liveness_context *liveness
);

#ifdef __cplusplus
}
#endif

#endif

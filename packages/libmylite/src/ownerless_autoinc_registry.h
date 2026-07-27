#ifndef MYLITE_OWNERLESS_AUTOINC_REGISTRY_H
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK 0
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL 1
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR 2
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_TIMEOUT 3
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_OWNER_DEAD 4
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_STALE 5
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_APPLIED_RELEASE_PENDING 6

#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE 64U
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE 32U

typedef struct mylite_ownerless_autoinc_registry_entry {
    uint64_t table_id;
    uint64_t next_value;
    uint64_t persistent_value;
    uint64_t entry_generation;
} mylite_ownerless_autoinc_registry_entry;

size_t mylite_ownerless_autoinc_registry_size(uint32_t slot_count);
int mylite_ownerless_autoinc_registry_initialize(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_count
);
int mylite_ownerless_autoinc_registry_read_or_seed(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t table_id,
    uint64_t seed_next_value,
    uint64_t *out_next_value
);
int mylite_ownerless_autoinc_registry_publish(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t table_id,
    uint64_t next_value,
    uint64_t persistent_value
);
int mylite_ownerless_autoinc_registry_entry_generation(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t table_id,
    uint64_t *out_entry_generation
);
/*
 * Removal is idempotent for an absent/already-retired incarnation. STALE means
 * the table ID currently names a different registry incarnation. The caller
 * must hold the table-lifecycle exclusion that prevents an old incarnation
 * from publishing after its tombstone is reused by another table.
 */
int mylite_ownerless_autoinc_registry_remove(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t table_id,
    uint64_t entry_generation
);
int mylite_ownerless_autoinc_registry_checkpoint_pending(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    int *out_pending
);
int mylite_ownerless_autoinc_registry_clear_checkpoint_pending(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation
);
int mylite_ownerless_autoinc_registry_snapshot(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    mylite_ownerless_autoinc_registry_entry *entries,
    size_t entry_capacity,
    size_t *out_entry_count
);
/* Complete a same-owner latch release after APPLIED_RELEASE_PENDING. */
int mylite_ownerless_autoinc_registry_finish_pending_release(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation
);
struct mylite_ownerless_process_registry_liveness_context;
int mylite_ownerless_autoinc_registry_recover_dead_latch(
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

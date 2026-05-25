#ifndef MYLITE_OWNERLESS_TRX_REGISTRY_H
#define MYLITE_OWNERLESS_TRX_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_TRX_REGISTRY_OK 0
#define MYLITE_OWNERLESS_TRX_REGISTRY_FULL 1
#define MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND 2
#define MYLITE_OWNERLESS_TRX_REGISTRY_TIMEOUT 3
#define MYLITE_OWNERLESS_TRX_REGISTRY_ERROR 4

#define MYLITE_OWNERLESS_TRX_REGISTRY_HEADER_SIZE 96U
#define MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE 64U
#define MYLITE_OWNERLESS_TRX_STATE_ACTIVE 1U
#define MYLITE_OWNERLESS_TRX_REGISTRY_UNASSIGNED_NO UINT64_MAX

size_t mylite_ownerless_trx_registry_size(uint32_t slot_count);
int mylite_ownerless_trx_registry_initialize(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_count,
    uint64_t next_trx_id
);
int mylite_ownerless_trx_registry_begin(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t *out_trx_id,
    uint32_t *out_slot_index,
    uint64_t *out_slot_generation
);
int mylite_ownerless_trx_registry_allocate_id(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t *out_trx_id
);
int mylite_ownerless_trx_registry_ensure_next_id_at_least(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t minimum_next_trx_id
);
int mylite_ownerless_trx_registry_assign_no(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t trx_id,
    uint64_t trx_no
);
int mylite_ownerless_trx_registry_assign_new_no(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t trx_id,
    uint64_t *out_trx_no
);
int mylite_ownerless_trx_registry_end(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t slot_index,
    uint64_t slot_generation
);
int mylite_ownerless_trx_registry_end_by_id(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t trx_id
);
int mylite_ownerless_trx_registry_release_owner(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t latch_owner_id,
    uint64_t latch_owner_generation,
    uint32_t *out_released_transactions
);
int mylite_ownerless_trx_registry_snapshot(
    void *mapping,
    size_t mapping_size,
    uint64_t *out_trx_ids,
    uint32_t trx_id_capacity,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_oldest_active_trx_id
);
int mylite_ownerless_trx_registry_snapshot_read_view(
    void *mapping,
    size_t mapping_size,
    uint64_t *out_trx_ids,
    uint32_t trx_id_capacity,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no
);
uint64_t mylite_ownerless_trx_registry_active_count(const void *mapping);
int mylite_ownerless_trx_registry_owner_active_count(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t latch_owner_id,
    uint64_t latch_owner_generation,
    uint32_t *out_active_count
);
uint64_t mylite_ownerless_trx_registry_oldest_active_trx_id(
    const void *mapping,
    size_t mapping_size
);
uint64_t mylite_ownerless_trx_registry_next_trx_id(const void *mapping);

#ifdef __cplusplus
}
#endif

#endif

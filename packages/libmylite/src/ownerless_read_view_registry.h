#ifndef MYLITE_OWNERLESS_READ_VIEW_REGISTRY_H
#define MYLITE_OWNERLESS_READ_VIEW_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK 0
#define MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL 1
#define MYLITE_OWNERLESS_READ_VIEW_REGISTRY_NOT_FOUND 2
#define MYLITE_OWNERLESS_READ_VIEW_REGISTRY_TIMEOUT 3
#define MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ERROR 4

#define MYLITE_OWNERLESS_READ_VIEW_REGISTRY_HEADER_SIZE 64U
#define MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE 576U
#define MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ID_CAPACITY 64U
#define MYLITE_OWNERLESS_READ_VIEW_STATE_ACTIVE 1U

size_t mylite_ownerless_read_view_registry_size(uint32_t slot_count);
int mylite_ownerless_read_view_registry_initialize(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_count
);
int mylite_ownerless_read_view_registry_open(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t low_limit_id,
    uint64_t low_limit_no,
    const uint64_t *trx_ids,
    uint32_t trx_id_count,
    uint32_t *out_slot_index,
    uint64_t *out_slot_generation
);
int mylite_ownerless_read_view_registry_close(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t slot_index,
    uint64_t slot_generation
);
int mylite_ownerless_read_view_registry_release_owner(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t *out_released_views
);
int mylite_ownerless_read_view_registry_snapshot_oldest(
    void *mapping,
    size_t mapping_size,
    uint64_t *out_trx_ids,
    uint32_t trx_id_capacity,
    uint32_t *out_trx_id_count,
    uint64_t *out_low_limit_id,
    uint64_t *out_low_limit_no
);
uint64_t mylite_ownerless_read_view_registry_active_count(const void *mapping);
int mylite_ownerless_read_view_registry_owner_active_count(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t *out_active_count
);

#ifdef __cplusplus
}
#endif

#endif

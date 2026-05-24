#ifndef MYLITE_OWNERLESS_READ_VIEW_HOOKS_H
#define MYLITE_OWNERLESS_READ_VIEW_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_READ_VIEW_OK 0
#define MYLITE_OWNERLESS_READ_VIEW_UNAVAILABLE 1
#define MYLITE_OWNERLESS_READ_VIEW_FULL 2
#define MYLITE_OWNERLESS_READ_VIEW_ERROR 3

typedef int (*mylite_ownerless_read_view_register_callback)(
    uint64_t low_limit_id,
    uint64_t low_limit_no,
    const uint64_t *trx_ids,
    unsigned int trx_id_count,
    uint32_t *out_slot_index,
    uint64_t *out_slot_generation,
    void *context);
typedef int (*mylite_ownerless_read_view_deregister_callback)(
    uint32_t slot_index,
    uint64_t slot_generation,
    void *context);
typedef int (*mylite_ownerless_read_view_snapshot_callback)(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_low_limit_id,
    uint64_t *out_low_limit_no,
    void *context);

void mylite_ownerless_read_view_set_hooks(
    mylite_ownerless_read_view_register_callback register_hook,
    mylite_ownerless_read_view_deregister_callback deregister_hook,
    mylite_ownerless_read_view_snapshot_callback snapshot_hook,
    void *context);
void mylite_ownerless_read_view_reset_hooks(void);
int mylite_ownerless_read_view_has_hooks(void);
int mylite_ownerless_read_view_register(
    uint64_t low_limit_id,
    uint64_t low_limit_no,
    const uint64_t *trx_ids,
    unsigned int trx_id_count,
    uint32_t *out_slot_index,
    uint64_t *out_slot_generation);
int mylite_ownerless_read_view_deregister(uint32_t slot_index, uint64_t slot_generation);
int mylite_ownerless_read_view_snapshot(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_low_limit_id,
    uint64_t *out_low_limit_no);

#ifdef __cplusplus
}
#endif

#endif

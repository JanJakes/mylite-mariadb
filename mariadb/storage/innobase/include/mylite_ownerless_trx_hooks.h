#ifndef MYLITE_OWNERLESS_TRX_HOOKS_INCLUDED
#define MYLITE_OWNERLESS_TRX_HOOKS_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_TRX_OK 0
#define MYLITE_OWNERLESS_TRX_UNAVAILABLE 1
#define MYLITE_OWNERLESS_TRX_FULL 2
#define MYLITE_OWNERLESS_TRX_ERROR 3

typedef int (*mylite_ownerless_trx_allocate_callback)(
    uint64_t *out_trx_id,
    void *context);
typedef int (*mylite_ownerless_trx_register_callback)(
    uint64_t *out_trx_id,
    void *context);
typedef int (*mylite_ownerless_trx_assign_no_callback)(
    uint64_t trx_id,
    uint64_t trx_no,
    void *context);
typedef int (*mylite_ownerless_trx_deregister_callback)(
    uint64_t trx_id,
    void *context);
typedef int (*mylite_ownerless_trx_snapshot_callback)(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no,
    void *context);

void mylite_ownerless_trx_set_hooks(
    mylite_ownerless_trx_allocate_callback allocate_hook,
    mylite_ownerless_trx_register_callback register_hook,
    mylite_ownerless_trx_assign_no_callback assign_no_hook,
    mylite_ownerless_trx_deregister_callback deregister_hook,
    mylite_ownerless_trx_snapshot_callback snapshot_hook,
    void *context);
void mylite_ownerless_trx_reset_hooks(void);
int mylite_ownerless_trx_has_hooks(void);
int mylite_ownerless_trx_allocate(uint64_t *out_trx_id);
int mylite_ownerless_trx_register(uint64_t *out_trx_id);
int mylite_ownerless_trx_assign_no(uint64_t trx_id, uint64_t trx_no);
int mylite_ownerless_trx_deregister(uint64_t trx_id);
int mylite_ownerless_trx_snapshot(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no);

#ifdef __cplusplus
}
#endif

#endif

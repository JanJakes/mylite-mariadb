#ifndef MYLITE_OWNERLESS_TRX_HOOKS_INCLUDED
#define MYLITE_OWNERLESS_TRX_HOOKS_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
#include <atomic>

extern std::atomic<bool> mylite_ownerless_trx_hooks_enabled;

static inline int mylite_ownerless_trx_hooks_enabled_fast(void)
{
    return mylite_ownerless_trx_hooks_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" {
#endif

#define MYLITE_OWNERLESS_TRX_OK 0
#define MYLITE_OWNERLESS_TRX_UNAVAILABLE 1
#define MYLITE_OWNERLESS_TRX_FULL 2
#define MYLITE_OWNERLESS_TRX_ERROR 3

#define MYLITE_OWNERLESS_TRX_RECOVERY_ABSENT 0
#define MYLITE_OWNERLESS_TRX_RECOVERY_LIVE_REMOTE 1
#define MYLITE_OWNERLESS_TRX_RECOVERY_REQUIRED 2
#define MYLITE_OWNERLESS_TRX_RECOVERY_BLOCKED 3

#define MYLITE_OWNERLESS_TRX_ROLLBACK_NONE 0
#define MYLITE_OWNERLESS_TRX_ROLLBACK_IN_PROGRESS 1
#define MYLITE_OWNERLESS_TRX_ROLLBACK_SAVEPOINT_READ_SAFE 2

typedef int (*mylite_ownerless_trx_allocate_callback)(
    uint64_t *out_trx_id,
    void *context);
typedef int (*mylite_ownerless_trx_register_callback)(
    uint64_t *out_trx_id,
    void *context);
typedef int (*mylite_ownerless_trx_assign_no_callback)(
    uint64_t trx_id,
    uint64_t *out_trx_no,
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
typedef uint64_t (*mylite_ownerless_trx_next_id_callback)(void *context);
typedef int (*mylite_ownerless_trx_rollback_state_callback)(
    uint64_t trx_id,
    uint32_t rollback_state,
    void *context);
typedef int (*mylite_ownerless_trx_recovery_state_callback)(
    uint64_t trx_id,
    int *out_state,
    void *context);

void mylite_ownerless_trx_set_hooks(
    mylite_ownerless_trx_allocate_callback allocate_hook,
    mylite_ownerless_trx_register_callback register_hook,
    mylite_ownerless_trx_assign_no_callback assign_no_hook,
    mylite_ownerless_trx_deregister_callback deregister_hook,
    mylite_ownerless_trx_snapshot_callback snapshot_hook,
    mylite_ownerless_trx_next_id_callback next_id_hook,
    mylite_ownerless_trx_rollback_state_callback rollback_state_hook,
    mylite_ownerless_trx_recovery_state_callback recovery_state_hook,
    void *context);
void mylite_ownerless_trx_reset_hooks(void);
int mylite_ownerless_trx_has_hooks(void);
uint64_t mylite_ownerless_trx_local_max_id(void);
void mylite_ownerless_trx_advance_local_max_id_at_least(uint64_t minimum_next_trx_id);
int mylite_ownerless_trx_allocate(uint64_t *out_trx_id);
int mylite_ownerless_trx_register(uint64_t *out_trx_id);
int mylite_ownerless_trx_assign_no(uint64_t trx_id, uint64_t *out_trx_no);
int mylite_ownerless_trx_deregister(uint64_t trx_id);
int mylite_ownerless_trx_snapshot(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no);
int mylite_ownerless_trx_snapshot_retry(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no);
uint64_t mylite_ownerless_trx_next_id(void);
int mylite_ownerless_trx_set_rollback_state(uint64_t trx_id, uint32_t rollback_state);
int mylite_ownerless_trx_recovery_state(uint64_t trx_id, int *out_state);

#ifdef __cplusplus
}
#endif

#endif

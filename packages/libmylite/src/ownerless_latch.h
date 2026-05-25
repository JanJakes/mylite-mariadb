#ifndef MYLITE_OWNERLESS_LATCH_H
#define MYLITE_OWNERLESS_LATCH_H

#include "ownerless_wait.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_LATCH_OK 0
#define MYLITE_OWNERLESS_LATCH_TIMEOUT 1
#define MYLITE_OWNERLESS_LATCH_OWNER_DEAD 2
#define MYLITE_OWNERLESS_LATCH_ERROR 3

#define MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED 0U
#define MYLITE_OWNERLESS_LATCH_STATE_LOCKED 1U
#define MYLITE_OWNERLESS_LATCH_SIZE 32U

typedef int (*mylite_ownerless_latch_owner_alive_callback)(
    uint32_t owner_id,
    uint64_t owner_generation,
    void *ctx
);

typedef struct mylite_ownerless_latch {
    uint64_t state_owner;
    mylite_ownerless_wait_word wake_epoch;
    uint32_t waiter_count;
    uint64_t owner_generation;
    uint64_t owner_death_count;
} mylite_ownerless_latch;

void mylite_ownerless_latch_initialize(mylite_ownerless_latch *latch);
int mylite_ownerless_latch_acquire(
    mylite_ownerless_latch *latch,
    uint32_t owner_id,
    uint64_t owner_generation,
    mylite_ownerless_latch_owner_alive_callback is_owner_alive,
    void *owner_alive_ctx,
    unsigned timeout_ms
);
int mylite_ownerless_latch_release(
    mylite_ownerless_latch *latch,
    uint32_t owner_id,
    uint64_t owner_generation
);
int mylite_ownerless_latch_snapshot(
    const mylite_ownerless_latch *latch,
    uint32_t *out_state,
    uint32_t *out_owner_id,
    uint64_t *out_owner_generation,
    uint32_t *out_waiter_count,
    uint64_t *out_owner_death_count
);

#ifdef __cplusplus
}
#endif

#endif

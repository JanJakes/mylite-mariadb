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
#define MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED 4
#define MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE 5
/* The caller stopped after publishing RELEASE_PENDING and must retry. */
#define MYLITE_OWNERLESS_LATCH_RELEASE_PENDING 6

#define MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED 0U
#define MYLITE_OWNERLESS_LATCH_STATE_LOCKED 1U
#define MYLITE_OWNERLESS_LATCH_STATE_ACQUIRING 2U
#define MYLITE_OWNERLESS_LATCH_STATE_RECOVERING 3U
#define MYLITE_OWNERLESS_LATCH_STATE_OWNER_DEAD 4U
#define MYLITE_OWNERLESS_LATCH_STATE_CLAIMING_RECOVERY 5U
#define MYLITE_OWNERLESS_LATCH_STATE_RELEASING 6U
#define MYLITE_OWNERLESS_LATCH_STATE_RELEASE_PENDING 7U
#define MYLITE_OWNERLESS_LATCH_SIZE 32U

/*
 * The 32-byte shared layout and legacy UNLOCKED/LOCKED values are unchanged,
 * so an existing segment needs no rebuild. The added states are transient
 * protocol values; concurrently mixing binaries that do not recognize them is
 * unsupported.
 */

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

typedef struct mylite_ownerless_latch_dead_owner {
    uint32_t owner_id;
    uint32_t reserved;
    uint64_t owner_generation;
} mylite_ownerless_latch_dead_owner;

void mylite_ownerless_latch_initialize(mylite_ownerless_latch *latch);
int mylite_ownerless_latch_acquire(
    mylite_ownerless_latch *latch,
    uint32_t owner_id,
    uint64_t owner_generation,
    mylite_ownerless_latch_owner_alive_callback is_owner_alive,
    void *owner_alive_ctx,
    unsigned timeout_ms
);
/*
 * RECOVERY_REQUIRED means the caller owns the latch in RECOVERING state. The
 * protected data must be repaired before mark_consistent(). A failed repair
 * must call mark_not_recoverable(); release() intentionally rejects both
 * states. This keeps dead-owner detection from becoming a blind latch steal.
 */
int mylite_ownerless_latch_acquire_recoverable(
    mylite_ownerless_latch *latch,
    uint32_t owner_id,
    uint64_t owner_generation,
    mylite_ownerless_latch_owner_alive_callback is_owner_alive,
    void *owner_alive_ctx,
    unsigned timeout_ms,
    mylite_ownerless_latch_dead_owner *out_dead_owner
);
int mylite_ownerless_latch_mark_consistent(
    mylite_ownerless_latch *latch,
    uint32_t owner_id,
    uint64_t owner_generation
);
int mylite_ownerless_latch_mark_not_recoverable(
    mylite_ownerless_latch *latch,
    uint32_t owner_id,
    uint64_t owner_generation
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

/* Internal deterministic fault support for focused first-party tests. */
void mylite_ownerless_latch_test_inject_release_pending_once(void);

#ifdef __cplusplus
}
#endif

#endif

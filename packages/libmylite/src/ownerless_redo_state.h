#ifndef MYLITE_OWNERLESS_REDO_STATE_H
#define MYLITE_OWNERLESS_REDO_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_REDO_STATE_OK 0
#define MYLITE_OWNERLESS_REDO_STATE_TIMEOUT 1
#define MYLITE_OWNERLESS_REDO_STATE_ERROR 2
#define MYLITE_OWNERLESS_REDO_STATE_OWNER_DEAD 3
#define MYLITE_OWNERLESS_REDO_STATE_APPLIED_RELEASE_PENDING 4

#define MYLITE_OWNERLESS_REDO_STATE_SIZE 4096U
#define MYLITE_OWNERLESS_REDO_STATE_VISIBLE_LSN_OFFSET 40U
#define MYLITE_OWNERLESS_REDO_STATE_VISIBLE_GENERATION_OFFSET 80U

typedef struct mylite_ownerless_redo_state_snapshot {
    uint64_t latest_lsn;
    uint64_t visible_lsn;
    uint64_t reserved_lsn;
    uint64_t durable_lsn;
    uint64_t written_lsn;
    uint64_t visible_generation;
    uint32_t refcount;
    uint32_t active_reservation_count;
    uint32_t completed_range_count;
    uint32_t latch_state;
    uint32_t latch_owner_id;
    uint64_t latch_owner_generation;
    uint32_t progress_latch_state;
    uint32_t progress_latch_owner_id;
    uint64_t progress_latch_owner_generation;
} mylite_ownerless_redo_state_snapshot;

typedef struct mylite_ownerless_redo_state_range {
    uint64_t start_lsn;
    uint64_t end_lsn;
} mylite_ownerless_redo_state_range;

int mylite_ownerless_redo_state_initialize(
    void *state,
    size_t state_size,
    uint64_t latest_lsn,
    uint64_t visible_lsn
);
int mylite_ownerless_redo_state_seed_checkpoint(
    void *state,
    size_t state_size,
    uint64_t latest_lsn,
    uint64_t visible_lsn
);
int mylite_ownerless_redo_state_enter(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    unsigned timeout_ms,
    uint64_t *out_latest_lsn
);
int mylite_ownerless_redo_state_leave(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t latest_lsn,
    uint64_t *out_advanced_latest_lsn,
    uint32_t *out_remaining
);
int mylite_ownerless_redo_state_reserve(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t minimum_start_lsn,
    uint64_t length,
    uint64_t *out_start_lsn,
    uint64_t *out_end_lsn
);
int mylite_ownerless_redo_state_complete_write(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t *out_written_lsn
);
int mylite_ownerless_redo_state_complete_write_and_leave(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn,
    uint64_t *out_advanced_latest_lsn,
    uint32_t *out_remaining
);
int mylite_ownerless_redo_state_complete_write_and_leave_batch(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    const mylite_ownerless_redo_state_range *ranges,
    size_t range_count,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn,
    uint64_t *out_advanced_latest_lsn,
    uint32_t *out_remaining,
    size_t *out_completed_count
);
int mylite_ownerless_redo_state_publish_visible(
    void *state,
    size_t state_size,
    uint64_t visible_lsn,
    uint64_t *out_latest_lsn,
    uint64_t *out_visible_lsn
);
int mylite_ownerless_redo_state_cleanup_owner(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t *out_released
);
int mylite_ownerless_redo_state_cleanup_owner_with_latch_owner(
    void *state,
    size_t state_size,
    uint32_t dead_owner_id,
    uint64_t dead_owner_generation,
    uint32_t latch_owner_id,
    uint64_t latch_owner_generation,
    uint32_t *out_released
);
int mylite_ownerless_redo_state_owner_active_count(
    const void *state,
    size_t state_size,
    uint32_t owner_id,
    uint32_t *out_active_count
);
int mylite_ownerless_redo_state_read_snapshot(
    const void *state,
    size_t state_size,
    mylite_ownerless_redo_state_snapshot *out_snapshot
);
int mylite_ownerless_redo_state_finish_pending_progress_release(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation
);
int mylite_ownerless_redo_state_finish_pending_state_release(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation
);
struct mylite_ownerless_process_registry_liveness_context;
/*
 * OWNER_DEAD after a successful latch repair means an incomplete reservation
 * remains and native redo recovery must coordinate before it is discarded.
 */
int mylite_ownerless_redo_state_recover_dead_latches(
    void *state,
    size_t state_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    const struct mylite_ownerless_process_registry_liveness_context *liveness
);

#ifdef __cplusplus
}
#endif

#endif

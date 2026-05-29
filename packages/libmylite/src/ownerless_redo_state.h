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

#define MYLITE_OWNERLESS_REDO_STATE_SIZE 4096U
#define MYLITE_OWNERLESS_REDO_STATE_VISIBLE_LSN_OFFSET 40U

typedef struct mylite_ownerless_redo_state_snapshot {
    uint64_t latest_lsn;
    uint64_t visible_lsn;
    uint64_t reserved_lsn;
    uint64_t durable_lsn;
    uint64_t written_lsn;
    uint32_t refcount;
    uint32_t active_reservation_count;
    uint32_t latch_state;
    uint32_t latch_owner_id;
    uint64_t latch_owner_generation;
    uint32_t progress_latch_state;
    uint32_t progress_latch_owner_id;
    uint64_t progress_latch_owner_generation;
} mylite_ownerless_redo_state_snapshot;

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

#ifdef __cplusplus
}
#endif

#endif

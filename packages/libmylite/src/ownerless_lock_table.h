#ifndef MYLITE_OWNERLESS_LOCK_TABLE_H
#define MYLITE_OWNERLESS_LOCK_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_LOCK_TABLE_OK 0
#define MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT 1
#define MYLITE_OWNERLESS_LOCK_TABLE_FULL 2
#define MYLITE_OWNERLESS_LOCK_TABLE_NOT_FOUND 3
#define MYLITE_OWNERLESS_LOCK_TABLE_ERROR 4
#define MYLITE_OWNERLESS_LOCK_TABLE_DEADLOCK 5
#define MYLITE_OWNERLESS_LOCK_TABLE_KILLED 6
#define MYLITE_OWNERLESS_LOCK_TABLE_OWNER_DEAD 7
/* The operation applied; the owner must retain its cleanup identity and retry. */
#define MYLITE_OWNERLESS_LOCK_TABLE_APPLIED_RELEASE_PENDING 8

#define MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE 96U
#define MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE 80U
#define MYLITE_OWNERLESS_LOCK_TABLE_PRODUCTION_ENTRY_COUNT 1024U
#define MYLITE_OWNERLESS_LOCK_TABLE_SHARED 1U
#define MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE 2U
#define MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE 3U
#define MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ 4U
#define MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE 5U
#define MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY 6U
#define MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE 7U
#define MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE 8U
#define MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE 9U
#define MYLITE_OWNERLESS_LOCK_TABLE_DEFAULT_DEADLOCK_WEIGHT 1U

typedef int (*mylite_ownerless_lock_table_cancel_callback)(void *context);

size_t mylite_ownerless_lock_table_size(uint32_t entry_count);
int mylite_ownerless_lock_table_initialize(
    void *mapping,
    size_t mapping_size,
    uint32_t entry_count
);
int mylite_ownerless_lock_table_acquire_exclusive(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t timeout_ms
);
int mylite_ownerless_lock_table_acquire_shared(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t timeout_ms
);
int mylite_ownerless_lock_table_acquire_upgradable(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t timeout_ms
);
int mylite_ownerless_lock_table_acquire_mode(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mode,
    uint64_t timeout_ms
);
int mylite_ownerless_lock_table_acquire_mode_for_session(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t session_id,
    uint32_t mode,
    uint64_t timeout_ms
);
/* The cancellation callback must be non-blocking and safe under the table latch. */
int mylite_ownerless_lock_table_acquire_mode_for_session_with_options(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t session_id,
    uint32_t mode,
    int bypass_queued_waiters,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    uint64_t timeout_ms
);
int mylite_ownerless_lock_table_acquire_mode_for_session_with_deadlock_weight(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t session_id,
    uint32_t mode,
    int bypass_queued_waiters,
    uint32_t deadlock_weight,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    uint64_t timeout_ms
);
int mylite_ownerless_lock_table_acquire_mode_for_session_with_scheduling(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t session_id,
    uint32_t mode,
    int bypass_queued_waiters,
    uint32_t deadlock_weight,
    uint64_t max_write_lock_count,
    mylite_ownerless_lock_table_cancel_callback is_cancelled,
    void *cancel_context,
    uint64_t timeout_ms
);
int mylite_ownerless_lock_table_release_exclusive(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation
);
int mylite_ownerless_lock_table_release_shared(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation
);
int mylite_ownerless_lock_table_release_upgradable(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation
);
int mylite_ownerless_lock_table_release_mode(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mode
);
int mylite_ownerless_lock_table_release_mode_for_session(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t session_id,
    uint32_t mode
);
int mylite_ownerless_lock_table_reclassify_mode_for_session(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t session_id,
    uint32_t old_mode,
    uint32_t new_mode
);
int mylite_ownerless_lock_table_release_owner(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t latch_owner_id,
    uint64_t latch_owner_generation,
    uint32_t *out_released_entries
);
int mylite_ownerless_lock_table_owner_active_count(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t latch_owner_id,
    uint64_t latch_owner_generation,
    uint32_t *out_active_count
);
struct mylite_ownerless_process_registry_liveness_context;
int mylite_ownerless_lock_table_recover_dead_latch(
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

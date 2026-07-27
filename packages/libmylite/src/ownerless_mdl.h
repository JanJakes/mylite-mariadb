#ifndef MYLITE_OWNERLESS_MDL_H
#define MYLITE_OWNERLESS_MDL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_MDL_NAMESPACE_SCHEMA 1U
#define MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE 2U

#ifndef MYLITE_OWNERLESS_MDL_WAIT_OPTIONS_DEFINED
#  define MYLITE_OWNERLESS_MDL_WAIT_OPTIONS_DEFINED
typedef int (*mylite_ownerless_mdl_cancel_callback)(void *context);

typedef struct mylite_ownerless_mdl_wait_options {
    mylite_ownerless_mdl_cancel_callback is_cancelled;
    void *cancel_context;
    unsigned int bypass_queued_waiters;
    unsigned int deadlock_weight;
    unsigned int reclassify_from_mode;
    uint64_t max_write_lock_count;
} mylite_ownerless_mdl_wait_options;
#endif

uint64_t mylite_ownerless_mdl_key_hash(
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
);
int mylite_ownerless_mdl_acquire_shared(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    uint64_t timeout_ms
);
int mylite_ownerless_mdl_acquire_upgradable(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    uint64_t timeout_ms
);
int mylite_ownerless_mdl_acquire_exclusive(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    uint64_t timeout_ms
);
int mylite_ownerless_mdl_acquire_mode(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    uint32_t mode,
    uint64_t timeout_ms
);
int mylite_ownerless_mdl_acquire_mode_for_session(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t session_id,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    uint32_t mode,
    uint64_t timeout_ms
);
int mylite_ownerless_mdl_acquire_mode_for_session_with_options(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t session_id,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    uint32_t mode,
    const mylite_ownerless_mdl_wait_options *wait_options,
    uint64_t timeout_ms
);
int mylite_ownerless_mdl_release_shared(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
);
int mylite_ownerless_mdl_release_upgradable(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
);
int mylite_ownerless_mdl_release_mode(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    uint32_t mode
);
int mylite_ownerless_mdl_release_mode_for_session(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t session_id,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    uint32_t mode
);
int mylite_ownerless_mdl_release_exclusive(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
);

#ifdef __cplusplus
}
#endif

#endif

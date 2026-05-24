#ifndef MYLITE_OWNERLESS_MDL_H
#define MYLITE_OWNERLESS_MDL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_MDL_NAMESPACE_SCHEMA 1U
#define MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE 2U

uint64_t mylite_ownerless_mdl_key_hash(
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
);
int mylite_ownerless_mdl_acquire_shared(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    unsigned timeout_ms
);
int mylite_ownerless_mdl_acquire_exclusive(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    unsigned timeout_ms
);
int mylite_ownerless_mdl_release_shared(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
);
int mylite_ownerless_mdl_release_exclusive(
    void *lock_table,
    size_t lock_table_size,
    uint32_t owner_id,
    uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
);

#ifdef __cplusplus
}
#endif

#endif

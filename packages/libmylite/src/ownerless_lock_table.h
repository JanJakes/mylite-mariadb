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

#define MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE 64U
#define MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE 64U
#define MYLITE_OWNERLESS_LOCK_TABLE_SHARED 1U
#define MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE 2U

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
    unsigned timeout_ms
);
int mylite_ownerless_lock_table_acquire_shared(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id,
    unsigned timeout_ms
);
int mylite_ownerless_lock_table_release_exclusive(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id
);
int mylite_ownerless_lock_table_release_shared(
    void *mapping,
    size_t mapping_size,
    uint64_t key_hash,
    uint32_t owner_id
);
int mylite_ownerless_lock_table_release_owner(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t *out_released_entries
);

#ifdef __cplusplus
}
#endif

#endif

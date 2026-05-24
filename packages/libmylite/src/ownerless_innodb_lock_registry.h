#ifndef MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_H
#define MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK 0
#define MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_FULL 1
#define MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_NOT_FOUND 2
#define MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT 3
#define MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ERROR 4
#define MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK 5

#define MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE 64U
#define MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE 128U
#define MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE 1U
#define MYLITE_OWNERLESS_INNODB_LOCK_STATE_WAITING 2U

#define MYLITE_OWNERLESS_INNODB_LOCK_KIND_TABLE 1U
#define MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD 2U

#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_IS 0U
#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX 1U
#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_S 2U
#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_X 3U
#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_AUTO_INC 4U

#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP 1U
#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP 2U
#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION 4U
#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_SUPREMUM 8U

size_t mylite_ownerless_innodb_lock_registry_size(uint32_t slot_count);
int mylite_ownerless_innodb_lock_registry_initialize(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_count
);
int mylite_ownerless_innodb_lock_registry_acquire_table(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    unsigned timeout_ms
);
int mylite_ownerless_innodb_lock_registry_reserve_table(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    unsigned timeout_ms
);
int mylite_ownerless_innodb_lock_registry_release_table(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode
);
int mylite_ownerless_innodb_lock_registry_wait_for_table(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    uint32_t blocker_owner_id,
    uint64_t blocker_trx_id
);
int mylite_ownerless_innodb_lock_registry_wait_until_table_available(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    unsigned timeout_ms
);
int mylite_ownerless_innodb_lock_registry_acquire_record(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned timeout_ms
);
int mylite_ownerless_innodb_lock_registry_reserve_record(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned timeout_ms
);
int mylite_ownerless_innodb_lock_registry_release_record(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags
);
int mylite_ownerless_innodb_lock_registry_wait_for_record(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    uint32_t blocker_owner_id,
    uint64_t blocker_trx_id
);
int mylite_ownerless_innodb_lock_registry_wait_until_record_available(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned timeout_ms
);
int mylite_ownerless_innodb_lock_registry_clear_wait(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t trx_id,
    uint32_t *out_cleared_waits
);
int mylite_ownerless_innodb_lock_registry_release_owner(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t *out_released_locks
);
uint64_t mylite_ownerless_innodb_lock_registry_active_count(const void *mapping);
uint64_t mylite_ownerless_innodb_lock_registry_waiting_count(const void *mapping);

#ifdef __cplusplus
}
#endif

#endif

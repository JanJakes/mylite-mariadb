#ifndef MYLITE_OWNERLESS_INNODB_LOCK_HOOKS_H
#define MYLITE_OWNERLESS_INNODB_LOCK_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_INNODB_LOCK_OK 0
#define MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE 1
#define MYLITE_OWNERLESS_INNODB_LOCK_FULL 2
#define MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT 3
#define MYLITE_OWNERLESS_INNODB_LOCK_ERROR 4

#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_IS 0U
#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX 1U
#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_S 2U
#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_X 3U
#define MYLITE_OWNERLESS_INNODB_LOCK_MODE_AUTO_INC 4U

#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP 1U
#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP 2U
#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION 4U
#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_SUPREMUM 8U

struct ib_lock_t;
struct trx_t;

typedef int (*mylite_ownerless_innodb_lock_acquire_table_callback)(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    unsigned int timeout_ms,
    void *context);
typedef int (*mylite_ownerless_innodb_lock_release_table_callback)(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    void *context);
typedef int (*mylite_ownerless_innodb_lock_acquire_record_callback)(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned int timeout_ms,
    void *context);
typedef int (*mylite_ownerless_innodb_lock_release_record_callback)(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    void *context);

void mylite_ownerless_innodb_lock_set_hooks(
    mylite_ownerless_innodb_lock_acquire_table_callback acquire_table_hook,
    mylite_ownerless_innodb_lock_release_table_callback release_table_hook,
    mylite_ownerless_innodb_lock_acquire_record_callback acquire_record_hook,
    mylite_ownerless_innodb_lock_release_record_callback release_record_hook,
    void *context);
void mylite_ownerless_innodb_lock_reset_hooks(void);
int mylite_ownerless_innodb_lock_has_hooks(void);
void mylite_ownerless_innodb_lock_publish_table(const struct ib_lock_t *lock);
void mylite_ownerless_innodb_lock_release_table(const struct ib_lock_t *lock);
void mylite_ownerless_innodb_lock_publish_record_bit(
    const struct ib_lock_t *lock,
    uint32_t heap_no);
void mylite_ownerless_innodb_lock_publish_record_bits(const struct ib_lock_t *lock);
void mylite_ownerless_innodb_lock_release_record_bit(
    const struct ib_lock_t *lock,
    uint32_t heap_no);
void mylite_ownerless_innodb_lock_release_record_bits(const struct ib_lock_t *lock);
void mylite_ownerless_innodb_lock_forget_transaction(struct trx_t *trx);

#ifdef __cplusplus
}
#endif

#endif

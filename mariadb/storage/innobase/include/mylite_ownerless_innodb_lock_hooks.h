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
#define MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK 5

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
struct dict_index_t;
struct dict_table_t;
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
typedef int (*mylite_ownerless_innodb_lock_wait_table_callback)(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    uint64_t blocker_trx_id,
    void *context);
typedef int (*mylite_ownerless_innodb_lock_wait_until_table_callback)(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    unsigned int timeout_ms,
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
typedef int (*mylite_ownerless_innodb_lock_wait_record_callback)(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    uint64_t blocker_trx_id,
    void *context);
typedef int (*mylite_ownerless_innodb_lock_wait_until_record_callback)(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned int timeout_ms,
    void *context);
typedef int (*mylite_ownerless_innodb_lock_clear_wait_callback)(
    uint64_t trx_id,
    void *context);
typedef int (*mylite_ownerless_innodb_redo_enter_callback)(
    uint64_t *out_latest_lsn,
    void *context);
typedef void (*mylite_ownerless_innodb_redo_leave_callback)(
    uint64_t latest_lsn,
    void *context);

enum mylite_ownerless_innodb_lock_external_wait_kind {
    MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_NONE = 0,
    MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_TABLE = 1,
    MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_RECORD = 2
};

struct mylite_ownerless_innodb_lock_external_wait {
    uint32_t kind;
    uint64_t trx_id;
    uint64_t table_id;
    uint64_t index_id;
    uint32_t space_id;
    uint32_t page_no;
    uint32_t heap_no;
    uint32_t mode;
    uint32_t flags;
};

void mylite_ownerless_innodb_lock_set_hooks(
    mylite_ownerless_innodb_lock_acquire_table_callback acquire_table_hook,
    mylite_ownerless_innodb_lock_release_table_callback release_table_hook,
    mylite_ownerless_innodb_lock_wait_table_callback wait_table_hook,
    mylite_ownerless_innodb_lock_acquire_record_callback acquire_record_hook,
    mylite_ownerless_innodb_lock_release_record_callback release_record_hook,
    mylite_ownerless_innodb_lock_wait_record_callback wait_record_hook,
    mylite_ownerless_innodb_lock_wait_until_table_callback wait_until_table_hook,
    mylite_ownerless_innodb_lock_wait_until_record_callback wait_until_record_hook,
    mylite_ownerless_innodb_lock_clear_wait_callback clear_wait_hook,
    mylite_ownerless_innodb_redo_enter_callback redo_enter_hook,
    mylite_ownerless_innodb_redo_leave_callback redo_leave_hook,
    void *context);
void mylite_ownerless_innodb_lock_reset_hooks(void);
int mylite_ownerless_innodb_lock_has_hooks(void);
int mylite_ownerless_innodb_lock_reserve_table(
    struct trx_t *trx,
    const struct dict_table_t *table,
    uint32_t mode,
    unsigned int timeout_ms);
void mylite_ownerless_innodb_lock_publish_table(const struct ib_lock_t *lock);
void mylite_ownerless_innodb_lock_release_table(const struct ib_lock_t *lock);
int mylite_ownerless_innodb_lock_publish_table_wait(
    const struct ib_lock_t *wait_lock,
    const struct ib_lock_t *blocker_lock);
int mylite_ownerless_innodb_lock_snapshot_external_wait(
    const struct ib_lock_t *wait_lock,
    struct mylite_ownerless_innodb_lock_external_wait *snapshot);
int mylite_ownerless_innodb_lock_wait_for_external(
    const struct mylite_ownerless_innodb_lock_external_wait *snapshot,
    unsigned int timeout_ms);
int mylite_ownerless_innodb_lock_reserve_record(
    struct trx_t *trx,
    const struct dict_index_t *index,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t type_mode,
    unsigned int timeout_ms);
void mylite_ownerless_innodb_lock_publish_record_bit(
    const struct ib_lock_t *lock,
    uint32_t heap_no);
void mylite_ownerless_innodb_lock_publish_record_bits(const struct ib_lock_t *lock);
void mylite_ownerless_innodb_lock_release_record_bit(
    const struct ib_lock_t *lock,
    uint32_t heap_no);
void mylite_ownerless_innodb_lock_release_record_bits(const struct ib_lock_t *lock);
int mylite_ownerless_innodb_lock_publish_record_wait(
    const struct ib_lock_t *wait_lock,
    const struct ib_lock_t *blocker_lock);
void mylite_ownerless_innodb_lock_clear_transaction_wait(struct trx_t *trx);
void mylite_ownerless_innodb_lock_forget_transaction(struct trx_t *trx);
void mylite_ownerless_innodb_flush_dirty_pages(void);
void mylite_ownerless_innodb_refresh_external_pages(uint64_t latest_lsn);
int mylite_ownerless_innodb_refresh_to_latest_external_lsn(void);
int mylite_ownerless_innodb_redo_is_active(void);
int mylite_ownerless_innodb_redo_enter(uint64_t *out_latest_lsn);
void mylite_ownerless_innodb_redo_leave(uint64_t latest_lsn);

#ifdef __cplusplus
}
#endif

#endif

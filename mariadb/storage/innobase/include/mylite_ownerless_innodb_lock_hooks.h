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

#define MYLITE_OWNERLESS_INNODB_PAGE_WRITE_INDEX_ID UINT64_MAX
#define MYLITE_OWNERLESS_INNODB_PAGE_WRITE_HEAP_NO UINT32_MAX
#define MYLITE_OWNERLESS_INNODB_SPACE_WRITE_PAGE_NO UINT32_MAX
#define MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_SPACE_ID (UINT32_MAX - 2U)
#define MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_PAGE_NO UINT32_MAX
#define MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_WRITE_PAGE_NO (UINT32_MAX - 1U)
#define MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_WAITED 1U

struct ib_lock_t;
struct buf_block_t;
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
typedef int (*mylite_ownerless_innodb_lock_acquire_page_write_callback)(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags,
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
typedef int (*mylite_ownerless_innodb_lock_release_page_writes_callback)(
    uint64_t trx_id,
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
typedef int (*mylite_ownerless_innodb_redo_observe_callback)(
    uint64_t *out_latest_lsn,
    void *context);
typedef int (*mylite_ownerless_innodb_redo_reserve_callback)(
    uint64_t current_lsn,
    uint64_t length,
    uint64_t *out_start_lsn,
    uint64_t *out_end_lsn,
    void *context);
typedef int (*mylite_ownerless_innodb_redo_written_callback)(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t *out_written_lsn,
    void *context);
typedef void (*mylite_ownerless_innodb_redo_leave_callback)(
    uint64_t latest_lsn,
    void *context);
typedef void (*mylite_ownerless_innodb_pages_visible_callback)(
    uint64_t visible_lsn,
    void *context);
typedef int (*mylite_ownerless_innodb_page_publish_callback)(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size,
    void *context);
typedef int (*mylite_ownerless_innodb_page_read_callback)(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    void *context);
typedef int (*mylite_ownerless_innodb_autoinc_read_callback)(
    uint64_t table_id,
    uint64_t seed_next_value,
    uint64_t *out_next_value,
    void *context);
typedef int (*mylite_ownerless_innodb_autoinc_publish_callback)(
    uint64_t table_id,
    uint64_t next_value,
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
    mylite_ownerless_innodb_lock_acquire_page_write_callback acquire_page_write_hook,
    mylite_ownerless_innodb_lock_release_record_callback release_page_write_hook,
    mylite_ownerless_innodb_lock_release_page_writes_callback release_page_writes_hook,
    mylite_ownerless_innodb_lock_wait_record_callback wait_record_hook,
    mylite_ownerless_innodb_lock_wait_until_table_callback wait_until_table_hook,
    mylite_ownerless_innodb_lock_wait_until_record_callback wait_until_record_hook,
    mylite_ownerless_innodb_lock_clear_wait_callback clear_wait_hook,
    mylite_ownerless_innodb_redo_enter_callback redo_enter_hook,
    mylite_ownerless_innodb_redo_observe_callback redo_observe_hook,
    mylite_ownerless_innodb_redo_reserve_callback redo_reserve_hook,
    mylite_ownerless_innodb_redo_written_callback redo_written_hook,
    mylite_ownerless_innodb_redo_leave_callback redo_leave_hook,
    mylite_ownerless_innodb_pages_visible_callback pages_visible_hook,
    mylite_ownerless_innodb_page_publish_callback page_publish_hook,
    mylite_ownerless_innodb_page_read_callback page_read_hook,
    void *context);
void mylite_ownerless_innodb_lock_reset_hooks(void);
int mylite_ownerless_innodb_lock_has_hooks(void);
void mylite_ownerless_innodb_autoinc_set_hooks(
    mylite_ownerless_innodb_autoinc_read_callback read_hook,
    mylite_ownerless_innodb_autoinc_publish_callback publish_hook,
    void *context);
void mylite_ownerless_innodb_autoinc_reset_hooks(void);
int mylite_ownerless_innodb_autoinc_has_hooks(void);
void mylite_ownerless_innodb_reset_thread_redo_latch_depth(void);
int mylite_ownerless_innodb_lock_reserve_table(
    struct trx_t *trx,
    const struct dict_table_t *table,
    uint32_t mode,
    unsigned int timeout_ms);
int mylite_ownerless_innodb_lock_acquire_autoinc(
    struct trx_t *trx,
    const struct dict_table_t *table,
    unsigned int timeout_ms);
void mylite_ownerless_innodb_lock_release_autoinc(
    struct trx_t *trx,
    const struct dict_table_t *table);
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
int mylite_ownerless_innodb_lock_wait_until_record_available(
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
int mylite_ownerless_innodb_lock_acquire_page_write(
    struct trx_t *trx,
    uint32_t space_id,
    uint32_t page_no,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags);
int mylite_ownerless_innodb_lock_acquire_transaction_page_write_gate(
    struct trx_t *trx,
    uint32_t space_id,
    unsigned int timeout_ms);
int mylite_ownerless_innodb_lock_release_page_write(
    struct trx_t *trx,
    uint32_t space_id,
    uint32_t page_no);
void mylite_ownerless_innodb_lock_release_transaction_page_writes(struct trx_t *trx);
void mylite_ownerless_innodb_lock_release_transaction_page_write_gates(
    struct trx_t *trx);
int mylite_ownerless_innodb_lock_publish_record_wait(
    const struct ib_lock_t *wait_lock,
    const struct ib_lock_t *blocker_lock);
void mylite_ownerless_innodb_lock_clear_transaction_wait(struct trx_t *trx);
void mylite_ownerless_innodb_lock_forget_transaction(struct trx_t *trx);
void mylite_ownerless_innodb_publish_transaction_pages_to_lsn(
    struct trx_t *trx, uint64_t visible_lsn);
void mylite_ownerless_innodb_flush_dirty_pages_to_lsn(uint64_t visible_lsn);
void mylite_ownerless_innodb_publish_buffer_pool_pages_to_lsn(uint64_t visible_lsn);
void mylite_ownerless_innodb_flush_dirty_pages_for_page_writes(uint64_t flush_lsn);
void mylite_ownerless_innodb_flush_space_dirty_pages(uint32_t space_id);
void mylite_ownerless_innodb_refresh_external_pages(uint64_t latest_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages(uint64_t visible_lsn);
int mylite_ownerless_innodb_refresh_page_for_read(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t visible_lsn);
void mylite_ownerless_innodb_evict_clean_external_pages(void);
int mylite_ownerless_innodb_advance_external_lsn(uint64_t latest_lsn);
void mylite_ownerless_innodb_refresh_external_space_header(uint32_t space_id);
void mylite_ownerless_innodb_refresh_external_space_allocation(uint32_t space_id);
void mylite_ownerless_innodb_refresh_external_space_headers(void);
void mylite_ownerless_innodb_evict_dictionary_cache(void);
int mylite_ownerless_innodb_refresh_page_for_write(const struct buf_block_t *block);
int mylite_ownerless_innodb_refresh_page_for_write_force(
    const struct buf_block_t *block);
int mylite_ownerless_innodb_refresh_external_wait_page(
    const struct mylite_ownerless_innodb_lock_external_wait *snapshot);
void mylite_ownerless_innodb_enable_external_page_visibility(uint64_t latest_lsn);
uint64_t mylite_ownerless_innodb_push_external_page_visibility(uint64_t latest_lsn);
void mylite_ownerless_innodb_restore_external_page_visibility(uint64_t previous_lsn);
void mylite_ownerless_innodb_clear_external_page_visibility(void);
int mylite_ownerless_innodb_refresh_to_latest_external_lsn(void);
uint64_t mylite_ownerless_innodb_current_lsn(void);
int mylite_ownerless_innodb_redo_is_active(void);
int mylite_ownerless_innodb_redo_enter(uint64_t *out_latest_lsn);
int mylite_ownerless_innodb_redo_observe(uint64_t *out_latest_lsn);
int mylite_ownerless_innodb_redo_reserve(
    uint64_t current_lsn,
    uint64_t length,
    uint64_t *out_start_lsn,
    uint64_t *out_end_lsn);
int mylite_ownerless_innodb_redo_written(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t *out_written_lsn);
void mylite_ownerless_innodb_redo_leave(uint64_t latest_lsn);
int mylite_ownerless_innodb_publish_page_version(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size);
int mylite_ownerless_innodb_read_page_version(
    uint32_t space_id,
    uint32_t page_no,
    void *page,
    uint32_t page_capacity);
int mylite_ownerless_innodb_autoinc_read(
    uint64_t table_id,
    uint64_t seed_next_value,
    uint64_t *out_next_value);
int mylite_ownerless_innodb_autoinc_publish(uint64_t table_id, uint64_t next_value);

#ifdef __cplusplus
}
#endif

#endif

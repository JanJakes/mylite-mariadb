#ifndef MYLITE_OWNERLESS_INNODB_LOCK_HOOKS_H
#define MYLITE_OWNERLESS_INNODB_LOCK_HOOKS_H

#include <stddef.h>
#include <stdint.h>

struct buf_block_t;

#define MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES 61U

#ifdef __cplusplus
#include <atomic>
#include <cstdlib>

extern std::atomic<bool> mylite_ownerless_innodb_lock_hooks_enabled;
extern std::atomic<bool> mylite_ownerless_innodb_lock_hooks_ever_enabled;
extern std::atomic<bool> mylite_ownerless_innodb_autoinc_hooks_enabled;
extern std::atomic<bool> mylite_ownerless_innodb_test_faults_enabled;

void mylite_ownerless_mark_retained_native_write_page_dirty(buf_block_t *block);

static inline int mylite_ownerless_innodb_lock_hooks_enabled_fast(void)
{
    return mylite_ownerless_innodb_lock_hooks_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

static inline int mylite_ownerless_innodb_lock_hooks_ever_enabled_fast(void)
{
    return mylite_ownerless_innodb_lock_hooks_ever_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

static inline int mylite_ownerless_innodb_autoinc_hooks_enabled_fast(void)
{
    return mylite_ownerless_innodb_autoinc_hooks_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

static inline int mylite_ownerless_innodb_test_faults_enabled_fast(void)
{
    return mylite_ownerless_innodb_test_faults_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

static inline int mylite_ownerless_innodb_test_setenv(
    const char *name, const char *value)
{
#ifdef _WIN32
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

static inline int mylite_ownerless_innodb_test_unsetenv(const char *name)
{
#ifdef _WIN32
    return _putenv_s(name, "");
#else
    return unsetenv(name);
#endif
}

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
#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_RESERVATION 16U
#define MYLITE_OWNERLESS_INNODB_RECORD_LOCK_FINALIZE_INSERT_RESERVATION 32U

#define MYLITE_OWNERLESS_INNODB_PAGE_WRITE_INDEX_ID UINT64_MAX
#define MYLITE_OWNERLESS_INNODB_PAGE_WRITE_HEAP_NO UINT32_MAX
#define MYLITE_OWNERLESS_INNODB_SPACE_WRITE_PAGE_NO UINT32_MAX
#define MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_SPACE_ID (UINT32_MAX - 2U)
#define MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_PAGE_NO UINT32_MAX
#define MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_WRITE_PAGE_NO (UINT32_MAX - 1U)
#define MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_READ_PAGE_NO (UINT32_MAX - 2U)
#define MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_WAITED 1U
#define MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_NEW_RECORD_PAGE 2U

#define MYLITE_OWNERLESS_INNODB_PAGE_VERSION_SNAPSHOT_BOUNDARY 1U
#define MYLITE_OWNERLESS_INNODB_PAGE_VERSION_EXTERNAL_SNAPSHOT_LINEAGE 2U
#define MYLITE_OWNERLESS_INNODB_PAGE_VERSION_NATIVE_SUPPORT_STATE 4U
#define MYLITE_OWNERLESS_INNODB_PAGE_VERSION_HISTORY_RSEG_DELTA 8U

#define MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_HISTORY_RSEG 1U
#define MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_PROOF_ONLY 2U
#define MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_HISTORY_RSEG_PAIR 4U

#define MYLITE_OWNERLESS_INNODB_PAGE_READ_HISTORY_RSEG_DELTA 1U

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
typedef int (*mylite_ownerless_innodb_lock_before_record_wait_callback)(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
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
typedef int (*mylite_ownerless_innodb_redo_observe_visible_callback)(
    uint64_t *out_visible_lsn,
    void *context);
typedef int (*mylite_ownerless_innodb_redo_observe_written_callback)(
    uint64_t *out_written_lsn,
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
typedef int (*mylite_ownerless_innodb_redo_leave_callback)(
    uint64_t latest_lsn,
    void *context);
typedef struct mylite_ownerless_innodb_redo_range {
    uint64_t start_lsn;
    uint64_t end_lsn;
} mylite_ownerless_innodb_redo_range;
typedef int (*mylite_ownerless_innodb_redo_written_leave_callback)(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn,
    void *context);
typedef int (*mylite_ownerless_innodb_redo_written_leave_batch_callback)(
    const mylite_ownerless_innodb_redo_range *ranges,
    size_t range_count,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn,
    size_t *out_completed_count,
    void *context);
typedef int (*mylite_ownerless_innodb_pages_visible_callback)(
    uint64_t visible_lsn,
    void *context);
typedef int (*mylite_ownerless_innodb_page_publish_callback)(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size,
    uint32_t publish_flags,
    void *context);
typedef int (*mylite_ownerless_innodb_page_write_active_callback)(
    uint32_t space_id,
    uint32_t page_no,
    int *out_active,
    void *context);
typedef int (*mylite_ownerless_innodb_history_proof_publish_pair_callback)(
    uint32_t space_id,
    uint32_t rseg_page_no,
    uint64_t rseg_page_lsn,
    const void *rseg_page,
    uint32_t rseg_page_size,
    uint32_t undo_page_no,
    uint64_t undo_page_lsn,
    const void *undo_page,
    uint32_t undo_page_size,
    uint64_t visible_lsn,
    void *context);
typedef void (*mylite_ownerless_innodb_page_publish_batch_callback)(void *context);
typedef int (*mylite_ownerless_innodb_page_read_callback)(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags,
    uint32_t read_options,
    void *context);
typedef int (*mylite_ownerless_innodb_skip_external_page_refresh_callback)(void *context);
typedef int (*mylite_ownerless_innodb_file_delete_guard_acquire_callback)(void *context);
typedef void (*mylite_ownerless_innodb_file_delete_guard_release_callback)(void *context);
typedef int (*mylite_ownerless_innodb_autoinc_read_callback)(
    uint64_t table_id,
    uint64_t seed_next_value,
    uint64_t *out_next_value,
    void *context);
typedef int (*mylite_ownerless_innodb_autoinc_publish_callback)(
    uint64_t table_id,
    uint64_t next_value,
    uint64_t persistent_value,
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
    mylite_ownerless_innodb_lock_release_page_writes_callback release_records_hook,
    mylite_ownerless_innodb_lock_acquire_page_write_callback acquire_page_write_hook,
    mylite_ownerless_innodb_lock_release_record_callback release_page_write_hook,
    mylite_ownerless_innodb_lock_release_page_writes_callback release_page_writes_hook,
    mylite_ownerless_innodb_lock_wait_record_callback wait_record_hook,
    mylite_ownerless_innodb_lock_wait_until_table_callback wait_until_table_hook,
    mylite_ownerless_innodb_lock_wait_until_record_callback wait_until_record_hook,
    mylite_ownerless_innodb_lock_before_record_wait_callback before_record_wait_hook,
    mylite_ownerless_innodb_lock_clear_wait_callback clear_wait_hook,
    mylite_ownerless_innodb_redo_enter_callback redo_enter_hook,
    mylite_ownerless_innodb_redo_observe_callback redo_observe_hook,
    mylite_ownerless_innodb_redo_observe_visible_callback redo_observe_visible_hook,
    mylite_ownerless_innodb_redo_reserve_callback redo_reserve_hook,
    mylite_ownerless_innodb_redo_written_callback redo_written_hook,
    mylite_ownerless_innodb_redo_leave_callback redo_leave_hook,
    mylite_ownerless_innodb_pages_visible_callback pages_visible_hook,
    mylite_ownerless_innodb_page_publish_callback page_publish_hook,
    mylite_ownerless_innodb_page_read_callback page_read_hook,
    mylite_ownerless_innodb_page_write_active_callback page_write_active_hook,
    mylite_ownerless_innodb_skip_external_page_refresh_callback skip_external_page_refresh_hook,
    int write_coordination_enabled,
    void *context);
void mylite_ownerless_innodb_lock_set_page_publish_batch_hooks(
    mylite_ownerless_innodb_page_publish_batch_callback begin_hook,
    mylite_ownerless_innodb_page_publish_batch_callback end_hook);
void mylite_ownerless_innodb_lock_set_history_proof_publish_pair_hook(
    mylite_ownerless_innodb_history_proof_publish_pair_callback pair_hook);
void mylite_ownerless_innodb_lock_set_redo_written_leave_hook(
    mylite_ownerless_innodb_redo_written_leave_callback written_leave_hook);
void mylite_ownerless_innodb_lock_set_redo_written_leave_batch_hook(
    mylite_ownerless_innodb_redo_written_leave_batch_callback written_leave_batch_hook);
void mylite_ownerless_innodb_lock_set_redo_observe_written_hook(
    mylite_ownerless_innodb_redo_observe_written_callback observe_written_hook);
void mylite_ownerless_innodb_lock_set_file_delete_guard_hooks(
    mylite_ownerless_innodb_file_delete_guard_acquire_callback acquire_hook,
    mylite_ownerless_innodb_file_delete_guard_release_callback release_hook,
    void *context);
void mylite_ownerless_innodb_lock_reset_file_delete_guard_hooks(void);
int mylite_ownerless_innodb_lock_reset_hooks(void);
int mylite_ownerless_innodb_lock_has_hooks(void);
int mylite_ownerless_innodb_file_delete_guard_acquire(void);
void mylite_ownerless_innodb_file_delete_guard_release(void);
int mylite_ownerless_innodb_write_coordination_enabled(void);
int mylite_ownerless_innodb_coordination_error(void);
void mylite_ownerless_innodb_note_coordination_error(void);
void mylite_ownerless_innodb_clear_coordination_error_for_recovery(void);
void mylite_ownerless_innodb_set_startup_lsn_advance_limit(uint64_t max_lsn);
void mylite_ownerless_innodb_clear_startup_lsn_advance_limit(void);
void mylite_ownerless_innodb_set_checkpoint_suppression(int suppressed);
int mylite_ownerless_innodb_checkpoint_suppressed(void);
void mylite_ownerless_innodb_set_relative_file_op_redo_paths(int enabled);
int mylite_ownerless_innodb_relative_file_op_redo_paths(void);
int mylite_ownerless_innodb_file_op_redo_relative_path(
    const char *datadir,
    const char *path,
    char *relative_path,
    size_t relative_path_size);
void mylite_ownerless_innodb_set_uncheckpointed_file_rename_recovery(int enabled);
int mylite_ownerless_innodb_uncheckpointed_file_rename_recovery(void);
void mylite_ownerless_innodb_note_file_op_redo(void);
int mylite_ownerless_innodb_take_file_op_redo(void);
void mylite_ownerless_innodb_note_file_rename_redo(void);
int mylite_ownerless_innodb_take_file_rename_redo(void);
void mylite_ownerless_innodb_set_test_faults_enabled(int enabled);
int mylite_ownerless_innodb_test_fault_is_configured(const char *fault_name);
int mylite_ownerless_innodb_test_fault_will_pause(const char *fault_name);
void mylite_ownerless_innodb_test_fault(const char *fault_name);
void mylite_ownerless_innodb_test_note_mtr_memmove(int index_page);
void mylite_ownerless_innodb_test_reset_mtr_memmove_count(void);
uint64_t mylite_ownerless_innodb_test_mtr_memmove_count(void);
int mylite_ownerless_innodb_test_set_next_transient_lock_trx_id(
    uint64_t next_id);
uint64_t mylite_ownerless_innodb_test_allocate_transient_lock_trx_id(void);
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
int mylite_ownerless_innodb_lock_reserve_insert_record(
    struct trx_t *trx,
    const struct dict_index_t *index,
    unsigned int timeout_ms);
int mylite_ownerless_innodb_lock_cancel_insert_record(
    struct trx_t *trx,
    const struct dict_index_t *index);
int mylite_ownerless_innodb_lock_finalize_insert_record(
    struct trx_t *trx,
    const struct dict_index_t *index,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no);
int mylite_ownerless_innodb_lock_release_rollback_insert_record(
    struct trx_t *trx,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no);
uint64_t mylite_ownerless_innodb_lock_transaction_id(struct trx_t *trx);
int mylite_ownerless_innodb_remote_trx_active(uint64_t trx_id, int *out_active);
int mylite_ownerless_innodb_lock_wait_until_record_available(
    struct trx_t *trx,
    const struct dict_index_t *index,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t type_mode,
    unsigned int timeout_ms);
int mylite_ownerless_innodb_lock_before_external_record_wait(
    struct trx_t *trx,
    const struct dict_index_t *index,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t type_mode);
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
int mylite_ownerless_innodb_lock_acquire_page_write_untracked(
    struct trx_t *trx,
    uint32_t space_id,
    uint32_t page_no,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags);
int mylite_ownerless_innodb_lock_reserve_record_page_write(
    struct trx_t *trx,
    const struct buf_block_t *block,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags);
int mylite_ownerless_innodb_lock_cancel_record_page_write(
    struct trx_t *trx,
    uint32_t space_id,
    uint32_t page_no);
int mylite_ownerless_innodb_lock_release_clean_record_page_write(
    struct trx_t *trx,
    uint32_t space_id,
    uint32_t page_no);
int mylite_ownerless_innodb_lock_acquire_transaction_page_write_gate(
    struct trx_t *trx,
    uint32_t space_id,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags);
int mylite_ownerless_innodb_lock_acquire_transaction_page_read_gate(
    struct trx_t *trx,
    uint32_t space_id,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags);
int mylite_ownerless_innodb_lock_release_page_write(
    struct trx_t *trx,
    uint32_t space_id,
    uint32_t page_no);
void mylite_ownerless_innodb_lock_release_transaction_page_writes(struct trx_t *trx);
void mylite_ownerless_innodb_lock_release_transaction_page_write_gates(
    struct trx_t *trx);
void mylite_ownerless_innodb_lock_release_transaction_clean_page_writes(
    struct trx_t *trx);
int mylite_ownerless_innodb_lock_publish_record_wait(
    const struct ib_lock_t *wait_lock,
    const struct ib_lock_t *blocker_lock);
void mylite_ownerless_innodb_lock_clear_transaction_wait(struct trx_t *trx);
void mylite_ownerless_innodb_lock_forget_transaction(struct trx_t *trx);
int mylite_ownerless_innodb_set_statement_execution_active(int enabled);
int mylite_ownerless_innodb_statement_execution_active(void);
int mylite_ownerless_innodb_set_statement_visible_fast_path(int enabled);
int mylite_ownerless_innodb_statement_visible_fast_path(void);
int mylite_ownerless_innodb_set_statement_explicit_transaction(int enabled);
int mylite_ownerless_innodb_statement_explicit_transaction(void);
int mylite_ownerless_innodb_set_statement_deferred_page_publish(int enabled);
int mylite_ownerless_innodb_statement_deferred_page_publish(void);
int mylite_ownerless_innodb_set_pages_visible_force(int enabled);
int mylite_ownerless_innodb_pages_visible_force(void);
int mylite_ownerless_innodb_set_page_write_refresh_bypass(int enabled);
int mylite_ownerless_innodb_page_write_refresh_bypass(void);
int mylite_ownerless_innodb_set_statement_plain_read(int enabled);
int mylite_ownerless_innodb_statement_plain_read(void);
int mylite_ownerless_innodb_set_statement_plain_read_preserve_local_pages(int enabled);
int mylite_ownerless_innodb_statement_plain_read_preserves_local_pages(void);
void mylite_ownerless_innodb_refresh_statement_plain_read_pages_once(void);
int mylite_ownerless_innodb_set_statement_dictionary_ddl(int enabled);
int mylite_ownerless_innodb_statement_dictionary_ddl(void);
int mylite_ownerless_innodb_set_statement_suppress_native_lifecycle_refresh(int enabled);
int mylite_ownerless_innodb_statement_suppress_native_lifecycle_refresh(void);
uint64_t mylite_ownerless_innodb_publish_transaction_pages_to_lsn(
    struct trx_t *trx, uint64_t visible_lsn);
uint64_t mylite_ownerless_innodb_publish_rollback_pages_to_lsn(
    struct trx_t *trx, uint64_t visible_lsn);
uint64_t mylite_ownerless_innodb_publish_rollback_proof_pages_to_lsn(
    struct trx_t *trx, uint64_t visible_lsn);
uint64_t mylite_ownerless_innodb_publish_transaction_buffer_pages_to_lsn(
    struct trx_t *trx, uint64_t visible_lsn);
void mylite_ownerless_innodb_flush_dirty_pages_to_lsn(uint64_t visible_lsn);
int mylite_ownerless_innodb_publish_pages_visible_lsn(uint64_t visible_lsn);
void mylite_ownerless_innodb_publish_dirty_pages_to_lsn(uint64_t visible_lsn);
void mylite_ownerless_innodb_publish_buffer_pool_pages_to_lsn(uint64_t visible_lsn);
void mylite_ownerless_innodb_flush_dirty_pages_for_page_writes(uint64_t flush_lsn);
uint64_t mylite_ownerless_innodb_flush_transaction_pages_for_page_writes(
    struct trx_t *trx,
    uint64_t flush_lsn,
    uint64_t *exact_flushed_pages,
    uint64_t *fallback_rounds);
uint64_t mylite_ownerless_innodb_flush_space_dirty_pages_to_lsn(
    uint32_t space_id,
    uint64_t flush_lsn);
uint64_t mylite_ownerless_innodb_flush_history_pages_to_lsn(
    uint32_t space_id,
    uint32_t rseg_page_no,
    uint32_t undo_page_no,
    uint64_t flush_lsn,
    uint64_t *exact_flushed_pages,
    uint64_t *fallback_rounds);
void mylite_ownerless_innodb_flush_space_dirty_pages(uint32_t space_id);
void mylite_ownerless_innodb_refresh_external_pages(uint64_t latest_lsn);
void mylite_ownerless_innodb_refresh_external_pages_retained(uint64_t latest_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages(uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_force(uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read(uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_no_skip(
    uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_force_native_current_no_skip(
    uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_no_skip(
    uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_preserve_clean_no_skip(
    uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_retained_no_skip(
    uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_retained_preserve_clean_no_skip(
    uint64_t visible_lsn);
void mylite_ownerless_innodb_materialize_retained_page_for_native_write(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_preserve(uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_native_visible_boundary(
    uint64_t visible_lsn);
void mylite_ownerless_innodb_refresh_buffer_pool_pages_force_preserve(
    uint64_t visible_lsn);
int mylite_ownerless_innodb_refresh_transaction_pages_from_native(
    struct trx_t *trx);
int mylite_ownerless_innodb_refresh_page_for_read(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t visible_lsn);
void mylite_ownerless_innodb_evict_clean_external_pages(void);
int mylite_ownerless_innodb_advance_external_lsn(uint64_t latest_lsn);
void mylite_ownerless_innodb_refresh_external_space_header(uint32_t space_id);
int mylite_ownerless_innodb_refresh_external_space_allocation(uint32_t space_id);
int mylite_ownerless_innodb_refresh_external_space_allocation_native_current(
    uint32_t space_id);
void mylite_ownerless_innodb_refresh_external_space_headers(void);
void mylite_ownerless_innodb_refresh_external_space_headers_no_skip(void);
void mylite_ownerless_innodb_evict_dictionary_cache(void);
int mylite_ownerless_innodb_repair_dictionary_rename(
    const char *old_name,
    const char *new_name
);
int mylite_ownerless_innodb_repair_dictionary_name_only(
    const char *old_name,
    const char *new_name
);
int mylite_ownerless_innodb_repair_foreign_key_id(
    const char *old_id,
    const char *new_id
);
int mylite_ownerless_innodb_repair_foreign_key_identity(
    const char *old_id,
    const char *new_id,
    const char *new_for_name
);
int mylite_ownerless_innodb_delete_foreign_key_metadata(const char *id);
int mylite_ownerless_innodb_pending_file_rename_target(const char *path);
int mylite_ownerless_innodb_can_skip_external_page_refresh(void);
int mylite_ownerless_innodb_refresh_page_for_write(const struct buf_block_t *block);
int mylite_ownerless_innodb_refresh_page_for_write_force(
    const struct buf_block_t *block);
int mylite_ownerless_innodb_refresh_page_for_write_after_wait(
    const struct buf_block_t *block);
int mylite_ownerless_innodb_refresh_page_for_current_read(
    const struct buf_block_t *block);
void mylite_ownerless_innodb_begin_internal_lock_wait(void);
void mylite_ownerless_innodb_end_internal_lock_wait(void);
int mylite_ownerless_innodb_internal_lock_wait_active(void);
struct trx_t *mylite_ownerless_innodb_push_page_write_trx_override(
    struct trx_t *trx);
void mylite_ownerless_innodb_restore_page_write_trx_override(
    struct trx_t *previous_trx);
int mylite_ownerless_innodb_refresh_external_wait_page(
    const struct mylite_ownerless_innodb_lock_external_wait *snapshot);
void mylite_ownerless_innodb_enable_external_page_visibility(uint64_t latest_lsn);
void mylite_ownerless_innodb_enable_current_external_page_visibility(uint64_t latest_lsn);
uint64_t mylite_ownerless_innodb_external_page_visibility(void);
int mylite_ownerless_innodb_external_page_visibility_is_current(void);
void mylite_ownerless_innodb_set_retained_external_page_visibility(int enabled);
int mylite_ownerless_innodb_retained_external_page_visibility(void);
int mylite_ownerless_innodb_set_retained_startup_native_write_scrub(int enabled);
void mylite_ownerless_innodb_set_startup_native_support_page_visibility(uint64_t latest_lsn);
uint64_t mylite_ownerless_innodb_startup_native_support_page_visibility(void);
uint64_t mylite_ownerless_innodb_startup_external_page_visibility(void);
void mylite_ownerless_innodb_clear_startup_external_page_visibility(void);
void mylite_ownerless_innodb_set_external_page_observation_token(uint64_t token);
void mylite_ownerless_innodb_clear_external_page_observations(void);
void mylite_ownerless_innodb_note_external_page_observed(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t commit_lsn);
int mylite_ownerless_innodb_external_page_observed_at_or_after(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t commit_lsn);
uint64_t mylite_ownerless_innodb_push_external_page_visibility(uint64_t latest_lsn);
void mylite_ownerless_innodb_restore_external_page_visibility(uint64_t previous_lsn);
void mylite_ownerless_innodb_clear_external_page_visibility(void);
void mylite_ownerless_innodb_close_current_read_view(void);
int mylite_ownerless_innodb_refresh_to_latest_external_lsn(void);
uint64_t mylite_ownerless_innodb_current_lsn(void);
uint64_t mylite_ownerless_innodb_checkpoint_lsn(void);
uint64_t mylite_ownerless_innodb_shutdown_lsn(void);
int mylite_ownerless_innodb_advance_startup_page_lsn(uint64_t latest_lsn);
int mylite_ownerless_innodb_make_checkpoint(void);
int mylite_ownerless_innodb_checkpoint_covers_lsn(uint64_t lsn);
int mylite_ownerless_innodb_settle_purge_before_hooks(unsigned int timeout_ms);
int mylite_ownerless_innodb_wait_recovered_rollback(unsigned int timeout_ms);
int mylite_ownerless_innodb_has_recovered_active_transactions(int *out_has_recovered);
int mylite_ownerless_innodb_reap_remote_recovered_transactions(void);
int mylite_ownerless_innodb_discard_remote_recovered_transactions_for_shutdown(void);
int mylite_ownerless_innodb_rollback_history_exists(int *out_exists);
int mylite_ownerless_innodb_redo_is_active(void);
int mylite_ownerless_innodb_redo_enter(uint64_t *out_latest_lsn);
int mylite_ownerless_innodb_redo_observe(uint64_t *out_latest_lsn);
int mylite_ownerless_innodb_redo_observe_visible(uint64_t *out_visible_lsn);
int mylite_ownerless_innodb_redo_observe_written(uint64_t *out_written_lsn);
int mylite_ownerless_innodb_redo_reserve(
    uint64_t current_lsn,
    uint64_t length,
    uint64_t *out_start_lsn,
    uint64_t *out_end_lsn);
int mylite_ownerless_innodb_redo_written(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t *out_written_lsn);
int mylite_ownerless_innodb_redo_leave(uint64_t latest_lsn);
int mylite_ownerless_innodb_redo_written_and_leave(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn);
int mylite_ownerless_innodb_redo_defer_written_and_leave(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn);
int mylite_ownerless_innodb_redo_flush_deferred(void);
int mylite_ownerless_innodb_publish_page_version(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size);
int mylite_ownerless_innodb_publish_page_version_with_flags(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size,
    uint32_t publish_flags);
/* Best-effort variant for native-support pages with an exact native flush
fallback. The caller must not use it for user-page visibility. */
int mylite_ownerless_innodb_try_publish_page_version_with_flags(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size,
    uint32_t publish_flags);
int mylite_ownerless_innodb_page_write_active(
    uint32_t space_id,
    uint32_t page_no,
    int *out_active);
int mylite_ownerless_innodb_publish_history_proof_pair(
    uint32_t space_id,
    uint32_t rseg_page_no,
    uint64_t rseg_page_lsn,
    const void *rseg_page,
    uint32_t rseg_page_size,
    uint32_t undo_page_no,
    uint64_t undo_page_lsn,
    const void *undo_page,
    uint32_t undo_page_size,
    uint64_t visible_lsn);
void mylite_ownerless_innodb_begin_page_publish_batch(void);
void mylite_ownerless_innodb_end_page_publish_batch(void);
int mylite_ownerless_innodb_read_page_version(
    uint32_t space_id,
    uint32_t page_no,
    void *page,
    uint32_t page_capacity);
int mylite_ownerless_innodb_retained_allocation_page_in_use(
    uint32_t space_id,
    uint32_t page_no);
int mylite_ownerless_innodb_read_page_version_with_metadata(
    uint32_t space_id,
    uint32_t page_no,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags);
int mylite_ownerless_innodb_read_startup_native_support_page_version_with_metadata(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags);
int mylite_ownerless_innodb_read_startup_native_support_page_version_with_history_rseg_delta(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags);
int mylite_ownerless_innodb_read_page_version_before_with_metadata(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags);
int mylite_ownerless_innodb_disk_page_lsn(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t *out_page_lsn);
int mylite_ownerless_innodb_disk_page_matches(
    uint32_t space_id,
    uint32_t page_no,
    const void *page,
    uint32_t page_size,
    uint64_t *out_page_lsn,
    int *out_matches);
int mylite_ownerless_innodb_autoinc_read(
    uint64_t table_id,
    uint64_t seed_next_value,
    uint64_t *out_next_value);
int mylite_ownerless_innodb_autoinc_publish(
    uint64_t table_id,
    uint64_t next_value,
    uint64_t persistent_value);
int mylite_ownerless_innodb_autoinc_replay_persistent(
    uint64_t table_id,
    uint64_t next_value,
    uint64_t persistent_value);

#ifdef __cplusplus
}

#ifndef LOCK_MODULE_IMPLEMENTATION
#define mylite_ownerless_innodb_lock_has_hooks() \
    mylite_ownerless_innodb_lock_hooks_enabled_fast()
#define mylite_ownerless_innodb_autoinc_has_hooks() \
    mylite_ownerless_innodb_autoinc_hooks_enabled_fast()
#endif
#endif

#endif

#ifndef MYLITE_OWNERLESS_PAGE_LOG_H
#define MYLITE_OWNERLESS_PAGE_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_PAGE_LOG_OK 0
#define MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND 1
#define MYLITE_OWNERLESS_PAGE_LOG_FULL 2
#define MYLITE_OWNERLESS_PAGE_LOG_ERROR 3
#define MYLITE_OWNERLESS_PAGE_LOG_BUSY 4

#define MYLITE_OWNERLESS_PAGE_LOG_RECORD_SNAPSHOT_BOUNDARY 128U
#define MYLITE_OWNERLESS_PAGE_LOG_RECORD_EXTERNAL_SNAPSHOT_LINEAGE 256U
#define MYLITE_OWNERLESS_PAGE_LOG_RECORD_NATIVE_SUPPORT_STATE 1024U
#define MYLITE_OWNERLESS_PAGE_LOG_RECORD_PROOF_ONLY 2048U
#define MYLITE_OWNERLESS_PAGE_LOG_RECORD_METADATA_CHECKSUM 4096U
#define MYLITE_OWNERLESS_PAGE_LOG_RECORD_HISTORY_RSEG_PAIR 8192U
#define MYLITE_OWNERLESS_PAGE_LOG_RECORD_HISTORY_RSEG_DELTA 512U

#define MYLITE_OWNERLESS_PAGE_LOG_APPEND_HISTORY_RSEG_DELTA 1U
#define MYLITE_OWNERLESS_PAGE_LOG_APPEND_NATIVE_SUPPORT_STATE 2U
#define MYLITE_OWNERLESS_PAGE_LOG_APPEND_PROOF_ONLY 4U
#define MYLITE_OWNERLESS_PAGE_LOG_APPEND_HISTORY_RSEG_PAIR 8U

#define MYLITE_OWNERLESS_PAGE_LOG_FIND_HISTORY_RSEG_DELTA 1U

#define MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE 12288U
#define MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE 64U

typedef int (*mylite_ownerless_page_log_replay_callback)(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    uint64_t record_offset,
    void *context
);
typedef int (*mylite_ownerless_page_log_checkpoint_complete_callback)(void *context);
typedef int (*mylite_ownerless_page_log_checkpoint_prepare_callback)(void *context);

typedef struct mylite_ownerless_page_log_append_session {
    int active;
    uint64_t log_offset;
    uint64_t next_record_offset;
    uint64_t log_device;
    uint64_t log_inode;
    uint64_t log_generation;
} mylite_ownerless_page_log_append_session;

int mylite_ownerless_page_log_initialize(int fd);
int mylite_ownerless_page_log_initialize_at(int fd, uint64_t log_offset);
int mylite_ownerless_page_log_register_checkpoint_stage(int fd, int stage_fd);
void mylite_ownerless_page_log_unregister_checkpoint_stage(int fd);
int mylite_ownerless_page_log_retire_process_lock(int fd);
int mylite_ownerless_page_log_test_faults_enabled(void);
void mylite_ownerless_page_log_test_inject_unlock_failure_once(void);
int mylite_ownerless_page_log_append(
    int fd,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_at(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_initialized_at(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t *out_record_offset
);
uint64_t mylite_ownerless_page_log_checksum_page(const void *page, uint32_t page_size);
int mylite_ownerless_page_log_append_initialized_at_with_checksum(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t page_checksum,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_initialized_at_with_checksum_and_options(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t page_checksum,
    uint32_t append_options,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_initialized_at_with_checksum_and_options_next(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t page_checksum,
    uint32_t append_options,
    uint64_t *out_record_offset,
    uint64_t *out_next_record_offset
);
int mylite_ownerless_page_log_append_snapshot_boundary_initialized_at(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_snapshot_boundary_initialized_at_next(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t *out_record_offset,
    uint64_t *out_next_record_offset
);
int mylite_ownerless_page_log_append_external_snapshot_lineage_initialized_at(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_external_snapshot_lineage_initialized_at_with_checksum(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t page_checksum,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_external_snapshot_lineage_initialized_at_with_checksum_and_options(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t page_checksum,
    uint32_t append_options,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_external_snapshot_lineage_initialized_at_with_checksum_and_options_next(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t page_checksum,
    uint32_t append_options,
    uint64_t *out_record_offset,
    uint64_t *out_next_record_offset
);
int mylite_ownerless_page_log_append_session_begin_initialized_at(
    int fd,
    uint64_t log_offset,
    mylite_ownerless_page_log_append_session *session
);
int mylite_ownerless_page_log_append_session_append(
    int fd,
    mylite_ownerless_page_log_append_session *session,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_session_append_with_checksum(
    int fd,
    mylite_ownerless_page_log_append_session *session,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t page_checksum,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_session_append_with_checksum_and_options(
    int fd,
    mylite_ownerless_page_log_append_session *session,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t page_checksum,
    uint32_t append_options,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_session_append_native_support_proof_pair(
    int fd,
    mylite_ownerless_page_log_append_session *session,
    uint32_t space_id,
    uint32_t first_page_no,
    uint64_t first_page_lsn,
    uint32_t first_page_size,
    uint32_t second_page_no,
    uint64_t second_page_lsn,
    uint32_t second_page_size,
    uint64_t commit_lsn,
    uint64_t *out_first_record_offset,
    uint64_t *out_second_record_offset
);
int mylite_ownerless_page_log_append_external_snapshot_lineage_session_append_with_checksum_and_options(
    int fd,
    mylite_ownerless_page_log_append_session *session,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t page_checksum,
    uint32_t append_options,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_session_end(
    int fd,
    mylite_ownerless_page_log_append_session *session
);
int mylite_ownerless_page_log_sync(int fd);
int mylite_ownerless_page_log_sync_at(int fd, uint64_t log_offset);
int mylite_ownerless_page_log_sync_initialized_at(int fd, uint64_t log_offset);
int mylite_ownerless_page_log_sync_initialized_if_changed_at(
    int fd,
    uint64_t log_offset,
    uint64_t known_synced_end_offset,
    uint64_t known_synced_generation,
    uint64_t *out_current_end_offset,
    uint64_t *out_current_generation,
    int *out_synced
);
int mylite_ownerless_page_log_record_is_snapshot_boundary_at(
    int fd,
    uint64_t record_offset,
    int *out_is_snapshot_boundary
);
int mylite_ownerless_page_log_record_is_external_snapshot_lineage_at(
    int fd,
    uint64_t record_offset,
    int *out_is_external_snapshot_lineage
);
int mylite_ownerless_page_log_record_is_native_support_state_at(
    int fd,
    uint64_t record_offset,
    int *out_is_native_support_state
);
int mylite_ownerless_page_log_record_metadata_flags_at(
    int fd,
    uint64_t record_offset,
    uint32_t *out_metadata_flags
);
int mylite_ownerless_page_log_record_page_size_at(
    int fd,
    uint64_t record_offset,
    uint32_t *out_page_size
);
int mylite_ownerless_page_log_record_next_offset_at(
    int fd,
    uint64_t log_offset,
    uint64_t record_offset,
    uint64_t *out_next_record_offset
);
int mylite_ownerless_page_log_has_readable_page_records_at(
    int fd,
    uint64_t log_offset,
    int *out_has_records
);
int mylite_ownerless_page_log_snapshot(int fd, uint64_t *out_snapshot_end_offset);
int mylite_ownerless_page_log_snapshot_at(
    int fd,
    uint64_t log_offset,
    uint64_t *out_snapshot_end_offset
);
int mylite_ownerless_page_log_snapshot_under_read_lock_at(
    int fd,
    uint64_t log_offset,
    uint64_t *out_snapshot_end_offset,
    uint64_t *out_log_generation
);
int mylite_ownerless_page_log_begin_read(int fd);
int mylite_ownerless_page_log_end_read(int fd);
int mylite_ownerless_page_log_find_latest(
    int fd,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_find_latest_at(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_find_latest_under_read_lock_at(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_find_latest_under_read_lock_at_with_flags(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags
);
int mylite_ownerless_page_log_find_latest_under_read_lock_at_with_flags_and_options(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags,
    uint32_t find_options
);
int mylite_ownerless_page_log_find_latest_in_snapshot(
    int fd,
    uint64_t snapshot_end_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_find_latest_in_snapshot_at(
    int fd,
    uint64_t log_offset,
    uint64_t snapshot_end_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at(
    int fd,
    uint64_t log_offset,
    uint64_t scan_start_offset,
    uint64_t snapshot_end_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    int *out_saw_page_record
);
int mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at_with_flags(
    int fd,
    uint64_t log_offset,
    uint64_t scan_start_offset,
    uint64_t snapshot_end_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags,
    int *out_saw_page_record
);
int mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at_with_flags_and_options(
    int fd,
    uint64_t log_offset,
    uint64_t scan_start_offset,
    uint64_t snapshot_end_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags,
    int *out_saw_page_record,
    uint32_t find_options
);
int mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at_with_flags_and_offset(
    int fd,
    uint64_t log_offset,
    uint64_t scan_start_offset,
    uint64_t snapshot_end_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags,
    uint64_t *out_record_offset,
    int *out_saw_page_record
);
int mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at_with_flags_and_offset_and_options(
    int fd,
    uint64_t log_offset,
    uint64_t scan_start_offset,
    uint64_t snapshot_end_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags,
    uint64_t *out_record_offset,
    int *out_saw_page_record,
    uint32_t find_options
);
int mylite_ownerless_page_log_read_record_at(
    int fd,
    uint64_t log_offset,
    uint64_t record_offset,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_read_page_at(
    int fd,
    uint64_t log_offset,
    uint64_t record_offset,
    uint32_t space_id,
    uint32_t page_no,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_read_page_under_read_lock_at(
    int fd,
    uint64_t log_offset,
    uint64_t record_offset,
    uint32_t space_id,
    uint32_t page_no,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_read_page_under_read_lock_at_with_flags(
    int fd,
    uint64_t log_offset,
    uint64_t record_offset,
    uint32_t space_id,
    uint32_t page_no,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags
);
int mylite_ownerless_page_log_replay_at(
    int fd,
    uint64_t log_offset,
    mylite_ownerless_page_log_replay_callback callback,
    void *context
);
int mylite_ownerless_page_log_replay_at_including_proof_only(
    int fd,
    uint64_t log_offset,
    mylite_ownerless_page_log_replay_callback callback,
    void *context
);
int mylite_ownerless_page_log_replay_stable_with_completion_at(
    int fd,
    uint64_t log_offset,
    mylite_ownerless_page_log_replay_callback callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint(
    int fd,
    uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint_with_completion(
    int fd,
    uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint_at(
    int fd,
    uint64_t log_offset,
    uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint_with_completion_at(
    int fd,
    uint64_t log_offset,
    uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint_retaining_native_support_at(
    int fd,
    uint64_t log_offset,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
    int fd,
    uint64_t log_offset,
    uint64_t safe_commit_lsn,
    uint64_t oldest_snapshot_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_prepare_callback prepare_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint_preserving_single_snapshot_at(
    int fd,
    uint64_t log_offset,
    uint64_t safe_commit_lsn,
    uint64_t snapshot_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_prepare_callback prepare_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint_if_safe(
    int fd,
    uint64_t safe_commit_lsn,
    int *out_checkpointed
);
int mylite_ownerless_page_log_checkpoint_if_safe_at(
    int fd,
    uint64_t log_offset,
    uint64_t safe_commit_lsn,
    int *out_checkpointed
);

#ifdef __cplusplus
}
#endif

#endif

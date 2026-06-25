#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ownerless_autoinc_registry.h"
#include "ownerless_dictionary_state.h"
#include "ownerless_innodb_lock_registry.h"
#include "ownerless_latch.h"
#include "ownerless_lock_table.h"
#include "ownerless_mdl.h"
#include "ownerless_page_index.h"
#include "ownerless_page_log.h"
#include "ownerless_page_pin_registry.h"
#include "ownerless_probe.h"
#include "ownerless_process_registry.h"
#include "ownerless_read_view_registry.h"
#include "ownerless_redo_state.h"
#include "ownerless_tablespace_replay.h"
#include "ownerless_trx_registry.h"
#include "ownerless_wait.h"

#include "mylite_ownerless_innodb_lock_hooks.h"
#include "ownerless_test_latch_compat.h"

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_PAGE_SIZE 4096
#define MYLITE_TEST_WAIT_TIMEOUT_MS 5000U
#define MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT 4U
#define MYLITE_TEST_LOCK_TABLE_LATCH_OFFSET 24U
#define MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT 8U
#define MYLITE_TEST_AUTOINC_REGISTRY_SLOT_COUNT 2U
#define MYLITE_TEST_INNODB_LOCK_REGISTRY_LATCH_OFFSET 24U
#define MYLITE_TEST_INNODB_LOCK_REGISTRY_ACTIVE_COUNT_OFFSET 16U
#define MYLITE_TEST_INNODB_LOCK_REGISTRY_OCCUPIED_LIMIT_OFFSET 72U
#define MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_STATE_OFFSET 12U
#define MYLITE_TEST_LOCK_HASH 0xAABBCCDDEEFF0011ULL
#define MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT 4U
#define MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT 4U
#define MYLITE_TEST_READ_VIEW_REGISTRY_SLOT_COUNT 4U
#define MYLITE_TEST_PAGE_PIN_REGISTRY_SLOT_COUNT 4U
#define MYLITE_TEST_REDO_STATE_PROGRESS_LATCH_OFFSET 96U
#define MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET 4U
#define MYLITE_TEST_INNODB_PAGE_LSN_OFFSET 16U
#define MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET 24U
#define MYLITE_TEST_INNODB_FIL_HEADER_SIZE 38U
#define MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET 34U
#define MYLITE_TEST_INNODB_PAGE_TYPE_INDEX 17855U
#define MYLITE_TEST_INNODB_PAGE_TYPE_UNDO_LOG 2U
#define MYLITE_TEST_INNODB_PAGE_TYPE_SYS 6U
#define MYLITE_TEST_INNODB_PAGE_TYPE_TRX_SYS 7U
#define MYLITE_TEST_INNODB_PAGE_TYPE_FSP_HEADER 8U
#define MYLITE_TEST_INNODB_PAGE_TYPE_RTREE 17854U
#define MYLITE_TEST_PAGE_LOG_RECORD_FLAGS_OFFSET 20U
#define MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA 32U
#define MYLITE_TEST_PAGE_LOG_RECORD_FLAG_UNDO_DELTA 64U
#define MYLITE_TEST_PAGE_LOG_RECORD_FLAG_HISTORY_RSEG_DELTA 512U
#define MYLITE_TEST_PAGE_LOG_RECORD_FLAG_EXTERNAL_SNAPSHOT_LINEAGE 256U
#define MYLITE_TEST_PAGE_LOG_RECORD_FLAG_NATIVE_SUPPORT_STATE 1024U
#define MYLITE_TEST_PAGE_LOG_RECORD_FLAG_PROOF_ONLY 2048U
#define MYLITE_TEST_PAGE_LOG_RECORD_PAYLOAD_CHECKSUM_OFFSET 48U

typedef struct byte_range_lock {
    off_t start;
    off_t length;
} byte_range_lock;

typedef struct cleanup_owner_locks_context {
    void *lock_table;
    uint32_t released_entries;
} cleanup_owner_locks_context;

typedef struct page_log_replay_context {
    void *page_index;
    size_t page_index_size;
} page_log_replay_context;

typedef struct page_log_retained_records {
    mylite_ownerless_page_index_record records[8];
    size_t count;
} page_log_retained_records;

typedef struct page_log_checkpoint_index_context {
    void *page_index;
    size_t page_index_size;
    page_log_retained_records retained;
    unsigned prepare_count;
} page_log_checkpoint_index_context;

typedef struct redo_reserve_thread_context {
    void *state;
    size_t state_size;
    uint64_t *starts;
    size_t offset;
    size_t count;
} redo_reserve_thread_context;

enum page_log_append_perf_stat_index {
    PAGE_LOG_APPEND_PERF_STAT_CALLS = 0,
    PAGE_LOG_APPEND_PERF_STAT_TOTAL_NS,
    PAGE_LOG_APPEND_PERF_STAT_LOCK_NS,
    PAGE_LOG_APPEND_PERF_STAT_HEADER_NS,
    PAGE_LOG_APPEND_PERF_STAT_BODY_NS,
    PAGE_LOG_APPEND_PERF_STAT_FSTAT_NS,
    PAGE_LOG_APPEND_PERF_STAT_CHECKSUM_NS,
    PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_WRITE_NS,
    PAGE_LOG_APPEND_PERF_STAT_RECORD_HEADER_WRITE_NS,
    PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_RECORD_HEADER_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_ENCODE_NS,
    PAGE_LOG_APPEND_PERF_STAT_FULL_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_TRAILING_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_FULL_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_TRAILING_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_LOG_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_LOG_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_SYS_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_SYS_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_TRX_SYS_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_TRX_SYS_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_BLOB_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_BLOB_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_OTHER_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_OTHER_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_LOG_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_LOG_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_SYS_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_SYS_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_TRX_SYS_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_TRX_SYS_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_BLOB_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_BLOB_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_OTHER_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_OTHER_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_VARINT_COMPACT_SPARSE_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_VARINT_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_RAW_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_FILL_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_UNIQUE,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_DUPLICATE,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_SIZE_MISMATCH,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_TABLE_OVERFLOW,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_CHANGED_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_FIL_HEADER_CHANGED_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_BODY_CHANGED_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_DIRECT_APPEND_CALLS,
    PAGE_LOG_APPEND_PERF_STAT_SESSION_BEGIN_CALLS,
    PAGE_LOG_APPEND_PERF_STAT_SESSION_APPEND_CALLS,
    PAGE_LOG_APPEND_PERF_STAT_SESSION_END_CALLS,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_FAST_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_SNAPSHOT_NS,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_ENCODE_NS,
    PAGE_LOG_APPEND_PERF_STAT_STANDALONE_ENCODE_NS,
    PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_STATS_NS,
    PAGE_LOG_APPEND_PERF_STAT_PAGE_TYPE_STATS_NS,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_BASE_NOTE_NS,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_EXACT_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_EXACT_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_FAST_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_EXACT_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_EXACT_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_FAST_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_FAST_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_EXACT_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_EXACT_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_LIMIT_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_LIMIT_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_STANDALONE_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_STANDALONE_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_BUILD_FAILURES,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_SKIPPED_STANDALONE_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_SKIPPED_STANDALONE_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_BUILD_FAILURES,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REUSED_FAST_PAYLOAD_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REUSED_FAST_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS,
    PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_NS,
    PAGE_LOG_APPEND_PERF_STAT_STANDALONE_MATERIALIZE_SKIPPED_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_STANDALONE_MATERIALIZE_SKIPPED_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_BASE_STANDALONE_SLOT_REUSE_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_DELTA_BASE_PAGE_BUFFER_REUSE_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_PRECOMPUTED_CHECKSUM_RECORDS,
    PAGE_LOG_APPEND_PERF_STAT_RECORD_HEADER_WRITE_CALLS,
    PAGE_LOG_APPEND_PERF_STAT_COUNT
};

enum page_log_scan_perf_stat_index {
    PAGE_LOG_SCAN_PERF_STAT_CALLS = 0,
    PAGE_LOG_SCAN_PERF_STAT_RECORD_HEADERS,
    PAGE_LOG_SCAN_PERF_STAT_PAGE_RECORDS,
    PAGE_LOG_SCAN_PERF_STAT_VISIBLE_PAGE_RECORDS,
    PAGE_LOG_SCAN_PERF_STAT_FOUND,
    PAGE_LOG_SCAN_PERF_STAT_NOT_FOUND_NO_PAGE_RECORD,
    PAGE_LOG_SCAN_PERF_STAT_NOT_FOUND_PAGE_RECORD_NOT_VISIBLE,
    PAGE_LOG_SCAN_PERF_STAT_ERRORS,
    PAGE_LOG_SCAN_PERF_STAT_STREAM_CHECKSUM_RECORDS,
    PAGE_LOG_SCAN_PERF_STAT_STREAM_CHECKSUM_BYTES,
    PAGE_LOG_SCAN_PERF_STAT_COUNT
};

enum page_log_sync_perf_stat_index {
    PAGE_LOG_SYNC_PERF_STAT_CALLS = 0,
    PAGE_LOG_SYNC_PERF_STAT_TOTAL_NS,
    PAGE_LOG_SYNC_PERF_STAT_LOCK_NS,
    PAGE_LOG_SYNC_PERF_STAT_HEADER_NS,
    PAGE_LOG_SYNC_PERF_STAT_DATA_SYNC_NS,
    PAGE_LOG_SYNC_PERF_STAT_SKIPPED_CLEAN,
    PAGE_LOG_SYNC_PERF_STAT_COUNT
};

void mylite_ownerless_page_log_set_append_perf_stats_enabled(int enabled);
void mylite_ownerless_page_log_set_append_detail_perf_stats_enabled(int enabled);
void mylite_ownerless_page_log_reset_append_perf_stats(void);
void mylite_ownerless_page_log_read_append_perf_stats(uint64_t *out_values, size_t value_count);
void mylite_ownerless_page_log_set_scan_perf_stats_enabled(int enabled);
void mylite_ownerless_page_log_reset_scan_perf_stats(void);
void mylite_ownerless_page_log_read_scan_perf_stats(uint64_t *out_values, size_t value_count);
void mylite_ownerless_page_log_set_sync_perf_stats_enabled(int enabled);
void mylite_ownerless_page_log_reset_sync_perf_stats(void);
void mylite_ownerless_page_log_read_sync_perf_stats(uint64_t *out_values, size_t value_count);

static void test_mmap_shared_visibility_across_processes(void);
static void test_fcntl_byte_range_lock_conflict(void);
static void test_fcntl_byte_range_lock_release_on_process_exit(void);
static void test_mmap_grow_and_remap(void);
static void test_wait_backend_wakes_across_processes(void);
static void test_wait_backend_times_out_without_change(void);
static void test_latch_records_owner_generation_and_wakes_waiter(void);
static void test_latch_reports_dead_owner_without_stealing(void);
static void test_platform_probe_records_required_primitives(void);
static void test_directory_probe_records_required_primitives(void);
static void test_page_log_reads_latest_visible_page(void);
static void test_page_log_uses_payload_offset(void);
static void test_page_log_append_reports_write_volume(void);
static void test_page_log_append_uses_precomputed_checksum(void);
static void test_page_log_append_detail_stats_are_opt_in(void);
static void test_page_log_encodes_sparse_zero_payloads(void);
static void test_page_log_encodes_fill_sparse_zero_payloads(void);
static void test_page_log_streams_sparse_checksum_validation(void);
static void test_page_log_rejects_corrupt_sparse_checksum_records(void);
static void test_page_log_encodes_index_fill_sparse_zero_payloads(void);
static void test_page_log_attributes_index_page_identity_deltas(void);
static void test_page_log_encodes_index_delta_payloads(void);
static void test_page_log_encodes_undo_delta_payloads(void);
static void test_page_log_keeps_sys_pages_standalone(void);
static void test_page_log_encodes_history_rseg_delta_payloads(void);
static void assert_page_log_encodes_history_rseg_delta_payload(
    uint16_t page_type,
    uint32_t page_no,
    uint64_t base_lsn,
    const char *log_name,
    enum page_log_append_perf_stat_index page_type_stat
);
static void test_page_log_fast_encodes_small_index_delta_payloads(void);
static void test_page_log_fast_encodes_medium_index_delta_payloads(void);
static void test_page_log_reuses_fast_miss_delta_payload_for_exact_fallback(void);
static void test_page_log_exact_probe_preserves_fill_sparse_rejection(void);
static void test_page_log_skips_repeated_exact_standalone_probe(void);
static void test_page_log_refreshes_delta_standalone_size_estimate(void);
static void test_page_log_reuses_delta_base_slot_for_standalone_refresh(void);
static void test_page_log_refreshes_index_delta_base(void);
static void test_page_log_skips_index_fill_sparse_without_fill_runs(void);
static void test_page_log_falls_back_to_compact_sparse_zero_payloads(void);
static void test_page_log_falls_back_to_legacy_sparse_zero_payloads(void);
static void test_page_log_initialized_append_uses_existing_header(void);
static void test_page_log_initialized_sync_uses_existing_header(void);
static void test_page_log_reads_under_existing_read_lock(void);
static void test_page_log_tail_scan_absence_generation(void);
static void test_page_log_accepts_legacy_checksum_records(void);
static void test_page_log_uses_reader_snapshots(void);
static void test_page_log_tolerates_corrupt_tail_record(void);
static void test_page_log_rejects_corrupt_interior_record(void);
static void test_page_log_checkpoints_retained_records(void);
static void test_page_log_preserves_oldest_snapshot_boundary(void);
static void test_page_log_preserves_external_snapshot_lineage_metadata(void);
static void test_page_log_external_snapshot_lineage_session_append(void);
static void test_page_log_preserves_native_support_metadata(void);
static void test_page_log_skips_proof_only_native_support_records(void);
static void test_page_log_appends_native_support_proof_pairs(void);
static void test_page_log_readable_record_scan_ignores_proof_only_records(void);
static void test_page_log_requires_boundaries_only_for_snapshot_pages(void);
static void test_page_log_checkpoint_waits_for_readers(void);
static void test_page_log_scan_recovers_from_stale_index_offset(void);
static void test_page_log_rejects_stale_index_offset_identity(void);
static void test_page_log_checkpoints_when_all_records_are_safe(void);
static void test_page_log_replays_record_offsets(void);
static void test_tablespace_replay_applies_visible_page_versions(void);
static void test_tablespace_replay_uses_latest_visible_page_lsn(void);
static void test_tablespace_replay_rewinds_newer_disk_page(void);
static void test_tablespace_replay_rewrites_same_lsn_different_image(void);
static void test_tablespace_replay_can_keep_native_same_lsn_page(void);
static void test_tablespace_replay_ignores_non_fsp_page_zero_candidates(void);
static void test_tablespace_replay_rejects_ambiguous_tablespace(void);
static void test_tablespace_replay_rejects_missing_tablespace(void);
static void test_tablespace_read_finds_native_snapshot_boundary(void);
static int replay_page_log_record_into_index(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    uint64_t record_offset,
    void *context
);
static void test_page_log_serializes_cross_process_appends(void);
static void test_page_index_publishes_latest_record_offsets(void);
static void test_page_index_replace_restores_index_after_wal_scan(void);
static void test_page_index_overflow_requires_wal_scan(void);
static void test_page_index_publishes_across_processes(void);
static int capture_page_log_record_for_index_replace(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    uint64_t record_offset,
    void *context
);
static int capture_page_log_record_for_checkpoint_index(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    uint64_t record_offset,
    void *context
);
static int prepare_page_log_checkpoint(void *context);
static int replace_page_index_after_page_log_checkpoint(void *context);
static void test_lock_table_allows_cross_process_shared_holders(void);
static void test_lock_table_upgradable_is_compatible_with_shared_holders(void);
static void test_lock_table_nonblocking_acquire_waits_for_latch(void);
static void test_lock_table_metadata_modes_follow_mariadb_matrix(void);
static void test_lock_table_waits_for_conflicting_owner_release(void);
static void test_lock_table_conflicting_owner_times_out(void);
static void test_lock_table_exclusive_waits_for_shared_release(void);
static void test_lock_table_counts_repeated_owner_acquisitions(void);
static void test_lock_table_allows_same_owner_mode_upgrade(void);
static void test_lock_table_releases_all_owner_locks(void);
static void test_innodb_lock_registry_table_compatibility(void);
static void test_innodb_lock_registry_record_compatibility(void);
static void test_innodb_lock_registry_nonblocking_reserve_waits_for_latch(void);
static void test_innodb_lock_registry_wait_edges_and_deadlocks(void);
static void test_innodb_lock_registry_table_waiter_death_requires_owner_cleanup(void);
static void test_innodb_lock_registry_detects_cross_registry_deadlocks(void);
static void test_innodb_lock_registry_detects_page_write_gate_deadlocks(void);
static void test_innodb_lock_registry_page_write_gates_cover_physical_pages(void);
static void test_innodb_lock_registry_page_write_owner_bypasses_blocked_waiter(void);
static void test_innodb_lock_registry_owner_blocks_waiting_lock(void);
static void test_innodb_lock_registry_same_page_waiter_fairness(void);
static void test_innodb_lock_registry_wait_until_rechecks_available_after_missed_wake(void);
static void test_innodb_lock_registry_waits_across_processes(void);
static void test_innodb_lock_registry_references_and_owner_cleanup(void);
static void test_innodb_lock_registry_shrinks_scan_limit(void);
static void test_autoinc_registry_preserves_high_watermarks(void);
static void test_mdl_key_hashes_are_stable_and_distinct(void);
static void test_mdl_upgradable_is_compatible_with_shared_holders(void);
static void test_mdl_metadata_modes_follow_mariadb_matrix(void);
static void test_mdl_table_lock_waits_across_processes(void);
static void test_trx_registry_allocates_cross_process_ids(void);
static void test_trx_registry_rejects_stale_end(void);
static void test_trx_registry_reports_full_when_slots_exhausted(void);
static void test_trx_registry_snapshots_active_ids(void);
static void test_trx_registry_assigns_read_view_serialisation_numbers(void);
static void test_trx_registry_bumps_next_id_and_ends_by_owner_id(void);
static void test_trx_registry_releases_dead_owner_transactions(void);
static void test_read_view_registry_snapshots_oldest_views(void);
static void test_read_view_registry_snapshots_cross_process_views(void);
static void test_read_view_registry_releases_dead_owner_views(void);
static void test_page_pin_registry_snapshots_oldest_pins(void);
static void test_page_pin_registry_snapshots_cross_process_pins(void);
static void test_page_pin_registry_releases_dead_owner_pins(void);
static void test_dictionary_state_serializes_ddl_generations(void);
static void test_dictionary_state_reports_dead_active_owner(void);
static void test_dictionary_state_recovers_marked_dead_owner(void);
static void test_redo_state_tracks_lsn_and_owner_lifecycle(void);
static void test_redo_state_seeds_checkpoint_monotonically(void);
static void test_redo_state_reserves_ranges_for_same_owner_threads(void);
static void test_redo_state_allows_bounded_fanout_reservations(void);
static void test_redo_state_deferred_batch_cap_preserves_peer_headroom(void);
static void test_redo_state_tracks_contiguous_written_ranges(void);
static void test_redo_state_combines_write_and_leave_like_separate_steps(void);
static void test_redo_state_batches_write_and_leave_ranges(void);
static void *reserve_redo_ranges_in_thread(void *context);
static int compare_uint64_values(const void *left, const void *right);
static void test_process_registry_allocates_cross_process_slots(void);
static void test_process_registry_rejects_stale_release(void);
static void test_process_registry_updates_heartbeat(void);
static void test_process_registry_cleans_dead_slots(void);
static void test_process_registry_cleanup_callback_releases_owner_locks(void);
static void test_process_registry_cleanup_callback_can_block_cleanup(void);
static void test_process_registry_counts_live_slots(void);
static void test_process_registry_cleans_exited_process_slot(void);
static int process_registry_test_pid_is_alive(uint64_t pid, void *ctx);
static int process_registry_pid_is_running(uint64_t pid, void *ctx);
static int dictionary_state_pid_is_alive(uint64_t pid, void *ctx);
static int process_registry_cleanup_owner_locks(
    uint32_t slot_index,
    uint64_t slot_generation,
    uint64_t pid,
    void *ctx
);
static int process_registry_cleanup_blocks_owner(
    uint32_t slot_index,
    uint64_t slot_generation,
    uint64_t pid,
    void *ctx
);
static int latch_test_owner_is_alive(uint32_t owner_id, uint64_t owner_generation, void *ctx);
static void set_write_lock(int fd, byte_range_lock range);
static int try_write_lock(int fd, byte_range_lock range);
static void unlock_range(int fd, byte_range_lock range);
static int open_file(const char *path);
static void truncate_file(int fd, off_t size);
static void write_file_at(int fd, const void *data, size_t size, off_t offset);
static void read_file_at(int fd, void *data, size_t size, off_t offset);
static void fill_innodb_test_page(
    uint8_t *page,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint8_t marker
);
static uint64_t innodb_test_page_lsn(const uint8_t *page);
static void store_test_be16(uint8_t *bytes, size_t offset, uint16_t value);
static void store_test_be32(uint8_t *bytes, size_t offset, uint32_t value);
static void store_test_be64(uint8_t *bytes, size_t offset, uint64_t value);
static void store_test_le64(uint8_t *bytes, size_t offset, uint64_t value);
static uint32_t load_test_le32(const uint8_t *bytes, size_t offset);
static uint64_t load_test_be64(const uint8_t *bytes, size_t offset);
static uint32_t read_page_log_record_flags(int fd, uint64_t record_offset);
static uint64_t legacy_page_log_checksum(const void *buffer, size_t size);
static uint32_t innodb_lock_registry_occupied_limit(void *registry);
static void *map_file(int fd, size_t size);
static void signal_pipe(int pipe_fd);
static void wait_for_pipe(int pipe_fd);
static void wait_for_child(pid_t child);
static void sleep_milliseconds(unsigned milliseconds);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static int path_exists(const char *path);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

int main(void) {
    test_mmap_shared_visibility_across_processes();
    test_fcntl_byte_range_lock_conflict();
    test_fcntl_byte_range_lock_release_on_process_exit();
    test_mmap_grow_and_remap();
    test_wait_backend_wakes_across_processes();
    test_wait_backend_times_out_without_change();
    test_latch_records_owner_generation_and_wakes_waiter();
    test_latch_reports_dead_owner_without_stealing();
    test_platform_probe_records_required_primitives();
    test_directory_probe_records_required_primitives();
    test_page_log_reads_latest_visible_page();
    test_page_log_uses_payload_offset();
    test_page_log_append_reports_write_volume();
    test_page_log_append_uses_precomputed_checksum();
    test_page_log_append_detail_stats_are_opt_in();
    test_page_log_encodes_sparse_zero_payloads();
    test_page_log_encodes_fill_sparse_zero_payloads();
    test_page_log_streams_sparse_checksum_validation();
    test_page_log_rejects_corrupt_sparse_checksum_records();
    test_page_log_encodes_index_fill_sparse_zero_payloads();
    test_page_log_attributes_index_page_identity_deltas();
    test_page_log_encodes_index_delta_payloads();
    test_page_log_encodes_undo_delta_payloads();
    test_page_log_keeps_sys_pages_standalone();
    test_page_log_encodes_history_rseg_delta_payloads();
    test_page_log_fast_encodes_small_index_delta_payloads();
    test_page_log_fast_encodes_medium_index_delta_payloads();
    test_page_log_reuses_fast_miss_delta_payload_for_exact_fallback();
    test_page_log_exact_probe_preserves_fill_sparse_rejection();
    test_page_log_skips_repeated_exact_standalone_probe();
    test_page_log_refreshes_delta_standalone_size_estimate();
    test_page_log_reuses_delta_base_slot_for_standalone_refresh();
    test_page_log_refreshes_index_delta_base();
    test_page_log_skips_index_fill_sparse_without_fill_runs();
    test_page_log_falls_back_to_compact_sparse_zero_payloads();
    test_page_log_falls_back_to_legacy_sparse_zero_payloads();
    test_page_log_initialized_append_uses_existing_header();
    test_page_log_initialized_sync_uses_existing_header();
    test_page_log_reads_under_existing_read_lock();
    test_page_log_tail_scan_absence_generation();
    test_page_log_accepts_legacy_checksum_records();
    test_page_log_uses_reader_snapshots();
    test_page_log_tolerates_corrupt_tail_record();
    test_page_log_rejects_corrupt_interior_record();
    test_page_log_checkpoints_retained_records();
    test_page_log_preserves_oldest_snapshot_boundary();
    test_page_log_preserves_external_snapshot_lineage_metadata();
    test_page_log_external_snapshot_lineage_session_append();
    test_page_log_preserves_native_support_metadata();
    test_page_log_skips_proof_only_native_support_records();
    test_page_log_appends_native_support_proof_pairs();
    test_page_log_readable_record_scan_ignores_proof_only_records();
    test_page_log_requires_boundaries_only_for_snapshot_pages();
    test_page_log_checkpoint_waits_for_readers();
    test_page_log_scan_recovers_from_stale_index_offset();
    test_page_log_rejects_stale_index_offset_identity();
    test_page_log_checkpoints_when_all_records_are_safe();
    test_page_log_replays_record_offsets();
    test_tablespace_replay_applies_visible_page_versions();
    test_tablespace_replay_uses_latest_visible_page_lsn();
    test_tablespace_replay_rewinds_newer_disk_page();
    test_tablespace_replay_rewrites_same_lsn_different_image();
    test_tablespace_replay_can_keep_native_same_lsn_page();
    test_tablespace_replay_ignores_non_fsp_page_zero_candidates();
    test_tablespace_replay_rejects_ambiguous_tablespace();
    test_tablespace_replay_rejects_missing_tablespace();
    test_tablespace_read_finds_native_snapshot_boundary();
    test_page_log_serializes_cross_process_appends();
    test_page_index_publishes_latest_record_offsets();
    test_page_index_replace_restores_index_after_wal_scan();
    test_page_index_overflow_requires_wal_scan();
    test_page_index_publishes_across_processes();
    test_lock_table_allows_cross_process_shared_holders();
    test_lock_table_upgradable_is_compatible_with_shared_holders();
    test_lock_table_nonblocking_acquire_waits_for_latch();
    test_lock_table_metadata_modes_follow_mariadb_matrix();
    test_lock_table_waits_for_conflicting_owner_release();
    test_lock_table_conflicting_owner_times_out();
    test_lock_table_exclusive_waits_for_shared_release();
    test_lock_table_counts_repeated_owner_acquisitions();
    test_lock_table_allows_same_owner_mode_upgrade();
    test_lock_table_releases_all_owner_locks();
    test_innodb_lock_registry_table_compatibility();
    test_innodb_lock_registry_record_compatibility();
    test_innodb_lock_registry_nonblocking_reserve_waits_for_latch();
    test_innodb_lock_registry_wait_edges_and_deadlocks();
    test_innodb_lock_registry_table_waiter_death_requires_owner_cleanup();
    test_innodb_lock_registry_detects_cross_registry_deadlocks();
    test_innodb_lock_registry_detects_page_write_gate_deadlocks();
    test_innodb_lock_registry_page_write_gates_cover_physical_pages();
    test_innodb_lock_registry_page_write_owner_bypasses_blocked_waiter();
    test_innodb_lock_registry_owner_blocks_waiting_lock();
    test_innodb_lock_registry_same_page_waiter_fairness();
    test_innodb_lock_registry_wait_until_rechecks_available_after_missed_wake();
    test_innodb_lock_registry_waits_across_processes();
    test_innodb_lock_registry_references_and_owner_cleanup();
    test_innodb_lock_registry_shrinks_scan_limit();
    test_autoinc_registry_preserves_high_watermarks();
    test_mdl_key_hashes_are_stable_and_distinct();
    test_mdl_upgradable_is_compatible_with_shared_holders();
    test_mdl_metadata_modes_follow_mariadb_matrix();
    test_mdl_table_lock_waits_across_processes();
    test_trx_registry_allocates_cross_process_ids();
    test_trx_registry_rejects_stale_end();
    test_trx_registry_reports_full_when_slots_exhausted();
    test_trx_registry_snapshots_active_ids();
    test_trx_registry_assigns_read_view_serialisation_numbers();
    test_trx_registry_bumps_next_id_and_ends_by_owner_id();
    test_trx_registry_releases_dead_owner_transactions();
    test_read_view_registry_snapshots_oldest_views();
    test_read_view_registry_snapshots_cross_process_views();
    test_read_view_registry_releases_dead_owner_views();
    test_page_pin_registry_snapshots_oldest_pins();
    test_page_pin_registry_snapshots_cross_process_pins();
    test_page_pin_registry_releases_dead_owner_pins();
    test_dictionary_state_serializes_ddl_generations();
    test_dictionary_state_reports_dead_active_owner();
    test_dictionary_state_recovers_marked_dead_owner();
    test_redo_state_tracks_lsn_and_owner_lifecycle();
    test_redo_state_seeds_checkpoint_monotonically();
    test_redo_state_reserves_ranges_for_same_owner_threads();
    test_redo_state_allows_bounded_fanout_reservations();
    test_redo_state_deferred_batch_cap_preserves_peer_headroom();
    test_redo_state_tracks_contiguous_written_ranges();
    test_redo_state_combines_write_and_leave_like_separate_steps();
    test_redo_state_batches_write_and_leave_ranges();
    test_process_registry_allocates_cross_process_slots();
    test_process_registry_rejects_stale_release();
    test_process_registry_updates_heartbeat();
    test_process_registry_cleans_dead_slots();
    test_process_registry_cleanup_callback_releases_owner_locks();
    test_process_registry_cleanup_callback_can_block_cleanup();
    test_process_registry_counts_live_slots();
    test_process_registry_cleans_exited_process_slot();
    return 0;
}

static void test_mmap_shared_visibility_across_processes(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "mmap-shared.bin");
    int parent_to_child[2];
    int child_to_parent[2];
    int fd = open_file(shm_path);
    uint32_t *words;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    words = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    words[0] = 0x11223344U;
    assert(msync(words, sizeof(words[0]), MS_SYNC) == 0);
    assert(pipe(parent_to_child) == 0);
    assert(pipe(child_to_parent) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        uint32_t *child_words;

        close(parent_to_child[1]);
        close(child_to_parent[0]);
        wait_for_pipe(parent_to_child[0]);

        child_fd = open_file(shm_path);
        child_words = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        assert(child_words[0] == 0x55667788U);
        child_words[1] = 0x99AABBCCU;
        assert(msync(child_words, MYLITE_TEST_PAGE_SIZE, MS_SYNC) == 0);
        assert(munmap(child_words, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        signal_pipe(child_to_parent[1]);
        _exit(0);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);
    words[0] = 0x55667788U;
    assert(msync(words, sizeof(words[0]), MS_SYNC) == 0);
    signal_pipe(parent_to_child[1]);
    wait_for_pipe(child_to_parent[0]);
    assert(words[1] == 0x99AABBCCU);
    wait_for_child(child);

    assert(munmap(words, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_fcntl_byte_range_lock_conflict(void) {
    char *root = make_temp_root();
    char *lock_path = path_join(root, "range-lock.bin");
    int fd = open_file(lock_path);
    byte_range_lock range = {.start = 11, .length = 7};
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    set_write_lock(fd, range);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd = open_file(lock_path);
        int lock_result = try_write_lock(child_fd, range);

        assert(lock_result == EAGAIN || lock_result == EACCES);
        assert(close(child_fd) == 0);
        _exit(0);
    }
    wait_for_child(child);
    unlock_range(fd, range);

    assert(close(fd) == 0);
    free(lock_path);
    remove_tree(root);
    free(root);
}

static void test_fcntl_byte_range_lock_release_on_process_exit(void) {
    char *root = make_temp_root();
    char *lock_path = path_join(root, "release-on-exit.bin");
    int ready_pipe[2];
    int fd = open_file(lock_path);
    byte_range_lock range = {.start = 23, .length = 5};
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(pipe(ready_pipe) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;

        close(ready_pipe[0]);
        child_fd = open_file(lock_path);
        set_write_lock(child_fd, range);
        signal_pipe(ready_pipe[1]);
        _exit(0);
    }

    close(ready_pipe[1]);
    wait_for_pipe(ready_pipe[0]);
    wait_for_child(child);
    set_write_lock(fd, range);
    unlock_range(fd, range);

    assert(close(fd) == 0);
    free(lock_path);
    remove_tree(root);
    free(root);
}

static void test_mmap_grow_and_remap(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "grow-remap.bin");
    int fd = open_file(shm_path);
    uint32_t *first_mapping;
    uint32_t *second_mapping;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    first_mapping = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    first_mapping[0] = 0xCAFEBABEU;
    assert(msync(first_mapping, sizeof(first_mapping[0]), MS_SYNC) == 0);
    assert(munmap(first_mapping, MYLITE_TEST_PAGE_SIZE) == 0);

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE * 2);
    second_mapping = map_file(fd, MYLITE_TEST_PAGE_SIZE * 2);
    assert(second_mapping[0] == 0xCAFEBABEU);
    second_mapping[MYLITE_TEST_PAGE_SIZE / sizeof(second_mapping[0])] = 0x12345678U;
    assert(msync(second_mapping, MYLITE_TEST_PAGE_SIZE * 2, MS_SYNC) == 0);
    assert(munmap(second_mapping, MYLITE_TEST_PAGE_SIZE * 2) == 0);

    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_wait_backend_wakes_across_processes(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "wait-backend.bin");
    int child_ready[2];
    int fd = open_file(shm_path);
    mylite_ownerless_wait_word *word;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    word = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    mylite_ownerless_wait_store(word, 0U);
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        mylite_ownerless_wait_word *child_word;
        int wait_result;

        close(child_ready[0]);
        child_fd = open_file(shm_path);
        child_word = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        signal_pipe(child_ready[1]);
        wait_result = mylite_ownerless_wait_for_change(child_word, 0U, MYLITE_TEST_WAIT_TIMEOUT_MS);
        assert(wait_result == MYLITE_OWNERLESS_WAIT_OK);
        assert(mylite_ownerless_wait_load(child_word) == 1U);
        assert(munmap(child_word, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    mylite_ownerless_wait_store(word, 1U);
    assert(mylite_ownerless_wait_wake(word) == MYLITE_OWNERLESS_WAIT_OK);
    wait_for_child(child);

    assert(munmap(word, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_wait_backend_times_out_without_change(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "wait-timeout.bin");
    int fd = open_file(shm_path);
    mylite_ownerless_wait_word *word;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    word = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    mylite_ownerless_wait_store(word, 7U);
    assert(mylite_ownerless_wait_for_change(word, 7U, 20U) == MYLITE_OWNERLESS_WAIT_TIMEOUT);
    mylite_ownerless_wait_store(word, 8U);
    assert(mylite_ownerless_wait_for_change(word, 7U, 0U) == MYLITE_OWNERLESS_WAIT_OK);

    assert(munmap(word, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_latch_records_owner_generation_and_wakes_waiter(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "ownerless-latch.bin");
    int child_ready[2];
    int fd = open_file(shm_path);
    mylite_ownerless_latch *latch;
    uint32_t state = 0U;
    uint32_t owner_id = 0U;
    uint32_t waiter_count = 0U;
    uint64_t owner_generation = 0U;
    uint64_t owner_death_count = 0U;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    latch = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    mylite_ownerless_latch_initialize(latch);
    assert(
        mylite_ownerless_latch_acquire(latch, 1U, 101U, NULL, NULL, 0U) == MYLITE_OWNERLESS_LATCH_OK
    );
    assert(
        mylite_ownerless_latch_snapshot(
            latch,
            &state,
            &owner_id,
            &owner_generation,
            &waiter_count,
            &owner_death_count
        ) == MYLITE_OWNERLESS_LATCH_OK
    );
    assert(state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED);
    assert(owner_id == 1U);
    assert(owner_generation == 101U);
    assert(waiter_count == 0U);
    assert(owner_death_count == 0U);
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        mylite_ownerless_latch *child_latch;

        close(child_ready[0]);
        child_fd = open_file(shm_path);
        child_latch = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        signal_pipe(child_ready[1]);
        assert(
            mylite_ownerless_latch_acquire(
                child_latch,
                2U,
                202U,
                NULL,
                NULL,
                MYLITE_TEST_WAIT_TIMEOUT_MS
            ) == MYLITE_OWNERLESS_LATCH_OK
        );
        assert(mylite_ownerless_latch_release(child_latch, 2U, 202U) == MYLITE_OWNERLESS_LATCH_OK);
        assert(munmap(child_latch, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    for (int attempt = 0; attempt < 1000; ++attempt) {
        assert(
            mylite_ownerless_latch_snapshot(
                latch,
                &state,
                &owner_id,
                &owner_generation,
                &waiter_count,
                &owner_death_count
            ) == MYLITE_OWNERLESS_LATCH_OK
        );
        if (waiter_count > 0U) {
            break;
        }
        sleep_milliseconds(1U);
    }
    assert(waiter_count > 0U);
    assert(mylite_ownerless_latch_release(latch, 1U, 101U) == MYLITE_OWNERLESS_LATCH_OK);
    wait_for_child(child);
    assert(
        mylite_ownerless_latch_snapshot(
            latch,
            &state,
            &owner_id,
            &owner_generation,
            &waiter_count,
            &owner_death_count
        ) == MYLITE_OWNERLESS_LATCH_OK
    );
    assert(state == MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED);
    assert(owner_id == 0U);
    assert(owner_generation == 0U);

    assert(munmap(latch, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_latch_reports_dead_owner_without_stealing(void) {
    mylite_ownerless_latch latch;
    uint32_t state = 0U;
    uint32_t owner_id = 0U;
    uint32_t waiter_count = 0U;
    uint64_t owner_generation = 0U;
    uint64_t owner_death_count = 0U;
    const uint32_t live_owner = 2U;

    mylite_ownerless_latch_initialize(&latch);
    assert(
        mylite_ownerless_latch_acquire(&latch, 1U, 101U, NULL, NULL, 0U) ==
        MYLITE_OWNERLESS_LATCH_OK
    );
    assert(
        mylite_ownerless_latch_acquire(
            &latch,
            2U,
            202U,
            latch_test_owner_is_alive,
            (void *)&live_owner,
            MYLITE_TEST_WAIT_TIMEOUT_MS
        ) == MYLITE_OWNERLESS_LATCH_OWNER_DEAD
    );
    assert(
        mylite_ownerless_latch_snapshot(
            &latch,
            &state,
            &owner_id,
            &owner_generation,
            &waiter_count,
            &owner_death_count
        ) == MYLITE_OWNERLESS_LATCH_OK
    );
    assert(state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED);
    assert(owner_id == 1U);
    assert(owner_generation == 101U);
    assert(owner_death_count == 1U);
    assert(mylite_ownerless_latch_release(&latch, 1U, 101U) == MYLITE_OWNERLESS_LATCH_OK);
}

static void test_platform_probe_records_required_primitives(void) {
    mylite_ownerless_probe_result probe = {0};

    assert(mylite_ownerless_probe_platform(&probe) == MYLITE_OWNERLESS_PROBE_OK);
    assert(probe.size == sizeof(probe));
    assert(probe.mmap_shared_visibility == 1U);
    assert(probe.byte_range_locks == 1U);
    assert(probe.lock_release_on_exit == 1U);
    assert(probe.grow_remap == 1U);
    assert(probe.wait_backend == 1U);
    assert(probe.required_primitives == 1U);
    assert(probe.platform_candidate == (probe.fast_wait_backend != 0U ? 1U : 0U));
}

static void test_directory_probe_records_required_primitives(void) {
    char *root = make_temp_root();
    mylite_ownerless_probe_result probe = {0};

    assert(mylite_ownerless_probe_directory(root, &probe) == MYLITE_OWNERLESS_PROBE_OK);
    assert(probe.size == sizeof(probe));
    assert(probe.mmap_shared_visibility == 1U);
    assert(probe.byte_range_locks == 1U);
    assert(probe.lock_release_on_exit == 1U);
    assert(probe.grow_remap == 1U);
    assert(probe.wait_backend == 1U);
    assert(probe.required_primitives == 1U);
    assert(probe.platform_candidate == (probe.fast_wait_backend != 0U ? 1U : 0U));

    remove_tree(root);
    free(root);
}

static void test_page_log_reads_latest_visible_page(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_v1[32];
    uint8_t page_v2[32];
    uint8_t page_stale_boundary[32];
    uint8_t other_page[32];
    uint8_t page_zero[32];
    uint8_t out_page[32];
    const char torn_tail = 'x';
    uint64_t first_offset = 0;
    uint64_t second_offset = 0;
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;
    uint64_t out_commit_lsn = 0;

    memset(page_v1, 0x11, sizeof(page_v1));
    memset(page_v2, 0x22, sizeof(page_v2));
    memset(page_stale_boundary, 0x66, sizeof(page_stale_boundary));
    memset(other_page, 0x33, sizeof(other_page));
    memset(page_zero, 0x44, sizeof(page_zero));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            0U,
            7U,
            90U,
            100U,
            page_v1,
            sizeof(page_v1),
            &first_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            0U,
            7U,
            190U,
            200U,
            page_v2,
            sizeof(page_v2),
            &second_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            0U,
            8U,
            195U,
            205U,
            other_page,
            sizeof(other_page),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            0U,
            7U,
            95U,
            240U,
            page_stale_boundary,
            sizeof(page_stale_boundary),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            0U,
            0U,
            300U,
            300U,
            page_zero,
            sizeof(page_zero),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(first_offset == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(second_offset > first_offset);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            0U,
            7U,
            150U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v1));
    assert(out_page_lsn == 90U);
    assert(out_commit_lsn == 100U);
    assert(memcmp(out_page, page_v1, sizeof(page_v1)) == 0);

    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            0U,
            7U,
            250U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_stale_boundary));
    assert(out_page_lsn == 95U);
    assert(out_commit_lsn == 240U);
    assert(memcmp(out_page, page_stale_boundary, sizeof(page_stale_boundary)) == 0);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            0U,
            7U,
            250U,
            out_page,
            1U,
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_FULL
    );
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            0U,
            9U,
            250U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    assert(lseek(fd, 0, SEEK_END) > 0);
    assert(write(fd, &torn_tail, sizeof(torn_tail)) == sizeof(torn_tail));
    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            0U,
            0U,
            500U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_zero));
    assert(out_page_lsn == 300U);
    assert(out_commit_lsn == 300U);
    assert(memcmp(out_page, page_zero, sizeof(page_zero)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_uses_payload_offset(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "offset-page-log.bin");
    int fd = open_file(log_path);
    const uint64_t log_offset = 128U;
    uint8_t page[16];
    uint8_t out_page[16];
    uint64_t record_offset = 0;
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;
    uint64_t out_commit_lsn = 0;

    memset(page, 0x66, sizeof(page));
    memset(out_page, 0, sizeof(out_page));
    truncate_file(fd, (off_t)log_offset);

    assert(mylite_ownerless_page_log_initialize_at(fd, log_offset) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append_at(
            fd,
            log_offset,
            0U,
            1U,
            10U,
            20U,
            page,
            sizeof(page),
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(record_offset == log_offset + MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            log_offset,
            record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page));
    assert(out_page_lsn == 10U);
    assert(out_commit_lsn == 20U);
    assert(memcmp(out_page, page, sizeof(page)) == 0);

    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest_at(
            fd,
            log_offset,
            0U,
            1U,
            20U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page));
    assert(out_page_lsn == 10U);
    assert(out_commit_lsn == 20U);
    assert(memcmp(out_page, page, sizeof(page)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_append_reports_write_volume(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "append-volume-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_v1[16];
    uint8_t page_v2[32];
    uint8_t page_v3[24];
    uint8_t out_page[24];
    uint64_t first_record_offset = 0;
    uint64_t second_record_offset = 0;
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;
    uint64_t out_commit_lsn = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    mylite_ownerless_page_log_append_session session = {0};

    memset(page_v1, 0x51, sizeof(page_v1));
    memset(page_v2, 0x52, sizeof(page_v2));
    memset(page_v3, 0x53, sizeof(page_v3));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            5U,
            1U,
            100U,
            100U,
            page_v1,
            sizeof(page_v1),
            &first_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_session_begin_initialized_at(fd, 0U, &session) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_session_append(
            fd,
            &session,
            5U,
            2U,
            120U,
            120U,
            page_v2,
            sizeof(page_v2),
            &second_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_append_session_end(fd, &session);
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(
        second_record_offset ==
        first_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE + sizeof(page_v1)
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DIRECT_APPEND_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SESSION_BEGIN_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SESSION_APPEND_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SESSION_END_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_BYTES] == sizeof(page_v1) + sizeof(page_v2));
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_RECORD_HEADER_BYTES] ==
        2U * MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FULL_RECORDS] == 2U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_FULL_PAYLOAD_BYTES] == sizeof(page_v1) + sizeof(page_v2)
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_TRAILING_ZERO_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_OTHER_RECORDS] == 2U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_OTHER_PAYLOAD_BYTES] == sizeof(page_v1) + sizeof(page_v2)
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    assert(
        mylite_ownerless_page_log_append(fd, 5U, 3U, 140U, 140U, page_v3, sizeof(page_v3), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);
    for (size_t i = 0; i < PAGE_LOG_APPEND_PERF_STAT_COUNT; ++i) {
        assert(stats[i] == 0U);
    }
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            5U,
            3U,
            140U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v3));
    assert(out_page_lsn == 140U);
    assert(out_commit_lsn == 140U);
    assert(memcmp(out_page, page_v3, sizeof(page_v3)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_append_uses_precomputed_checksum(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "append-precomputed-checksum-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t record_offset = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;
    uint64_t out_commit_lsn = 0;
    uint64_t page_checksum = 0;

    fill_innodb_test_page(page, 17U, 4U, 180U, 0x5CU);
    page_checksum = mylite_ownerless_page_log_checksum_page(page, sizeof(page));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append_initialized_at_with_checksum(
            fd,
            0U,
            17U,
            4U,
            180U,
            180U,
            page,
            sizeof(page),
            page_checksum,
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(record_offset == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_PRECOMPUTED_CHECKSUM_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CHECKSUM_NS] == 0U);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            17U,
            4U,
            180U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page));
    assert(out_page_lsn == 180U);
    assert(out_commit_lsn == 180U);
    assert(memcmp(out_page, page, sizeof(page)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_append_detail_stats_are_opt_in(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "append-detail-stats-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};

    fill_innodb_test_page(page, 15U, 1U, 100U, 0x3AU);
    store_test_be16(page, MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET, MYLITE_TEST_INNODB_PAGE_TYPE_INDEX);

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    mylite_ownerless_page_log_set_append_detail_perf_stats_enabled(0);
    assert(
        mylite_ownerless_page_log_append(fd, 15U, 1U, 100U, 100U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_BYTES] > 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_PAYLOAD_BYTES] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_OTHER_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_PAGE_TYPE_STATS_NS] == 0U);

    mylite_ownerless_page_log_set_append_detail_perf_stats_enabled(1);
    store_test_be64(page, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 120U);
    assert(
        mylite_ownerless_page_log_append(fd, 15U, 1U, 120U, 120U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_PAYLOAD_BYTES] > 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_OTHER_RECORDS] == 0U);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_encodes_sparse_zero_payloads(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "trimmed-payload-page-log.bin");
    int fd = open_file(log_path);
    uint8_t full_page[128];
    uint8_t sparse_page[128];
    uint8_t trailing_page[128];
    uint8_t zero_page[128];
    uint8_t out_page[128];
    uint64_t first_record_offset = 0;
    uint64_t sparse_record_offset = 0;
    uint64_t trailing_record_offset = 0;
    uint64_t zero_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    mylite_ownerless_page_log_append_session session = {0};
    struct stat log_stat = {0};
    const size_t sparse_payload_size = 14U;
    const size_t compact_sparse_metadata_size = 10U;
    const size_t compact_sparse_data_size = 4U;
    const size_t trailing_payload_size = 16U;

    memset(full_page, 0x44, sizeof(full_page));
    memset(sparse_page, 0, sizeof(sparse_page));
    memset(trailing_page, 0, sizeof(trailing_page));
    memset(zero_page, 0, sizeof(zero_page));
    sparse_page[0] = 0x31;
    sparse_page[7] = 0x32;
    sparse_page[32] = 0x33;
    sparse_page[sizeof(sparse_page) - 1U] = 0x34;
    memset(trailing_page, 0x55, trailing_payload_size);
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            9U,
            1U,
            80U,
            80U,
            full_page,
            sizeof(full_page),
            &first_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_session_begin_initialized_at(fd, 0U, &session) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_session_append(
            fd,
            &session,
            9U,
            1U,
            120U,
            120U,
            sparse_page,
            sizeof(sparse_page),
            &sparse_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_session_append(
            fd,
            &session,
            9U,
            1U,
            140U,
            140U,
            trailing_page,
            sizeof(trailing_page),
            &trailing_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_session_append(
            fd,
            &session,
            9U,
            1U,
            160U,
            160U,
            zero_page,
            sizeof(zero_page),
            &zero_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_append_session_end(fd, &session);
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(first_record_offset == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(
        sparse_record_offset ==
        first_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE + sizeof(full_page)
    );
    assert(
        zero_record_offset == trailing_record_offset +
                                  MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE +
                                  trailing_payload_size
    );
    assert(
        trailing_record_offset ==
        sparse_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE + sparse_payload_size
    );
    assert(fstat(fd, &log_stat) == 0);
    assert(
        log_stat.st_size ==
        (off_t)(zero_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE)
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 4U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_BYTES] ==
        sizeof(full_page) + sparse_payload_size + trailing_payload_size
    );
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_RECORD_HEADER_BYTES] ==
        4U * MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FULL_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FULL_PAYLOAD_BYTES] == sizeof(full_page));
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_PAYLOAD_BYTES] == sparse_payload_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_ZERO_RECORDS] == 1U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES] == sparse_payload_size
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_VARINT_COMPACT_SPARSE_ZERO_RECORDS] == 1U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_VARINT_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES] ==
        sparse_payload_size
    );
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_METADATA_BYTES] ==
        compact_sparse_metadata_size
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_DATA_BYTES] == compact_sparse_data_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_TRAILING_ZERO_RECORDS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_TRAILING_ZERO_PAYLOAD_BYTES] == trailing_payload_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_RECORDS] == 3U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_PAYLOAD_BYTES] ==
        sparse_payload_size + trailing_payload_size
    );
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_COMPACT_SPARSE_METADATA_BYTES] ==
        compact_sparse_metadata_size
    );
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_COMPACT_SPARSE_DATA_BYTES] ==
        compact_sparse_data_size
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_OTHER_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_OTHER_PAYLOAD_BYTES] == sizeof(full_page));
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_OTHER_COMPACT_SPARSE_METADATA_BYTES] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_OTHER_COMPACT_SPARSE_DATA_BYTES] == 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            9U,
            1U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(sparse_page));
    assert(page_lsn == 120U);
    assert(commit_lsn == 120U);
    assert(memcmp(out_page, sparse_page, sizeof(sparse_page)) == 0);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            trailing_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(trailing_page));
    assert(page_lsn == 140U);
    assert(commit_lsn == 140U);
    assert(memcmp(out_page, trailing_page, sizeof(trailing_page)) == 0);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            zero_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(zero_page));
    assert(page_lsn == 160U);
    assert(commit_lsn == 160U);
    assert(memcmp(out_page, zero_page, sizeof(zero_page)) == 0);

    assert(
        mylite_ownerless_page_log_checkpoint(fd, 140U, NULL, NULL) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(fstat(fd, &log_stat) == 0);
    assert(
        log_stat.st_size == (off_t)(MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE +
                                    MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE)
    );
    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            9U,
            1U,
            160U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(zero_page));
    assert(page_lsn == 160U);
    assert(commit_lsn == 160U);
    assert(memcmp(out_page, zero_page, sizeof(zero_page)) == 0);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            9U,
            1U,
            140U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_encodes_fill_sparse_zero_payloads(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "fill-sparse-payload-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page[256];
    uint8_t out_page[256];
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    page_log_checkpoint_index_context checkpoint_context = {0};
    struct stat log_stat = {0};
    const size_t fill_sparse_payload_size = 24U;
    const size_t fill_sparse_metadata_size = 18U;
    const size_t fill_sparse_raw_data_size = 5U;
    const size_t fill_sparse_fill_size = 1U;

    memset(page, 0, sizeof(page));
    memset(out_page, 0xEE, sizeof(out_page));
    store_test_be16(page, MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET, MYLITE_TEST_INNODB_PAGE_TYPE_SYS);
    page[32] = 0x11U;
    page[33] = 0x22U;
    page[63] = 0x44U;
    memset(page + 64U, 0xFF, 128U);
    page[220] = 0x33U;

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            12U,
            1U,
            200U,
            200U,
            page,
            sizeof(page),
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(record_offset == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(fstat(fd, &log_stat) == 0);
    assert(
        log_stat.st_size == (off_t)(record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE +
                                    fill_sparse_payload_size)
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_BYTES] == fill_sparse_payload_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_PAYLOAD_BYTES] == fill_sparse_payload_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_ZERO_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_VARINT_COMPACT_SPARSE_ZERO_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_RECORDS] == 1U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_PAYLOAD_BYTES] == fill_sparse_payload_size
    );
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_METADATA_BYTES] == fill_sparse_metadata_size
    );
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_RAW_DATA_BYTES] == fill_sparse_raw_data_size
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_FILL_BYTES] == fill_sparse_fill_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SYS_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SYS_PAYLOAD_BYTES] == fill_sparse_payload_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SYS_COMPACT_SPARSE_METADATA_BYTES] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SYS_COMPACT_SPARSE_DATA_BYTES] == 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            12U,
            1U,
            200U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page));
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);
    assert(memcmp(out_page, page, sizeof(page)) == 0);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page));
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);
    assert(memcmp(out_page, page, sizeof(page)) == 0);

    assert(
        mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
            fd,
            0U,
            200U,
            150U,
            capture_page_log_record_for_checkpoint_index,
            prepare_page_log_checkpoint,
            NULL,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.prepare_count == 1U);
    assert(checkpoint_context.retained.count == 0U);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_streams_sparse_checksum_validation(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *log_path = path_join(root, "stream-sparse-checksum-page-log.bin");
    int fd = open_file(log_path);
    uint8_t *index = calloc(1U, index_size);
    page_log_replay_context context = {.page_index = index, .page_index_size = index_size};
    uint8_t page[256];
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint64_t append_stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    uint64_t scan_stats[PAGE_LOG_SCAN_PERF_STAT_COUNT] = {0};

    assert(index != NULL);
    memset(page, 0, sizeof(page));
    store_test_be16(page, MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET, MYLITE_TEST_INNODB_PAGE_TYPE_SYS);
    page[32] = 0x11U;
    page[33] = 0x22U;
    page[63] = 0x44U;
    memset(page + 64U, 0xFF, 128U);
    page[220] = 0x33U;

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            12U,
            1U,
            200U,
            200U,
            page,
            sizeof(page),
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(append_stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);
    assert(append_stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_RECORDS] == 1U);

    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    mylite_ownerless_page_log_reset_scan_perf_stats();
    mylite_ownerless_page_log_set_scan_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_replay_at(fd, 0U, replay_page_log_record_into_index, &context) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_scan_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_scan_perf_stats(scan_stats, PAGE_LOG_SCAN_PERF_STAT_COUNT);
    assert(scan_stats[PAGE_LOG_SCAN_PERF_STAT_STREAM_CHECKSUM_RECORDS] == 1U);
    assert(scan_stats[PAGE_LOG_SCAN_PERF_STAT_STREAM_CHECKSUM_BYTES] == sizeof(page));
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            1U,
            10U,
            12U,
            1U,
            200U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);

    assert(close(fd) == 0);
    free(index);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_rejects_corrupt_sparse_checksum_records(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *interior_log_path = path_join(root, "corrupt-sparse-interior-page-log.bin");
    char *tail_log_path = path_join(root, "corrupt-sparse-tail-page-log.bin");
    int fd = open_file(interior_log_path);
    uint8_t *index = calloc(1U, index_size);
    page_log_replay_context context = {.page_index = index, .page_index_size = index_size};
    uint8_t page_v1[256];
    uint8_t page_v2[256];
    uint8_t out_page[256];
    uint64_t first_offset = 0;
    uint64_t second_offset = 0;
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    int checkpointed = -1;
    const uint8_t corrupt_byte = 0xCCU;

    assert(index != NULL);
    memset(page_v1, 0, sizeof(page_v1));
    store_test_be16(page_v1, MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET, MYLITE_TEST_INNODB_PAGE_TYPE_SYS);
    page_v1[32] = 0x11U;
    page_v1[33] = 0x22U;
    page_v1[63] = 0x44U;
    memset(page_v1 + 64U, 0xFF, 128U);
    page_v1[220] = 0x33U;
    memcpy(page_v2, page_v1, sizeof(page_v2));
    page_v2[32] = 0x55U;
    page_v2[220] = 0x66U;
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            12U,
            1U,
            200U,
            200U,
            page_v1,
            sizeof(page_v1),
            &first_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            12U,
            1U,
            220U,
            220U,
            page_v2,
            sizeof(page_v2),
            &second_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(second_offset > first_offset);
    assert(
        pwrite(
            fd,
            &corrupt_byte,
            sizeof(corrupt_byte),
            (off_t)(first_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE + 1U)
        ) == sizeof(corrupt_byte)
    );
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            12U,
            1U,
            220U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_log_replay_at(fd, 0U, replay_page_log_record_into_index, &context) ==
        MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(
        mylite_ownerless_page_log_checkpoint_if_safe(fd, 220U, &checkpointed) ==
        MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(checkpointed == 0);
    assert(close(fd) == 0);

    memset(index, 0, index_size);
    memset(out_page, 0, sizeof(out_page));
    checkpointed = -1;
    fd = open_file(tail_log_path);
    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            12U,
            1U,
            200U,
            200U,
            page_v1,
            sizeof(page_v1),
            &first_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            12U,
            1U,
            220U,
            220U,
            page_v2,
            sizeof(page_v2),
            &second_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        pwrite(
            fd,
            &corrupt_byte,
            sizeof(corrupt_byte),
            (off_t)(second_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE + 1U)
        ) == sizeof(corrupt_byte)
    );
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            12U,
            1U,
            220U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v1));
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);
    assert(memcmp(out_page, page_v1, sizeof(page_v1)) == 0);
    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_log_replay_at(fd, 0U, replay_page_log_record_into_index, &context) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            1U,
            10U,
            12U,
            1U,
            220U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == first_offset);
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);
    assert(
        mylite_ownerless_page_log_checkpoint_if_safe(fd, 220U, &checkpointed) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpointed == 1);

    assert(close(fd) == 0);
    free(index);
    free(tail_log_path);
    free(interior_log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_encodes_index_fill_sparse_zero_payloads(void) {
    char *root = make_temp_root();
    char *missing_boundary_log_path = path_join(root, "index-fill-sparse-missing-boundary.bin");
    char *checkpoint_log_path = path_join(root, "index-fill-sparse-checkpoint.bin");
    int fd = open_file(missing_boundary_log_path);
    uint8_t page_boundary[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_after[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    page_log_checkpoint_index_context checkpoint_context = {0};

    fill_innodb_test_page(page_after, 51U, 9U, 200U, 0x11U);
    store_test_be16(
        page_after,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    memset(page_after + 256U, 0xA5, 512U);
    memset(page_after + 2048U, 0x5A, 1024U);
    page_after[4000] = 0x42U;
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            51U,
            9U,
            200U,
            200U,
            page_after,
            sizeof(page_after),
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SYS_RECORDS] == 0U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_PAYLOAD_BYTES] ==
        stats[PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_BYTES]
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_PAYLOAD_BYTES] < sizeof(page_after));
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_COMPACT_SPARSE_METADATA_BYTES] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_COMPACT_SPARSE_DATA_BYTES] == 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            51U,
            9U,
            200U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_after));
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);
    assert(memcmp(out_page, page_after, sizeof(page_after)) == 0);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_after));
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);
    assert(memcmp(out_page, page_after, sizeof(page_after)) == 0);

    assert(
        mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
            fd,
            0U,
            200U,
            150U,
            capture_page_log_record_for_checkpoint_index,
            prepare_page_log_checkpoint,
            NULL,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_BUSY
    );
    assert(checkpoint_context.prepare_count == 0U);
    assert(checkpoint_context.retained.count == 0U);
    assert(close(fd) == 0);

    fd = open_file(checkpoint_log_path);
    memset(&checkpoint_context, 0, sizeof(checkpoint_context));
    fill_innodb_test_page(page_boundary, 51U, 9U, 150U, 0x22U);
    store_test_be16(
        page_boundary,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    memset(page_boundary + 256U, 0xC3, 512U);
    memset(page_boundary + 2048U, 0x3C, 1024U);
    page_boundary[4000] = 0x24U;

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            51U,
            9U,
            150U,
            150U,
            page_boundary,
            sizeof(page_boundary),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            51U,
            9U,
            200U,
            200U,
            page_after,
            sizeof(page_after),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_RECORDS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 2U);

    assert(
        mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
            fd,
            0U,
            200U,
            150U,
            capture_page_log_record_for_checkpoint_index,
            prepare_page_log_checkpoint,
            NULL,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.prepare_count == 1U);
    assert(checkpoint_context.retained.count == 2U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            51U,
            9U,
            150U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_boundary));
    assert(page_lsn == 150U);
    assert(commit_lsn == 150U);
    assert(memcmp(out_page, page_boundary, sizeof(page_boundary)) == 0);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            51U,
            9U,
            200U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_after));
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);
    assert(memcmp(out_page, page_after, sizeof(page_after)) == 0);

    assert(close(fd) == 0);
    free(checkpoint_log_path);
    free(missing_boundary_log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_attributes_index_page_identity_deltas(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-page-delta-attribution.bin");
    int fd = open_file(log_path);
    uint8_t page_v1[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_v2[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_v3[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_small[256];
    uint8_t sys_page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint32_t out_page_size = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};

    fill_innodb_test_page(page_v1, 61U, 11U, 300U, 0x10U);
    store_test_be16(
        page_v1,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    memcpy(page_v2, page_v1, sizeof(page_v2));
    page_v2[26] ^= 0x11U;
    page_v2[64] = 0x22U;
    page_v2[128] = 0x33U;
    memcpy(page_v3, page_v2, sizeof(page_v3));
    page_v3[27] ^= 0x44U;
    page_v3[65] = 0x23U;

    memset(page_small, 0, sizeof(page_small));
    store_test_be32(page_small, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 11U);
    store_test_be64(page_small, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 330U);
    store_test_be16(
        page_small,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_small, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 61U);
    page_small[128] = 0x70U;

    fill_innodb_test_page(sys_page, 61U, 12U, 340U, 0x55U);
    store_test_be16(
        sys_page,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_SYS
    );
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            61U,
            11U,
            300U,
            300U,
            page_v1,
            sizeof(page_v1),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            61U,
            11U,
            310U,
            310U,
            page_v2,
            sizeof(page_v2),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            61U,
            11U,
            320U,
            320U,
            page_v3,
            sizeof(page_v3),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            61U,
            11U,
            330U,
            330U,
            page_small,
            sizeof(page_small),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            61U,
            12U,
            340U,
            340U,
            sys_page,
            sizeof(sys_page),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 5U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 4U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SYS_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_UNIQUE] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_DUPLICATE] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_SIZE_MISMATCH] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_TABLE_OVERFLOW] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_CHANGED_BYTES] == 5U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_FIL_HEADER_CHANGED_BYTES] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_BODY_CHANGED_BYTES] == 3U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_FIL_HEADER_CHANGED_BYTES] +
            stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_BODY_CHANGED_BYTES] ==
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_IDENTITY_CHANGED_BYTES]
    );
    assert(MYLITE_TEST_INNODB_FIL_HEADER_SIZE == 38U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            61U,
            11U,
            330U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_small));
    assert(page_lsn == 330U);
    assert(commit_lsn == 330U);
    assert(memcmp(out_page, page_small, sizeof(page_small)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_encodes_index_delta_payloads(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-delta-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_after_checkpoint[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_other_identity[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t base_record_offset = 0;
    uint64_t early_delta_record_offset = 0;
    uint64_t delta_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    mylite_ownerless_page_log_append_session session = {0};
    page_log_checkpoint_index_context checkpoint_context = {0};
    uint32_t early_delta_flags = 0;
    uint32_t delta_flags = 0;
    uint32_t retained_flags = 0;

    memset(page_base, 0x5C, sizeof(page_base));
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 17U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 500U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 70U);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 510U);
    page_delta[128] ^= 0x21U;
    page_delta[1024] ^= 0x42U;

    memcpy(page_after_checkpoint, page_delta, sizeof(page_after_checkpoint));
    store_test_be64(page_after_checkpoint, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 520U);
    page_after_checkpoint[256] ^= 0x18U;

    memcpy(page_other_identity, page_after_checkpoint, sizeof(page_other_identity));
    store_test_be32(page_other_identity, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 18U);
    store_test_be64(page_other_identity, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 530U);
    page_other_identity[512] ^= 0x33U;
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            70U,
            17U,
            500U,
            500U,
            page_base,
            sizeof(page_base),
            &base_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            70U,
            17U,
            501U,
            501U,
            page_delta,
            sizeof(page_delta),
            &early_delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(base_record_offset == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(early_delta_record_offset > base_record_offset);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 1U);
    early_delta_flags = read_page_log_record_flags(fd, early_delta_record_offset);
    assert((early_delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U);

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append_session_begin_initialized_at(fd, 0U, &session) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_session_append(
            fd,
            &session,
            70U,
            17U,
            510U,
            510U,
            page_delta,
            sizeof(page_delta),
            &delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_append_session_end(fd, &session);
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(delta_record_offset > base_record_offset);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES] > 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES] < sizeof(page_delta));
    delta_flags = read_page_log_record_flags(fd, delta_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U);

    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            delta_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 510U);
    assert(commit_lsn == 510U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            70U,
            17U,
            510U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 510U);
    assert(commit_lsn == 510U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            505U,
            capture_page_log_record_for_checkpoint_index,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.retained.count == 1U);
    assert(checkpoint_context.retained.records[0].commit_lsn == 510U);
    retained_flags =
        read_page_log_record_flags(fd, checkpoint_context.retained.records[0].record_offset);
    assert((retained_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            checkpoint_context.retained.records[0].record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 510U);
    assert(commit_lsn == 510U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            70U,
            17U,
            500U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            70U,
            17U,
            520U,
            520U,
            page_after_checkpoint,
            sizeof(page_after_checkpoint),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            70U,
            18U,
            530U,
            530U,
            page_other_identity,
            sizeof(page_other_identity),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 0U);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            70U,
            17U,
            520U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_after_checkpoint));
    assert(page_lsn == 520U);
    assert(commit_lsn == 520U);
    assert(memcmp(out_page, page_after_checkpoint, sizeof(page_after_checkpoint)) == 0);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            70U,
            18U,
            530U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_other_identity));
    assert(page_lsn == 530U);
    assert(commit_lsn == 530U);
    assert(memcmp(out_page, page_other_identity, sizeof(page_other_identity)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_encodes_undo_delta_payloads(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "undo-delta-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_after_checkpoint[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t delta_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    page_log_checkpoint_index_context checkpoint_context = {0};
    uint32_t delta_flags = 0;
    uint32_t retained_flags = 0;

    memset(page_base, 0x47, sizeof(page_base));
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 31U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 700U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_UNDO_LOG
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 1U);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 710U);
    page_delta[96] ^= 0x14U;
    page_delta[2048] ^= 0x2BU;

    memcpy(page_after_checkpoint, page_delta, sizeof(page_after_checkpoint));
    store_test_be64(page_after_checkpoint, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 720U);
    page_after_checkpoint[512] ^= 0x5DU;
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            1U,
            31U,
            700U,
            700U,
            page_base,
            sizeof(page_base),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            1U,
            31U,
            710U,
            710U,
            page_delta,
            sizeof(page_delta),
            &delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_UNDO_LOG_RECORDS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_FAST_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_PAYLOAD_BYTES] > 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_PAYLOAD_BYTES] < sizeof(page_delta));
    delta_flags = read_page_log_record_flags(fd, delta_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_UNDO_DELTA) != 0U);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U);

    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            delta_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 710U);
    assert(commit_lsn == 710U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            1U,
            31U,
            710U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 710U);
    assert(commit_lsn == 710U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            705U,
            capture_page_log_record_for_checkpoint_index,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.retained.count == 1U);
    assert(checkpoint_context.retained.records[0].commit_lsn == 710U);
    retained_flags =
        read_page_log_record_flags(fd, checkpoint_context.retained.records[0].record_offset);
    assert((retained_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_UNDO_DELTA) == 0U);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            checkpoint_context.retained.records[0].record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 710U);
    assert(commit_lsn == 710U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            1U,
            31U,
            700U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            1U,
            31U,
            720U,
            720U,
            page_after_checkpoint,
            sizeof(page_after_checkpoint),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_UNDO_LOG_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_UNDO_DELTA_FAST_RECORDS] == 0U);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            1U,
            31U,
            720U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_after_checkpoint));
    assert(page_lsn == 720U);
    assert(commit_lsn == 720U);
    assert(memcmp(out_page, page_after_checkpoint, sizeof(page_after_checkpoint)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_keeps_sys_pages_standalone(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "sys-standalone-page-log.bin");
    int fd = open_file(log_path);

    const struct {
        uint32_t space_id;
        uint16_t page_type;
        enum page_log_append_perf_stat_index page_type_stat;
    } cases[] = {
        {1U, MYLITE_TEST_INNODB_PAGE_TYPE_SYS, PAGE_LOG_APPEND_PERF_STAT_SYS_RECORDS},
        {80U, MYLITE_TEST_INNODB_PAGE_TYPE_SYS, PAGE_LOG_APPEND_PERF_STAT_SYS_RECORDS},
        {0U, MYLITE_TEST_INNODB_PAGE_TYPE_TRX_SYS, PAGE_LOG_APPEND_PERF_STAT_TRX_SYS_RECORDS},
        {1U, MYLITE_TEST_INNODB_PAGE_TYPE_TRX_SYS, PAGE_LOG_APPEND_PERF_STAT_TRX_SYS_RECORDS},
    };

    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t second_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    uint32_t second_flags = 0;

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    for (size_t case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        const uint32_t space_id = cases[case_index].space_id;
        const uint32_t page_no = 3U + (uint32_t)case_index;
        const uint64_t base_lsn = 930U + (uint64_t)(case_index * 20U);
        const uint64_t delta_lsn = base_lsn + 10U;
        const int fill_byte = 0x3C + (int)case_index;

        memset(page_base, fill_byte, sizeof(page_base));
        store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, page_no);
        store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, base_lsn);
        store_test_be16(
            page_base,
            MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
            cases[case_index].page_type
        );
        store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, space_id);

        memcpy(page_delta, page_base, sizeof(page_delta));
        store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, delta_lsn);
        page_delta[128] ^= 0x45U;
        page_delta[2048] ^= 0x23U;

        mylite_ownerless_page_log_reset_append_perf_stats();
        mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
        assert(
            mylite_ownerless_page_log_append(
                fd,
                space_id,
                page_no,
                base_lsn,
                base_lsn,
                page_base,
                sizeof(page_base),
                NULL
            ) == MYLITE_OWNERLESS_PAGE_LOG_OK
        );
        assert(
            mylite_ownerless_page_log_append(
                fd,
                space_id,
                page_no,
                delta_lsn,
                delta_lsn,
                page_delta,
                sizeof(page_delta),
                &second_record_offset
            ) == MYLITE_OWNERLESS_PAGE_LOG_OK
        );
        mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
        mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

        assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
        assert(stats[cases[case_index].page_type_stat] == 2U);
        assert(stats[PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_RECORDS] == 0U);
        second_flags = read_page_log_record_flags(fd, second_record_offset);
        assert((second_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U);
        assert((second_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_UNDO_DELTA) == 0U);
        assert((second_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_HISTORY_RSEG_DELTA) == 0U);

        memset(out_page, 0xEE, sizeof(out_page));
        assert(
            mylite_ownerless_page_log_read_record_at(
                fd,
                0U,
                second_record_offset,
                out_page,
                sizeof(out_page),
                &out_page_size,
                &page_lsn,
                &commit_lsn
            ) == MYLITE_OWNERLESS_PAGE_LOG_OK
        );
        assert(out_page_size == sizeof(page_delta));
        assert(page_lsn == delta_lsn);
        assert(commit_lsn == delta_lsn);
        assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);
    }

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void assert_page_log_encodes_history_rseg_delta_payload(
    uint16_t page_type,
    uint32_t page_no,
    uint64_t base_lsn,
    const char *log_name,
    enum page_log_append_perf_stat_index page_type_stat
) {
    char *root = make_temp_root();
    char *log_path = path_join(root, log_name);
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t delta_record_offset = 0;
    const uint64_t delta_lsn = base_lsn + 10U;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    page_log_checkpoint_index_context checkpoint_context = {0};
    uint32_t delta_flags = 0;
    uint32_t retained_flags = 0;

    memset(page_base, 0x5A, sizeof(page_base));
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, page_no);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, base_lsn);
    store_test_be16(page_base, MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET, page_type);
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 1U);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, delta_lsn);
    page_delta[128] ^= 0x11U;
    page_delta[3072] ^= 0x27U;
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append_initialized_at_with_checksum_and_options(
            fd,
            0U,
            1U,
            page_no,
            base_lsn,
            base_lsn,
            page_base,
            sizeof(page_base),
            mylite_ownerless_page_log_checksum_page(page_base, sizeof(page_base)),
            MYLITE_OWNERLESS_PAGE_LOG_APPEND_HISTORY_RSEG_DELTA,
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_initialized_at_with_checksum_and_options(
            fd,
            0U,
            1U,
            page_no,
            delta_lsn,
            delta_lsn,
            page_delta,
            sizeof(page_delta),
            mylite_ownerless_page_log_checksum_page(page_delta, sizeof(page_delta)),
            MYLITE_OWNERLESS_PAGE_LOG_APPEND_HISTORY_RSEG_DELTA,
            &delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
    assert(stats[page_type_stat] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_FAST_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_PAYLOAD_BYTES] > 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_HISTORY_RSEG_DELTA_PAYLOAD_BYTES] < sizeof(page_delta));
    delta_flags = read_page_log_record_flags(fd, delta_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_HISTORY_RSEG_DELTA) != 0U);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_UNDO_DELTA) == 0U);

    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            delta_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == delta_lsn);
    assert(commit_lsn == delta_lsn);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            1U,
            page_no,
            delta_lsn,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == delta_lsn);
    assert(commit_lsn == delta_lsn);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            base_lsn + 5U,
            capture_page_log_record_for_checkpoint_index,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.retained.count == 1U);
    assert(checkpoint_context.retained.records[0].commit_lsn == delta_lsn);
    retained_flags =
        read_page_log_record_flags(fd, checkpoint_context.retained.records[0].record_offset);
    assert((retained_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_HISTORY_RSEG_DELTA) == 0U);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            checkpoint_context.retained.records[0].record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == delta_lsn);
    assert(commit_lsn == delta_lsn);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_encodes_history_rseg_delta_payloads(void) {
    assert_page_log_encodes_history_rseg_delta_payload(
        MYLITE_TEST_INNODB_PAGE_TYPE_SYS,
        3U,
        1000U,
        "history-rseg-delta-sys-page-log.bin",
        PAGE_LOG_APPEND_PERF_STAT_SYS_RECORDS
    );
    assert_page_log_encodes_history_rseg_delta_payload(
        MYLITE_TEST_INNODB_PAGE_TYPE_TRX_SYS,
        5U,
        1100U,
        "history-rseg-delta-trx-sys-page-log.bin",
        PAGE_LOG_APPEND_PERF_STAT_TRX_SYS_RECORDS
    );
}

static void test_page_log_fast_encodes_small_index_delta_payloads(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-delta-fast-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t delta_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    uint32_t delta_flags = 0;

    memset(page_base, 0, sizeof(page_base));
    for (uint32_t offset = 96U; offset + 1U < MYLITE_TEST_PAGE_SIZE - 96U; offset += 5U) {
        page_base[offset] = (uint8_t)(0x31U + (offset & 0x3FU));
        page_base[offset + 1U] = (uint8_t)(0x87U ^ (offset & 0x7FU));
    }
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 44U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 900U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 72U);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 920U);
    page_delta[2048] ^= 0x5AU;
    page_delta[2051] ^= 0x19U;
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            72U,
            44U,
            900U,
            900U,
            page_base,
            sizeof(page_base),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            72U,
            44U,
            920U,
            920U,
            page_delta,
            sizeof(page_delta),
            &delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES] > 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES] <= 2048U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_PAYLOAD_BYTES] ==
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES]
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_EXACT_RECORDS] == 0U);
    delta_flags = read_page_log_record_flags(fd, delta_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U);

    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            delta_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 920U);
    assert(commit_lsn == 920U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            72U,
            44U,
            920U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 920U);
    assert(commit_lsn == 920U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_fast_encodes_medium_index_delta_payloads(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-delta-medium-fast-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE * 2U];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE * 2U];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE * 2U];
    uint64_t delta_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    uint32_t delta_flags = 0;

    memset(page_base, 0x6DU, sizeof(page_base));
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 45U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 930U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 73U);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 940U);
    for (uint32_t offset = 2048U; offset < 4608U; ++offset) {
        page_delta[offset] ^= (uint8_t)(0x11U + (offset & 0x3FU));
    }
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            73U,
            45U,
            930U,
            930U,
            page_base,
            sizeof(page_base),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            73U,
            45U,
            940U,
            940U,
            page_delta,
            sizeof(page_delta),
            &delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_EXACT_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES] > 2048U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES] <= 4096U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_PAYLOAD_BYTES] ==
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES]
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_LIMIT_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS] == 0U);
    delta_flags = read_page_log_record_flags(fd, delta_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            73U,
            45U,
            940U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 940U);
    assert(commit_lsn == 940U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_reuses_fast_miss_delta_payload_for_exact_fallback(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-delta-fast-miss-reuse-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE * 4U];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE * 4U];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE * 4U];
    uint64_t delta_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    uint32_t delta_flags = 0;

    memset(page_base, 0x6DU, sizeof(page_base));
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 45U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 930U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 73U);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 940U);
    for (uint32_t offset = 4096U; offset < 9728U; ++offset) {
        page_delta[offset] ^= (uint8_t)(0x11U + (offset & 0x3FU));
    }
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            73U,
            45U,
            930U,
            930U,
            page_base,
            sizeof(page_base),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            73U,
            45U,
            940U,
            940U,
            page_delta,
            sizeof(page_delta),
            &delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_EXACT_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES] > 4096U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_EXACT_PAYLOAD_BYTES] ==
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES]
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_LIMIT_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_STANDALONE_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REUSED_FAST_PAYLOAD_RECORDS] == 1U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REUSED_FAST_PAYLOAD_BYTES] ==
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_PAYLOAD_BYTES]
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_MATERIALIZE_SKIPPED_RECORDS] == 1U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_MATERIALIZE_SKIPPED_BYTES] == sizeof(page_delta)
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_RECORDS] == 0U);
    delta_flags = read_page_log_record_flags(fd, delta_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            73U,
            45U,
            940U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 940U);
    assert(commit_lsn == 940U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 950U);
    for (uint32_t offset = 4096U; offset < 9728U; ++offset) {
        page_delta[offset] ^= (uint8_t)(0x21U + (offset & 0x3FU));
    }

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            73U,
            45U,
            950U,
            950U,
            page_delta,
            sizeof(page_delta),
            &delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    memset(stats, 0, sizeof(stats));
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_EXACT_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_LIMIT_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REUSED_FAST_PAYLOAD_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_MATERIALIZE_SKIPPED_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_RECORDS] == 0U);
    delta_flags = read_page_log_record_flags(fd, delta_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            73U,
            45U,
            950U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 950U);
    assert(commit_lsn == 950U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 960U);
    for (uint32_t offset = 4096U; offset < 9728U; ++offset) {
        page_delta[offset] ^= (uint8_t)(0x31U + (offset & 0x3FU));
    }

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            73U,
            45U,
            960U,
            960U,
            page_delta,
            sizeof(page_delta),
            &delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    memset(stats, 0, sizeof(stats));
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_EXACT_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_LIMIT_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REUSED_FAST_PAYLOAD_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_MATERIALIZE_SKIPPED_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_RECORDS] == 0U);
    delta_flags = read_page_log_record_flags(fd, delta_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            73U,
            45U,
            960U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 960U);
    assert(commit_lsn == 960U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_exact_probe_preserves_fill_sparse_rejection(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-delta-fill-sparse-reject-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t delta_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    uint32_t delta_flags = 0;

    memset(page_base, 0, sizeof(page_base));
    memset(page_base + 256U, 0xA5, 2048U);
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 76U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 980U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 77U);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 990U);
    for (uint32_t offset = 1024U; offset < 1088U; ++offset) {
        page_delta[offset] = (uint8_t)(0x30U + (offset & 0x3FU));
    }
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            77U,
            76U,
            980U,
            980U,
            page_base,
            sizeof(page_base),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            77U,
            76U,
            990U,
            990U,
            page_delta,
            sizeof(page_delta),
            &delta_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_STANDALONE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_RECORDS] == 1U);
    delta_flags = read_page_log_record_flags(fd, delta_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            77U,
            76U,
            990U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 990U);
    assert(commit_lsn == 990U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_skips_repeated_exact_standalone_probe(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-delta-repeated-standalone-reject-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta_one[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta_two[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t first_record_offset = 0;
    uint64_t second_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};

    memset(page_base, 0, sizeof(page_base));
    memset(page_base + 256U, 0xA5, 2048U);
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 78U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 1000U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 78U);

    memcpy(page_delta_one, page_base, sizeof(page_delta_one));
    store_test_be64(page_delta_one, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 1010U);
    for (uint32_t offset = 1024U; offset < 1088U; ++offset) {
        page_delta_one[offset] = (uint8_t)(0x40U + (offset & 0x3FU));
    }

    memcpy(page_delta_two, page_delta_one, sizeof(page_delta_two));
    store_test_be64(page_delta_two, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 1020U);
    for (uint32_t offset = 1536U; offset < 1600U; ++offset) {
        page_delta_two[offset] = (uint8_t)(0x80U ^ (offset & 0x3FU));
    }
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            78U,
            78U,
            1000U,
            1000U,
            page_base,
            sizeof(page_base),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            78U,
            78U,
            1010U,
            1010U,
            page_delta_one,
            sizeof(page_delta_one),
            &first_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_STANDALONE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_SKIPPED_STANDALONE_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_RECORDS] == 1U);
    assert(
        (read_page_log_record_flags(fd, first_record_offset) &
         MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            78U,
            78U,
            1020U,
            1020U,
            page_delta_two,
            sizeof(page_delta_two),
            &second_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_STANDALONE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_SKIPPED_STANDALONE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_SKIPPED_STANDALONE_PAYLOAD_BYTES] > 0U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_PAYLOAD_BYTES] ==
        stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_SKIPPED_STANDALONE_PAYLOAD_BYTES]
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_RECORDS] == 1U);
    assert(
        (read_page_log_record_flags(fd, second_record_offset) &
         MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U
    );

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            78U,
            78U,
            1020U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta_two));
    assert(page_lsn == 1020U);
    assert(commit_lsn == 1020U);
    assert(memcmp(out_page, page_delta_two, sizeof(page_delta_two)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_refreshes_delta_standalone_size_estimate(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-delta-standalone-estimate-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE * 2U];
    uint8_t page_delta_one[MYLITE_TEST_PAGE_SIZE * 2U];
    uint8_t page_delta_two[MYLITE_TEST_PAGE_SIZE * 2U];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE * 2U];
    uint64_t latest_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    uint32_t delta_flags = 0;

    memset(page_base, 0, sizeof(page_base));
    for (uint32_t offset = 96U; offset < 1120U; ++offset) {
        page_base[offset] = (uint8_t)(0x31U + (offset & 0x7FU));
    }
    for (uint32_t offset = 4096U; offset < sizeof(page_base); ++offset) {
        page_base[offset] = 0x6DU;
    }
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 46U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 950U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 74U);

    memcpy(page_delta_one, page_base, sizeof(page_delta_one));
    store_test_be64(page_delta_one, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 960U);
    for (uint32_t offset = 4608U; offset < 5208U; ++offset) {
        page_delta_one[offset] = (uint8_t)(0x10U + (offset & 0x3FU));
    }

    memcpy(page_delta_two, page_delta_one, sizeof(page_delta_two));
    store_test_be64(page_delta_two, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 970U);
    for (uint32_t offset = 5400U; offset < 5416U; ++offset) {
        page_delta_two[offset] = (uint8_t)(0x90U ^ (offset & 0x1FU));
    }
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            74U,
            46U,
            950U,
            950U,
            page_base,
            sizeof(page_base),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            74U,
            46U,
            960U,
            960U,
            page_delta_one,
            sizeof(page_delta_one),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            74U,
            46U,
            970U,
            970U,
            page_delta_two,
            sizeof(page_delta_two),
            &latest_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_EXACT_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_FAST_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_STANDALONE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_FAST_REJECTED_LIMIT_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REUSED_FAST_PAYLOAD_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_MATERIALIZE_SKIPPED_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_RECORDS] == 0U);
    delta_flags = read_page_log_record_flags(fd, latest_record_offset);
    assert((delta_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            74U,
            46U,
            970U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta_two));
    assert(page_lsn == 970U);
    assert(commit_lsn == 970U);
    assert(memcmp(out_page, page_delta_two, sizeof(page_delta_two)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_reuses_delta_base_slot_for_standalone_refresh(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-delta-standalone-slot-reuse-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_refresh[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_after_refresh[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t refresh_record_offset = 0;
    uint64_t after_refresh_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};

    memset(page_base, 0x5CU, sizeof(page_base));
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 34U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 840U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 75U);
    memset(out_page, 0xEE, sizeof(out_page));

    memcpy(page_refresh, page_base, sizeof(page_refresh));
    store_test_be64(page_refresh, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 850U);
    for (uint32_t offset = 512U; offset < 3000U; ++offset) {
        page_refresh[offset] ^= (uint8_t)(0x23U + (offset & 0x1FU));
    }

    memcpy(page_after_refresh, page_refresh, sizeof(page_after_refresh));
    store_test_be64(page_after_refresh, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 851U);
    page_after_refresh[3400] ^= 0x7DU;

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            75U,
            34U,
            840U,
            840U,
            page_base,
            sizeof(page_base),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            75U,
            34U,
            850U,
            850U,
            page_refresh,
            sizeof(page_refresh),
            &refresh_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_EXACT_REJECTED_STANDALONE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_STANDALONE_SIZE_PROBE_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_BASE_STANDALONE_SLOT_REUSE_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_BASE_PAGE_BUFFER_REUSE_RECORDS] == 1U);
    assert(
        (read_page_log_record_flags(fd, refresh_record_offset) &
         MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U
    );
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            refresh_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_refresh));
    assert(page_lsn == 850U);
    assert(commit_lsn == 850U);
    assert(memcmp(out_page, page_refresh, sizeof(page_refresh)) == 0);

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            75U,
            34U,
            851U,
            851U,
            page_after_refresh,
            sizeof(page_after_refresh),
            &after_refresh_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_BASE_STANDALONE_SLOT_REUSE_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DELTA_BASE_PAGE_BUFFER_REUSE_RECORDS] == 0U);
    assert(
        (read_page_log_record_flags(fd, after_refresh_record_offset) &
         MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U
    );
    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            after_refresh_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_after_refresh));
    assert(page_lsn == 851U);
    assert(commit_lsn == 851U);
    assert(memcmp(out_page, page_after_refresh, sizeof(page_after_refresh)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_refreshes_index_delta_base(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-delta-base-refresh-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_refresh[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_after_refresh[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t record_offset = 0;
    uint64_t refresh_record_offset = 0;
    uint64_t after_refresh_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    const unsigned int refresh_limit = 32U;

    memset(page_base, 0x5CU, sizeof(page_base));
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 33U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 600U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 71U);
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            71U,
            33U,
            600U,
            600U,
            page_base,
            sizeof(page_base),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    for (unsigned int delta = 0; delta < refresh_limit; ++delta) {
        memcpy(page_delta, page_base, sizeof(page_delta));
        store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 700U + delta);
        for (unsigned int byte_index = 0; byte_index <= delta; ++byte_index) {
            page_delta[128U + byte_index] ^= (uint8_t)(0x10U + (byte_index & 0x0FU));
        }
        assert(
            mylite_ownerless_page_log_append(
                fd,
                71U,
                33U,
                700U + delta,
                700U + delta,
                page_delta,
                sizeof(page_delta),
                &record_offset
            ) == MYLITE_OWNERLESS_PAGE_LOG_OK
        );
        assert(
            (read_page_log_record_flags(fd, record_offset) &
             MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U
        );
    }
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == refresh_limit);

    memcpy(page_refresh, page_base, sizeof(page_refresh));
    store_test_be64(page_refresh, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 800U);
    for (unsigned int byte_index = 0; byte_index <= refresh_limit; ++byte_index) {
        page_refresh[128U + byte_index] ^= (uint8_t)(0x30U + (byte_index & 0x0FU));
    }

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            71U,
            33U,
            800U,
            800U,
            page_refresh,
            sizeof(page_refresh),
            &refresh_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 0U);
    assert(
        (read_page_log_record_flags(fd, refresh_record_offset) &
         MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U
    );
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            refresh_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_refresh));
    assert(page_lsn == 800U);
    assert(commit_lsn == 800U);
    assert(memcmp(out_page, page_refresh, sizeof(page_refresh)) == 0);

    memcpy(page_after_refresh, page_refresh, sizeof(page_after_refresh));
    store_test_be64(page_after_refresh, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 801U);
    page_after_refresh[512] ^= 0x7DU;

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            71U,
            33U,
            801U,
            801U,
            page_after_refresh,
            sizeof(page_after_refresh),
            &after_refresh_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_DELTA_RECORDS] == 1U);
    assert(
        (read_page_log_record_flags(fd, after_refresh_record_offset) &
         MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U
    );
    memset(out_page, 0xEE, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            after_refresh_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_after_refresh));
    assert(page_lsn == 801U);
    assert(commit_lsn == 801U);
    assert(memcmp(out_page, page_after_refresh, sizeof(page_after_refresh)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_skips_index_fill_sparse_without_fill_runs(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "index-fill-sparse-prefilter-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};

    fill_innodb_test_page(page, 52U, 10U, 210U, 0x31U);
    store_test_be16(page, MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET, MYLITE_TEST_INNODB_PAGE_TYPE_INDEX);
    page[256] = 0x41U;
    page[300] = 0x42U;
    page[384] = 0x43U;
    page[512] = 0x44U;
    page[768] = 0x45U;
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(fd, 52U, 10U, 210U, 210U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_FILL_SPARSE_ZERO_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_ZERO_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SYS_RECORDS] == 0U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_PAYLOAD_BYTES] ==
        stats[PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_BYTES]
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_PAYLOAD_BYTES] < sizeof(page));
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_INDEX_COMPACT_SPARSE_DATA_BYTES] > 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            52U,
            10U,
            210U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page));
    assert(page_lsn == 210U);
    assert(commit_lsn == 210U);
    assert(memcmp(out_page, page, sizeof(page)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_falls_back_to_compact_sparse_zero_payloads(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "compact-sparse-fallback-page-log.bin");
    int fd = open_file(log_path);
    const size_t page_size = 32768U;
    const size_t nonzero_offset = 20000U;
    const size_t nonzero_size = 128U;
    uint8_t *page = calloc(page_size, 1U);
    uint8_t *out_page = malloc(page_size);
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    struct stat log_stat = {0};
    const size_t sparse_payload_size = sizeof(uint16_t) + (2U * sizeof(uint16_t)) + nonzero_size;
    const size_t compact_sparse_metadata_size = sizeof(uint16_t) + (2U * sizeof(uint16_t));
    const size_t compact_sparse_data_size = nonzero_size;

    assert(page != NULL);
    assert(out_page != NULL);
    for (size_t i = 0; i < nonzero_size; ++i) {
        page[nonzero_offset + i] = (uint8_t)(0x40U + (i % 63U));
    }
    memset(out_page, 0xEE, page_size);

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            10U,
            1U,
            170U,
            170U,
            page,
            (uint32_t)page_size,
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(record_offset == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(fstat(fd, &log_stat) == 0);
    assert(
        log_stat.st_size ==
        (off_t)(record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE + sparse_payload_size)
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_PAYLOAD_BYTES] == sparse_payload_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_ZERO_RECORDS] == 1U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES] == sparse_payload_size
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_VARINT_COMPACT_SPARSE_ZERO_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_VARINT_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES] == 0U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_METADATA_BYTES] ==
        compact_sparse_metadata_size
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_DATA_BYTES] == compact_sparse_data_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_PAYLOAD_BYTES] == sparse_payload_size);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_COMPACT_SPARSE_METADATA_BYTES] ==
        compact_sparse_metadata_size
    );
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_COMPACT_SPARSE_DATA_BYTES] ==
        compact_sparse_data_size
    );

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            10U,
            1U,
            170U,
            out_page,
            page_size,
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == (uint32_t)page_size);
    assert(page_lsn == 170U);
    assert(commit_lsn == 170U);
    assert(memcmp(out_page, page, page_size) == 0);

    assert(close(fd) == 0);
    free(out_page);
    free(page);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_falls_back_to_legacy_sparse_zero_payloads(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "legacy-sparse-payload-page-log.bin");
    int fd = open_file(log_path);
    const size_t page_size = 70000U;
    const size_t nonzero_offset = 66000U;
    const size_t sparse_payload_size = sizeof(uint32_t) + (2U * sizeof(uint32_t)) + 1U;
    uint8_t *page = calloc(page_size, 1U);
    uint8_t *out_page = malloc(page_size);
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    struct stat log_stat = {0};

    assert(page != NULL);
    assert(out_page != NULL);
    page[nonzero_offset] = 0x5A;
    memset(out_page, 0xEE, page_size);

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            11U,
            1U,
            180U,
            180U,
            page,
            (uint32_t)page_size,
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(record_offset == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(fstat(fd, &log_stat) == 0);
    assert(
        log_stat.st_size ==
        (off_t)(record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE + sparse_payload_size)
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPARSE_ZERO_PAYLOAD_BYTES] == sparse_payload_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_ZERO_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_VARINT_COMPACT_SPARSE_ZERO_RECORDS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_VARINT_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_METADATA_BYTES] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_COMPACT_SPARSE_DATA_BYTES] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_RECORDS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_PAYLOAD_BYTES] == sparse_payload_size);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_COMPACT_SPARSE_METADATA_BYTES] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SPACE_METADATA_COMPACT_SPARSE_DATA_BYTES] == 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            11U,
            1U,
            180U,
            out_page,
            page_size,
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == (uint32_t)page_size);
    assert(page_lsn == 180U);
    assert(commit_lsn == 180U);
    assert(memcmp(out_page, page, page_size) == 0);

    assert(close(fd) == 0);
    free(out_page);
    free(page);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_initialized_append_uses_existing_header(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "initialized-append-page-log.bin");
    int fd = open_file(log_path);
    const uint64_t log_offset = 256U;
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t out_page[16];
    uint64_t first_record_offset = 0;
    uint64_t second_record_offset = 0;
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;
    uint64_t out_commit_lsn = 0;

    memset(page_v1, 0x41, sizeof(page_v1));
    memset(page_v2, 0x42, sizeof(page_v2));
    memset(out_page, 0, sizeof(out_page));
    truncate_file(fd, (off_t)log_offset);

    assert(
        mylite_ownerless_page_log_append_initialized_at(
            fd,
            log_offset,
            4U,
            5U,
            90U,
            90U,
            page_v1,
            sizeof(page_v1),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );

    assert(mylite_ownerless_page_log_initialize_at(fd, log_offset) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append_initialized_at(
            fd,
            log_offset,
            4U,
            5U,
            100U,
            100U,
            page_v1,
            sizeof(page_v1),
            &first_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(first_record_offset == log_offset + MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(
        mylite_ownerless_page_log_append_initialized_at(
            fd,
            log_offset,
            4U,
            5U,
            120U,
            120U,
            page_v2,
            sizeof(page_v2),
            &second_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        second_record_offset ==
        first_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE + sizeof(page_v1)
    );

    assert(
        mylite_ownerless_page_log_find_latest_at(
            fd,
            log_offset,
            4U,
            5U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v2));
    assert(out_page_lsn == 120U);
    assert(out_commit_lsn == 120U);
    assert(memcmp(out_page, page_v2, sizeof(page_v2)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_initialized_sync_uses_existing_header(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "initialized-sync-page-log.bin");
    int fd = open_file(log_path);
    const uint64_t log_offset = 256U;
    uint8_t page[16];
    uint64_t stats[PAGE_LOG_SYNC_PERF_STAT_COUNT] = {0};
    uint64_t current_end_offset = 0;
    uint64_t current_generation = 0;
    uint64_t previous_end_offset = 0;
    uint64_t previous_generation = 0;
    int synced = 0;

    memset(page, 0x43, sizeof(page));
    truncate_file(fd, (off_t)log_offset);

    assert(
        mylite_ownerless_page_log_sync_initialized_at(fd, log_offset) ==
        MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(mylite_ownerless_page_log_initialize_at(fd, log_offset) == MYLITE_OWNERLESS_PAGE_LOG_OK);

    mylite_ownerless_page_log_reset_sync_perf_stats();
    mylite_ownerless_page_log_set_sync_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_sync_initialized_if_changed_at(
            fd,
            log_offset,
            0U,
            0U,
            &current_end_offset,
            &current_generation,
            &synced
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(synced == 1);
    assert(current_end_offset >= log_offset + MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(current_generation != 0U);
    previous_end_offset = current_end_offset;
    previous_generation = current_generation;

    synced = 1;
    assert(
        mylite_ownerless_page_log_sync_initialized_if_changed_at(
            fd,
            log_offset,
            previous_end_offset,
            previous_generation,
            &current_end_offset,
            &current_generation,
            &synced
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(synced == 0);
    assert(current_end_offset == previous_end_offset);
    assert(current_generation == previous_generation);

    assert(
        mylite_ownerless_page_log_append_initialized_at(
            fd,
            log_offset,
            4U,
            6U,
            100U,
            100U,
            page,
            sizeof(page),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    synced = 0;
    assert(
        mylite_ownerless_page_log_sync_initialized_if_changed_at(
            fd,
            log_offset,
            previous_end_offset,
            previous_generation,
            &current_end_offset,
            &current_generation,
            &synced
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(synced == 1);
    assert(current_end_offset > previous_end_offset);
    assert(current_generation == previous_generation);
    assert(mylite_ownerless_page_log_sync_at(fd, log_offset) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_set_sync_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_sync_perf_stats(stats, PAGE_LOG_SYNC_PERF_STAT_COUNT);
    assert(stats[PAGE_LOG_SYNC_PERF_STAT_CALLS] == 4U);
    assert(stats[PAGE_LOG_SYNC_PERF_STAT_SKIPPED_CLEAN] == 1U);
    assert(stats[PAGE_LOG_SYNC_PERF_STAT_DATA_SYNC_NS] <= stats[PAGE_LOG_SYNC_PERF_STAT_TOTAL_NS]);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_reads_under_existing_read_lock(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "guarded-page-log.bin");
    int fd = open_file(log_path);
    const uint64_t log_offset = 128U;
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t out_page[16];
    uint64_t record_offset = 0;
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;
    uint64_t out_commit_lsn = 0;

    memset(page_v1, 0x31, sizeof(page_v1));
    memset(page_v2, 0x32, sizeof(page_v2));
    memset(out_page, 0, sizeof(out_page));
    truncate_file(fd, (off_t)log_offset);

    assert(mylite_ownerless_page_log_initialize_at(fd, log_offset) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append_at(
            fd,
            log_offset,
            7U,
            8U,
            100U,
            100U,
            page_v1,
            sizeof(page_v1),
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_at(
            fd,
            log_offset,
            7U,
            8U,
            120U,
            120U,
            page_v2,
            sizeof(page_v2),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(mylite_ownerless_page_log_begin_read(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_read_page_under_read_lock_at(
            fd,
            log_offset,
            record_offset,
            7U,
            8U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v1));
    assert(out_page_lsn == 100U);
    assert(out_commit_lsn == 100U);
    assert(memcmp(out_page, page_v1, sizeof(page_v1)) == 0);

    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest_under_read_lock_at(
            fd,
            log_offset,
            7U,
            8U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v2));
    assert(out_page_lsn == 120U);
    assert(out_commit_lsn == 120U);
    assert(memcmp(out_page, page_v2, sizeof(page_v2)) == 0);

    assert(
        mylite_ownerless_page_log_find_latest_under_read_lock_at(
            fd,
            log_offset,
            7U,
            9U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );
    mylite_ownerless_page_log_end_read(fd);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_tail_scan_absence_generation(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "tail-scan-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_a[16];
    uint8_t page_b[16];
    uint8_t out_page[16];
    uint64_t first_snapshot_end = 0;
    uint64_t second_snapshot_end = 0;
    uint64_t initial_generation = 0;
    uint64_t second_generation = 0;
    uint64_t checkpoint_generation = 0;
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;
    uint64_t out_commit_lsn = 0;
    int saw_page_record = -1;
    int checkpointed = 0;

    memset(page_a, 0x41, sizeof(page_a));
    memset(page_b, 0x42, sizeof(page_b));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(fd, 10U, 1U, 100U, 100U, page_a, sizeof(page_a), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(mylite_ownerless_page_log_begin_read(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_snapshot_under_read_lock_at(
            fd,
            0U,
            &first_snapshot_end,
            &initial_generation
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(initial_generation != 0U);
    assert(first_snapshot_end > MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(
        mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at(
            fd,
            0U,
            0U,
            first_snapshot_end,
            10U,
            2U,
            100U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn,
            &saw_page_record
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );
    assert(saw_page_record == 0);
    mylite_ownerless_page_log_end_read(fd);

    assert(
        mylite_ownerless_page_log_append(fd, 11U, 1U, 120U, 120U, page_b, sizeof(page_b), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(mylite_ownerless_page_log_begin_read(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_snapshot_under_read_lock_at(
            fd,
            0U,
            &second_snapshot_end,
            &second_generation
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(second_generation == initial_generation);
    assert(second_snapshot_end > first_snapshot_end);
    saw_page_record = -1;
    assert(
        mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at(
            fd,
            0U,
            first_snapshot_end,
            second_snapshot_end,
            10U,
            2U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn,
            &saw_page_record
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );
    assert(saw_page_record == 0);

    saw_page_record = -1;
    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at(
            fd,
            0U,
            first_snapshot_end,
            second_snapshot_end,
            11U,
            1U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn,
            &saw_page_record
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(saw_page_record == 1);
    assert(out_page_size == sizeof(page_b));
    assert(out_page_lsn == 120U);
    assert(out_commit_lsn == 120U);
    assert(memcmp(out_page, page_b, sizeof(page_b)) == 0);
    mylite_ownerless_page_log_end_read(fd);

    assert(
        mylite_ownerless_page_log_checkpoint_if_safe(fd, 120U, &checkpointed) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpointed == 1);
    assert(mylite_ownerless_page_log_begin_read(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_snapshot_under_read_lock_at(
            fd,
            0U,
            &second_snapshot_end,
            &checkpoint_generation
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_generation != second_generation);
    mylite_ownerless_page_log_end_read(fd);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_uses_reader_snapshots(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "snapshot-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t out_page[16];
    uint64_t snapshot_end_offset = 0;
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;
    uint64_t out_commit_lsn = 0;

    memset(page_v1, 0x11, sizeof(page_v1));
    memset(page_v2, 0x22, sizeof(page_v2));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(fd, 42U, 7U, 100U, 100U, page_v1, sizeof(page_v1), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_snapshot(fd, &snapshot_end_offset) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(fd, 42U, 7U, 120U, 120U, page_v2, sizeof(page_v2), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_page_log_find_latest_in_snapshot(
            fd,
            snapshot_end_offset,
            42U,
            7U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v1));
    assert(out_page_lsn == 100U);
    assert(out_commit_lsn == 100U);
    assert(memcmp(out_page, page_v1, sizeof(page_v1)) == 0);

    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v2));
    assert(out_page_lsn == 120U);
    assert(out_commit_lsn == 120U);
    assert(memcmp(out_page, page_v2, sizeof(page_v2)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_accepts_legacy_checksum_records(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *log_path = path_join(root, "legacy-checksum-page-log.bin");
    int fd = open_file(log_path);
    uint8_t *index = calloc(1U, index_size);
    page_log_replay_context context = {.page_index = index, .page_index_size = index_size};
    uint8_t page[32];
    uint8_t out_page[32];
    uint8_t legacy_checksum[8] = {0};
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    int checkpointed = -1;
    struct stat log_stat;

    assert(index != NULL);
    memset(page, 0xAB, sizeof(page));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            90U,
            100U,
            page,
            sizeof(page),
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    store_test_le64(legacy_checksum, 0U, legacy_page_log_checksum(page, sizeof(page)));
    write_file_at(
        fd,
        legacy_checksum,
        sizeof(legacy_checksum),
        (off_t)(record_offset + MYLITE_TEST_PAGE_LOG_RECORD_PAYLOAD_CHECKSUM_OFFSET)
    );

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            100U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page));
    assert(page_lsn == 90U);
    assert(commit_lsn == 100U);
    assert(memcmp(out_page, page, sizeof(page)) == 0);

    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page));
    assert(page_lsn == 90U);
    assert(commit_lsn == 100U);
    assert(memcmp(out_page, page, sizeof(page)) == 0);

    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_log_replay_at(fd, 0U, replay_page_log_record_into_index, &context) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            100U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(page_lsn == 90U);
    assert(commit_lsn == 100U);

    assert(
        mylite_ownerless_page_log_checkpoint_if_safe(fd, 100U, &checkpointed) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpointed == 1);
    assert(fstat(fd, &log_stat) == 0);
    assert(log_stat.st_size == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);

    assert(close(fd) == 0);
    free(index);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_tolerates_corrupt_tail_record(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *log_path = path_join(root, "corrupt-tail-page-log.bin");
    int fd = open_file(log_path);
    uint8_t *index = calloc(1U, index_size);
    page_log_replay_context context = {.page_index = index, .page_index_size = index_size};
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t out_page[16];
    uint64_t first_offset = 0;
    uint64_t second_offset = 0;
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    const uint8_t corrupt_byte = 0xCCU;
    int checkpointed = -1;
    struct stat log_stat;

    assert(index != NULL);
    memset(page_v1, 0x11, sizeof(page_v1));
    memset(page_v2, 0x22, sizeof(page_v2));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(mylite_ownerless_page_log_sync(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            90U,
            100U,
            page_v1,
            sizeof(page_v1),
            &first_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            110U,
            120U,
            page_v2,
            sizeof(page_v2),
            &second_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        pwrite(
            fd,
            &corrupt_byte,
            sizeof(corrupt_byte),
            (off_t)(second_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE)
        ) == sizeof(corrupt_byte)
    );

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v1));
    assert(page_lsn == 90U);
    assert(commit_lsn == 100U);
    assert(memcmp(out_page, page_v1, sizeof(page_v1)) == 0);

    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_log_replay_at(fd, 0U, replay_page_log_record_into_index, &context) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == first_offset);
    assert(page_lsn == 90U);
    assert(commit_lsn == 100U);

    assert(
        mylite_ownerless_page_log_checkpoint_if_safe(fd, 120U, &checkpointed) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpointed == 1);
    assert(fstat(fd, &log_stat) == 0);
    assert(log_stat.st_size == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);

    assert(close(fd) == 0);
    free(index);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_rejects_corrupt_interior_record(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *log_path = path_join(root, "corrupt-interior-page-log.bin");
    int fd = open_file(log_path);
    uint8_t *index = calloc(1U, index_size);
    page_log_replay_context context = {.page_index = index, .page_index_size = index_size};
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t out_page[16];
    uint64_t first_offset = 0;
    uint64_t second_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    int checkpointed = -1;
    const uint8_t corrupt_byte = 0xCCU;

    assert(index != NULL);
    memset(page_v1, 0x11, sizeof(page_v1));
    memset(page_v2, 0x22, sizeof(page_v2));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            90U,
            100U,
            page_v1,
            sizeof(page_v1),
            &first_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            110U,
            120U,
            page_v2,
            sizeof(page_v2),
            &second_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(second_offset > first_offset);
    assert(
        pwrite(
            fd,
            &corrupt_byte,
            sizeof(corrupt_byte),
            (off_t)(first_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE)
        ) == sizeof(corrupt_byte)
    );

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_log_replay_at(fd, 0U, replay_page_log_record_into_index, &context) ==
        MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(
        mylite_ownerless_page_log_checkpoint_if_safe(fd, 120U, &checkpointed) ==
        MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(checkpointed == 0);

    assert(close(fd) == 0);
    free(index);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_checkpoints_retained_records(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *log_path = path_join(root, "checkpoint-page-log.bin");
    int fd = open_file(log_path);
    uint8_t *index = calloc(1U, index_size);
    page_log_checkpoint_index_context checkpoint_context = {0};
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t other_page[16];
    uint8_t out_page[16];
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;

    assert(index != NULL);
    checkpoint_context.page_index = index;
    checkpoint_context.page_index_size = index_size;
    memset(page_v1, 0x11, sizeof(page_v1));
    memset(page_v2, 0x22, sizeof(page_v2));
    memset(other_page, 0x33, sizeof(other_page));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(fd, 42U, 7U, 90U, 100U, page_v1, sizeof(page_v1), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(fd, 42U, 7U, 110U, 120U, page_v2, sizeof(page_v2), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            43U,
            8U,
            130U,
            140U,
            other_page,
            sizeof(other_page),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_require_wal_scan(index, index_size, 1U, 10U) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_log_checkpoint_with_completion(
            fd,
            110U,
            capture_page_log_record_for_checkpoint_index,
            replace_page_index_after_page_log_checkpoint,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.retained.count == 2U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            110U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            140U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v2));
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(memcmp(out_page, page_v2, sizeof(page_v2)) == 0);

    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            140U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v2));
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(memcmp(out_page, page_v2, sizeof(page_v2)) == 0);

    assert(close(fd) == 0);
    free(index);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_preserves_oldest_snapshot_boundary(void) {
    char *root = make_temp_root();
    char *busy_log_path = path_join(root, "missing-boundary-page-log.bin");
    char *checkpoint_log_path = path_join(root, "snapshot-boundary-page-log.bin");
    char *single_snapshot_log_path = path_join(root, "single-snapshot-boundary-page-log.bin");
    int fd = open_file(busy_log_path);
    page_log_checkpoint_index_context checkpoint_context = {0};
    uint8_t page_before[16];
    uint8_t page_boundary[16];
    uint8_t page_after_first[16];
    uint8_t page_after_second[16];
    uint8_t page_independent[16];
    uint8_t out_page[16];
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;

    memset(page_before, 0x10, sizeof(page_before));
    memset(page_boundary, 0x20, sizeof(page_boundary));
    memset(page_after_first, 0x30, sizeof(page_after_first));
    memset(page_after_second, 0x40, sizeof(page_after_second));
    memset(page_independent, 0x50, sizeof(page_independent));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            140U,
            140U,
            page_after_second,
            sizeof(page_after_second),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
            fd,
            0U,
            140U,
            100U,
            capture_page_log_record_for_checkpoint_index,
            prepare_page_log_checkpoint,
            NULL,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_BUSY
    );
    assert(checkpoint_context.prepare_count == 0U);
    assert(checkpoint_context.retained.count == 0U);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            140U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(commit_lsn == 140U);
    assert(close(fd) == 0);

    fd = open_file(checkpoint_log_path);
    memset(&checkpoint_context, 0, sizeof(checkpoint_context));
    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            80U,
            80U,
            page_before,
            sizeof(page_before),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            100U,
            100U,
            page_boundary,
            sizeof(page_boundary),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            120U,
            120U,
            page_after_first,
            sizeof(page_after_first),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            140U,
            140U,
            page_after_second,
            sizeof(page_after_second),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            43U,
            8U,
            100U,
            100U,
            page_independent,
            sizeof(page_independent),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
            fd,
            0U,
            140U,
            100U,
            capture_page_log_record_for_checkpoint_index,
            prepare_page_log_checkpoint,
            NULL,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.prepare_count == 1U);
    assert(checkpoint_context.retained.count == 3U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            100U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(commit_lsn == 100U);
    assert(memcmp(out_page, page_boundary, sizeof(page_boundary)) == 0);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            140U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(commit_lsn == 140U);
    assert(memcmp(out_page, page_after_second, sizeof(page_after_second)) == 0);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            43U,
            8U,
            100U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    assert(close(fd) == 0);

    fd = open_file(single_snapshot_log_path);
    memset(&checkpoint_context, 0, sizeof(checkpoint_context));
    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            80U,
            80U,
            page_before,
            sizeof(page_before),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            100U,
            100U,
            page_boundary,
            sizeof(page_boundary),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            120U,
            120U,
            page_after_first,
            sizeof(page_after_first),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            140U,
            140U,
            page_after_second,
            sizeof(page_after_second),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            43U,
            8U,
            100U,
            100U,
            page_independent,
            sizeof(page_independent),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_page_log_checkpoint_preserving_single_snapshot_at(
            fd,
            0U,
            140U,
            100U,
            capture_page_log_record_for_checkpoint_index,
            prepare_page_log_checkpoint,
            NULL,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.prepare_count == 1U);
    assert(checkpoint_context.retained.count == 1U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            140U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(commit_lsn == 100U);
    assert(memcmp(out_page, page_boundary, sizeof(page_boundary)) == 0);

    assert(close(fd) == 0);
    free(single_snapshot_log_path);
    free(checkpoint_log_path);
    free(busy_log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_preserves_external_snapshot_lineage_metadata(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "external-snapshot-lineage-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_delta[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t base_record_offset = 0;
    uint64_t lineage_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint32_t lineage_flags = 0;
    uint32_t lineage_metadata_flags = 0;
    uint32_t retained_flags = 0;
    uint32_t retained_metadata_flags = 0;
    int is_lineage = 0;
    int is_snapshot_boundary = 0;
    page_log_checkpoint_index_context checkpoint_context = {0};

    memset(page_base, 0x71, sizeof(page_base));
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 21U);
    store_test_be64(page_base, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 700U);
    store_test_be16(
        page_base,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_INDEX
    );
    store_test_be32(page_base, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 80U);

    memcpy(page_delta, page_base, sizeof(page_delta));
    store_test_be64(page_delta, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 710U);
    page_delta[128] ^= 0x19U;
    page_delta[2048] ^= 0x27U;
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            80U,
            21U,
            700U,
            700U,
            page_base,
            sizeof(page_base),
            &base_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_external_snapshot_lineage_initialized_at(
            fd,
            0U,
            80U,
            21U,
            710U,
            710U,
            page_delta,
            sizeof(page_delta),
            &lineage_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(lineage_record_offset > base_record_offset);

    assert(
        mylite_ownerless_page_log_record_is_external_snapshot_lineage_at(
            fd,
            lineage_record_offset,
            &is_lineage
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(is_lineage == 1);
    assert(
        mylite_ownerless_page_log_record_is_snapshot_boundary_at(
            fd,
            lineage_record_offset,
            &is_snapshot_boundary
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(is_snapshot_boundary == 0);
    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            lineage_record_offset,
            &lineage_metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    lineage_flags = read_page_log_record_flags(fd, lineage_record_offset);
    assert((lineage_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_EXTERNAL_SNAPSHOT_LINEAGE) != 0U);
    assert((lineage_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) != 0U);
    assert(
        lineage_metadata_flags ==
        (lineage_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_EXTERNAL_SNAPSHOT_LINEAGE)
    );

    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            705U,
            capture_page_log_record_for_checkpoint_index,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.retained.count == 1U);
    assert(checkpoint_context.retained.records[0].commit_lsn == 710U);
    retained_flags =
        read_page_log_record_flags(fd, checkpoint_context.retained.records[0].record_offset);
    assert((retained_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_EXTERNAL_SNAPSHOT_LINEAGE) != 0U);
    assert((retained_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_INDEX_DELTA) == 0U);
    is_lineage = 0;
    assert(
        mylite_ownerless_page_log_record_is_external_snapshot_lineage_at(
            fd,
            checkpoint_context.retained.records[0].record_offset,
            &is_lineage
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(is_lineage == 1);
    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            checkpoint_context.retained.records[0].record_offset,
            &retained_metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        retained_metadata_flags ==
        (retained_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_EXTERNAL_SNAPSHOT_LINEAGE)
    );

    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            checkpoint_context.retained.records[0].record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_delta));
    assert(page_lsn == 710U);
    assert(commit_lsn == 710U);
    assert(memcmp(out_page, page_delta, sizeof(page_delta)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_external_snapshot_lineage_session_append(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "external-snapshot-lineage-session-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint64_t record_offset = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    mylite_ownerless_page_log_append_session session = {0};
    int is_lineage = 0;
    uint32_t flags = 0;

    memset(page, 0x81, sizeof(page));
    store_test_be32(page, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, 22U);
    store_test_be64(page, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 720U);
    store_test_be16(page, MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET, MYLITE_TEST_INNODB_PAGE_TYPE_INDEX);
    store_test_be32(page, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, 81U);

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append_session_begin_initialized_at(fd, 0U, &session) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_external_snapshot_lineage_session_append_with_checksum_and_options(
            fd,
            &session,
            81U,
            22U,
            720U,
            720U,
            page,
            sizeof(page),
            mylite_ownerless_page_log_checksum_page(page, sizeof(page)),
            0U,
            &record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_append_session_end(fd, &session);
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DIRECT_APPEND_CALLS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SESSION_BEGIN_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SESSION_APPEND_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SESSION_END_CALLS] == 1U);
    assert(
        mylite_ownerless_page_log_record_is_external_snapshot_lineage_at(
            fd,
            record_offset,
            &is_lineage
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(is_lineage == 1);
    flags = read_page_log_record_flags(fd, record_offset);
    assert((flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_EXTERNAL_SNAPSHOT_LINEAGE) != 0U);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_preserves_native_support_metadata(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "native-support-metadata-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t page_native[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t base_record_offset = 0;
    uint64_t native_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint32_t native_flags = 0;
    uint32_t native_metadata_flags = 0;
    uint32_t retained_flags = 0;
    uint32_t retained_metadata_flags = 0;
    int is_native_support = 0;
    page_log_checkpoint_index_context checkpoint_context = {0};

    fill_innodb_test_page(page_base, 90U, 4U, 200U, 0x21U);
    memcpy(page_native, page_base, sizeof(page_native));
    store_test_be64(page_native, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, 220U);
    page_native[128] = 0x35U;
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            90U,
            4U,
            200U,
            200U,
            page_base,
            sizeof(page_base),
            &base_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_initialized_at_with_checksum_and_options(
            fd,
            0U,
            90U,
            4U,
            220U,
            220U,
            page_native,
            sizeof(page_native),
            mylite_ownerless_page_log_checksum_page(page_native, sizeof(page_native)),
            MYLITE_OWNERLESS_PAGE_LOG_APPEND_NATIVE_SUPPORT_STATE,
            &native_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(native_record_offset > base_record_offset);

    assert(
        mylite_ownerless_page_log_record_is_native_support_state_at(
            fd,
            base_record_offset,
            &is_native_support
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(is_native_support == 0);
    assert(
        mylite_ownerless_page_log_record_is_native_support_state_at(
            fd,
            native_record_offset,
            &is_native_support
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(is_native_support == 1);
    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            native_record_offset,
            &native_metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    native_flags = read_page_log_record_flags(fd, native_record_offset);
    assert((native_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_NATIVE_SUPPORT_STATE) != 0U);
    assert(
        native_metadata_flags ==
        (native_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_NATIVE_SUPPORT_STATE)
    );

    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            210U,
            capture_page_log_record_for_checkpoint_index,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.retained.count == 1U);
    assert(checkpoint_context.retained.records[0].commit_lsn == 220U);
    retained_flags =
        read_page_log_record_flags(fd, checkpoint_context.retained.records[0].record_offset);
    assert((retained_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_NATIVE_SUPPORT_STATE) != 0U);
    assert(
        mylite_ownerless_page_log_record_is_native_support_state_at(
            fd,
            checkpoint_context.retained.records[0].record_offset,
            &is_native_support
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(is_native_support == 1);
    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            checkpoint_context.retained.records[0].record_offset,
            &retained_metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        retained_metadata_flags ==
        (retained_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_NATIVE_SUPPORT_STATE)
    );

    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            checkpoint_context.retained.records[0].record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_native));
    assert(page_lsn == 220U);
    assert(commit_lsn == 220U);
    assert(memcmp(out_page, page_native, sizeof(page_native)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_skips_proof_only_native_support_records(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "proof-only-native-support-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint64_t base_record_offset = 0;
    uint64_t proof_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint32_t metadata_flags = 0;
    uint32_t record_flags = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    page_log_retained_records replay_records = {0};
    page_log_retained_records proof_replay_records = {0};
    page_log_checkpoint_index_context checkpoint_context = {0};
    int is_native_support = 0;

    fill_innodb_test_page(page_base, 90U, 4U, 200U, 0x21U);
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            90U,
            4U,
            200U,
            200U,
            page_base,
            sizeof(page_base),
            &base_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append_initialized_at_with_checksum_and_options(
            fd,
            0U,
            90U,
            4U,
            220U,
            220U,
            NULL,
            sizeof(page_base),
            0U,
            MYLITE_OWNERLESS_PAGE_LOG_APPEND_NATIVE_SUPPORT_STATE |
                MYLITE_OWNERLESS_PAGE_LOG_APPEND_PROOF_ONLY,
            &proof_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(proof_record_offset > base_record_offset);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_BYTES] == 0U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_RECORD_HEADER_BYTES] ==
        MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_PRECOMPUTED_CHECKSUM_RECORDS] == 0U);

    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            proof_record_offset,
            &metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_NATIVE_SUPPORT_STATE) != 0U);
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_PROOF_ONLY) != 0U);
    record_flags = read_page_log_record_flags(fd, proof_record_offset);
    assert((record_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_NATIVE_SUPPORT_STATE) != 0U);
    assert((record_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_PROOF_ONLY) != 0U);
    assert(
        mylite_ownerless_page_log_record_is_native_support_state_at(
            fd,
            proof_record_offset,
            &is_native_support
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(is_native_support == 1);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            90U,
            4U,
            300U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_base));
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);
    assert(memcmp(out_page, page_base, sizeof(page_base)) == 0);
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            proof_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    assert(
        mylite_ownerless_page_log_replay_at(
            fd,
            0U,
            capture_page_log_record_for_index_replace,
            &replay_records
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(replay_records.count == 1U);
    assert(replay_records.records[0].record_offset == base_record_offset);
    assert(
        mylite_ownerless_page_log_replay_at_including_proof_only(
            fd,
            0U,
            capture_page_log_record_for_index_replace,
            &proof_replay_records
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(proof_replay_records.count == 2U);
    assert(proof_replay_records.records[0].record_offset == base_record_offset);
    assert(proof_replay_records.records[1].record_offset == proof_record_offset);

    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            210U,
            capture_page_log_record_for_checkpoint_index,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.retained.count == 0U);
    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE,
            &metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_NATIVE_SUPPORT_STATE) != 0U);
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_PROOF_ONLY) != 0U);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            90U,
            4U,
            300U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_appends_native_support_proof_pairs(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "native-support-proof-pair-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_base[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    mylite_ownerless_page_log_append_session session = {0};
    uint64_t base_record_offset = 0;
    uint64_t first_record_offset = 0;
    uint64_t second_record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    uint32_t metadata_flags = 0;
    uint32_t record_flags = 0;
    uint64_t stats[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};
    page_log_retained_records replay_records = {0};
    page_log_checkpoint_index_context checkpoint_context = {0};
    int is_native_support = 0;

    fill_innodb_test_page(page_base, 91U, 4U, 200U, 0x23U);
    memset(out_page, 0xEE, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            91U,
            4U,
            200U,
            200U,
            page_base,
            sizeof(page_base),
            &base_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    mylite_ownerless_page_log_reset_append_perf_stats();
    mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
    assert(
        mylite_ownerless_page_log_append_session_begin_initialized_at(fd, 0U, &session) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_session_append_native_support_proof_pair(
            fd,
            &session,
            91U,
            4U,
            220U,
            sizeof(page_base),
            5U,
            225U,
            sizeof(page_base),
            230U,
            &first_record_offset,
            &second_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    mylite_ownerless_page_log_append_session_end(fd, &session);
    mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
    mylite_ownerless_page_log_read_append_perf_stats(stats, PAGE_LOG_APPEND_PERF_STAT_COUNT);

    assert(first_record_offset > base_record_offset);
    assert(
        second_record_offset == first_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_CALLS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_DIRECT_APPEND_CALLS] == 0U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SESSION_BEGIN_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SESSION_APPEND_CALLS] == 2U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_SESSION_END_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_BYTES] == 0U);
    assert(
        stats[PAGE_LOG_APPEND_PERF_STAT_RECORD_HEADER_BYTES] ==
        2U * MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE
    );
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_RECORD_HEADER_WRITE_CALLS] == 1U);
    assert(stats[PAGE_LOG_APPEND_PERF_STAT_PRECOMPUTED_CHECKSUM_RECORDS] == 0U);

    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            first_record_offset,
            &metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_NATIVE_SUPPORT_STATE) != 0U);
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_PROOF_ONLY) != 0U);
    record_flags = read_page_log_record_flags(fd, first_record_offset);
    assert((record_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_NATIVE_SUPPORT_STATE) != 0U);
    assert((record_flags & MYLITE_TEST_PAGE_LOG_RECORD_FLAG_PROOF_ONLY) != 0U);
    assert(
        mylite_ownerless_page_log_record_is_native_support_state_at(
            fd,
            first_record_offset,
            &is_native_support
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(is_native_support == 1);

    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            second_record_offset,
            &metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_NATIVE_SUPPORT_STATE) != 0U);
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_PROOF_ONLY) != 0U);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            91U,
            4U,
            300U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_base));
    assert(page_lsn == 200U);
    assert(commit_lsn == 200U);
    assert(memcmp(out_page, page_base, sizeof(page_base)) == 0);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            91U,
            5U,
            300U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            first_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );
    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            second_record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    assert(
        mylite_ownerless_page_log_replay_at(
            fd,
            0U,
            capture_page_log_record_for_index_replace,
            &replay_records
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(replay_records.count == 1U);
    assert(replay_records.records[0].record_offset == base_record_offset);

    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            210U,
            capture_page_log_record_for_checkpoint_index,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.retained.count == 0U);
    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE,
            &metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_PROOF_ONLY) != 0U);
    assert(
        mylite_ownerless_page_log_record_metadata_flags_at(
            fd,
            MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
            &metadata_flags
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_PROOF_ONLY) != 0U);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_readable_record_scan_ignores_proof_only_records(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "proof-only-readable-record-scan-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint64_t proof_record_offset = 0;
    uint64_t payload_record_offset = 0;
    int has_records = -1;
    const uint8_t corrupt_tail = 0xCCU;
    uint8_t corrupt_header[MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE];

    fill_innodb_test_page(page, 92U, 4U, 200U, 0x25U);
    memset(corrupt_header, 0xCC, sizeof(corrupt_header));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_has_readable_page_records_at(fd, 0U, &has_records) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(has_records == 0);

    assert(
        mylite_ownerless_page_log_append_initialized_at_with_checksum_and_options(
            fd,
            0U,
            92U,
            4U,
            220U,
            220U,
            NULL,
            sizeof(page),
            0U,
            MYLITE_OWNERLESS_PAGE_LOG_APPEND_NATIVE_SUPPORT_STATE |
                MYLITE_OWNERLESS_PAGE_LOG_APPEND_PROOF_ONLY,
            &proof_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    has_records = -1;
    assert(
        mylite_ownerless_page_log_has_readable_page_records_at(fd, 0U, &has_records) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(has_records == 0);

    assert(
        pwrite(
            fd,
            &corrupt_tail,
            sizeof(corrupt_tail),
            (off_t)(proof_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE)
        ) == sizeof(corrupt_tail)
    );
    has_records = -1;
    assert(
        mylite_ownerless_page_log_has_readable_page_records_at(fd, 0U, &has_records) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(has_records == 0);
    assert(
        ftruncate(
            fd,
            (off_t)(proof_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE)
        ) == 0
    );

    assert(
        pwrite(
            fd,
            corrupt_header,
            sizeof(corrupt_header),
            (off_t)(proof_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE)
        ) == (ssize_t)sizeof(corrupt_header)
    );
    has_records = -1;
    assert(
        mylite_ownerless_page_log_has_readable_page_records_at(fd, 0U, &has_records) ==
        MYLITE_OWNERLESS_PAGE_LOG_ERROR
    );
    assert(has_records == 0);
    assert(
        ftruncate(
            fd,
            (off_t)(proof_record_offset + MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE)
        ) == 0
    );

    assert(
        mylite_ownerless_page_log_append(
            fd,
            92U,
            4U,
            240U,
            240U,
            page,
            sizeof(page),
            &payload_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(payload_record_offset > proof_record_offset);
    has_records = -1;
    assert(
        mylite_ownerless_page_log_has_readable_page_records_at(fd, 0U, &has_records) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(has_records == 1);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_requires_boundaries_only_for_snapshot_pages(void) {
    char *root = make_temp_root();
    char *support_log_path = path_join(root, "support-page-log.bin");
    char *snapshot_log_path = path_join(root, "snapshot-page-log.bin");
    int fd = open_file(support_log_path);
    page_log_checkpoint_index_context checkpoint_context = {0};
    uint8_t page[MYLITE_TEST_PAGE_SIZE];

    fill_innodb_test_page(page, 42U, 0U, 140U, 0x60U);
    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(fd, 42U, 0U, 140U, 140U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
            fd,
            0U,
            140U,
            100U,
            capture_page_log_record_for_checkpoint_index,
            prepare_page_log_checkpoint,
            NULL,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpoint_context.prepare_count == 1U);
    assert(checkpoint_context.retained.count == 0U);
    assert(close(fd) == 0);

    fd = open_file(snapshot_log_path);
    memset(&checkpoint_context, 0, sizeof(checkpoint_context));
    fill_innodb_test_page(page, 43U, 1U, 140U, 0x70U);
    store_test_be16(page, MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET, MYLITE_TEST_INNODB_PAGE_TYPE_RTREE);
    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(fd, 43U, 1U, 140U, 140U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
            fd,
            0U,
            140U,
            100U,
            capture_page_log_record_for_checkpoint_index,
            prepare_page_log_checkpoint,
            NULL,
            &checkpoint_context
        ) == MYLITE_OWNERLESS_PAGE_LOG_BUSY
    );
    assert(checkpoint_context.prepare_count == 0U);
    assert(checkpoint_context.retained.count == 0U);
    assert(close(fd) == 0);

    free(snapshot_log_path);
    free(support_log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_checkpoint_waits_for_readers(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "reader-checkpoint-page-log.bin");
    int fd = open_file(log_path);
    int child_ready[2];
    uint8_t page[16];
    pid_t child;
    struct stat log_stat;

    memset(page, 0x77, sizeof(page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(fd, 42U, 7U, 90U, 100U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(mylite_ownerless_page_log_begin_read(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;

        close(child_ready[0]);
        child_fd = open_file(log_path);
        signal_pipe(child_ready[1]);
        assert(
            mylite_ownerless_page_log_checkpoint(child_fd, 100U, NULL, NULL) ==
            MYLITE_OWNERLESS_PAGE_LOG_OK
        );
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    sleep_milliseconds(100U);
    {
        int child_status = 0;
        assert(waitpid(child, &child_status, WNOHANG) == 0);
    }

    mylite_ownerless_page_log_end_read(fd);
    wait_for_child(child);
    assert(fstat(fd, &log_stat) == 0);
    assert(log_stat.st_size == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_scan_recovers_from_stale_index_offset(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *log_path = path_join(root, "stale-index-page-log.bin");
    int fd = open_file(log_path);
    uint8_t *index = calloc(1U, index_size);
    page_log_retained_records retained = {0};
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t out_page[16];
    uint64_t stale_record_offset = 0;
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;

    assert(index != NULL);
    memset(page_v1, 0x11, sizeof(page_v1));
    memset(page_v2, 0x22, sizeof(page_v2));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            90U,
            100U,
            page_v1,
            sizeof(page_v1),
            &stale_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(fd, 42U, 7U, 110U, 120U, page_v2, sizeof(page_v2), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            100U,
            90U,
            stale_record_offset
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            100U,
            capture_page_log_record_for_index_replace,
            &retained
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(retained.count == 1U);

    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            140U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == stale_record_offset);
    assert(page_lsn == 90U);
    assert(commit_lsn == 100U);

    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v2));
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(memcmp(out_page, page_v2, sizeof(page_v2)) == 0);

    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            140U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v2));
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(memcmp(out_page, page_v2, sizeof(page_v2)) == 0);

    assert(close(fd) == 0);
    free(index);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_rejects_stale_index_offset_identity(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *log_path = path_join(root, "stale-index-identity-page-log.bin");
    int fd = open_file(log_path);
    uint8_t *index = calloc(1U, index_size);
    page_log_retained_records retained = {0};
    uint8_t old_target_page[16];
    uint8_t target_page[16];
    uint8_t other_page[16];
    uint8_t out_page[16];
    uint64_t stale_record_offset = 0;
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;

    assert(index != NULL);
    memset(old_target_page, 0x11, sizeof(old_target_page));
    memset(target_page, 0x22, sizeof(target_page));
    memset(other_page, 0x33, sizeof(other_page));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            90U,
            100U,
            old_target_page,
            sizeof(old_target_page),
            &stale_record_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            43U,
            8U,
            110U,
            120U,
            other_page,
            sizeof(other_page),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(
            fd,
            42U,
            7U,
            110U,
            120U,
            target_page,
            sizeof(target_page),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            120U,
            110U,
            stale_record_offset
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_log_checkpoint(
            fd,
            100U,
            capture_page_log_record_for_index_replace,
            &retained
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(retained.count == 2U);

    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            140U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == stale_record_offset);
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);

    assert(
        mylite_ownerless_page_log_read_page_at(
            fd,
            0U,
            record_offset,
            42U,
            7U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    assert(
        mylite_ownerless_page_log_read_record_at(
            fd,
            0U,
            record_offset,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(other_page));
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(memcmp(out_page, other_page, sizeof(other_page)) == 0);

    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            140U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(target_page));
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(memcmp(out_page, target_page, sizeof(target_page)) == 0);

    assert(close(fd) == 0);
    free(index);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_checkpoints_when_all_records_are_safe(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "safe-checkpoint-page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t out_page[16];
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;
    int checkpointed = -1;
    struct stat log_stat;

    memset(page_v1, 0x44, sizeof(page_v1));
    memset(page_v2, 0x55, sizeof(page_v2));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append(fd, 42U, 7U, 90U, 100U, page_v1, sizeof(page_v1), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append(fd, 42U, 7U, 110U, 120U, page_v2, sizeof(page_v2), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_page_log_checkpoint_if_safe(fd, 110U, &checkpointed) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpointed == 0);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(page_v2));
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(memcmp(out_page, page_v2, sizeof(page_v2)) == 0);

    checkpointed = 0;
    assert(
        mylite_ownerless_page_log_checkpoint_if_safe(fd, 120U, &checkpointed) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(checkpointed == 1);
    assert(fstat(fd, &log_stat) == 0);
    assert(log_stat.st_size == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            42U,
            7U,
            120U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND
    );

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_log_replays_record_offsets(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *log_path = path_join(root, "replay-page-log.bin");
    int fd = open_file(log_path);
    uint8_t *index = calloc(1U, index_size);
    page_log_replay_context context = {.page_index = index, .page_index_size = index_size};
    const uint64_t log_offset = 128U;
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t other_page[16];
    const char torn_tail = 'x';
    uint64_t first_offset = 0;
    uint64_t second_offset = 0;
    uint64_t other_offset = 0;
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;

    assert(index != NULL);
    memset(page_v1, 0x77, sizeof(page_v1));
    memset(page_v2, 0x88, sizeof(page_v2));
    memset(other_page, 0x99, sizeof(other_page));
    truncate_file(fd, (off_t)log_offset);
    assert(mylite_ownerless_page_log_initialize_at(fd, log_offset) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(
        mylite_ownerless_page_log_append_at(
            fd,
            log_offset,
            42U,
            7U,
            90U,
            100U,
            page_v1,
            sizeof(page_v1),
            &first_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_at(
            fd,
            log_offset,
            42U,
            7U,
            110U,
            120U,
            page_v2,
            sizeof(page_v2),
            &second_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_log_append_at(
            fd,
            log_offset,
            42U,
            8U,
            115U,
            125U,
            other_page,
            sizeof(other_page),
            &other_offset
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(first_offset == log_offset + MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    assert(second_offset > first_offset);
    assert(other_offset > second_offset);
    assert(lseek(fd, 0, SEEK_END) > 0);
    assert(write(fd, &torn_tail, sizeof(torn_tail)) == sizeof(torn_tail));

    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_log_replay_at(
            fd,
            log_offset,
            replay_page_log_record_into_index,
            &context
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == second_offset);
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            100U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            8U,
            125U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == other_offset);
    assert(page_lsn == 115U);
    assert(commit_lsn == 125U);

    assert(close(fd) == 0);
    free(index);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_tablespace_replay_applies_visible_page_versions(void) {
    char *root = make_temp_root();
    char *datadir = path_join(root, "datadir");
    char *space_path = path_join(datadir, "table.ibd");
    char *log_path = path_join(root, "page-log.bin");
    int space_fd;
    int log_fd;
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];

    assert(mkdir(datadir, 0700) == 0);
    space_fd = open_file(space_path);
    log_fd = open_file(log_path);
    truncate_file(space_fd, MYLITE_TEST_PAGE_SIZE * 5);

    fill_innodb_test_page(page, 42U, 0U, 10U, 0x10U);
    write_file_at(space_fd, page, sizeof(page), 0);
    fill_innodb_test_page(page, 42U, 3U, 20U, 0x20U);
    write_file_at(space_fd, page, sizeof(page), MYLITE_TEST_PAGE_SIZE * 3);
    fill_innodb_test_page(page, 42U, 4U, 30U, 0x30U);
    write_file_at(space_fd, page, sizeof(page), MYLITE_TEST_PAGE_SIZE * 4);

    assert(mylite_ownerless_page_log_initialize(log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    fill_innodb_test_page(page, 42U, 3U, 100U, 0x40U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 42U, 3U, 100U, 100U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    fill_innodb_test_page(page, 42U, 3U, 120U, 0x50U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 42U, 3U, 120U, 120U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    fill_innodb_test_page(page, 42U, 4U, 160U, 0x60U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 42U, 4U, 160U, 160U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_tablespace_replay_apply(datadir, log_fd, 0U, 120U) ==
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );

    read_file_at(space_fd, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE * 3);
    assert(innodb_test_page_lsn(out_page) == 120U);
    assert(out_page[128] == 0x50U);
    read_file_at(space_fd, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE * 4);
    assert(innodb_test_page_lsn(out_page) == 30U);
    assert(out_page[128] == 0x30U);

    assert(close(log_fd) == 0);
    assert(close(space_fd) == 0);
    free(log_path);
    free(space_path);
    free(datadir);
    remove_tree(root);
    free(root);
}

static void test_tablespace_replay_uses_latest_visible_page_lsn(void) {
    char *root = make_temp_root();
    char *datadir = path_join(root, "datadir");
    char *space_path = path_join(datadir, "commit-order-table.ibd");
    char *log_path = path_join(root, "page-log.bin");
    int space_fd;
    int log_fd;
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];

    assert(mkdir(datadir, 0700) == 0);
    space_fd = open_file(space_path);
    log_fd = open_file(log_path);
    truncate_file(space_fd, MYLITE_TEST_PAGE_SIZE * 2);

    fill_innodb_test_page(page, 46U, 0U, 10U, 0x10U);
    write_file_at(space_fd, page, sizeof(page), 0);
    fill_innodb_test_page(page, 46U, 1U, 20U, 0x20U);
    write_file_at(space_fd, page, sizeof(page), MYLITE_TEST_PAGE_SIZE);

    assert(mylite_ownerless_page_log_initialize(log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    fill_innodb_test_page(page, 46U, 1U, 300U, 0x30U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 46U, 1U, 300U, 100U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    fill_innodb_test_page(page, 46U, 1U, 250U, 0x40U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 46U, 1U, 250U, 120U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_tablespace_replay_apply(datadir, log_fd, 0U, 100U) ==
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );
    read_file_at(space_fd, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE);
    assert(innodb_test_page_lsn(out_page) == 300U);
    assert(out_page[128] == 0x30U);

    assert(
        mylite_ownerless_tablespace_replay_apply(datadir, log_fd, 0U, 120U) ==
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );
    read_file_at(space_fd, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE);
    assert(innodb_test_page_lsn(out_page) == 300U);
    assert(out_page[128] == 0x30U);

    assert(close(log_fd) == 0);
    assert(close(space_fd) == 0);
    free(log_path);
    free(space_path);
    free(datadir);
    remove_tree(root);
    free(root);
}

static void test_tablespace_replay_rewinds_newer_disk_page(void) {
    char *root = make_temp_root();
    char *datadir = path_join(root, "datadir");
    char *space_path = path_join(datadir, "table.ibd");
    char *log_path = path_join(root, "page-log.bin");
    int space_fd;
    int log_fd;
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];

    assert(mkdir(datadir, 0700) == 0);
    space_fd = open_file(space_path);
    log_fd = open_file(log_path);
    truncate_file(space_fd, MYLITE_TEST_PAGE_SIZE * 3);

    fill_innodb_test_page(page, 43U, 0U, 10U, 0x10U);
    write_file_at(space_fd, page, sizeof(page), 0);
    fill_innodb_test_page(page, 43U, 2U, 300U, 0x70U);
    write_file_at(space_fd, page, sizeof(page), MYLITE_TEST_PAGE_SIZE * 2);

    assert(mylite_ownerless_page_log_initialize(log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    fill_innodb_test_page(page, 43U, 2U, 200U, 0x80U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 43U, 2U, 200U, 200U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_tablespace_replay_apply(datadir, log_fd, 0U, 200U) ==
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );

    read_file_at(space_fd, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE * 2);
    assert(innodb_test_page_lsn(out_page) == 200U);
    assert(out_page[128] == 0x80U);

    assert(close(log_fd) == 0);
    assert(close(space_fd) == 0);
    free(log_path);
    free(space_path);
    free(datadir);
    remove_tree(root);
    free(root);
}

static void test_tablespace_replay_rewrites_same_lsn_different_image(void) {
    char *root = make_temp_root();
    char *datadir = path_join(root, "datadir");
    char *space_path = path_join(datadir, "same-lsn-table.ibd");
    char *log_path = path_join(root, "page-log.bin");
    int space_fd;
    int log_fd;
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];

    assert(mkdir(datadir, 0700) == 0);
    space_fd = open_file(space_path);
    log_fd = open_file(log_path);
    truncate_file(space_fd, MYLITE_TEST_PAGE_SIZE * 2);

    fill_innodb_test_page(page, 47U, 0U, 10U, 0x10U);
    write_file_at(space_fd, page, sizeof(page), 0);
    fill_innodb_test_page(page, 47U, 1U, 200U, 0x20U);
    write_file_at(space_fd, page, sizeof(page), MYLITE_TEST_PAGE_SIZE);

    assert(mylite_ownerless_page_log_initialize(log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    fill_innodb_test_page(page, 47U, 1U, 200U, 0x90U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 47U, 1U, 200U, 220U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_tablespace_replay_apply(datadir, log_fd, 0U, 220U) ==
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );

    read_file_at(space_fd, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE);
    assert(innodb_test_page_lsn(out_page) == 200U);
    assert(out_page[128] == 0x90U);

    assert(close(log_fd) == 0);
    assert(close(space_fd) == 0);
    free(log_path);
    free(space_path);
    free(datadir);
    remove_tree(root);
    free(root);
}

static void test_tablespace_replay_can_keep_native_same_lsn_page(void) {
    char *root = make_temp_root();
    char *datadir = path_join(root, "datadir");
    char *space_path = path_join(datadir, "same-lsn-native-table.ibd");
    char *log_path = path_join(root, "page-log.bin");
    int space_fd;
    int log_fd;
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];

    assert(mkdir(datadir, 0700) == 0);
    space_fd = open_file(space_path);
    log_fd = open_file(log_path);
    truncate_file(space_fd, MYLITE_TEST_PAGE_SIZE * 2);

    fill_innodb_test_page(page, 48U, 0U, 10U, 0x10U);
    write_file_at(space_fd, page, sizeof(page), 0);
    fill_innodb_test_page(page, 48U, 1U, 240U, 0x20U);
    write_file_at(space_fd, page, sizeof(page), MYLITE_TEST_PAGE_SIZE);

    assert(mylite_ownerless_page_log_initialize(log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    fill_innodb_test_page(page, 48U, 1U, 240U, 0x90U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 48U, 1U, 240U, 260U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_tablespace_replay_apply_with_flags(
            datadir,
            log_fd,
            0U,
            260U,
            MYLITE_OWNERLESS_TABLESPACE_REPLAY_KEEP_NATIVE_SAME_LSN
        ) == MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );

    read_file_at(space_fd, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE);
    assert(innodb_test_page_lsn(out_page) == 240U);
    assert(out_page[128] == 0x20U);

    assert(close(log_fd) == 0);
    assert(close(space_fd) == 0);
    free(log_path);
    free(space_path);
    free(datadir);
    remove_tree(root);
    free(root);
}

static void test_tablespace_replay_ignores_non_fsp_page_zero_candidates(void) {
    char *root = make_temp_root();
    char *datadir = path_join(root, "datadir");
    char *fake_path = path_join(datadir, "ib_logfile0");
    char *space_path = path_join(datadir, "ibdata1");
    char *log_path = path_join(root, "page-log.bin");
    int fake_fd;
    int space_fd;
    int log_fd;
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];

    assert(mkdir(datadir, 0700) == 0);
    fake_fd = open_file(fake_path);
    space_fd = open_file(space_path);
    log_fd = open_file(log_path);
    truncate_file(fake_fd, MYLITE_TEST_PAGE_SIZE);
    truncate_file(space_fd, MYLITE_TEST_PAGE_SIZE * 2);

    fill_innodb_test_page(page, 45U, 0U, 10U, 0x10U);
    store_test_be16(page, MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET, 0U);
    write_file_at(fake_fd, page, sizeof(page), 0);
    fill_innodb_test_page(page, 45U, 0U, 20U, 0x20U);
    write_file_at(space_fd, page, sizeof(page), 0);
    fill_innodb_test_page(page, 45U, 1U, 30U, 0x30U);
    write_file_at(space_fd, page, sizeof(page), MYLITE_TEST_PAGE_SIZE);

    assert(mylite_ownerless_page_log_initialize(log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    fill_innodb_test_page(page, 45U, 1U, 200U, 0x40U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 45U, 1U, 200U, 200U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_tablespace_replay_apply(datadir, log_fd, 0U, 200U) ==
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );

    read_file_at(space_fd, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE);
    assert(innodb_test_page_lsn(out_page) == 200U);
    assert(out_page[128] == 0x40U);
    read_file_at(fake_fd, out_page, sizeof(out_page), 0);
    assert(innodb_test_page_lsn(out_page) == 10U);
    assert(out_page[128] == 0x10U);

    assert(close(log_fd) == 0);
    assert(close(space_fd) == 0);
    assert(close(fake_fd) == 0);
    free(log_path);
    free(space_path);
    free(fake_path);
    free(datadir);
    remove_tree(root);
    free(root);
}

static void test_tablespace_replay_rejects_ambiguous_tablespace(void) {
    char *root = make_temp_root();
    char *datadir = path_join(root, "datadir");
    char *schema_a = path_join(datadir, "schema_a");
    char *schema_b = path_join(datadir, "schema_b");
    char *space_path_a = path_join(schema_a, "ambiguous.ibd");
    char *space_path_b = path_join(schema_b, "ambiguous-copy.ibd");
    char *log_path = path_join(root, "ambiguous-page-log.bin");
    int space_fd_a;
    int space_fd_b;
    int log_fd;
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;

    assert(mkdir(datadir, 0700) == 0);
    assert(mkdir(schema_a, 0700) == 0);
    assert(mkdir(schema_b, 0700) == 0);
    space_fd_a = open_file(space_path_a);
    space_fd_b = open_file(space_path_b);
    log_fd = open_file(log_path);
    truncate_file(space_fd_a, MYLITE_TEST_PAGE_SIZE * 2);
    truncate_file(space_fd_b, MYLITE_TEST_PAGE_SIZE * 2);

    fill_innodb_test_page(page, 49U, 0U, 10U, 0x10U);
    write_file_at(space_fd_a, page, sizeof(page), 0);
    fill_innodb_test_page(page, 49U, 1U, 20U, 0x20U);
    write_file_at(space_fd_a, page, sizeof(page), MYLITE_TEST_PAGE_SIZE);

    fill_innodb_test_page(page, 49U, 0U, 11U, 0x11U);
    write_file_at(space_fd_b, page, sizeof(page), 0);
    fill_innodb_test_page(page, 49U, 1U, 30U, 0x30U);
    write_file_at(space_fd_b, page, sizeof(page), MYLITE_TEST_PAGE_SIZE);

    assert(mylite_ownerless_page_log_initialize(log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    fill_innodb_test_page(page, 49U, 1U, 200U, 0x90U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 49U, 1U, 200U, 200U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_tablespace_replay_apply(datadir, log_fd, 0U, 200U) ==
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR
    );
    assert(
        mylite_ownerless_tablespace_read_page_at_or_before(
            datadir,
            49U,
            1U,
            MYLITE_TEST_PAGE_SIZE,
            100U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn
        ) == MYLITE_OWNERLESS_TABLESPACE_REPLAY_NOT_FOUND
    );
    assert(
        mylite_ownerless_tablespace_replay_apply_with_flags(
            datadir,
            log_fd,
            0U,
            200U,
            MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES
        ) == MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );

    read_file_at(space_fd_a, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE);
    assert(innodb_test_page_lsn(out_page) == 20U);
    assert(out_page[128] == 0x20U);
    read_file_at(space_fd_b, out_page, sizeof(out_page), MYLITE_TEST_PAGE_SIZE);
    assert(innodb_test_page_lsn(out_page) == 30U);
    assert(out_page[128] == 0x30U);

    assert(close(log_fd) == 0);
    assert(close(space_fd_b) == 0);
    assert(close(space_fd_a) == 0);
    free(log_path);
    free(space_path_b);
    free(space_path_a);
    free(schema_b);
    free(schema_a);
    free(datadir);
    remove_tree(root);
    free(root);
}

static void test_tablespace_replay_rejects_missing_tablespace(void) {
    char *root = make_temp_root();
    char *datadir = path_join(root, "datadir");
    char *log_path = path_join(root, "page-log.bin");
    int log_fd;
    uint8_t page[MYLITE_TEST_PAGE_SIZE];

    assert(mkdir(datadir, 0700) == 0);
    log_fd = open_file(log_path);
    assert(mylite_ownerless_page_log_initialize(log_fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    fill_innodb_test_page(page, 44U, 0U, 100U, 0x90U);
    assert(
        mylite_ownerless_page_log_append(log_fd, 44U, 0U, 100U, 100U, page, sizeof(page), NULL) ==
        MYLITE_OWNERLESS_PAGE_LOG_OK
    );

    assert(
        mylite_ownerless_tablespace_replay_apply(datadir, log_fd, 0U, 100U) ==
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR
    );
    assert(
        mylite_ownerless_tablespace_replay_apply_with_flags(
            datadir,
            log_fd,
            0U,
            100U,
            MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES
        ) == MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );
    assert(
        mylite_ownerless_tablespace_replay_apply_with_flags(datadir, log_fd, 0U, 100U, 0x8000U) ==
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR
    );

    assert(close(log_fd) == 0);
    free(log_path);
    free(datadir);
    remove_tree(root);
    free(root);
}

static void test_tablespace_read_finds_native_snapshot_boundary(void) {
    char *root = make_temp_root();
    char *datadir = path_join(root, "datadir");
    char *space_path = path_join(datadir, "boundary-table.ibd");
    int space_fd;
    uint8_t page[MYLITE_TEST_PAGE_SIZE];
    uint8_t out_page[MYLITE_TEST_PAGE_SIZE];
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;

    assert(mkdir(datadir, 0700) == 0);
    space_fd = open_file(space_path);
    truncate_file(space_fd, MYLITE_TEST_PAGE_SIZE * 2);

    fill_innodb_test_page(page, 48U, 0U, 10U, 0x10U);
    write_file_at(space_fd, page, sizeof(page), 0);
    fill_innodb_test_page(page, 48U, 1U, 90U, 0x20U);
    write_file_at(space_fd, page, sizeof(page), MYLITE_TEST_PAGE_SIZE);

    memset(out_page, 0, sizeof(out_page));
    assert(
        mylite_ownerless_tablespace_read_page_at_or_before(
            datadir,
            48U,
            1U,
            MYLITE_TEST_PAGE_SIZE,
            100U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn
        ) == MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
    );
    assert(out_page_size == MYLITE_TEST_PAGE_SIZE);
    assert(out_page_lsn == 90U);
    assert(out_page[128] == 0x20U);

    assert(
        mylite_ownerless_tablespace_read_page_at_or_before(
            datadir,
            48U,
            1U,
            MYLITE_TEST_PAGE_SIZE,
            80U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn
        ) == MYLITE_OWNERLESS_TABLESPACE_REPLAY_NOT_FOUND
    );
    assert(
        mylite_ownerless_tablespace_read_page_at_or_before(
            datadir,
            49U,
            1U,
            MYLITE_TEST_PAGE_SIZE,
            100U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn
        ) == MYLITE_OWNERLESS_TABLESPACE_REPLAY_NOT_FOUND
    );

    assert(close(space_fd) == 0);
    free(space_path);
    free(datadir);
    remove_tree(root);
    free(root);
}

static int replay_page_log_record_into_index(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    uint64_t record_offset,
    void *context
) {
    page_log_replay_context *replay = context;
    const int result = mylite_ownerless_page_index_publish(
        replay->page_index,
        replay->page_index_size,
        1U,
        10U,
        space_id,
        page_no,
        commit_lsn,
        page_lsn,
        record_offset
    );

    return result == MYLITE_OWNERLESS_PAGE_INDEX_OK ? MYLITE_OWNERLESS_PAGE_LOG_OK
                                                    : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

static int capture_page_log_record_for_index_replace(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    uint64_t record_offset,
    void *context
) {
    page_log_retained_records *retained = context;

    if (retained == NULL ||
        retained->count >= sizeof(retained->records) / sizeof(retained->records[0])) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    retained->records[retained->count] = (mylite_ownerless_page_index_record){
        .space_id = space_id,
        .page_no = page_no,
        .commit_lsn = commit_lsn,
        .page_lsn = page_lsn,
        .record_offset = record_offset,
    };
    retained->count++;
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

static int capture_page_log_record_for_checkpoint_index(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    uint64_t record_offset,
    void *context
) {
    page_log_checkpoint_index_context *checkpoint = context;

    if (checkpoint == NULL) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return capture_page_log_record_for_index_replace(
        space_id,
        page_no,
        page_lsn,
        commit_lsn,
        record_offset,
        &checkpoint->retained
    );
}

static int prepare_page_log_checkpoint(void *context) {
    page_log_checkpoint_index_context *checkpoint = context;

    if (checkpoint == NULL) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    checkpoint->prepare_count++;
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

static int replace_page_index_after_page_log_checkpoint(void *context) {
    page_log_checkpoint_index_context *checkpoint = context;
    const mylite_ownerless_page_index_record *records;
    int result;

    if (checkpoint == NULL) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    records = checkpoint->retained.count == 0U ? NULL : checkpoint->retained.records;
    result = mylite_ownerless_page_index_replace(
        checkpoint->page_index,
        checkpoint->page_index_size,
        1U,
        10U,
        records,
        checkpoint->retained.count
    );
    return result == MYLITE_OWNERLESS_PAGE_INDEX_OK ? MYLITE_OWNERLESS_PAGE_LOG_OK
                                                    : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

static void test_page_log_serializes_cross_process_appends(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "cross-process-page-log.bin");
    int parent_to_child[2];
    int child_to_parent[2];
    int fd = open_file(log_path);
    uint8_t parent_page[16];
    uint8_t child_page[16];
    uint8_t out_page[16];
    uint32_t out_page_size = 0;
    uint64_t out_page_lsn = 0;
    uint64_t out_commit_lsn = 0;
    pid_t child;

    memset(parent_page, 0x44, sizeof(parent_page));
    memset(child_page, 0x55, sizeof(child_page));
    memset(out_page, 0, sizeof(out_page));

    assert(mylite_ownerless_page_log_initialize(fd) == MYLITE_OWNERLESS_PAGE_LOG_OK);
    assert(pipe(parent_to_child) == 0);
    assert(pipe(child_to_parent) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;

        close(parent_to_child[1]);
        close(child_to_parent[0]);
        wait_for_pipe(parent_to_child[0]);
        child_fd = open_file(log_path);
        assert(
            mylite_ownerless_page_log_append(
                child_fd,
                0U,
                3U,
                220U,
                220U,
                child_page,
                sizeof(child_page),
                NULL
            ) == MYLITE_OWNERLESS_PAGE_LOG_OK
        );
        assert(close(child_fd) == 0);
        signal_pipe(child_to_parent[1]);
        _exit(0);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);
    assert(
        mylite_ownerless_page_log_append(
            fd,
            0U,
            3U,
            100U,
            100U,
            parent_page,
            sizeof(parent_page),
            NULL
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    signal_pipe(parent_to_child[1]);
    wait_for_pipe(child_to_parent[0]);
    wait_for_child(child);

    assert(
        mylite_ownerless_page_log_find_latest(
            fd,
            0U,
            3U,
            500U,
            out_page,
            sizeof(out_page),
            &out_page_size,
            &out_page_lsn,
            &out_commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(out_page_size == sizeof(child_page));
    assert(out_page_lsn == 220U);
    assert(out_commit_lsn == 220U);
    assert(memcmp(out_page, child_page, sizeof(child_page)) == 0);

    assert(close(fd) == 0);
    free(log_path);
    remove_tree(root);
    free(root);
}

static void test_page_index_publishes_latest_record_offsets(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    uint8_t *index = calloc(1U, index_size);
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint64_t index_generation = 0;
    uint64_t current_generation = 0;
    uint64_t previous_generation = 0;

    assert(index != NULL);
    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find_with_generation(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            100U,
            &record_offset,
            &page_lsn,
            &commit_lsn,
            &index_generation
        ) == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND
    );
    assert(index_generation != 0U);
    assert(
        mylite_ownerless_page_index_generation(index, index_size, &current_generation) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(current_generation == index_generation);
    previous_generation = index_generation;
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            100U,
            90U,
            4096U
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find_with_generation(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            100U,
            &record_offset,
            &page_lsn,
            &commit_lsn,
            &index_generation
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(index_generation > previous_generation);
    previous_generation = index_generation;
    assert(record_offset == 4096U);
    assert(page_lsn == 90U);
    assert(commit_lsn == 100U);
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            120U,
            110U,
            8192U
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == 8192U);
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            140U,
            100U,
            12288U
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            140U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == 12288U);
    assert(page_lsn == 100U);
    assert(commit_lsn == 140U);
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            120U,
            110U,
            16384U
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            100U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            90U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED
    );
    assert(
        mylite_ownerless_page_index_require_wal_scan(index, index_size, 1U, 10U) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find_with_generation(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn,
            &index_generation
        ) == MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED
    );
    assert(index_generation > previous_generation);
    previous_generation = index_generation;
    assert(
        mylite_ownerless_page_index_clear(index, index_size, 1U, 10U) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find_with_generation(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn,
            &index_generation
        ) == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND
    );
    assert(index_generation > previous_generation);
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            140U,
            130U,
            12288U
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            140U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == 12288U);
    assert(page_lsn == 130U);
    assert(commit_lsn == 140U);

    free(index);
}

static void test_page_index_replace_restores_index_after_wal_scan(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    uint8_t *index = calloc(1U, index_size);
    mylite_ownerless_page_index_record records[2];
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;

    assert(index != NULL);
    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            100U,
            90U,
            4096U
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_require_wal_scan(index, index_size, 1U, 10U) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED
    );

    records[0] = (mylite_ownerless_page_index_record){
        .space_id = 42U,
        .page_no = 7U,
        .commit_lsn = 120U,
        .page_lsn = 110U,
        .record_offset = 8192U,
    };
    records[1] = (mylite_ownerless_page_index_record){
        .space_id = 43U,
        .page_no = 8U,
        .commit_lsn = 140U,
        .page_lsn = 130U,
        .record_offset = 12288U,
    };
    assert(
        mylite_ownerless_page_index_replace(index, index_size, 1U, 10U, records, 2U) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );

    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == 8192U);
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            100U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            43U,
            8U,
            140U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == 12288U);
    assert(page_lsn == 130U);
    assert(commit_lsn == 140U);

    assert(
        mylite_ownerless_page_index_replace(index, index_size, 1U, 10U, NULL, 0U) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            43U,
            8U,
            140U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND
    );

    free(index);
}

static void test_page_index_overflow_requires_wal_scan(void) {
    enum { entry_count = 1U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    uint8_t *index = calloc(1U, index_size);
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;

    assert(index != NULL);
    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            100U,
            90U,
            4096U
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            42U,
            7U,
            120U,
            110U,
            8192U
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == 8192U);
    assert(page_lsn == 110U);
    assert(commit_lsn == 120U);
    assert(
        mylite_ownerless_page_index_publish(
            index,
            index_size,
            1U,
            10U,
            43U,
            8U,
            140U,
            130U,
            12288U
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            2U,
            20U,
            42U,
            7U,
            120U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED
    );

    free(index);
}

static void test_page_index_publishes_across_processes(void) {
    enum { entry_count = 8U };

    const size_t index_size = MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
                              (entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    char *root = make_temp_root();
    char *shm_path = path_join(root, "page-index.bin");
    int parent_to_child[2];
    int child_to_parent[2];
    int fd = open_file(shm_path);
    uint8_t *index;
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    pid_t child;

    truncate_file(fd, (off_t)index_size);
    index = map_file(fd, index_size);
    assert(
        mylite_ownerless_page_index_initialize(index, index_size, entry_count) ==
        MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(pipe(parent_to_child) == 0);
    assert(pipe(child_to_parent) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        uint8_t *child_index;

        close(parent_to_child[1]);
        close(child_to_parent[0]);
        wait_for_pipe(parent_to_child[0]);
        child_fd = open_file(shm_path);
        child_index = map_file(child_fd, index_size);
        assert(
            mylite_ownerless_page_index_publish(
                child_index,
                index_size,
                2U,
                20U,
                43U,
                8U,
                130U,
                125U,
                16384U
            ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
        );
        assert(munmap(child_index, index_size) == 0);
        assert(close(child_fd) == 0);
        signal_pipe(child_to_parent[1]);
        _exit(0);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);
    signal_pipe(parent_to_child[1]);
    wait_for_pipe(child_to_parent[0]);
    assert(
        mylite_ownerless_page_index_find(
            index,
            index_size,
            1U,
            10U,
            43U,
            8U,
            130U,
            &record_offset,
            &page_lsn,
            &commit_lsn
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == 16384U);
    assert(page_lsn == 125U);
    assert(commit_lsn == 130U);
    wait_for_child(child);

    assert(munmap(index, index_size) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_allows_cross_process_shared_holders(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-shared.bin");
    int fd = open_file(shm_path);
    void *table;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd = open_file(shm_path);
        void *child_table = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);

        assert(
            mylite_ownerless_lock_table_acquire_shared(
                child_table,
                MYLITE_TEST_PAGE_SIZE,
                MYLITE_TEST_LOCK_HASH,
                2U,
                0U
            ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
        assert(
            mylite_ownerless_lock_table_release_shared(
                child_table,
                MYLITE_TEST_PAGE_SIZE,
                MYLITE_TEST_LOCK_HASH,
                2U
            ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
        assert(munmap(child_table, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    wait_for_child(child);
    assert(
        mylite_ownerless_lock_table_release_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_upgradable_is_compatible_with_shared_holders(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-upgradable.bin");
    int fd = open_file(shm_path);
    void *table;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_upgradable(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_upgradable(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            3U,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_lock_table_release_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_upgradable(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_nonblocking_acquire_waits_for_latch(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-latch-contention.bin");
    int child_ready[2];
    int fd = open_file(shm_path);
    void *table;
    mylite_ownerless_latch *latch;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    latch =
        (mylite_ownerless_latch *)((unsigned char *)table + MYLITE_TEST_LOCK_TABLE_LATCH_OFFSET);
    assert(
        mylite_ownerless_latch_acquire(latch, 7U, 700U, NULL, NULL, 0U) == MYLITE_OWNERLESS_LATCH_OK
    );
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        void *child_table;

        close(child_ready[0]);
        child_fd = open_file(shm_path);
        child_table = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        signal_pipe(child_ready[1]);
        assert(
            mylite_ownerless_lock_table_acquire_shared(
                child_table,
                MYLITE_TEST_PAGE_SIZE,
                MYLITE_TEST_LOCK_HASH,
                2U,
                0U
            ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
        assert(
            mylite_ownerless_lock_table_release_shared(
                child_table,
                MYLITE_TEST_PAGE_SIZE,
                MYLITE_TEST_LOCK_HASH,
                2U
            ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
        assert(munmap(child_table, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    sleep_milliseconds(50U);
    assert(mylite_ownerless_latch_release(latch, 7U, 700U) == MYLITE_OWNERLESS_LATCH_OK);
    close(child_ready[0]);
    wait_for_child(child);

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_metadata_modes_follow_mariadb_matrix(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-mdl-matrix.bin");
    int fd = open_file(shm_path);
    void *table;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            3U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_lock_table_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            3U,
            MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_lock_table_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            3U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_lock_table_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            3U,
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_lock_table_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_waits_for_conflicting_owner_release(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-release.bin");
    int child_ready[2];
    int fd = open_file(shm_path);
    void *table;
    pid_t child;
    const size_t table_size = mylite_ownerless_lock_table_size(MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT);

    assert(table_size > 0U);
    assert(table_size <= MYLITE_TEST_PAGE_SIZE);
    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        void *child_table;
        int acquire_result;

        close(child_ready[0]);
        child_fd = open_file(shm_path);
        child_table = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        signal_pipe(child_ready[1]);
        acquire_result = mylite_ownerless_lock_table_acquire_exclusive(
            child_table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_TEST_WAIT_TIMEOUT_MS
        );
        assert(acquire_result == MYLITE_OWNERLESS_LOCK_TABLE_OK);
        assert(
            mylite_ownerless_lock_table_release_exclusive(
                child_table,
                MYLITE_TEST_PAGE_SIZE,
                MYLITE_TEST_LOCK_HASH,
                2U
            ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
        assert(munmap(child_table, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    sleep_milliseconds(50U);
    assert(
        mylite_ownerless_lock_table_release_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    wait_for_child(child);

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_conflicting_owner_times_out(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-timeout.bin");
    int fd = open_file(shm_path);
    void *table;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd = open_file(shm_path);
        void *child_table = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        int acquire_result = mylite_ownerless_lock_table_acquire_exclusive(
            child_table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            20U
        );

        assert(acquire_result == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT);
        assert(munmap(child_table, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    wait_for_child(child);
    assert(
        mylite_ownerless_lock_table_release_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_exclusive_waits_for_shared_release(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-shared-release.bin");
    int child_ready[2];
    int fd = open_file(shm_path);
    void *table;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        void *child_table;
        int acquire_result;

        close(child_ready[0]);
        child_fd = open_file(shm_path);
        child_table = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        signal_pipe(child_ready[1]);
        acquire_result = mylite_ownerless_lock_table_acquire_exclusive(
            child_table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            MYLITE_TEST_WAIT_TIMEOUT_MS
        );
        assert(acquire_result == MYLITE_OWNERLESS_LOCK_TABLE_OK);
        assert(
            mylite_ownerless_lock_table_release_exclusive(
                child_table,
                MYLITE_TEST_PAGE_SIZE,
                MYLITE_TEST_LOCK_HASH,
                2U
            ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
        assert(munmap(child_table, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    sleep_milliseconds(50U);
    assert(
        mylite_ownerless_lock_table_release_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    wait_for_child(child);

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_counts_repeated_owner_acquisitions(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-refcount.bin");
    int fd = open_file(shm_path);
    void *table;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_lock_table_release_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_allows_same_owner_mode_upgrade(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-owner-upgrade.bin");
    int fd = open_file(shm_path);
    void *table;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_lock_table_release_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_lock_table_releases_all_owner_locks(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "lock-table-owner-release.bin");
    int fd = open_file(shm_path);
    void *table;
    uint32_t released_entries = 0U;
    uint32_t active_count = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH + 1U,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_owner_active_count(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(active_count == 2U);
    assert(
        mylite_ownerless_lock_table_owner_active_count(
            table,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &active_count
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(active_count == 1U);
    assert(
        mylite_ownerless_lock_table_release_owner(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &released_entries
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(released_entries == 2U);
    assert(
        mylite_ownerless_lock_table_owner_active_count(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(active_count == 0U);
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            3U,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH + 1U,
            3U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_release_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            3U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_table_compatibility(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-table-compat.bin");
    int fd = open_file(shm_path);
    void *registry;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_reserve_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            399U,
            39U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_reserve_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            399U,
            39U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            399U,
            39U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            100U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IS,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_reserve_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            8U,
            18U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            100U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            20U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            101U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            102U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_S,
            20U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            4U,
            103U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_AUTO_INC,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            101U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            102U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_S,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            4U,
            103U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_AUTO_INC
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            102U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_S,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            5U,
            104U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            20U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_record_compatibility(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-record-compat.bin");
    int fd = open_file(shm_path);
    void *registry;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            200U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_S,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_reserve_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            8U,
            208U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            200U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            20U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            201U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_S,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            7U,
            206U,
            20U,
            3U,
            7U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            202U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            20U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            4U,
            203U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            5U,
            204U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP |
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION,
            20U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            200U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_S,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            7U,
            206U,
            20U,
            3U,
            7U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            201U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_S,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            4U,
            203U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            5U,
            204U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP |
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            6U,
            205U,
            20U,
            3U,
            7U,
            9U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_nonblocking_reserve_waits_for_latch(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-latch-contention.bin");
    int child_ready[2];
    int fd = open_file(shm_path);
    void *registry;
    mylite_ownerless_latch *latch;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    latch = (mylite_ownerless_latch *)((unsigned char *)registry +
                                       MYLITE_TEST_INNODB_LOCK_REGISTRY_LATCH_OFFSET);
    assert(
        mylite_ownerless_latch_acquire(latch, 7U, 700U, NULL, NULL, 0U) == MYLITE_OWNERLESS_LATCH_OK
    );
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        void *child_registry;

        close(child_ready[0]);
        child_fd = open_file(shm_path);
        child_registry = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        signal_pipe(child_ready[1]);
        assert(
            mylite_ownerless_innodb_lock_registry_reserve_record(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                201U,
                20U,
                3U,
                7U,
                9U,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
                0U
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
        assert(
            mylite_ownerless_innodb_lock_registry_release_record(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                201U,
                20U,
                3U,
                7U,
                9U,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
        assert(munmap(child_registry, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    sleep_milliseconds(50U);
    assert(mylite_ownerless_latch_release(latch, 7U, 700U) == MYLITE_OWNERLESS_LATCH_OK);
    close(child_ready[0]);
    wait_for_child(child);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_wait_edges_and_deadlocks(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-wait-graph.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t cleared_waits = 0U;
    uint32_t released_locks = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            500U,
            50U,
            5U,
            10U,
            12U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            503U,
            50U,
            5U,
            10U,
            12U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            1U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            501U,
            50U,
            5U,
            12U,
            13U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            501U,
            50U,
            5U,
            11U,
            13U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 3U);
    assert(
        mylite_ownerless_innodb_lock_registry_wait_until_record_available(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            502U,
            50U,
            5U,
            10U,
            12U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            1U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);
    assert(
        mylite_ownerless_innodb_lock_registry_wait_for_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            500U,
            50U,
            5U,
            11U,
            13U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            2U,
            501U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_wait_for_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            501U,
            50U,
            5U,
            10U,
            12U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            1U,
            500U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_clear_wait(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            500U,
            &cleared_waits
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(cleared_waits == 1U);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &released_locks
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(released_locks == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &released_locks
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(released_locks == 2U);
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_table_waiter_death_requires_owner_cleanup(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-table-waiter-death.bin");
    int fd = open_file(shm_path);
    void *registry;
    pid_t child;
    uint32_t released_locks = 0U;
    int child_status = 0;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            100U,
            900U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        const int wait_result = mylite_ownerless_innodb_lock_registry_wait_until_table_available(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            200U,
            900U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_S,
            60000U
        );
        (void)wait_result;
        _exit(2);
    }

    for (unsigned iteration = 0U;
         iteration < 500U && mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U;
         ++iteration) {
        sleep_milliseconds(10U);
    }
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);
    assert(kill(child, SIGKILL) == 0);
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFSIGNALED(child_status));
    assert(WTERMSIG(child_status) == SIGKILL);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);

    assert(
        mylite_ownerless_innodb_lock_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &released_locks
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(released_locks == 1U);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 1U);

    released_locks = 0U;
    assert(
        mylite_ownerless_innodb_lock_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &released_locks
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(released_locks == 1U);
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_detects_cross_registry_deadlocks(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-cross-registry-deadlock.bin");
    int fd = open_file(shm_path);
    void *mapping;
    void *row_registry;
    void *page_write_registry;
    uint32_t cleared_waits = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE * 2);
    mapping = map_file(fd, MYLITE_TEST_PAGE_SIZE * 2);
    row_registry = mapping;
    page_write_registry = (unsigned char *)mapping + MYLITE_TEST_PAGE_SIZE;
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            row_registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            page_write_registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            row_registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            100U,
            10U,
            5U,
            7U,
            2U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_wait_for_record(
            page_write_registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            100U,
            UINT64_MAX,
            5U,
            7U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            2U,
            200U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(page_write_registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_wait_until_record_available_with_cycle_registry(
            row_registry,
            MYLITE_TEST_PAGE_SIZE,
            page_write_registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            200U,
            10U,
            5U,
            7U,
            2U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            MYLITE_TEST_WAIT_TIMEOUT_MS
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(row_registry) == 0U);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(page_write_registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_clear_wait(
            page_write_registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            100U,
            &cleared_waits
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(cleared_waits == 1U);

    assert(munmap(mapping, MYLITE_TEST_PAGE_SIZE * 2) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_detects_page_write_gate_deadlocks(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-page-write-gate-deadlock.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t cleared_waits = 0U;
    uint32_t released_locks = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            900U,
            UINT64_MAX,
            5U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            900U,
            UINT64_MAX,
            6U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_wait_for_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            900U,
            UINT64_MAX,
            6U,
            UINT32_MAX - 1U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            2U,
            900U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_wait_until_record_available(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            900U,
            UINT64_MAX,
            5U,
            UINT32_MAX - 1U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            MYLITE_TEST_WAIT_TIMEOUT_MS
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_clear_wait(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            900U,
            &cleared_waits
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(cleared_waits == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &released_locks
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(released_locks == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &released_locks
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(released_locks == 1U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_page_write_gates_cover_physical_pages(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-page-write-gates.bin");
    int fd = open_file(shm_path);
    void *registry;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            UINT64_MAX,
            5U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            UINT64_MAX,
            5U,
            4U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            30U,
            UINT64_MAX,
            5U,
            UINT32_MAX - 1U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            30U,
            UINT64_MAX,
            6U,
            UINT32_MAX - 1U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            30U,
            UINT64_MAX,
            6U,
            UINT32_MAX - 1U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            UINT64_MAX,
            5U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            UINT64_MAX,
            5U,
            4U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            30U,
            UINT64_MAX,
            5U,
            UINT32_MAX - 1U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            4U,
            40U,
            UINT64_MAX,
            5U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            4U,
            40U,
            UINT64_MAX,
            6U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            4U,
            40U,
            UINT64_MAX,
            6U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            30U,
            UINT64_MAX,
            5U,
            UINT32_MAX - 1U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            UINT64_MAX,
            7U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            UINT64_MAX,
            UINT32_MAX - 2U,
            UINT32_MAX,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            UINT64_MAX,
            7U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            UINT64_MAX,
            UINT32_MAX - 2U,
            UINT32_MAX,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            UINT64_MAX,
            8U,
            3U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            UINT64_MAX,
            UINT32_MAX - 2U,
            UINT32_MAX,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_page_write_owner_bypasses_blocked_waiter(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-page-write-owner-bypass.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t cleared_waits = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            UINT64_MAX,
            2U,
            UINT32_MAX,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_wait_for_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            UINT64_MAX,
            2U,
            UINT32_MAX,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            1U,
            10U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            UINT64_MAX,
            2U,
            8U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 2U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            UINT64_MAX,
            2U,
            8U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            UINT64_MAX,
            2U,
            UINT32_MAX,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_clear_wait(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            &cleared_waits
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(cleared_waits == 1U);
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_owner_blocks_waiting_lock(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-owner-blocks-waiter.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t cleared_waits = 0U;
    int blocks_waiting_lock = 0;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_owner_blocks_waiting_lock(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            &blocks_waiting_lock
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(blocks_waiting_lock == 0);
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            1000U,
            5U,
            10U,
            11U,
            12U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_wait_for_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            2000U,
            5U,
            10U,
            11U,
            12U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            1U,
            1000U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_owner_blocks_waiting_lock(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            &blocks_waiting_lock
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(blocks_waiting_lock == 1);
    assert(
        mylite_ownerless_innodb_lock_registry_owner_blocks_waiting_lock(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            &blocks_waiting_lock
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(blocks_waiting_lock == 0);
    assert(
        mylite_ownerless_innodb_lock_registry_clear_wait(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            2000U,
            &cleared_waits
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(cleared_waits == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_wait_for_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            2001U,
            5U,
            10U,
            11U,
            13U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            1U,
            1000U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_owner_blocks_waiting_lock(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            &blocks_waiting_lock
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(blocks_waiting_lock == 0);
    assert(
        mylite_ownerless_innodb_lock_registry_clear_wait(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            2001U,
            &cleared_waits
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(cleared_waits == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            1000U,
            5U,
            10U,
            11U,
            12U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_same_page_waiter_fairness(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-same-page-fairness.bin");
    int fd = open_file(shm_path);
    void *registry;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            700U,
            70U,
            7U,
            21U,
            31U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            703U,
            70U,
            7U,
            21U,
            31U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            704U,
            70U,
            7U,
            21U,
            32U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            703U,
            70U,
            7U,
            21U,
            31U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            704U,
            70U,
            7U,
            21U,
            32U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_wait_for_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            701U,
            70U,
            7U,
            21U,
            32U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            1U,
            700U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            700U,
            70U,
            7U,
            21U,
            31U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            702U,
            70U,
            7U,
            21U,
            33U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            1U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT
    );
    assert(
        mylite_ownerless_innodb_lock_registry_wait_until_record_available(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            701U,
            70U,
            7U,
            21U,
            32U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            701U,
            70U,
            7U,
            21U,
            32U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            701U,
            70U,
            7U,
            21U,
            32U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            702U,
            70U,
            7U,
            21U,
            33U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            702U,
            70U,
            7U,
            21U,
            33U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_wait_until_rechecks_available_after_missed_wake(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-wait-final-recheck.bin");
    int fd = open_file(shm_path);
    int child_ready[2];
    void *registry;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            300U,
            30U,
            4U,
            8U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(child_ready[0]);
        signal_pipe(child_ready[1]);
        assert(
            mylite_ownerless_innodb_lock_registry_wait_until_record_available(
                registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                301U,
                30U,
                4U,
                8U,
                10U,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
                2000U
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
        assert(
            mylite_ownerless_innodb_lock_registry_acquire_record(
                registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                301U,
                30U,
                4U,
                8U,
                10U,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
                0U
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
        assert(
            mylite_ownerless_innodb_lock_registry_release_record(
                registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                301U,
                30U,
                4U,
                8U,
                10U,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    for (unsigned iteration = 0U;
         iteration < 1500U && mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U;
         ++iteration) {
        sleep_milliseconds(1U);
    }
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);
    {
        uint32_t active_count = 0U;
        uint32_t free_state = 0U;
        mylite_ownerless_latch *latch =
            (mylite_ownerless_latch *)((unsigned char *)registry +
                                       MYLITE_TEST_INNODB_LOCK_REGISTRY_LATCH_OFFSET);

        assert(
            mylite_ownerless_latch_acquire(latch, 9U, 900U, NULL, NULL, 5000U) ==
            MYLITE_OWNERLESS_LATCH_OK
        );
        memcpy(
            (unsigned char *)registry + MYLITE_TEST_INNODB_LOCK_REGISTRY_ACTIVE_COUNT_OFFSET,
            &active_count,
            sizeof(active_count)
        );
        memcpy(
            (unsigned char *)registry + MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE +
                MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_STATE_OFFSET,
            &free_state,
            sizeof(free_state)
        );
        assert(mylite_ownerless_latch_release(latch, 9U, 900U) == MYLITE_OWNERLESS_LATCH_OK);
    }

    wait_for_child(child);
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_waits_across_processes(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-record-wait.bin");
    int fd = open_file(shm_path);
    int child_ready[2];
    void *registry;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            300U,
            30U,
            4U,
            8U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        void *child_registry;
        uint32_t acquire_flags = 0U;

        close(child_ready[0]);
        signal_pipe(child_ready[1]);
        child_fd = open_file(shm_path);
        child_registry = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        assert(
            mylite_ownerless_innodb_lock_registry_acquire_record_with_flags(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                MYLITE_TEST_OWNER_GENERATION(2U),
                301U,
                30U,
                4U,
                8U,
                10U,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
                MYLITE_TEST_WAIT_TIMEOUT_MS,
                &acquire_flags
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
        assert((acquire_flags & MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ACQUIRE_WAITED) != 0U);
        assert(mylite_ownerless_innodb_lock_registry_waiting_count(child_registry) == 0U);
        assert(
            mylite_ownerless_innodb_lock_registry_release_record(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                301U,
                30U,
                4U,
                8U,
                10U,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
        assert(munmap(child_registry, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    for (unsigned iteration = 0U;
         iteration < 500U && mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 0U;
         ++iteration) {
        sleep_milliseconds(1U);
    }
    assert(mylite_ownerless_innodb_lock_registry_waiting_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            300U,
            30U,
            4U,
            8U,
            10U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    wait_for_child(child);
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_references_and_owner_cleanup(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-owner-cleanup.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t released_locks = 0U;
    uint32_t released_transaction_records = 0U;
    uint32_t active_count = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            400U,
            40U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            400U,
            40U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_table(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            400U,
            40U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            401U,
            41U,
            4U,
            9U,
            11U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            42U,
            UINT64_MAX,
            4U,
            12U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            42U,
            UINT64_MAX,
            4U,
            13U,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(
        mylite_ownerless_innodb_lock_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(active_count == 4U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_transaction_records(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            42U,
            UINT64_MAX,
            UINT32_MAX,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            &released_transaction_records
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(released_transaction_records == 2U);
    assert(
        mylite_ownerless_innodb_lock_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(active_count == 2U);
    assert(
        mylite_ownerless_innodb_lock_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &released_locks
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(released_locks == 2U);
    assert(
        mylite_ownerless_innodb_lock_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(active_count == 0U);
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_innodb_lock_registry_shrinks_scan_limit(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "innodb-lock-scan-limit.bin");
    int fd = open_file(shm_path);
    void *registry;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_innodb_lock_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(innodb_lock_registry_occupied_limit(registry) == 0U);

    for (uint32_t heap_no = 1U; heap_no <= 6U; ++heap_no) {
        assert(
            mylite_ownerless_innodb_lock_registry_acquire_record(
                registry,
                MYLITE_TEST_PAGE_SIZE,
                1U,
                900U,
                90U,
                9U,
                19U,
                heap_no,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
                0U
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
    }
    assert(innodb_lock_registry_occupied_limit(registry) == 6U);

    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            900U,
            90U,
            9U,
            19U,
            3U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(innodb_lock_registry_occupied_limit(registry) == 6U);

    for (uint32_t heap_no = 6U; heap_no > 0U; --heap_no) {
        if (heap_no == 3U) {
            continue;
        }
        assert(
            mylite_ownerless_innodb_lock_registry_release_record(
                registry,
                MYLITE_TEST_PAGE_SIZE,
                1U,
                900U,
                90U,
                9U,
                19U,
                heap_no,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
    }
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);
    assert(innodb_lock_registry_occupied_limit(registry) == 0U);

    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            901U,
            91U,
            9U,
            19U,
            1U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(innodb_lock_registry_occupied_limit(registry) == 1U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_autoinc_registry_preserves_high_watermarks(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "autoinc-registry.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint64_t next_value = 0U;
    int checkpoint_pending = 1;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_autoinc_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_AUTOINC_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(
        mylite_ownerless_autoinc_registry_checkpoint_pending(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            &checkpoint_pending
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(checkpoint_pending == 0);
    assert(
        mylite_ownerless_autoinc_registry_read_or_seed(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            101U,
            7U,
            &next_value
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(next_value == 7U);
    assert(
        mylite_ownerless_autoinc_registry_publish(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            101U,
            12U
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(
        mylite_ownerless_autoinc_registry_checkpoint_pending(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            &checkpoint_pending
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(checkpoint_pending == 1);
    assert(
        mylite_ownerless_autoinc_registry_clear_checkpoint_pending(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U)
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    checkpoint_pending = 1;
    assert(
        mylite_ownerless_autoinc_registry_checkpoint_pending(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            &checkpoint_pending
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(checkpoint_pending == 0);
    assert(
        mylite_ownerless_autoinc_registry_read_or_seed(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            MYLITE_TEST_OWNER_GENERATION(2U),
            101U,
            9U,
            &next_value
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(next_value == 12U);
    assert(
        mylite_ownerless_autoinc_registry_publish(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            MYLITE_TEST_OWNER_GENERATION(2U),
            101U,
            10U
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(
        mylite_ownerless_autoinc_registry_checkpoint_pending(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            MYLITE_TEST_OWNER_GENERATION(2U),
            &checkpoint_pending
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(checkpoint_pending == 0);
    assert(
        mylite_ownerless_autoinc_registry_read_or_seed(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            101U,
            1U,
            &next_value
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(next_value == 12U);
    assert(
        mylite_ownerless_autoinc_registry_read_or_seed(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            202U,
            3U,
            &next_value
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK
    );
    assert(next_value == 3U);
    assert(
        mylite_ownerless_autoinc_registry_publish(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_TEST_OWNER_GENERATION(1U),
            303U,
            1U
        ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_mdl_key_hashes_are_stable_and_distinct(void) {
    const uint64_t app_posts_hash =
        mylite_ownerless_mdl_key_hash(MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE, "app", "posts");
    const uint64_t app_posts_again_hash =
        mylite_ownerless_mdl_key_hash(MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE, "app", "posts");
    const uint64_t app_comments_hash =
        mylite_ownerless_mdl_key_hash(MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE, "app", "comments");
    const uint64_t app_schema_hash =
        mylite_ownerless_mdl_key_hash(MYLITE_OWNERLESS_MDL_NAMESPACE_SCHEMA, "app", "");

    assert(app_posts_hash != 0U);
    assert(app_posts_hash == app_posts_again_hash);
    assert(app_posts_hash != app_comments_hash);
    assert(app_posts_hash != app_schema_hash);
    assert(mylite_ownerless_mdl_key_hash(99U, "app", "posts") == 0U);
}

static void test_mdl_upgradable_is_compatible_with_shared_holders(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "mdl-upgradable-lock.bin");
    int fd = open_file(shm_path);
    void *table;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_acquire_upgradable(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts",
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts",
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_acquire_upgradable(
            table,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts",
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_mdl_release_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts"
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_release_upgradable(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts"
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_mdl_metadata_modes_follow_mariadb_matrix(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "mdl-matrix-lock.bin");
    int fd = open_file(shm_path);
    void *table;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts",
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts",
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_mdl_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts",
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(
        mylite_ownerless_mdl_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_SCHEMA,
            "app",
            "",
            MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_SCHEMA,
            "app",
            "",
            MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_acquire_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_SCHEMA,
            "app",
            "",
            MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
            20U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT
    );
    assert(
        mylite_ownerless_mdl_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_SCHEMA,
            "app",
            "",
            MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_release_mode(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_SCHEMA,
            "app",
            "",
            MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_mdl_table_lock_waits_across_processes(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "mdl-table-lock.bin");
    int child_ready[2];
    int fd = open_file(shm_path);
    void *table;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    table = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_lock_table_initialize(
            table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_acquire_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts",
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_acquire_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "comments",
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_mdl_release_exclusive(
            table,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "comments"
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        void *child_table;
        int acquire_result;

        close(child_ready[0]);
        child_fd = open_file(shm_path);
        child_table = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        signal_pipe(child_ready[1]);
        acquire_result = mylite_ownerless_mdl_acquire_exclusive(
            child_table,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts",
            MYLITE_TEST_WAIT_TIMEOUT_MS
        );
        assert(acquire_result == MYLITE_OWNERLESS_LOCK_TABLE_OK);
        assert(
            mylite_ownerless_mdl_release_exclusive(
                child_table,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
                "app",
                "posts"
            ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
        );
        assert(munmap(child_table, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    sleep_milliseconds(50U);
    assert(
        mylite_ownerless_mdl_release_shared(
            table,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE,
            "app",
            "posts"
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    wait_for_child(child);

    assert(munmap(table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_trx_registry_allocates_cross_process_ids(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "trx-registry-cross-process.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t parent_slot = 0U;
    uint64_t parent_generation = 0U;
    uint64_t parent_trx_id = 0U;
    pid_t child;
    const size_t registry_size =
        mylite_ownerless_trx_registry_size(MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT);

    assert(registry_size > 0U);
    assert(registry_size <= MYLITE_TEST_PAGE_SIZE);
    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_trx_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            100U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &parent_trx_id,
            &parent_slot,
            &parent_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(parent_trx_id == 100U);
    assert(mylite_ownerless_trx_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_trx_registry_oldest_active_trx_id(registry, MYLITE_TEST_PAGE_SIZE) == 100U
    );
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 101U);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd = open_file(shm_path);
        void *child_registry = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        uint64_t child_trx_id = 0U;
        uint32_t child_slot = 0U;
        uint64_t child_generation = 0U;
        uint64_t trx_ids[MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT] = {0};
        uint32_t trx_id_count = 0U;
        uint64_t next_trx_id = 0U;
        uint64_t oldest_trx_id = 0U;

        assert(
            mylite_ownerless_trx_registry_begin(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                &child_trx_id,
                &child_slot,
                &child_generation
            ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
        );
        assert(child_trx_id == 101U);
        assert(child_slot != parent_slot);
        assert(mylite_ownerless_trx_registry_active_count(child_registry) == 2U);
        assert(
            mylite_ownerless_trx_registry_snapshot(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                trx_ids,
                MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
                &trx_id_count,
                &next_trx_id,
                &oldest_trx_id
            ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
        );
        assert(trx_id_count == 2U);
        assert(trx_ids[0] == 100U);
        assert(trx_ids[1] == 101U);
        assert(next_trx_id == 102U);
        assert(oldest_trx_id == 100U);
        assert(
            mylite_ownerless_trx_registry_end(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                child_slot,
                child_generation
            ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
        );
        assert(munmap(child_registry, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    wait_for_child(child);
    assert(mylite_ownerless_trx_registry_active_count(registry) == 1U);
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 102U);
    assert(
        mylite_ownerless_trx_registry_oldest_active_trx_id(registry, MYLITE_TEST_PAGE_SIZE) == 100U
    );
    assert(
        mylite_ownerless_trx_registry_end(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            parent_slot,
            parent_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(mylite_ownerless_trx_registry_active_count(registry) == 0U);
    assert(
        mylite_ownerless_trx_registry_oldest_active_trx_id(registry, MYLITE_TEST_PAGE_SIZE) == 0U
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_trx_registry_rejects_stale_end(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "trx-registry-stale-end.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint64_t trx_id = 0U;
    uint32_t slot = 0U;
    uint64_t generation = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_trx_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            200U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &trx_id,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id == 200U);
    assert(
        mylite_ownerless_trx_registry_end(registry, MYLITE_TEST_PAGE_SIZE, slot, generation + 1U) ==
        MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND
    );
    assert(mylite_ownerless_trx_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_trx_registry_end(registry, MYLITE_TEST_PAGE_SIZE, slot, generation) ==
        MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_end(registry, MYLITE_TEST_PAGE_SIZE, slot, generation) ==
        MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND
    );
    assert(mylite_ownerless_trx_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_trx_registry_reports_full_when_slots_exhausted(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "trx-registry-full.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint64_t trx_id = 0U;
    uint32_t slot = 0U;
    uint64_t generation = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_trx_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            500U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    for (uint32_t owner_id = 1U; owner_id <= MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT; ++owner_id) {
        assert(
            mylite_ownerless_trx_registry_begin(
                registry,
                MYLITE_TEST_PAGE_SIZE,
                owner_id,
                &trx_id,
                &slot,
                &generation
            ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
        );
        assert(trx_id == 499U + owner_id);
    }
    assert(mylite_ownerless_trx_registry_active_count(registry) == 4U);
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 504U);
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            5U,
            &trx_id,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_FULL
    );
    assert(mylite_ownerless_trx_registry_active_count(registry) == 4U);
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 504U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_trx_registry_snapshots_active_ids(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "trx-registry-snapshot.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint64_t trx_ids[MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT] = {0};
    uint64_t trx_id = 0U;
    uint32_t first_slot = 0U;
    uint32_t second_slot = 0U;
    uint32_t third_slot = 0U;
    uint64_t first_generation = 0U;
    uint64_t second_generation = 0U;
    uint64_t third_generation = 0U;
    uint32_t trx_id_count = 0U;
    uint64_t next_trx_id = 0U;
    uint64_t oldest_trx_id = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_trx_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            300U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            &trx_id,
            &first_slot,
            &first_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id == 300U);
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &trx_id,
            &second_slot,
            &second_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id == 301U);
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &trx_id,
            &third_slot,
            &third_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id == 302U);
    assert(
        mylite_ownerless_trx_registry_snapshot(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            NULL,
            0U,
            &trx_id_count,
            &next_trx_id,
            &oldest_trx_id
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_FULL
    );
    assert(trx_id_count == 3U);
    assert(next_trx_id == 303U);
    assert(oldest_trx_id == 300U);
    assert(
        mylite_ownerless_trx_registry_snapshot(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            trx_ids,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            &trx_id_count,
            &next_trx_id,
            &oldest_trx_id
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id_count == 3U);
    assert(trx_ids[0] == 300U);
    assert(trx_ids[1] == 301U);
    assert(trx_ids[2] == 302U);
    assert(next_trx_id == 303U);
    assert(oldest_trx_id == 300U);
    assert(
        mylite_ownerless_trx_registry_end(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_oldest_active_trx_id(registry, MYLITE_TEST_PAGE_SIZE) == 301U
    );
    assert(
        mylite_ownerless_trx_registry_end(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            second_slot,
            second_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_end(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            third_slot,
            third_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(mylite_ownerless_trx_registry_active_count(registry) == 0U);
    assert(
        mylite_ownerless_trx_registry_oldest_active_trx_id(registry, MYLITE_TEST_PAGE_SIZE) == 0U
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_trx_registry_assigns_read_view_serialisation_numbers(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "trx-registry-read-view.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint64_t trx_ids[MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT] = {0};
    uint64_t first_trx_id = 0U;
    uint64_t second_trx_id = 0U;
    uint64_t third_trx_id = 0U;
    uint64_t allocated_id = 0U;
    uint32_t first_slot = 0U;
    uint32_t second_slot = 0U;
    uint32_t third_slot = 0U;
    uint64_t first_generation = 0U;
    uint64_t second_generation = 0U;
    uint64_t third_generation = 0U;
    uint32_t trx_id_count = 0U;
    uint64_t next_trx_id = 0U;
    uint64_t min_trx_no = 0U;
    uint64_t first_trx_no = 0U;
    uint64_t second_trx_no = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_trx_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            700U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_allocate_id(registry, MYLITE_TEST_PAGE_SIZE, &allocated_id) ==
        MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(allocated_id == 700U);
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 701U);
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &first_trx_id,
            &first_slot,
            &first_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &second_trx_id,
            &second_slot,
            &second_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U,
            &third_trx_id,
            &third_slot,
            &third_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(first_trx_id == 701U);
    assert(second_trx_id == 702U);
    assert(third_trx_id == 703U);
    assert(
        mylite_ownerless_trx_registry_snapshot_read_view(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            NULL,
            0U,
            &trx_id_count,
            &next_trx_id,
            &min_trx_no
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_FULL
    );
    assert(trx_id_count == 3U);
    assert(next_trx_id == 704U);
    assert(min_trx_no == 704U);
    assert(
        mylite_ownerless_trx_registry_assign_new_no(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            second_trx_id,
            &second_trx_no
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(second_trx_no == 704U);
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 705U);
    assert(
        mylite_ownerless_trx_registry_assign_new_no(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            first_trx_id,
            &first_trx_no
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(first_trx_no == 705U);
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 706U);
    assert(
        mylite_ownerless_trx_registry_assign_new_no(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            999U,
            &allocated_id
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND
    );
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 706U);
    assert(
        mylite_ownerless_trx_registry_snapshot_read_view(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            trx_ids,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            &trx_id_count,
            &next_trx_id,
            &min_trx_no
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id_count == 3U);
    assert(trx_ids[0] == first_trx_id);
    assert(trx_ids[1] == second_trx_id);
    assert(trx_ids[2] == third_trx_id);
    assert(next_trx_id == 706U);
    assert(min_trx_no == second_trx_no);
    assert(
        mylite_ownerless_trx_registry_assign_no(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            third_trx_id,
            710U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_snapshot_read_view(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            trx_ids,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            &trx_id_count,
            &next_trx_id,
            &min_trx_no
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(min_trx_no == second_trx_no);
    assert(
        mylite_ownerless_trx_registry_end(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            second_slot,
            second_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_snapshot_read_view(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            trx_ids,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            &trx_id_count,
            &next_trx_id,
            &min_trx_no
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id_count == 2U);
    assert(min_trx_no == first_trx_no);
    assert(
        mylite_ownerless_trx_registry_end(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_end(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            third_slot,
            third_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_trx_registry_bumps_next_id_and_ends_by_owner_id(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "trx-registry-owner-end.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint64_t first_trx_id = 0U;
    uint64_t second_trx_id = 0U;
    uint32_t first_slot = 0U;
    uint32_t second_slot = 0U;
    uint64_t first_generation = 0U;
    uint64_t second_generation = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_trx_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            5U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_ensure_next_id_at_least(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            3U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 5U);
    assert(
        mylite_ownerless_trx_registry_ensure_next_id_at_least(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            20U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 20U);

    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            7U,
            &first_trx_id,
            &first_slot,
            &first_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            8U,
            &second_trx_id,
            &second_slot,
            &second_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(first_trx_id == 20U);
    assert(second_trx_id == 21U);
    assert(mylite_ownerless_trx_registry_active_count(registry) == 2U);
    assert(
        mylite_ownerless_trx_registry_end_by_id(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            8U,
            first_trx_id
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND
    );
    assert(mylite_ownerless_trx_registry_active_count(registry) == 2U);
    assert(
        mylite_ownerless_trx_registry_end_by_id(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            7U,
            first_trx_id
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(mylite_ownerless_trx_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_trx_registry_oldest_active_trx_id(registry, MYLITE_TEST_PAGE_SIZE) ==
        second_trx_id
    );
    assert(
        mylite_ownerless_trx_registry_end(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            second_slot,
            second_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(mylite_ownerless_trx_registry_active_count(registry) == 0U);
    assert(mylite_ownerless_trx_registry_next_trx_id(registry) == 22U);

    (void)first_slot;
    (void)first_generation;
    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_trx_registry_releases_dead_owner_transactions(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "trx-registry-owner-cleanup.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint64_t trx_id = 0U;
    uint64_t trx_ids[MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT] = {0};
    uint32_t slot = 0U;
    uint64_t generation = 0U;
    uint32_t released_transactions = 0U;
    uint32_t trx_id_count = 0U;
    uint64_t next_trx_id = 0U;
    uint64_t oldest_trx_id = 0U;
    uint32_t active_count = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_trx_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            400U
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &trx_id,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id == 400U);
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &trx_id,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id == 401U);
    assert(
        mylite_ownerless_trx_registry_begin(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &trx_id,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id == 402U);
    assert(
        mylite_ownerless_trx_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(active_count == 2U);
    assert(
        mylite_ownerless_trx_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &active_count
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(active_count == 1U);
    assert(
        mylite_ownerless_trx_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &released_transactions
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(released_transactions == 2U);
    assert(
        mylite_ownerless_trx_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(active_count == 0U);
    assert(mylite_ownerless_trx_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_trx_registry_oldest_active_trx_id(registry, MYLITE_TEST_PAGE_SIZE) == 401U
    );
    assert(
        mylite_ownerless_trx_registry_snapshot(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            trx_ids,
            MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT,
            &trx_id_count,
            &next_trx_id,
            &oldest_trx_id
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id_count == 1U);
    assert(trx_ids[0] == 401U);
    assert(next_trx_id == 403U);
    assert(oldest_trx_id == 401U);
    assert(
        mylite_ownerless_trx_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &released_transactions
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(released_transactions == 0U);
    assert(
        mylite_ownerless_trx_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &released_transactions
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(released_transactions == 1U);
    assert(mylite_ownerless_trx_registry_active_count(registry) == 0U);
    assert(
        mylite_ownerless_trx_registry_oldest_active_trx_id(registry, MYLITE_TEST_PAGE_SIZE) == 0U
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_read_view_registry_snapshots_oldest_views(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "read-view-registry-snapshot.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint64_t first_ids[] = {10U, 15U, 19U};
    uint64_t second_ids[] = {9U, 12U, 21U};
    uint64_t oldest_ids[4] = {0};
    uint64_t overflow_ids[MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ID_CAPACITY + 1U] = {0};
    uint32_t first_slot = 0U;
    uint32_t second_slot = 0U;
    uint64_t first_generation = 0U;
    uint64_t second_generation = 0U;
    uint32_t oldest_id_count = 0U;
    uint64_t low_limit_id = 0U;
    uint64_t low_limit_no = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_read_view_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_READ_VIEW_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            20U,
            30U,
            first_ids,
            3U,
            &first_slot,
            &first_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            18U,
            25U,
            second_ids,
            3U,
            &second_slot,
            &second_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(mylite_ownerless_read_view_registry_active_count(registry) == 2U);
    assert(
        mylite_ownerless_read_view_registry_snapshot_oldest(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            oldest_ids,
            2U,
            &oldest_id_count,
            &low_limit_id,
            &low_limit_no
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL
    );
    assert(oldest_id_count == 4U);
    assert(low_limit_id == 18U);
    assert(low_limit_no == 25U);
    assert(
        mylite_ownerless_read_view_registry_snapshot_oldest(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            oldest_ids,
            4U,
            &oldest_id_count,
            &low_limit_id,
            &low_limit_no
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(oldest_id_count == 4U);
    assert(oldest_ids[0] == 9U);
    assert(oldest_ids[1] == 10U);
    assert(oldest_ids[2] == 12U);
    assert(oldest_ids[3] == 15U);
    assert(low_limit_id == 18U);
    assert(low_limit_no == 25U);
    assert(
        mylite_ownerless_read_view_registry_close(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(mylite_ownerless_read_view_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_read_view_registry_snapshot_oldest(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            oldest_ids,
            4U,
            &oldest_id_count,
            &low_limit_id,
            &low_limit_no
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(oldest_id_count == 2U);
    assert(oldest_ids[0] == 9U);
    assert(oldest_ids[1] == 12U);
    assert(low_limit_id == 18U);
    assert(low_limit_no == 25U);
    assert(
        mylite_ownerless_read_view_registry_close(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_NOT_FOUND
    );
    assert(
        mylite_ownerless_read_view_registry_close(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            second_slot,
            second_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(mylite_ownerless_read_view_registry_active_count(registry) == 0U);
    assert(
        mylite_ownerless_read_view_registry_snapshot_oldest(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            oldest_ids,
            4U,
            &oldest_id_count,
            &low_limit_id,
            &low_limit_no
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(oldest_id_count == 0U);
    assert(low_limit_id == 0U);
    assert(low_limit_no == 0U);
    assert(
        mylite_ownerless_read_view_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            40U,
            40U,
            overflow_ids,
            MYLITE_OWNERLESS_READ_VIEW_REGISTRY_ID_CAPACITY + 1U,
            &first_slot,
            &first_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_read_view_registry_snapshots_cross_process_views(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "read-view-registry-cross-process.bin");
    int fd = open_file(shm_path);
    int child_ready[2];
    int parent_done[2];
    void *registry;
    uint64_t parent_ids[] = {11U, 29U, 31U};
    uint64_t oldest_ids[4] = {0};
    uint32_t parent_slot = 0U;
    uint64_t parent_generation = 0U;
    uint32_t oldest_id_count = 0U;
    uint64_t low_limit_id = 0U;
    uint64_t low_limit_no = 0U;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_read_view_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_READ_VIEW_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            30U,
            50U,
            parent_ids,
            3U,
            &parent_slot,
            &parent_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(pipe(child_ready) == 0);
    assert(pipe(parent_done) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        void *child_registry;
        uint64_t child_ids[] = {7U, 11U, 28U};
        uint64_t child_oldest_ids[4] = {0};
        uint32_t child_slot = 0U;
        uint64_t child_generation = 0U;
        uint32_t child_oldest_count = 0U;
        uint64_t child_low_limit_id = 0U;
        uint64_t child_low_limit_no = 0U;

        close(child_ready[0]);
        close(parent_done[1]);
        child_fd = open_file(shm_path);
        child_registry = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        assert(
            mylite_ownerless_read_view_registry_open(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                25U,
                40U,
                child_ids,
                3U,
                &child_slot,
                &child_generation
            ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
        );
        assert(
            mylite_ownerless_read_view_registry_snapshot_oldest(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                child_oldest_ids,
                4U,
                &child_oldest_count,
                &child_low_limit_id,
                &child_low_limit_no
            ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
        );
        assert(child_oldest_count == 2U);
        assert(child_oldest_ids[0] == 7U);
        assert(child_oldest_ids[1] == 11U);
        assert(child_low_limit_id == 25U);
        assert(child_low_limit_no == 40U);
        signal_pipe(child_ready[1]);
        wait_for_pipe(parent_done[0]);
        assert(
            mylite_ownerless_read_view_registry_close(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                child_slot,
                child_generation
            ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
        );
        assert(munmap(child_registry, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    close(parent_done[0]);
    wait_for_pipe(child_ready[0]);
    assert(
        mylite_ownerless_read_view_registry_snapshot_oldest(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            oldest_ids,
            4U,
            &oldest_id_count,
            &low_limit_id,
            &low_limit_no
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(oldest_id_count == 2U);
    assert(oldest_ids[0] == 7U);
    assert(oldest_ids[1] == 11U);
    assert(low_limit_id == 25U);
    assert(low_limit_no == 40U);
    signal_pipe(parent_done[1]);
    wait_for_child(child);
    assert(mylite_ownerless_read_view_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_read_view_registry_close(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            parent_slot,
            parent_generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(mylite_ownerless_read_view_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_read_view_registry_releases_dead_owner_views(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "read-view-registry-owner-cleanup.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t slot = 0U;
    uint64_t generation = 0U;
    uint32_t released_views = 0U;
    uint32_t active_count = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_read_view_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_READ_VIEW_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            10U,
            NULL,
            0U,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            11U,
            11U,
            NULL,
            0U,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            12U,
            12U,
            NULL,
            0U,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(
        mylite_ownerless_read_view_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(active_count == 2U);
    assert(
        mylite_ownerless_read_view_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &active_count
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(active_count == 1U);
    assert(
        mylite_ownerless_read_view_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &released_views
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(released_views == 2U);
    assert(
        mylite_ownerless_read_view_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(active_count == 0U);
    assert(mylite_ownerless_read_view_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_read_view_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            &released_views
        ) == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
    );
    assert(released_views == 1U);
    assert(mylite_ownerless_read_view_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_page_pin_registry_snapshots_oldest_pins(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "page-pin-registry-snapshot.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t first_slot = 0U;
    uint32_t second_slot = 0U;
    uint32_t overflow_slot = 0U;
    uint64_t first_generation = 0U;
    uint64_t second_generation = 0U;
    uint64_t overflow_generation = 0U;
    uint32_t active_count = 0U;
    uint64_t oldest_read_lsn = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_page_pin_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PAGE_PIN_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(
        mylite_ownerless_page_pin_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            120U,
            &first_slot,
            &first_generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(
        mylite_ownerless_page_pin_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            90U,
            &second_slot,
            &second_generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(mylite_ownerless_page_pin_registry_active_count(registry) == 2U);
    assert(
        mylite_ownerless_page_pin_registry_snapshot_oldest(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            &active_count,
            &oldest_read_lsn
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(active_count == 2U);
    assert(oldest_read_lsn == 90U);
    assert(
        mylite_ownerless_page_pin_registry_close(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(
        mylite_ownerless_page_pin_registry_snapshot_oldest(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            &active_count,
            &oldest_read_lsn
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(active_count == 1U);
    assert(oldest_read_lsn == 90U);
    assert(
        mylite_ownerless_page_pin_registry_close(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_NOT_FOUND
    );
    assert(
        mylite_ownerless_page_pin_registry_close(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            second_slot,
            second_generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(
        mylite_ownerless_page_pin_registry_snapshot_oldest(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            &active_count,
            &oldest_read_lsn
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(active_count == 0U);
    assert(oldest_read_lsn == 0U);
    for (uint32_t index = 0U; index < MYLITE_TEST_PAGE_PIN_REGISTRY_SLOT_COUNT; ++index) {
        assert(
            mylite_ownerless_page_pin_registry_open(
                registry,
                MYLITE_TEST_PAGE_SIZE,
                index + 1U,
                index + 10U,
                1000U + index,
                &overflow_slot,
                &overflow_generation
            ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
        );
    }
    assert(
        mylite_ownerless_page_pin_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            99U,
            99U,
            9999U,
            &overflow_slot,
            &overflow_generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_FULL
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_page_pin_registry_snapshots_cross_process_pins(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "page-pin-registry-cross-process.bin");
    int fd = open_file(shm_path);
    int child_ready[2];
    int parent_done[2];
    void *registry;
    uint32_t parent_slot = 0U;
    uint64_t parent_generation = 0U;
    uint32_t active_count = 0U;
    uint64_t oldest_read_lsn = 0U;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_page_pin_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PAGE_PIN_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(
        mylite_ownerless_page_pin_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            120U,
            &parent_slot,
            &parent_generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(pipe(child_ready) == 0);
    assert(pipe(parent_done) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        void *child_registry;
        uint32_t child_slot = 0U;
        uint64_t child_generation = 0U;
        uint32_t child_active_count = 0U;
        uint64_t child_oldest_read_lsn = 0U;

        close(child_ready[0]);
        close(parent_done[1]);
        child_fd = open_file(shm_path);
        child_registry = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        assert(
            mylite_ownerless_page_pin_registry_open(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                20U,
                80U,
                &child_slot,
                &child_generation
            ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
        );
        assert(
            mylite_ownerless_page_pin_registry_snapshot_oldest(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                20U,
                &child_active_count,
                &child_oldest_read_lsn
            ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
        );
        assert(child_active_count == 2U);
        assert(child_oldest_read_lsn == 80U);
        signal_pipe(child_ready[1]);
        wait_for_pipe(parent_done[0]);
        assert(
            mylite_ownerless_page_pin_registry_close(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                20U,
                child_slot,
                child_generation
            ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
        );
        assert(munmap(child_registry, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    close(parent_done[0]);
    wait_for_pipe(child_ready[0]);
    assert(
        mylite_ownerless_page_pin_registry_snapshot_oldest(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            &active_count,
            &oldest_read_lsn
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(active_count == 2U);
    assert(oldest_read_lsn == 80U);
    signal_pipe(parent_done[1]);
    wait_for_child(child);
    assert(mylite_ownerless_page_pin_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_page_pin_registry_close(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            parent_slot,
            parent_generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(mylite_ownerless_page_pin_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_page_pin_registry_releases_dead_owner_pins(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "page-pin-registry-owner-cleanup.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t slot = 0U;
    uint64_t generation = 0U;
    uint32_t released_pins = 0U;
    uint32_t active_count = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_page_pin_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PAGE_PIN_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(
        mylite_ownerless_page_pin_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            100U,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(
        mylite_ownerless_page_pin_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            20U,
            80U,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(
        mylite_ownerless_page_pin_registry_open(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            10U,
            120U,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(
        mylite_ownerless_page_pin_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            99U,
            99U,
            &active_count
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(active_count == 2U);
    assert(
        mylite_ownerless_page_pin_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            99U,
            99U,
            &released_pins
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(released_pins == 2U);
    assert(
        mylite_ownerless_page_pin_registry_owner_active_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            1U,
            99U,
            99U,
            &active_count
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(active_count == 0U);
    assert(mylite_ownerless_page_pin_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_page_pin_registry_release_owner(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            2U,
            99U,
            99U,
            &released_pins
        ) == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK
    );
    assert(released_pins == 1U);
    assert(mylite_ownerless_page_pin_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_dictionary_state_serializes_ddl_generations(void) {
    uint8_t state[MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE];
    uint64_t generation = 0U;
    uint32_t active_count = 0U;
    mylite_ownerless_dictionary_state_snapshot snapshot;

    assert(
        mylite_ownerless_dictionary_state_initialize(state, sizeof(state)) ==
        MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_wait_ready(
            state,
            sizeof(state),
            dictionary_state_pid_is_alive,
            NULL,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 0U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            1U,
            10U,
            (uint64_t)getpid(),
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert((generation & 1U) == 1U);
    assert(
        mylite_ownerless_dictionary_state_owner_active_count(
            state,
            sizeof(state),
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(active_count == 1U);
    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            1U,
            10U,
            (uint64_t)getpid(),
            1U,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_TIMEOUT
    );
    assert(
        mylite_ownerless_dictionary_state_wait_ready(
            state,
            sizeof(state),
            dictionary_state_pid_is_alive,
            NULL,
            1U,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_TIMEOUT
    );

    assert(
        mylite_ownerless_dictionary_state_finish_ddl(state, sizeof(state), 1U, 10U, &generation) ==
        MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert((generation & 1U) == 0U);
    assert(
        mylite_ownerless_dictionary_state_wait_ready(
            state,
            sizeof(state),
            dictionary_state_pid_is_alive,
            NULL,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 2U);
    assert(
        mylite_ownerless_dictionary_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(snapshot.active_owner_id == 0U);
}

static void test_dictionary_state_reports_dead_active_owner(void) {
    uint8_t state[MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE];
    uint64_t generation = 0U;

    assert(
        mylite_ownerless_dictionary_state_initialize(state, sizeof(state)) ==
        MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            1U,
            10U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_wait_ready(
            state,
            sizeof(state),
            dictionary_state_pid_is_alive,
            NULL,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_BUSY
    );
}

static void test_dictionary_state_recovers_marked_dead_owner(void) {
    uint8_t state[MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE];
    uint64_t generation = 0U;
    uint32_t active_count = 0U;

    assert(
        mylite_ownerless_dictionary_state_initialize(state, sizeof(state)) ==
        MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            1U,
            10U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            1U,
            10U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            1U,
            11U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            1U,
            10U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            1U,
            11U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            1U,
            10U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 2U);
    assert(
        mylite_ownerless_dictionary_state_owner_active_count(
            state,
            sizeof(state),
            1U,
            &active_count
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(active_count == 0U);
    assert(
        mylite_ownerless_dictionary_state_wait_ready(
            state,
            sizeof(state),
            dictionary_state_pid_is_alive,
            NULL,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 2U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            2U,
            20U,
            (uint64_t)getpid(),
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            2U,
            20U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_finish_ddl(state, sizeof(state), 2U, 20U, &generation) ==
        MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            2U,
            20U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            3U,
            30U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            3U,
            30U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_LIKE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            3U,
            30U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            3U,
            30U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_LIKE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 6U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            4U,
            40U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            4U,
            40U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_SELECT
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            4U,
            40U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_LIKE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            4U,
            40U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_SELECT,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 8U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            5U,
            50U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            5U,
            50U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            5U,
            50U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_SELECT,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            5U,
            50U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 10U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            6U,
            60U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            6U,
            60U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_LIKE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            6U,
            60U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            6U,
            60U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_LIKE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 12U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            7U,
            70U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            7U,
            70U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_SELECT
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            7U,
            70U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_LIKE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            7U,
            70U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_SELECT,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 14U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            8U,
            80U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            8U,
            80U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            8U,
            80U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_SELECT,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            8U,
            80U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 16U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            9U,
            90U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            9U,
            90U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TRUNCATE_TABLE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            9U,
            90U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            9U,
            90U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TRUNCATE_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 18U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            10U,
            100U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TRUNCATE_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 20U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            10U,
            100U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_FORCE_REBUILD
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_FORCE_REBUILD,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 22U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            10U,
            100U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ROW_FORMAT_DYNAMIC
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_FORCE_REBUILD,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ROW_FORMAT_DYNAMIC,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 24U);

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            10U,
            100U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_8
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ROW_FORMAT_DYNAMIC,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_8,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 26U);

    const uint32_t compressed_key_block_recovery_kinds[] = {
        MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_1,
        MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_2,
        MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_4,
        MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_16,
    };
    for (size_t index = 0U; index < sizeof(compressed_key_block_recovery_kinds) /
                                        sizeof(compressed_key_block_recovery_kinds[0]);
         ++index) {
        assert(
            mylite_ownerless_dictionary_state_begin_ddl(
                state,
                sizeof(state),
                10U,
                100U,
                UINT64_MAX,
                MYLITE_TEST_WAIT_TIMEOUT_MS,
                &generation
            ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
        );
        assert(
            mylite_ownerless_dictionary_state_mark_recoverable(
                state,
                sizeof(state),
                10U,
                100U,
                compressed_key_block_recovery_kinds[index]
            ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
        );
        assert(
            mylite_ownerless_dictionary_state_recover_dead_owner(
                state,
                sizeof(state),
                10U,
                100U,
                MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ROW_FORMAT_DYNAMIC,
                &generation
            ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
        );
        assert(
            mylite_ownerless_dictionary_state_recover_dead_owner(
                state,
                sizeof(state),
                10U,
                100U,
                compressed_key_block_recovery_kinds[index],
                &generation
            ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
        );
        assert(generation == 28U + (2U * (uint64_t)index));
    }

    const uint32_t trigger_recovery_kinds[] = {
        MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TRIGGER,
        MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TRIGGER,
    };
    for (size_t index = 0U;
         index < sizeof(trigger_recovery_kinds) / sizeof(trigger_recovery_kinds[0]);
         ++index) {
        assert(
            mylite_ownerless_dictionary_state_begin_ddl(
                state,
                sizeof(state),
                10U,
                100U,
                UINT64_MAX,
                MYLITE_TEST_WAIT_TIMEOUT_MS,
                &generation
            ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
        );
        assert(
            mylite_ownerless_dictionary_state_mark_recoverable(
                state,
                sizeof(state),
                10U,
                100U,
                trigger_recovery_kinds[index]
            ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
        );
        assert(
            mylite_ownerless_dictionary_state_recover_dead_owner(
                state,
                sizeof(state),
                10U,
                100U,
                MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_16,
                &generation
            ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
        );
        assert(
            mylite_ownerless_dictionary_state_recover_dead_owner(
                state,
                sizeof(state),
                10U,
                100U,
                trigger_recovery_kinds[index],
                &generation
            ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
        );
        assert(generation == 36U + (2U * (uint64_t)index));
    }

    assert(
        mylite_ownerless_dictionary_state_begin_ddl(
            state,
            sizeof(state),
            10U,
            100U,
            UINT64_MAX,
            MYLITE_TEST_WAIT_TIMEOUT_MS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE_IF_EXISTS
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TRIGGER,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    assert(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state,
            sizeof(state),
            10U,
            100U,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE_IF_EXISTS,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    assert(generation == 40U);
}

static void test_redo_state_tracks_lsn_and_owner_lifecycle(void) {
    uint8_t state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    uint8_t overflow_state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    uint64_t latest_lsn = 0U;
    uint64_t advanced_lsn = 0U;
    uint64_t start_lsn = 0U;
    uint64_t end_lsn = 0U;
    uint32_t remaining = 0U;
    uint32_t released = 0U;
    uint32_t active_count = 0U;
    mylite_ownerless_redo_state_snapshot snapshot;

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 120U, 100U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latest_lsn == 120U);
    assert(snapshot.visible_lsn == 100U);
    assert(snapshot.reserved_lsn == 120U);
    assert(snapshot.written_lsn == 120U);
    assert(snapshot.refcount == 0U);
    assert(snapshot.active_reservation_count == 0U);
    assert(snapshot.progress_latch_state == MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED);
    assert(
        mylite_ownerless_latch_acquire(
            (mylite_ownerless_latch *)(state + MYLITE_TEST_REDO_STATE_PROGRESS_LATCH_OFFSET),
            8U,
            80U,
            NULL,
            NULL,
            100U
        ) == MYLITE_OWNERLESS_LATCH_OK
    );
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.progress_latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED);
    assert(snapshot.progress_latch_owner_id == 8U);
    assert(snapshot.progress_latch_owner_generation == 80U);
    assert(
        mylite_ownerless_latch_release(
            (mylite_ownerless_latch *)(state + MYLITE_TEST_REDO_STATE_PROGRESS_LATCH_OFFSET),
            8U,
            80U
        ) == MYLITE_OWNERLESS_LATCH_OK
    );

    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 1U, 10U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(latest_lsn == 120U);
    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 1U, 10U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(latest_lsn == 120U);
    assert(
        mylite_ownerless_redo_state_leave(
            state,
            sizeof(state),
            2U,
            20U,
            999U,
            &advanced_lsn,
            &remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_ERROR
    );
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latest_lsn == 120U);
    assert(snapshot.refcount == 2U);
    assert(snapshot.latch_state == MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED);
    assert(snapshot.active_reservation_count == 0U);
    assert(
        mylite_ownerless_redo_state_reserve(
            state,
            sizeof(state),
            1U,
            10U,
            0U,
            5U,
            &start_lsn,
            &end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(start_lsn == 120U);
    assert(end_lsn == 125U);
    assert(
        mylite_ownerless_redo_state_owner_active_count(state, sizeof(state), 1U, &active_count) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(active_count == 2U);
    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 1U, 10U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(latest_lsn == 125U);
    assert(
        mylite_ownerless_redo_state_leave(
            state,
            sizeof(state),
            1U,
            10U,
            140U,
            &advanced_lsn,
            &remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(advanced_lsn == 140U);
    assert(remaining == 2U);
    assert(
        mylite_ownerless_redo_state_leave(
            state,
            sizeof(state),
            1U,
            10U,
            150U,
            &advanced_lsn,
            &remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(advanced_lsn == 150U);
    assert(remaining == 1U);
    assert(
        mylite_ownerless_redo_state_leave(
            state,
            sizeof(state),
            1U,
            10U,
            150U,
            &advanced_lsn,
            &remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(advanced_lsn == 0U);
    assert(remaining == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latest_lsn == 150U);
    assert(snapshot.reserved_lsn == 150U);
    assert(snapshot.written_lsn == 120U);
    assert(snapshot.refcount == 0U);
    assert(snapshot.active_reservation_count == 1U);
    assert(snapshot.latch_state == MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED);

    assert(
        mylite_ownerless_redo_state_reserve(
            state,
            sizeof(state),
            2U,
            20U,
            0U,
            32U,
            &start_lsn,
            &end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(start_lsn == 150U);
    assert(end_lsn == 182U);
    assert(
        mylite_ownerless_redo_state_reserve(
            state,
            sizeof(state),
            3U,
            30U,
            240U,
            18U,
            &start_lsn,
            &end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(start_lsn == 240U);
    assert(end_lsn == 258U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.active_reservation_count == 3U);
    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            2U,
            20U,
            150U,
            182U,
            &advanced_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(advanced_lsn == 0U);
    assert(
        mylite_ownerless_redo_state_owner_active_count(state, sizeof(state), 2U, &active_count) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(active_count == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.active_reservation_count == 2U);

    assert(
        mylite_ownerless_redo_state_publish_visible(
            state,
            sizeof(state),
            180U,
            &latest_lsn,
            &advanced_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(latest_lsn == 150U);
    assert(advanced_lsn == 120U);

    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 4U, 40U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_owner_active_count(state, sizeof(state), 4U, &active_count) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(active_count == 1U);
    assert(
        mylite_ownerless_redo_state_cleanup_owner(state, sizeof(state), 4U, 40U, &released) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(released == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latch_state == MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED);
    assert(snapshot.refcount == 0U);
    assert(snapshot.active_reservation_count == 2U);
    assert(
        mylite_ownerless_redo_state_owner_active_count(state, sizeof(state), 4U, &active_count) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(active_count == 0U);

    assert(
        mylite_ownerless_redo_state_initialize(
            overflow_state,
            sizeof(overflow_state),
            UINT64_MAX - 2U,
            100U
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_reserve(
            overflow_state,
            sizeof(overflow_state),
            5U,
            50U,
            0U,
            4U,
            &start_lsn,
            &end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_ERROR
    );
    assert(start_lsn == 0U);
    assert(end_lsn == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(
            overflow_state,
            sizeof(overflow_state),
            &snapshot
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.reserved_lsn == UINT64_MAX - 2U);
    assert(snapshot.written_lsn == UINT64_MAX - 2U);
    assert(snapshot.latch_state == MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED);
}

static void test_redo_state_seeds_checkpoint_monotonically(void) {
    uint8_t state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    uint64_t latest_lsn = 0U;
    uint64_t advanced_lsn = 0U;
    uint32_t remaining = 0U;
    uint32_t active_count = 0U;
    mylite_ownerless_redo_state_snapshot snapshot;

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 120U, 100U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 1U, 10U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(latest_lsn == 120U);

    assert(
        mylite_ownerless_redo_state_seed_checkpoint(state, sizeof(state), 110U, 90U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latest_lsn == 120U);
    assert(snapshot.visible_lsn == 100U);
    assert(snapshot.reserved_lsn == 120U);
    assert(snapshot.durable_lsn == 100U);
    assert(snapshot.written_lsn == 120U);
    assert(snapshot.refcount == 1U);
    assert(
        mylite_ownerless_redo_state_owner_active_count(state, sizeof(state), 1U, &active_count) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(active_count == 1U);

    assert(
        mylite_ownerless_redo_state_seed_checkpoint(state, sizeof(state), 140U, 130U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latest_lsn == 140U);
    assert(snapshot.visible_lsn == 130U);
    assert(snapshot.reserved_lsn == 140U);
    assert(snapshot.durable_lsn == 130U);
    assert(snapshot.written_lsn == 140U);
    assert(snapshot.refcount == 1U);

    assert(
        mylite_ownerless_redo_state_seed_checkpoint(state, sizeof(state), 125U, 150U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latest_lsn == 150U);
    assert(snapshot.visible_lsn == 150U);
    assert(snapshot.reserved_lsn == 150U);
    assert(snapshot.durable_lsn == 150U);
    assert(snapshot.written_lsn == 150U);
    assert(snapshot.refcount == 1U);
    assert(
        mylite_ownerless_redo_state_leave(
            state,
            sizeof(state),
            1U,
            10U,
            150U,
            &advanced_lsn,
            &remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(advanced_lsn == 0U);
    assert(remaining == 0U);
}

static void test_redo_state_reserves_ranges_for_same_owner_threads(void) {
    enum {
        thread_count = 4,
        reservations_per_thread = 128,
        reservation_count = thread_count * reservations_per_thread,
    };

    uint8_t state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    pthread_t threads[thread_count];
    redo_reserve_thread_context contexts[thread_count];
    uint64_t starts[reservation_count];
    uint64_t latest_lsn = 0U;
    uint64_t advanced_lsn = 0U;
    uint32_t remaining = 0U;
    mylite_ownerless_redo_state_snapshot snapshot;

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 300U, 300U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 1U, 10U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(latest_lsn == 300U);

    for (size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        contexts[thread_index].state = state;
        contexts[thread_index].state_size = sizeof(state);
        contexts[thread_index].starts = starts;
        contexts[thread_index].offset = thread_index * reservations_per_thread;
        contexts[thread_index].count = reservations_per_thread;
        assert(
            pthread_create(
                &threads[thread_index],
                NULL,
                reserve_redo_ranges_in_thread,
                &contexts[thread_index]
            ) == 0
        );
    }
    for (size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        assert(pthread_join(threads[thread_index], NULL) == 0);
    }

    qsort(starts, reservation_count, sizeof(starts[0]), compare_uint64_values);
    for (size_t index = 0; index < reservation_count; ++index) {
        assert(starts[index] == 300U + index);
    }
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.reserved_lsn == 300U + reservation_count);
    assert(snapshot.written_lsn == 300U + reservation_count);
    assert(snapshot.active_reservation_count == 0U);
    assert(
        mylite_ownerless_redo_state_leave(
            state,
            sizeof(state),
            1U,
            10U,
            300U + reservation_count,
            &advanced_lsn,
            &remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(advanced_lsn == 300U + reservation_count);
    assert(remaining == 0U);
}

static void test_redo_state_allows_bounded_fanout_reservations(void) {
    enum {
        reservation_count = 32,
    };

    uint8_t state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    uint64_t starts[reservation_count];
    uint64_t ends[reservation_count];
    uint64_t written_lsn = 0U;
    mylite_ownerless_redo_state_snapshot snapshot;

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 400U, 400U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    for (uint32_t owner_id = 1U; owner_id <= reservation_count; ++owner_id) {
        assert(
            mylite_ownerless_redo_state_reserve(
                state,
                sizeof(state),
                owner_id,
                owner_id + 100U,
                0U,
                1U,
                &starts[owner_id - 1U],
                &ends[owner_id - 1U]
            ) == MYLITE_OWNERLESS_REDO_STATE_OK
        );
        assert(ends[owner_id - 1U] == starts[owner_id - 1U] + 1U);
    }
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.active_reservation_count == reservation_count);

    for (uint32_t owner_id = 1U; owner_id <= reservation_count; ++owner_id) {
        assert(
            mylite_ownerless_redo_state_complete_write(
                state,
                sizeof(state),
                owner_id,
                owner_id + 100U,
                starts[owner_id - 1U],
                ends[owner_id - 1U],
                &written_lsn
            ) == MYLITE_OWNERLESS_REDO_STATE_OK
        );
    }
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.active_reservation_count == 0U);
    assert(snapshot.written_lsn == 400U + reservation_count);
}

static void test_redo_state_deferred_batch_cap_preserves_peer_headroom(void) {
    uint8_t state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    mylite_ownerless_redo_state_range ranges[MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES];
    uint64_t latest_lsn = 0U;
    uint64_t peer_start_lsn = 0U;
    uint64_t peer_end_lsn = 0U;
    uint64_t written_lsn = 0U;
    uint64_t advanced_lsn = 0U;
    uint32_t remaining = 0U;
    size_t completed_count = 0U;
    mylite_ownerless_redo_state_snapshot snapshot;

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 1000U, 1000U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    for (uint32_t index = 0U; index < MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES; ++index) {
        assert(
            mylite_ownerless_redo_state_enter(state, sizeof(state), 1U, 10U, 100U, &latest_lsn) ==
            MYLITE_OWNERLESS_REDO_STATE_OK
        );
        assert(
            mylite_ownerless_redo_state_reserve(
                state,
                sizeof(state),
                1U,
                10U,
                0U,
                1U,
                &ranges[index].start_lsn,
                &ranges[index].end_lsn
            ) == MYLITE_OWNERLESS_REDO_STATE_OK
        );
    }
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.refcount == MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES);
    assert(snapshot.active_reservation_count == MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES);

    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 2U, 20U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_reserve(
            state,
            sizeof(state),
            2U,
            20U,
            0U,
            1U,
            &peer_start_lsn,
            &peer_end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_complete_write_and_leave(
            state,
            sizeof(state),
            2U,
            20U,
            peer_start_lsn,
            peer_end_lsn,
            0U,
            &written_lsn,
            &advanced_lsn,
            &remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(remaining == MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES);

    assert(
        mylite_ownerless_redo_state_complete_write_and_leave_batch(
            state,
            sizeof(state),
            1U,
            10U,
            ranges,
            MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES,
            1200U,
            &written_lsn,
            &advanced_lsn,
            &remaining,
            &completed_count
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(completed_count == MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES);
    assert(advanced_lsn == 1200U);
    assert(remaining == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.refcount == 0U);
    assert(snapshot.active_reservation_count == 0U);
    assert(snapshot.completed_range_count == 0U);
    assert(snapshot.latest_lsn == 1200U);
    assert(snapshot.written_lsn == 1200U);
}

static void test_redo_state_tracks_contiguous_written_ranges(void) {
    uint8_t state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    uint64_t latest_lsn = 0U;
    uint64_t advanced_lsn = 0U;
    uint64_t written_lsn = 0U;
    uint64_t start_lsn = 0U;
    uint64_t end_lsn = 0U;
    uint32_t remaining = 0U;
    mylite_ownerless_redo_state_snapshot snapshot;

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 100U, 100U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            1U,
            10U,
            120U,
            130U,
            &written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(written_lsn == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.written_lsn == 100U);
    assert(snapshot.completed_range_count == 1U);

    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            1U,
            10U,
            100U,
            110U,
            &written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(written_lsn == 110U);
    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            1U,
            10U,
            110U,
            120U,
            &written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(written_lsn == 130U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.written_lsn == 130U);
    assert(snapshot.completed_range_count == 0U);

    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            1U,
            10U,
            141U,
            150U,
            &written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(written_lsn == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.written_lsn == 130U);
    assert(snapshot.completed_range_count == 1U);
    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            1U,
            10U,
            131U,
            140U,
            &written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(written_lsn == 150U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.written_lsn == 150U);
    assert(snapshot.completed_range_count == 0U);

    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            1U,
            10U,
            105U,
            115U,
            &written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(written_lsn == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.completed_range_count == 0U);

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 200U, 200U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 1U, 10U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(latest_lsn == 200U);
    assert(
        mylite_ownerless_redo_state_leave(
            state,
            sizeof(state),
            1U,
            10U,
            240U,
            &advanced_lsn,
            &remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(advanced_lsn == 240U);
    assert(remaining == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latest_lsn == 240U);
    assert(snapshot.reserved_lsn == 240U);
    assert(snapshot.written_lsn == 240U);
    assert(snapshot.completed_range_count == 0U);

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 0U, 0U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_reserve(
            state,
            sizeof(state),
            1U,
            10U,
            12288U,
            16U,
            &start_lsn,
            &end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(start_lsn == 12288U);
    assert(end_lsn == 12304U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.written_lsn == 12288U);
    assert(snapshot.completed_range_count == 0U);
    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            1U,
            10U,
            12288U,
            12304U,
            &written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(written_lsn == 12304U);
    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            1U,
            10U,
            12305U,
            12320U,
            &written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(written_lsn == 12320U);

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 100U, 100U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    for (uint64_t lsn = 110U; lsn < 190U; ++lsn) {
        assert(
            mylite_ownerless_redo_state_complete_write(
                state,
                sizeof(state),
                1U,
                10U,
                lsn,
                lsn + 1U,
                &written_lsn
            ) == MYLITE_OWNERLESS_REDO_STATE_OK
        );
        assert(written_lsn == 0U);
    }
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.completed_range_count == 1U);
    assert(
        mylite_ownerless_redo_state_complete_write(
            state,
            sizeof(state),
            1U,
            10U,
            100U,
            110U,
            &written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(written_lsn == 190U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.completed_range_count == 0U);
}

static void test_redo_state_combines_write_and_leave_like_separate_steps(void) {
    uint8_t separate_state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    uint8_t combined_state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    uint64_t latest_lsn = 0U;
    uint64_t separate_start_lsn = 0U;
    uint64_t separate_end_lsn = 0U;
    uint64_t combined_start_lsn = 0U;
    uint64_t combined_end_lsn = 0U;
    uint64_t separate_written_lsn = 0U;
    uint64_t combined_written_lsn = 0U;
    uint64_t separate_advanced_lsn = 0U;
    uint64_t combined_advanced_lsn = 0U;
    uint32_t separate_remaining = 0U;
    uint32_t combined_remaining = 0U;
    mylite_ownerless_redo_state_snapshot separate_snapshot;
    mylite_ownerless_redo_state_snapshot combined_snapshot;

    assert(
        mylite_ownerless_redo_state_initialize(
            separate_state,
            sizeof(separate_state),
            100U,
            100U
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_initialize(
            combined_state,
            sizeof(combined_state),
            100U,
            100U
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_enter(
            separate_state,
            sizeof(separate_state),
            1U,
            10U,
            100U,
            &latest_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(latest_lsn == 100U);
    assert(
        mylite_ownerless_redo_state_enter(
            combined_state,
            sizeof(combined_state),
            1U,
            10U,
            100U,
            &latest_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(latest_lsn == 100U);
    assert(
        mylite_ownerless_redo_state_reserve(
            separate_state,
            sizeof(separate_state),
            1U,
            10U,
            0U,
            12U,
            &separate_start_lsn,
            &separate_end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_reserve(
            combined_state,
            sizeof(combined_state),
            1U,
            10U,
            0U,
            12U,
            &combined_start_lsn,
            &combined_end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(separate_start_lsn == combined_start_lsn);
    assert(separate_end_lsn == combined_end_lsn);

    assert(
        mylite_ownerless_redo_state_complete_write(
            separate_state,
            sizeof(separate_state),
            1U,
            10U,
            separate_start_lsn,
            separate_end_lsn,
            &separate_written_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_leave(
            separate_state,
            sizeof(separate_state),
            1U,
            10U,
            140U,
            &separate_advanced_lsn,
            &separate_remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_complete_write_and_leave(
            combined_state,
            sizeof(combined_state),
            1U,
            10U,
            combined_start_lsn,
            combined_end_lsn,
            140U,
            &combined_written_lsn,
            &combined_advanced_lsn,
            &combined_remaining
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );

    assert(separate_written_lsn == combined_written_lsn);
    assert(separate_advanced_lsn == combined_advanced_lsn);
    assert(separate_remaining == combined_remaining);
    assert(
        mylite_ownerless_redo_state_read_snapshot(
            separate_state,
            sizeof(separate_state),
            &separate_snapshot
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_read_snapshot(
            combined_state,
            sizeof(combined_state),
            &combined_snapshot
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(separate_snapshot.latest_lsn == combined_snapshot.latest_lsn);
    assert(separate_snapshot.visible_lsn == combined_snapshot.visible_lsn);
    assert(separate_snapshot.reserved_lsn == combined_snapshot.reserved_lsn);
    assert(separate_snapshot.durable_lsn == combined_snapshot.durable_lsn);
    assert(separate_snapshot.written_lsn == combined_snapshot.written_lsn);
    assert(separate_snapshot.refcount == combined_snapshot.refcount);
    assert(
        separate_snapshot.active_reservation_count == combined_snapshot.active_reservation_count
    );
    assert(separate_snapshot.completed_range_count == combined_snapshot.completed_range_count);
    assert(combined_snapshot.latest_lsn == 140U);
    assert(combined_snapshot.written_lsn == 140U);
    assert(combined_snapshot.refcount == 0U);
    assert(combined_snapshot.active_reservation_count == 0U);
    assert(combined_snapshot.completed_range_count == 0U);
}

static void test_redo_state_batches_write_and_leave_ranges(void) {
    uint8_t state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    uint64_t latest_lsn = 0U;
    uint64_t start_lsn = 0U;
    uint64_t end_lsn = 0U;
    uint64_t written_lsn = 0U;
    uint64_t advanced_lsn = 0U;
    uint32_t remaining = 0U;
    size_t completed_count = 99U;
    mylite_ownerless_redo_state_range ranges[2];
    mylite_ownerless_redo_state_snapshot snapshot;

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 100U, 100U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 1U, 10U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 1U, 10U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_reserve(
            state,
            sizeof(state),
            1U,
            10U,
            0U,
            10U,
            &ranges[0].start_lsn,
            &ranges[0].end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_reserve(
            state,
            sizeof(state),
            1U,
            10U,
            0U,
            10U,
            &ranges[1].start_lsn,
            &ranges[1].end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_complete_write_and_leave_batch(
            state,
            sizeof(state),
            1U,
            10U,
            ranges,
            2U,
            150U,
            &written_lsn,
            &advanced_lsn,
            &remaining,
            &completed_count
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(completed_count == 2U);
    assert(written_lsn == 120U);
    assert(advanced_lsn == 150U);
    assert(remaining == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latest_lsn == 150U);
    assert(snapshot.reserved_lsn == 150U);
    assert(snapshot.written_lsn == 150U);
    assert(snapshot.refcount == 0U);
    assert(snapshot.active_reservation_count == 0U);
    assert(snapshot.completed_range_count == 0U);

    assert(
        mylite_ownerless_redo_state_initialize(state, sizeof(state), 200U, 200U) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 2U, 20U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_enter(state, sizeof(state), 2U, 20U, 100U, &latest_lsn) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(
        mylite_ownerless_redo_state_reserve(
            state,
            sizeof(state),
            2U,
            20U,
            0U,
            8U,
            &start_lsn,
            &end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(start_lsn == 200U);
    assert(end_lsn == 208U);
    ranges[1].start_lsn = start_lsn;
    ranges[1].end_lsn = end_lsn;
    assert(
        mylite_ownerless_redo_state_reserve(
            state,
            sizeof(state),
            2U,
            20U,
            0U,
            8U,
            &start_lsn,
            &end_lsn
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(start_lsn == 208U);
    assert(end_lsn == 216U);
    ranges[0].start_lsn = start_lsn;
    ranges[0].end_lsn = end_lsn;
    completed_count = 0U;
    written_lsn = 0U;
    advanced_lsn = 0U;
    remaining = 0U;
    assert(
        mylite_ownerless_redo_state_complete_write_and_leave_batch(
            state,
            sizeof(state),
            2U,
            20U,
            ranges,
            2U,
            240U,
            &written_lsn,
            &advanced_lsn,
            &remaining,
            &completed_count
        ) == MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(completed_count == 2U);
    assert(written_lsn == 216U);
    assert(advanced_lsn == 240U);
    assert(remaining == 0U);
    assert(
        mylite_ownerless_redo_state_read_snapshot(state, sizeof(state), &snapshot) ==
        MYLITE_OWNERLESS_REDO_STATE_OK
    );
    assert(snapshot.latest_lsn == 240U);
    assert(snapshot.written_lsn == 240U);
    assert(snapshot.active_reservation_count == 0U);
    assert(snapshot.completed_range_count == 0U);

    completed_count = 99U;
    written_lsn = 99U;
    advanced_lsn = 99U;
    remaining = 99U;
    assert(
        mylite_ownerless_redo_state_complete_write_and_leave_batch(
            state,
            sizeof(state),
            2U,
            20U,
            ranges,
            0U,
            240U,
            &written_lsn,
            &advanced_lsn,
            &remaining,
            &completed_count
        ) == MYLITE_OWNERLESS_REDO_STATE_ERROR
    );
    assert(completed_count == 0U);
    assert(written_lsn == 0U);
    assert(advanced_lsn == 0U);
    assert(remaining == 0U);
}

static void *reserve_redo_ranges_in_thread(void *context) {
    redo_reserve_thread_context *reservation = (redo_reserve_thread_context *)context;

    for (size_t index = 0; index < reservation->count; ++index) {
        uint64_t start_lsn = 0U;
        uint64_t end_lsn = 0U;
        assert(
            mylite_ownerless_redo_state_reserve(
                reservation->state,
                reservation->state_size,
                1U,
                10U,
                0U,
                1U,
                &start_lsn,
                &end_lsn
            ) == MYLITE_OWNERLESS_REDO_STATE_OK
        );
        assert(end_lsn == start_lsn + 1U);
        reservation->starts[reservation->offset + index] = start_lsn;
        assert(
            mylite_ownerless_redo_state_complete_write(
                reservation->state,
                reservation->state_size,
                1U,
                10U,
                start_lsn,
                end_lsn,
                NULL
            ) == MYLITE_OWNERLESS_REDO_STATE_OK
        );
    }
    return NULL;
}

static int compare_uint64_values(const void *left, const void *right) {
    const uint64_t left_value = *(const uint64_t *)left;
    const uint64_t right_value = *(const uint64_t *)right;

    if (left_value < right_value) {
        return -1;
    }
    return left_value > right_value ? 1 : 0;
}

static void test_process_registry_allocates_cross_process_slots(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "process-registry.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t parent_slot = 0U;
    uint64_t parent_generation = 0U;
    uint64_t generation_after_child = 0U;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_process_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(mylite_ownerless_process_registry_generation(registry) == 0U);
    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            (uint64_t)getpid(),
            1U,
            0U,
            &parent_slot,
            &parent_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(mylite_ownerless_process_registry_active_count(registry) == 1U);
    assert(mylite_ownerless_process_registry_generation(registry) == parent_generation);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd = open_file(shm_path);
        void *child_registry = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        uint32_t child_slot = 0U;
        uint64_t child_generation = 0U;

        assert(
            mylite_ownerless_process_registry_allocate(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                (uint64_t)getpid(),
                1U,
                0U,
                &child_slot,
                &child_generation
            ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
        );
        assert(child_slot != parent_slot);
        assert(mylite_ownerless_process_registry_active_count(child_registry) == 2U);
        assert(child_generation > parent_generation);
        assert(mylite_ownerless_process_registry_generation(child_registry) == child_generation);
        assert(
            mylite_ownerless_process_registry_release(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                child_slot,
                child_generation
            ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
        );
        assert(munmap(child_registry, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    wait_for_child(child);
    assert(mylite_ownerless_process_registry_active_count(registry) == 1U);
    generation_after_child = mylite_ownerless_process_registry_generation(registry);
    assert(generation_after_child > parent_generation);
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            parent_slot,
            parent_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(mylite_ownerless_process_registry_active_count(registry) == 0U);
    assert(mylite_ownerless_process_registry_generation(registry) > generation_after_child);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_process_registry_rejects_stale_release(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "process-registry-stale.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t slot = 0U;
    uint64_t generation = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_process_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            (uint64_t)getpid(),
            1U,
            0U,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            slot,
            generation + 1U
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND
    );
    assert(mylite_ownerless_process_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            slot,
            generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_process_registry_updates_heartbeat(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "process-registry-heartbeat.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t slot = 0U;
    uint64_t generation = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_process_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            (uint64_t)getpid(),
            1U,
            0U,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_heartbeat(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            slot,
            generation,
            1234U
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_heartbeat(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            slot,
            generation + 1U,
            5678U
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND
    );
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            slot,
            generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_process_registry_cleans_dead_slots(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "process-registry-cleanup.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t live_slot = 0U;
    uint64_t live_generation = 0U;
    uint32_t dead_slot = 0U;
    uint64_t dead_generation = 0U;
    uint32_t cleaned_slots = 0U;
    uint64_t live_pid = 111U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_process_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            live_pid,
            1U,
            0U,
            &live_slot,
            &live_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            222U,
            1U,
            0U,
            &dead_slot,
            &dead_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(mylite_ownerless_process_registry_active_count(registry) == 2U);
    assert(
        mylite_ownerless_process_registry_cleanup_dead(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            process_registry_test_pid_is_alive,
            &live_pid,
            &cleaned_slots
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(cleaned_slots == 1U);
    assert(mylite_ownerless_process_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            dead_slot,
            dead_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND
    );
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            live_slot,
            live_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_process_registry_cleanup_callback_releases_owner_locks(void) {
    char *root = make_temp_root();
    char *registry_path = path_join(root, "process-registry-cleanup-locks.bin");
    char *lock_table_path = path_join(root, "cleanup-lock-table.bin");
    int registry_fd = open_file(registry_path);
    int lock_table_fd = open_file(lock_table_path);
    void *registry;
    void *lock_table;
    cleanup_owner_locks_context cleanup_context;
    uint32_t dead_slot = 0U;
    uint64_t dead_generation = 0U;
    uint32_t cleaned_slots = 0U;

    truncate_file(registry_fd, MYLITE_TEST_PAGE_SIZE);
    truncate_file(lock_table_fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(registry_fd, MYLITE_TEST_PAGE_SIZE);
    lock_table = map_file(lock_table_fd, MYLITE_TEST_PAGE_SIZE);
    cleanup_context.lock_table = lock_table;
    cleanup_context.released_entries = 0U;
    assert(
        mylite_ownerless_process_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_lock_table_initialize(
            lock_table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            222U,
            1U,
            0U,
            &dead_slot,
            &dead_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(dead_slot == 0U);
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            lock_table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            1U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    assert(
        mylite_ownerless_process_registry_cleanup_dead_with_callback(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            process_registry_test_pid_is_alive,
            &(uint64_t){111U},
            process_registry_cleanup_owner_locks,
            &cleanup_context,
            &cleaned_slots
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(cleaned_slots == 1U);
    assert(cleanup_context.released_entries == 1U);
    assert(
        mylite_ownerless_lock_table_acquire_exclusive(
            lock_table,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_LOCK_HASH,
            2U,
            0U
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );

    assert(munmap(lock_table, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(lock_table_fd) == 0);
    assert(close(registry_fd) == 0);
    free(lock_table_path);
    free(registry_path);
    remove_tree(root);
    free(root);
}

static void test_process_registry_cleanup_callback_can_block_cleanup(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "process-registry-cleanup-blocked.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t dead_slot = 0U;
    uint64_t dead_generation = 0U;
    uint32_t cleaned_slots = 0U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_process_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            222U,
            1U,
            0U,
            &dead_slot,
            &dead_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_cleanup_dead_with_callback(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            process_registry_test_pid_is_alive,
            &(uint64_t){111U},
            process_registry_cleanup_blocks_owner,
            NULL,
            &cleaned_slots
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY
    );
    assert(cleaned_slots == 0U);
    assert(mylite_ownerless_process_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            dead_slot,
            dead_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_process_registry_counts_live_slots(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "process-registry-live-count.bin");
    int fd = open_file(shm_path);
    void *registry;
    uint32_t first_slot = 0U;
    uint32_t second_slot = 0U;
    uint64_t first_generation = 0U;
    uint64_t second_generation = 0U;
    uint64_t live_count = 0U;
    const uint64_t live_pid = 111U;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_process_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            live_pid,
            1U,
            0U,
            &first_slot,
            &first_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            222U,
            1U,
            0U,
            &second_slot,
            &second_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_live_count(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            process_registry_test_pid_is_alive,
            (void *)&live_pid,
            &live_count
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(live_count == 1U);
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            first_slot,
            first_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            second_slot,
            second_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static void test_process_registry_cleans_exited_process_slot(void) {
    char *root = make_temp_root();
    char *shm_path = path_join(root, "process-registry-exited.bin");
    int child_ready[2];
    int fd = open_file(shm_path);
    void *registry;
    uint32_t cleaned_slots = 0U;
    pid_t child;

    truncate_file(fd, MYLITE_TEST_PAGE_SIZE);
    registry = map_file(fd, MYLITE_TEST_PAGE_SIZE);
    assert(
        mylite_ownerless_process_registry_initialize(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(pipe(child_ready) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int child_fd;
        void *child_registry;
        uint32_t child_slot = 0U;
        uint64_t child_generation = 0U;

        close(child_ready[0]);
        child_fd = open_file(shm_path);
        child_registry = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        assert(
            mylite_ownerless_process_registry_allocate(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                (uint64_t)getpid(),
                1U,
                0U,
                &child_slot,
                &child_generation
            ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
        );
        assert(mylite_ownerless_process_registry_active_count(child_registry) == 1U);
        signal_pipe(child_ready[1]);
        assert(munmap(child_registry, MYLITE_TEST_PAGE_SIZE) == 0);
        assert(close(child_fd) == 0);
        _exit(0);
    }

    close(child_ready[1]);
    wait_for_pipe(child_ready[0]);
    wait_for_child(child);
    assert(mylite_ownerless_process_registry_active_count(registry) == 1U);
    assert(
        mylite_ownerless_process_registry_cleanup_dead(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            process_registry_pid_is_running,
            NULL,
            &cleaned_slots
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(cleaned_slots == 1U);
    assert(mylite_ownerless_process_registry_active_count(registry) == 0U);

    assert(munmap(registry, MYLITE_TEST_PAGE_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    remove_tree(root);
    free(root);
}

static int process_registry_test_pid_is_alive(uint64_t pid, void *ctx) {
    const uint64_t *live_pid = (const uint64_t *)ctx;

    return pid == *live_pid;
}

static int process_registry_pid_is_running(uint64_t pid, void *ctx) {
    const pid_t probe_pid = (pid_t)pid;

    (void)ctx;

    if (probe_pid <= 0 || (uint64_t)probe_pid != pid) {
        return 0;
    }
    if (kill(probe_pid, 0) == 0) {
        return 1;
    }
    return errno == EPERM;
}

static int dictionary_state_pid_is_alive(uint64_t pid, void *ctx) {
    (void)ctx;
    if (pid > (uint64_t)INT32_MAX) {
        return 0;
    }
    return process_registry_pid_is_running(pid, NULL);
}

static int process_registry_cleanup_owner_locks(
    uint32_t slot_index,
    uint64_t slot_generation,
    uint64_t pid,
    void *ctx
) {
    cleanup_owner_locks_context *cleanup_context = (cleanup_owner_locks_context *)ctx;
    uint32_t released_entries = 0U;

    (void)slot_generation;
    (void)pid;
    assert(
        mylite_ownerless_lock_table_release_owner(
            cleanup_context->lock_table,
            MYLITE_TEST_PAGE_SIZE,
            slot_index + 1U,
            &released_entries
        ) == MYLITE_OWNERLESS_LOCK_TABLE_OK
    );
    cleanup_context->released_entries += released_entries;
    return MYLITE_OWNERLESS_PROCESS_CLEANUP_OK;
}

static int process_registry_cleanup_blocks_owner(
    uint32_t slot_index,
    uint64_t slot_generation,
    uint64_t pid,
    void *ctx
) {
    (void)slot_index;
    (void)slot_generation;
    (void)pid;
    (void)ctx;
    return MYLITE_OWNERLESS_PROCESS_CLEANUP_BLOCKED;
}

static int latch_test_owner_is_alive(uint32_t owner_id, uint64_t owner_generation, void *ctx) {
    const uint32_t *live_owner = (const uint32_t *)ctx;

    (void)owner_generation;
    return owner_id == *live_owner;
}

static void set_write_lock(int fd, byte_range_lock range) {
    assert(try_write_lock(fd, range) == 0);
}

static int try_write_lock(int fd, byte_range_lock range) {
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = range.start,
        .l_len = range.length,
    };

    if (fcntl(fd, F_SETLK, &lock) == 0) {
        return 0;
    }
    return errno;
}

static void unlock_range(int fd, byte_range_lock range) {
    struct flock lock = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = range.start,
        .l_len = range.length,
    };

    assert(fcntl(fd, F_SETLK, &lock) == 0);
}

static int open_file(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    assert(fd >= 0);
    return fd;
}

static void truncate_file(int fd, off_t size) {
    assert(ftruncate(fd, size) == 0);
}

static void write_file_at(int fd, const void *data, size_t size, off_t offset) {
    const uint8_t *bytes = data;
    size_t written = 0;

    while (written < size) {
        ssize_t result = pwrite(fd, bytes + written, size - written, offset + (off_t)written);

        if (result < 0) {
            assert(errno == EINTR);
            continue;
        }
        assert(result > 0);
        written += (size_t)result;
    }
}

static void read_file_at(int fd, void *data, size_t size, off_t offset) {
    uint8_t *bytes = data;
    size_t read_bytes = 0;

    while (read_bytes < size) {
        ssize_t result =
            pread(fd, bytes + read_bytes, size - read_bytes, offset + (off_t)read_bytes);

        if (result < 0) {
            assert(errno == EINTR);
            continue;
        }
        assert(result > 0);
        read_bytes += (size_t)result;
    }
}

static void fill_innodb_test_page(
    uint8_t *page,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint8_t marker
) {
    memset(page, 0, MYLITE_TEST_PAGE_SIZE);
    store_test_be32(page, MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET, page_no);
    store_test_be64(page, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET, page_lsn);
    store_test_be16(
        page,
        MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET,
        MYLITE_TEST_INNODB_PAGE_TYPE_FSP_HEADER
    );
    store_test_be32(page, MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET, space_id);
    page[128] = marker;
}

static uint64_t innodb_test_page_lsn(const uint8_t *page) {
    return load_test_be64(page, MYLITE_TEST_INNODB_PAGE_LSN_OFFSET);
}

static void store_test_be16(uint8_t *bytes, size_t offset, uint16_t value) {
    bytes[offset] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[offset + 1U] = (uint8_t)(value & 0xFFU);
}

static void store_test_be32(uint8_t *bytes, size_t offset, uint32_t value) {
    bytes[offset] = (uint8_t)((value >> 24U) & 0xFFU);
    bytes[offset + 1U] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[offset + 2U] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[offset + 3U] = (uint8_t)(value & 0xFFU);
}

static void store_test_be64(uint8_t *bytes, size_t offset, uint64_t value) {
    for (size_t index = 0; index < 8U; ++index) {
        bytes[offset + index] = (uint8_t)((value >> ((7U - index) * 8U)) & 0xFFU);
    }
}

static void store_test_le64(uint8_t *bytes, size_t offset, uint64_t value) {
    for (size_t index = 0; index < 8U; ++index) {
        bytes[offset + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

static uint32_t load_test_le32(const uint8_t *bytes, size_t offset) {
    return ((uint32_t)bytes[offset]) | ((uint32_t)bytes[offset + 1U] << 8U) |
           ((uint32_t)bytes[offset + 2U] << 16U) | ((uint32_t)bytes[offset + 3U] << 24U);
}

static uint64_t load_test_be64(const uint8_t *bytes, size_t offset) {
    uint64_t value = 0;

    for (size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

static uint32_t read_page_log_record_flags(int fd, uint64_t record_offset) {
    uint8_t bytes[sizeof(uint32_t)] = {0};

    read_file_at(
        fd,
        bytes,
        sizeof(bytes),
        (off_t)(record_offset + MYLITE_TEST_PAGE_LOG_RECORD_FLAGS_OFFSET)
    );
    return load_test_le32(bytes, 0U);
}

static uint64_t legacy_page_log_checksum(const void *buffer, size_t size) {
    const uint8_t *bytes = buffer;
    uint64_t hash = 1469598103934665603ULL;

    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint32_t innodb_lock_registry_occupied_limit(void *registry) {
    uint32_t value = 0U;

    memcpy(
        &value,
        (const unsigned char *)registry + MYLITE_TEST_INNODB_LOCK_REGISTRY_OCCUPIED_LIMIT_OFFSET,
        sizeof(value)
    );
    return value;
}

static void *map_file(int fd, size_t size) {
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    assert(mapping != MAP_FAILED);
    return mapping;
}

static void signal_pipe(int pipe_fd) {
    const char value = 'x';

    assert(write(pipe_fd, &value, sizeof(value)) == sizeof(value));
    assert(close(pipe_fd) == 0);
}

static void wait_for_pipe(int pipe_fd) {
    char value = '\0';

    assert(read(pipe_fd, &value, sizeof(value)) == sizeof(value));
    assert(value == 'x');
    assert(close(pipe_fd) == 0);
}

static void wait_for_child(pid_t child) {
    int child_status = 0;

    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status));
    assert(WEXITSTATUS(child_status) == 0);
}

static void sleep_milliseconds(unsigned milliseconds) {
    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)((milliseconds % 1000U) * 1000000U),
    };

    while (nanosleep(&remaining, &remaining) != 0) {
        assert(errno == EINTR);
    }
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-ownerless-primitives.XXXXXX";
    char *root = mkdtemp(template_path);

    assert(root != NULL);
    return strdup(root);
}

static char *path_join(const char *directory, const char *name) {
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    char *path = malloc(directory_length + name_length + 2);

    assert(path != NULL);
    assert(sprintf(path, "%s/%s", directory, name) > 0);
    return path;
}

static int path_exists(const char *path) {
    struct stat path_stat;

    return stat(path, &path_stat) == 0;
}

static void remove_tree(const char *path) {
    if (!path_exists(path)) {
        return;
    }
    assert(
        nftw(path, remove_tree_entry, MYLITE_TEST_REMOVE_TREE_MAX_FDS, FTW_DEPTH | FTW_PHYS) == 0
    );
}

static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
) {
    (void)path_stat;
    (void)walk;

    if (type_flag == FTW_DP || type_flag == FTW_D) {
        return rmdir(path);
    }
    return unlink(path);
}

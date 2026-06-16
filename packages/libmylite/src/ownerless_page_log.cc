#include "ownerless_page_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#ifndef MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
#  define MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS 0
#endif

#ifndef MYLITE_WITH_MARIADB_EMBEDDED
#  define MYLITE_WITH_MARIADB_EMBEDDED 0
#endif

#if MYLITE_WITH_MARIADB_EMBEDDED
extern "C" std::uint32_t my_crc32c(std::uint32_t crc, const void *buf, std::size_t len);
#endif

namespace {

constexpr std::array<unsigned char, 8> k_header_magic = {
    'M',
    'Y',
    'L',
    'P',
    'A',
    'G',
    'E',
    '\0',
};
constexpr std::array<unsigned char, 8> k_record_magic = {
    'M',
    'Y',
    'L',
    'P',
    'G',
    'R',
    'E',
    'C',
};
constexpr std::uint32_t k_format_version = 1;
constexpr std::size_t k_header_magic_offset = 0;
constexpr std::size_t k_header_format_offset = 8;
constexpr std::size_t k_header_size_offset = 12;
constexpr std::size_t k_header_record_header_size_offset = 16;
constexpr std::size_t k_header_generation_offset = 24;
constexpr std::size_t k_record_magic_offset = 0;
constexpr std::size_t k_record_space_id_offset = 8;
constexpr std::size_t k_record_page_no_offset = 12;
constexpr std::size_t k_record_page_size_offset = 16;
constexpr std::size_t k_record_flags_offset = 20;
constexpr std::size_t k_record_page_lsn_offset = 24;
constexpr std::size_t k_record_commit_lsn_offset = 32;
constexpr std::size_t k_record_payload_size_offset = 40;
constexpr std::size_t k_record_payload_checksum_offset = 48;
constexpr std::uint32_t k_record_flag_trailing_zero_payload = 1U;
constexpr std::uint32_t k_record_flag_sparse_zero_payload = 2U;
constexpr std::uint32_t k_record_flag_compact_sparse_zero_payload = 4U;
constexpr std::uint32_t k_record_flag_varint_compact_sparse_zero_payload = 8U;
constexpr std::uint32_t k_record_flag_fill_sparse_zero_payload = 16U;
constexpr std::uint32_t k_record_flag_index_delta_payload = 32U;
constexpr std::uint32_t k_record_flag_undo_delta_payload = 64U;
constexpr std::uint32_t k_record_flag_snapshot_boundary =
    MYLITE_OWNERLESS_PAGE_LOG_RECORD_SNAPSHOT_BOUNDARY;
constexpr std::uint32_t k_record_flag_external_snapshot_lineage =
    MYLITE_OWNERLESS_PAGE_LOG_RECORD_EXTERNAL_SNAPSHOT_LINEAGE;
constexpr std::uint32_t k_record_encoding_flags =
    k_record_flag_trailing_zero_payload | k_record_flag_sparse_zero_payload |
    k_record_flag_compact_sparse_zero_payload | k_record_flag_varint_compact_sparse_zero_payload |
    k_record_flag_fill_sparse_zero_payload | k_record_flag_index_delta_payload |
    k_record_flag_undo_delta_payload;
constexpr std::uint32_t k_record_metadata_flags =
    k_record_flag_snapshot_boundary | k_record_flag_external_snapshot_lineage;
constexpr std::uint32_t k_record_flags_known_mask =
    k_record_encoding_flags | k_record_metadata_flags;
constexpr unsigned char k_fill_sparse_run_kind_raw = 0U;
constexpr unsigned char k_fill_sparse_run_kind_fill = 1U;
constexpr std::uint32_t k_fill_sparse_min_fill_run_size = 8U;
#if MYLITE_WITH_MARIADB_EMBEDDED
constexpr std::uint32_t k_page_checksum_second_seed = 0xa5a5a5a5U;
#endif
constexpr std::uint64_t k_legacy_checksum_offset_basis = 1469598103934665603ULL;
constexpr std::uint64_t k_legacy_checksum_prime = 1099511628211ULL;
constexpr std::size_t k_checksum_stream_chunk_size = 4096U;
constexpr std::size_t k_innodb_fil_page_type_offset = 24;
constexpr std::size_t k_innodb_fil_page_data_offset = 38;
constexpr std::uint32_t k_innodb_system_space_id = 0;
constexpr std::uint16_t k_innodb_fil_page_index = 17855;
constexpr std::uint16_t k_innodb_fil_page_type_allocated = 0;
constexpr std::uint16_t k_innodb_fil_page_undo_log = 2;
constexpr std::uint16_t k_innodb_fil_page_inode = 3;
constexpr std::uint16_t k_innodb_fil_page_ibuf_free_list = 4;
constexpr std::uint16_t k_innodb_fil_page_ibuf_bitmap = 5;
constexpr std::uint16_t k_innodb_fil_page_type_sys = 6;
constexpr std::uint16_t k_innodb_fil_page_type_trx_sys = 7;
constexpr std::uint16_t k_innodb_fil_page_type_fsp_hdr = 8;
constexpr std::uint16_t k_innodb_fil_page_type_xdes = 9;
constexpr std::uint16_t k_innodb_fil_page_type_blob = 10;
constexpr std::uint16_t k_innodb_fil_page_type_zblob = 11;
constexpr std::uint16_t k_innodb_fil_page_type_zblob2 = 12;
constexpr std::size_t k_index_page_identity_slot_count = 1024;
constexpr std::size_t k_index_page_identity_probe_limit = 8;
constexpr std::size_t k_index_delta_base_slot_count = 1024;
constexpr std::size_t k_index_delta_base_probe_limit = 8;
constexpr std::size_t k_index_delta_base_record_offset_size = sizeof(std::uint64_t);
constexpr std::uint32_t k_index_delta_base_min_standalone_observations = 1;
constexpr std::uint32_t k_index_delta_base_max_delta_records = 32;
constexpr std::uint64_t k_index_delta_fast_payload_size_limit = 2048;
constexpr off_t k_append_lock_start = 0;
constexpr off_t k_checkpoint_lock_start = 1;

using PageLogHeader = std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE>;

struct PageRecordHeader {
    std::uint32_t space_id = 0;
    std::uint32_t page_no = 0;
    std::uint32_t page_size = 0;
    std::uint32_t flags = 0;
    std::uint64_t page_lsn = 0;
    std::uint64_t commit_lsn = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t checksum = 0;
};

struct ScannedPageRecord {
    off_t record_offset = 0;
    off_t payload_offset = 0;
    off_t next_record_offset = 0;
    PageRecordHeader record = {};
    bool requires_snapshot_boundary = true;
};

struct PageRetentionState {
    bool has_after_oldest_checkpointed_record = false;
    bool has_boundary_record = false;
    off_t boundary_record_offset = 0;
    PageRecordHeader boundary_record = {};
};

struct CompactSparsePayloadComposition {
    bool valid = false;
    std::uint64_t metadata_bytes = 0;
    std::uint64_t data_bytes = 0;
};

struct IndexPageIdentitySlot {
    bool valid = false;
    std::uint32_t space_id = 0;
    std::uint32_t page_no = 0;
    std::uint32_t page_size = 0;
    std::vector<unsigned char> page;
};

struct IndexPageDeltaBaseSlot {
    bool valid = false;
    std::uint64_t log_device = 0;
    std::uint64_t log_inode = 0;
    std::uint64_t log_offset = 0;
    std::uint64_t log_generation = 0;
    std::uint32_t delta_flag = 0;
    std::uint32_t space_id = 0;
    std::uint32_t page_no = 0;
    std::uint32_t page_size = 0;
    std::uint64_t record_offset = 0;
    std::uint64_t standalone_payload_size = 0;
    std::uint32_t standalone_observations = 0;
    std::uint32_t delta_records_since_base = 0;
    std::shared_ptr<const std::vector<unsigned char>> page;
};

struct IndexPageDeltaBaseSnapshot {
    bool found = false;
    std::uint32_t delta_flag = 0;
    std::uint64_t record_offset = 0;
    std::uint64_t standalone_payload_size = 0;
    std::shared_ptr<const std::vector<unsigned char>> page;
};

struct IndexPageDeltaRun {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

struct PageChecksumAccumulator {
#if MYLITE_WITH_MARIADB_EMBEDDED
    std::uint32_t low = 0;
    std::uint32_t high = k_page_checksum_second_seed;
#endif
    std::uint64_t legacy = k_legacy_checksum_offset_basis;
    std::uint64_t bytes = 0;
};

enum PageLogAppendPerfStatIndex : std::size_t {
    PAGE_LOG_APPEND_PERF_CALLS = 0,
    PAGE_LOG_APPEND_PERF_TOTAL_NS,
    PAGE_LOG_APPEND_PERF_LOCK_NS,
    PAGE_LOG_APPEND_PERF_HEADER_NS,
    PAGE_LOG_APPEND_PERF_BODY_NS,
    PAGE_LOG_APPEND_PERF_FSTAT_NS,
    PAGE_LOG_APPEND_PERF_CHECKSUM_NS,
    PAGE_LOG_APPEND_PERF_PAYLOAD_WRITE_NS,
    PAGE_LOG_APPEND_PERF_RECORD_HEADER_WRITE_NS,
    PAGE_LOG_APPEND_PERF_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_RECORD_HEADER_BYTES,
    PAGE_LOG_APPEND_PERF_ENCODE_NS,
    PAGE_LOG_APPEND_PERF_FULL_RECORDS,
    PAGE_LOG_APPEND_PERF_TRAILING_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_SPARSE_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_FULL_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_TRAILING_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_SPARSE_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_COMPACT_SPARSE_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_INDEX_RECORDS,
    PAGE_LOG_APPEND_PERF_INDEX_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_UNDO_LOG_RECORDS,
    PAGE_LOG_APPEND_PERF_UNDO_LOG_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_SYS_RECORDS,
    PAGE_LOG_APPEND_PERF_SYS_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_TRX_SYS_RECORDS,
    PAGE_LOG_APPEND_PERF_TRX_SYS_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_SPACE_METADATA_RECORDS,
    PAGE_LOG_APPEND_PERF_SPACE_METADATA_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_BLOB_RECORDS,
    PAGE_LOG_APPEND_PERF_BLOB_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_OTHER_RECORDS,
    PAGE_LOG_APPEND_PERF_OTHER_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_INDEX_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_INDEX_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_UNDO_LOG_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_UNDO_LOG_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_SYS_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_SYS_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_TRX_SYS_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_TRX_SYS_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_SPACE_METADATA_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_SPACE_METADATA_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_BLOB_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_BLOB_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_OTHER_COMPACT_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_OTHER_COMPACT_SPARSE_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_VARINT_COMPACT_SPARSE_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_VARINT_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_FILL_SPARSE_ZERO_RECORDS,
    PAGE_LOG_APPEND_PERF_FILL_SPARSE_ZERO_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_FILL_SPARSE_METADATA_BYTES,
    PAGE_LOG_APPEND_PERF_FILL_SPARSE_RAW_DATA_BYTES,
    PAGE_LOG_APPEND_PERF_FILL_SPARSE_FILL_BYTES,
    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_UNIQUE,
    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_DUPLICATE,
    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_SIZE_MISMATCH,
    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_TABLE_OVERFLOW,
    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_CHANGED_BYTES,
    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_FIL_HEADER_CHANGED_BYTES,
    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_BODY_CHANGED_BYTES,
    PAGE_LOG_APPEND_PERF_INDEX_DELTA_RECORDS,
    PAGE_LOG_APPEND_PERF_INDEX_DELTA_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_DIRECT_APPEND_CALLS,
    PAGE_LOG_APPEND_PERF_SESSION_BEGIN_CALLS,
    PAGE_LOG_APPEND_PERF_SESSION_APPEND_CALLS,
    PAGE_LOG_APPEND_PERF_SESSION_END_CALLS,
    PAGE_LOG_APPEND_PERF_INDEX_DELTA_FAST_RECORDS,
    PAGE_LOG_APPEND_PERF_UNDO_DELTA_RECORDS,
    PAGE_LOG_APPEND_PERF_UNDO_DELTA_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_UNDO_DELTA_FAST_RECORDS,
    PAGE_LOG_APPEND_PERF_DELTA_SNAPSHOT_NS,
    PAGE_LOG_APPEND_PERF_DELTA_ENCODE_NS,
    PAGE_LOG_APPEND_PERF_STANDALONE_ENCODE_NS,
    PAGE_LOG_APPEND_PERF_PAYLOAD_STATS_NS,
    PAGE_LOG_APPEND_PERF_PAGE_TYPE_STATS_NS,
    PAGE_LOG_APPEND_PERF_DELTA_BASE_NOTE_NS,
    PAGE_LOG_APPEND_PERF_INDEX_DELTA_FAST_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_INDEX_DELTA_EXACT_RECORDS,
    PAGE_LOG_APPEND_PERF_INDEX_DELTA_EXACT_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_UNDO_DELTA_FAST_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_UNDO_DELTA_EXACT_RECORDS,
    PAGE_LOG_APPEND_PERF_UNDO_DELTA_EXACT_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_LIMIT_RECORDS,
    PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_LIMIT_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_STANDALONE_RECORDS,
    PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_STANDALONE_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_BUILD_FAILURES,
    PAGE_LOG_APPEND_PERF_DELTA_EXACT_REJECTED_STANDALONE_RECORDS,
    PAGE_LOG_APPEND_PERF_DELTA_EXACT_REJECTED_STANDALONE_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_DELTA_EXACT_REJECTED_BUILD_FAILURES,
    PAGE_LOG_APPEND_PERF_DELTA_EXACT_REUSED_FAST_PAYLOAD_RECORDS,
    PAGE_LOG_APPEND_PERF_DELTA_EXACT_REUSED_FAST_PAYLOAD_BYTES,
    PAGE_LOG_APPEND_PERF_STANDALONE_SIZE_PROBE_CALLS,
    PAGE_LOG_APPEND_PERF_STANDALONE_SIZE_PROBE_NS,
    PAGE_LOG_APPEND_PERF_STANDALONE_MATERIALIZE_SKIPPED_RECORDS,
    PAGE_LOG_APPEND_PERF_STANDALONE_MATERIALIZE_SKIPPED_BYTES,
    PAGE_LOG_APPEND_PERF_STAT_COUNT
};

enum class PageDeltaEncodeDecision { Ineligible, Encoded, BuildFailed, FastLimit, Standalone };

enum PageLogScanPerfStatIndex : std::size_t {
    PAGE_LOG_SCAN_PERF_CALLS = 0,
    PAGE_LOG_SCAN_PERF_RECORD_HEADERS,
    PAGE_LOG_SCAN_PERF_PAGE_RECORDS,
    PAGE_LOG_SCAN_PERF_VISIBLE_PAGE_RECORDS,
    PAGE_LOG_SCAN_PERF_FOUND,
    PAGE_LOG_SCAN_PERF_NOT_FOUND_NO_PAGE_RECORD,
    PAGE_LOG_SCAN_PERF_NOT_FOUND_PAGE_RECORD_NOT_VISIBLE,
    PAGE_LOG_SCAN_PERF_ERRORS,
    PAGE_LOG_SCAN_PERF_STREAM_CHECKSUM_RECORDS,
    PAGE_LOG_SCAN_PERF_STREAM_CHECKSUM_BYTES,
    PAGE_LOG_SCAN_PERF_STAT_COUNT
};

enum PageLogSyncPerfStatIndex : std::size_t {
    PAGE_LOG_SYNC_PERF_CALLS = 0,
    PAGE_LOG_SYNC_PERF_TOTAL_NS,
    PAGE_LOG_SYNC_PERF_LOCK_NS,
    PAGE_LOG_SYNC_PERF_HEADER_NS,
    PAGE_LOG_SYNC_PERF_DATA_SYNC_NS,
    PAGE_LOG_SYNC_PERF_SKIPPED_CLEAN,
    PAGE_LOG_SYNC_PERF_STAT_COUNT
};

std::atomic<bool> page_log_append_perf_stats_enabled{false};
std::atomic<bool> page_log_append_detail_perf_stats_enabled{false};
std::atomic<std::uint64_t> page_log_append_perf_stats[PAGE_LOG_APPEND_PERF_STAT_COUNT];
std::atomic<bool> page_log_scan_perf_stats_enabled{false};
std::atomic<std::uint64_t> page_log_scan_perf_stats[PAGE_LOG_SCAN_PERF_STAT_COUNT];
std::atomic<bool> page_log_sync_perf_stats_enabled{false};
std::atomic<std::uint64_t> page_log_sync_perf_stats[PAGE_LOG_SYNC_PERF_STAT_COUNT];
std::mutex index_page_identity_stats_mutex;
std::array<IndexPageIdentitySlot, k_index_page_identity_slot_count> index_page_identity_slots;
std::mutex index_page_delta_base_mutex;
std::array<IndexPageDeltaBaseSlot, k_index_delta_base_slot_count> index_page_delta_base_slots;

bool page_log_append_perf_stats_are_enabled() {
    return page_log_append_perf_stats_enabled.load(std::memory_order_relaxed);
}

bool page_log_append_detail_perf_stats_are_enabled(bool append_stats_enabled) {
    return append_stats_enabled &&
           page_log_append_detail_perf_stats_enabled.load(std::memory_order_relaxed);
}

bool page_log_append_detail_perf_stats_are_enabled() {
    return page_log_append_detail_perf_stats_are_enabled(page_log_append_perf_stats_are_enabled());
}

bool page_log_scan_perf_stats_are_enabled() {
    return page_log_scan_perf_stats_enabled.load(std::memory_order_relaxed);
}

bool page_log_sync_perf_stats_are_enabled() {
    return page_log_sync_perf_stats_enabled.load(std::memory_order_relaxed);
}

std::uint64_t page_log_append_perf_now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

void page_log_scan_perf_add(PageLogScanPerfStatIndex index, std::uint64_t value) {
    if (page_log_scan_perf_stats_are_enabled()) {
        page_log_scan_perf_stats[index].fetch_add(value, std::memory_order_relaxed);
    }
}

void page_log_append_perf_add(PageLogAppendPerfStatIndex index, std::uint64_t value) {
    if (page_log_append_perf_stats_are_enabled()) {
        page_log_append_perf_stats[index].fetch_add(value, std::memory_order_relaxed);
    }
}

void page_log_append_perf_add_if_enabled(
    bool stats_enabled,
    PageLogAppendPerfStatIndex index,
    std::uint64_t value
) {
    if (stats_enabled) {
        page_log_append_perf_stats[index].fetch_add(value, std::memory_order_relaxed);
    }
}

void page_log_append_perf_add_delta_accepted_record(
    std::uint32_t delta_flag,
    std::uint64_t payload_size,
    bool fast
) {
    if (delta_flag == k_record_flag_index_delta_payload) {
        if (fast) {
            page_log_append_perf_add(PAGE_LOG_APPEND_PERF_INDEX_DELTA_FAST_RECORDS, 1U);
            page_log_append_perf_add(
                PAGE_LOG_APPEND_PERF_INDEX_DELTA_FAST_PAYLOAD_BYTES,
                payload_size
            );
        } else {
            page_log_append_perf_add(PAGE_LOG_APPEND_PERF_INDEX_DELTA_EXACT_RECORDS, 1U);
            page_log_append_perf_add(
                PAGE_LOG_APPEND_PERF_INDEX_DELTA_EXACT_PAYLOAD_BYTES,
                payload_size
            );
        }
    } else if (delta_flag == k_record_flag_undo_delta_payload) {
        if (fast) {
            page_log_append_perf_add(PAGE_LOG_APPEND_PERF_UNDO_DELTA_FAST_RECORDS, 1U);
            page_log_append_perf_add(
                PAGE_LOG_APPEND_PERF_UNDO_DELTA_FAST_PAYLOAD_BYTES,
                payload_size
            );
        } else {
            page_log_append_perf_add(PAGE_LOG_APPEND_PERF_UNDO_DELTA_EXACT_RECORDS, 1U);
            page_log_append_perf_add(
                PAGE_LOG_APPEND_PERF_UNDO_DELTA_EXACT_PAYLOAD_BYTES,
                payload_size
            );
        }
    }
}

void page_log_append_perf_add_delta_rejection(
    PageDeltaEncodeDecision decision,
    std::uint64_t delta_payload_size,
    bool fast
) {
    if (fast) {
        if (decision == PageDeltaEncodeDecision::FastLimit) {
            page_log_append_perf_add(PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_LIMIT_RECORDS, 1U);
            page_log_append_perf_add(
                PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_LIMIT_PAYLOAD_BYTES,
                delta_payload_size
            );
        } else if (decision == PageDeltaEncodeDecision::Standalone) {
            page_log_append_perf_add(
                PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_STANDALONE_RECORDS,
                1U
            );
            page_log_append_perf_add(
                PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_STANDALONE_PAYLOAD_BYTES,
                delta_payload_size
            );
        } else if (decision == PageDeltaEncodeDecision::BuildFailed) {
            page_log_append_perf_add(PAGE_LOG_APPEND_PERF_DELTA_FAST_REJECTED_BUILD_FAILURES, 1U);
        }
        return;
    }

    if (decision == PageDeltaEncodeDecision::Standalone) {
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_DELTA_EXACT_REJECTED_STANDALONE_RECORDS, 1U);
        page_log_append_perf_add(
            PAGE_LOG_APPEND_PERF_DELTA_EXACT_REJECTED_STANDALONE_PAYLOAD_BYTES,
            delta_payload_size
        );
    } else if (decision == PageDeltaEncodeDecision::BuildFailed) {
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_DELTA_EXACT_REJECTED_BUILD_FAILURES, 1U);
    }
}

void page_log_append_perf_add_elapsed_if_enabled(
    bool stats_enabled,
    PageLogAppendPerfStatIndex index,
    std::uint64_t start_ns
) {
    if (start_ns != 0U) {
        page_log_append_perf_add_if_enabled(
            stats_enabled,
            index,
            page_log_append_perf_now_ns() - start_ns
        );
    }
}

void page_log_sync_perf_add(PageLogSyncPerfStatIndex index, std::uint64_t value) {
    if (page_log_sync_perf_stats_are_enabled()) {
        page_log_sync_perf_stats[index].fetch_add(value, std::memory_order_relaxed);
    }
}

void page_log_sync_perf_add_elapsed(PageLogSyncPerfStatIndex index, std::uint64_t start_ns) {
    if (start_ns != 0U) {
        page_log_sync_perf_add(index, page_log_append_perf_now_ns() - start_ns);
    }
}

class PageLogAppendPerfScope {
  public:
    explicit PageLogAppendPerfScope(PageLogAppendPerfStatIndex index)
        : PageLogAppendPerfScope(index, page_log_append_perf_stats_are_enabled()) {}

    PageLogAppendPerfScope(PageLogAppendPerfStatIndex index, bool stats_enabled)
        : index_(index), stats_enabled_(stats_enabled),
          start_ns_(stats_enabled ? page_log_append_perf_now_ns() : 0U) {}

    ~PageLogAppendPerfScope() {
        page_log_append_perf_add_elapsed_if_enabled(stats_enabled_, index_, start_ns_);
    }

    PageLogAppendPerfScope(const PageLogAppendPerfScope &) = delete;
    PageLogAppendPerfScope &operator=(const PageLogAppendPerfScope &) = delete;

  private:
    PageLogAppendPerfStatIndex index_;
    bool stats_enabled_;
    std::uint64_t start_ns_;
};

class PageLogSyncPerfScope {
  public:
    explicit PageLogSyncPerfScope(PageLogSyncPerfStatIndex index)
        : index_(index),
          start_ns_(page_log_sync_perf_stats_are_enabled() ? page_log_append_perf_now_ns() : 0U) {}

    ~PageLogSyncPerfScope() {
        page_log_sync_perf_add_elapsed(index_, start_ns_);
    }

    PageLogSyncPerfScope(const PageLogSyncPerfScope &) = delete;
    PageLogSyncPerfScope &operator=(const PageLogSyncPerfScope &) = delete;

  private:
    PageLogSyncPerfStatIndex index_;
    std::uint64_t start_ns_;
};

enum class PayloadStatus {
    Ok,
    Mismatch,
    Error,
};

int validate_or_create_header(int fd, off_t log_offset);
int validate_existing_header(int fd, off_t log_offset);
int validate_existing_header_size(int fd, off_t log_offset);
int sync_at_common(int fd, std::uint64_t log_offset, bool validate_header);
int append_locked(
    int fd,
    off_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset,
    std::uint32_t extra_record_flags
);
int append_record_at_locked(
    int fd,
    off_t log_offset,
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_generation,
    off_t record_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset,
    std::uint64_t *out_next_record_offset,
    std::uint32_t extra_record_flags,
    bool append_stats_enabled,
    bool append_detail_stats_enabled
);
int snapshot_locked(int fd, off_t log_offset, std::uint64_t *out_snapshot_end_offset);
int snapshot_locked_with_generation(
    int fd,
    off_t log_offset,
    std::uint64_t *out_snapshot_end_offset,
    std::uint64_t *out_log_generation
);
int snapshot_under_read_lock(int fd, off_t log_offset, std::uint64_t *out_snapshot_end_offset);
int snapshot_under_read_lock_with_generation(
    int fd,
    off_t log_offset,
    std::uint64_t *out_snapshot_end_offset,
    std::uint64_t *out_log_generation
);
int find_latest_in_snapshot(
    int fd,
    off_t log_offset,
    off_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags
);
int find_latest_in_snapshot_range(
    int fd,
    off_t log_offset,
    off_t scan_start_offset,
    off_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags,
    int *out_saw_page_record
);
int replay_in_snapshot(
    int fd,
    off_t log_offset,
    off_t snapshot_end_offset,
    mylite_ownerless_page_log_replay_callback callback,
    void *context
);
int read_record_at_locked(
    int fd,
    off_t log_offset,
    off_t physical_record_offset,
    bool require_page_identity,
    std::uint32_t space_id,
    std::uint32_t page_no,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags
);
int checkpoint_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int checkpoint_preserving_oldest_snapshot_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    std::uint64_t oldest_snapshot_lsn,
    bool retain_checkpointed_snapshot_records_after_oldest,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_prepare_callback prepare_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int checkpoint_preserving_oldest_snapshot_at_common(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    std::uint64_t oldest_snapshot_lsn,
    bool retain_checkpointed_snapshot_records_after_oldest,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_prepare_callback prepare_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int checkpoint_if_safe_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    int *out_checkpointed
);
std::uint64_t page_key(std::uint32_t space_id, std::uint32_t page_no);
bool acquire_append_lock(int fd);
bool acquire_snapshot_lock(int fd);
bool acquire_checkpoint_read_lock(int fd);
bool acquire_checkpoint_write_lock(int fd);
bool acquire_log_lock(int fd, short lock_type, off_t lock_start);
void release_log_lock(int fd, off_t lock_start);
void maybe_pause_for_test_fault(const char *fault_name);
bool read_header(int fd, off_t log_offset, PageLogHeader &header);
bool write_header(int fd, off_t log_offset);
std::uint64_t header_generation(const PageLogHeader &header);
bool increment_header_generation(int fd, off_t log_offset);
bool header_matches(const PageLogHeader &header);
bool read_record_header(int fd, off_t offset, PageRecordHeader &header);
bool write_record_header(int fd, off_t offset, const PageRecordHeader &header);
bool write_exact_at(int fd, const void *buffer, std::size_t size, off_t offset);
bool read_exact_at(int fd, void *buffer, std::size_t size, off_t offset);
bool sync_file(int fd);
bool sync_file_data(int fd);
bool next_io_offset(off_t offset, std::size_t progress, off_t *out_offset);
bool offset_adds(off_t offset, std::uint64_t length, off_t *out_offset);
std::uint64_t encoded_payload_size_for_page(
    const void *page,
    std::uint32_t page_size,
    std::uint32_t *out_flags,
    std::vector<unsigned char> *out_payload
);
std::uint64_t encoded_payload_size_for_page_probe(const void *page, std::uint32_t page_size);
bool record_uses_trailing_zero_payload(const PageRecordHeader &record);
bool record_uses_sparse_zero_payload(const PageRecordHeader &record);
bool record_uses_compact_sparse_zero_payload(const PageRecordHeader &record);
bool record_uses_varint_compact_sparse_zero_payload(const PageRecordHeader &record);
bool record_uses_fill_sparse_zero_payload(const PageRecordHeader &record);
bool record_uses_index_delta_payload(const PageRecordHeader &record);
bool record_uses_undo_delta_payload(const PageRecordHeader &record);
bool record_uses_any_delta_payload(const PageRecordHeader &record);
bool record_uses_any_sparse_zero_payload(const PageRecordHeader &record);
bool record_payload_shape_valid(const PageRecordHeader &record);
bool page_is_innodb_index_page(const void *page, std::uint32_t page_size);
bool page_is_innodb_undo_log_page(const void *page, std::uint32_t page_size);
bool page_delta_flag_for_page(
    std::uint32_t space_id,
    const void *page,
    std::uint32_t page_size,
    std::uint32_t *out_delta_flag
);
CompactSparsePayloadComposition compact_sparse_payload_composition(
    std::uint32_t flags,
    const std::vector<unsigned char> &payload
);
CompactSparsePayloadComposition fill_sparse_payload_composition(
    const std::vector<unsigned char> &payload
);
CompactSparsePayloadComposition record_append_payload_encoding_stats(
    std::uint32_t flags,
    std::uint64_t payload_size,
    const std::vector<unsigned char> &payload
);
void record_append_page_type_stats(
    std::uint32_t space_id,
    std::uint32_t page_no,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t payload_size,
    const CompactSparsePayloadComposition &compact_sparse
);
void record_append_index_page_identity_stats(
    std::uint32_t space_id,
    std::uint32_t page_no,
    const unsigned char *page,
    std::uint32_t page_size
);
std::uint64_t index_page_identity_fingerprint(std::uint32_t space_id, std::uint32_t page_no);
std::uint64_t mix64(std::uint64_t value);
bool maybe_encode_page_delta_payload(
    const IndexPageDeltaBaseSnapshot &snapshot,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t standalone_payload_size,
    bool enforce_fast_limit,
    std::uint32_t *inout_flags,
    std::vector<unsigned char> *inout_payload,
    PageDeltaEncodeDecision *out_decision,
    std::uint64_t *out_delta_payload_size,
    std::vector<unsigned char> *out_rejected_payload
);
bool index_delta_base_snapshot_for_page(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset,
    std::uint64_t log_generation,
    std::uint32_t space_id,
    std::uint32_t page_no,
    const void *page,
    std::uint32_t page_size,
    IndexPageDeltaBaseSnapshot *out_snapshot
);
bool index_delta_payload_beats_standalone(
    std::size_t delta_payload_size,
    std::uint64_t standalone_payload_size
);
bool delta_pages_word_equal(
    const unsigned char *base_page,
    const unsigned char *page,
    std::uint32_t offset
);
bool build_index_delta_payload(
    std::uint64_t base_record_offset,
    const std::vector<unsigned char> &base_page,
    const unsigned char *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload
);
bool index_delta_base_snapshot(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset,
    std::uint64_t log_generation,
    std::uint32_t delta_flag,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t page_size,
    IndexPageDeltaBaseSnapshot *out_snapshot
);
void note_index_delta_base_after_successful_append(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset,
    std::uint64_t log_generation,
    std::uint32_t space_id,
    std::uint32_t page_no,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t record_offset,
    std::uint64_t record_payload_size,
    std::uint32_t record_flags,
    std::uint64_t observed_standalone_payload_size
);
std::uint64_t index_delta_base_fingerprint(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset,
    std::uint64_t log_generation,
    std::uint32_t delta_flag,
    std::uint32_t space_id,
    std::uint32_t page_no
);
void invalidate_index_delta_bases_for_log(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset
);
bool decode_page_delta_payload(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    void *out_page,
    std::size_t page_capacity
);
bool read_standalone_or_rewrite_delta_payload(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    PageRecordHeader *out_record,
    std::vector<unsigned char> *out_payload
);
bool record_page_too_large(const PageRecordHeader &record, std::size_t page_capacity);
bool read_record_page_payload(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    void *out_page,
    std::size_t page_capacity
);
bool read_non_delta_record_page_payload(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    void *out_page,
    std::size_t page_capacity
);
bool read_record_page_type(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    std::uint16_t *out_page_type
);
void checksum_accumulator_update(
    PageChecksumAccumulator *inout_checksum,
    const void *buffer,
    std::size_t size
);
bool checksum_accumulator_update_file(
    PageChecksumAccumulator *inout_checksum,
    int fd,
    off_t offset,
    std::uint64_t size
);
void checksum_accumulator_update_repeated(
    PageChecksumAccumulator *inout_checksum,
    unsigned char byte,
    std::uint64_t size
);
bool checksum_accumulator_matches(
    const PageChecksumAccumulator &checksum,
    std::uint64_t expected_checksum
);
std::uint64_t trailing_zero_payload_size_for_page(const void *page, std::uint32_t page_size);
bool sparse_zero_payload_size_for_page(
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_size,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
);
bool compact_sparse_zero_payload_size_for_page(
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_size,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
);
bool fill_sparse_zero_payload_size_for_page(
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_size
);
bool build_sparse_zero_payload(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
);
bool build_compact_sparse_zero_payload(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known,
    bool *out_uses_varint_payload
);
bool compact_sparse_payload_fill_sparse_size(
    const std::vector<unsigned char> &payload,
    bool varint_payload,
    std::uint64_t *out_size
);
bool fill_sparse_encoded_run_size(
    std::uint64_t *inout_size,
    std::uint32_t gap,
    std::uint32_t run_size,
    bool fill_run
);
std::uint32_t varuint16_encoded_size(std::uint32_t value);
bool build_fill_sparse_zero_payload(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload
);
bool build_fill_sparse_zero_payload_if_smaller_than_compact(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
);
bool build_fill_sparse_zero_payload_internal(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload,
    bool require_smaller_than_compact,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
);
bool append_fill_sparse_run(
    std::vector<unsigned char> *out_payload,
    std::uint32_t gap,
    std::uint32_t run_size,
    unsigned char kind,
    const unsigned char *raw_bytes,
    unsigned char fill_byte
);
bool append_varuint16(std::vector<unsigned char> *out_payload, std::uint32_t value);
bool read_varuint16(
    const unsigned char *payload,
    std::size_t payload_size,
    std::size_t *inout_cursor,
    std::uint32_t *out_value
);
bool read_varuint16_at(
    int fd,
    off_t payload_offset,
    std::size_t payload_size,
    std::size_t *inout_cursor,
    std::uint32_t *out_value
);
PayloadStatus record_payload_status(int fd, off_t payload_offset, const PageRecordHeader &record);
PayloadStatus stream_non_delta_record_payload_status(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record
);
bool record_requires_oldest_snapshot_boundary(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record
);
bool record_page_type_is_native_support_state(std::uint16_t page_type);
bool record_checksum_matches(const void *page, std::uint64_t page_size, std::uint64_t checksum);
bool record_is_better(const PageRecordHeader &candidate, const PageRecordHeader &current);
std::uint64_t checksum_bytes(const void *buffer, std::size_t size);
std::uint64_t legacy_checksum_bytes(const void *buffer, std::size_t size);
std::uint16_t load_be16(const unsigned char *bytes);
std::uint16_t load16(const unsigned char *bytes, std::size_t offset);
std::uint32_t load32(const unsigned char *bytes, std::size_t offset);
std::uint64_t load64(const unsigned char *bytes, std::size_t offset);
void store16(unsigned char *bytes, std::size_t offset, std::uint16_t value);
void store32(unsigned char *bytes, std::size_t offset, std::uint32_t value);
void store64(unsigned char *bytes, std::size_t offset, std::uint64_t value);

} // namespace

extern "C" void mylite_ownerless_page_log_set_append_perf_stats_enabled(int enabled) {
    const bool is_enabled = enabled != 0;
    page_log_append_perf_stats_enabled.store(is_enabled, std::memory_order_relaxed);
    page_log_append_detail_perf_stats_enabled.store(is_enabled, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_page_log_set_append_detail_perf_stats_enabled(int enabled) {
    page_log_append_detail_perf_stats_enabled.store(enabled != 0, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_page_log_reset_append_perf_stats(void) {
    for (std::size_t i = 0; i < PAGE_LOG_APPEND_PERF_STAT_COUNT; ++i) {
        page_log_append_perf_stats[i].store(0, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> guard(index_page_identity_stats_mutex);
    for (IndexPageIdentitySlot &slot : index_page_identity_slots) {
        slot.valid = false;
        slot.space_id = 0;
        slot.page_no = 0;
        slot.page_size = 0;
        slot.page.clear();
    }
}

extern "C" void mylite_ownerless_page_log_read_append_perf_stats(
    std::uint64_t *out_values,
    std::size_t value_count
) {
    if (out_values == nullptr || value_count == 0U) {
        return;
    }
    const std::size_t copy_count = value_count < PAGE_LOG_APPEND_PERF_STAT_COUNT
                                       ? value_count
                                       : PAGE_LOG_APPEND_PERF_STAT_COUNT;
    for (std::size_t i = 0; i < copy_count; ++i) {
        out_values[i] = page_log_append_perf_stats[i].load(std::memory_order_relaxed);
    }
}

extern "C" void mylite_ownerless_page_log_set_scan_perf_stats_enabled(int enabled) {
    page_log_scan_perf_stats_enabled.store(enabled != 0, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_page_log_reset_scan_perf_stats(void) {
    for (std::size_t i = 0; i < PAGE_LOG_SCAN_PERF_STAT_COUNT; ++i) {
        page_log_scan_perf_stats[i].store(0, std::memory_order_relaxed);
    }
}

extern "C" void mylite_ownerless_page_log_read_scan_perf_stats(
    std::uint64_t *out_values,
    std::size_t value_count
) {
    if (out_values == nullptr || value_count == 0U) {
        return;
    }
    const std::size_t copy_count =
        value_count < PAGE_LOG_SCAN_PERF_STAT_COUNT ? value_count : PAGE_LOG_SCAN_PERF_STAT_COUNT;
    for (std::size_t i = 0; i < copy_count; ++i) {
        out_values[i] = page_log_scan_perf_stats[i].load(std::memory_order_relaxed);
    }
}

extern "C" void mylite_ownerless_page_log_set_sync_perf_stats_enabled(int enabled) {
    page_log_sync_perf_stats_enabled.store(enabled != 0, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_page_log_reset_sync_perf_stats(void) {
    for (std::size_t i = 0; i < PAGE_LOG_SYNC_PERF_STAT_COUNT; ++i) {
        page_log_sync_perf_stats[i].store(0, std::memory_order_relaxed);
    }
}

extern "C" void mylite_ownerless_page_log_read_sync_perf_stats(
    std::uint64_t *out_values,
    std::size_t value_count
) {
    if (out_values == nullptr || value_count == 0U) {
        return;
    }
    const std::size_t copy_count =
        value_count < PAGE_LOG_SYNC_PERF_STAT_COUNT ? value_count : PAGE_LOG_SYNC_PERF_STAT_COUNT;
    for (std::size_t i = 0; i < copy_count; ++i) {
        out_values[i] = page_log_sync_perf_stats[i].load(std::memory_order_relaxed);
    }
}

int mylite_ownerless_page_log_initialize(int fd) {
    return mylite_ownerless_page_log_initialize_at(fd, 0U);
}

int mylite_ownerless_page_log_initialize_at(int fd, std::uint64_t log_offset) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int result = validate_or_create_header(fd, static_cast<off_t>(log_offset));
    if (result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        struct stat file_stat = {};
        if (::fstat(fd, &file_stat) == 0) {
            invalidate_index_delta_bases_for_log(
                static_cast<std::uint64_t>(file_stat.st_dev),
                static_cast<std::uint64_t>(file_stat.st_ino),
                log_offset
            );
        }
    }
    release_log_lock(fd, k_append_lock_start);
    return result;
}

namespace {

int append_at_common(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset,
    bool validate_header,
    std::uint32_t extra_record_flags
) {
    const bool append_stats_enabled = page_log_append_perf_stats_are_enabled();
    PageLogAppendPerfScope total_scope(PAGE_LOG_APPEND_PERF_TOTAL_NS, append_stats_enabled);
    page_log_append_perf_add_if_enabled(append_stats_enabled, PAGE_LOG_APPEND_PERF_CALLS, 1U);
    page_log_append_perf_add_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_DIRECT_APPEND_CALLS,
        1U
    );
    if (fd < 0 || commit_lsn == 0U || page == nullptr || page_size == 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    std::uint64_t stage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
    if (!acquire_append_lock(fd)) {
        page_log_append_perf_add_elapsed_if_enabled(
            append_stats_enabled,
            PAGE_LOG_APPEND_PERF_LOCK_NS,
            stage_start_ns
        );
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    page_log_append_perf_add_elapsed_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_LOCK_NS,
        stage_start_ns
    );
    const auto offset = static_cast<off_t>(log_offset);
    int header_result = MYLITE_OWNERLESS_PAGE_LOG_OK;
    if (validate_header) {
        stage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
        header_result = validate_or_create_header(fd, offset);
        page_log_append_perf_add_elapsed_if_enabled(
            append_stats_enabled,
            PAGE_LOG_APPEND_PERF_HEADER_NS,
            stage_start_ns
        );
    }
    const int append_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK ? append_locked(
                                                                                  fd,
                                                                                  offset,
                                                                                  space_id,
                                                                                  page_no,
                                                                                  page_lsn,
                                                                                  commit_lsn,
                                                                                  page,
                                                                                  page_size,
                                                                                  out_record_offset,
                                                                                  extra_record_flags
                                                                              )
                                                                            : header_result;
    release_log_lock(fd, k_append_lock_start);
    return append_result;
}

int sync_at_common(int fd, std::uint64_t log_offset, bool validate_header) {
    PageLogSyncPerfScope total_scope(PAGE_LOG_SYNC_PERF_TOTAL_NS);
    page_log_sync_perf_add(PAGE_LOG_SYNC_PERF_CALLS, 1U);
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    std::uint64_t stage_start_ns =
        page_log_sync_perf_stats_are_enabled() ? page_log_append_perf_now_ns() : 0U;
    if (!acquire_snapshot_lock(fd)) {
        page_log_sync_perf_add_elapsed(PAGE_LOG_SYNC_PERF_LOCK_NS, stage_start_ns);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    page_log_sync_perf_add_elapsed(PAGE_LOG_SYNC_PERF_LOCK_NS, stage_start_ns);

    const auto offset = static_cast<off_t>(log_offset);
    stage_start_ns = page_log_sync_perf_stats_are_enabled() ? page_log_append_perf_now_ns() : 0U;
    int result = validate_header ? validate_existing_header(fd, offset)
                                 : validate_existing_header_size(fd, offset);
    page_log_sync_perf_add_elapsed(PAGE_LOG_SYNC_PERF_HEADER_NS, stage_start_ns);
    if (result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        stage_start_ns =
            page_log_sync_perf_stats_are_enabled() ? page_log_append_perf_now_ns() : 0U;
        result =
            sync_file_data(fd) ? MYLITE_OWNERLESS_PAGE_LOG_OK : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        page_log_sync_perf_add_elapsed(PAGE_LOG_SYNC_PERF_DATA_SYNC_NS, stage_start_ns);
    }

    release_log_lock(fd, k_append_lock_start);
    return result;
}

} // namespace

int mylite_ownerless_page_log_append(
    int fd,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    return mylite_ownerless_page_log_append_at(
        fd,
        0U,
        space_id,
        page_no,
        page_lsn,
        commit_lsn,
        page,
        page_size,
        out_record_offset
    );
}

int mylite_ownerless_page_log_append_at(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    return append_at_common(
        fd,
        log_offset,
        space_id,
        page_no,
        page_lsn,
        commit_lsn,
        page,
        page_size,
        out_record_offset,
        true,
        0U
    );
}

int mylite_ownerless_page_log_append_initialized_at(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    return append_at_common(
        fd,
        log_offset,
        space_id,
        page_no,
        page_lsn,
        commit_lsn,
        page,
        page_size,
        out_record_offset,
        false,
        0U
    );
}

int mylite_ownerless_page_log_append_snapshot_boundary_initialized_at(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    return append_at_common(
        fd,
        log_offset,
        space_id,
        page_no,
        page_lsn,
        commit_lsn,
        page,
        page_size,
        out_record_offset,
        false,
        k_record_flag_snapshot_boundary
    );
}

int mylite_ownerless_page_log_append_external_snapshot_lineage_initialized_at(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    return append_at_common(
        fd,
        log_offset,
        space_id,
        page_no,
        page_lsn,
        commit_lsn,
        page,
        page_size,
        out_record_offset,
        false,
        k_record_flag_external_snapshot_lineage
    );
}

int mylite_ownerless_page_log_append_session_begin_initialized_at(
    int fd,
    std::uint64_t log_offset,
    mylite_ownerless_page_log_append_session *session
) {
    const bool append_stats_enabled = page_log_append_perf_stats_are_enabled();
    page_log_append_perf_add_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_SESSION_BEGIN_CALLS,
        1U
    );
    if (session == nullptr || fd < 0 || session->active != 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    std::uint64_t stage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
    if (!acquire_append_lock(fd)) {
        page_log_append_perf_add_elapsed_if_enabled(
            append_stats_enabled,
            PAGE_LOG_APPEND_PERF_LOCK_NS,
            stage_start_ns
        );
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    page_log_append_perf_add_elapsed_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_LOCK_NS,
        stage_start_ns
    );

    const auto offset = static_cast<off_t>(log_offset);
    struct stat file_stat = {};
    PageLogHeader header = {};
    off_t records_offset = 0;
    stage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
    const int fstat_result = ::fstat(fd, &file_stat);
    page_log_append_perf_add_elapsed_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_FSTAT_NS,
        stage_start_ns
    );
    if (fstat_result != 0 ||
        !offset_adds(offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset || !read_header(fd, offset, header) ||
        !header_matches(header)) {
        release_log_lock(fd, k_append_lock_start);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    session->active = 1;
    session->log_offset = log_offset;
    session->next_record_offset = static_cast<std::uint64_t>(file_stat.st_size);
    session->log_device = static_cast<std::uint64_t>(file_stat.st_dev);
    session->log_inode = static_cast<std::uint64_t>(file_stat.st_ino);
    session->log_generation = header_generation(header);
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int mylite_ownerless_page_log_append_session_append(
    int fd,
    mylite_ownerless_page_log_append_session *session,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    const bool append_stats_enabled = page_log_append_perf_stats_are_enabled();
    const bool append_detail_stats_enabled =
        page_log_append_detail_perf_stats_are_enabled(append_stats_enabled);
    PageLogAppendPerfScope total_scope(PAGE_LOG_APPEND_PERF_TOTAL_NS, append_stats_enabled);
    page_log_append_perf_add_if_enabled(append_stats_enabled, PAGE_LOG_APPEND_PERF_CALLS, 1U);
    page_log_append_perf_add_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_SESSION_APPEND_CALLS,
        1U
    );
    if (fd < 0 || session == nullptr || session->active == 0 || commit_lsn == 0U ||
        page == nullptr || page_size == 0U ||
        session->log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        session->next_record_offset >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto record_offset = static_cast<off_t>(session->next_record_offset);
    std::uint64_t next_record_offset = 0;
    const int result = append_record_at_locked(
        fd,
        static_cast<off_t>(session->log_offset),
        session->log_device,
        session->log_inode,
        session->log_generation,
        record_offset,
        space_id,
        page_no,
        page_lsn,
        commit_lsn,
        page,
        page_size,
        out_record_offset,
        &next_record_offset,
        0U,
        append_stats_enabled,
        append_detail_stats_enabled
    );
    if (result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        session->next_record_offset = next_record_offset;
    }
    return result;
}

void mylite_ownerless_page_log_append_session_end(
    int fd,
    mylite_ownerless_page_log_append_session *session
) {
    const bool append_stats_enabled = page_log_append_perf_stats_are_enabled();
    page_log_append_perf_add_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_SESSION_END_CALLS,
        1U
    );
    if (session == nullptr || session->active == 0) {
        return;
    }
    if (fd >= 0) {
        release_log_lock(fd, k_append_lock_start);
    }
    session->active = 0;
    session->log_offset = 0U;
    session->next_record_offset = 0U;
    session->log_device = 0U;
    session->log_inode = 0U;
    session->log_generation = 0U;
}

int mylite_ownerless_page_log_sync(int fd) {
    return mylite_ownerless_page_log_sync_at(fd, 0U);
}

int mylite_ownerless_page_log_sync_at(int fd, std::uint64_t log_offset) {
    return sync_at_common(fd, log_offset, true);
}

int mylite_ownerless_page_log_sync_initialized_at(int fd, std::uint64_t log_offset) {
    return sync_at_common(fd, log_offset, false);
}

int mylite_ownerless_page_log_sync_initialized_if_changed_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t known_synced_end_offset,
    std::uint64_t known_synced_generation,
    std::uint64_t *out_current_end_offset,
    std::uint64_t *out_current_generation,
    int *out_synced
) {
    PageLogSyncPerfScope total_scope(PAGE_LOG_SYNC_PERF_TOTAL_NS);
    page_log_sync_perf_add(PAGE_LOG_SYNC_PERF_CALLS, 1U);
    if (out_current_end_offset != nullptr) {
        *out_current_end_offset = 0U;
    }
    if (out_current_generation != nullptr) {
        *out_current_generation = 0U;
    }
    if (out_synced != nullptr) {
        *out_synced = 0;
    }
    if (fd < 0 || log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    std::uint64_t stage_start_ns =
        page_log_sync_perf_stats_are_enabled() ? page_log_append_perf_now_ns() : 0U;
    if (!acquire_snapshot_lock(fd)) {
        page_log_sync_perf_add_elapsed(PAGE_LOG_SYNC_PERF_LOCK_NS, stage_start_ns);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    page_log_sync_perf_add_elapsed(PAGE_LOG_SYNC_PERF_LOCK_NS, stage_start_ns);

    const auto offset = static_cast<off_t>(log_offset);
    int result = MYLITE_OWNERLESS_PAGE_LOG_OK;
    struct stat file_stat = {};
    PageLogHeader header = {};
    off_t header_end = 0;
    stage_start_ns = page_log_sync_perf_stats_are_enabled() ? page_log_append_perf_now_ns() : 0U;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &header_end) ||
        file_stat.st_size < header_end || !read_header(fd, offset, header) ||
        !header_matches(header)) {
        result = MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    page_log_sync_perf_add_elapsed(PAGE_LOG_SYNC_PERF_HEADER_NS, stage_start_ns);

    const std::uint64_t current_end_offset =
        result == MYLITE_OWNERLESS_PAGE_LOG_OK ? static_cast<std::uint64_t>(file_stat.st_size) : 0U;
    const std::uint64_t current_generation =
        result == MYLITE_OWNERLESS_PAGE_LOG_OK ? header_generation(header) : 0U;
    if (result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        if (known_synced_end_offset == current_end_offset &&
            known_synced_generation == current_generation) {
            page_log_sync_perf_add(PAGE_LOG_SYNC_PERF_SKIPPED_CLEAN, 1U);
        } else {
            stage_start_ns =
                page_log_sync_perf_stats_are_enabled() ? page_log_append_perf_now_ns() : 0U;
            result =
                sync_file_data(fd) ? MYLITE_OWNERLESS_PAGE_LOG_OK : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            page_log_sync_perf_add_elapsed(PAGE_LOG_SYNC_PERF_DATA_SYNC_NS, stage_start_ns);
            if (result == MYLITE_OWNERLESS_PAGE_LOG_OK && out_synced != nullptr) {
                *out_synced = 1;
            }
        }
    }

    release_log_lock(fd, k_append_lock_start);
    if (result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        if (out_current_end_offset != nullptr) {
            *out_current_end_offset = current_end_offset;
        }
        if (out_current_generation != nullptr) {
            *out_current_generation = current_generation;
        }
    }
    return result;
}

int mylite_ownerless_page_log_record_is_snapshot_boundary_at(
    int fd,
    std::uint64_t record_offset,
    int *out_is_snapshot_boundary
) {
    if (out_is_snapshot_boundary != nullptr) {
        *out_is_snapshot_boundary = 0;
    }
    if (fd < 0 || out_is_snapshot_boundary == nullptr ||
        record_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageRecordHeader record = {};
    if (!read_record_header(fd, static_cast<off_t>(record_offset), record)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    *out_is_snapshot_boundary = (record.flags & k_record_flag_snapshot_boundary) != 0U ? 1 : 0;
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int mylite_ownerless_page_log_record_is_external_snapshot_lineage_at(
    int fd,
    std::uint64_t record_offset,
    int *out_is_external_snapshot_lineage
) {
    if (out_is_external_snapshot_lineage != nullptr) {
        *out_is_external_snapshot_lineage = 0;
    }
    if (fd < 0 || out_is_external_snapshot_lineage == nullptr ||
        record_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageRecordHeader record = {};
    if (!read_record_header(fd, static_cast<off_t>(record_offset), record)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    *out_is_external_snapshot_lineage =
        (record.flags & k_record_flag_external_snapshot_lineage) != 0U ? 1 : 0;
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int mylite_ownerless_page_log_record_next_offset_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t record_offset,
    std::uint64_t *out_next_record_offset
) {
    if (out_next_record_offset != nullptr) {
        *out_next_record_offset = 0U;
    }
    if (fd < 0 || out_next_record_offset == nullptr ||
        log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        record_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto physical_log_offset = static_cast<off_t>(log_offset);
    const auto physical_record_offset = static_cast<off_t>(record_offset);
    off_t records_offset = 0;
    off_t payload_offset = 0;
    off_t next_record_offset = 0;
    struct stat file_stat = {};
    PageRecordHeader record = {};
    if (validate_existing_header(fd, physical_log_offset) != MYLITE_OWNERLESS_PAGE_LOG_OK ||
        ::fstat(fd, &file_stat) != 0 ||
        !offset_adds(physical_log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        physical_record_offset < records_offset ||
        !offset_adds(
            physical_record_offset,
            MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
            &payload_offset
        ) ||
        !read_record_header(fd, physical_record_offset, record) ||
        !offset_adds(payload_offset, record.payload_size, &next_record_offset) ||
        next_record_offset > file_stat.st_size) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    *out_next_record_offset = static_cast<std::uint64_t>(next_record_offset);
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int mylite_ownerless_page_log_snapshot(int fd, std::uint64_t *out_snapshot_end_offset) {
    return mylite_ownerless_page_log_snapshot_at(fd, 0U, out_snapshot_end_offset);
}

int mylite_ownerless_page_log_snapshot_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t *out_snapshot_end_offset
) {
    if (fd < 0 || out_snapshot_end_offset == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const auto offset = static_cast<off_t>(log_offset);
    if (!acquire_snapshot_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    int header_result = validate_existing_header(fd, offset);
    int snapshot_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
                              ? snapshot_locked(fd, offset, out_snapshot_end_offset)
                              : header_result;
    release_log_lock(fd, k_append_lock_start);
    if (snapshot_result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return snapshot_result;
    }

    if (!acquire_append_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    header_result = validate_or_create_header(fd, offset);
    snapshot_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
                          ? snapshot_locked(fd, offset, out_snapshot_end_offset)
                          : header_result;
    release_log_lock(fd, k_append_lock_start);
    return snapshot_result;
}

int mylite_ownerless_page_log_snapshot_under_read_lock_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t *out_snapshot_end_offset,
    std::uint64_t *out_log_generation
) {
    if (fd < 0 || out_snapshot_end_offset == nullptr || out_log_generation == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    return snapshot_under_read_lock_with_generation(
        fd,
        static_cast<off_t>(log_offset),
        out_snapshot_end_offset,
        out_log_generation
    );
}

int mylite_ownerless_page_log_begin_read(int fd) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return acquire_checkpoint_read_lock(fd) ? MYLITE_OWNERLESS_PAGE_LOG_OK
                                            : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

void mylite_ownerless_page_log_end_read(int fd) {
    if (fd >= 0) {
        release_log_lock(fd, k_checkpoint_lock_start);
    }
}

int mylite_ownerless_page_log_find_latest(
    int fd,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    return mylite_ownerless_page_log_find_latest_at(
        fd,
        0U,
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );
}

int mylite_ownerless_page_log_find_latest_under_read_lock_at_with_flags(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags
) {
    if (out_record_flags != nullptr) {
        *out_record_flags = 0U;
    }
    if (fd < 0 || out_page == nullptr || page_capacity == 0U || out_page_size == nullptr ||
        out_page_lsn == nullptr || out_commit_lsn == nullptr || out_record_flags == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    std::uint64_t snapshot_end_offset = 0;
    const int snapshot_result = snapshot_under_read_lock(fd, offset, &snapshot_end_offset);
    if (snapshot_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return snapshot_result;
    }
    if (snapshot_end_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    return find_latest_in_snapshot(
        fd,
        offset,
        static_cast<off_t>(snapshot_end_offset),
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        out_record_flags
    );
}

int mylite_ownerless_page_log_find_latest_at(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || out_page == nullptr || page_capacity == 0U || out_page_size == nullptr ||
        out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    std::uint64_t snapshot_end_offset = 0;
    const int snapshot_result =
        mylite_ownerless_page_log_snapshot_at(fd, log_offset, &snapshot_end_offset);
    if (snapshot_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return snapshot_result;
    }
    return mylite_ownerless_page_log_find_latest_in_snapshot_at(
        fd,
        log_offset,
        snapshot_end_offset,
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );
}

int mylite_ownerless_page_log_find_latest_under_read_lock_at(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || out_page == nullptr || page_capacity == 0U || out_page_size == nullptr ||
        out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    std::uint64_t snapshot_end_offset = 0;
    const int snapshot_result = snapshot_under_read_lock(fd, offset, &snapshot_end_offset);
    if (snapshot_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return snapshot_result;
    }
    if (snapshot_end_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    return find_latest_in_snapshot(
        fd,
        offset,
        static_cast<off_t>(snapshot_end_offset),
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        nullptr
    );
}

int mylite_ownerless_page_log_find_latest_in_snapshot(
    int fd,
    std::uint64_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    return mylite_ownerless_page_log_find_latest_in_snapshot_at(
        fd,
        0U,
        snapshot_end_offset,
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );
}

int mylite_ownerless_page_log_find_latest_in_snapshot_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || out_page == nullptr || page_capacity == 0U || out_page_size == nullptr ||
        out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        snapshot_end_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    const auto end_offset = static_cast<off_t>(snapshot_end_offset);
    if (!acquire_checkpoint_read_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int result = find_latest_in_snapshot(
        fd,
        offset,
        end_offset,
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        nullptr
    );
    release_log_lock(fd, k_checkpoint_lock_start);
    return result;
}

int mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t scan_start_offset,
    std::uint64_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    int *out_saw_page_record
) {
    if (fd < 0 || out_page == nullptr || page_capacity == 0U || out_page_size == nullptr ||
        out_page_lsn == nullptr || out_commit_lsn == nullptr || out_saw_page_record == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        scan_start_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        snapshot_end_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    return find_latest_in_snapshot_range(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(scan_start_offset),
        static_cast<off_t>(snapshot_end_offset),
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        nullptr,
        out_saw_page_record
    );
}

int mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at_with_flags(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t scan_start_offset,
    std::uint64_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags,
    int *out_saw_page_record
) {
    if (out_record_flags != nullptr) {
        *out_record_flags = 0U;
    }
    if (fd < 0 || out_page == nullptr || page_capacity == 0U || out_page_size == nullptr ||
        out_page_lsn == nullptr || out_commit_lsn == nullptr || out_record_flags == nullptr ||
        out_saw_page_record == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        scan_start_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        snapshot_end_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    return find_latest_in_snapshot_range(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(scan_start_offset),
        static_cast<off_t>(snapshot_end_offset),
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        out_record_flags,
        out_saw_page_record
    );
}

int mylite_ownerless_page_log_read_record_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t record_offset,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || record_offset == 0U || out_page == nullptr || page_capacity == 0U ||
        out_page_size == nullptr || out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        record_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_checkpoint_read_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const int result = read_record_at_locked(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(record_offset),
        false,
        0U,
        0U,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        nullptr
    );

    release_log_lock(fd, k_checkpoint_lock_start);
    return result;
}

int mylite_ownerless_page_log_read_page_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t record_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || record_offset == 0U || out_page == nullptr || page_capacity == 0U ||
        out_page_size == nullptr || out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        record_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_checkpoint_read_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const int result = read_record_at_locked(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(record_offset),
        true,
        space_id,
        page_no,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        nullptr
    );

    release_log_lock(fd, k_checkpoint_lock_start);
    return result;
}

int mylite_ownerless_page_log_read_page_under_read_lock_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t record_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || record_offset == 0U || out_page == nullptr || page_capacity == 0U ||
        out_page_size == nullptr || out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        record_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    return read_record_at_locked(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(record_offset),
        true,
        space_id,
        page_no,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        nullptr
    );
}

int mylite_ownerless_page_log_read_page_under_read_lock_at_with_flags(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t record_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags
) {
    if (out_record_flags != nullptr) {
        *out_record_flags = 0U;
    }
    if (fd < 0 || record_offset == 0U || out_page == nullptr || page_capacity == 0U ||
        out_page_size == nullptr || out_page_lsn == nullptr || out_commit_lsn == nullptr ||
        out_record_flags == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        record_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    return read_record_at_locked(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(record_offset),
        true,
        space_id,
        page_no,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        out_record_flags
    );
}

int mylite_ownerless_page_log_replay_at(
    int fd,
    std::uint64_t log_offset,
    mylite_ownerless_page_log_replay_callback callback,
    void *context
) {
    if (fd < 0 || callback == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    std::uint64_t snapshot_end_offset = 0;
    const int snapshot_result =
        mylite_ownerless_page_log_snapshot_at(fd, log_offset, &snapshot_end_offset);
    if (snapshot_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return snapshot_result;
    }
    if (snapshot_end_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_checkpoint_read_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int replay_result = replay_in_snapshot(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(snapshot_end_offset),
        callback,
        context
    );
    release_log_lock(fd, k_checkpoint_lock_start);
    return replay_result;
}

int mylite_ownerless_page_log_checkpoint(
    int fd,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    void *context
) {
    return mylite_ownerless_page_log_checkpoint_with_completion_at(
        fd,
        0U,
        safe_commit_lsn,
        retained_record_callback,
        nullptr,
        context
    );
}

int mylite_ownerless_page_log_checkpoint_with_completion(
    int fd,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    return mylite_ownerless_page_log_checkpoint_with_completion_at(
        fd,
        0U,
        safe_commit_lsn,
        retained_record_callback,
        complete_callback,
        context
    );
}

int mylite_ownerless_page_log_checkpoint_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    void *context
) {
    return mylite_ownerless_page_log_checkpoint_with_completion_at(
        fd,
        log_offset,
        safe_commit_lsn,
        retained_record_callback,
        nullptr,
        context
    );
}

int mylite_ownerless_page_log_checkpoint_with_completion_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_checkpoint_write_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        release_log_lock(fd, k_checkpoint_lock_start);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    const int header_result = validate_or_create_header(fd, offset);
    const int checkpoint_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
                                      ? checkpoint_locked(
                                            fd,
                                            offset,
                                            safe_commit_lsn,
                                            retained_record_callback,
                                            complete_callback,
                                            context
                                        )
                                      : header_result;

    release_log_lock(fd, k_append_lock_start);
    release_log_lock(fd, k_checkpoint_lock_start);
    return checkpoint_result;
}

int mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    std::uint64_t oldest_snapshot_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_prepare_callback prepare_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    return checkpoint_preserving_oldest_snapshot_at_common(
        fd,
        log_offset,
        safe_commit_lsn,
        oldest_snapshot_lsn,
        true,
        retained_record_callback,
        prepare_callback,
        complete_callback,
        context
    );
}

int mylite_ownerless_page_log_checkpoint_preserving_single_snapshot_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    std::uint64_t snapshot_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_prepare_callback prepare_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    return checkpoint_preserving_oldest_snapshot_at_common(
        fd,
        log_offset,
        safe_commit_lsn,
        snapshot_lsn,
        false,
        retained_record_callback,
        prepare_callback,
        complete_callback,
        context
    );
}

namespace {

int checkpoint_preserving_oldest_snapshot_at_common(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    std::uint64_t oldest_snapshot_lsn,
    bool retain_checkpointed_snapshot_records_after_oldest,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_prepare_callback prepare_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    if (fd < 0 || oldest_snapshot_lsn == 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_checkpoint_write_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        release_log_lock(fd, k_checkpoint_lock_start);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    const int header_result = validate_or_create_header(fd, offset);
    const int checkpoint_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
                                      ? checkpoint_preserving_oldest_snapshot_locked(
                                            fd,
                                            offset,
                                            safe_commit_lsn,
                                            oldest_snapshot_lsn,
                                            retain_checkpointed_snapshot_records_after_oldest,
                                            retained_record_callback,
                                            prepare_callback,
                                            complete_callback,
                                            context
                                        )
                                      : header_result;

    release_log_lock(fd, k_append_lock_start);
    release_log_lock(fd, k_checkpoint_lock_start);
    return checkpoint_result;
}

} // namespace

int mylite_ownerless_page_log_checkpoint_if_safe(
    int fd,
    std::uint64_t safe_commit_lsn,
    int *out_checkpointed
) {
    return mylite_ownerless_page_log_checkpoint_if_safe_at(
        fd,
        0U,
        safe_commit_lsn,
        out_checkpointed
    );
}

int mylite_ownerless_page_log_checkpoint_if_safe_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    int *out_checkpointed
) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (out_checkpointed != nullptr) {
        *out_checkpointed = 0;
    }
    if (!acquire_checkpoint_write_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        release_log_lock(fd, k_checkpoint_lock_start);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    const int header_result = validate_or_create_header(fd, offset);
    const int checkpoint_result =
        header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
            ? checkpoint_if_safe_locked(fd, offset, safe_commit_lsn, out_checkpointed)
            : header_result;

    release_log_lock(fd, k_append_lock_start);
    release_log_lock(fd, k_checkpoint_lock_start);
    return checkpoint_result;
}

namespace {

int validate_or_create_header(int fd, off_t log_offset) {
    struct stat file_stat = {};
    if (::fstat(fd, &file_stat) != 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (file_stat.st_size < log_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    off_t header_end = 0;
    if (!offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &header_end)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (file_stat.st_size == log_offset) {
        return write_header(fd, log_offset) ? MYLITE_OWNERLESS_PAGE_LOG_OK
                                            : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (file_stat.st_size < header_end) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageLogHeader header = {};
    return read_header(fd, log_offset, header) && header_matches(header)
               ? MYLITE_OWNERLESS_PAGE_LOG_OK
               : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

int validate_existing_header(int fd, off_t log_offset) {
    struct stat file_stat = {};
    off_t header_end = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &header_end) ||
        file_stat.st_size < header_end) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageLogHeader header = {};
    return read_header(fd, log_offset, header) && header_matches(header)
               ? MYLITE_OWNERLESS_PAGE_LOG_OK
               : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

int validate_existing_header_size(int fd, off_t log_offset) {
    struct stat file_stat = {};
    off_t header_end = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &header_end) ||
        file_stat.st_size < header_end) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int append_locked(
    int fd,
    off_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset,
    std::uint32_t extra_record_flags
) {
    const bool append_stats_enabled = page_log_append_perf_stats_are_enabled();
    const bool append_detail_stats_enabled =
        page_log_append_detail_perf_stats_are_enabled(append_stats_enabled);
    PageLogAppendPerfScope body_scope(PAGE_LOG_APPEND_PERF_BODY_NS, append_stats_enabled);
    if ((extra_record_flags & ~k_record_metadata_flags) != 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    struct stat file_stat = {};
    PageLogHeader header = {};
    const std::uint64_t fstat_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
    const int fstat_result = ::fstat(fd, &file_stat);
    page_log_append_perf_add_elapsed_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_FSTAT_NS,
        fstat_start_ns
    );
    off_t records_offset = 0;
    if (fstat_result != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset || !read_header(fd, log_offset, header) ||
        !header_matches(header)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    return append_record_at_locked(
        fd,
        log_offset,
        static_cast<std::uint64_t>(file_stat.st_dev),
        static_cast<std::uint64_t>(file_stat.st_ino),
        header_generation(header),
        file_stat.st_size,
        space_id,
        page_no,
        page_lsn,
        commit_lsn,
        page,
        page_size,
        out_record_offset,
        nullptr,
        extra_record_flags,
        append_stats_enabled,
        append_detail_stats_enabled
    );
}

int append_record_at_locked(
    int fd,
    off_t log_offset,
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_generation,
    off_t record_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset,
    std::uint64_t *out_next_record_offset,
    std::uint32_t extra_record_flags,
    bool append_stats_enabled,
    bool append_detail_stats_enabled
) {
    off_t payload_offset = 0;
    if (!offset_adds(
            record_offset,
            MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
            &payload_offset
        )) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    off_t end_offset = 0;
    std::uint32_t record_flags = 0;
    std::uint64_t encoded_payload_size = 0;
    const void *record_page = page;
    thread_local std::vector<unsigned char> encoded_payload;
    encoded_payload.clear();
    std::uint64_t observed_standalone_payload_size = 0U;
    try {
        const std::uint64_t stage_start_ns =
            append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
        IndexPageDeltaBaseSnapshot page_delta_snapshot;
        std::uint64_t substage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
        const bool has_page_delta_snapshot = index_delta_base_snapshot_for_page(
            log_device,
            log_inode,
            static_cast<std::uint64_t>(log_offset),
            log_generation,
            space_id,
            page_no,
            record_page,
            page_size,
            &page_delta_snapshot
        );
        page_log_append_perf_add_elapsed_if_enabled(
            append_stats_enabled,
            PAGE_LOG_APPEND_PERF_DELTA_SNAPSHOT_NS,
            substage_start_ns
        );
        substage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
        thread_local std::vector<unsigned char> rejected_delta_payload;
        rejected_delta_payload.clear();
        PageDeltaEncodeDecision fast_delta_decision = PageDeltaEncodeDecision::Ineligible;
        std::uint64_t fast_delta_payload_size = 0U;
        const bool fast_page_delta_encoded =
            has_page_delta_snapshot && maybe_encode_page_delta_payload(
                                           page_delta_snapshot,
                                           record_page,
                                           page_size,
                                           page_delta_snapshot.standalone_payload_size,
                                           true,
                                           &record_flags,
                                           &encoded_payload,
                                           &fast_delta_decision,
                                           &fast_delta_payload_size,
                                           &rejected_delta_payload
                                       );
        page_log_append_perf_add_elapsed_if_enabled(
            append_stats_enabled,
            PAGE_LOG_APPEND_PERF_DELTA_ENCODE_NS,
            substage_start_ns
        );
        if (fast_page_delta_encoded) {
            encoded_payload_size = encoded_payload.size();
            if (append_stats_enabled) {
                page_log_append_perf_add_delta_accepted_record(
                    record_flags,
                    encoded_payload_size,
                    true
                );
            }
        } else {
            if (append_stats_enabled && has_page_delta_snapshot) {
                page_log_append_perf_add_delta_rejection(
                    fast_delta_decision,
                    fast_delta_payload_size,
                    true
                );
            }
            PageDeltaEncodeDecision exact_delta_decision = PageDeltaEncodeDecision::Ineligible;
            std::uint64_t exact_delta_payload_size = 0U;
            bool exact_page_delta_encoded = false;
            const bool exact_delta_build_failed =
                has_page_delta_snapshot &&
                fast_delta_decision == PageDeltaEncodeDecision::BuildFailed;
            if (has_page_delta_snapshot && !rejected_delta_payload.empty()) {
                const std::uint64_t size_probe_start_ns =
                    append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
                page_log_append_perf_add_if_enabled(
                    append_stats_enabled,
                    PAGE_LOG_APPEND_PERF_STANDALONE_SIZE_PROBE_CALLS,
                    1U
                );
                const std::uint64_t probed_standalone_payload_size =
                    encoded_payload_size_for_page_probe(record_page, page_size);
                observed_standalone_payload_size = probed_standalone_payload_size;
                page_log_append_perf_add_elapsed_if_enabled(
                    append_stats_enabled,
                    PAGE_LOG_APPEND_PERF_STANDALONE_SIZE_PROBE_NS,
                    size_probe_start_ns
                );
                exact_delta_payload_size = rejected_delta_payload.size();
                if (index_delta_payload_beats_standalone(
                        rejected_delta_payload.size(),
                        probed_standalone_payload_size
                    )) {
                    encoded_payload.swap(rejected_delta_payload);
                    encoded_payload_size = encoded_payload.size();
                    record_flags = page_delta_snapshot.delta_flag;
                    exact_delta_decision = PageDeltaEncodeDecision::Encoded;
                    exact_page_delta_encoded = true;
                    page_log_append_perf_add_if_enabled(
                        append_stats_enabled,
                        PAGE_LOG_APPEND_PERF_DELTA_EXACT_REUSED_FAST_PAYLOAD_RECORDS,
                        1U
                    );
                    page_log_append_perf_add_if_enabled(
                        append_stats_enabled,
                        PAGE_LOG_APPEND_PERF_DELTA_EXACT_REUSED_FAST_PAYLOAD_BYTES,
                        exact_delta_payload_size
                    );
                    page_log_append_perf_add_if_enabled(
                        append_stats_enabled,
                        PAGE_LOG_APPEND_PERF_STANDALONE_MATERIALIZE_SKIPPED_RECORDS,
                        1U
                    );
                    page_log_append_perf_add_if_enabled(
                        append_stats_enabled,
                        PAGE_LOG_APPEND_PERF_STANDALONE_MATERIALIZE_SKIPPED_BYTES,
                        probed_standalone_payload_size
                    );
                } else {
                    exact_delta_decision = PageDeltaEncodeDecision::Standalone;
                    rejected_delta_payload.clear();
                }
            } else if (exact_delta_build_failed) {
                exact_delta_decision = PageDeltaEncodeDecision::BuildFailed;
            }
            substage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
            if (!exact_page_delta_encoded) {
                encoded_payload_size = encoded_payload_size_for_page(
                    record_page,
                    page_size,
                    &record_flags,
                    &encoded_payload
                );
                page_log_append_perf_add_elapsed_if_enabled(
                    append_stats_enabled,
                    PAGE_LOG_APPEND_PERF_STANDALONE_ENCODE_NS,
                    substage_start_ns
                );
                substage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
                if (has_page_delta_snapshot &&
                    exact_delta_decision == PageDeltaEncodeDecision::Ineligible) {
                    exact_page_delta_encoded = maybe_encode_page_delta_payload(
                        page_delta_snapshot,
                        record_page,
                        page_size,
                        encoded_payload_size,
                        false,
                        &record_flags,
                        &encoded_payload,
                        &exact_delta_decision,
                        &exact_delta_payload_size,
                        nullptr
                    );
                }
                page_log_append_perf_add_elapsed_if_enabled(
                    append_stats_enabled,
                    PAGE_LOG_APPEND_PERF_DELTA_ENCODE_NS,
                    substage_start_ns
                );
            }
            if (exact_page_delta_encoded) {
                encoded_payload_size = encoded_payload.size();
                if (append_stats_enabled) {
                    page_log_append_perf_add_delta_accepted_record(
                        record_flags,
                        encoded_payload_size,
                        false
                    );
                }
            } else if (append_stats_enabled && has_page_delta_snapshot) {
                page_log_append_perf_add_delta_rejection(
                    exact_delta_decision,
                    exact_delta_payload_size,
                    false
                );
            }
        }
        page_log_append_perf_add_elapsed_if_enabled(
            append_stats_enabled,
            PAGE_LOG_APPEND_PERF_ENCODE_NS,
            stage_start_ns
        );
    } catch (const std::bad_alloc &) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    CompactSparsePayloadComposition compact_sparse;
    std::uint64_t stage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
    if (append_stats_enabled) {
        compact_sparse = record_append_payload_encoding_stats(
            record_flags,
            encoded_payload_size,
            encoded_payload
        );
    }
    page_log_append_perf_add_elapsed_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_PAYLOAD_STATS_NS,
        stage_start_ns
    );
    if (append_detail_stats_enabled) {
        stage_start_ns = page_log_append_perf_now_ns();
        record_append_page_type_stats(
            space_id,
            page_no,
            record_page,
            page_size,
            encoded_payload_size,
            compact_sparse
        );
        page_log_append_perf_add_elapsed_if_enabled(
            append_stats_enabled,
            PAGE_LOG_APPEND_PERF_PAGE_TYPE_STATS_NS,
            stage_start_ns
        );
    }
    if (!offset_adds(payload_offset, encoded_payload_size, &end_offset)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageRecordHeader record = {};
    record.space_id = space_id;
    record.page_no = page_no;
    record.page_size = page_size;
    record.flags = record_flags | extra_record_flags;
    record.page_lsn = page_lsn;
    record.commit_lsn = commit_lsn;
    record.payload_size = encoded_payload_size;
    stage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
    record.checksum = checksum_bytes(record_page, page_size);
    page_log_append_perf_add_elapsed_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_CHECKSUM_NS,
        stage_start_ns
    );

    stage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
    const void *payload = encoded_payload.empty() ? record_page : encoded_payload.data();
    const bool payload_written =
        write_exact_at(fd, payload, static_cast<std::size_t>(encoded_payload_size), payload_offset);
    page_log_append_perf_add_elapsed_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_PAYLOAD_WRITE_NS,
        stage_start_ns
    );
    if (!payload_written) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    page_log_append_perf_add_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_PAYLOAD_BYTES,
        encoded_payload_size
    );

    stage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
    const bool record_header_written = write_record_header(fd, record_offset, record);
    page_log_append_perf_add_elapsed_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_RECORD_HEADER_WRITE_NS,
        stage_start_ns
    );
    if (!record_header_written) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    stage_start_ns = append_stats_enabled ? page_log_append_perf_now_ns() : 0U;
    note_index_delta_base_after_successful_append(
        log_device,
        log_inode,
        static_cast<std::uint64_t>(log_offset),
        log_generation,
        space_id,
        page_no,
        record_page,
        page_size,
        static_cast<std::uint64_t>(record_offset),
        encoded_payload_size,
        record_flags,
        observed_standalone_payload_size
    );
    page_log_append_perf_add_elapsed_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_DELTA_BASE_NOTE_NS,
        stage_start_ns
    );
    page_log_append_perf_add_if_enabled(
        append_stats_enabled,
        PAGE_LOG_APPEND_PERF_RECORD_HEADER_BYTES,
        MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE
    );
    if (out_record_offset != nullptr) {
        *out_record_offset = static_cast<std::uint64_t>(record_offset);
    }
    if (out_next_record_offset != nullptr) {
        *out_next_record_offset = static_cast<std::uint64_t>(end_offset);
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int snapshot_locked(int fd, off_t log_offset, std::uint64_t *out_snapshot_end_offset) {
    return snapshot_locked_with_generation(fd, log_offset, out_snapshot_end_offset, nullptr);
}

int snapshot_locked_with_generation(
    int fd,
    off_t log_offset,
    std::uint64_t *out_snapshot_end_offset,
    std::uint64_t *out_log_generation
) {
    if (out_snapshot_end_offset == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    struct stat file_stat = {};
    off_t records_offset = 0;
    PageLogHeader header = {};
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset || !read_header(fd, log_offset, header) ||
        !header_matches(header)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    *out_snapshot_end_offset = static_cast<std::uint64_t>(file_stat.st_size);
    if (out_log_generation != nullptr) {
        *out_log_generation = header_generation(header);
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int snapshot_under_read_lock(int fd, off_t log_offset, std::uint64_t *out_snapshot_end_offset) {
    return snapshot_under_read_lock_with_generation(
        fd,
        log_offset,
        out_snapshot_end_offset,
        nullptr
    );
}

int snapshot_under_read_lock_with_generation(
    int fd,
    off_t log_offset,
    std::uint64_t *out_snapshot_end_offset,
    std::uint64_t *out_log_generation
) {
    if (!acquire_snapshot_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int header_result = validate_existing_header(fd, log_offset);
    const int snapshot_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
                                    ? snapshot_locked_with_generation(
                                          fd,
                                          log_offset,
                                          out_snapshot_end_offset,
                                          out_log_generation
                                      )
                                    : header_result;
    release_log_lock(fd, k_append_lock_start);
    return snapshot_result;
}

int find_latest_in_snapshot(
    int fd,
    off_t log_offset,
    off_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags
) {
    return find_latest_in_snapshot_range(
        fd,
        log_offset,
        log_offset,
        snapshot_end_offset,
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        out_record_flags,
        nullptr
    );
}

int find_latest_in_snapshot_range(
    int fd,
    off_t log_offset,
    off_t scan_start_offset,
    off_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags,
    int *out_saw_page_record
) {
    if (out_saw_page_record != nullptr) {
        *out_saw_page_record = 0;
    }
    if (out_record_flags != nullptr) {
        *out_record_flags = 0U;
    }
    const bool collect_scan_perf = page_log_scan_perf_stats_are_enabled();
    std::uint64_t scanned_record_headers = 0;
    std::uint64_t scanned_page_records = 0;
    std::uint64_t scanned_visible_page_records = 0;
    const auto publish_scan_perf = [&](PageLogScanPerfStatIndex outcome) {
        if (!collect_scan_perf) {
            return;
        }
        page_log_scan_perf_add(PAGE_LOG_SCAN_PERF_RECORD_HEADERS, scanned_record_headers);
        page_log_scan_perf_add(PAGE_LOG_SCAN_PERF_PAGE_RECORDS, scanned_page_records);
        page_log_scan_perf_add(
            PAGE_LOG_SCAN_PERF_VISIBLE_PAGE_RECORDS,
            scanned_visible_page_records
        );
        page_log_scan_perf_add(outcome, 1U);
    };
    page_log_scan_perf_add(PAGE_LOG_SCAN_PERF_CALLS, 1U);

    struct stat file_stat = {};
    off_t records_offset = 0;
    const int header_result = validate_existing_header(fd, log_offset);
    if (header_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        publish_scan_perf(PAGE_LOG_SCAN_PERF_ERRORS);
        return header_result;
    }
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset || snapshot_end_offset < records_offset ||
        snapshot_end_offset > file_stat.st_size) {
        publish_scan_perf(PAGE_LOG_SCAN_PERF_ERRORS);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    scan_start_offset = std::max(scan_start_offset, records_offset);
    if (scan_start_offset > snapshot_end_offset) {
        publish_scan_perf(PAGE_LOG_SCAN_PERF_ERRORS);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageRecordHeader best = {};
    off_t best_payload_offset = 0;
    for (off_t record_offset = scan_start_offset; record_offset < snapshot_end_offset;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
        if (!offset_adds(
                record_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &payload_offset
            )) {
            publish_scan_perf(PAGE_LOG_SCAN_PERF_ERRORS);
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (payload_offset > snapshot_end_offset) {
            break;
        }
        if (!read_record_header(fd, record_offset, record)) {
            break;
        }
        ++scanned_record_headers;
        off_t next_record_offset = 0;
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            publish_scan_perf(PAGE_LOG_SCAN_PERF_ERRORS);
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > snapshot_end_offset) {
            break;
        }
        if (record.space_id != space_id || record.page_no != page_no) {
            record_offset = next_record_offset;
            continue;
        }
        if (out_saw_page_record != nullptr) {
            *out_saw_page_record = 1;
        }
        ++scanned_page_records;
        if (record.commit_lsn > max_commit_lsn) {
            record_offset = next_record_offset;
            continue;
        }
        ++scanned_visible_page_records;
        if (!record_is_better(record, best)) {
            record_offset = next_record_offset;
            continue;
        }

        const PayloadStatus payload_status = record_payload_status(fd, payload_offset, record);
        if (payload_status == PayloadStatus::Mismatch &&
            next_record_offset == snapshot_end_offset) {
            break;
        }
        if (payload_status != PayloadStatus::Ok) {
            publish_scan_perf(PAGE_LOG_SCAN_PERF_ERRORS);
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        best = record;
        best_payload_offset = payload_offset;
        record_offset = next_record_offset;
    }
    if (best.commit_lsn == 0U) {
        publish_scan_perf(
            scanned_page_records == 0U ? PAGE_LOG_SCAN_PERF_NOT_FOUND_NO_PAGE_RECORD
                                       : PAGE_LOG_SCAN_PERF_NOT_FOUND_PAGE_RECORD_NOT_VISIBLE
        );
        return MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND;
    }
    if (record_page_too_large(best, page_capacity)) {
        publish_scan_perf(PAGE_LOG_SCAN_PERF_ERRORS);
        return MYLITE_OWNERLESS_PAGE_LOG_FULL;
    }
    if (!read_record_page_payload(fd, best_payload_offset, best, out_page, page_capacity)) {
        publish_scan_perf(PAGE_LOG_SCAN_PERF_ERRORS);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    *out_page_size = best.page_size;
    *out_page_lsn = best.page_lsn;
    *out_commit_lsn = best.commit_lsn;
    if (out_record_flags != nullptr) {
        *out_record_flags = best.flags;
    }
    publish_scan_perf(PAGE_LOG_SCAN_PERF_FOUND);
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int read_record_at_locked(
    int fd,
    off_t log_offset,
    off_t physical_record_offset,
    bool require_page_identity,
    std::uint32_t space_id,
    std::uint32_t page_no,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags
) {
    if (out_record_flags != nullptr) {
        *out_record_flags = 0U;
    }
    int result = validate_existing_header(fd, log_offset);
    if (result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return result;
    }

    struct stat file_stat = {};
    off_t records_offset = 0;
    off_t payload_offset = 0;
    off_t next_record_offset = 0;
    PageRecordHeader record = {};
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        physical_record_offset < records_offset ||
        !offset_adds(
            physical_record_offset,
            MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
            &payload_offset
        ) ||
        payload_offset > file_stat.st_size ||
        !read_record_header(fd, physical_record_offset, record) ||
        !offset_adds(payload_offset, record.payload_size, &next_record_offset) ||
        next_record_offset > file_stat.st_size) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    if (require_page_identity && (record.space_id != space_id || record.page_no != page_no)) {
        return MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND;
    }
    if (record_page_too_large(record, page_capacity)) {
        return MYLITE_OWNERLESS_PAGE_LOG_FULL;
    }
    if (!read_record_page_payload(fd, payload_offset, record, out_page, page_capacity)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    *out_page_size = record.page_size;
    *out_page_lsn = record.page_lsn;
    *out_commit_lsn = record.commit_lsn;
    if (out_record_flags != nullptr) {
        *out_record_flags = record.flags;
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int replay_in_snapshot(
    int fd,
    off_t log_offset,
    off_t snapshot_end_offset,
    mylite_ownerless_page_log_replay_callback callback,
    void *context
) {
    struct stat file_stat = {};
    off_t records_offset = 0;
    const int header_result = validate_existing_header(fd, log_offset);
    if (header_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return header_result;
    }
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset || snapshot_end_offset < records_offset ||
        snapshot_end_offset > file_stat.st_size) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    for (off_t record_offset = records_offset; record_offset < snapshot_end_offset;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
        off_t next_record_offset = 0;
        if (!offset_adds(
                record_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &payload_offset
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (payload_offset > snapshot_end_offset) {
            break;
        }
        if (!read_record_header(fd, record_offset, record)) {
            break;
        }
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > snapshot_end_offset) {
            break;
        }

        if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
            if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }
        }
        const PayloadStatus payload_status = record_payload_status(fd, payload_offset, record);
        if (payload_status == PayloadStatus::Mismatch &&
            next_record_offset == snapshot_end_offset) {
            break;
        }
        if (payload_status != PayloadStatus::Ok) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }

        const int callback_result = callback(
            record.space_id,
            record.page_no,
            record.page_lsn,
            record.commit_lsn,
            static_cast<std::uint64_t>(record_offset),
            context
        );
        if (callback_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
            return callback_result;
        }
        record_offset = next_record_offset;
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int checkpoint_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    struct stat file_stat = {};
    off_t records_offset = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    invalidate_index_delta_bases_for_log(
        static_cast<std::uint64_t>(file_stat.st_dev),
        static_cast<std::uint64_t>(file_stat.st_ino),
        static_cast<std::uint64_t>(log_offset)
    );

    off_t write_offset = records_offset;
    for (off_t record_offset = records_offset; record_offset < file_stat.st_size;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
        off_t next_record_offset = 0;
        if (!offset_adds(
                record_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &payload_offset
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (payload_offset > file_stat.st_size) {
            break;
        }
        if (!read_record_header(fd, record_offset, record)) {
            break;
        }
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > file_stat.st_size) {
            break;
        }

        if (record.commit_lsn > safe_commit_lsn) {
            if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
                if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
                    return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
                }
            }
            const PayloadStatus payload_status = record_payload_status(fd, payload_offset, record);
            if (payload_status == PayloadStatus::Mismatch &&
                next_record_offset == file_stat.st_size) {
                break;
            }
            if (payload_status != PayloadStatus::Ok) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }
            PageRecordHeader retained_record = {};
            std::vector<unsigned char> retained_payload;
            if (!read_standalone_or_rewrite_delta_payload(
                    fd,
                    payload_offset,
                    record,
                    &retained_record,
                    &retained_payload
                )) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }

            off_t write_payload_offset = 0;
            if (!offset_adds(
                    write_offset,
                    MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                    &write_payload_offset
                ) ||
                !write_record_header(fd, write_offset, retained_record) ||
                !write_exact_at(
                    fd,
                    retained_payload.data(),
                    retained_payload.size(),
                    write_payload_offset
                )) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }
            if (retained_record_callback != nullptr) {
                const int callback_result = retained_record_callback(
                    retained_record.space_id,
                    retained_record.page_no,
                    retained_record.page_lsn,
                    retained_record.commit_lsn,
                    static_cast<std::uint64_t>(write_offset),
                    context
                );
                if (callback_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
                    return callback_result;
                }
            }
            if (!offset_adds(write_payload_offset, retained_record.payload_size, &write_offset)) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }
        }
        record_offset = next_record_offset;
    }

    if (!increment_header_generation(fd, log_offset)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    maybe_pause_for_test_fault("checkpoint-before-truncate");
    if (::ftruncate(fd, write_offset) != 0 || !sync_file(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return complete_callback == nullptr ? MYLITE_OWNERLESS_PAGE_LOG_OK : complete_callback(context);
}

int checkpoint_preserving_oldest_snapshot_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    std::uint64_t oldest_snapshot_lsn,
    bool retain_checkpointed_snapshot_records_after_oldest,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_prepare_callback prepare_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    struct stat file_stat = {};
    off_t records_offset = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    invalidate_index_delta_bases_for_log(
        static_cast<std::uint64_t>(file_stat.st_dev),
        static_cast<std::uint64_t>(file_stat.st_ino),
        static_cast<std::uint64_t>(log_offset)
    );

    std::vector<ScannedPageRecord> records;
    std::unordered_map<std::uint64_t, PageRetentionState> retention_by_page;
    for (off_t record_offset = records_offset; record_offset < file_stat.st_size;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
        off_t next_record_offset = 0;
        if (!offset_adds(
                record_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &payload_offset
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (payload_offset > file_stat.st_size) {
            break;
        }
        if (!read_record_header(fd, record_offset, record)) {
            break;
        }
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > file_stat.st_size) {
            break;
        }

        const PayloadStatus payload_status = record_payload_status(fd, payload_offset, record);
        if (payload_status == PayloadStatus::Mismatch && next_record_offset == file_stat.st_size) {
            break;
        }
        if (payload_status != PayloadStatus::Ok) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }

        const bool requires_snapshot_boundary =
            record_requires_oldest_snapshot_boundary(fd, payload_offset, record);
        const ScannedPageRecord scanned_record{
            record_offset,
            payload_offset,
            next_record_offset,
            record,
            requires_snapshot_boundary,
        };
        records.push_back(scanned_record);
        PageRetentionState &retention =
            retention_by_page[page_key(record.space_id, record.page_no)];
        if (requires_snapshot_boundary && record.commit_lsn > oldest_snapshot_lsn &&
            record.commit_lsn <= safe_commit_lsn) {
            retention.has_after_oldest_checkpointed_record = true;
        } else {
            const bool record_can_bound_snapshot = record.commit_lsn <= oldest_snapshot_lsn;
            const bool record_improves_boundary =
                !retention.has_boundary_record ||
                record_is_better(record, retention.boundary_record);
            if (record_can_bound_snapshot && record_improves_boundary) {
                retention.has_boundary_record = true;
                retention.boundary_record_offset = record_offset;
                retention.boundary_record = record;
            }
        }
        record_offset = next_record_offset;
    }

    for (const auto &entry : retention_by_page) {
        const PageRetentionState &retention = entry.second;
        if (retention.has_after_oldest_checkpointed_record && !retention.has_boundary_record) {
            return MYLITE_OWNERLESS_PAGE_LOG_BUSY;
        }
    }

    if (prepare_callback != nullptr) {
        const int prepare_result = prepare_callback(context);
        if (prepare_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
            return prepare_result;
        }
    }

    off_t write_offset = records_offset;
    for (const ScannedPageRecord &scanned : records) {
        const PageRecordHeader &record = scanned.record;
        const auto retention_it = retention_by_page.find(page_key(record.space_id, record.page_no));
        const bool retain_boundary =
            retention_it != retention_by_page.end() &&
            retention_it->second.has_after_oldest_checkpointed_record &&
            scanned.record_offset == retention_it->second.boundary_record_offset;
        if (record.commit_lsn <= oldest_snapshot_lsn && !retain_boundary) {
            continue;
        }
        if (!retain_boundary && record.commit_lsn <= safe_commit_lsn &&
            (!scanned.requires_snapshot_boundary ||
             !retain_checkpointed_snapshot_records_after_oldest)) {
            continue;
        }

        if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
            if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }
        }
        PageRecordHeader retained_record = {};
        std::vector<unsigned char> retained_payload;
        if (!read_standalone_or_rewrite_delta_payload(
                fd,
                scanned.payload_offset,
                record,
                &retained_record,
                &retained_payload
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }

        off_t write_payload_offset = 0;
        if (!offset_adds(
                write_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &write_payload_offset
            ) ||
            !write_record_header(fd, write_offset, retained_record) ||
            !write_exact_at(
                fd,
                retained_payload.data(),
                retained_payload.size(),
                write_payload_offset
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (retained_record_callback != nullptr) {
            const int callback_result = retained_record_callback(
                retained_record.space_id,
                retained_record.page_no,
                retained_record.page_lsn,
                retained_record.commit_lsn,
                static_cast<std::uint64_t>(write_offset),
                context
            );
            if (callback_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
                return callback_result;
            }
        }
        if (!offset_adds(write_payload_offset, retained_record.payload_size, &write_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
    }

    if (!increment_header_generation(fd, log_offset)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    maybe_pause_for_test_fault("checkpoint-before-truncate");
    if (::ftruncate(fd, write_offset) != 0 || !sync_file(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return complete_callback == nullptr ? MYLITE_OWNERLESS_PAGE_LOG_OK : complete_callback(context);
}

int checkpoint_if_safe_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    int *out_checkpointed
) {
    struct stat file_stat = {};
    off_t records_offset = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    invalidate_index_delta_bases_for_log(
        static_cast<std::uint64_t>(file_stat.st_dev),
        static_cast<std::uint64_t>(file_stat.st_ino),
        static_cast<std::uint64_t>(log_offset)
    );

    for (off_t record_offset = records_offset; record_offset < file_stat.st_size;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
        off_t next_record_offset = 0;
        if (!offset_adds(
                record_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &payload_offset
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (payload_offset > file_stat.st_size) {
            break;
        }
        if (!read_record_header(fd, record_offset, record)) {
            break;
        }
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > file_stat.st_size) {
            break;
        }
        const PayloadStatus payload_status = record_payload_status(fd, payload_offset, record);
        if (payload_status == PayloadStatus::Mismatch && next_record_offset == file_stat.st_size) {
            break;
        }
        if (payload_status != PayloadStatus::Ok) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (record.commit_lsn > safe_commit_lsn) {
            return MYLITE_OWNERLESS_PAGE_LOG_OK;
        }
        record_offset = next_record_offset;
    }

    if (file_stat.st_size == records_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }
    if (!increment_header_generation(fd, log_offset)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    maybe_pause_for_test_fault("checkpoint-before-truncate");
    if (::ftruncate(fd, records_offset) != 0 || !sync_file(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (out_checkpointed != nullptr) {
        *out_checkpointed = 1;
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

bool acquire_append_lock(int fd) {
    return acquire_log_lock(fd, F_WRLCK, k_append_lock_start);
}

bool acquire_snapshot_lock(int fd) {
    return acquire_log_lock(fd, F_RDLCK, k_append_lock_start);
}

bool acquire_checkpoint_read_lock(int fd) {
    return acquire_log_lock(fd, F_RDLCK, k_checkpoint_lock_start);
}

bool acquire_checkpoint_write_lock(int fd) {
    return acquire_log_lock(fd, F_WRLCK, k_checkpoint_lock_start);
}

bool acquire_log_lock(int fd, short lock_type, off_t lock_start) {
    struct flock lock = {};
    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = lock_start;
    lock.l_len = 1;
    while (::fcntl(fd, F_SETLKW, &lock) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

void release_log_lock(int fd, off_t lock_start) {
    struct flock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = lock_start;
    lock.l_len = 1;
    static_cast<void>(::fcntl(fd, F_SETLK, &lock));
}

void maybe_pause_for_test_fault(const char *fault_name) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    const char *configured_fault = std::getenv("MYLITE_OWNERLESS_TEST_FAULT");
    if (configured_fault == nullptr || std::strcmp(configured_fault, fault_name) != 0) {
        return;
    }

    const char *ready_fd_value = std::getenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD");
    if (ready_fd_value != nullptr) {
        char *end = nullptr;
        const long ready_fd = std::strtol(ready_fd_value, &end, 10);
        if (end != ready_fd_value && *end == '\0' && ready_fd >= 0 &&
            ready_fd <= std::numeric_limits<int>::max()) {
            const char value = 'x';
            static_cast<void>(::write(static_cast<int>(ready_fd), &value, sizeof(value)));
            static_cast<void>(::close(static_cast<int>(ready_fd)));
        }
    }

    for (;;) {
        ::pause();
    }
#else
    (void)fault_name;
#endif
}

bool read_header(int fd, off_t log_offset, PageLogHeader &header) {
    return read_exact_at(fd, header.data(), header.size(), log_offset);
}

bool write_header(int fd, off_t log_offset) {
    PageLogHeader header = {};
    std::memcpy(
        header.data() + k_header_magic_offset,
        k_header_magic.data(),
        k_header_magic.size()
    );
    store32(header.data(), k_header_format_offset, k_format_version);
    store32(header.data(), k_header_size_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    store32(
        header.data(),
        k_header_record_header_size_offset,
        MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE
    );
    store64(header.data(), k_header_generation_offset, 1U);
    return write_exact_at(fd, header.data(), header.size(), log_offset);
}

std::uint64_t header_generation(const PageLogHeader &header) {
    return load64(header.data(), k_header_generation_offset);
}

bool increment_header_generation(int fd, off_t log_offset) {
    PageLogHeader header = {};
    if (!read_header(fd, log_offset, header) || !header_matches(header)) {
        return false;
    }
    off_t generation_offset = 0;
    if (!offset_adds(
            log_offset,
            static_cast<std::uint64_t>(k_header_generation_offset),
            &generation_offset
        )) {
        return false;
    }
    std::uint64_t generation = header_generation(header);
    generation = generation == std::numeric_limits<std::uint64_t>::max() ? 1U : generation + 1U;
    unsigned char encoded_generation[sizeof(std::uint64_t)] = {};
    store64(encoded_generation, 0U, generation);
    return write_exact_at(fd, encoded_generation, sizeof(encoded_generation), generation_offset);
}

bool header_matches(const PageLogHeader &header) {
    return std::memcmp(
               header.data() + k_header_magic_offset,
               k_header_magic.data(),
               k_header_magic.size()
           ) == 0 &&
           load32(header.data(), k_header_format_offset) == k_format_version &&
           load32(header.data(), k_header_size_offset) == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE &&
           load32(header.data(), k_header_record_header_size_offset) ==
               MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE;
}

bool read_record_header(int fd, off_t offset, PageRecordHeader &header) {
    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE> bytes = {};
    if (!read_exact_at(fd, bytes.data(), bytes.size(), offset) ||
        std::memcmp(
            bytes.data() + k_record_magic_offset,
            k_record_magic.data(),
            k_record_magic.size()
        ) != 0) {
        return false;
    }

    header.space_id = load32(bytes.data(), k_record_space_id_offset);
    header.page_no = load32(bytes.data(), k_record_page_no_offset);
    header.page_size = load32(bytes.data(), k_record_page_size_offset);
    header.flags = load32(bytes.data(), k_record_flags_offset);
    header.page_lsn = load64(bytes.data(), k_record_page_lsn_offset);
    header.commit_lsn = load64(bytes.data(), k_record_commit_lsn_offset);
    header.payload_size = load64(bytes.data(), k_record_payload_size_offset);
    header.checksum = load64(bytes.data(), k_record_payload_checksum_offset);
    return record_payload_shape_valid(header) && header.commit_lsn != 0U;
}

bool write_record_header(int fd, off_t offset, const PageRecordHeader &header) {
    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE> bytes = {};
    std::memcpy(bytes.data() + k_record_magic_offset, k_record_magic.data(), k_record_magic.size());
    store32(bytes.data(), k_record_space_id_offset, header.space_id);
    store32(bytes.data(), k_record_page_no_offset, header.page_no);
    store32(bytes.data(), k_record_page_size_offset, header.page_size);
    store32(bytes.data(), k_record_flags_offset, header.flags);
    store64(bytes.data(), k_record_page_lsn_offset, header.page_lsn);
    store64(bytes.data(), k_record_commit_lsn_offset, header.commit_lsn);
    store64(bytes.data(), k_record_payload_size_offset, header.payload_size);
    store64(bytes.data(), k_record_payload_checksum_offset, header.checksum);
    return write_exact_at(fd, bytes.data(), bytes.size(), offset);
}

bool write_exact_at(int fd, const void *buffer, std::size_t size, off_t offset) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    std::size_t written = 0;
    while (written < size) {
        off_t io_offset = 0;
        if (!next_io_offset(offset, written, &io_offset)) {
            return false;
        }
        const ssize_t result = ::pwrite(fd, bytes + written, size - written, io_offset);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

bool read_exact_at(int fd, void *buffer, std::size_t size, off_t offset) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    std::size_t read = 0;
    while (read < size) {
        off_t io_offset = 0;
        if (!next_io_offset(offset, read, &io_offset)) {
            return false;
        }
        const ssize_t result = ::pread(fd, bytes + read, size - read, io_offset);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        read += static_cast<std::size_t>(result);
    }
    return true;
}

bool sync_file(int fd) {
    while (::fsync(fd) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool sync_file_data(int fd) {
#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    while (::fdatasync(fd) != 0) {
#else
    while (::fsync(fd) != 0) {
#endif
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool next_io_offset(off_t offset, std::size_t progress, off_t *out_offset) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (progress > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    return offset_adds(offset, static_cast<std::uint64_t>(progress), out_offset);
}

bool offset_adds(off_t offset, std::uint64_t length, off_t *out_offset) {
    if (out_offset == nullptr || offset < 0 ||
        length > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max() - offset)) {
        return false;
    }
    *out_offset = offset + static_cast<off_t>(length);
    return true;
}

std::uint64_t encoded_payload_size_for_page(
    const void *page,
    std::uint32_t page_size,
    std::uint32_t *out_flags,
    std::vector<unsigned char> *out_payload
) {
    std::uint32_t flags = 0;
    std::uint64_t encoded_size = page_size;
    std::uint64_t trailing_size = page_size;
    bool trailing_size_known = false;
    if (out_payload != nullptr) {
        const std::uint16_t page_type =
            page_size >= k_innodb_fil_page_type_offset + sizeof(std::uint16_t)
                ? load_be16(
                      static_cast<const unsigned char *>(page) + k_innodb_fil_page_type_offset
                  )
                : 0U;
        if (page_type == k_innodb_fil_page_type_sys) {
            std::uint64_t fill_trailing_size = page_size;
            bool fill_trailing_size_known = false;
            if (build_fill_sparse_zero_payload_if_smaller_than_compact(
                    page,
                    page_size,
                    out_payload,
                    &fill_trailing_size,
                    &fill_trailing_size_known
                )) {
                trailing_size = fill_trailing_size;
                trailing_size_known = true;
                flags |= k_record_flag_fill_sparse_zero_payload;
                encoded_size = out_payload->size();
            } else if (fill_trailing_size_known) {
                trailing_size = fill_trailing_size;
                trailing_size_known = true;
            }
        }
        std::uint64_t compact_trailing_size = page_size;
        bool compact_trailing_size_known = false;
        bool compact_uses_varint_payload = false;
        if (flags == 0U) {
            if (build_compact_sparse_zero_payload(
                    page,
                    page_size,
                    out_payload,
                    &compact_trailing_size,
                    &compact_trailing_size_known,
                    &compact_uses_varint_payload
                )) {
                trailing_size = compact_trailing_size;
                trailing_size_known = true;
                if (out_payload->size() < trailing_size) {
                    thread_local std::vector<unsigned char> fill_payload;
                    fill_payload.clear();
                    std::uint64_t fill_sparse_index_size = 0U;
                    const bool fill_sparse_candidate =
                        page_type == k_innodb_fil_page_type_sys ||
                        (page_type == k_innodb_fil_page_index &&
                         compact_sparse_payload_fill_sparse_size(
                             *out_payload,
                             compact_uses_varint_payload,
                             &fill_sparse_index_size
                         ) &&
                         fill_sparse_index_size < out_payload->size());
                    if (fill_sparse_candidate &&
                        build_fill_sparse_zero_payload(page, page_size, &fill_payload) &&
                        fill_payload.size() < out_payload->size()) {
                        out_payload->swap(fill_payload);
                        flags |= k_record_flag_fill_sparse_zero_payload;
                    } else if (compact_uses_varint_payload) {
                        flags |= k_record_flag_varint_compact_sparse_zero_payload;
                    } else {
                        flags |= k_record_flag_compact_sparse_zero_payload;
                    }
                    encoded_size = out_payload->size();
                }
            } else if (compact_trailing_size_known) {
                trailing_size = compact_trailing_size;
                trailing_size_known = true;
            } else {
                std::uint64_t sparse_trailing_size = page_size;
                bool sparse_trailing_size_known = false;
                if (build_sparse_zero_payload(
                        page,
                        page_size,
                        out_payload,
                        &sparse_trailing_size,
                        &sparse_trailing_size_known
                    )) {
                    trailing_size = sparse_trailing_size;
                    trailing_size_known = true;
                    if (out_payload->size() < trailing_size) {
                        flags |= k_record_flag_sparse_zero_payload;
                        encoded_size = out_payload->size();
                    }
                } else if (sparse_trailing_size_known) {
                    trailing_size = sparse_trailing_size;
                    trailing_size_known = true;
                }
            }
        }
    }
    if (flags == 0U && !trailing_size_known) {
        trailing_size = trailing_zero_payload_size_for_page(page, page_size);
    }
    if (flags == 0U && trailing_size < page_size) {
        flags |= k_record_flag_trailing_zero_payload;
        encoded_size = trailing_size;
        if (out_payload != nullptr) {
            out_payload->clear();
        }
    } else if (flags == 0U && out_payload != nullptr) {
        out_payload->clear();
    }
    if (out_flags != nullptr) {
        *out_flags = flags;
    }
    return encoded_size;
}

std::uint64_t encoded_payload_size_for_page_probe(const void *page, std::uint32_t page_size) {
    std::uint64_t trailing_size = page_size;
    bool trailing_size_known = false;
    const std::uint16_t page_type =
        page_size >= k_innodb_fil_page_type_offset + sizeof(std::uint16_t)
            ? load_be16(static_cast<const unsigned char *>(page) + k_innodb_fil_page_type_offset)
            : 0U;

    std::uint64_t compact_size = page_size;
    std::uint64_t compact_trailing_size = page_size;
    bool compact_trailing_size_known = false;
    if (compact_sparse_zero_payload_size_for_page(
            page,
            page_size,
            &compact_size,
            &compact_trailing_size,
            &compact_trailing_size_known
        )) {
        trailing_size = compact_trailing_size;
        trailing_size_known = true;
        if (compact_size < trailing_size) {
            if (page_type == k_innodb_fil_page_type_sys || page_type == k_innodb_fil_page_index) {
                std::uint64_t fill_size = page_size;
                if (fill_sparse_zero_payload_size_for_page(page, page_size, &fill_size) &&
                    fill_size < compact_size) {
                    return fill_size;
                }
            }
            return compact_size;
        }
    } else if (compact_trailing_size_known) {
        trailing_size = compact_trailing_size;
        trailing_size_known = true;
    } else {
        std::uint64_t sparse_size = page_size;
        std::uint64_t sparse_trailing_size = page_size;
        bool sparse_trailing_size_known = false;
        if (sparse_zero_payload_size_for_page(
                page,
                page_size,
                &sparse_size,
                &sparse_trailing_size,
                &sparse_trailing_size_known
            )) {
            trailing_size = sparse_trailing_size;
            trailing_size_known = true;
            if (sparse_size < trailing_size) {
                return sparse_size;
            }
        } else if (sparse_trailing_size_known) {
            trailing_size = sparse_trailing_size;
            trailing_size_known = true;
        }
    }

    if (!trailing_size_known) {
        trailing_size = trailing_zero_payload_size_for_page(page, page_size);
    }
    return trailing_size < page_size ? trailing_size : page_size;
}

std::uint64_t trailing_zero_payload_size_for_page(const void *page, std::uint32_t page_size) {
    const auto *bytes = static_cast<const unsigned char *>(page);
    std::uint64_t encoded_size = page_size;
    while (encoded_size > 0U && bytes[encoded_size - 1U] == 0U) {
        --encoded_size;
    }
    return encoded_size;
}

bool sparse_zero_payload_size_for_page(
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_size,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
) {
    if (out_size == nullptr) {
        return false;
    }
    if (out_trailing_size_known != nullptr) {
        *out_trailing_size_known = false;
    }
    const auto *bytes = static_cast<const unsigned char *>(page);
    std::uint64_t payload_size = sizeof(std::uint32_t);
    std::uint32_t run_count = 0U;
    std::uint64_t trailing_size = 0U;

    for (std::uint32_t offset = 0U; offset < page_size;) {
        while (offset < page_size && bytes[offset] == 0U) {
            ++offset;
        }
        if (offset == page_size) {
            break;
        }
        const std::uint32_t run_start = offset;
        while (offset < page_size && bytes[offset] != 0U) {
            ++offset;
        }
        trailing_size = offset;
        const std::uint32_t run_size = offset - run_start;
        const std::uint64_t encoded_run_size = (2U * sizeof(std::uint32_t)) + run_size;
        if (payload_size > std::numeric_limits<std::uint64_t>::max() - encoded_run_size ||
            payload_size + encoded_run_size >= page_size) {
            return false;
        }
        payload_size += encoded_run_size;
        ++run_count;
    }

    if (out_trailing_size != nullptr) {
        *out_trailing_size = trailing_size;
    }
    if (out_trailing_size_known != nullptr) {
        *out_trailing_size_known = true;
    }
    if (run_count == 0U) {
        return false;
    }
    *out_size = payload_size;
    return true;
}

bool compact_sparse_zero_payload_size_for_page(
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_size,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
) {
    if (out_size == nullptr) {
        return false;
    }
    if (out_trailing_size_known != nullptr) {
        *out_trailing_size_known = false;
    }
    const auto *bytes = static_cast<const unsigned char *>(page);
    std::uint32_t run_count = 0U;
    std::uint32_t previous_run_end = 0U;
    std::uint64_t compact_payload_size = sizeof(std::uint16_t);
    std::uint64_t varint_payload_size = sizeof(std::uint16_t);
    std::uint64_t trailing_size = 0U;

    for (std::uint32_t offset = 0U; offset < page_size;) {
        while (offset < page_size && bytes[offset] == 0U) {
            ++offset;
        }
        if (offset == page_size) {
            break;
        }
        const std::uint32_t run_start = offset;
        while (offset < page_size && bytes[offset] != 0U) {
            ++offset;
        }
        trailing_size = offset;
        const std::uint32_t run_size = offset - run_start;
        if (run_start > std::numeric_limits<std::uint16_t>::max() ||
            run_size > std::numeric_limits<std::uint16_t>::max() ||
            run_count == std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }

        const std::uint64_t compact_encoded_run_size = (2U * sizeof(std::uint16_t)) + run_size;
        if (compact_payload_size >
                std::numeric_limits<std::uint64_t>::max() - compact_encoded_run_size ||
            compact_payload_size + compact_encoded_run_size >= page_size) {
            return false;
        }
        compact_payload_size += compact_encoded_run_size;

        const std::uint64_t varint_encoded_run_size =
            varuint16_encoded_size(run_start - previous_run_end) +
            varuint16_encoded_size(run_size) + run_size;
        if (varint_payload_size >
            std::numeric_limits<std::uint64_t>::max() - varint_encoded_run_size) {
            return false;
        }
        varint_payload_size += varint_encoded_run_size;
        previous_run_end = offset;
        ++run_count;
    }

    if (out_trailing_size != nullptr) {
        *out_trailing_size = trailing_size;
    }
    if (out_trailing_size_known != nullptr) {
        *out_trailing_size_known = true;
    }
    if (run_count == 0U) {
        return false;
    }
    *out_size =
        varint_payload_size < compact_payload_size ? varint_payload_size : compact_payload_size;
    return true;
}

bool fill_sparse_zero_payload_size_for_page(
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_size
) {
    if (out_size == nullptr || page_size > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    const auto *bytes = static_cast<const unsigned char *>(page);
    std::uint64_t payload_size = sizeof(std::uint16_t);
    std::uint32_t run_count = 0U;
    std::uint32_t previous_run_end = 0U;
    bool has_fill_run = false;

    for (std::uint32_t offset = 0U; offset < page_size;) {
        while (offset < page_size && bytes[offset] == 0U) {
            ++offset;
        }
        if (offset == page_size) {
            break;
        }

        while (offset < page_size && bytes[offset] != 0U) {
            const unsigned char fill_byte = bytes[offset];
            std::uint32_t fill_size = 1U;
            while (offset + fill_size < page_size && bytes[offset + fill_size] == fill_byte) {
                ++fill_size;
            }
            if (fill_size >= k_fill_sparse_min_fill_run_size) {
                if (run_count == std::numeric_limits<std::uint16_t>::max() ||
                    !fill_sparse_encoded_run_size(
                        &payload_size,
                        offset - previous_run_end,
                        fill_size,
                        true
                    ) ||
                    payload_size >= page_size) {
                    return false;
                }
                ++run_count;
                has_fill_run = true;
                offset += fill_size;
                previous_run_end = offset;
                continue;
            }

            const std::uint32_t raw_start = offset;
            offset += fill_size;
            while (offset < page_size && bytes[offset] != 0U) {
                const unsigned char next_fill_byte = bytes[offset];
                std::uint32_t next_fill_size = 1U;
                while (offset + next_fill_size < page_size &&
                       bytes[offset + next_fill_size] == next_fill_byte) {
                    ++next_fill_size;
                }
                if (next_fill_size >= k_fill_sparse_min_fill_run_size) {
                    break;
                }
                offset += next_fill_size;
            }
            const std::uint32_t raw_size = offset - raw_start;
            if (run_count == std::numeric_limits<std::uint16_t>::max() ||
                !fill_sparse_encoded_run_size(
                    &payload_size,
                    raw_start - previous_run_end,
                    raw_size,
                    false
                ) ||
                payload_size >= page_size) {
                return false;
            }
            ++run_count;
            previous_run_end = offset;
        }
    }

    if (run_count == 0U || !has_fill_run) {
        return false;
    }
    *out_size = payload_size;
    return true;
}

bool build_sparse_zero_payload(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
) {
    if (out_payload == nullptr) {
        return false;
    }
    if (out_trailing_size_known != nullptr) {
        *out_trailing_size_known = false;
    }
    const auto *bytes = static_cast<const unsigned char *>(page);
    out_payload->clear();
    out_payload->reserve(page_size);
    out_payload->resize(sizeof(std::uint32_t));
    std::uint32_t run_count = 0;
    std::uint64_t trailing_size = 0U;

    for (std::uint32_t offset = 0; offset < page_size;) {
        while (offset < page_size && bytes[offset] == 0U) {
            ++offset;
        }
        if (offset == page_size) {
            break;
        }
        const std::uint32_t run_start = offset;
        while (offset < page_size && bytes[offset] != 0U) {
            ++offset;
        }
        trailing_size = offset;
        const std::uint32_t run_size = offset - run_start;
        const std::size_t cursor = out_payload->size();
        const std::size_t encoded_run_size = (2U * sizeof(std::uint32_t)) + run_size;
        if (cursor + encoded_run_size >= page_size) {
            out_payload->clear();
            return false;
        }
        out_payload->resize(cursor + encoded_run_size);
        store32(out_payload->data(), cursor, run_start);
        store32(out_payload->data(), cursor + sizeof(std::uint32_t), run_size);
        std::memcpy(
            out_payload->data() + cursor + (2U * sizeof(std::uint32_t)),
            bytes + run_start,
            run_size
        );
        ++run_count;
    }

    if (out_trailing_size != nullptr) {
        *out_trailing_size = trailing_size;
    }
    if (out_trailing_size_known != nullptr) {
        *out_trailing_size_known = true;
    }
    if (run_count == 0U) {
        out_payload->clear();
        return false;
    }
    store32(out_payload->data(), 0U, run_count);
    return true;
}

bool build_compact_sparse_zero_payload(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known,
    bool *out_uses_varint_payload
) {
    if (out_payload == nullptr) {
        return false;
    }
    if (out_trailing_size_known != nullptr) {
        *out_trailing_size_known = false;
    }
    if (out_uses_varint_payload != nullptr) {
        *out_uses_varint_payload = false;
    }
    const auto *bytes = static_cast<const unsigned char *>(page);
    out_payload->clear();
    thread_local std::vector<unsigned char> varint_payload;
    varint_payload.clear();
    varint_payload.reserve(page_size);
    varint_payload.resize(sizeof(std::uint16_t));
    std::uint32_t run_count = 0;
    std::uint64_t trailing_size = 0U;
    std::uint32_t previous_run_end = 0U;
    std::size_t compact_payload_size = sizeof(std::uint16_t);

    for (std::uint32_t offset = 0; offset < page_size;) {
        while (offset < page_size && bytes[offset] == 0U) {
            ++offset;
        }
        if (offset == page_size) {
            break;
        }
        const std::uint32_t run_start = offset;
        while (offset < page_size && bytes[offset] != 0U) {
            ++offset;
        }
        trailing_size = offset;
        const std::uint32_t run_size = offset - run_start;
        if (run_start > std::numeric_limits<std::uint16_t>::max() ||
            run_size > std::numeric_limits<std::uint16_t>::max() ||
            run_count == std::numeric_limits<std::uint16_t>::max()) {
            out_payload->clear();
            varint_payload.clear();
            return false;
        }

        const std::size_t encoded_run_size = (2U * sizeof(std::uint16_t)) + run_size;
        if (compact_payload_size + encoded_run_size >= page_size) {
            out_payload->clear();
            varint_payload.clear();
            return false;
        }
        compact_payload_size += encoded_run_size;
        if (!append_varuint16(&varint_payload, run_start - previous_run_end) ||
            !append_varuint16(&varint_payload, run_size)) {
            out_payload->clear();
            varint_payload.clear();
            return false;
        }
        const std::size_t raw_payload_offset = varint_payload.size();
        varint_payload.resize(raw_payload_offset + run_size);
        std::memcpy(varint_payload.data() + raw_payload_offset, bytes + run_start, run_size);
        ++run_count;
        previous_run_end = offset;
    }

    if (out_trailing_size != nullptr) {
        *out_trailing_size = trailing_size;
    }
    if (out_trailing_size_known != nullptr) {
        *out_trailing_size_known = true;
    }
    if (run_count == 0U) {
        out_payload->clear();
        varint_payload.clear();
        return false;
    }
    store16(varint_payload.data(), 0U, static_cast<std::uint16_t>(run_count));
    if (varint_payload.size() < compact_payload_size) {
        out_payload->swap(varint_payload);
        if (out_uses_varint_payload != nullptr) {
            *out_uses_varint_payload = true;
        }
        return true;
    }

    out_payload->reserve(compact_payload_size);
    out_payload->resize(sizeof(std::uint16_t));
    for (std::uint32_t offset = 0; offset < page_size;) {
        while (offset < page_size && bytes[offset] == 0U) {
            ++offset;
        }
        if (offset == page_size) {
            break;
        }
        const std::uint32_t run_start = offset;
        while (offset < page_size && bytes[offset] != 0U) {
            ++offset;
        }
        const std::uint32_t run_size = offset - run_start;
        const std::size_t cursor = out_payload->size();
        out_payload->resize(cursor + (2U * sizeof(std::uint16_t)) + run_size);
        store16(out_payload->data(), cursor, static_cast<std::uint16_t>(run_start));
        store16(
            out_payload->data(),
            cursor + sizeof(std::uint16_t),
            static_cast<std::uint16_t>(run_size)
        );
        std::memcpy(
            out_payload->data() + cursor + (2U * sizeof(std::uint16_t)),
            bytes + run_start,
            run_size
        );
    }
    store16(out_payload->data(), 0U, static_cast<std::uint16_t>(run_count));
    return true;
}

bool compact_sparse_payload_fill_sparse_size(
    const std::vector<unsigned char> &payload,
    bool varint_payload,
    std::uint64_t *out_size
) {
    if (out_size == nullptr || payload.size() < sizeof(std::uint16_t)) {
        return false;
    }

    const std::uint16_t run_count = load16(payload.data(), 0U);
    std::size_t cursor = sizeof(std::uint16_t);
    bool has_previous_run = false;
    std::uint32_t previous_run_end = 0U;
    std::uint64_t fill_sparse_size = sizeof(std::uint16_t);
    std::uint32_t fill_sparse_run_count = 0U;
    std::uint32_t previous_fill_sparse_run_end = 0U;
    bool has_fill_run = false;
    for (std::uint32_t run_index = 0; run_index < run_count; ++run_index) {
        std::uint32_t run_offset = 0U;
        std::uint32_t run_size = 0U;
        if (varint_payload) {
            std::uint32_t zero_gap = 0U;
            if (!read_varuint16(payload.data(), payload.size(), &cursor, &zero_gap) ||
                !read_varuint16(payload.data(), payload.size(), &cursor, &run_size) ||
                previous_run_end > std::numeric_limits<std::uint32_t>::max() - zero_gap) {
                return false;
            }
            run_offset = previous_run_end + zero_gap;
        } else {
            if (cursor + (2U * sizeof(std::uint16_t)) > payload.size()) {
                return false;
            }
            run_offset = load16(payload.data(), cursor);
            cursor += sizeof(std::uint16_t);
            run_size = load16(payload.data(), cursor);
            cursor += sizeof(std::uint16_t);
        }
        if (run_size == 0U || (has_previous_run && run_offset <= previous_run_end) ||
            run_offset > std::numeric_limits<std::uint32_t>::max() - run_size ||
            run_size > payload.size() - cursor) {
            return false;
        }
        const unsigned char *run_bytes = payload.data() + cursor;
        for (std::uint32_t local_offset = 0U; local_offset < run_size;) {
            const unsigned char fill_byte = run_bytes[local_offset];
            std::uint32_t fill_size = 1U;
            while (local_offset + fill_size < run_size &&
                   run_bytes[local_offset + fill_size] == fill_byte) {
                ++fill_size;
            }
            if (fill_size >= k_fill_sparse_min_fill_run_size) {
                const std::uint32_t absolute_offset = run_offset + local_offset;
                if (fill_sparse_run_count == std::numeric_limits<std::uint16_t>::max() ||
                    !fill_sparse_encoded_run_size(
                        &fill_sparse_size,
                        absolute_offset - previous_fill_sparse_run_end,
                        fill_size,
                        true
                    )) {
                    return false;
                }
                ++fill_sparse_run_count;
                has_fill_run = true;
                if (fill_sparse_size >= payload.size()) {
                    return false;
                }
                local_offset += fill_size;
                previous_fill_sparse_run_end = absolute_offset + fill_size;
                continue;
            }

            const std::uint32_t raw_start = local_offset;
            local_offset += fill_size;
            while (local_offset < run_size) {
                const unsigned char next_fill_byte = run_bytes[local_offset];
                std::uint32_t next_fill_size = 1U;
                while (local_offset + next_fill_size < run_size &&
                       run_bytes[local_offset + next_fill_size] == next_fill_byte) {
                    ++next_fill_size;
                }
                if (next_fill_size >= k_fill_sparse_min_fill_run_size) {
                    break;
                }
                local_offset += next_fill_size;
            }
            const std::uint32_t raw_size = local_offset - raw_start;
            const std::uint32_t absolute_offset = run_offset + raw_start;
            if (fill_sparse_run_count == std::numeric_limits<std::uint16_t>::max() ||
                !fill_sparse_encoded_run_size(
                    &fill_sparse_size,
                    absolute_offset - previous_fill_sparse_run_end,
                    raw_size,
                    false
                )) {
                return false;
            }
            ++fill_sparse_run_count;
            if (fill_sparse_size >= payload.size()) {
                return false;
            }
            previous_fill_sparse_run_end = absolute_offset + raw_size;
        }
        cursor += run_size;
        previous_run_end = run_offset + run_size;
        has_previous_run = true;
    }
    if (cursor != payload.size() || fill_sparse_run_count == 0U || !has_fill_run) {
        return false;
    }
    *out_size = fill_sparse_size;
    return true;
}

bool fill_sparse_encoded_run_size(
    std::uint64_t *inout_size,
    std::uint32_t gap,
    std::uint32_t run_size,
    bool fill_run
) {
    if (inout_size == nullptr || run_size == 0U ||
        gap > std::numeric_limits<std::uint16_t>::max() ||
        run_size > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    const std::uint64_t encoded_size = varuint16_encoded_size(gap) +
                                       varuint16_encoded_size(run_size) + 1U +
                                       (fill_run ? 1U : run_size);
    if (*inout_size > std::numeric_limits<std::uint64_t>::max() - encoded_size) {
        return false;
    }
    *inout_size += encoded_size;
    return true;
}

std::uint32_t varuint16_encoded_size(std::uint32_t value) {
    std::uint32_t size = 0U;
    do {
        ++size;
        value >>= 7U;
    } while (value != 0U);
    return size;
}

bool build_fill_sparse_zero_payload(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload
) {
    return build_fill_sparse_zero_payload_internal(
        page,
        page_size,
        out_payload,
        false,
        nullptr,
        nullptr
    );
}

bool build_fill_sparse_zero_payload_if_smaller_than_compact(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
) {
    return build_fill_sparse_zero_payload_internal(
        page,
        page_size,
        out_payload,
        true,
        out_trailing_size,
        out_trailing_size_known
    );
}

bool build_fill_sparse_zero_payload_internal(
    const void *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload,
    bool require_smaller_than_compact,
    std::uint64_t *out_trailing_size,
    bool *out_trailing_size_known
) {
    if (out_payload == nullptr || page_size > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    if (out_trailing_size_known != nullptr) {
        *out_trailing_size_known = false;
    }

    const auto *bytes = static_cast<const unsigned char *>(page);
    out_payload->clear();
    out_payload->reserve(page_size);
    out_payload->resize(sizeof(std::uint16_t));

    std::uint32_t fill_run_count = 0U;
    std::uint32_t fill_previous_run_end = 0U;
    std::uint32_t compact_run_count = 0U;
    std::uint32_t compact_previous_run_end = 0U;
    std::uint64_t compact_payload_size = sizeof(std::uint16_t);
    std::uint64_t varint_payload_size = sizeof(std::uint16_t);
    std::uint64_t trailing_size = 0U;

    for (std::uint32_t offset = 0; offset < page_size;) {
        while (offset < page_size && bytes[offset] == 0U) {
            ++offset;
        }
        if (offset == page_size) {
            break;
        }

        const std::uint32_t compact_run_start = offset;
        while (offset < page_size && bytes[offset] != 0U) {
            const unsigned char fill_byte = bytes[offset];
            std::uint32_t fill_size = 1U;
            while (offset + fill_size < page_size && bytes[offset + fill_size] == fill_byte) {
                ++fill_size;
            }
            if (fill_size >= k_fill_sparse_min_fill_run_size) {
                if (fill_run_count == std::numeric_limits<std::uint16_t>::max() ||
                    !append_fill_sparse_run(
                        out_payload,
                        offset - fill_previous_run_end,
                        fill_size,
                        k_fill_sparse_run_kind_fill,
                        nullptr,
                        fill_byte
                    ) ||
                    out_payload->size() >= page_size) {
                    out_payload->clear();
                    return false;
                }
                ++fill_run_count;
                offset += fill_size;
                fill_previous_run_end = offset;
                continue;
            }

            const std::uint32_t raw_start = offset;
            offset += fill_size;
            while (offset < page_size && bytes[offset] != 0U) {
                const unsigned char next_fill_byte = bytes[offset];
                std::uint32_t next_fill_size = 1U;
                while (offset + next_fill_size < page_size &&
                       bytes[offset + next_fill_size] == next_fill_byte) {
                    ++next_fill_size;
                }
                if (next_fill_size >= k_fill_sparse_min_fill_run_size) {
                    break;
                }
                offset += next_fill_size;
            }
            const std::uint32_t raw_size = offset - raw_start;
            if (fill_run_count == std::numeric_limits<std::uint16_t>::max() ||
                !append_fill_sparse_run(
                    out_payload,
                    raw_start - fill_previous_run_end,
                    raw_size,
                    k_fill_sparse_run_kind_raw,
                    bytes + raw_start,
                    0U
                ) ||
                out_payload->size() >= page_size) {
                out_payload->clear();
                return false;
            }
            ++fill_run_count;
            fill_previous_run_end = offset;
        }

        if (require_smaller_than_compact) {
            trailing_size = offset;
            const std::uint32_t compact_run_size = offset - compact_run_start;
            if (compact_run_start > std::numeric_limits<std::uint16_t>::max() ||
                compact_run_size > std::numeric_limits<std::uint16_t>::max() ||
                compact_run_count == std::numeric_limits<std::uint16_t>::max()) {
                out_payload->clear();
                return false;
            }

            const std::uint64_t compact_encoded_run_size =
                (2U * sizeof(std::uint16_t)) + compact_run_size;
            if (compact_payload_size >
                    std::numeric_limits<std::uint64_t>::max() - compact_encoded_run_size ||
                compact_payload_size + compact_encoded_run_size >= page_size) {
                out_payload->clear();
                return false;
            }
            compact_payload_size += compact_encoded_run_size;

            const std::uint64_t varint_encoded_run_size =
                varuint16_encoded_size(compact_run_start - compact_previous_run_end) +
                varuint16_encoded_size(compact_run_size) + compact_run_size;
            if (varint_payload_size >
                std::numeric_limits<std::uint64_t>::max() - varint_encoded_run_size) {
                out_payload->clear();
                return false;
            }
            varint_payload_size += varint_encoded_run_size;
            ++compact_run_count;
            compact_previous_run_end = offset;
        }
    }

    if (require_smaller_than_compact) {
        if (out_trailing_size != nullptr) {
            *out_trailing_size = trailing_size;
        }
        if (out_trailing_size_known != nullptr) {
            *out_trailing_size_known = true;
        }
    }
    if (fill_run_count == 0U || (require_smaller_than_compact && compact_run_count == 0U)) {
        out_payload->clear();
        return false;
    }

    if (require_smaller_than_compact) {
        const std::uint64_t selected_compact_size =
            varint_payload_size < compact_payload_size ? varint_payload_size : compact_payload_size;
        if (selected_compact_size >= trailing_size ||
            out_payload->size() >= selected_compact_size) {
            out_payload->clear();
            return false;
        }
    }
    store16(out_payload->data(), 0U, static_cast<std::uint16_t>(fill_run_count));
    return true;
}

bool append_fill_sparse_run(
    std::vector<unsigned char> *out_payload,
    std::uint32_t gap,
    std::uint32_t run_size,
    unsigned char kind,
    const unsigned char *raw_bytes,
    unsigned char fill_byte
) {
    if (out_payload == nullptr || run_size == 0U ||
        gap > std::numeric_limits<std::uint16_t>::max() ||
        run_size > std::numeric_limits<std::uint16_t>::max() ||
        (kind != k_fill_sparse_run_kind_raw && kind != k_fill_sparse_run_kind_fill) ||
        (kind == k_fill_sparse_run_kind_raw && raw_bytes == nullptr)) {
        return false;
    }
    if (!append_varuint16(out_payload, gap) || !append_varuint16(out_payload, run_size)) {
        return false;
    }
    out_payload->push_back(kind);
    if (kind == k_fill_sparse_run_kind_fill) {
        out_payload->push_back(fill_byte);
        return true;
    }
    const std::size_t raw_payload_offset = out_payload->size();
    out_payload->resize(raw_payload_offset + run_size);
    std::memcpy(out_payload->data() + raw_payload_offset, raw_bytes, run_size);
    return true;
}

bool append_varuint16(std::vector<unsigned char> *out_payload, std::uint32_t value) {
    if (out_payload == nullptr || value > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    do {
        unsigned char byte = static_cast<unsigned char>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) {
            byte = static_cast<unsigned char>(byte | 0x80U);
        }
        out_payload->push_back(byte);
    } while (value != 0U);
    return true;
}

bool read_varuint16(
    const unsigned char *payload,
    std::size_t payload_size,
    std::size_t *inout_cursor,
    std::uint32_t *out_value
) {
    if (payload == nullptr || inout_cursor == nullptr || out_value == nullptr) {
        return false;
    }
    std::uint32_t value = 0;
    std::uint32_t shift = 0;
    for (unsigned byte_index = 0; byte_index < 3U; ++byte_index) {
        if (*inout_cursor >= payload_size) {
            return false;
        }
        const unsigned char byte = payload[*inout_cursor];
        ++(*inout_cursor);
        value |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) {
            if (value > std::numeric_limits<std::uint16_t>::max()) {
                return false;
            }
            *out_value = value;
            return true;
        }
        shift += 7U;
    }
    return false;
}

bool read_varuint16_at(
    int fd,
    off_t payload_offset,
    std::size_t payload_size,
    std::size_t *inout_cursor,
    std::uint32_t *out_value
) {
    if (inout_cursor == nullptr || out_value == nullptr) {
        return false;
    }
    std::uint32_t value = 0;
    std::uint32_t shift = 0;
    for (unsigned byte_index = 0; byte_index < 3U; ++byte_index) {
        if (*inout_cursor >= payload_size) {
            return false;
        }
        off_t byte_offset = 0;
        unsigned char byte = 0;
        if (!offset_adds(payload_offset, *inout_cursor, &byte_offset) ||
            !read_exact_at(fd, &byte, sizeof(byte), byte_offset)) {
            return false;
        }
        ++(*inout_cursor);
        value |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) {
            if (value > std::numeric_limits<std::uint16_t>::max()) {
                return false;
            }
            *out_value = value;
            return true;
        }
        shift += 7U;
    }
    return false;
}

bool record_uses_trailing_zero_payload(const PageRecordHeader &record) {
    return (record.flags & k_record_flag_trailing_zero_payload) != 0U;
}

bool record_uses_sparse_zero_payload(const PageRecordHeader &record) {
    return (record.flags & k_record_flag_sparse_zero_payload) != 0U;
}

bool record_uses_compact_sparse_zero_payload(const PageRecordHeader &record) {
    return (record.flags & k_record_flag_compact_sparse_zero_payload) != 0U;
}

bool record_uses_varint_compact_sparse_zero_payload(const PageRecordHeader &record) {
    return (record.flags & k_record_flag_varint_compact_sparse_zero_payload) != 0U;
}

bool record_uses_fill_sparse_zero_payload(const PageRecordHeader &record) {
    return (record.flags & k_record_flag_fill_sparse_zero_payload) != 0U;
}

bool record_uses_index_delta_payload(const PageRecordHeader &record) {
    return (record.flags & k_record_flag_index_delta_payload) != 0U;
}

bool record_uses_undo_delta_payload(const PageRecordHeader &record) {
    return (record.flags & k_record_flag_undo_delta_payload) != 0U;
}

bool record_uses_any_delta_payload(const PageRecordHeader &record) {
    return record_uses_index_delta_payload(record) || record_uses_undo_delta_payload(record);
}

bool record_uses_any_sparse_zero_payload(const PageRecordHeader &record) {
    return record_uses_sparse_zero_payload(record) ||
           record_uses_compact_sparse_zero_payload(record) ||
           record_uses_varint_compact_sparse_zero_payload(record) ||
           record_uses_fill_sparse_zero_payload(record);
}

CompactSparsePayloadComposition compact_sparse_payload_composition(
    std::uint32_t flags,
    const std::vector<unsigned char> &payload
) {
    CompactSparsePayloadComposition composition;
    const bool compact_sparse = (flags & k_record_flag_compact_sparse_zero_payload) != 0U;
    const bool varint_compact_sparse =
        (flags & k_record_flag_varint_compact_sparse_zero_payload) != 0U;
    if ((!compact_sparse && !varint_compact_sparse) || payload.size() < sizeof(std::uint16_t)) {
        return composition;
    }

    const std::uint16_t run_count = load16(payload.data(), 0U);
    std::uint64_t metadata_bytes = sizeof(std::uint16_t);
    if (varint_compact_sparse) {
        std::size_t cursor = sizeof(std::uint16_t);
        bool has_previous_run = false;
        std::uint32_t previous_run_end = 0;
        for (std::uint32_t run_index = 0; run_index < run_count; ++run_index) {
            const std::size_t metadata_start = cursor;
            std::uint32_t zero_gap = 0;
            std::uint32_t run_size = 0;
            if (!read_varuint16(payload.data(), payload.size(), &cursor, &zero_gap) ||
                !read_varuint16(payload.data(), payload.size(), &cursor, &run_size) ||
                previous_run_end > std::numeric_limits<std::uint32_t>::max() - zero_gap) {
                return composition;
            }
            const std::uint32_t run_offset = previous_run_end + zero_gap;
            if (run_size == 0U || (has_previous_run && run_offset <= previous_run_end) ||
                run_size > payload.size() - cursor) {
                return composition;
            }
            metadata_bytes += cursor - metadata_start;
            cursor += run_size;
            previous_run_end = run_offset + run_size;
            has_previous_run = true;
        }
        if (cursor != payload.size()) {
            return composition;
        }
    } else {
        metadata_bytes += static_cast<std::uint64_t>(run_count) * 2U * sizeof(std::uint16_t);
    }
    if (metadata_bytes > payload.size()) {
        return composition;
    }

    composition.valid = true;
    composition.metadata_bytes = metadata_bytes;
    composition.data_bytes = static_cast<std::uint64_t>(payload.size()) - metadata_bytes;
    return composition;
}

CompactSparsePayloadComposition fill_sparse_payload_composition(
    const std::vector<unsigned char> &payload
) {
    CompactSparsePayloadComposition composition;
    if (payload.size() < sizeof(std::uint16_t)) {
        return composition;
    }

    const std::uint16_t run_count = load16(payload.data(), 0U);
    std::uint64_t metadata_bytes = sizeof(std::uint16_t);
    std::uint64_t raw_data_bytes = 0;
    std::size_t cursor = sizeof(std::uint16_t);
    bool has_previous_run = false;
    std::uint32_t previous_run_end = 0;
    for (std::uint32_t run_index = 0; run_index < run_count; ++run_index) {
        const std::size_t metadata_start = cursor;
        std::uint32_t zero_gap = 0;
        std::uint32_t run_size = 0;
        if (!read_varuint16(payload.data(), payload.size(), &cursor, &zero_gap) ||
            !read_varuint16(payload.data(), payload.size(), &cursor, &run_size) ||
            previous_run_end > std::numeric_limits<std::uint32_t>::max() - zero_gap ||
            cursor >= payload.size()) {
            return composition;
        }
        const std::uint32_t run_offset = previous_run_end + zero_gap;
        const unsigned char kind = payload[cursor++];
        if (run_size == 0U || (has_previous_run && run_offset < previous_run_end) ||
            run_offset > std::numeric_limits<std::uint32_t>::max() - run_size ||
            (kind != k_fill_sparse_run_kind_raw && kind != k_fill_sparse_run_kind_fill)) {
            return composition;
        }
        metadata_bytes += cursor - metadata_start;
        if (kind == k_fill_sparse_run_kind_fill) {
            if (cursor >= payload.size()) {
                return composition;
            }
            ++cursor;
        } else {
            if (run_size > payload.size() - cursor) {
                return composition;
            }
            cursor += run_size;
            raw_data_bytes += run_size;
        }
        previous_run_end = run_offset + run_size;
        has_previous_run = true;
    }
    if (cursor != payload.size() || metadata_bytes > payload.size()) {
        return composition;
    }

    composition.valid = true;
    composition.metadata_bytes = metadata_bytes;
    composition.data_bytes = raw_data_bytes;
    return composition;
}

CompactSparsePayloadComposition record_append_payload_encoding_stats(
    std::uint32_t flags,
    std::uint64_t payload_size,
    const std::vector<unsigned char> &payload
) {
    CompactSparsePayloadComposition compact_sparse;
    if (!page_log_append_perf_stats_are_enabled()) {
        return compact_sparse;
    }
    if ((flags & k_record_flag_index_delta_payload) != 0U) {
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_INDEX_DELTA_RECORDS, 1U);
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_INDEX_DELTA_PAYLOAD_BYTES, payload_size);
        return compact_sparse;
    }
    if ((flags & k_record_flag_undo_delta_payload) != 0U) {
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_UNDO_DELTA_RECORDS, 1U);
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_UNDO_DELTA_PAYLOAD_BYTES, payload_size);
        return compact_sparse;
    }
    if ((flags & (k_record_flag_sparse_zero_payload | k_record_flag_compact_sparse_zero_payload |
                  k_record_flag_varint_compact_sparse_zero_payload |
                  k_record_flag_fill_sparse_zero_payload)) != 0U) {
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_SPARSE_ZERO_RECORDS, 1U);
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_SPARSE_ZERO_PAYLOAD_BYTES, payload_size);
        if ((flags & (k_record_flag_compact_sparse_zero_payload |
                      k_record_flag_varint_compact_sparse_zero_payload)) != 0U) {
            compact_sparse = compact_sparse_payload_composition(flags, payload);
            page_log_append_perf_add(PAGE_LOG_APPEND_PERF_COMPACT_SPARSE_ZERO_RECORDS, 1U);
            page_log_append_perf_add(
                PAGE_LOG_APPEND_PERF_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES,
                payload_size
            );
            if ((flags & k_record_flag_varint_compact_sparse_zero_payload) != 0U) {
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_VARINT_COMPACT_SPARSE_ZERO_RECORDS,
                    1U
                );
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_VARINT_COMPACT_SPARSE_ZERO_PAYLOAD_BYTES,
                    payload_size
                );
            }
            if (compact_sparse.valid) {
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_COMPACT_SPARSE_METADATA_BYTES,
                    compact_sparse.metadata_bytes
                );
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_COMPACT_SPARSE_DATA_BYTES,
                    compact_sparse.data_bytes
                );
            }
        }
        if ((flags & k_record_flag_fill_sparse_zero_payload) != 0U) {
            const CompactSparsePayloadComposition fill_sparse =
                fill_sparse_payload_composition(payload);
            page_log_append_perf_add(PAGE_LOG_APPEND_PERF_FILL_SPARSE_ZERO_RECORDS, 1U);
            page_log_append_perf_add(
                PAGE_LOG_APPEND_PERF_FILL_SPARSE_ZERO_PAYLOAD_BYTES,
                payload_size
            );
            if (fill_sparse.valid) {
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_FILL_SPARSE_METADATA_BYTES,
                    fill_sparse.metadata_bytes
                );
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_FILL_SPARSE_RAW_DATA_BYTES,
                    fill_sparse.data_bytes
                );
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_FILL_SPARSE_FILL_BYTES,
                    payload_size - fill_sparse.metadata_bytes - fill_sparse.data_bytes
                );
            }
        }
        return compact_sparse;
    }
    if ((flags & k_record_flag_trailing_zero_payload) != 0U) {
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_TRAILING_ZERO_RECORDS, 1U);
        page_log_append_perf_add(PAGE_LOG_APPEND_PERF_TRAILING_ZERO_PAYLOAD_BYTES, payload_size);
        return compact_sparse;
    }
    page_log_append_perf_add(PAGE_LOG_APPEND_PERF_FULL_RECORDS, 1U);
    page_log_append_perf_add(PAGE_LOG_APPEND_PERF_FULL_PAYLOAD_BYTES, payload_size);
    return compact_sparse;
}

void record_append_page_type_stats(
    std::uint32_t space_id,
    std::uint32_t page_no,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t payload_size,
    const CompactSparsePayloadComposition &compact_sparse
) {
    if (!page_log_append_detail_perf_stats_are_enabled()) {
        return;
    }

    PageLogAppendPerfStatIndex record_index = PAGE_LOG_APPEND_PERF_OTHER_RECORDS;
    PageLogAppendPerfStatIndex byte_index = PAGE_LOG_APPEND_PERF_OTHER_PAYLOAD_BYTES;
    PageLogAppendPerfStatIndex compact_metadata_index =
        PAGE_LOG_APPEND_PERF_OTHER_COMPACT_SPARSE_METADATA_BYTES;
    PageLogAppendPerfStatIndex compact_data_index =
        PAGE_LOG_APPEND_PERF_OTHER_COMPACT_SPARSE_DATA_BYTES;
    if (page != nullptr && page_size >= k_innodb_fil_page_type_offset + sizeof(std::uint16_t)) {
        const auto *bytes = static_cast<const unsigned char *>(page);
        switch (load_be16(bytes + k_innodb_fil_page_type_offset)) {
        case k_innodb_fil_page_index:
            record_index = PAGE_LOG_APPEND_PERF_INDEX_RECORDS;
            byte_index = PAGE_LOG_APPEND_PERF_INDEX_PAYLOAD_BYTES;
            compact_metadata_index = PAGE_LOG_APPEND_PERF_INDEX_COMPACT_SPARSE_METADATA_BYTES;
            compact_data_index = PAGE_LOG_APPEND_PERF_INDEX_COMPACT_SPARSE_DATA_BYTES;
            record_append_index_page_identity_stats(space_id, page_no, bytes, page_size);
            break;
        case k_innodb_fil_page_undo_log:
            record_index = PAGE_LOG_APPEND_PERF_UNDO_LOG_RECORDS;
            byte_index = PAGE_LOG_APPEND_PERF_UNDO_LOG_PAYLOAD_BYTES;
            compact_metadata_index = PAGE_LOG_APPEND_PERF_UNDO_LOG_COMPACT_SPARSE_METADATA_BYTES;
            compact_data_index = PAGE_LOG_APPEND_PERF_UNDO_LOG_COMPACT_SPARSE_DATA_BYTES;
            break;
        case k_innodb_fil_page_type_sys:
            record_index = PAGE_LOG_APPEND_PERF_SYS_RECORDS;
            byte_index = PAGE_LOG_APPEND_PERF_SYS_PAYLOAD_BYTES;
            compact_metadata_index = PAGE_LOG_APPEND_PERF_SYS_COMPACT_SPARSE_METADATA_BYTES;
            compact_data_index = PAGE_LOG_APPEND_PERF_SYS_COMPACT_SPARSE_DATA_BYTES;
            break;
        case k_innodb_fil_page_type_trx_sys:
            record_index = PAGE_LOG_APPEND_PERF_TRX_SYS_RECORDS;
            byte_index = PAGE_LOG_APPEND_PERF_TRX_SYS_PAYLOAD_BYTES;
            compact_metadata_index = PAGE_LOG_APPEND_PERF_TRX_SYS_COMPACT_SPARSE_METADATA_BYTES;
            compact_data_index = PAGE_LOG_APPEND_PERF_TRX_SYS_COMPACT_SPARSE_DATA_BYTES;
            break;
        case k_innodb_fil_page_type_allocated:
        case k_innodb_fil_page_inode:
        case k_innodb_fil_page_ibuf_free_list:
        case k_innodb_fil_page_ibuf_bitmap:
        case k_innodb_fil_page_type_fsp_hdr:
        case k_innodb_fil_page_type_xdes:
            record_index = PAGE_LOG_APPEND_PERF_SPACE_METADATA_RECORDS;
            byte_index = PAGE_LOG_APPEND_PERF_SPACE_METADATA_PAYLOAD_BYTES;
            compact_metadata_index =
                PAGE_LOG_APPEND_PERF_SPACE_METADATA_COMPACT_SPARSE_METADATA_BYTES;
            compact_data_index = PAGE_LOG_APPEND_PERF_SPACE_METADATA_COMPACT_SPARSE_DATA_BYTES;
            break;
        case k_innodb_fil_page_type_blob:
        case k_innodb_fil_page_type_zblob:
        case k_innodb_fil_page_type_zblob2:
            record_index = PAGE_LOG_APPEND_PERF_BLOB_RECORDS;
            byte_index = PAGE_LOG_APPEND_PERF_BLOB_PAYLOAD_BYTES;
            compact_metadata_index = PAGE_LOG_APPEND_PERF_BLOB_COMPACT_SPARSE_METADATA_BYTES;
            compact_data_index = PAGE_LOG_APPEND_PERF_BLOB_COMPACT_SPARSE_DATA_BYTES;
            break;
        default:
            break;
        }
    }

    page_log_append_perf_add(record_index, 1U);
    page_log_append_perf_add(byte_index, payload_size);
    if (compact_sparse.valid) {
        page_log_append_perf_add(compact_metadata_index, compact_sparse.metadata_bytes);
        page_log_append_perf_add(compact_data_index, compact_sparse.data_bytes);
    }
}

void record_append_index_page_identity_stats(
    std::uint32_t space_id,
    std::uint32_t page_no,
    const unsigned char *page,
    std::uint32_t page_size
) {
    if (page == nullptr || page_size == 0U) {
        return;
    }

    const std::uint64_t fingerprint = index_page_identity_fingerprint(space_id, page_no);
    const std::size_t first_slot =
        static_cast<std::size_t>(fingerprint) & (k_index_page_identity_slot_count - 1U);
    std::lock_guard<std::mutex> guard(index_page_identity_stats_mutex);
    for (std::size_t attempt = 0; attempt < k_index_page_identity_probe_limit; ++attempt) {
        IndexPageIdentitySlot &slot = index_page_identity_slots
            [(first_slot + attempt) & (k_index_page_identity_slot_count - 1U)];
        if (slot.valid && slot.space_id == space_id && slot.page_no == page_no) {
            if (slot.page_size != page_size || slot.page.size() != page_size) {
                page_log_append_perf_add(PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_SIZE_MISMATCH, 1U);
            } else {
                std::uint64_t changed_bytes = 0;
                std::uint64_t fil_header_changed_bytes = 0;
                std::uint64_t body_changed_bytes = 0;
                for (std::uint32_t offset = 0; offset < page_size; ++offset) {
                    if (slot.page[offset] == page[offset]) {
                        continue;
                    }
                    ++changed_bytes;
                    if (offset < k_innodb_fil_page_data_offset) {
                        ++fil_header_changed_bytes;
                    } else {
                        ++body_changed_bytes;
                    }
                }
                page_log_append_perf_add(PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_DUPLICATE, 1U);
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_CHANGED_BYTES,
                    changed_bytes
                );
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_FIL_HEADER_CHANGED_BYTES,
                    fil_header_changed_bytes
                );
                page_log_append_perf_add(
                    PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_BODY_CHANGED_BYTES,
                    body_changed_bytes
                );
            }
            slot.page_size = page_size;
            try {
                slot.page.assign(page, page + page_size);
            } catch (const std::bad_alloc &) {
                slot.valid = false;
                slot.page_size = 0;
                slot.page.clear();
            }
            return;
        }
        if (!slot.valid) {
            slot.space_id = space_id;
            slot.page_no = page_no;
            slot.page_size = page_size;
            try {
                slot.page.assign(page, page + page_size);
            } catch (const std::bad_alloc &) {
                slot.page_size = 0;
                slot.page.clear();
                page_log_append_perf_add(PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_TABLE_OVERFLOW, 1U);
                return;
            }
            slot.valid = true;
            page_log_append_perf_add(PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_UNIQUE, 1U);
            return;
        }
    }

    page_log_append_perf_add(PAGE_LOG_APPEND_PERF_INDEX_IDENTITY_TABLE_OVERFLOW, 1U);
}

std::uint64_t index_page_identity_fingerprint(std::uint32_t space_id, std::uint32_t page_no) {
    std::uint64_t value = (static_cast<std::uint64_t>(space_id) << 32) | page_no;
    value = mix64(value);
    return value == 0U ? 1U : value;
}

std::uint64_t mix64(std::uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

bool page_is_innodb_index_page(const void *page, std::uint32_t page_size) {
    if (page == nullptr || page_size < k_innodb_fil_page_type_offset + sizeof(std::uint16_t)) {
        return false;
    }
    const auto *bytes = static_cast<const unsigned char *>(page);
    return load_be16(bytes + k_innodb_fil_page_type_offset) == k_innodb_fil_page_index;
}

bool page_is_innodb_undo_log_page(const void *page, std::uint32_t page_size) {
    if (page == nullptr || page_size < k_innodb_fil_page_type_offset + sizeof(std::uint16_t)) {
        return false;
    }
    const auto *bytes = static_cast<const unsigned char *>(page);
    return load_be16(bytes + k_innodb_fil_page_type_offset) == k_innodb_fil_page_undo_log;
}

bool page_delta_flag_for_page(
    std::uint32_t space_id,
    const void *page,
    std::uint32_t page_size,
    std::uint32_t *out_delta_flag
) {
    if (out_delta_flag == nullptr) {
        return false;
    }
    *out_delta_flag = 0U;
    if (page_size > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    if (page_is_innodb_index_page(page, page_size)) {
        if (space_id == k_innodb_system_space_id) {
            return false;
        }
        *out_delta_flag = k_record_flag_index_delta_payload;
        return true;
    }
    if (page_is_innodb_undo_log_page(page, page_size)) {
        *out_delta_flag = k_record_flag_undo_delta_payload;
        return true;
    }
    return false;
}

bool maybe_encode_page_delta_payload(
    const IndexPageDeltaBaseSnapshot &snapshot,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t standalone_payload_size,
    bool enforce_fast_limit,
    std::uint32_t *inout_flags,
    std::vector<unsigned char> *inout_payload,
    PageDeltaEncodeDecision *out_decision,
    std::uint64_t *out_delta_payload_size,
    std::vector<unsigned char> *out_rejected_payload
) {
    if (out_decision != nullptr) {
        *out_decision = PageDeltaEncodeDecision::Ineligible;
    }
    if (out_delta_payload_size != nullptr) {
        *out_delta_payload_size = 0U;
    }
    if (out_rejected_payload != nullptr) {
        out_rejected_payload->clear();
    }
    if (inout_flags == nullptr || inout_payload == nullptr || !snapshot.found ||
        snapshot.standalone_payload_size == 0U || snapshot.page == nullptr ||
        snapshot.page->size() != page_size) {
        return false;
    }
    if (snapshot.delta_flag != k_record_flag_index_delta_payload &&
        snapshot.delta_flag != k_record_flag_undo_delta_payload) {
        return false;
    }

    thread_local std::vector<unsigned char> delta_payload;
    delta_payload.clear();
    if (!build_index_delta_payload(
            snapshot.record_offset,
            *snapshot.page,
            static_cast<const unsigned char *>(page),
            page_size,
            &delta_payload
        )) {
        if (out_decision != nullptr) {
            *out_decision = PageDeltaEncodeDecision::BuildFailed;
        }
        delta_payload.clear();
        return false;
    }
    if (out_delta_payload_size != nullptr) {
        *out_delta_payload_size = delta_payload.size();
    }
    if (enforce_fast_limit && delta_payload.size() > k_index_delta_fast_payload_size_limit) {
        if (out_decision != nullptr) {
            *out_decision = PageDeltaEncodeDecision::FastLimit;
        }
        if (out_rejected_payload != nullptr) {
            out_rejected_payload->swap(delta_payload);
        } else {
            delta_payload.clear();
        }
        return false;
    }
    if (!index_delta_payload_beats_standalone(delta_payload.size(), standalone_payload_size)) {
        if (out_decision != nullptr) {
            *out_decision = PageDeltaEncodeDecision::Standalone;
        }
        if (out_rejected_payload != nullptr) {
            out_rejected_payload->swap(delta_payload);
        } else {
            delta_payload.clear();
        }
        return false;
    }

    inout_payload->swap(delta_payload);
    *inout_flags = snapshot.delta_flag;
    if (out_decision != nullptr) {
        *out_decision = PageDeltaEncodeDecision::Encoded;
    }
    return true;
}

bool index_delta_base_snapshot_for_page(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset,
    std::uint64_t log_generation,
    std::uint32_t space_id,
    std::uint32_t page_no,
    const void *page,
    std::uint32_t page_size,
    IndexPageDeltaBaseSnapshot *out_snapshot
) {
    if (out_snapshot == nullptr) {
        return false;
    }
    out_snapshot->found = false;
    out_snapshot->delta_flag = 0U;
    out_snapshot->record_offset = 0U;
    out_snapshot->standalone_payload_size = 0U;
    out_snapshot->page.reset();
    std::uint32_t delta_flag = 0U;
    if (!page_delta_flag_for_page(space_id, page, page_size, &delta_flag)) {
        return false;
    }
    return index_delta_base_snapshot(
        log_device,
        log_inode,
        log_offset,
        log_generation,
        delta_flag,
        space_id,
        page_no,
        page_size,
        out_snapshot
    );
}

bool index_delta_payload_beats_standalone(
    std::size_t delta_payload_size,
    std::uint64_t standalone_payload_size
) {
    return delta_payload_size > 0U &&
           static_cast<std::uint64_t>(delta_payload_size) * 2U < standalone_payload_size;
}

bool delta_pages_word_equal(
    const unsigned char *base_page,
    const unsigned char *page,
    std::uint32_t offset
) {
    std::uint64_t base_word = 0;
    std::uint64_t page_word = 0;
    std::memcpy(&base_word, base_page + offset, sizeof(base_word));
    std::memcpy(&page_word, page + offset, sizeof(page_word));
    return base_word == page_word;
}

bool build_index_delta_payload(
    std::uint64_t base_record_offset,
    const std::vector<unsigned char> &base_page,
    const unsigned char *page,
    std::uint32_t page_size,
    std::vector<unsigned char> *out_payload
) {
    if (page == nullptr || out_payload == nullptr || base_page.size() != page_size ||
        page_size > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }

    thread_local std::vector<IndexPageDeltaRun> runs;
    runs.clear();
    std::uint32_t previous_run_end = 0U;
    std::uint64_t raw_bytes = 0U;
    for (std::uint32_t offset = 0; offset < page_size;) {
        while (offset + sizeof(std::uint64_t) <= page_size &&
               delta_pages_word_equal(base_page.data(), page, offset)) {
            offset += sizeof(std::uint64_t);
        }
        while (offset < page_size && base_page[offset] == page[offset]) {
            ++offset;
        }
        if (offset == page_size) {
            break;
        }
        const std::uint32_t run_start = offset;
        while (offset < page_size && base_page[offset] != page[offset]) {
            ++offset;
        }
        const std::uint32_t run_size = offset - run_start;
        if (runs.size() == std::numeric_limits<std::uint16_t>::max() ||
            run_start < previous_run_end ||
            run_start - previous_run_end > std::numeric_limits<std::uint16_t>::max() ||
            run_size == 0U || run_size > std::numeric_limits<std::uint16_t>::max()) {
            runs.clear();
            out_payload->clear();
            return false;
        }
        runs.push_back({run_start, run_size});
        previous_run_end = offset;
        raw_bytes += run_size;
    }
    if (runs.empty()) {
        out_payload->clear();
        return false;
    }

    out_payload->clear();
    out_payload->reserve(
        k_index_delta_base_record_offset_size + sizeof(std::uint16_t) + (runs.size() * 2U * 3U) +
        static_cast<std::size_t>(raw_bytes)
    );
    out_payload->resize(k_index_delta_base_record_offset_size + sizeof(std::uint16_t));
    store64(out_payload->data(), 0U, base_record_offset);
    store16(
        out_payload->data(),
        k_index_delta_base_record_offset_size,
        static_cast<std::uint16_t>(runs.size())
    );
    previous_run_end = 0U;
    for (const IndexPageDeltaRun &run : runs) {
        if (!append_varuint16(out_payload, run.offset - previous_run_end) ||
            !append_varuint16(out_payload, run.size)) {
            out_payload->clear();
            runs.clear();
            return false;
        }
        previous_run_end = run.offset + run.size;
    }
    const std::size_t raw_payload_offset = out_payload->size();
    out_payload->resize(raw_payload_offset + static_cast<std::size_t>(raw_bytes));
    unsigned char *raw_payload = out_payload->data() + raw_payload_offset;
    for (const IndexPageDeltaRun &run : runs) {
        std::memcpy(raw_payload, page + run.offset, run.size);
        raw_payload += run.size;
    }
    runs.clear();
    return out_payload->size() < page_size;
}

bool index_delta_base_snapshot(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset,
    std::uint64_t log_generation,
    std::uint32_t delta_flag,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t page_size,
    IndexPageDeltaBaseSnapshot *out_snapshot
) {
    if (out_snapshot == nullptr) {
        return false;
    }
    out_snapshot->found = false;
    out_snapshot->delta_flag = 0U;
    out_snapshot->record_offset = 0U;
    out_snapshot->standalone_payload_size = 0U;
    out_snapshot->page.reset();
    if (delta_flag != k_record_flag_index_delta_payload &&
        delta_flag != k_record_flag_undo_delta_payload) {
        return false;
    }

    const std::uint64_t fingerprint = index_delta_base_fingerprint(
        log_device,
        log_inode,
        log_offset,
        log_generation,
        delta_flag,
        space_id,
        page_no
    );
    const std::size_t first_slot =
        static_cast<std::size_t>(fingerprint) & (k_index_delta_base_slot_count - 1U);
    std::lock_guard<std::mutex> guard(index_page_delta_base_mutex);
    for (std::size_t attempt = 0; attempt < k_index_delta_base_probe_limit; ++attempt) {
        const IndexPageDeltaBaseSlot &slot = index_page_delta_base_slots
            [(first_slot + attempt) & (k_index_delta_base_slot_count - 1U)];
        if (!slot.valid) {
            continue;
        }
        if (slot.log_device == log_device && slot.log_inode == log_inode &&
            slot.log_offset == log_offset && slot.log_generation == log_generation &&
            slot.delta_flag == delta_flag && slot.space_id == space_id && slot.page_no == page_no &&
            slot.page_size == page_size && slot.page != nullptr && slot.page->size() == page_size) {
            if (slot.standalone_observations < k_index_delta_base_min_standalone_observations) {
                return false;
            }
            if (slot.delta_records_since_base >= k_index_delta_base_max_delta_records) {
                return false;
            }
            out_snapshot->page = slot.page;
            out_snapshot->delta_flag = delta_flag;
            out_snapshot->record_offset = slot.record_offset;
            out_snapshot->standalone_payload_size = slot.standalone_payload_size;
            out_snapshot->found = true;
            return true;
        }
    }
    return false;
}

void note_index_delta_base_after_successful_append(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset,
    std::uint64_t log_generation,
    std::uint32_t space_id,
    std::uint32_t page_no,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t record_offset,
    std::uint64_t record_payload_size,
    std::uint32_t record_flags,
    std::uint64_t observed_standalone_payload_size
) {
    std::uint32_t delta_flag = 0U;
    if (!page_delta_flag_for_page(space_id, page, page_size, &delta_flag)) {
        return;
    }
    const std::uint32_t record_delta_flag =
        record_flags & (k_record_flag_index_delta_payload | k_record_flag_undo_delta_payload);
    if (record_delta_flag != 0U && record_delta_flag != delta_flag) {
        return;
    }
    const std::uint64_t fingerprint = index_delta_base_fingerprint(
        log_device,
        log_inode,
        log_offset,
        log_generation,
        delta_flag,
        space_id,
        page_no
    );
    const std::size_t first_slot =
        static_cast<std::size_t>(fingerprint) & (k_index_delta_base_slot_count - 1U);
    std::lock_guard<std::mutex> guard(index_page_delta_base_mutex);
    if (record_delta_flag != 0U) {
        for (std::size_t attempt = 0; attempt < k_index_delta_base_probe_limit; ++attempt) {
            IndexPageDeltaBaseSlot &slot = index_page_delta_base_slots
                [(first_slot + attempt) & (k_index_delta_base_slot_count - 1U)];
            if (slot.valid && slot.log_device == log_device && slot.log_inode == log_inode &&
                slot.log_offset == log_offset && slot.log_generation == log_generation &&
                slot.delta_flag == delta_flag && slot.space_id == space_id &&
                slot.page_no == page_no && slot.page_size == page_size && slot.page != nullptr &&
                slot.page->size() == page_size &&
                slot.delta_records_since_base < std::numeric_limits<std::uint32_t>::max()) {
                if (observed_standalone_payload_size != 0U) {
                    slot.standalone_payload_size = observed_standalone_payload_size;
                    if (slot.standalone_observations < std::numeric_limits<std::uint32_t>::max()) {
                        ++slot.standalone_observations;
                    }
                }
                ++slot.delta_records_since_base;
                return;
            }
        }
        return;
    }

    const auto *bytes = static_cast<const unsigned char *>(page);
    for (std::size_t attempt = 0; attempt < k_index_delta_base_probe_limit; ++attempt) {
        IndexPageDeltaBaseSlot &slot = index_page_delta_base_slots
            [(first_slot + attempt) & (k_index_delta_base_slot_count - 1U)];
        const bool matching_slot =
            slot.valid && slot.log_device == log_device && slot.log_inode == log_inode &&
            slot.log_offset == log_offset && slot.log_generation == log_generation &&
            slot.delta_flag == delta_flag && slot.space_id == space_id && slot.page_no == page_no;
        if (matching_slot || !slot.valid) {
            const bool matching_page_size = matching_slot && slot.page_size == page_size &&
                                            slot.page != nullptr && slot.page->size() == page_size;
            slot.log_device = log_device;
            slot.log_inode = log_inode;
            slot.log_offset = log_offset;
            slot.log_generation = log_generation;
            slot.delta_flag = delta_flag;
            slot.space_id = space_id;
            slot.page_no = page_no;
            slot.page_size = page_size;
            slot.record_offset = record_offset;
            slot.standalone_payload_size = record_payload_size;
            if (!matching_page_size) {
                slot.standalone_observations = 1U;
            } else if (slot.standalone_observations < std::numeric_limits<std::uint32_t>::max()) {
                ++slot.standalone_observations;
            }
            slot.delta_records_since_base = 0U;
            try {
                slot.page = std::make_shared<std::vector<unsigned char>>(bytes, bytes + page_size);
                slot.valid = true;
            } catch (const std::bad_alloc &) {
                slot.valid = false;
                slot.page_size = 0U;
                slot.delta_flag = 0U;
                slot.record_offset = 0U;
                slot.standalone_payload_size = 0U;
                slot.standalone_observations = 0U;
                slot.delta_records_since_base = 0U;
                slot.page.reset();
            }
            return;
        }
    }
}

std::uint64_t index_delta_base_fingerprint(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset,
    std::uint64_t log_generation,
    std::uint32_t delta_flag,
    std::uint32_t space_id,
    std::uint32_t page_no
) {
    std::uint64_t value = mix64(log_device);
    value ^= mix64(log_inode + 0x9e3779b97f4a7c15ULL);
    value ^= mix64(log_offset + 0xbf58476d1ce4e5b9ULL);
    value ^= mix64(log_generation + 0x94d049bb133111ebULL);
    value ^= mix64(static_cast<std::uint64_t>(delta_flag) + 0xd6e8feb86659fd93ULL);
    value ^= mix64((static_cast<std::uint64_t>(space_id) << 32U) | page_no);
    value = mix64(value);
    return value == 0U ? 1U : value;
}

void invalidate_index_delta_bases_for_log(
    std::uint64_t log_device,
    std::uint64_t log_inode,
    std::uint64_t log_offset
) {
    std::lock_guard<std::mutex> guard(index_page_delta_base_mutex);
    for (IndexPageDeltaBaseSlot &slot : index_page_delta_base_slots) {
        if (slot.valid && slot.log_device == log_device && slot.log_inode == log_inode &&
            slot.log_offset == log_offset) {
            slot.valid = false;
            slot.log_generation = 0U;
            slot.delta_flag = 0U;
            slot.space_id = 0U;
            slot.page_no = 0U;
            slot.page_size = 0U;
            slot.record_offset = 0U;
            slot.standalone_payload_size = 0U;
            slot.standalone_observations = 0U;
            slot.delta_records_since_base = 0U;
            slot.page.reset();
        }
    }
}

bool record_payload_shape_valid(const PageRecordHeader &record) {
    if (record.page_size == 0U || (record.flags & ~k_record_flags_known_mask) != 0U) {
        return false;
    }
    const std::uint32_t encoding_flags = record.flags & k_record_encoding_flags;
    if ((encoding_flags & (encoding_flags - 1U)) != 0U) {
        return false;
    }
    if (record_uses_any_delta_payload(record)) {
        return record.payload_size >=
                   k_index_delta_base_record_offset_size + sizeof(std::uint16_t) &&
               record.payload_size < record.page_size;
    }
    if (record_uses_sparse_zero_payload(record)) {
        return record.payload_size >= sizeof(std::uint32_t) &&
               record.payload_size < record.page_size;
    }
    if (record_uses_compact_sparse_zero_payload(record)) {
        return record.payload_size >= sizeof(std::uint16_t) &&
               record.payload_size < record.page_size;
    }
    if (record_uses_varint_compact_sparse_zero_payload(record)) {
        return record.payload_size >= sizeof(std::uint16_t) &&
               record.payload_size < record.page_size;
    }
    if (record_uses_fill_sparse_zero_payload(record)) {
        return record.payload_size >= sizeof(std::uint16_t) &&
               record.payload_size < record.page_size;
    }
    if (record_uses_trailing_zero_payload(record)) {
        return record.payload_size < record.page_size;
    }
    return encoding_flags == 0U && record.payload_size == record.page_size;
}

bool decode_page_delta_payload(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    void *out_page,
    std::size_t page_capacity
) {
    if (!record_payload_shape_valid(record) || !record_uses_any_delta_payload(record) ||
        record_page_too_large(record, page_capacity)) {
        return false;
    }
    if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
        if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
    }
    if (payload_offset < static_cast<off_t>(MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE)) {
        return false;
    }

    const std::size_t payload_size = static_cast<std::size_t>(record.payload_size);
    std::vector<unsigned char> payload;
    try {
        payload.resize(payload_size);
    } catch (const std::bad_alloc &) {
        return false;
    }
    if (!read_exact_at(fd, payload.data(), payload.size(), payload_offset)) {
        return false;
    }

    const std::uint64_t base_record_offset_value = load64(payload.data(), 0U);
    if (base_record_offset_value > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return false;
    }
    const auto base_record_offset = static_cast<off_t>(base_record_offset_value);
    const off_t delta_record_offset =
        payload_offset - static_cast<off_t>(MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE);
    if (base_record_offset >= delta_record_offset) {
        return false;
    }

    PageRecordHeader base = {};
    off_t base_payload_offset = 0;
    off_t base_next_record_offset = 0;
    if (!offset_adds(
            base_record_offset,
            MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
            &base_payload_offset
        ) ||
        !read_record_header(fd, base_record_offset, base) ||
        !offset_adds(base_payload_offset, base.payload_size, &base_next_record_offset) ||
        base_next_record_offset > delta_record_offset || record_uses_any_delta_payload(base) ||
        base.space_id != record.space_id || base.page_no != record.page_no ||
        base.page_size != record.page_size) {
        return false;
    }
    if (!read_non_delta_record_page_payload(
            fd,
            base_payload_offset,
            base,
            out_page,
            page_capacity
        )) {
        return false;
    }

    const std::uint16_t run_count = load16(payload.data(), k_index_delta_base_record_offset_size);
    if (run_count == 0U) {
        return false;
    }
    std::size_t cursor = k_index_delta_base_record_offset_size + sizeof(std::uint16_t);
    std::uint32_t previous_run_end = 0U;
    std::uint64_t raw_bytes = 0U;
    std::vector<IndexPageDeltaRun> runs;
    try {
        runs.reserve(run_count);
    } catch (const std::bad_alloc &) {
        return false;
    }
    for (std::uint32_t run_index = 0; run_index < run_count; ++run_index) {
        std::uint32_t gap = 0U;
        std::uint32_t run_size = 0U;
        if (!read_varuint16(payload.data(), payload.size(), &cursor, &gap) ||
            !read_varuint16(payload.data(), payload.size(), &cursor, &run_size) ||
            previous_run_end > std::numeric_limits<std::uint32_t>::max() - gap) {
            return false;
        }
        const std::uint32_t run_offset = previous_run_end + gap;
        if (run_size == 0U || run_offset < previous_run_end || run_offset > record.page_size ||
            run_size > record.page_size - run_offset ||
            raw_bytes > std::numeric_limits<std::uint64_t>::max() - run_size) {
            return false;
        }
        try {
            runs.push_back({run_offset, run_size});
        } catch (const std::bad_alloc &) {
            return false;
        }
        previous_run_end = run_offset + run_size;
        raw_bytes += run_size;
    }
    if (raw_bytes > payload.size() - cursor) {
        return false;
    }

    std::size_t raw_cursor = cursor;
    for (const IndexPageDeltaRun &run : runs) {
        std::memcpy(
            static_cast<unsigned char *>(out_page) + run.offset,
            payload.data() + raw_cursor,
            run.size
        );
        raw_cursor += run.size;
    }
    return raw_cursor == payload.size();
}

bool read_standalone_or_rewrite_delta_payload(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    PageRecordHeader *out_record,
    std::vector<unsigned char> *out_payload
) {
    if (out_record == nullptr || out_payload == nullptr || !record_payload_shape_valid(record)) {
        return false;
    }
    try {
        out_payload->clear();
        if (!record_uses_any_delta_payload(record)) {
            if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
                if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
                    return false;
                }
            }
            out_payload->resize(static_cast<std::size_t>(record.payload_size));
            if (!out_payload->empty() &&
                !read_exact_at(fd, out_payload->data(), out_payload->size(), payload_offset)) {
                return false;
            }
            *out_record = record;
            return true;
        }

        std::vector<unsigned char> page(record.page_size);
        if (!read_record_page_payload(fd, payload_offset, record, page.data(), page.size())) {
            return false;
        }
        PageRecordHeader rewritten = record;
        std::uint32_t rewritten_flags = 0U;
        const std::uint64_t rewritten_payload_size = encoded_payload_size_for_page(
            page.data(),
            record.page_size,
            &rewritten_flags,
            out_payload
        );
        if (out_payload->empty()) {
            out_payload->assign(
                page.data(),
                page.data() + static_cast<std::size_t>(rewritten_payload_size)
            );
        }
        rewritten.flags = rewritten_flags | (record.flags & k_record_metadata_flags);
        rewritten.payload_size = rewritten_payload_size;
        rewritten.checksum = checksum_bytes(page.data(), page.size());
        *out_record = rewritten;
        return true;
    } catch (const std::bad_alloc &) {
        return false;
    }
}

bool record_page_too_large(const PageRecordHeader &record, std::size_t page_capacity) {
    return record.page_size > page_capacity ||
           record.page_size > std::numeric_limits<std::uint32_t>::max();
}

bool read_non_delta_record_page_payload(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    void *out_page,
    std::size_t page_capacity
) {
    if (!record_payload_shape_valid(record) || record_uses_any_delta_payload(record) ||
        record_page_too_large(record, page_capacity)) {
        return false;
    }
    if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
        if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
    }
    const std::size_t page_size = static_cast<std::size_t>(record.page_size);
    const std::size_t payload_size = static_cast<std::size_t>(record.payload_size);
    if (record_uses_any_sparse_zero_payload(record)) {
        const bool compact_sparse = record_uses_compact_sparse_zero_payload(record);
        const bool varint_compact_sparse = record_uses_varint_compact_sparse_zero_payload(record);
        const bool fill_sparse = record_uses_fill_sparse_zero_payload(record);
        const std::size_t run_count_size = (compact_sparse || varint_compact_sparse || fill_sparse)
                                               ? sizeof(std::uint16_t)
                                               : sizeof(std::uint32_t);
        const std::size_t run_header_size =
            compact_sparse ? 2U * sizeof(std::uint16_t) : 2U * sizeof(std::uint32_t);
        std::unique_ptr<unsigned char[]> payload(new (std::nothrow) unsigned char[payload_size]);
        if (payload == nullptr || !read_exact_at(fd, payload.get(), payload_size, payload_offset)) {
            return false;
        }
        std::memset(out_page, 0, page_size);
        if (payload_size < run_count_size) {
            return false;
        }
        const std::uint32_t run_count = (compact_sparse || varint_compact_sparse || fill_sparse)
                                            ? load16(payload.get(), 0U)
                                            : load32(payload.get(), 0U);
        if (run_count == 0U) {
            return false;
        }
        std::size_t cursor = run_count_size;
        bool has_previous_run = false;
        std::uint32_t previous_run_end = 0;
        for (std::uint32_t run_index = 0; run_index < run_count; ++run_index) {
            if (!varint_compact_sparse && !fill_sparse && cursor + run_header_size > payload_size) {
                return false;
            }
            std::uint32_t run_offset = 0;
            std::uint32_t run_size = 0;
            unsigned char fill_sparse_kind = k_fill_sparse_run_kind_raw;
            if (varint_compact_sparse || fill_sparse) {
                std::uint32_t zero_gap = 0;
                if (!read_varuint16(payload.get(), payload_size, &cursor, &zero_gap) ||
                    !read_varuint16(payload.get(), payload_size, &cursor, &run_size) ||
                    previous_run_end > std::numeric_limits<std::uint32_t>::max() - zero_gap) {
                    return false;
                }
                run_offset = previous_run_end + zero_gap;
                if (fill_sparse) {
                    if (cursor >= payload_size) {
                        return false;
                    }
                    fill_sparse_kind = payload[cursor++];
                }
            } else {
                run_offset =
                    compact_sparse ? load16(payload.get(), cursor) : load32(payload.get(), cursor);
                cursor += run_header_size / 2U;
                run_size =
                    compact_sparse ? load16(payload.get(), cursor) : load32(payload.get(), cursor);
                cursor += run_header_size / 2U;
            }
            const bool overlaps_previous =
                has_previous_run &&
                (fill_sparse ? run_offset < previous_run_end : run_offset <= previous_run_end);
            if (run_size == 0U || overlaps_previous || run_offset > record.page_size ||
                run_size > record.page_size - run_offset ||
                (fill_sparse && fill_sparse_kind != k_fill_sparse_run_kind_raw &&
                 fill_sparse_kind != k_fill_sparse_run_kind_fill) ||
                (fill_sparse && fill_sparse_kind == k_fill_sparse_run_kind_raw &&
                 run_size > payload_size - cursor) ||
                (!fill_sparse && run_size > payload_size - cursor)) {
                return false;
            }
            if (fill_sparse && fill_sparse_kind == k_fill_sparse_run_kind_fill) {
                if (cursor >= payload_size) {
                    return false;
                }
                std::memset(
                    static_cast<unsigned char *>(out_page) + run_offset,
                    payload[cursor++],
                    run_size
                );
            } else {
                if (run_size > payload_size - cursor) {
                    return false;
                }
                std::memcpy(
                    static_cast<unsigned char *>(out_page) + run_offset,
                    payload.get() + cursor,
                    run_size
                );
                cursor += run_size;
            }
            previous_run_end = run_offset + run_size;
            has_previous_run = true;
        }
        if (cursor != payload_size) {
            return false;
        }
    } else if (record_uses_trailing_zero_payload(record)) {
        std::memset(out_page, 0, page_size);
        if (!read_exact_at(fd, out_page, payload_size, payload_offset)) {
            return false;
        }
    } else {
        if (!read_exact_at(fd, out_page, payload_size, payload_offset)) {
            return false;
        }
    }
    return record_checksum_matches(out_page, record.page_size, record.checksum);
}

bool read_record_page_payload(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    void *out_page,
    std::size_t page_capacity
) {
    if (!record_payload_shape_valid(record) || record_page_too_large(record, page_capacity)) {
        return false;
    }
    if (record_uses_any_delta_payload(record)) {
        return decode_page_delta_payload(fd, payload_offset, record, out_page, page_capacity) &&
               record_checksum_matches(out_page, record.page_size, record.checksum);
    }
    return read_non_delta_record_page_payload(fd, payload_offset, record, out_page, page_capacity);
}

bool read_record_page_type(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record,
    std::uint16_t *out_page_type
) {
    if (out_page_type == nullptr || !record_payload_shape_valid(record) ||
        record.page_size < k_innodb_fil_page_type_offset + sizeof(std::uint16_t)) {
        return false;
    }
    if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
        if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
    }
    const std::size_t payload_size = static_cast<std::size_t>(record.payload_size);
    unsigned char page_type_bytes[2] = {};

    if (record_uses_any_delta_payload(record)) {
        std::unique_ptr<unsigned char[]> page(new (std::nothrow) unsigned char[record.page_size]);
        if (page == nullptr ||
            !read_record_page_payload(fd, payload_offset, record, page.get(), record.page_size)) {
            return false;
        }
        std::memcpy(
            page_type_bytes,
            page.get() + k_innodb_fil_page_type_offset,
            sizeof(page_type_bytes)
        );
    } else if (record_uses_any_sparse_zero_payload(record)) {
        const bool compact_sparse = record_uses_compact_sparse_zero_payload(record);
        const bool varint_compact_sparse = record_uses_varint_compact_sparse_zero_payload(record);
        const bool fill_sparse = record_uses_fill_sparse_zero_payload(record);
        const std::size_t run_count_size = (compact_sparse || varint_compact_sparse || fill_sparse)
                                               ? sizeof(std::uint16_t)
                                               : sizeof(std::uint32_t);
        const std::size_t run_header_size =
            compact_sparse ? 2U * sizeof(std::uint16_t) : 2U * sizeof(std::uint32_t);
        unsigned char run_count_bytes[sizeof(std::uint32_t)] = {};
        if (!read_exact_at(fd, run_count_bytes, run_count_size, payload_offset)) {
            return false;
        }
        if (payload_size < run_count_size) {
            return false;
        }
        const std::uint32_t run_count = (compact_sparse || varint_compact_sparse || fill_sparse)
                                            ? load16(run_count_bytes, 0U)
                                            : load32(run_count_bytes, 0U);
        if (run_count == 0U) {
            return false;
        }
        std::size_t cursor = run_count_size;
        bool has_previous_run = false;
        std::uint32_t previous_run_end = 0;
        for (std::uint32_t run_index = 0; run_index < run_count; ++run_index) {
            if (!varint_compact_sparse && !fill_sparse && cursor + run_header_size > payload_size) {
                return false;
            }
            std::uint32_t run_offset = 0;
            std::uint32_t run_size = 0;
            unsigned char fill_sparse_kind = k_fill_sparse_run_kind_raw;
            if (varint_compact_sparse || fill_sparse) {
                std::uint32_t zero_gap = 0;
                if (!read_varuint16_at(fd, payload_offset, payload_size, &cursor, &zero_gap) ||
                    !read_varuint16_at(fd, payload_offset, payload_size, &cursor, &run_size) ||
                    previous_run_end > std::numeric_limits<std::uint32_t>::max() - zero_gap) {
                    return false;
                }
                run_offset = previous_run_end + zero_gap;
                if (fill_sparse) {
                    off_t kind_offset = 0;
                    if (cursor >= payload_size ||
                        !offset_adds(payload_offset, cursor, &kind_offset) ||
                        !read_exact_at(fd, &fill_sparse_kind, 1U, kind_offset)) {
                        return false;
                    }
                    ++cursor;
                }
            } else {
                off_t run_header_offset = 0;
                unsigned char run_header[2U * sizeof(std::uint32_t)] = {};
                if (!offset_adds(payload_offset, cursor, &run_header_offset) ||
                    !read_exact_at(fd, run_header, run_header_size, run_header_offset)) {
                    return false;
                }
                run_offset = compact_sparse ? load16(run_header, 0U) : load32(run_header, 0U);
                run_size = compact_sparse ? load16(run_header, sizeof(std::uint16_t))
                                          : load32(run_header, sizeof(std::uint32_t));
                cursor += run_header_size;
            }
            const bool overlaps_previous =
                has_previous_run &&
                (fill_sparse ? run_offset < previous_run_end : run_offset <= previous_run_end);
            if (run_size == 0U || overlaps_previous || run_offset > record.page_size ||
                run_size > record.page_size - run_offset ||
                (fill_sparse && fill_sparse_kind != k_fill_sparse_run_kind_raw &&
                 fill_sparse_kind != k_fill_sparse_run_kind_fill) ||
                (fill_sparse && fill_sparse_kind == k_fill_sparse_run_kind_raw &&
                 run_size > payload_size - cursor) ||
                (!fill_sparse && run_size > payload_size - cursor)) {
                return false;
            }
            for (std::size_t byte_index = 0; byte_index < sizeof(page_type_bytes); ++byte_index) {
                const std::uint32_t target_offset =
                    static_cast<std::uint32_t>(k_innodb_fil_page_type_offset + byte_index);
                if (target_offset >= run_offset && target_offset < run_offset + run_size) {
                    if (fill_sparse && fill_sparse_kind == k_fill_sparse_run_kind_fill) {
                        off_t fill_byte_offset = 0;
                        if (cursor >= payload_size ||
                            !offset_adds(payload_offset, cursor, &fill_byte_offset) ||
                            !read_exact_at(
                                fd,
                                &page_type_bytes[byte_index],
                                1U,
                                fill_byte_offset
                            )) {
                            return false;
                        }
                    } else {
                        off_t target_file_offset = 0;
                        if (!offset_adds(
                                payload_offset,
                                cursor + (target_offset - run_offset),
                                &target_file_offset
                            ) ||
                            !read_exact_at(
                                fd,
                                &page_type_bytes[byte_index],
                                1U,
                                target_file_offset
                            )) {
                            return false;
                        }
                    }
                }
            }
            if (fill_sparse && fill_sparse_kind == k_fill_sparse_run_kind_fill) {
                if (cursor >= payload_size) {
                    return false;
                }
                ++cursor;
            } else {
                if (run_size > payload_size - cursor) {
                    return false;
                }
                cursor += run_size;
            }
            previous_run_end = run_offset + run_size;
            has_previous_run = true;
        }
        if (cursor != payload_size) {
            return false;
        }
    } else if (record_uses_trailing_zero_payload(record)) {
        if (record.payload_size > k_innodb_fil_page_type_offset) {
            off_t page_type_offset = 0;
            if (!offset_adds(payload_offset, k_innodb_fil_page_type_offset, &page_type_offset)) {
                return false;
            }
            const std::uint64_t available = record.payload_size - k_innodb_fil_page_type_offset;
            const std::uint64_t bytes_to_read64 =
                std::min<std::uint64_t>(sizeof(page_type_bytes), available);
            const std::size_t bytes_to_read = static_cast<std::size_t>(bytes_to_read64);
            if (!read_exact_at(fd, page_type_bytes, bytes_to_read, page_type_offset)) {
                return false;
            }
        }
    } else {
        off_t page_type_offset = 0;
        if (record.payload_size < k_innodb_fil_page_type_offset + sizeof(page_type_bytes) ||
            !offset_adds(payload_offset, k_innodb_fil_page_type_offset, &page_type_offset) ||
            !read_exact_at(fd, page_type_bytes, sizeof(page_type_bytes), page_type_offset)) {
            return false;
        }
    }

    *out_page_type = load_be16(page_type_bytes);
    return true;
}

void checksum_accumulator_update(
    PageChecksumAccumulator *inout_checksum,
    const void *buffer,
    std::size_t size
) {
    if (inout_checksum == nullptr || buffer == nullptr || size == 0U) {
        return;
    }
#if MYLITE_WITH_MARIADB_EMBEDDED
    inout_checksum->low = my_crc32c(inout_checksum->low, buffer, size);
    inout_checksum->high = my_crc32c(inout_checksum->high, buffer, size);
#endif
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    for (std::size_t index = 0; index < size; ++index) {
        inout_checksum->legacy ^= bytes[index];
        inout_checksum->legacy *= k_legacy_checksum_prime;
    }
    inout_checksum->bytes += size;
}

bool checksum_accumulator_update_file(
    PageChecksumAccumulator *inout_checksum,
    int fd,
    off_t offset,
    std::uint64_t size
) {
    if (inout_checksum == nullptr) {
        return false;
    }
    std::array<unsigned char, k_checksum_stream_chunk_size> buffer = {};
    std::uint64_t consumed = 0;
    while (consumed < size) {
        const std::uint64_t remaining = size - consumed;
        const auto chunk_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        off_t chunk_offset = 0;
        if (!offset_adds(offset, consumed, &chunk_offset) ||
            !read_exact_at(fd, buffer.data(), chunk_size, chunk_offset)) {
            return false;
        }
        checksum_accumulator_update(inout_checksum, buffer.data(), chunk_size);
        consumed += chunk_size;
    }
    return true;
}

void checksum_accumulator_update_repeated(
    PageChecksumAccumulator *inout_checksum,
    unsigned char byte,
    std::uint64_t size
) {
    if (inout_checksum == nullptr || size == 0U) {
        return;
    }
    std::array<unsigned char, k_checksum_stream_chunk_size> buffer = {};
    if (byte != 0U) {
        buffer.fill(byte);
    }
    std::uint64_t consumed = 0;
    while (consumed < size) {
        const std::uint64_t remaining = size - consumed;
        const auto chunk_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        checksum_accumulator_update(inout_checksum, buffer.data(), chunk_size);
        consumed += chunk_size;
    }
}

bool checksum_accumulator_matches(
    const PageChecksumAccumulator &checksum,
    std::uint64_t expected_checksum
) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    const std::uint64_t crc_checksum =
        (static_cast<std::uint64_t>(checksum.high) << 32U) | checksum.low;
    if (crc_checksum == expected_checksum) {
        return true;
    }
#endif
    return checksum.legacy == expected_checksum;
}

PayloadStatus record_payload_status(int fd, off_t payload_offset, const PageRecordHeader &record) {
    if (!record_payload_shape_valid(record)) {
        return PayloadStatus::Error;
    }
    if (!record_uses_any_delta_payload(record)) {
        return stream_non_delta_record_payload_status(fd, payload_offset, record);
    }
    std::unique_ptr<unsigned char[]> page(new (std::nothrow) unsigned char[record.page_size]);
    if (page == nullptr) {
        return PayloadStatus::Error;
    }
    if (!read_record_page_payload(fd, payload_offset, record, page.get(), record.page_size)) {
        return PayloadStatus::Mismatch;
    }
    return PayloadStatus::Ok;
}

PayloadStatus stream_non_delta_record_payload_status(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record
) {
    if (!record_payload_shape_valid(record) || record_uses_any_delta_payload(record)) {
        return PayloadStatus::Error;
    }
    if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
        if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
            return PayloadStatus::Mismatch;
        }
    }

    const std::size_t payload_size = static_cast<std::size_t>(record.payload_size);
    PageChecksumAccumulator checksum;
    if (record_uses_any_sparse_zero_payload(record)) {
        std::vector<unsigned char> payload;
        try {
            payload.resize(payload_size);
        } catch (const std::bad_alloc &) {
            return PayloadStatus::Error;
        }
        if (payload_size != 0U &&
            !read_exact_at(fd, payload.data(), payload_size, payload_offset)) {
            return PayloadStatus::Mismatch;
        }

        const bool compact_sparse = record_uses_compact_sparse_zero_payload(record);
        const bool varint_compact_sparse = record_uses_varint_compact_sparse_zero_payload(record);
        const bool fill_sparse = record_uses_fill_sparse_zero_payload(record);
        const std::size_t run_count_size = (compact_sparse || varint_compact_sparse || fill_sparse)
                                               ? sizeof(std::uint16_t)
                                               : sizeof(std::uint32_t);
        const std::size_t run_header_size =
            compact_sparse ? 2U * sizeof(std::uint16_t) : 2U * sizeof(std::uint32_t);
        if (payload_size < run_count_size) {
            return PayloadStatus::Mismatch;
        }
        const std::uint32_t run_count = (compact_sparse || varint_compact_sparse || fill_sparse)
                                            ? load16(payload.data(), 0U)
                                            : load32(payload.data(), 0U);
        if (run_count == 0U) {
            return PayloadStatus::Mismatch;
        }

        std::size_t cursor = run_count_size;
        bool has_previous_run = false;
        std::uint32_t previous_run_end = 0;
        for (std::uint32_t run_index = 0; run_index < run_count; ++run_index) {
            if (!varint_compact_sparse && !fill_sparse &&
                (cursor > payload_size || run_header_size > payload_size - cursor)) {
                return PayloadStatus::Mismatch;
            }
            std::uint32_t run_offset = 0;
            std::uint32_t run_size = 0;
            unsigned char fill_sparse_kind = k_fill_sparse_run_kind_raw;
            if (varint_compact_sparse || fill_sparse) {
                std::uint32_t zero_gap = 0;
                if (!read_varuint16(payload.data(), payload_size, &cursor, &zero_gap) ||
                    !read_varuint16(payload.data(), payload_size, &cursor, &run_size) ||
                    previous_run_end > std::numeric_limits<std::uint32_t>::max() - zero_gap) {
                    return PayloadStatus::Mismatch;
                }
                run_offset = previous_run_end + zero_gap;
                if (fill_sparse) {
                    if (cursor >= payload_size) {
                        return PayloadStatus::Mismatch;
                    }
                    fill_sparse_kind = payload[cursor++];
                }
            } else {
                run_offset = compact_sparse ? load16(payload.data(), cursor)
                                            : load32(payload.data(), cursor);
                cursor += run_header_size / 2U;
                run_size = compact_sparse ? load16(payload.data(), cursor)
                                          : load32(payload.data(), cursor);
                cursor += run_header_size / 2U;
            }

            const bool overlaps_previous =
                has_previous_run &&
                (fill_sparse ? run_offset < previous_run_end : run_offset <= previous_run_end);
            if (run_size == 0U || overlaps_previous || run_offset > record.page_size ||
                run_size > record.page_size - run_offset ||
                (fill_sparse && fill_sparse_kind != k_fill_sparse_run_kind_raw &&
                 fill_sparse_kind != k_fill_sparse_run_kind_fill) ||
                (fill_sparse && fill_sparse_kind == k_fill_sparse_run_kind_raw &&
                 run_size > payload_size - cursor) ||
                (!fill_sparse && run_size > payload_size - cursor)) {
                return PayloadStatus::Mismatch;
            }

            checksum_accumulator_update_repeated(
                &checksum,
                0U,
                static_cast<std::uint64_t>(run_offset - previous_run_end)
            );
            if (fill_sparse && fill_sparse_kind == k_fill_sparse_run_kind_fill) {
                if (cursor >= payload_size) {
                    return PayloadStatus::Mismatch;
                }
                checksum_accumulator_update_repeated(&checksum, payload[cursor++], run_size);
            } else {
                checksum_accumulator_update(&checksum, payload.data() + cursor, run_size);
                cursor += run_size;
            }
            previous_run_end = run_offset + run_size;
            has_previous_run = true;
        }
        if (cursor != payload_size) {
            return PayloadStatus::Mismatch;
        }
        checksum_accumulator_update_repeated(
            &checksum,
            0U,
            static_cast<std::uint64_t>(record.page_size - previous_run_end)
        );
    } else if (record_uses_trailing_zero_payload(record)) {
        if (!checksum_accumulator_update_file(&checksum, fd, payload_offset, record.payload_size)) {
            return PayloadStatus::Mismatch;
        }
        checksum_accumulator_update_repeated(&checksum, 0U, record.page_size - record.payload_size);
    } else if (!checksum_accumulator_update_file(
                   &checksum,
                   fd,
                   payload_offset,
                   record.payload_size
               )) {
        return PayloadStatus::Mismatch;
    }

    if (!checksum_accumulator_matches(checksum, record.checksum)) {
        return PayloadStatus::Mismatch;
    }
    page_log_scan_perf_add(PAGE_LOG_SCAN_PERF_STREAM_CHECKSUM_RECORDS, 1U);
    page_log_scan_perf_add(PAGE_LOG_SCAN_PERF_STREAM_CHECKSUM_BYTES, checksum.bytes);
    return PayloadStatus::Ok;
}

bool record_checksum_matches(const void *page, std::uint64_t page_size, std::uint64_t checksum) {
    const std::size_t size = static_cast<std::size_t>(page_size);
    return checksum_bytes(page, size) == checksum || legacy_checksum_bytes(page, size) == checksum;
}

bool record_requires_oldest_snapshot_boundary(
    int fd,
    off_t payload_offset,
    const PageRecordHeader &record
) {
    std::uint16_t page_type = 0;
    if (!read_record_page_type(fd, payload_offset, record, &page_type)) {
        return true;
    }
    return !record_page_type_is_native_support_state(page_type);
}

bool record_page_type_is_native_support_state(std::uint16_t page_type) {
    switch (page_type) {
    case k_innodb_fil_page_type_allocated:
    case k_innodb_fil_page_undo_log:
    case k_innodb_fil_page_inode:
    case k_innodb_fil_page_ibuf_free_list:
    case k_innodb_fil_page_ibuf_bitmap:
    case k_innodb_fil_page_type_sys:
    case k_innodb_fil_page_type_trx_sys:
    case k_innodb_fil_page_type_fsp_hdr:
    case k_innodb_fil_page_type_xdes:
        return true;
    default:
        return false;
    }
}

bool record_is_better(const PageRecordHeader &candidate, const PageRecordHeader &current) {
    return current.commit_lsn == 0U || candidate.commit_lsn > current.commit_lsn ||
           (candidate.commit_lsn == current.commit_lsn && candidate.page_lsn > current.page_lsn);
}

std::uint64_t page_key(std::uint32_t space_id, std::uint32_t page_no) {
    return (static_cast<std::uint64_t>(space_id) << 32U) | page_no;
}

std::uint64_t checksum_bytes(const void *buffer, std::size_t size) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    const std::uint32_t low = my_crc32c(0U, buffer, size);
    const std::uint32_t high = my_crc32c(k_page_checksum_second_seed, buffer, size);
    return (static_cast<std::uint64_t>(high) << 32U) | low;
#else
    return legacy_checksum_bytes(buffer, size);
#endif
}

std::uint64_t legacy_checksum_bytes(const void *buffer, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    std::uint64_t hash = k_legacy_checksum_offset_basis;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= k_legacy_checksum_prime;
    }
    return hash;
}

std::uint16_t load_be16(const unsigned char *bytes) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) << 8U | static_cast<std::uint16_t>(bytes[1])
    );
}

std::uint16_t load16(const unsigned char *bytes, std::size_t offset) {
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint16_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint32_t load32(const unsigned char *bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint64_t load64(const unsigned char *bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

void store16(unsigned char *bytes, std::size_t offset, std::uint16_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
}

void store32(unsigned char *bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
}

void store64(unsigned char *bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
}

} // namespace

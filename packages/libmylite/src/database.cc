#include <mylite/mylite.h>

#include "ownerless_autoinc_registry.h"
#include "ownerless_dictionary_state.h"
#include "ownerless_innodb_lock_registry.h"
#include "ownerless_latch.h"
#include "ownerless_lock_table.h"
#include "ownerless_mdl.h"
#include "ownerless_page_index.h"
#include "ownerless_page_log.h"
#include "ownerless_page_pin_registry.h"
#include "ownerless_process_registry.h"
#include "ownerless_read_view_registry.h"
#include "ownerless_redo_state.h"
#include "ownerless_tablespace_replay.h"
#include "ownerless_trx_registry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if MYLITE_WITH_MARIADB_EMBEDDED
#  include "mylite_ownerless_innodb_lock_hooks.h"
#  include "mylite_ownerless_mdl_hooks.h"
#  include "mylite_ownerless_read_view_hooks.h"
#  include "mylite_ownerless_runtime_hooks.h"
#  include "mylite_ownerless_trx_hooks.h"
#  include "ownerless_probe.h"
#  include "ownerless_wait.h"
#  include <mysql.h>
extern "C" std::uint32_t my_crc32c(std::uint32_t crc, const void *buf, std::size_t len);
extern "C" my_bool mylite_embedded_start_transaction(MYSQL *mysql);
#endif

#if MYLITE_WITH_MARIADB_EMBEDDED
enum OwnerlessDatabasePerfStatIndex : std::size_t {
    OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_CALLS = 0,
    OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_TOTAL_NS,
    OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_BOUNDARY_NS,
    OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_BOUNDARY_APPEND_CALLS,
    OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_APPEND_NS,
    OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_INDEX_NS,
    OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_CALLS,
    OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_TOTAL_NS,
    OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_SYNC_NS,
    OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_REDO_STATE_NS,
    OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_CHECKPOINT_NS,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_CALLS,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_TOTAL_NS,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_LOCK_NS,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_READ_NS,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_WRITE_NS,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_SYNC_NS,
    OWNERLESS_DATABASE_PERF_TABLE_LOCK_ACQUIRE_CALLS,
    OWNERLESS_DATABASE_PERF_TABLE_LOCK_ACQUIRE_NS,
    OWNERLESS_DATABASE_PERF_TABLE_LOCK_RELEASE_CALLS,
    OWNERLESS_DATABASE_PERF_TABLE_LOCK_RELEASE_NS,
    OWNERLESS_DATABASE_PERF_RECORD_LOCK_ACQUIRE_CALLS,
    OWNERLESS_DATABASE_PERF_RECORD_LOCK_ACQUIRE_NS,
    OWNERLESS_DATABASE_PERF_RECORD_LOCK_RELEASE_CALLS,
    OWNERLESS_DATABASE_PERF_RECORD_LOCK_RELEASE_NS,
    OWNERLESS_DATABASE_PERF_MDL_ACQUIRE_CALLS,
    OWNERLESS_DATABASE_PERF_MDL_ACQUIRE_NS,
    OWNERLESS_DATABASE_PERF_MDL_RELEASE_CALLS,
    OWNERLESS_DATABASE_PERF_MDL_RELEASE_NS,
    OWNERLESS_DATABASE_PERF_TRX_ALLOCATE_CALLS,
    OWNERLESS_DATABASE_PERF_TRX_ALLOCATE_NS,
    OWNERLESS_DATABASE_PERF_TRX_REGISTER_CALLS,
    OWNERLESS_DATABASE_PERF_TRX_REGISTER_NS,
    OWNERLESS_DATABASE_PERF_TRX_ASSIGN_NO_CALLS,
    OWNERLESS_DATABASE_PERF_TRX_ASSIGN_NO_NS,
    OWNERLESS_DATABASE_PERF_TRX_DEREGISTER_CALLS,
    OWNERLESS_DATABASE_PERF_TRX_DEREGISTER_NS,
    OWNERLESS_DATABASE_PERF_TRX_SNAPSHOT_CALLS,
    OWNERLESS_DATABASE_PERF_TRX_SNAPSHOT_NS,
    OWNERLESS_DATABASE_PERF_READ_VIEW_REGISTER_CALLS,
    OWNERLESS_DATABASE_PERF_READ_VIEW_REGISTER_NS,
    OWNERLESS_DATABASE_PERF_READ_VIEW_DEREGISTER_CALLS,
    OWNERLESS_DATABASE_PERF_READ_VIEW_DEREGISTER_NS,
    OWNERLESS_DATABASE_PERF_READ_VIEW_SNAPSHOT_CALLS,
    OWNERLESS_DATABASE_PERF_READ_VIEW_SNAPSHOT_NS,
    OWNERLESS_DATABASE_PERF_REDO_ENTER_CALLS,
    OWNERLESS_DATABASE_PERF_REDO_ENTER_NS,
    OWNERLESS_DATABASE_PERF_REDO_OBSERVE_CALLS,
    OWNERLESS_DATABASE_PERF_REDO_OBSERVE_NS,
    OWNERLESS_DATABASE_PERF_REDO_RESERVE_CALLS,
    OWNERLESS_DATABASE_PERF_REDO_RESERVE_NS,
    OWNERLESS_DATABASE_PERF_REDO_WRITTEN_CALLS,
    OWNERLESS_DATABASE_PERF_REDO_WRITTEN_NS,
    OWNERLESS_DATABASE_PERF_REDO_LEAVE_CALLS,
    OWNERLESS_DATABASE_PERF_REDO_LEAVE_NS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_CALLS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_TOTAL_NS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_NS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_DIRECT_NS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_HITS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_MISSES,
    OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_SCAN_REQUIRED,
    OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_STALE,
    OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_ERRORS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_CALLS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_NS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_FOUND,
    OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_MISSES,
    OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_NEGATIVE_CACHE_HITS,
    OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_NEGATIVE_CACHE_STORES,
    OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_ERRORS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_CALLS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_TOTAL_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_PRESSURE_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_RUNTIME_STATEMENT_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_TEMPORARY_TABLE_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_STATEMENT_LOCK_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_REFRESH_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_BIND_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_RESULT_SETUP_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_DICTIONARY_BEGIN_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_SNAPSHOT_PIN_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_MYSQL_EXECUTE_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_POST_STATE_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_DICTIONARY_FINISH_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_AFFECTED_ROWS_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_RECLAIM_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_PREPARE_CALLS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_PREPARE_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_CLOSE_CALLS,
    OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_CLOSE_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_RESET_CALLS,
    OWNERLESS_DATABASE_PERF_PREPARED_RESET_TOTAL_NS,
    OWNERLESS_DATABASE_PERF_PREPARED_RESET_MYSQL_NS,
    OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_CALLS,
    OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_NS,
    OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_EMPTY,
    OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_TRX_IDS,
    OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_NATIVE_CLEARED,
    OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_CALLS,
    OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_ALLOWED,
    OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_UNMAPPED,
    OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_ACTIVE_COUNT,
    OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_GENERATION,
    OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_ACTIVE_PINS,
    OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_BASELINE,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_FILE_READ_ELIDED,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_GENERATION_CACHE_HITS,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_LEGACY_WRITE_ELIDED,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_NOOP_ELIDED,
    OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_DEFERRED_LATEST_COALESCED,
    OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_PAGE_LOG_CHECKSUM_NS,
    OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_INDEX_SKIPPED_NATIVE_SUPPORT,
    OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_CALLS,
    OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_NS,
    OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_APPEND_NS,
    OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_SUCCEEDED,
    OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_UNAVAILABLE,
    OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_FAILED,
    OWNERLESS_DATABASE_PERF_REFRESH_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_TOTAL_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_DICTIONARY_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_SHARED_SNAPSHOT_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_PIN_SNAPSHOT_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_BASELINE_PIN_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_BASELINE_PIN_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_ADVANCE_TRX_HORIZON_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_ADVANCE_TRX_HORIZON_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_CLOSE_READ_VIEW_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_CLOSE_READ_VIEW_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_NATIVE_FLUSH_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_NATIVE_FLUSH_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_EXTERNAL_REFRESH_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_EXTERNAL_REFRESH_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_HANDLE_PIN_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_HANDLE_PIN_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_CLEAN_PAGE_REFRESH_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_CLEAN_PAGE_REFRESH_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_VISIBILITY_PUSH_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_VISIBILITY_PUSH_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_VISIBILITY_ENABLE_CALLS,
    OWNERLESS_DATABASE_PERF_REFRESH_VISIBILITY_ENABLE_NS,
    OWNERLESS_DATABASE_PERF_REFRESH_LOCAL_NATIVE_CURRENT_READ,
    OWNERLESS_DATABASE_PERF_REFRESH_PAGE_VERSION_READS_ENABLED,
    OWNERLESS_DATABASE_PERF_STAT_COUNT
};

static std::atomic<bool> ownerless_database_perf_stats_enabled{false};
static std::atomic<std::uint64_t> ownerless_database_perf_stats[OWNERLESS_DATABASE_PERF_STAT_COUNT];
static std::mutex ownerless_page_log_negative_cache_mutex;

static bool ownerless_database_perf_stats_are_enabled() {
    return ownerless_database_perf_stats_enabled.load(std::memory_order_relaxed);
}

static std::uint64_t ownerless_database_perf_now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

static void ownerless_database_perf_add(OwnerlessDatabasePerfStatIndex index, std::uint64_t value) {
    if (ownerless_database_perf_stats_are_enabled()) {
        ownerless_database_perf_stats[index].fetch_add(value, std::memory_order_relaxed);
    }
}

static void ownerless_database_perf_add_elapsed(
    OwnerlessDatabasePerfStatIndex index,
    std::uint64_t start_ns
) {
    if (start_ns == 0U) {
        return;
    }
    if (ownerless_database_perf_stats_are_enabled()) {
        ownerless_database_perf_stats[index].fetch_add(
            ownerless_database_perf_now_ns() - start_ns,
            std::memory_order_relaxed
        );
    }
}

class OwnerlessDatabasePerfScope {
  public:
    explicit OwnerlessDatabasePerfScope(OwnerlessDatabasePerfStatIndex index)
        : index_(index),
          start_ns_(
              ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U
          ) {}

    ~OwnerlessDatabasePerfScope() {
        ownerless_database_perf_add_elapsed(index_, start_ns_);
    }

    OwnerlessDatabasePerfScope(const OwnerlessDatabasePerfScope &) = delete;
    OwnerlessDatabasePerfScope &operator=(const OwnerlessDatabasePerfScope &) = delete;

  private:
    OwnerlessDatabasePerfStatIndex index_;
    std::uint64_t start_ns_;
};

class OwnerlessDatabasePerfCountedScope {
  public:
    OwnerlessDatabasePerfCountedScope(
        OwnerlessDatabasePerfStatIndex count_index,
        OwnerlessDatabasePerfStatIndex ns_index
    )
        : ns_index_(ns_index), enabled_(ownerless_database_perf_stats_are_enabled()),
          start_ns_(enabled_ ? ownerless_database_perf_now_ns() : 0U) {
        if (enabled_) {
            ownerless_database_perf_stats[count_index].fetch_add(1U, std::memory_order_relaxed);
        }
    }

    ~OwnerlessDatabasePerfCountedScope() {
        if (enabled_) {
            ownerless_database_perf_stats[ns_index_].fetch_add(
                ownerless_database_perf_now_ns() - start_ns_,
                std::memory_order_relaxed
            );
        }
    }

    OwnerlessDatabasePerfCountedScope(const OwnerlessDatabasePerfCountedScope &) = delete;
    OwnerlessDatabasePerfCountedScope &operator=(const OwnerlessDatabasePerfCountedScope &) =
        delete;

  private:
    OwnerlessDatabasePerfStatIndex ns_index_;
    bool enabled_;
    std::uint64_t start_ns_;
};

extern "C" void mylite_ownerless_database_set_perf_stats_enabled(int enabled) {
    ownerless_database_perf_stats_enabled.store(enabled != 0, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_database_reset_perf_stats(void) {
    for (std::size_t i = 0; i < OWNERLESS_DATABASE_PERF_STAT_COUNT; ++i) {
        ownerless_database_perf_stats[i].store(0, std::memory_order_relaxed);
    }
}

extern "C" void mylite_ownerless_database_read_perf_stats(
    std::uint64_t *out_values,
    std::size_t value_count
) {
    if (out_values == nullptr || value_count == 0U) {
        return;
    }
    const std::size_t copy_count =
        std::min<std::size_t>(value_count, OWNERLESS_DATABASE_PERF_STAT_COUNT);
    for (std::size_t i = 0; i < copy_count; ++i) {
        out_values[i] = ownerless_database_perf_stats[i].load(std::memory_order_relaxed);
    }
}

enum EmbeddedOpenPerfStatIndex : std::size_t {
    EMBEDDED_OPEN_PERF_OPEN_CALLS = 0,
    EMBEDDED_OPEN_PERF_OPEN_TOTAL_NS,
    EMBEDDED_OPEN_PERF_OPEN_VALIDATE_NS,
    EMBEDDED_OPEN_PERF_OPEN_ALLOCATE_NORMALIZE_NS,
    EMBEDDED_OPEN_PERF_OPEN_RUNTIME_PATH_NS,
    EMBEDDED_OPEN_PERF_OPEN_PREPARE_DIRECTORY_NS,
    EMBEDDED_OPEN_PERF_OPEN_PLATFORM_PROBE_NS,
    EMBEDDED_OPEN_PERF_OPEN_STARTUP_LOCK_NS,
    EMBEDDED_OPEN_PERF_OPEN_START_RUNTIME_NS,
    EMBEDDED_OPEN_PERF_OPEN_CONNECT_RUNTIME_NS,
    EMBEDDED_OPEN_PERF_OPEN_SYSTEM_TABLES_NS,
    EMBEDDED_OPEN_PERF_OPEN_DICTIONARY_NS,
    EMBEDDED_OPEN_PERF_START_RUNTIME_CALLS,
    EMBEDDED_OPEN_PERF_START_RUNTIME_TOTAL_NS,
    EMBEDDED_OPEN_PERF_START_DATABASE_LOCK_NS,
    EMBEDDED_OPEN_PERF_START_CONCURRENCY_METADATA_NS,
    EMBEDDED_OPEN_PERF_START_SHARED_MEMORY_PREPARE_NS,
    EMBEDDED_OPEN_PERF_START_LAYOUT_ARGUMENTS_NS,
    EMBEDDED_OPEN_PERF_START_MAP_SHARED_MEMORY_NS,
    EMBEDDED_OPEN_PERF_START_OPEN_PAGE_LOG_NS,
    EMBEDDED_OPEN_PERF_START_OPEN_CHECKPOINT_NS,
    EMBEDDED_OPEN_PERF_START_PRE_HOOKS_NS,
    EMBEDDED_OPEN_PERF_START_REDO_EVIDENCE_NS,
    EMBEDDED_OPEN_PERF_START_BOOTSTRAP_LOCK_NS,
    EMBEDDED_OPEN_PERF_START_MYSQL_SERVER_INIT_NS,
    EMBEDDED_OPEN_PERF_START_POST_HOOKS_NS,
    EMBEDDED_OPEN_PERF_START_REDO_BACKUP_NS,
    EMBEDDED_OPEN_PERF_START_SCHEDULER_NS,
    EMBEDDED_OPEN_PERF_CONNECT_CALLS,
    EMBEDDED_OPEN_PERF_CONNECT_TOTAL_NS,
    EMBEDDED_OPEN_PERF_CONNECT_MYSQL_INIT_NS,
    EMBEDDED_OPEN_PERF_CONNECT_MYSQL_REAL_CONNECT_NS,
    EMBEDDED_OPEN_PERF_SYSTEM_TABLES_CALLS,
    EMBEDDED_OPEN_PERF_SYSTEM_TABLES_EXECUTIONS,
    EMBEDDED_OPEN_PERF_SYSTEM_TABLES_TOTAL_NS,
    EMBEDDED_OPEN_PERF_SYSTEM_TABLES_LOCK_NS,
    EMBEDDED_OPEN_PERF_SYSTEM_TABLES_STATEMENTS_NS,
    EMBEDDED_OPEN_PERF_CLOSE_CALLS,
    EMBEDDED_OPEN_PERF_CLOSE_TOTAL_NS,
    EMBEDDED_OPEN_PERF_CLOSE_ROLLBACK_NS,
    EMBEDDED_OPEN_PERF_CLOSE_CONNECTION_NS,
    EMBEDDED_OPEN_PERF_CLOSE_RELEASE_RUNTIME_NS,
    EMBEDDED_OPEN_PERF_RELEASE_RUNTIME_CALLS,
    EMBEDDED_OPEN_PERF_RELEASE_RUNTIME_TOTAL_NS,
    EMBEDDED_OPEN_PERF_RELEASE_STOP_SCHEDULER_NS,
    EMBEDDED_OPEN_PERF_RELEASE_STARTUP_LOCK_NS,
    EMBEDDED_OPEN_PERF_RELEASE_RECLAIM_NS,
    EMBEDDED_OPEN_PERF_RELEASE_REDO_CAPTURE_NS,
    EMBEDDED_OPEN_PERF_RELEASE_RESET_HOOKS_NS,
    EMBEDDED_OPEN_PERF_RELEASE_MYSQL_THREAD_END_NS,
    EMBEDDED_OPEN_PERF_RELEASE_MYSQL_SERVER_END_NS,
    EMBEDDED_OPEN_PERF_RELEASE_MYSQL_SHUTDOWN_NS,
    EMBEDDED_OPEN_PERF_RELEASE_REDO_RESTORE_NS,
    EMBEDDED_OPEN_PERF_RELEASE_UNMAP_NS,
    EMBEDDED_OPEN_PERF_RELEASE_CLEANUP_NS,
    EMBEDDED_OPEN_PERF_RELEASE_DATABASE_LOCK_NS,
    EMBEDDED_OPEN_PERF_STAT_COUNT
};

static std::atomic<bool> embedded_open_perf_stats_enabled{false};
static std::atomic<std::uint64_t> embedded_open_perf_stats[EMBEDDED_OPEN_PERF_STAT_COUNT];

static bool embedded_open_perf_stats_are_enabled() {
    return embedded_open_perf_stats_enabled.load(std::memory_order_relaxed);
}

static std::uint64_t embedded_open_perf_now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

static void embedded_open_perf_add(EmbeddedOpenPerfStatIndex index, std::uint64_t value) {
    if (embedded_open_perf_stats_are_enabled()) {
        embedded_open_perf_stats[index].fetch_add(value, std::memory_order_relaxed);
    }
}

static void embedded_open_perf_add_elapsed(
    EmbeddedOpenPerfStatIndex index,
    std::uint64_t start_ns
) {
    if (start_ns == 0U) {
        return;
    }
    if (embedded_open_perf_stats_are_enabled()) {
        embedded_open_perf_stats[index].fetch_add(
            embedded_open_perf_now_ns() - start_ns,
            std::memory_order_relaxed
        );
    }
}

static std::uint64_t embedded_open_perf_start_ns() {
    return embedded_open_perf_stats_are_enabled() ? embedded_open_perf_now_ns() : 0U;
}

class EmbeddedOpenPerfScope {
  public:
    explicit EmbeddedOpenPerfScope(EmbeddedOpenPerfStatIndex index)
        : index_(index), start_ns_(embedded_open_perf_start_ns()) {}

    ~EmbeddedOpenPerfScope() {
        embedded_open_perf_add_elapsed(index_, start_ns_);
    }

    EmbeddedOpenPerfScope(const EmbeddedOpenPerfScope &) = delete;
    EmbeddedOpenPerfScope &operator=(const EmbeddedOpenPerfScope &) = delete;

  private:
    EmbeddedOpenPerfStatIndex index_;
    std::uint64_t start_ns_;
};

extern "C" void mylite_embedded_open_perf_set_enabled(int enabled) {
    embedded_open_perf_stats_enabled.store(enabled != 0, std::memory_order_relaxed);
}

extern "C" void mylite_embedded_open_perf_reset(void) {
    for (std::size_t i = 0; i < EMBEDDED_OPEN_PERF_STAT_COUNT; ++i) {
        embedded_open_perf_stats[i].store(0, std::memory_order_relaxed);
    }
}

extern "C" void mylite_embedded_open_perf_read(std::uint64_t *out_values, std::size_t value_count) {
    if (out_values == nullptr || value_count == 0U) {
        return;
    }
    const std::size_t copy_count =
        std::min<std::size_t>(value_count, EMBEDDED_OPEN_PERF_STAT_COUNT);
    for (std::size_t i = 0; i < copy_count; ++i) {
        out_values[i] = embedded_open_perf_stats[i].load(std::memory_order_relaxed);
    }
}
#endif

enum ExecResultPerfStatIndex : std::size_t {
    EXEC_RESULT_PERF_CALLS = 0,
    EXEC_RESULT_PERF_MYSQL_QUERY_NS,
    EXEC_RESULT_PERF_MYSQL_QUERY_ERRORS,
    EXEC_RESULT_PERF_AFFECTED_ROWS_NS,
    EXEC_RESULT_PERF_STORE_RESULT_NS,
    EXEC_RESULT_PERF_RESULT_SETS,
    EXEC_RESULT_PERF_NO_RESULT_SETS,
    EXEC_RESULT_PERF_CURRENT_SCHEMA_NS,
    EXEC_RESULT_PERF_STATUS_UPDATE_NS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_CALLS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_NS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_ERRORS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_AUTOCOMMIT_NOOPS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_START_TRANSACTION_CALLS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_COMMIT_CALLS,
    EXEC_RESULT_PERF_NATIVE_CONTROL_ROLLBACK_CALLS,
    EXEC_RESULT_PERF_STAT_COUNT
};

namespace {
std::atomic<bool> exec_result_perf_stats_enabled{false};
std::atomic<std::uint64_t> exec_result_perf_stats[EXEC_RESULT_PERF_STAT_COUNT];
} // namespace

#if MYLITE_WITH_MARIADB_EMBEDDED
static bool exec_result_perf_stats_are_enabled() {
    return exec_result_perf_stats_enabled.load(std::memory_order_relaxed);
}

static std::uint64_t exec_result_perf_now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

static std::uint64_t exec_result_perf_start_ns() {
    return exec_result_perf_stats_are_enabled() ? exec_result_perf_now_ns() : 0U;
}

static void exec_result_perf_add(ExecResultPerfStatIndex index, std::uint64_t value) {
    if (exec_result_perf_stats_are_enabled()) {
        exec_result_perf_stats[index].fetch_add(value, std::memory_order_relaxed);
    }
}

static void exec_result_perf_add_elapsed(ExecResultPerfStatIndex index, std::uint64_t start_ns) {
    if (start_ns == 0U) {
        return;
    }
    if (exec_result_perf_stats_are_enabled()) {
        exec_result_perf_stats[index].fetch_add(
            exec_result_perf_now_ns() - start_ns,
            std::memory_order_relaxed
        );
    }
}
#endif

extern "C" MYLITE_API void mylite_exec_result_perf_set_enabled(int enabled) {
    exec_result_perf_stats_enabled.store(enabled != 0, std::memory_order_relaxed);
}

extern "C" MYLITE_API void mylite_exec_result_perf_reset(void) {
    for (std::size_t i = 0; i < EXEC_RESULT_PERF_STAT_COUNT; ++i) {
        exec_result_perf_stats[i].store(0, std::memory_order_relaxed);
    }
}

extern "C" MYLITE_API void mylite_exec_result_perf_read(
    std::uint64_t *out_values,
    std::size_t value_count
) {
    if (out_values == nullptr || value_count == 0U) {
        return;
    }
    const std::size_t copy_count = std::min<std::size_t>(value_count, EXEC_RESULT_PERF_STAT_COUNT);
    for (std::size_t i = 0; i < copy_count; ++i) {
        out_values[i] = exec_result_perf_stats[i].load(std::memory_order_relaxed);
    }
}

#ifndef MYLITE_MARIADB_MESSAGES_DIR
#  define MYLITE_MARIADB_MESSAGES_DIR ""
#endif

#ifndef MYLITE_MARIADB_CHARSETS_DIR
#  define MYLITE_MARIADB_CHARSETS_DIR ""
#endif

#ifndef MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
#  define MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS 0
#endif

#ifndef MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES
#  define MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES 65536
#endif

#ifndef MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_INTERVAL_MS
#  define MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_INTERVAL_MS 50
#endif

#ifndef MYLITE_OWNERLESS_SINGLE_OWNER_FOREGROUND_RECLAIM_MIN_BYTES
#  define MYLITE_OWNERLESS_SINGLE_OWNER_FOREGROUND_RECLAIM_MIN_BYTES (64ULL * 1024ULL * 1024ULL)
#endif

namespace {

constexpr unsigned k_known_open_flags =
    MYLITE_OPEN_READONLY | MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE |
    MYLITE_OPEN_URI | MYLITE_OPEN_SHARED_READONLY | MYLITE_OPEN_OWNERLESS_RW;
constexpr const char *k_sqlstate_ok = "00000";
constexpr const char *k_sqlstate_general = "HY000";
constexpr const char *k_not_an_error = "not an error";
constexpr const char *k_bad_db_handle = "bad database handle";
constexpr const char *k_memory_database_path = ":memory:";
constexpr int k_decimal_base = 10;
constexpr unsigned k_statement_lock_wait_timeout_ms = 60000;

#if MYLITE_WITH_MARIADB_EMBEDDED
constexpr unsigned k_mariadb_lock_deadlock_errno = 1213;
constexpr unsigned k_mariadb_no_such_table_in_engine_errno = 1932;
constexpr auto k_ownerless_checkpoint_scheduler_interval =
    std::chrono::milliseconds(MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_INTERVAL_MS);
static_assert(MYLITE_OWNERLESS_MDL_MODE_SHARED == MYLITE_OWNERLESS_LOCK_TABLE_SHARED);
static_assert(MYLITE_OWNERLESS_MDL_MODE_EXCLUSIVE == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE);
static_assert(MYLITE_OWNERLESS_MDL_MODE_UPGRADABLE == MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE);
static_assert(MYLITE_OWNERLESS_MDL_MODE_SHARED_READ == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ);
static_assert(MYLITE_OWNERLESS_MDL_MODE_SHARED_WRITE == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE);
static_assert(
    MYLITE_OWNERLESS_MDL_MODE_SHARED_READ_ONLY == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY
);
static_assert(
    MYLITE_OWNERLESS_MDL_MODE_SHARED_NO_WRITE == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE
);
static_assert(
    MYLITE_OWNERLESS_MDL_MODE_SHARED_NO_READ_WRITE ==
    MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE
);
static_assert(
    MYLITE_OWNERLESS_MDL_MODE_SCOPED_INTENTION_EXCLUSIVE ==
    MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE
);
constexpr std::size_t k_sql_policy_token_count = 256;
constexpr const char *k_meta_filename = "mylite.meta";
constexpr const char *k_lock_filename = "mylite.lock";
constexpr const char *k_concurrency_dir_name = "concurrency";
constexpr const char *k_concurrency_meta_filename = "mylite-concurrency.meta";
constexpr const char *k_concurrency_lock_filename = "mylite-concurrency.lock";
constexpr const char *k_concurrency_shm_filename = "mylite-concurrency.shm";
constexpr const char *k_concurrency_wal_filename = "mylite-concurrency.wal";
constexpr const char *k_concurrency_checkpoint_filename = "mylite-concurrency.ckpt";
constexpr const char *k_concurrency_startup_lock_filename = "mylite-runtime-startup.lock";
constexpr const char *k_ownerless_platform_probe_meta_filename = "mylite-ownerless-platform.meta";
constexpr const char *k_concurrency_redo_header_filename = "mylite-redo-header.bin";
constexpr const char *k_datadir_name = "datadir";
constexpr const char *k_tmpdir_name = "tmp";
constexpr const char *k_rundir_name = "run";
constexpr const char *k_plugin_directory_name = "plugins";
constexpr const char *k_innodb_redo_log_filename = "ib_logfile0";
constexpr const char *k_innodb_temp_tablespace_filename = "ibtmp1";
constexpr const char *k_statement_lock_filename = "mylite-statements.lock";
constexpr const char *k_mariadb_base_ref = "mariadb-11.8.6";
constexpr const char *k_metadata_format_line = "format=1";
constexpr const char *k_concurrency_mode_line = "mode=exclusive";
constexpr const char *k_innodb_temp_data_file_path = "ibtmp1:12M:autoextend";
constexpr const char *k_create_mysql_database_sql = "CREATE DATABASE IF NOT EXISTS mysql";
constexpr const char *k_create_proc_table_sql =
    "CREATE TABLE IF NOT EXISTS mysql.proc ("
    "db char(64) collate utf8mb3_bin DEFAULT '' NOT NULL, "
    "name char(64) DEFAULT '' NOT NULL, "
    "type enum('FUNCTION','PROCEDURE','PACKAGE','PACKAGE BODY') NOT NULL, "
    "specific_name char(64) DEFAULT '' NOT NULL, "
    "language enum('SQL') DEFAULT 'SQL' NOT NULL, "
    "sql_data_access enum('CONTAINS_SQL','NO_SQL','READS_SQL_DATA','MODIFIES_SQL_DATA') "
    "DEFAULT 'CONTAINS_SQL' NOT NULL, "
    "is_deterministic enum('YES','NO') DEFAULT 'NO' NOT NULL, "
    "security_type enum('INVOKER','DEFINER') DEFAULT 'DEFINER' NOT NULL, "
    "param_list blob DEFAULT '' NOT NULL, "
    "returns longblob NOT NULL, "
    "body longblob NOT NULL, "
    "definer varchar(384) collate utf8mb3_bin DEFAULT '' NOT NULL, "
    "created timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, "
    "modified timestamp NOT NULL DEFAULT '0000-00-00 00:00:00', "
    "sql_mode set('REAL_AS_FLOAT','PIPES_AS_CONCAT','ANSI_QUOTES','IGNORE_SPACE',"
    "'IGNORE_BAD_TABLE_OPTIONS','ONLY_FULL_GROUP_BY','NO_UNSIGNED_SUBTRACTION',"
    "'NO_DIR_IN_CREATE','POSTGRESQL','ORACLE','MSSQL','DB2','MAXDB','NO_KEY_OPTIONS',"
    "'NO_TABLE_OPTIONS','NO_FIELD_OPTIONS','MYSQL323','MYSQL40','ANSI',"
    "'NO_AUTO_VALUE_ON_ZERO','NO_BACKSLASH_ESCAPES','STRICT_TRANS_TABLES',"
    "'STRICT_ALL_TABLES','NO_ZERO_IN_DATE','NO_ZERO_DATE','INVALID_DATES',"
    "'ERROR_FOR_DIVISION_BY_ZERO','TRADITIONAL','NO_AUTO_CREATE_USER',"
    "'HIGH_NOT_PRECEDENCE','NO_ENGINE_SUBSTITUTION','PAD_CHAR_TO_FULL_LENGTH',"
    "'EMPTY_STRING_IS_NULL','SIMULTANEOUS_ASSIGNMENT','TIME_ROUND_FRACTIONAL') "
    "DEFAULT '' NOT NULL, "
    "comment text collate utf8mb3_bin NOT NULL, "
    "character_set_client char(32) collate utf8mb3_bin, "
    "collation_connection char(64) collate utf8mb3_bin, "
    "db_collation char(64) collate utf8mb3_bin, "
    "body_utf8 longblob, "
    "aggregate enum('NONE','GROUP') DEFAULT 'NONE' NOT NULL, "
    "PRIMARY KEY (db,name,type)) "
    "engine=Aria transactional=1 character set utf8mb3 COLLATE utf8mb3_general_ci "
    "comment='Stored Procedures'";
constexpr const char *k_create_procs_priv_table_sql =
    "CREATE TABLE IF NOT EXISTS mysql.procs_priv ("
    "Host char(255) binary DEFAULT '' NOT NULL, "
    "Db char(64) binary DEFAULT '' NOT NULL, "
    "User char(128) binary DEFAULT '' NOT NULL, "
    "Routine_name char(64) COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL, "
    "Routine_type enum('FUNCTION','PROCEDURE','PACKAGE','PACKAGE BODY') NOT NULL, "
    "Grantor varchar(384) DEFAULT '' NOT NULL, "
    "Proc_priv set('Execute','Alter Routine','Grant','Show Create Routine') "
    "COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL, "
    "Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, "
    "PRIMARY KEY (Host,Db,User,Routine_name,Routine_type), "
    "KEY Grantor (Grantor)) "
    "engine=Aria transactional=1 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin "
    "comment='Procedure privileges'";
constexpr int k_runtime_directory_attempts = 100;
constexpr unsigned k_lock_poll_initial_interval_ms = 1;
constexpr unsigned k_lock_poll_max_interval_ms = 10;
constexpr unsigned k_concurrency_lock_wait_timeout_ms = 5000;
constexpr unsigned k_system_tables_lock_wait_timeout_ms = 60000;
constexpr unsigned k_ownerless_runtime_startup_attempts = 3;
constexpr unsigned k_ownerless_runtime_startup_retry_delay_ms = 50;
constexpr std::size_t k_ownerless_redo_header_prefix_size = 4096;
constexpr std::size_t k_ownerless_redo_startup_prefix_size = 12288;
constexpr std::size_t k_ownerless_redo_header_checksum_offset = 508;
constexpr std::size_t k_ownerless_redo_checkpoint_1_offset = 4096;
constexpr std::size_t k_ownerless_redo_checkpoint_2_offset = 8192;
constexpr std::size_t k_ownerless_redo_checkpoint_lsn_offset = 0;
constexpr std::size_t k_ownerless_redo_checkpoint_end_lsn_offset = 8;
constexpr std::size_t k_ownerless_redo_checkpoint_reserved_offset = 16;
constexpr std::size_t k_ownerless_redo_checkpoint_reserved_length = 44;
constexpr std::size_t k_ownerless_redo_checkpoint_checksum_offset = 60;
constexpr std::size_t k_ownerless_redo_header_backup_header_size = 32;
constexpr std::uint32_t k_ownerless_redo_header_backup_format = 1;
constexpr std::size_t k_ownerless_redo_header_backup_format_offset = 8;
constexpr std::size_t k_ownerless_redo_header_backup_header_size_offset = 12;
constexpr std::size_t k_ownerless_redo_header_backup_file_size_offset = 16;
constexpr std::size_t k_ownerless_redo_header_backup_payload_size_offset = 24;
constexpr std::size_t k_ownerless_redo_header_backup_payload_offset =
    k_ownerless_redo_header_backup_header_size;
constexpr off_t k_ownerless_redo_backup_file_size_tolerance = 4096;
constexpr std::array<unsigned char, 4> k_mariadb_physical_redo_header_magic = {
    'P',
    'h',
    'y',
    's',
};
constexpr std::uint32_t k_mariadb_redo_format_10_8 = 0x50687973U;
constexpr std::array<unsigned char, 8> k_concurrency_redo_header_magic = {
    'M',
    'Y',
    'L',
    'R',
    'D',
    'O',
    '0',
    '1',
};
constexpr unsigned long k_initial_result_buffer_size = 4096;
constexpr off_t k_persisted_config_lock_start = 0;
constexpr off_t k_persisted_config_lock_length = 1;
constexpr off_t k_recovery_lock_start = 1;
constexpr off_t k_recovery_lock_length = 1;
constexpr off_t k_shm_resize_lock_start = 2;
constexpr off_t k_shm_resize_lock_length = 1;
constexpr off_t k_system_tables_lock_start = 3;
constexpr off_t k_system_tables_lock_length = 1;
constexpr off_t k_dictionary_statement_lock_start = 4;
constexpr off_t k_dictionary_statement_lock_length = 1;
constexpr off_t k_global_write_statement_lock_start = 6;
constexpr off_t k_global_write_statement_lock_length = 1;
constexpr off_t k_ownerless_runtime_startup_lock_start = 0;
constexpr off_t k_ownerless_runtime_startup_lock_length = 1;
constexpr off_t k_table_statement_lock_start = 4096;
constexpr off_t k_table_statement_lock_length = 1;
constexpr std::uint64_t k_table_statement_lock_slot_count = 65536;
constexpr std::size_t k_innodb_fil_page_type_offset = 24;
constexpr std::size_t k_innodb_fil_page_space_id_offset = 34;
constexpr std::uint16_t k_innodb_fil_page_type_allocated = 0;
constexpr std::uint16_t k_innodb_fil_page_undo_log = 2;
constexpr std::uint16_t k_innodb_fil_page_inode = 3;
constexpr std::uint16_t k_innodb_fil_page_ibuf_free_list = 4;
constexpr std::uint16_t k_innodb_fil_page_ibuf_bitmap = 5;
constexpr std::uint16_t k_innodb_fil_page_type_sys = 6;
constexpr std::uint16_t k_innodb_fil_page_type_trx_sys = 7;
constexpr std::uint16_t k_innodb_fil_page_type_fsp_hdr = 8;
constexpr std::uint16_t k_innodb_fil_page_type_xdes = 9;
constexpr std::uint32_t k_innodb_page_size = 16384;
constexpr std::uint32_t k_innodb_page_size_max = 65536;
constexpr off_t k_minimum_concurrency_shm_size = 2097152;
constexpr std::array<unsigned char, 8> k_concurrency_shm_magic = {
    'M',
    'Y',
    'L',
    'S',
    'H',
    'M',
    '0',
    '1',
};
constexpr std::array<unsigned char, 8> k_concurrency_wal_magic = {
    'M',
    'Y',
    'L',
    'W',
    'A',
    'L',
    '0',
    '1',
};
constexpr std::array<unsigned char, 8> k_concurrency_checkpoint_magic = {
    'M',
    'Y',
    'L',
    'C',
    'K',
    'P',
    '0',
    '1',
};
constexpr std::size_t k_concurrency_shm_header_size = 128;
constexpr std::size_t k_concurrency_recovery_header_size = 128;
constexpr std::size_t k_concurrency_checkpoint_latest_lsn_offset =
    k_concurrency_recovery_header_size;
constexpr std::size_t k_concurrency_checkpoint_visible_lsn_offset =
    k_concurrency_checkpoint_latest_lsn_offset + sizeof(std::uint64_t);
constexpr std::size_t k_concurrency_checkpoint_native_file_op_needed_offset =
    k_concurrency_checkpoint_visible_lsn_offset + sizeof(std::uint64_t);
constexpr off_t k_concurrency_checkpoint_payload_end = static_cast<off_t>(
    k_concurrency_checkpoint_native_file_op_needed_offset + sizeof(std::uint64_t)
);
constexpr std::array<unsigned char, 8> k_concurrency_checkpoint_lsn_record_magic = {
    'M',
    'Y',
    'L',
    'C',
    'L',
    'S',
    'N',
    '1',
};
constexpr std::uint32_t k_concurrency_checkpoint_lsn_record_format = 1;
constexpr std::size_t k_concurrency_checkpoint_lsn_record_count = 2;
constexpr std::size_t k_concurrency_checkpoint_lsn_record_size = 64;
constexpr std::size_t k_concurrency_checkpoint_lsn_record_magic_offset = 0;
constexpr std::size_t k_concurrency_checkpoint_lsn_record_format_offset = 8;
constexpr std::size_t k_concurrency_checkpoint_lsn_record_reserved_offset = 12;
constexpr std::size_t k_concurrency_checkpoint_lsn_record_generation_offset = 16;
constexpr std::size_t k_concurrency_checkpoint_lsn_record_latest_lsn_offset = 24;
constexpr std::size_t k_concurrency_checkpoint_lsn_record_visible_lsn_offset = 32;
constexpr std::size_t k_concurrency_checkpoint_lsn_record_checksum_offset = 40;
constexpr off_t k_concurrency_checkpoint_lsn_records_offset = k_concurrency_checkpoint_payload_end;
constexpr std::array<unsigned char, 8> k_concurrency_checkpoint_native_file_op_record_magic = {
    'M',
    'Y',
    'L',
    'C',
    'F',
    'O',
    'P',
    '1',
};
constexpr std::uint32_t k_concurrency_checkpoint_native_file_op_record_format = 1;
constexpr std::size_t k_concurrency_checkpoint_native_file_op_record_count = 2;
constexpr std::size_t k_concurrency_checkpoint_native_file_op_record_size = 64;
constexpr std::size_t k_concurrency_checkpoint_native_file_op_record_magic_offset = 0;
constexpr std::size_t k_concurrency_checkpoint_native_file_op_record_format_offset = 8;
constexpr std::size_t k_concurrency_checkpoint_native_file_op_record_reserved_offset = 12;
constexpr std::size_t k_concurrency_checkpoint_native_file_op_record_generation_offset = 16;
constexpr std::size_t k_concurrency_checkpoint_native_file_op_record_needed_offset = 24;
constexpr std::size_t k_concurrency_checkpoint_native_file_op_record_checksum_offset = 32;
constexpr off_t k_concurrency_checkpoint_native_file_op_records_offset = static_cast<off_t>(
    k_concurrency_checkpoint_lsn_records_offset +
    (k_concurrency_checkpoint_lsn_record_count * k_concurrency_checkpoint_lsn_record_size)
);
constexpr off_t k_concurrency_checkpoint_file_end = static_cast<off_t>(
    k_concurrency_checkpoint_native_file_op_records_offset +
    (k_concurrency_checkpoint_native_file_op_record_count *
     k_concurrency_checkpoint_native_file_op_record_size)
);
constexpr off_t k_concurrency_checkpoint_lock_start = 0;
constexpr off_t k_concurrency_checkpoint_lock_length = 1;
constexpr std::size_t k_database_uuid_size = 36;
constexpr std::uint32_t k_concurrency_shm_format_version = 10;
constexpr std::uint32_t k_concurrency_recovery_format_version = 1;
constexpr std::uint32_t k_concurrency_shm_header_version_min = 1;
constexpr std::uint32_t k_concurrency_shm_byte_order = 0x01020304U;
constexpr std::uint32_t k_concurrency_shm_state_clean = 1;
constexpr std::uint32_t k_concurrency_shm_state_dirty = 2;
constexpr std::uint32_t k_concurrency_shm_state_rebuilding = 3;
constexpr std::size_t k_concurrency_shm_magic_offset = 0;
constexpr std::size_t k_concurrency_shm_format_offset = 8;
constexpr std::size_t k_concurrency_shm_min_format_offset = 12;
constexpr std::size_t k_concurrency_shm_header_size_offset = 16;
constexpr std::size_t k_concurrency_shm_byte_order_offset = 20;
constexpr std::size_t k_concurrency_shm_flags_offset = 24;
constexpr std::size_t k_concurrency_shm_state_offset = 28;
constexpr std::size_t k_concurrency_shm_mapping_size_offset = 32;
constexpr std::size_t k_concurrency_shm_generation_offset = 40;
constexpr std::size_t k_concurrency_shm_recovery_generation_offset = 48;
constexpr std::size_t k_concurrency_shm_segment_table_offset = 56;
constexpr std::size_t k_concurrency_shm_segment_count_offset = 60;
constexpr std::size_t k_concurrency_shm_database_uuid_offset = 64;
constexpr std::size_t k_concurrency_shm_device_offset = 100;
constexpr std::size_t k_concurrency_shm_inode_offset = 108;
constexpr std::size_t k_concurrency_recovery_magic_offset = 0;
constexpr std::size_t k_concurrency_recovery_format_offset = 8;
constexpr std::size_t k_concurrency_recovery_header_size_offset = 12;
constexpr std::size_t k_concurrency_recovery_byte_order_offset = 16;
constexpr std::size_t k_concurrency_recovery_flags_offset = 20;
constexpr std::size_t k_concurrency_recovery_generation_offset = 24;
constexpr std::size_t k_concurrency_recovery_database_uuid_offset = 64;
constexpr std::size_t k_concurrency_shm_segment_table_start = 128;
constexpr std::size_t k_concurrency_shm_segment_descriptor_size = 32;
constexpr std::uint32_t k_concurrency_shm_segment_count = 12;
constexpr std::size_t k_concurrency_shm_segment_type_offset = 0;
constexpr std::size_t k_concurrency_shm_segment_version_offset = 4;
constexpr std::size_t k_concurrency_shm_segment_data_offset = 8;
constexpr std::size_t k_concurrency_shm_segment_length_offset = 16;
constexpr std::size_t k_concurrency_shm_segment_generation_offset = 24;
constexpr std::uint32_t k_concurrency_process_registry_segment_type = 1;
constexpr std::uint32_t k_concurrency_process_registry_segment_version = 3;
constexpr std::uint32_t k_concurrency_wait_channel_segment_type = 2;
constexpr std::uint32_t k_concurrency_wait_channel_segment_version = 1;
constexpr std::uint32_t k_concurrency_mdl_lock_table_segment_type = 3;
constexpr std::uint32_t k_concurrency_mdl_lock_table_segment_version = 2;
constexpr std::uint32_t k_concurrency_trx_registry_segment_type = 4;
constexpr std::uint32_t k_concurrency_trx_registry_segment_version = 2;
constexpr std::uint32_t k_concurrency_read_view_registry_segment_type = 5;
constexpr std::uint32_t k_concurrency_read_view_registry_segment_version = 2;
constexpr std::uint32_t k_concurrency_innodb_lock_registry_segment_type = 6;
constexpr std::uint32_t k_concurrency_innodb_lock_registry_segment_version = 5;
constexpr std::uint32_t k_concurrency_redo_state_segment_type = 7;
constexpr std::uint32_t k_concurrency_redo_state_segment_version = 8;
constexpr std::uint32_t k_concurrency_page_index_segment_type = 8;
constexpr std::uint32_t k_concurrency_page_index_segment_version = 3;
constexpr std::uint32_t k_concurrency_dictionary_state_segment_type = 9;
constexpr std::uint32_t k_concurrency_dictionary_state_segment_version = 1;
constexpr std::uint32_t k_concurrency_page_write_lock_registry_segment_type = 10;
constexpr std::uint32_t k_concurrency_page_write_lock_registry_segment_version = 2;
constexpr std::uint32_t k_concurrency_autoinc_registry_segment_type = 11;
constexpr std::uint32_t k_concurrency_autoinc_registry_segment_version = 1;
constexpr std::uint32_t k_concurrency_page_pin_registry_segment_type = 12;
constexpr std::uint32_t k_concurrency_page_pin_registry_segment_version = 1;
constexpr std::size_t k_concurrency_process_registry_offset = 512;
constexpr std::size_t k_concurrency_process_registry_header_size =
    MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_process_slot_count = 16;
constexpr std::size_t k_concurrency_process_slot_size = MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_process_registry_size =
    k_concurrency_process_registry_header_size +
    (k_concurrency_process_slot_count * k_concurrency_process_slot_size);
constexpr std::size_t k_concurrency_wait_channel_offset =
    k_concurrency_process_registry_offset + k_concurrency_process_registry_size;
constexpr std::size_t k_concurrency_wait_channel_header_size = 64;
constexpr std::uint32_t k_concurrency_wait_channel_count = 16;
constexpr std::size_t k_concurrency_wait_channel_size = 64;
constexpr std::size_t k_concurrency_wait_channel_segment_size =
    k_concurrency_wait_channel_header_size +
    (k_concurrency_wait_channel_count * k_concurrency_wait_channel_size);
constexpr std::size_t k_concurrency_mdl_lock_table_offset =
    k_concurrency_wait_channel_offset + k_concurrency_wait_channel_segment_size;
constexpr std::size_t k_concurrency_mdl_lock_table_header_size =
    MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_mdl_lock_table_entry_count = 128;
constexpr std::size_t k_concurrency_mdl_lock_table_entry_size =
    MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE;
constexpr std::size_t k_concurrency_mdl_lock_table_segment_size =
    k_concurrency_mdl_lock_table_header_size +
    (k_concurrency_mdl_lock_table_entry_count * k_concurrency_mdl_lock_table_entry_size);
constexpr std::size_t k_concurrency_trx_registry_offset =
    k_concurrency_mdl_lock_table_offset + k_concurrency_mdl_lock_table_segment_size;
constexpr std::size_t k_concurrency_trx_registry_header_size =
    MYLITE_OWNERLESS_TRX_REGISTRY_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_trx_slot_count = 64;
constexpr std::size_t k_concurrency_trx_slot_size = MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_trx_registry_segment_size =
    k_concurrency_trx_registry_header_size +
    (k_concurrency_trx_slot_count * k_concurrency_trx_slot_size);
constexpr std::size_t k_concurrency_read_view_registry_offset =
    k_concurrency_trx_registry_offset + k_concurrency_trx_registry_segment_size;
constexpr std::size_t k_concurrency_read_view_registry_header_size =
    MYLITE_OWNERLESS_READ_VIEW_REGISTRY_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_read_view_slot_count = 64;
constexpr std::size_t k_concurrency_read_view_slot_size =
    MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_read_view_registry_segment_size =
    k_concurrency_read_view_registry_header_size +
    (k_concurrency_read_view_slot_count * k_concurrency_read_view_slot_size);
constexpr std::size_t k_concurrency_innodb_lock_registry_offset =
    k_concurrency_read_view_registry_offset + k_concurrency_read_view_registry_segment_size;
constexpr std::size_t k_concurrency_innodb_lock_registry_header_size =
    MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_innodb_lock_slot_count = 4096;
constexpr std::size_t k_concurrency_innodb_lock_slot_size =
    MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_innodb_lock_registry_segment_size =
    k_concurrency_innodb_lock_registry_header_size +
    (k_concurrency_innodb_lock_slot_count * k_concurrency_innodb_lock_slot_size);
constexpr std::size_t k_concurrency_redo_state_offset =
    ((k_concurrency_innodb_lock_registry_offset + k_concurrency_innodb_lock_registry_segment_size +
      63U) /
     64U) *
    64U;
constexpr std::size_t k_concurrency_redo_state_segment_size = MYLITE_OWNERLESS_REDO_STATE_SIZE;
constexpr std::size_t k_concurrency_redo_state_latch_offset = 0;
constexpr std::size_t k_concurrency_redo_state_latest_lsn_offset = 32;
constexpr std::size_t k_concurrency_redo_state_visible_lsn_offset =
    MYLITE_OWNERLESS_REDO_STATE_VISIBLE_LSN_OFFSET;
constexpr std::size_t k_concurrency_redo_state_refcount_offset = 48;
constexpr std::size_t k_concurrency_page_index_offset =
    k_concurrency_redo_state_offset + k_concurrency_redo_state_segment_size;
constexpr std::uint32_t k_concurrency_page_index_entry_count = 16384;
constexpr std::size_t k_concurrency_page_index_segment_size =
    MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
    (k_concurrency_page_index_entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
constexpr std::size_t k_concurrency_dictionary_state_offset =
    ((k_concurrency_page_index_offset + k_concurrency_page_index_segment_size + 63U) / 64U) * 64U;
constexpr std::size_t k_concurrency_dictionary_state_segment_size =
    MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE;
constexpr std::size_t k_concurrency_page_write_lock_registry_offset =
    ((k_concurrency_dictionary_state_offset + k_concurrency_dictionary_state_segment_size + 63U) /
     64U) *
    64U;
constexpr std::uint32_t k_concurrency_page_write_lock_slot_count = 2048;
constexpr std::size_t k_concurrency_page_write_lock_slot_size =
    MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_page_write_lock_registry_segment_size =
    k_concurrency_innodb_lock_registry_header_size +
    (k_concurrency_page_write_lock_slot_count * k_concurrency_page_write_lock_slot_size);
constexpr std::size_t k_concurrency_autoinc_registry_offset =
    ((k_concurrency_page_write_lock_registry_offset +
      k_concurrency_page_write_lock_registry_segment_size + 63U) /
     64U) *
    64U;
constexpr std::uint32_t k_concurrency_autoinc_slot_count = 2048;
constexpr std::size_t k_concurrency_autoinc_registry_segment_size =
    MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE +
    (k_concurrency_autoinc_slot_count * MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE);
constexpr std::size_t k_concurrency_page_pin_registry_offset =
    ((k_concurrency_autoinc_registry_offset + k_concurrency_autoinc_registry_segment_size + 63U) /
     64U) *
    64U;
constexpr std::uint32_t k_concurrency_page_pin_slot_count = 128;
constexpr std::size_t k_concurrency_page_pin_registry_segment_size =
    MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_HEADER_SIZE +
    (k_concurrency_page_pin_slot_count * MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE);
constexpr std::uint64_t k_empty_ownerless_page_log_size = static_cast<std::uint64_t>(
    k_concurrency_recovery_header_size + MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE
);
constexpr std::uint32_t k_ownerless_native_page_proof_capacity = 65536U;
constexpr std::uint32_t k_ownerless_innodb_record_page_heap_no =
    std::numeric_limits<std::uint32_t>::max() - 1U;
constexpr std::uint64_t k_concurrency_initial_trx_id = 1;
constexpr std::uint64_t k_ownerless_baseline_page_version_read_pin_lsn = 1;
constexpr std::uint32_t k_concurrency_process_open_mode_exclusive = 1;
constexpr std::uint32_t k_concurrency_process_open_mode_shared_readonly = 2;
constexpr std::uint32_t k_concurrency_bootstrap_latch_owner_id =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t k_concurrency_registry_slot_count_offset = 0;
constexpr std::size_t k_concurrency_registry_slot_size_offset = 4;
constexpr std::size_t k_concurrency_registry_active_count_offset = 16;
constexpr std::size_t k_concurrency_process_slot_wait_channel_offset = 64;
constexpr std::size_t k_concurrency_process_slot_wait_channel_count_offset = 72;
constexpr std::size_t k_concurrency_process_slot_state_offset = 8;
constexpr std::size_t k_concurrency_process_slot_open_mode_offset = 12;
constexpr std::size_t k_concurrency_process_slot_pid_offset = 16;
constexpr std::size_t k_concurrency_process_slot_explicit_transaction_count_offset = 80;
constexpr std::size_t k_concurrency_wait_header_channel_count_offset = 0;
constexpr std::size_t k_concurrency_wait_header_channel_size_offset = 4;
constexpr std::size_t k_concurrency_wait_header_generation_offset = 8;
constexpr std::size_t k_concurrency_mdl_lock_header_entry_count_offset = 0;
constexpr std::size_t k_concurrency_mdl_lock_header_entry_size_offset = 4;
constexpr std::size_t k_concurrency_mdl_lock_header_generation_offset = 8;
constexpr std::size_t k_concurrency_mdl_lock_header_active_count_offset = 16;
constexpr std::size_t k_concurrency_trx_header_slot_count_offset = 0;
constexpr std::size_t k_concurrency_trx_header_slot_size_offset = 4;
constexpr std::size_t k_concurrency_trx_header_active_count_offset = 16;
constexpr std::size_t k_concurrency_trx_header_next_id_offset = 24;
constexpr std::size_t k_concurrency_trx_header_oldest_active_offset = 64;
constexpr std::size_t k_concurrency_read_view_header_slot_count_offset = 0;
constexpr std::size_t k_concurrency_read_view_header_slot_size_offset = 4;
constexpr std::size_t k_concurrency_read_view_header_active_count_offset = 16;
constexpr std::size_t k_concurrency_innodb_lock_header_slot_count_offset = 0;
constexpr std::size_t k_concurrency_innodb_lock_header_slot_size_offset = 4;
constexpr std::size_t k_concurrency_innodb_lock_header_active_count_offset = 16;
constexpr std::size_t k_concurrency_innodb_lock_header_waiting_count_offset = 64;
constexpr std::size_t k_concurrency_innodb_lock_header_occupied_limit_offset = 72;
constexpr std::size_t k_concurrency_page_index_header_entry_count_offset = 32;
constexpr std::size_t k_concurrency_page_index_header_entry_size_offset = 36;
constexpr std::size_t k_concurrency_page_index_header_active_count_offset = 40;
constexpr std::size_t k_concurrency_page_pin_header_slot_count_offset = 0;
constexpr std::size_t k_concurrency_page_pin_header_slot_size_offset = 4;
constexpr std::size_t k_concurrency_page_pin_header_active_count_offset = 16;
static_assert(
    k_concurrency_shm_segment_table_start +
            (k_concurrency_shm_segment_count * k_concurrency_shm_segment_descriptor_size) <=
        k_concurrency_process_registry_offset,
    "concurrency shared-memory segment table overlaps process registry"
);
static_assert(
    k_concurrency_page_pin_registry_offset + k_concurrency_page_pin_registry_segment_size <=
        static_cast<std::size_t>(k_minimum_concurrency_shm_size),
    "concurrency shared-memory segments exceed the minimum mapping size"
);
static_assert(
    k_concurrency_redo_state_latch_offset + MYLITE_OWNERLESS_LATCH_SIZE <=
        k_concurrency_redo_state_latest_lsn_offset,
    "redo state latch overlaps redo visibility state"
);
static_assert(
    k_concurrency_checkpoint_visible_lsn_offset ==
        k_concurrency_checkpoint_latest_lsn_offset + sizeof(std::uint64_t),
    "checkpoint visible LSN must follow checkpoint latest LSN"
);
static_assert(
    k_concurrency_checkpoint_native_file_op_needed_offset ==
        k_concurrency_checkpoint_visible_lsn_offset + sizeof(std::uint64_t),
    "checkpoint native file-op marker must follow checkpoint visible LSN"
);
static_assert(
    k_concurrency_checkpoint_lsn_record_generation_offset % alignof(std::uint64_t) == 0,
    "checkpoint LSN record generation must be naturally aligned"
);
static_assert(
    k_concurrency_checkpoint_lsn_record_checksum_offset + sizeof(std::uint32_t) <=
        k_concurrency_checkpoint_lsn_record_size,
    "checkpoint LSN record checksum exceeds record"
);
static_assert(
    k_concurrency_checkpoint_native_file_op_record_generation_offset % alignof(std::uint64_t) == 0,
    "checkpoint native file-op record generation must be naturally aligned"
);
static_assert(
    k_concurrency_checkpoint_native_file_op_record_needed_offset % alignof(std::uint64_t) == 0,
    "checkpoint native file-op record needed value must be naturally aligned"
);
static_assert(
    k_concurrency_checkpoint_native_file_op_record_checksum_offset + sizeof(std::uint32_t) <=
        k_concurrency_checkpoint_native_file_op_record_size,
    "checkpoint native file-op record checksum exceeds record"
);
static_assert(
    k_concurrency_redo_state_refcount_offset + sizeof(std::uint32_t) <=
        k_concurrency_redo_state_segment_size,
    "redo state refcount exceeds redo state segment"
);

struct RuntimeLayout {
    std::filesystem::path cleanup_directory;
    std::filesystem::path cleanup_tmp_directory;
    std::filesystem::path runtime_parent_directory;
    std::filesystem::path data_directory;
    std::filesystem::path tmp_directory;
    std::filesystem::path plugin_directory;
};

struct OwnerlessRedoStartupPrefixSnapshot {
    bool captured = false;
    bool restore_file_size = false;
    std::size_t restore_prefix_size = 0;
    std::filesystem::path path;
    std::array<unsigned char, k_ownerless_redo_startup_prefix_size> prefix = {};
    off_t file_size = 0;
};

struct DatabaseLockWait {
    int lock_fd = -1;
    unsigned busy_timeout_ms = 0;
};

void release_fd_lock(int fd, off_t start, off_t length);
void release_concurrency_lock(int lock_fd, off_t start, off_t length);

struct OwnerlessStatementByteLock {
    int fd = -1;
    off_t start = 0;
    off_t length = 0;
};

struct OwnerlessStatementLockRequest {
    off_t start = 0;
    off_t length = 0;
    short lock_type = F_UNLCK;
};

struct OwnerlessStatementLocks {
    std::vector<OwnerlessStatementByteLock> locks;

    OwnerlessStatementLocks() = default;
    OwnerlessStatementLocks(const OwnerlessStatementLocks &) = delete;
    OwnerlessStatementLocks &operator=(const OwnerlessStatementLocks &) = delete;

    ~OwnerlessStatementLocks() {
        release();
    }

    void add(int fd, off_t start, off_t length) {
        locks.push_back({fd, start, length});
    }

    void release() {
        for (auto iter = locks.rbegin(); iter != locks.rend(); ++iter) {
            if (iter->fd >= 0) {
                release_fd_lock(iter->fd, iter->start, iter->length);
                iter->fd = -1;
            }
        }
        locks.clear();
    }
};

struct ParameterBinding {
    MYSQL_BIND bind = {};
    std::vector<unsigned char> bytes;
    unsigned long length = 0;
    my_bool is_null = 0;
    my_bool error = 0;
    long long int64_value = 0;
    unsigned long long uint64_value = 0;
    double double_value = 0.0;
};

struct ResultColumn {
    MYSQL_BIND bind = {};
    enum enum_field_types field_type = MYSQL_TYPE_NULL;
    unsigned int flags = 0;
    std::string name;
    std::string org_name;
    std::string table;
    std::string org_table;
    std::vector<unsigned char> buffer;
    std::vector<unsigned char> retired_buffer;
    unsigned long length = 0;
    my_bool is_null = 0;
    my_bool error = 0;
};

struct SqlPolicyTokens {
    std::array<std::string_view, k_sql_policy_token_count> values;
    std::size_t count = 0;
};

struct OwnerlessStatementFastPathPolicy {
    bool visible_fast_path = false;
    bool append_batch_fast_path = false;
    bool deferred_page_publish_fast_path = false;
};
#endif

#if MYLITE_WITH_MARIADB_EMBEDDED
struct OwnerlessMdlHookContext {
    void *lock_table = nullptr;
    std::size_t lock_table_size = 0;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
};

struct OwnerlessTrxHookContext {
    void *trx_registry = nullptr;
    std::size_t trx_registry_size = 0;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
};

struct OwnerlessReadViewHookContext {
    void *read_view_registry = nullptr;
    std::size_t read_view_registry_size = 0;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
};

constexpr std::size_t k_ownerless_page_log_negative_cache_size = 1024U;

struct OwnerlessPageLogNegativeCacheEntry {
    bool valid = false;
    std::uint32_t space_id = 0;
    std::uint32_t page_no = 0;
    std::uint64_t index_generation = 0;
    std::uint64_t max_commit_lsn = 0;
    std::uint64_t log_generation = 0;
    std::uint64_t covered_end_offset = 0;
};

struct OwnerlessInnoDBLockHookContext {
    void *process_registry = nullptr;
    std::size_t process_registry_size = 0;
    void *trx_registry = nullptr;
    std::size_t trx_registry_size = 0;
    void *lock_registry = nullptr;
    std::size_t lock_registry_size = 0;
    void *autoinc_registry = nullptr;
    std::size_t autoinc_registry_size = 0;
    void *page_write_lock_registry = nullptr;
    std::size_t page_write_lock_registry_size = 0;
    void *page_pin_registry = nullptr;
    std::size_t page_pin_registry_size = 0;
    void *redo_state = nullptr;
    std::size_t redo_state_size = 0;
    void *page_index = nullptr;
    std::size_t page_index_size = 0;
    int page_log_fd = -1;
    std::uint64_t page_log_offset = 0;
    int checkpoint_fd = -1;
    const char *database_path = nullptr;
    bool page_log_reads_enabled = false;
    bool page_versioning_enabled = false;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    std::array<OwnerlessPageLogNegativeCacheEntry, k_ownerless_page_log_negative_cache_size>
        page_log_negative_cache = {};
};

struct OwnerlessPageLogAppendBatchState {
    OwnerlessInnoDBLockHookContext *hook = nullptr;
    mylite_ownerless_page_log_append_session session = {};
};

thread_local OwnerlessPageLogAppendBatchState ownerless_page_log_append_batch = {};

struct OwnerlessProcessCleanupContext {
    void *lock_table = nullptr;
    std::size_t lock_table_size = 0;
    void *trx_registry = nullptr;
    std::size_t trx_registry_size = 0;
    void *read_view_registry = nullptr;
    std::size_t read_view_registry_size = 0;
    void *page_pin_registry = nullptr;
    std::size_t page_pin_registry_size = 0;
    void *innodb_lock_registry = nullptr;
    std::size_t innodb_lock_registry_size = 0;
    void *page_write_lock_registry = nullptr;
    std::size_t page_write_lock_registry_size = 0;
    void *redo_state = nullptr;
    std::size_t redo_state_size = 0;
    void *dictionary_state = nullptr;
    std::size_t dictionary_state_size = 0;
    std::uint32_t latch_owner_id = 0;
    std::uint64_t latch_owner_generation = 0;
};

struct RuntimeState;

struct OwnerlessPageIndexRebuildContext {
    void *page_index = nullptr;
    std::size_t page_index_size = 0;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
};

struct OwnerlessPageLogReclaimContext {
    RuntimeState *runtime = nullptr;
    std::uint64_t visible_lsn = 0;
    std::vector<mylite_ownerless_page_index_record> retained_records;
};

struct OwnerlessNativePageCheckpointRecord {
    std::uint32_t space_id = 0;
    std::uint32_t page_no = 0;
    std::uint64_t page_lsn = 0;
    std::uint64_t commit_lsn = 0;
    std::uint64_t record_offset = 0;
    bool external_snapshot_lineage_record = false;
};

struct OwnerlessNativePageCheckpointProofContext {
    RuntimeState *runtime = nullptr;
    std::uint64_t visible_lsn = 0;
    std::vector<OwnerlessNativePageCheckpointRecord> records;
    bool blocked = false;
};

struct OwnerlessPageVisibilityScope {
    ~OwnerlessPageVisibilityScope() {
        mylite_ownerless_innodb_clear_external_page_visibility();
    }
};

struct OwnerlessPressureState {
    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    std::uint64_t page_log_bytes = 0;
    std::uint64_t page_log_limit_bytes = 0;
    bool page_log_limit_reached = false;
};

struct OwnerlessPageLogSyncAnchor {
    int fd = -1;
    std::uint64_t log_offset = 0;
    std::uint64_t end_offset = 0;
    std::uint64_t generation = 0;
};

struct OwnerlessCheckpointLsnSyncAnchor {
    int fd = -1;
    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
};

struct OwnerlessCheckpointLsnRecord {
    std::uint64_t generation = 0;
    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
};

struct OwnerlessCheckpointLsnGenerationCache {
    int fd = -1;
    std::uint64_t registry_generation = 0;
    OwnerlessCheckpointLsnRecord record = {};
};

struct OwnerlessCheckpointNativeFileOpRecord {
    std::uint64_t generation = 0;
    bool needed = false;
};
#endif

struct ConcurrencyShmFileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
};

struct RuntimeState {
    std::mutex mutex;
    unsigned ref_count = 0;
    std::filesystem::path cleanup_directory;
    std::filesystem::path cleanup_tmp_directory;
    std::filesystem::path runtime_parent_directory;
    std::string database_path;
    std::vector<std::string> arguments;
    std::vector<char *> argv;
    int lock_fd = -1;
    int durability = MYLITE_DURABILITY_FULL;
    bool ownerless_rw_mode = false;
    bool readonly_mode = false;
#if MYLITE_WITH_MARIADB_EMBEDDED
    std::atomic<bool> core_system_tables_ready{false};
    int concurrency_shm_fd = -1;
    int concurrency_wal_fd = -1;
    int concurrency_checkpoint_fd = -1;
    int ownerless_statement_lock_fd = -1;
    void *concurrency_shm_mapping = nullptr;
    std::size_t concurrency_shm_mapping_size = 0;
    std::uint32_t concurrency_process_slot_index = 0;
    std::uint64_t concurrency_process_slot_generation = 0;
    OwnerlessMdlHookContext ownerless_mdl_hook = {};
    OwnerlessTrxHookContext ownerless_trx_hook = {};
    OwnerlessReadViewHookContext ownerless_read_view_hook = {};
    OwnerlessInnoDBLockHookContext ownerless_innodb_lock_hook = {};
    std::condition_variable ownerless_checkpoint_scheduler_cv;
    std::thread ownerless_checkpoint_scheduler_thread;
    std::chrono::steady_clock::time_point ownerless_last_statement_reclaim_attempt = {};
    std::chrono::steady_clock::time_point ownerless_last_statement_activity = {};
    bool ownerless_checkpoint_scheduler_stop = false;
    bool ownerless_runtime_has_local_write = false;
    bool ownerless_runtime_started_with_page_version_wal = false;
    std::atomic<bool> ownerless_runtime_consumed_page_version_wal{false};
    std::atomic<bool> ownerless_runtime_consumed_current_page_version_wal{false};
    std::atomic<bool> ownerless_runtime_consumed_external_snapshot_page_version_wal{false};
    unsigned ownerless_active_statement_count = 0;
    unsigned ownerless_active_explicit_transaction_count = 0;
#endif
};

RuntimeState g_runtime;
#if MYLITE_WITH_MARIADB_EMBEDDED
std::mutex g_system_table_mutex;
std::mutex g_ownerless_page_log_sync_anchor_mutex;
OwnerlessPageLogSyncAnchor g_ownerless_page_log_sync_anchor;
std::mutex g_ownerless_checkpoint_lsn_sync_anchor_mutex;
OwnerlessCheckpointLsnSyncAnchor g_ownerless_checkpoint_lsn_sync_anchor;
std::mutex g_ownerless_checkpoint_lsn_generation_cache_mutex;
OwnerlessCheckpointLsnGenerationCache g_ownerless_checkpoint_lsn_generation_cache;
std::atomic<std::uint64_t> g_ownerless_next_page_observation_token{1};
#endif

} // namespace

struct ErrorSnapshot {
    int errcode = MYLITE_OK;
    int extended_errcode = MYLITE_OK;
    unsigned mariadb_errno = 0;
    std::string sqlstate;
    std::string errmsg;
};

struct OwnerlessInsertForeignKeyCacheEntry {
    std::string schema_name;
    std::string table_name;
    std::uint64_t dictionary_generation = 0;
    bool has_foreign_keys = true;
};

struct OwnerlessInsertAutoIncrementCacheEntry {
    std::string schema_name;
    std::string table_name;
    std::uint64_t dictionary_generation = 0;
    bool has_auto_increment = true;
};

enum class OwnerlessTransactionIsolation {
    ReadUncommitted,
    ReadCommitted,
    RepeatableRead,
    Serializable,
};

enum class NativeControlStatement {
    None,
    Commit,
    Rollback,
    AutocommitOff,
    AutocommitOn,
    StartTransaction,
};

struct mylite_db {
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL mysql = {};
#endif
    std::string database_path;
    std::string current_schema;
    int errcode = MYLITE_OK;
    int extended_errcode = MYLITE_OK;
    unsigned mariadb_errno = 0;
    std::string sqlstate = k_sqlstate_ok;
    std::string errmsg = k_not_an_error;
    std::string warning_message;
    long long changes = 0;
    unsigned long long last_insert_id = 0;
    unsigned active_statement_count = 0;
    std::uint64_t ownerless_observed_lsn = 0;
    std::uint64_t ownerless_observed_visible_lsn = 0;
    std::uint64_t ownerless_page_version_read_lsn = 0;
    std::uint64_t ownerless_page_observation_token = 0;
    std::uint64_t ownerless_local_native_read_lsn = 0;
    std::uint64_t ownerless_page_version_read_pin_lsn = 0;
    std::uint64_t ownerless_page_version_read_pin_generation = 0;
    std::uint64_t ownerless_native_startup_refresh_lsn = 0;
    std::uint64_t ownerless_pending_post_open_clean_page_refresh_lsn = 0;
    bool ownerless_pending_post_open_clean_page_refresh_visible_boundary = false;
    bool ownerless_pending_post_open_clean_page_refresh_current_boundary = false;
    bool ownerless_preserve_native_recovery_pages = false;
    std::uint64_t ownerless_clean_pages_evicted_lsn = 0;
    std::uint64_t ownerless_clean_pages_evicted_generation = 0;
    std::uint64_t ownerless_clean_pages_evicted_visible_generation = 0;
    std::uint64_t ownerless_transaction_snapshot_visible_lsn = 0;
    std::uint64_t ownerless_transaction_snapshot_pin_lsn = 0;
    std::uint64_t ownerless_transaction_snapshot_pin_generation = 0;
    std::uint64_t ownerless_observed_dictionary_generation = 0;
    bool ownerless_observed_dictionary_generation_initialized = false;
    bool ownerless_peer_dictionary_refresh_requires_conservative_write = false;
    bool ownerless_peer_dictionary_refresh_uses_native_visible_boundary = false;
    std::uint64_t ownerless_page_log_limit_bytes = 0;
    std::vector<std::uint64_t> ownerless_page_write_trx_ids;
    std::vector<std::string> ownerless_temporary_table_names;
    std::vector<OwnerlessInsertForeignKeyCacheEntry> ownerless_insert_foreign_key_cache;
    std::vector<OwnerlessInsertAutoIncrementCacheEntry> ownerless_insert_auto_increment_cache;
    unsigned ownerless_statement_lock_wait_timeout_ms = k_statement_lock_wait_timeout_ms;
    unsigned ownerless_active_page_visibility_statement_count = 0;
    OwnerlessTransactionIsolation ownerless_session_transaction_isolation =
        OwnerlessTransactionIsolation::RepeatableRead;
    OwnerlessTransactionIsolation ownerless_next_transaction_isolation =
        OwnerlessTransactionIsolation::RepeatableRead;
    OwnerlessTransactionIsolation ownerless_active_transaction_isolation =
        OwnerlessTransactionIsolation::RepeatableRead;
    bool ownerless_explicit_transaction_active = false;
    bool ownerless_transaction_has_local_write = false;
    bool ownerless_transaction_has_locking_read = false;
    bool ownerless_transaction_visible_fast_commit_candidate = false;
    bool ownerless_transaction_visible_fast_commit_disqualified = false;
    bool ownerless_transaction_snapshot_visibility_pinned = false;
    bool ownerless_transaction_snapshot_pin_registered = false;
    bool ownerless_page_version_read_pin_registered = false;
    bool ownerless_next_transaction_isolation_set = false;
    bool ownerless_rw_open = false;
    bool readonly_open = false;
    bool connected = false;
    std::uint32_t ownerless_transaction_snapshot_pin_slot = 0;
    std::uint32_t ownerless_page_version_read_pin_slot = 0;
};

thread_local mylite_db *ownerless_current_statement_db = nullptr;
thread_local bool ownerless_statement_defers_page_log_append_batch = false;
thread_local bool ownerless_statement_allows_deferred_latest_checkpoint_coalescing = false;
thread_local bool ownerless_statement_latest_checkpoint_preserved = false;

struct OwnerlessStatementPageWriteTrackingScope {
    explicit OwnerlessStatementPageWriteTrackingScope(mylite_db &db)
        : previous(ownerless_current_statement_db) {
        ownerless_current_statement_db = &db;
    }

    ~OwnerlessStatementPageWriteTrackingScope() {
        ownerless_current_statement_db = previous;
    }

  private:
    mylite_db *previous = nullptr;
};

#if MYLITE_WITH_MARIADB_EMBEDDED
namespace {
void ownerless_page_log_append_batch_release_current();
}

struct OwnerlessStatementVisibleFastPathScope {
    explicit OwnerlessStatementVisibleFastPathScope(
        bool enabled,
        bool defer_page_log_append_batch,
        bool defer_page_publish,
        bool coalesce_deferred_latest_checkpoint
    )
        : previous(mylite_ownerless_innodb_set_statement_visible_fast_path(enabled ? 1 : 0)),
          previous_defer_page_publish(mylite_ownerless_innodb_set_statement_deferred_page_publish(
              defer_page_publish ? 1 : 0
          )),
          previous_defer_page_log_append_batch(ownerless_statement_defers_page_log_append_batch),
          previous_coalesce_deferred_latest_checkpoint(
              ownerless_statement_allows_deferred_latest_checkpoint_coalescing
          ),
          previous_latest_checkpoint_preserved(ownerless_statement_latest_checkpoint_preserved) {
        ownerless_statement_defers_page_log_append_batch = defer_page_log_append_batch;
        ownerless_statement_allows_deferred_latest_checkpoint_coalescing =
            coalesce_deferred_latest_checkpoint;
        if (coalesce_deferred_latest_checkpoint && !previous_coalesce_deferred_latest_checkpoint) {
            ownerless_statement_latest_checkpoint_preserved = false;
        }
    }

    ~OwnerlessStatementVisibleFastPathScope() {
        if (ownerless_statement_defers_page_log_append_batch) {
            ownerless_page_log_append_batch_release_current();
        }
        ownerless_statement_latest_checkpoint_preserved = previous_latest_checkpoint_preserved;
        ownerless_statement_allows_deferred_latest_checkpoint_coalescing =
            previous_coalesce_deferred_latest_checkpoint;
        ownerless_statement_defers_page_log_append_batch = previous_defer_page_log_append_batch;
        mylite_ownerless_innodb_set_statement_deferred_page_publish(previous_defer_page_publish);
        mylite_ownerless_innodb_set_statement_visible_fast_path(previous);
    }

  private:
    int previous = 0;
    int previous_defer_page_publish = 0;
    bool previous_defer_page_log_append_batch = false;
    bool previous_coalesce_deferred_latest_checkpoint = false;
    bool previous_latest_checkpoint_preserved = false;
};

struct OwnerlessStatementPlainReadScope {
    explicit OwnerlessStatementPlainReadScope(bool enabled, bool preserve_local_pages = false)
        : previous(mylite_ownerless_innodb_set_statement_plain_read(enabled ? 1 : 0)),
          previous_preserve_local_pages(
              mylite_ownerless_innodb_set_statement_plain_read_preserve_local_pages(
                  enabled && preserve_local_pages ? 1 : 0
              )
          ) {}

    ~OwnerlessStatementPlainReadScope() {
        mylite_ownerless_innodb_set_statement_plain_read_preserve_local_pages(
            previous_preserve_local_pages
        );
        mylite_ownerless_innodb_set_statement_plain_read(previous);
    }

  private:
    int previous = 0;
    int previous_preserve_local_pages = 0;
};

struct OwnerlessStatementDictionaryDdlScope {
    explicit OwnerlessStatementDictionaryDdlScope(bool enabled)
        : previous(mylite_ownerless_innodb_set_statement_dictionary_ddl(enabled ? 1 : 0)) {}

    ~OwnerlessStatementDictionaryDdlScope() {
        mylite_ownerless_innodb_set_statement_dictionary_ddl(previous);
    }

  private:
    int previous = 0;
};

struct OwnerlessStatementNativeLifecycleRefreshScope {
    explicit OwnerlessStatementNativeLifecycleRefreshScope(bool suppress)
        : previous(mylite_ownerless_innodb_set_statement_suppress_native_lifecycle_refresh(
              suppress ? 1 : 0
          )) {}

    ~OwnerlessStatementNativeLifecycleRefreshScope() {
        mylite_ownerless_innodb_set_statement_suppress_native_lifecycle_refresh(previous);
    }

  private:
    int previous = 0;
};
#else
struct OwnerlessStatementVisibleFastPathScope {
    explicit OwnerlessStatementVisibleFastPathScope(
        bool enabled,
        bool defer_page_log_append_batch,
        bool defer_page_publish,
        bool coalesce_deferred_latest_checkpoint
    ) {
        (void)enabled;
        (void)defer_page_log_append_batch;
        (void)defer_page_publish;
        (void)coalesce_deferred_latest_checkpoint;
    }
};

struct OwnerlessStatementPlainReadScope {
    explicit OwnerlessStatementPlainReadScope(bool enabled) {
        (void)enabled;
    }
};

struct OwnerlessStatementDictionaryDdlScope {
    explicit OwnerlessStatementDictionaryDdlScope(bool enabled) {
        (void)enabled;
    }
};

struct OwnerlessStatementNativeLifecycleRefreshScope {
    explicit OwnerlessStatementNativeLifecycleRefreshScope(bool suppress) {
        (void)suppress;
    }
};
#endif

struct mylite_stmt {
    mylite_db *db = nullptr;
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL_STMT *stmt = nullptr;
    MYSQL_RES *metadata = nullptr;
    std::vector<ParameterBinding> parameters;
    std::vector<MYSQL_BIND> parameter_binds;
    std::vector<ResultColumn> columns;
    std::vector<MYSQL_BIND> result_binds;
    std::unique_ptr<std::string> ownerless_sql_text;
    SqlPolicyTokens ownerless_policy_tokens = {};
    bool result_binds_dirty = false;
    bool ownerless_policy_tokens_valid = false;
    bool ownerless_page_visibility_enabled = false;
    bool ownerless_runtime_statement_active = false;
    bool ownerless_native_prepare_per_step = false;
#endif
    bool executed = false;
    bool has_result = false;
    bool has_row = false;
};

namespace {

#if MYLITE_WITH_MARIADB_EMBEDDED
void close_ownerless_ephemeral_native_statement(mylite_stmt &stmt);

struct ScopedOwnerlessEphemeralNativeStatement {
    explicit ScopedOwnerlessEphemeralNativeStatement(mylite_stmt &stmt_arg)
        : stmt(stmt_arg), active(stmt_arg.ownerless_native_prepare_per_step) {}

    ~ScopedOwnerlessEphemeralNativeStatement() {
        if (active) {
            close_ownerless_ephemeral_native_statement(stmt);
        }
    }

    mylite_stmt &stmt;
    bool active = false;
};
#endif

int open_impl(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
int validate_open_args(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
bool shared_readonly_open_available(void);
bool ownerless_rw_open_available(void);
#if MYLITE_WITH_MARIADB_EMBEDDED
int validate_runtime_database_path(mylite_db &db);
int prepare_database_directory(const std::filesystem::path &database_path, unsigned flags);
int validate_ownerless_platform_for_database(mylite_db &db);
int prepare_existing_database_directory(const std::filesystem::path &database_path, unsigned flags);
int validate_database_layout(const std::filesystem::path &database_path);
int validate_layout_directory(const std::filesystem::path &directory);
int validate_database_metadata(const std::filesystem::path &metadata_path);
bool ownerless_concurrency_runtime_files_exist(const std::filesystem::path &database_path);
bool ownerless_platform_probe_proof_matches(
    const std::filesystem::path &metadata_path,
    std::uint64_t database_device
);
int write_ownerless_platform_probe_proof(
    const std::filesystem::path &metadata_path,
    std::uint64_t database_device
);
int prepare_concurrency_metadata(const std::filesystem::path &database_path);
int prepare_concurrency_shared_memory(
    const std::filesystem::path &database_path,
    bool allow_recovery_rebuild
);
int read_concurrency_database_uuid(
    const std::filesystem::path &metadata_path,
    std::string &database_uuid
);
int prepare_concurrency_recovery_files(
    const std::filesystem::path &concurrency_directory,
    std::string_view database_uuid
);
int prepare_concurrency_recovery_file(
    const std::filesystem::path &file_path,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
);
int prepare_concurrency_checkpoint_file(
    const std::filesystem::path &file_path,
    std::string_view database_uuid
);
bool concurrency_recovery_header_matches(
    const std::array<unsigned char, k_concurrency_recovery_header_size> &header,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
);
void build_concurrency_recovery_header(
    std::array<unsigned char, k_concurrency_recovery_header_size> &header,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
);
int prepare_concurrency_shm_layout(
    const std::filesystem::path &database_path,
    int shm_fd,
    int page_log_fd,
    int checkpoint_fd,
    off_t shm_size,
    const ConcurrencyShmFileIdentity &shm_identity,
    std::string_view database_uuid,
    bool allow_recovery_rebuild,
    bool initial_shared_memory
);
int replay_concurrency_tablespaces(
    const std::filesystem::path &database_path,
    int page_log_fd,
    int checkpoint_fd
);
int discard_stale_reader_page_log(int page_log_fd);
bool concurrency_shm_header_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    const ConcurrencyShmFileIdentity &shm_identity,
    std::string_view database_uuid
);
bool concurrency_shm_header_layout_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    const ConcurrencyShmFileIdentity &shm_identity,
    std::string_view database_uuid
);
bool concurrency_shm_segments_match(int shm_fd, off_t shm_size);
bool concurrency_shm_has_stale_reader_state_without_recovery(int shm_fd, off_t shm_size);
bool concurrency_shm_rebuild_requires_recovery(int shm_fd, off_t shm_size);
bool concurrency_shm_header_identity_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    std::string_view database_uuid
);
void build_concurrency_shm_header(
    std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    const ConcurrencyShmFileIdentity &shm_identity,
    std::string_view database_uuid,
    std::uint64_t recovery_generation
);
ConcurrencyShmFileIdentity concurrency_shm_file_identity(const struct stat &shm_stat);
int initialize_concurrency_shm_segments(
    int shm_fd,
    int page_log_fd,
    int checkpoint_fd,
    std::uint64_t next_trx_id
);
bool write_concurrency_segment_descriptor(
    int shm_fd,
    std::uint32_t index,
    std::uint32_t type,
    std::uint32_t version,
    std::uint64_t offset,
    std::uint64_t length
);
int initialize_concurrency_process_registry(int shm_fd);
int initialize_concurrency_wait_channels(int shm_fd);
int initialize_concurrency_mdl_lock_table(int shm_fd);
std::uint64_t read_concurrency_trx_next_id_from_shm(int shm_fd);
int initialize_concurrency_trx_registry(int shm_fd, std::uint64_t next_trx_id);
int initialize_concurrency_read_view_registry(int shm_fd);
int initialize_concurrency_page_pin_registry(int shm_fd);
int initialize_concurrency_innodb_lock_registry(int shm_fd);
int initialize_concurrency_page_write_lock_registry(int shm_fd);
int initialize_concurrency_redo_state(int shm_fd, int checkpoint_fd);
int initialize_concurrency_page_index(int shm_fd, int page_log_fd);
int initialize_concurrency_dictionary_state(int shm_fd);
int initialize_concurrency_autoinc_registry(int shm_fd);
void reclaim_ownerless_page_log_after_native_checkpoint(RuntimeState &runtime);
bool ownerless_page_log_payload_bytes(RuntimeState &runtime, std::uint64_t *out_page_log_bytes);
bool ownerless_page_log_checkpoint_due(RuntimeState &runtime);
bool ownerless_runtime_in_single_owner_epoch_locked(RuntimeState &runtime);
bool ownerless_page_log_has_uncheckpointed_records(RuntimeState &runtime);
bool ownerless_page_log_has_payload_records(RuntimeState &runtime);
bool ownerless_runtime_has_live_shared_readonly_peer(RuntimeState &runtime);
bool clear_ownerless_native_file_op_checkpoint_without_page_log(RuntimeState &runtime);
bool ownerless_autoinc_checkpoint_pending(RuntimeState &runtime);
bool clear_ownerless_autoinc_checkpoint_pending(RuntimeState &runtime);
bool seed_ownerless_runtime_redo_state_checkpoint(
    RuntimeState &runtime,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
);
int seed_ownerless_native_checkpoint_baseline(
    RuntimeState &runtime,
    std::uint64_t *out_baseline_lsn
);
bool ownerless_statement_checkpoint_has_no_active_pins(RuntimeState &runtime);
void ownerless_checkpoint_scheduler_loop(RuntimeState *runtime);
int start_ownerless_checkpoint_scheduler(RuntimeState &runtime);
void stop_ownerless_checkpoint_scheduler(RuntimeState &runtime, std::unique_lock<std::mutex> &lock);
bool begin_ownerless_runtime_statement(mylite_db &db);
void end_ownerless_runtime_statement(mylite_db &db);
void set_ownerless_explicit_transaction_active(mylite_db &db, bool active);
void publish_ownerless_explicit_transaction_count_locked(RuntimeState &runtime);
void advance_ownerless_handle_read_lsn_after_autocommit_write(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool statement_started_in_explicit_transaction
);
void maybe_reclaim_ownerless_page_log_after_statement(mylite_db &db, const SqlPolicyTokens &tokens);
void mark_ownerless_native_file_op_checkpoint_after_dictionary_ddl(
    mylite_db &db,
    const SqlPolicyTokens &tokens
);
bool advance_ownerless_no_live_page_visible_lsn_for_reclaim(
    RuntimeState &runtime,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    std::uint64_t *out_visible_lsn,
    bool force_native_checkpoint
);
bool prepare_ownerless_page_log_native_checkpoint_for_reclaim(
    RuntimeState &runtime,
    std::uint64_t visible_lsn,
    bool skip_external_refresh,
    bool require_native_page_lsn_proof,
    bool allow_no_live_consumed_native_successor
);
bool ownerless_page_log_has_native_page_lsn_proof(
    RuntimeState &runtime,
    std::uint64_t visible_lsn,
    bool allow_no_live_consumed_native_successor
);
bool ownerless_live_peer_page_log_reclaim_safe(RuntimeState &runtime, std::uint64_t visible_lsn);
bool ownerless_page_log_record_is_native_support_state(
    RuntimeState &runtime,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset
);
bool ownerless_page_image_is_native_support_state(const void *page, std::uint32_t page_size);
bool ownerless_native_page_checkpoint_record_is_better(
    const OwnerlessNativePageCheckpointRecord &candidate,
    const OwnerlessNativePageCheckpointRecord &current
);
bool verify_ownerless_native_page_checkpoint_latest_record(
    RuntimeState &runtime,
    const OwnerlessNativePageCheckpointRecord &record,
    std::uint64_t visible_lsn,
    bool allow_no_live_consumed_native_successor
);
bool ownerless_file_per_table_page_matches(
    RuntimeState &runtime,
    const OwnerlessNativePageCheckpointRecord &record
);
bool ownerless_file_per_table_page_is_discarded(
    RuntimeState &runtime,
    const OwnerlessNativePageCheckpointRecord &record
);
bool ownerless_file_per_table_space_is_absent(RuntimeState &runtime, std::uint32_t space_id);
int collect_ownerless_native_page_checkpoint_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
);
bool publish_ownerless_snapshot_boundary_if_needed(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t visible_lsn,
    std::uint32_t page_size
);
bool ownerless_runtime_has_no_live_peers(RuntimeState &runtime);
bool ownerless_runtime_has_no_live_explicit_transactions(RuntimeState &runtime);
bool ownerless_runtime_live_reclaim_has_no_native_write_state(RuntimeState &runtime);
int snapshot_ownerless_page_version_pins(
    RuntimeState &runtime,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
);
bool ownerless_runtime_has_external_page_version_pin(RuntimeState &runtime);
int replay_concurrency_page_index(void *page_index, std::size_t page_index_size, int page_log_fd);
int replay_concurrency_page_index_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
);
int collect_ownerless_reclaimed_page_index_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
);
int replace_ownerless_page_index_after_reclaim(void *context);
int page_log_result_from_page_index_result(int result);
int read_concurrency_process_active_count(int shm_fd, std::uint64_t *out_active_count);
int read_concurrency_process_live_count(int shm_fd, std::uint64_t *out_live_count);
int validate_concurrency_shm_mapping(int shm_fd, off_t shm_size, std::string_view database_uuid);
int map_concurrency_shared_memory_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
);
int open_concurrency_page_log_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
);
int open_concurrency_checkpoint_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
);
int allocate_concurrency_process_slot(RuntimeState &runtime);
int install_ownerless_runtime_lifecycle_hooks(RuntimeState &runtime);
int install_ownerless_innodb_lock_hooks(RuntimeState &runtime);
int install_ownerless_runtime_hooks(RuntimeState &runtime);
int refresh_ownerless_external_pages_before_statement(
    mylite_db &db,
    bool allow_page_version_reads,
    bool allow_global_refresh,
    bool force_native_flush,
    bool *out_page_version_reads_enabled
);
int read_ownerless_pressure_state(mylite_db &db, OwnerlessPressureState &state);
int enforce_ownerless_page_log_limit_policy(mylite_db &db, const SqlPolicyTokens &tokens);
int refresh_ownerless_dictionary_before_statement(mylite_db &db, bool allow_global_refresh);
bool ownerless_observed_dictionary_generation_ready(const mylite_db &db, void *dictionary_state);
int flush_ownerless_dictionary_cache(mylite_db &db);
int refresh_ownerless_dictionary_cache_after_stale_engine_error(mylite_db &db);
void initialize_ownerless_dictionary_generation(mylite_db &db);
void clear_ownerless_insert_foreign_key_cache(mylite_db &db);
bool ownerless_dictionary_ddl_statement(const SqlPolicyTokens &tokens);
bool ownerless_dictionary_ddl_needs_native_file_op_checkpoint(const SqlPolicyTokens &tokens);
bool ownerless_stale_engine_error_allows_retry(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool statement_started_in_explicit_transaction
);
bool ownerless_temporary_table_ddl_statement(const SqlPolicyTokens &tokens);
bool ownerless_table_identifier_token(std::string_view token);
std::string ownerless_normalized_identifier(std::string_view token);
bool ownerless_tracked_temporary_table_name(const mylite_db &db, std::string_view table_name);
bool ownerless_table_reference_skip_token(std::string_view token);
std::string_view ownerless_raw_identifier_token_at(
    const SqlPolicyTokens &tokens,
    std::size_t index
);
bool ownerless_statement_uses_tracked_temporary_table(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
);
bool ownerless_statement_uses_temporary_table(const mylite_db &db, const SqlPolicyTokens &tokens);
std::string ownerless_temporary_table_name_from_ddl(const SqlPolicyTokens &tokens);
void update_ownerless_temporary_table_state_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
);
std::vector<OwnerlessStatementLockRequest> ownerless_autocommit_write_statement_lock_requests(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
);
int ownerless_begin_dictionary_ddl(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool *out_ddl_started
);
int ownerless_finish_dictionary_ddl(mylite_db &db, bool ddl_started);
int ownerless_dictionary_result_from_state_result(int state_result);
bool ownerless_connection_is_in_explicit_transaction(const mylite_db &db);
bool ownerless_transaction_has_local_write_or_locking_read(const mylite_db &db);
bool ownerless_connection_allows_global_refresh(const mylite_db &db, bool allow_page_version_reads);
bool statement_allows_ownerless_page_version_reads(const SqlPolicyTokens &tokens);
bool statement_is_tableless_ownerless_plain_read(const SqlPolicyTokens &tokens);
bool ownerless_select_statement_has_table_reference(const SqlPolicyTokens &tokens);
std::uint64_t ownerless_handle_observed_read_lsn(const mylite_db &db);
std::uint64_t ownerless_monotonic_page_version_read_lsn(
    const mylite_db &db,
    std::uint64_t read_lsn
);
int update_ownerless_transaction_state_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
);
bool ownerless_transaction_pins_consistent_reads(const mylite_db &db);
int ensure_ownerless_consistent_snapshot_start_pin(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool *out_pin_registered
);
int open_ownerless_page_version_pin(
    mylite_db &db,
    std::uint64_t read_lsn,
    std::uint32_t *out_slot,
    std::uint64_t *out_generation
);
bool close_ownerless_page_version_pin(mylite_db &db, std::uint32_t slot, std::uint64_t generation);
int ensure_ownerless_handle_page_version_pin(mylite_db &db, std::uint64_t read_lsn);
void release_ownerless_handle_page_version_pin(mylite_db &db);
void release_ownerless_completed_statement_page_visibility(
    mylite_db &db,
    bool close_current_read_view,
    bool release_handle_pin = true
);
void reset_ownerless_application_read_refresh_state(mylite_db &db);
void refresh_ownerless_pending_post_open_clean_pages(mylite_db &db);
int ensure_ownerless_transaction_page_version_pin(mylite_db &db, std::uint64_t read_lsn);
void release_ownerless_transaction_page_version_pin(mylite_db &db);
void update_ownerless_transaction_isolation_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
);
void update_ownerless_statement_lock_timeout_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
);
bool sql_sets_ownerless_statement_lock_timeout(const SqlPolicyTokens &tokens, unsigned *out_ms);
bool sql_sets_transaction_isolation(
    const SqlPolicyTokens &tokens,
    OwnerlessTransactionIsolation *out_isolation,
    bool *out_session_scope
);
bool sql_starts_consistent_snapshot_transaction(const SqlPolicyTokens &tokens);
bool sql_starts_explicit_transaction(const SqlPolicyTokens &tokens);
bool sql_ends_explicit_transaction(const SqlPolicyTokens &tokens);
bool sql_chains_transaction(const SqlPolicyTokens &tokens);
bool ownerless_transaction_end_has_local_write(const mylite_db &db, const SqlPolicyTokens &tokens);
bool ownerless_transaction_end_blocks_waiting_native_lock(mylite_db &db);
int acquire_ownerless_statement_locks(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    OwnerlessStatementLocks &lock
);
int ownerless_statement_lock_fd(mylite_db &db);
int ownerless_runtime_statement_lock_fd(RuntimeState &runtime);
bool acquire_ownerless_live_reclaim_statement_gate(
    RuntimeState &runtime,
    OwnerlessStatementLocks &lock
);
void unmap_concurrency_shared_memory_for_runtime(RuntimeState &runtime);
void reset_ownerless_runtime_hooks(RuntimeState &runtime);
void reset_ownerless_native_shutdown_hooks(RuntimeState &runtime);
void advance_ownerless_local_trx_horizon(RuntimeState &runtime);
void clear_ownerless_native_hook_contexts(RuntimeState &runtime);
void reset_ownerless_page_log_sync_anchor();
void reset_ownerless_checkpoint_lsn_sync_anchor();
void reset_ownerless_checkpoint_lsn_generation_cache();
bool ownerless_checkpoint_lsn_sync_anchor_matches(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
);
void ownerless_checkpoint_lsn_sync_anchor_store(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
);
int sync_ownerless_page_log_if_changed(OwnerlessInnoDBLockHookContext *hook);
void release_concurrency_owner_state(RuntimeState &runtime);
void release_concurrency_process_slot(RuntimeState &runtime);
int ownerless_mdl_acquire_hook(
    const mylite_ownerless_mdl_key_view *key,
    double lock_wait_timeout,
    void *ctx
);
void ownerless_mdl_release_hook(const mylite_ownerless_mdl_key_view *key, void *ctx);
unsigned ownerless_mdl_timeout_ms(double lock_wait_timeout);
int ownerless_mdl_result_from_lock_table_result(int lock_table_result);
int ownerless_trx_allocate_hook(std::uint64_t *out_trx_id, void *ctx);
int ownerless_trx_register_hook(std::uint64_t *out_trx_id, void *ctx);
int ownerless_trx_assign_no_hook(std::uint64_t trx_id, std::uint64_t *out_trx_no, void *ctx);
int ownerless_trx_deregister_hook(std::uint64_t trx_id, void *ctx);
int ownerless_trx_snapshot_hook(
    std::uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_min_trx_no,
    void *ctx
);
int ownerless_trx_result_from_registry_result(int registry_result);
int ownerless_trx_deregister_result_from_registry_result(int registry_result);
int ownerless_read_view_register_hook(
    std::uint64_t low_limit_id,
    std::uint64_t low_limit_no,
    const std::uint64_t *trx_ids,
    unsigned int trx_id_count,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation,
    void *ctx
);
int ownerless_read_view_deregister_hook(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    void *ctx
);
int ownerless_read_view_snapshot_hook(
    std::uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    std::uint64_t *out_low_limit_id,
    std::uint64_t *out_low_limit_no,
    void *ctx
);
int ownerless_read_view_result_from_registry_result(int registry_result);
int ownerless_innodb_lock_acquire_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned int timeout_ms,
    void *ctx
);
int ownerless_innodb_lock_release_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    void *ctx
);
int ownerless_innodb_lock_wait_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    std::uint64_t blocker_trx_id,
    void *ctx
);
int ownerless_innodb_lock_wait_until_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned int timeout_ms,
    void *ctx
);
int ownerless_innodb_lock_acquire_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    void *ctx
);
int ownerless_innodb_lock_release_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
);
int ownerless_innodb_lock_acquire_page_write_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    std::uint32_t *out_acquire_flags,
    void *ctx
);
int ownerless_innodb_lock_release_page_write_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
);
int ownerless_innodb_lock_release_page_writes_hook(std::uint64_t trx_id, void *ctx);
void record_ownerless_page_write_trx_id(mylite_db &db, std::uint64_t trx_id);
void forget_ownerless_page_write_trx_id(mylite_db &db, std::uint64_t trx_id);
int release_ownerless_page_write_trx_ids(mylite_db &db);
int ownerless_innodb_lock_wait_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    std::uint64_t blocker_trx_id,
    void *ctx
);
int ownerless_innodb_lock_wait_until_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    void *ctx
);
int ownerless_innodb_lock_before_record_wait_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
);
void normalize_ownerless_record_lock_resource(
    std::uint32_t mode,
    std::uint32_t *heap_no,
    std::uint32_t *flags
);
bool ownerless_record_lock_uses_page_resource(std::uint32_t mode, std::uint32_t flags);
int ownerless_innodb_lock_clear_wait_hook(std::uint64_t trx_id, void *ctx);
int ownerless_innodb_autoinc_read_hook(
    std::uint64_t table_id,
    std::uint64_t seed_next_value,
    std::uint64_t *out_next_value,
    void *ctx
);
int ownerless_innodb_autoinc_publish_hook(
    std::uint64_t table_id,
    std::uint64_t next_value,
    void *ctx
);
int ownerless_innodb_redo_enter_hook(std::uint64_t *out_latest_lsn, void *ctx);
int ownerless_innodb_redo_observe_hook(std::uint64_t *out_latest_lsn, void *ctx);
int ownerless_innodb_redo_reserve_hook(
    std::uint64_t current_lsn,
    std::uint64_t length,
    std::uint64_t *out_start_lsn,
    std::uint64_t *out_end_lsn,
    void *ctx
);
int ownerless_innodb_redo_written_hook(
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t *out_written_lsn,
    void *ctx
);
int ownerless_innodb_redo_written_leave_hook(
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t latest_lsn,
    std::uint64_t *out_written_lsn,
    void *ctx
);
void ownerless_innodb_redo_leave_hook(std::uint64_t latest_lsn, void *ctx);
void ownerless_innodb_pages_visible_hook(std::uint64_t visible_lsn, void *ctx);
void ownerless_persist_redo_checkpoint(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
);
void ownerless_innodb_page_publish_batch_begin_hook(void *ctx);
void ownerless_innodb_page_publish_batch_end_hook(void *ctx);
void ownerless_page_log_append_batch_release_for_snapshot(OwnerlessInnoDBLockHookContext *hook);
int ownerless_innodb_history_proof_publish_pair_hook(
    std::uint32_t space_id,
    std::uint32_t rseg_page_no,
    std::uint64_t rseg_page_lsn,
    const void *rseg_page,
    std::uint32_t rseg_page_size,
    std::uint32_t undo_page_no,
    std::uint64_t undo_page_lsn,
    const void *undo_page,
    std::uint32_t undo_page_size,
    std::uint64_t visible_lsn,
    void *ctx
);
bool ownerless_test_fails_native_support_page_publish(bool native_support_page);
int append_ownerless_page_version(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t visible_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t page_checksum,
    bool native_support_page,
    bool external_snapshot_lineage,
    std::uint32_t publish_flags,
    std::uint64_t *out_record_offset
);
void pause_for_ownerless_test_fault(const char *fault_name);
int ownerless_innodb_page_publish_hook(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t visible_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint32_t publish_flags,
    void *ctx
);
int ownerless_innodb_page_read_hook(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags,
    void *ctx
);
int ownerless_innodb_skip_external_page_refresh_hook(void *ctx);
int ownerless_innodb_page_read_locked(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags
);
std::uint32_t ownerless_innodb_page_version_flags(std::uint32_t page_log_flags);
bool ownerless_page_log_negative_cache_absence_lookup(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    std::uint64_t index_generation,
    std::uint64_t log_generation,
    std::uint64_t snapshot_end_offset,
    std::uint64_t *out_scan_start_offset
);
void ownerless_page_log_negative_cache_store(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    std::uint64_t index_generation,
    std::uint64_t log_generation,
    std::uint64_t covered_end_offset
);
int ownerless_innodb_lock_result_from_registry_result(int registry_result);
int ownerless_innodb_lock_result_from_page_index_result(int index_result);
int ownerless_runtime_may_delete_shared_file_hook(void *ctx);
unsigned char *runtime_process_registry(RuntimeState &runtime);
unsigned char *runtime_process_slot(RuntimeState &runtime);
unsigned char *runtime_trx_registry(RuntimeState &runtime);
unsigned char *runtime_read_view_registry(RuntimeState &runtime);
unsigned char *runtime_page_pin_registry(RuntimeState &runtime);
unsigned char *runtime_innodb_lock_registry(RuntimeState &runtime);
unsigned char *runtime_autoinc_registry(RuntimeState &runtime);
unsigned char *runtime_page_write_lock_registry(RuntimeState &runtime);
unsigned char *runtime_redo_state(RuntimeState &runtime);
unsigned char *runtime_page_index(RuntimeState &runtime);
unsigned char *runtime_dictionary_state(RuntimeState &runtime);
std::uint32_t ownerless_owner_id_from_slot_index(std::uint32_t slot_index);
int ownerless_process_is_alive(std::uint64_t pid, void *ctx);
bool ownerless_process_registry_has_other_live_explicit_transactions(
    const void *registry,
    std::size_t registry_size,
    std::uint32_t owner_id
);
bool ownerless_trx_registry_has_other_active_transactions(OwnerlessInnoDBLockHookContext *hook);
int ownerless_process_cleanup_dead_owner_state(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t pid,
    void *ctx
);
int ownerless_process_cleanup_owner_state(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t pid,
    void *ctx
);
int ownerless_process_release_owner_page_write_locks(
    OwnerlessProcessCleanupContext &cleanup,
    std::uint32_t owner_id
);
bool ownerless_process_owner_state_requires_recovery(
    OwnerlessProcessCleanupContext &cleanup,
    std::uint32_t owner_id
);
int mylite_result_from_process_registry_result(int registry_result);
bool update_concurrency_shm_state(int shm_fd, std::uint32_t state);
bool update_concurrency_checkpoint_lsn(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
);
bool update_concurrency_checkpoint_lsn_from_redo_state(
    int checkpoint_fd,
    void *redo_state,
    std::size_t redo_state_size,
    const void *process_registry,
    std::size_t process_registry_size,
    std::uint64_t owner_generation,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
);
bool ownerless_checkpoint_generation_cache_allowed(
    const void *process_registry,
    std::size_t process_registry_size,
    std::uint64_t owner_generation,
    std::uint64_t *out_registry_generation
);
bool write_concurrency_checkpoint_lsn_with_current_record_locked(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable,
    const OwnerlessCheckpointLsnRecord &current_record,
    bool has_current_record,
    OwnerlessCheckpointLsnRecord *out_record
);
bool write_concurrency_checkpoint_lsn_with_generation_cache_locked(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable,
    std::uint64_t registry_generation
);
bool read_concurrency_checkpoint_lsn_records(
    int checkpoint_fd,
    OwnerlessCheckpointLsnRecord *out_record,
    bool *out_has_record
);
bool read_concurrency_checkpoint_legacy_lsn(
    int checkpoint_fd,
    std::uint64_t *out_latest_lsn,
    std::uint64_t *out_visible_lsn
);
bool read_concurrency_checkpoint_lsn(
    int checkpoint_fd,
    std::uint64_t *out_latest_lsn,
    std::uint64_t *out_visible_lsn
);
void build_concurrency_checkpoint_lsn_record(
    std::array<unsigned char, k_concurrency_checkpoint_lsn_record_size> &record,
    std::uint64_t generation,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
);
bool parse_concurrency_checkpoint_lsn_record(
    const std::array<unsigned char, k_concurrency_checkpoint_lsn_record_size> &record,
    OwnerlessCheckpointLsnRecord *out_record,
    bool *out_empty
);
off_t concurrency_checkpoint_lsn_record_offset(std::size_t index);
bool read_concurrency_native_file_op_checkpoint_records(
    int checkpoint_fd,
    OwnerlessCheckpointNativeFileOpRecord *out_record,
    bool *out_has_record,
    bool *out_saw_nonempty_record
);
void build_concurrency_native_file_op_checkpoint_record(
    std::array<unsigned char, k_concurrency_checkpoint_native_file_op_record_size> &record,
    std::uint64_t generation,
    bool needed
);
bool parse_concurrency_native_file_op_checkpoint_record(
    const std::array<unsigned char, k_concurrency_checkpoint_native_file_op_record_size> &record,
    OwnerlessCheckpointNativeFileOpRecord *out_record,
    bool *out_empty
);
off_t concurrency_native_file_op_checkpoint_record_offset(std::size_t index);
bool write_concurrency_native_file_op_checkpoint_locked(int checkpoint_fd, bool needed);
bool read_concurrency_native_file_op_checkpoint_legacy_needed(int checkpoint_fd, bool *out_needed);
bool mark_concurrency_native_file_op_checkpoint_needed(int checkpoint_fd);
bool read_concurrency_native_file_op_checkpoint_needed(int checkpoint_fd, bool *out_needed);
bool clear_concurrency_native_file_op_checkpoint_needed(int checkpoint_fd);
bool acquire_fd_write_lock(int fd, off_t start, off_t length);
bool acquire_fd_range_lock(int fd, off_t start, off_t length, short lock_type, unsigned timeout_ms);
void release_fd_lock(int fd, off_t start, off_t length);
std::uint64_t current_time_milliseconds(void);
bool read_exact_at(int fd, unsigned char *data, std::size_t length, off_t offset);
bool write_exact_at(int fd, const unsigned char *data, std::size_t length, off_t offset);
bool sync_fd_data(int fd);
int capture_ownerless_redo_startup_prefix(
    const std::filesystem::path &database_path,
    OwnerlessRedoStartupPrefixSnapshot &snapshot,
    bool repair_from_backup,
    bool update_backup
);
bool read_ownerless_redo_header_backup(
    const std::filesystem::path &database_path,
    const std::filesystem::path &redo_path,
    off_t redo_file_size,
    OwnerlessRedoStartupPrefixSnapshot &snapshot
);
bool ownerless_redo_header_backup_is_valid(const std::filesystem::path &database_path);
bool restore_ownerless_redo_startup_prefix(const OwnerlessRedoStartupPrefixSnapshot &snapshot);
bool restore_ownerless_redo_shutdown_header_if_needed(
    const OwnerlessRedoStartupPrefixSnapshot &snapshot
);
std::uint32_t load_le32(const unsigned char *bytes, std::size_t offset);
std::uint64_t load_le64(const unsigned char *bytes, std::size_t offset);
std::uint32_t load_be32(const unsigned char *bytes, std::size_t offset);
std::uint64_t load_be64(const unsigned char *bytes, std::size_t offset);
void store_le32(unsigned char *bytes, std::size_t offset, std::uint32_t value);
void store_le64(unsigned char *bytes, std::size_t offset, std::uint64_t value);
std::uint64_t load_shared64(const unsigned char *bytes, std::size_t offset);
int acquire_concurrency_lock(
    const std::filesystem::path &lock_path,
    off_t start,
    off_t length,
    short lock_type
);
int acquire_concurrency_lock(
    const std::filesystem::path &lock_path,
    off_t start,
    off_t length,
    short lock_type,
    unsigned timeout_ms
);
int acquire_concurrency_lock(const std::filesystem::path &lock_path, off_t start, off_t length);
void release_concurrency_lock(int lock_fd, off_t start, off_t length);
int validate_concurrency_metadata(const std::filesystem::path &metadata_path);
bool database_directory_is_empty(
    const std::filesystem::path &database_path,
    std::error_code &error
);
int start_runtime(mylite_db &db, unsigned flags, const mylite_open_config *config);
int connect_runtime(mylite_db &db);
int ensure_core_system_tables(mylite_db &db);
int execute_core_system_table_statements(mylite_db &db);
int execute_system_table_statement(mylite_db &db, const char *sql);
int acquire_database_lock(
    mylite_db &db,
    const std::filesystem::path &database_path,
    const mylite_open_config *config
);
int wait_for_database_lock(DatabaseLockWait wait);
void release_database_lock(int lock_fd);
unsigned configured_busy_timeout_ms(const mylite_open_config *config);
bool unsafe_disable_database_lock_for_tests(void);
void cleanup_runtime_layout(const RuntimeLayout &layout);
#endif
void cleanup_runtime_state(RuntimeState &runtime);
void clear_runtime_state(RuntimeState &runtime);
void remove_directory_if_empty(const std::filesystem::path &directory);
void close_connection(mylite_db &db);
void release_runtime(void);
int exec_result_impl(
    mylite_db *db,
    const char *sql,
    mylite_exec_result_metadata_callback metadata_callback,
    mylite_exec_result_callback row_callback,
    void *ctx,
    char **errmsg
);
int prepare_impl(
    mylite_db *db,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
);

struct LegacyExecCallbackContext {
    mylite_exec_callback callback = nullptr;
    void *ctx = nullptr;
    std::vector<char *> column_names;
};

int legacy_exec_result_callback(
    void *ctx,
    int column_count,
    char **values,
    const std::size_t *value_lengths,
    const mylite_exec_column *columns
) {
    (void)value_lengths;
    LegacyExecCallbackContext *legacy_context = static_cast<LegacyExecCallbackContext *>(ctx);
    if (legacy_context == nullptr || legacy_context->callback == nullptr) {
        return 0;
    }

    legacy_context->column_names.clear();
    legacy_context->column_names.reserve(static_cast<std::size_t>(column_count));
    for (int column = 0; column < column_count; ++column) {
        const char *name = columns[column].name != nullptr ? columns[column].name : "";
        legacy_context->column_names.push_back(const_cast<char *>(name));
    }
    mylite_exec_callback legacy_callback = legacy_context->callback;
    return legacy_callback(
        legacy_context->ctx,
        column_count,
        values,
        legacy_context->column_names.data()
    );
}
#if MYLITE_WITH_MARIADB_EMBEDDED
int store_and_emit_result(
    mylite_db &db,
    mylite_exec_result_metadata_callback metadata_callback,
    mylite_exec_result_callback callback,
    void *ctx,
    bool *has_result
);
int drain_remaining_query_results(mylite_db &db);
NativeControlStatement classify_native_control_statement(std::string_view sql);
bool sql_tokens_have_only_optional_trailing_semicolon(
    const std::array<std::string_view, 6> &tokens,
    std::size_t token_count,
    std::size_t required_count
);
int execute_native_control_statement(mylite_db &db, NativeControlStatement statement);
bool native_control_autocommit_is_noop(const mylite_db &db, NativeControlStatement statement);
void rollback_active_transaction_after_deadlock(mylite_db &db);
void rollback_failed_ownerless_implicit_statement(
    mylite_db &db,
    bool statement_started_in_explicit_transaction
);
int rollback_active_transaction(mylite_db &db);
ErrorSnapshot capture_error(const mylite_db &db);
void restore_error(mylite_db &db, const ErrorSnapshot &snapshot);
int initialize_statement_results(mylite_stmt &stmt, bool release_existing_results);
int fetch_statement_row(mylite_stmt &stmt);
int drain_remaining_statement_results(mylite_stmt &stmt);
int refresh_dirty_result_binds(mylite_stmt &stmt);
int fetch_truncated_statement_columns(mylite_stmt &stmt);
int configure_column_buffer(ResultColumn &column, unsigned long buffer_length);
int allocate_column_buffer(std::vector<unsigned char> &buffer, unsigned long buffer_length);
void release_statement_results(mylite_stmt &stmt);
void clear_statement_ownerless_runtime_activity(mylite_stmt &stmt);
void enable_statement_ownerless_page_visibility(mylite_stmt &stmt, bool enabled);
void clear_statement_ownerless_page_visibility(mylite_stmt &stmt);
int retry_ownerless_prepare_after_stale_engine_error(
    mylite_stmt &stmt,
    const char *sql,
    std::size_t sql_len
);
int retry_ownerless_prepared_execute_after_stale_engine_error(mylite_stmt &stmt);
int prepare_ownerless_ephemeral_native_statement(mylite_stmt &stmt);
void close_ownerless_ephemeral_native_statement(mylite_stmt &stmt);
int build_ownerless_prepared_text_sql(mylite_stmt &stmt, std::string &out_sql);
int append_ownerless_prepared_parameter_sql(
    mylite_stmt &stmt,
    const ParameterBinding &parameter,
    std::string &out_sql
);
int append_ownerless_prepared_text_literal(
    mylite_stmt &stmt,
    const ParameterBinding &parameter,
    std::string &out_sql
);
int append_ownerless_prepared_blob_literal(const ParameterBinding &parameter, std::string &out_sql);
ParameterBinding *parameter_at(mylite_stmt &stmt, unsigned index);
int bind_null_value(mylite_stmt &stmt, unsigned index);
int bind_bytes(
    mylite_stmt &stmt,
    unsigned index,
    const void *value,
    std::size_t value_len,
    enum enum_field_types buffer_type,
    mylite_destructor destructor
);
void bind_parameter_buffer(ParameterBinding &parameter);
int bind_parameters(mylite_stmt &stmt);
mylite_value_type column_type(const ResultColumn &column);
const ResultColumn *metadata_column_at(const mylite_stmt *stmt, unsigned column);
const ResultColumn *value_column_at(const mylite_stmt *stmt, unsigned column);
void set_mariadb_statement_error(mylite_db &db, MYSQL_STMT *stmt);
void set_mariadb_statement_error(mylite_stmt &stmt);
int reject_unsupported_sql_policy(mylite_db &db, std::string_view sql);
void update_current_schema_after_successful_sql(mylite_db &db, std::string_view sql);
void update_current_schema_after_successful_sql(mylite_db &db, const SqlPolicyTokens &tokens);
bool is_unsupported_server_surface_sql(
    const SqlPolicyTokens &tokens,
    const std::string &current_schema
);
bool is_readonly_rejected_sql_statement(const mylite_db &db, const SqlPolicyTokens &tokens);
bool sql_statement_requires_write(const SqlPolicyTokens &tokens);
bool ownerless_prepared_write_defers_native_prepare(
    std::string_view sql,
    const SqlPolicyTokens &tokens
);
bool count_sql_parameter_markers(std::string_view sql, std::size_t *out_count);
bool sql_contains_identifier_token(std::string_view sql, const char *keyword);
bool ownerless_transaction_commit_allows_visible_fast_path(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
);
OwnerlessStatementFastPathPolicy ownerless_statement_fast_path_policy(
    mylite_db &db,
    std::string_view sql,
    const SqlPolicyTokens &tokens
);
bool ownerless_statement_deferred_latest_checkpoint_coalescing_allowed(
    const mylite_db &db,
    bool statement_append_batch_fast_path,
    bool statement_started_in_explicit_transaction
);
void reset_ownerless_transaction_visible_fast_proof(mylite_db &db);
void disqualify_ownerless_transaction_visible_fast_proof(mylite_db &db);
void update_ownerless_explicit_transaction_visible_fast_proof_before_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool statement_visible_fast_path
);
void disqualify_ownerless_explicit_transaction_visible_fast_proof_after_failed_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool statement_started_in_explicit_transaction
);
bool ownerless_insert_target_table(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::string *out_schema,
    std::string *out_table
);
bool ownerless_insert_statement_has_target_column_list(const SqlPolicyTokens &tokens);
std::string ownerless_escape_metadata_literal(mylite_db &db, std::string_view value);
bool ownerless_cached_insert_target_foreign_key_state(
    const mylite_db &db,
    std::string_view schema_name,
    std::string_view table_name,
    bool *out_has_foreign_keys
);
void ownerless_cache_insert_target_foreign_key_state(
    mylite_db &db,
    std::string_view schema_name,
    std::string_view table_name,
    bool has_foreign_keys
);
bool ownerless_insert_target_has_foreign_keys(mylite_db &db, const SqlPolicyTokens &tokens);
bool ownerless_cached_insert_target_auto_increment_state(
    const mylite_db &db,
    std::string_view schema_name,
    std::string_view table_name,
    bool *out_has_auto_increment
);
void ownerless_cache_insert_target_auto_increment_state(
    mylite_db &db,
    std::string_view schema_name,
    std::string_view table_name,
    bool has_auto_increment
);
bool ownerless_insert_target_has_auto_increment(mylite_db &db, const SqlPolicyTokens &tokens);
bool sql_statement_requests_write_transaction(const SqlPolicyTokens &tokens);
bool sql_statement_uses_locking_read(const SqlPolicyTokens &tokens);
bool sql_statement_needs_ownerless_current_read_refresh(const SqlPolicyTokens &tokens);
bool is_unsupported_oracle_sql_mode_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_procedure_analyse_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_vector_runtime_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_xml_sql_function_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_dynamic_column_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_table_directory_option_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_ownerless_engine_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_routine_ddl_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_routine_execution_statement(
    const mylite_db &db,
    std::string_view sql
);
bool is_unsupported_ownerless_sequence_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_table_admin_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_lock_tables_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_flush_table_lock_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_isolation_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_special_index_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_partition_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_tablespace_management_statement(
    const mylite_db &db,
    std::string_view sql
);
bool is_unsupported_ownerless_table_storage_option_statement(
    const mylite_db &db,
    std::string_view sql
);
bool is_unsupported_account_or_event_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_plugin_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_udf_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_replication_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_binlog_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_xa_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_replication_function_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_vector_sql_function_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_vector_index_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_xml_sql_function_call(const SqlPolicyTokens &tokens);
bool is_unsupported_dynamic_column_function_call(const SqlPolicyTokens &tokens);
bool is_unsupported_server_utility_function_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_sql_handler_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_select_file_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_load_file_import_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_help_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_static_show_info_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_processlist_metadata_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_thread_control_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_foreign_server_metadata_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_backup_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_userstat_diagnostics_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool is_unsupported_user_variable_diagnostics_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool is_unsupported_statement_profiling_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool is_unsupported_query_cache_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_query_log_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_optimizer_trace_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool is_unsupported_persistent_statistics_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_server_set_statement(const SqlPolicyTokens &tokens);
bool sql_sets_non_innodb_storage_engine_variable(const SqlPolicyTokens &tokens);
bool sql_uses_non_innodb_table_engine(const SqlPolicyTokens &tokens);
bool sql_statement_can_use_table_engine_option(const SqlPolicyTokens &tokens);
bool sql_assigns_transaction_isolation_variable(const SqlPolicyTokens &tokens);
bool is_ownerless_storage_engine_variable(std::string_view token);
bool is_ownerless_supported_default_engine(std::string_view engine);
bool is_ownerless_supported_table_engine(std::string_view engine);
SqlPolicyTokens collect_sql_policy_tokens(std::string_view sql);
bool next_sql_token(std::string_view sql, std::size_t &offset, std::string_view &token);
void skip_sql_spacing_and_comments(std::string_view sql, std::size_t &offset);
bool enter_executable_sql_comment(std::string_view sql, std::size_t &offset);
bool skip_dash_sql_comment(std::string_view sql, std::size_t &offset);
bool skip_hash_sql_comment(std::string_view sql, std::size_t &offset);
bool skip_block_sql_comment(std::string_view sql, std::size_t &offset);
void skip_quoted_sql_token(std::string_view sql, std::size_t &offset);
bool is_sql_space(char value);
bool is_sql_identifier_char(char value);
bool is_sql_identifier_token(std::string_view token);
std::string_view first_identifier_token(std::string_view sql);
std::string_view identifier_token_at(const SqlPolicyTokens &tokens, std::size_t index);
std::string_view unquoted_identifier_token(std::string_view token);
bool has_identifier_token(
    const SqlPolicyTokens &tokens,
    const char *keyword,
    std::size_t start_index
);
bool has_information_schema_userstat_statistics_table(const SqlPolicyTokens &tokens);
bool has_current_schema_userstat_statistics_table_reference(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool has_information_schema_table(const SqlPolicyTokens &tokens, const char *table_name);
bool has_current_schema_table_reference(
    const SqlPolicyTokens &tokens,
    const char *table_name,
    std::string_view current_schema
);
bool has_unqualified_table_reference(const SqlPolicyTokens &tokens, const char *table_name);
bool is_sql_mode_assignment_target(const SqlPolicyTokens &tokens, std::size_t index);
bool sql_mode_assignment_mentions_oracle(const SqlPolicyTokens &tokens, std::size_t index);
bool token_contains_sql_mode_name(std::string_view token, const char *mode_name);
bool token_equals(std::string_view token, const char *keyword);
bool identifier_token_equals(std::string_view token, const char *keyword);
bool table_reference_keyword(std::string_view token);
bool token_in(std::string_view token, const char *first, const char *second);
bool token_in(std::string_view token, const char *first, const char *second, const char *third);
bool token_in(
    std::string_view token,
    const char *first,
    const char *second,
    const char *third,
    const char *fourth
);
bool is_userstat_statistics_table_token(std::string_view token);
bool is_server_variable_token(std::string_view token);
bool is_query_log_variable_token(std::string_view token);
bool is_persistent_statistics_variable_token(std::string_view token);
bool is_system_variable_qualified_token(const SqlPolicyTokens &tokens, std::size_t index);
bool is_system_variable_assignment_start(const SqlPolicyTokens &tokens, std::size_t index);
std::size_t first_set_assignment_token_index(const SqlPolicyTokens &tokens);
std::filesystem::path normalize_database_path(const char *path);
bool is_memory_database_path(const std::filesystem::path &database_path);
void initialize_database_layout(const std::filesystem::path &database_path);
void create_layout_directory(const std::filesystem::path &directory, const char *message);
void write_database_metadata(const std::filesystem::path &metadata_path);
void write_concurrency_metadata(const std::filesystem::path &metadata_path);
std::string generate_database_uuid(void);
void fill_database_uuid_bytes(std::array<unsigned char, 16> &bytes);
void fill_database_uuid_bytes_from_fallback(std::array<unsigned char, 16> &bytes);
bool is_database_uuid(std::string_view value);
bool is_unsigned_decimal(std::string_view value);
RuntimeLayout create_runtime_layout(
    const std::filesystem::path &database_path,
    const mylite_open_config *config,
    bool allow_stale_cleanup
);
RuntimeLayout create_memory_runtime_layout(const mylite_open_config *config);
RuntimeLayout create_persistent_runtime_layout(
    const std::filesystem::path &database_path,
    bool allow_stale_cleanup
);
std::filesystem::path runtime_root(const mylite_open_config *config);
std::string unique_runtime_name(void);
int configured_durability(const mylite_open_config *config);
const char *innodb_flush_log_at_trx_commit_option(int durability);
void create_runtime_subdirectory(const std::filesystem::path &directory, const char *message);
std::vector<std::string> runtime_arguments(
    const RuntimeLayout &layout,
    bool ownerless_rw_open,
    bool readonly_open,
    int durability
);
std::vector<char *> mutable_arguments(std::vector<std::string> &arguments);
void remove_directory_contents_if_present(const std::filesystem::path &directory);
#endif
void remove_directory_if_present(const std::filesystem::path &directory);
int copy_error_message(mylite_db &db, char **errmsg);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_ok(mylite_db &db);
#endif
void set_error(mylite_db &db, int code, const char *message);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &db);
int parse_warning_level(const char *level);
#endif
const char *safe_c_str(const std::string &value);
bool has_config_field(const mylite_open_config *config, std::size_t field_end);

#if MYLITE_WITH_MARIADB_EMBEDDED
struct ScopedConcurrencyLock {
    int fd = -1;
    off_t start = 0;
    off_t length = 0;

    ScopedConcurrencyLock() = default;
    ScopedConcurrencyLock(const ScopedConcurrencyLock &) = delete;
    ScopedConcurrencyLock &operator=(const ScopedConcurrencyLock &) = delete;

    ~ScopedConcurrencyLock() {
        release();
    }

    bool acquire(
        const std::filesystem::path &lock_path,
        off_t lock_start,
        off_t lock_length,
        unsigned timeout_ms
    ) {
        release();
        const int acquired_fd =
            acquire_concurrency_lock(lock_path, lock_start, lock_length, F_WRLCK, timeout_ms);
        if (acquired_fd < 0) {
            return false;
        }
        fd = acquired_fd;
        start = lock_start;
        length = lock_length;
        return true;
    }

    void release() {
        if (fd < 0) {
            return;
        }
        release_concurrency_lock(fd, start, length);
        fd = -1;
    }
};

struct ScopedOwnerlessRuntimeStatement {
    mylite_db *db = nullptr;
    bool active = false;

    explicit ScopedOwnerlessRuntimeStatement(mylite_db &database)
        : db(&database), active(begin_ownerless_runtime_statement(database)) {}

    ScopedOwnerlessRuntimeStatement(const ScopedOwnerlessRuntimeStatement &) = delete;
    ScopedOwnerlessRuntimeStatement &operator=(const ScopedOwnerlessRuntimeStatement &) = delete;

    ~ScopedOwnerlessRuntimeStatement() {
        release();
    }

    void release() {
        if (!active || db == nullptr) {
            return;
        }
        end_ownerless_runtime_statement(*db);
        active = false;
    }

    bool dismiss() {
        const bool was_active = active;
        active = false;
        db = nullptr;
        return was_active;
    }
};
#endif

} // namespace

int mylite_open(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    return open_impl(path, out_db, flags, config);
}

unsigned long long mylite_capabilities(void) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    unsigned long long capabilities = MYLITE_CAP_SAME_PROCESS_CONCURRENCY;
    if (shared_readonly_open_available()) {
        capabilities |= MYLITE_CAP_SHARED_READONLY;
    }
    if (ownerless_rw_open_available()) {
        capabilities |= MYLITE_CAP_OWNERLESS_RW;
    }
    return capabilities;
#else
    return 0U;
#endif
}

int mylite_ownerless_pressure_status(mylite_db *db, mylite_ownerless_pressure_info *out_info) {
    if (db == nullptr || out_info == nullptr ||
        out_info->size < sizeof(mylite_ownerless_pressure_info)) {
        return MYLITE_MISUSE;
    }

    const std::size_t caller_size = out_info->size;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->size = caller_size;

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    set_ok(*db);
    OwnerlessPressureState state;
    const int result = read_ownerless_pressure_state(*db, state);
    if (result != MYLITE_OK) {
        return result;
    }

    out_info->active_page_version_pin_count = state.active_pin_count;
    out_info->page_version_wal_limit_reached = state.page_log_limit_reached ? 1 : 0;
    out_info->oldest_page_version_pin_lsn = state.oldest_pin_lsn;
    out_info->page_version_wal_bytes = state.page_log_bytes;
    out_info->page_version_wal_limit_bytes = state.page_log_limit_bytes;
    return MYLITE_OK;
#endif
}

int mylite_close(mylite_db *db) {
    if (db == nullptr) {
        return MYLITE_OK;
    }
#if MYLITE_WITH_MARIADB_EMBEDDED
    EmbeddedOpenPerfScope close_scope(EMBEDDED_OPEN_PERF_CLOSE_TOTAL_NS);
    embedded_open_perf_add(EMBEDDED_OPEN_PERF_CLOSE_CALLS, 1U);
#endif
    if (db->active_statement_count > 0U) {
        set_error(*db, MYLITE_BUSY, "database has active statements");
        return MYLITE_BUSY;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    std::uint64_t stage_start_ns = embedded_open_perf_start_ns();
    if (rollback_active_transaction(*db) != MYLITE_OK) {
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_CLOSE_ROLLBACK_NS, stage_start_ns);
        return MYLITE_ERROR;
    }
    release_ownerless_handle_page_version_pin(*db);
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_CLOSE_ROLLBACK_NS, stage_start_ns);
#endif
#if MYLITE_WITH_MARIADB_EMBEDDED
    stage_start_ns = embedded_open_perf_start_ns();
#endif
    close_connection(*db);
#if MYLITE_WITH_MARIADB_EMBEDDED
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_CLOSE_CONNECTION_NS, stage_start_ns);
    stage_start_ns = embedded_open_perf_start_ns();
#endif
    release_runtime();
#if MYLITE_WITH_MARIADB_EMBEDDED
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_CLOSE_RELEASE_RUNTIME_NS, stage_start_ns);
#endif
    delete db;
    return MYLITE_OK;
}

int mylite_exec(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
) {
    LegacyExecCallbackContext legacy_context;
    legacy_context.callback = callback;
    legacy_context.ctx = ctx;
    return exec_result_impl(
        db,
        sql,
        nullptr,
        callback != nullptr ? legacy_exec_result_callback : nullptr,
        &legacy_context,
        errmsg
    );
}

int mylite_exec_result(
    mylite_db *db,
    const char *sql,
    mylite_exec_result_callback callback,
    void *ctx,
    char **errmsg
) {
    return exec_result_impl(db, sql, nullptr, callback, ctx, errmsg);
}

int mylite_exec_result_with_metadata(
    mylite_db *db,
    const char *sql,
    mylite_exec_result_metadata_callback metadata_callback,
    mylite_exec_result_callback row_callback,
    void *ctx,
    char **errmsg
) {
    return exec_result_impl(db, sql, metadata_callback, row_callback, ctx, errmsg);
}

int mylite_prepare(
    mylite_db *db,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
) {
    return prepare_impl(db, sql, sql_len, out_stmt, tail);
}

int mylite_step(mylite_stmt *stmt) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    set_ok(*stmt->db);
    if (!stmt->executed) {
        if (!stmt->db->ownerless_rw_open) {
            const int bind_result = bind_parameters(*stmt);
            if (bind_result != MYLITE_OK) {
                return bind_result;
            }
            const int initial_result_setup = initialize_statement_results(*stmt, true);
            if (initial_result_setup != MYLITE_OK) {
                return initial_result_setup;
            }
            refresh_ownerless_pending_post_open_clean_pages(*stmt->db);
            OwnerlessStatementPlainReadScope native_startup_plain_read(
                stmt->db->ownerless_native_startup_refresh_lsn != 0U
            );
            if (mysql_stmt_execute(stmt->stmt) != 0) {
                set_mariadb_statement_error(*stmt);
                return MYLITE_ERROR;
            }

            stmt->db->changes = 0;
            stmt->db->last_insert_id =
                static_cast<unsigned long long>(mysql_stmt_insert_id(stmt->stmt));
            stmt->executed = true;

            if (!stmt->has_result && mysql_stmt_field_count(stmt->stmt) != 0U) {
                const int result_setup = initialize_statement_results(*stmt, false);
                if (result_setup != MYLITE_OK) {
                    return result_setup;
                }
            }
            if (!stmt->has_result) {
                const my_ulonglong affected_rows = mysql_stmt_affected_rows(stmt->stmt);
                stmt->db->changes = affected_rows == static_cast<my_ulonglong>(-1)
                                        ? 0
                                        : static_cast<long long>(std::min<my_ulonglong>(
                                              affected_rows,
                                              static_cast<my_ulonglong>(LLONG_MAX)
                                          ));
                return MYLITE_DONE;
            }
            return fetch_statement_row(*stmt);
        }

        if (!stmt->ownerless_policy_tokens_valid) {
            set_error(*stmt->db, MYLITE_ERROR, "ownerless prepared statement policy is missing");
            return MYLITE_ERROR;
        }
        OwnerlessDatabasePerfScope ownerless_step_perf_scope(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_TOTAL_NS
        );
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PREPARED_STEP_CALLS, 1U);
        const SqlPolicyTokens &policy_tokens = stmt->ownerless_policy_tokens;
        std::uint64_t ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        const int pressure_result =
            enforce_ownerless_page_log_limit_policy(*stmt->db, policy_tokens);
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_PRESSURE_NS,
            ownerless_stage_start
        );
        if (pressure_result != MYLITE_OK) {
            return pressure_result;
        }
        ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        ScopedOwnerlessRuntimeStatement runtime_statement(*stmt->db);
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_RUNTIME_STATEMENT_NS,
            ownerless_stage_start
        );
        ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        const bool statement_uses_temporary_table =
            ownerless_statement_uses_temporary_table(*stmt->db, policy_tokens);
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_TEMPORARY_TABLE_NS,
            ownerless_stage_start
        );
        OwnerlessStatementLocks statement_locks;
        ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        const int statement_lock_result =
            acquire_ownerless_statement_locks(*stmt->db, policy_tokens, statement_locks);
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_STATEMENT_LOCK_NS,
            ownerless_stage_start
        );
        if (statement_lock_result != MYLITE_OK) {
            return statement_lock_result;
        }
        const bool allow_page_version_reads =
            !statement_uses_temporary_table &&
            !stmt->db->ownerless_peer_dictionary_refresh_requires_conservative_write &&
            statement_allows_ownerless_page_version_reads(policy_tokens);
        const bool allow_current_read_refresh =
            !statement_uses_temporary_table &&
            (sql_statement_needs_ownerless_current_read_refresh(policy_tokens) ||
             ownerless_transaction_end_has_local_write(*stmt->db, policy_tokens));
        const bool tableless_ownerless_plain_read =
            statement_is_tableless_ownerless_plain_read(policy_tokens);
        ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        bool page_version_reads_enabled = false;
        const int refresh_result = refresh_ownerless_external_pages_before_statement(
            *stmt->db,
            allow_page_version_reads,
            !statement_uses_temporary_table && !tableless_ownerless_plain_read &&
                (ownerless_connection_allows_global_refresh(*stmt->db, allow_page_version_reads) ||
                 allow_current_read_refresh),
            ownerless_dictionary_ddl_statement(policy_tokens),
            &page_version_reads_enabled
        );
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_REFRESH_NS,
            ownerless_stage_start
        );
        if (refresh_result != MYLITE_OK) {
            if (page_version_reads_enabled) {
                release_ownerless_completed_statement_page_visibility(
                    *stmt->db,
                    !ownerless_connection_is_in_explicit_transaction(*stmt->db)
                );
            }
            return refresh_result;
        }
        ScopedOwnerlessEphemeralNativeStatement ephemeral_native_statement(*stmt);
        std::string ownerless_prepared_text_sql;
        if (stmt->ownerless_native_prepare_per_step) {
            ownerless_stage_start =
                ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
            const int text_sql_result =
                build_ownerless_prepared_text_sql(*stmt, ownerless_prepared_text_sql);
            ownerless_database_perf_add_elapsed(
                OWNERLESS_DATABASE_PERF_PREPARED_STEP_BIND_NS,
                ownerless_stage_start
            );
            if (text_sql_result != MYLITE_OK) {
                if (page_version_reads_enabled) {
                    release_ownerless_completed_statement_page_visibility(
                        *stmt->db,
                        !ownerless_connection_is_in_explicit_transaction(*stmt->db)
                    );
                }
                return text_sql_result;
            }
            enable_statement_ownerless_page_visibility(*stmt, page_version_reads_enabled);
        } else {
            const int ephemeral_prepare_result =
                prepare_ownerless_ephemeral_native_statement(*stmt);
            if (ephemeral_prepare_result != MYLITE_OK) {
                if (page_version_reads_enabled) {
                    release_ownerless_completed_statement_page_visibility(
                        *stmt->db,
                        !ownerless_connection_is_in_explicit_transaction(*stmt->db)
                    );
                }
                return ephemeral_prepare_result;
            }
            enable_statement_ownerless_page_visibility(*stmt, page_version_reads_enabled);
            ownerless_stage_start =
                ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
            const int bind_result = bind_parameters(*stmt);
            ownerless_database_perf_add_elapsed(
                OWNERLESS_DATABASE_PERF_PREPARED_STEP_BIND_NS,
                ownerless_stage_start
            );
            if (bind_result != MYLITE_OK) {
                clear_statement_ownerless_page_visibility(*stmt);
                return bind_result;
            }
            ownerless_stage_start =
                ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
            const int initial_result_setup = initialize_statement_results(*stmt, true);
            ownerless_database_perf_add_elapsed(
                OWNERLESS_DATABASE_PERF_PREPARED_STEP_RESULT_SETUP_NS,
                ownerless_stage_start
            );
            if (initial_result_setup != MYLITE_OK) {
                clear_statement_ownerless_page_visibility(*stmt);
                return initial_result_setup;
            }
        }
        bool dictionary_ddl_started = false;
        ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        const int dictionary_ddl_result =
            ownerless_begin_dictionary_ddl(*stmt->db, policy_tokens, &dictionary_ddl_started);
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_DICTIONARY_BEGIN_NS,
            ownerless_stage_start
        );
        if (dictionary_ddl_result != MYLITE_OK) {
            clear_statement_ownerless_page_visibility(*stmt);
            return dictionary_ddl_result;
        }
        OwnerlessStatementDictionaryDdlScope dictionary_ddl_scope(dictionary_ddl_started);
        bool consistent_snapshot_start_pin_registered = false;
        ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        const int consistent_snapshot_pin_result = ensure_ownerless_consistent_snapshot_start_pin(
            *stmt->db,
            policy_tokens,
            &consistent_snapshot_start_pin_registered
        );
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_SNAPSHOT_PIN_NS,
            ownerless_stage_start
        );
        if (consistent_snapshot_pin_result != MYLITE_OK) {
            const int dictionary_finish_result =
                ownerless_finish_dictionary_ddl(*stmt->db, dictionary_ddl_started);
            if (dictionary_finish_result != MYLITE_OK) {
                set_error(
                    *stmt->db,
                    dictionary_finish_result,
                    "ownerless dictionary change could not finish after failed statement"
                );
                clear_statement_ownerless_page_visibility(*stmt);
                return dictionary_finish_result;
            }
            clear_statement_ownerless_page_visibility(*stmt);
            return consistent_snapshot_pin_result;
        }
        const bool statement_started_in_explicit_transaction =
            ownerless_connection_is_in_explicit_transaction(*stmt->db);
        OwnerlessStatementPageWriteTrackingScope page_write_tracking(*stmt->db);
        OwnerlessStatementPlainReadScope plain_read(
            page_version_reads_enabled,
            stmt->db->ownerless_local_native_read_lsn != 0U ||
                ownerless_transaction_has_local_write_or_locking_read(*stmt->db)
        );
        OwnerlessStatementNativeLifecycleRefreshScope native_lifecycle_refresh(
            dictionary_ddl_started ||
            stmt->db->ownerless_peer_dictionary_refresh_requires_conservative_write
        );
        const OwnerlessStatementFastPathPolicy fast_path_policy =
            ownerless_statement_fast_path_policy(
                *stmt->db,
                ownerless_prepared_text_sql,
                policy_tokens
            );
        update_ownerless_explicit_transaction_visible_fast_proof_before_sql(
            *stmt->db,
            policy_tokens,
            fast_path_policy.visible_fast_path
        );
        OwnerlessStatementVisibleFastPathScope visible_fast_path(
            fast_path_policy.visible_fast_path,
            fast_path_policy.append_batch_fast_path,
            fast_path_policy.deferred_page_publish_fast_path,
            ownerless_statement_deferred_latest_checkpoint_coalescing_allowed(
                *stmt->db,
                fast_path_policy.append_batch_fast_path,
                statement_started_in_explicit_transaction
            )
        );
        ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        my_ulonglong ownerless_prepared_affected_rows = static_cast<my_ulonglong>(-1);
        if (stmt->ownerless_native_prepare_per_step) {
            if (mysql_real_query(
                    &stmt->db->mysql,
                    ownerless_prepared_text_sql.data(),
                    static_cast<unsigned long>(ownerless_prepared_text_sql.size())
                ) != 0) {
                ownerless_database_perf_add_elapsed(
                    OWNERLESS_DATABASE_PERF_PREPARED_STEP_MYSQL_EXECUTE_NS,
                    ownerless_stage_start
                );
                set_mariadb_error(*stmt->db);
                disqualify_ownerless_explicit_transaction_visible_fast_proof_after_failed_sql(
                    *stmt->db,
                    policy_tokens,
                    statement_started_in_explicit_transaction
                );
                const int dictionary_finish_result =
                    ownerless_finish_dictionary_ddl(*stmt->db, dictionary_ddl_started);
                if (dictionary_finish_result != MYLITE_OK) {
                    set_error(
                        *stmt->db,
                        dictionary_finish_result,
                        "ownerless dictionary change could not finish after failed statement"
                    );
                    clear_statement_ownerless_page_visibility(*stmt);
                    return dictionary_finish_result;
                }
                if (consistent_snapshot_start_pin_registered) {
                    release_ownerless_transaction_page_version_pin(*stmt->db);
                }
                rollback_failed_ownerless_implicit_statement(
                    *stmt->db,
                    statement_started_in_explicit_transaction
                );
                rollback_active_transaction_after_deadlock(*stmt->db);
                clear_statement_ownerless_page_visibility(*stmt);
                return MYLITE_ERROR;
            }
            bool has_text_result = false;
            const int drain_result =
                store_and_emit_result(*stmt->db, nullptr, nullptr, nullptr, &has_text_result);
            if (drain_result != MYLITE_OK || has_text_result) {
                ownerless_database_perf_add_elapsed(
                    OWNERLESS_DATABASE_PERF_PREPARED_STEP_MYSQL_EXECUTE_NS,
                    ownerless_stage_start
                );
                if (has_text_result) {
                    set_error(
                        *stmt->db,
                        MYLITE_ERROR,
                        "ownerless prepared text write unexpectedly returned result metadata"
                    );
                }
                disqualify_ownerless_explicit_transaction_visible_fast_proof_after_failed_sql(
                    *stmt->db,
                    policy_tokens,
                    statement_started_in_explicit_transaction
                );
                const int dictionary_finish_result =
                    ownerless_finish_dictionary_ddl(*stmt->db, dictionary_ddl_started);
                if (dictionary_finish_result != MYLITE_OK) {
                    set_error(
                        *stmt->db,
                        dictionary_finish_result,
                        "ownerless dictionary change could not finish after failed statement"
                    );
                    clear_statement_ownerless_page_visibility(*stmt);
                    return dictionary_finish_result;
                }
                if (consistent_snapshot_start_pin_registered) {
                    release_ownerless_transaction_page_version_pin(*stmt->db);
                }
                clear_statement_ownerless_page_visibility(*stmt);
                return MYLITE_ERROR;
            }
            ownerless_prepared_affected_rows = mysql_affected_rows(&stmt->db->mysql);
            stmt->db->last_insert_id =
                static_cast<unsigned long long>(mysql_insert_id(&stmt->db->mysql));
        } else {
            if (mysql_stmt_execute(stmt->stmt) != 0) {
                ownerless_database_perf_add_elapsed(
                    OWNERLESS_DATABASE_PERF_PREPARED_STEP_MYSQL_EXECUTE_NS,
                    ownerless_stage_start
                );
                set_mariadb_statement_error(*stmt);
                disqualify_ownerless_explicit_transaction_visible_fast_proof_after_failed_sql(
                    *stmt->db,
                    policy_tokens,
                    statement_started_in_explicit_transaction
                );
                if (ownerless_stale_engine_error_allows_retry(
                        *stmt->db,
                        policy_tokens,
                        statement_started_in_explicit_transaction
                    ) &&
                    retry_ownerless_prepared_execute_after_stale_engine_error(*stmt) == MYLITE_OK) {
                    set_ok(*stmt->db);
                    ownerless_stage_start = ownerless_database_perf_stats_are_enabled()
                                                ? ownerless_database_perf_now_ns()
                                                : 0U;
                    if (mysql_stmt_execute(stmt->stmt) == 0) {
                        goto ownerless_prepared_execute_success;
                    }
                    ownerless_database_perf_add_elapsed(
                        OWNERLESS_DATABASE_PERF_PREPARED_STEP_MYSQL_EXECUTE_NS,
                        ownerless_stage_start
                    );
                    set_mariadb_statement_error(*stmt);
                }
                const int dictionary_finish_result =
                    ownerless_finish_dictionary_ddl(*stmt->db, dictionary_ddl_started);
                if (dictionary_finish_result != MYLITE_OK) {
                    set_error(
                        *stmt->db,
                        dictionary_finish_result,
                        "ownerless dictionary change could not finish after failed statement"
                    );
                    clear_statement_ownerless_page_visibility(*stmt);
                    return dictionary_finish_result;
                }
                if (consistent_snapshot_start_pin_registered) {
                    release_ownerless_transaction_page_version_pin(*stmt->db);
                }
                rollback_failed_ownerless_implicit_statement(
                    *stmt->db,
                    statement_started_in_explicit_transaction
                );
                rollback_active_transaction_after_deadlock(*stmt->db);
                clear_statement_ownerless_page_visibility(*stmt);
                return MYLITE_ERROR;
            }
        ownerless_prepared_execute_success:
            stmt->db->last_insert_id =
                static_cast<unsigned long long>(mysql_stmt_insert_id(stmt->stmt));
        }
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_MYSQL_EXECUTE_NS,
            ownerless_stage_start
        );
        refresh_ownerless_pending_post_open_clean_pages(*stmt->db);
        stmt->db->changes = 0;
        stmt->executed = true;
        ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        update_ownerless_temporary_table_state_after_successful_sql(*stmt->db, policy_tokens);
        const int transaction_state_result =
            update_ownerless_transaction_state_after_successful_sql(*stmt->db, policy_tokens);
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_POST_STATE_NS,
            ownerless_stage_start
        );
        update_ownerless_statement_lock_timeout_after_successful_sql(*stmt->db, policy_tokens);
        if (transaction_state_result != MYLITE_OK) {
            clear_statement_ownerless_page_visibility(*stmt);
            return transaction_state_result;
        }
        ownerless_stage_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        const int dictionary_finish_result =
            ownerless_finish_dictionary_ddl(*stmt->db, dictionary_ddl_started);
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_DICTIONARY_FINISH_NS,
            ownerless_stage_start
        );
        if (dictionary_finish_result != MYLITE_OK) {
            set_error(
                *stmt->db,
                dictionary_finish_result,
                "ownerless dictionary change could not finish"
            );
            clear_statement_ownerless_page_visibility(*stmt);
            return dictionary_finish_result;
        }
        if (dictionary_ddl_started) {
            mark_ownerless_native_file_op_checkpoint_after_dictionary_ddl(*stmt->db, policy_tokens);
        }
        if (!statement_started_in_explicit_transaction ||
            sql_ends_explicit_transaction(policy_tokens) ||
            !stmt->db->ownerless_transaction_has_local_write) {
            const int page_write_release_result = release_ownerless_page_write_trx_ids(*stmt->db);
            if (page_write_release_result != MYLITE_OK) {
                set_error(
                    *stmt->db,
                    page_write_release_result,
                    "ownerless page-write locks could not release"
                );
                clear_statement_ownerless_page_visibility(*stmt);
                return page_write_release_result;
            }
        }
        if (dictionary_ddl_started) {
            const int dictionary_flush_result = flush_ownerless_dictionary_cache(*stmt->db);
            if (dictionary_flush_result != MYLITE_OK) {
                clear_statement_ownerless_page_visibility(*stmt);
                return dictionary_flush_result;
            }
            clear_ownerless_insert_foreign_key_cache(*stmt->db);
            if (ownerless_runtime_has_external_page_version_pin(g_runtime)) {
                stmt->db->ownerless_peer_dictionary_refresh_requires_conservative_write = true;
            }
        }
        advance_ownerless_handle_read_lsn_after_autocommit_write(
            *stmt->db,
            policy_tokens,
            statement_started_in_explicit_transaction
        );
        if (!stmt->ownerless_native_prepare_per_step && !stmt->has_result &&
            mysql_stmt_field_count(stmt->stmt) != 0U) {
            const int result_setup = initialize_statement_results(*stmt, false);
            if (result_setup != MYLITE_OK) {
                clear_statement_ownerless_page_visibility(*stmt);
                return result_setup;
            }
        }
        if (!stmt->has_result) {
            ownerless_stage_start =
                ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
            const my_ulonglong affected_rows = stmt->ownerless_native_prepare_per_step
                                                   ? ownerless_prepared_affected_rows
                                                   : mysql_stmt_affected_rows(stmt->stmt);
            stmt->db->changes = affected_rows == static_cast<my_ulonglong>(-1)
                                    ? 0
                                    : static_cast<long long>(std::min<my_ulonglong>(
                                          affected_rows,
                                          static_cast<my_ulonglong>(LLONG_MAX)
                                      ));
            ownerless_database_perf_add_elapsed(
                OWNERLESS_DATABASE_PERF_PREPARED_STEP_AFFECTED_ROWS_NS,
                ownerless_stage_start
            );
            statement_locks.release();
            ownerless_stage_start =
                ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
            maybe_reclaim_ownerless_page_log_after_statement(*stmt->db, policy_tokens);
            ownerless_database_perf_add_elapsed(
                OWNERLESS_DATABASE_PERF_PREPARED_STEP_RECLAIM_NS,
                ownerless_stage_start
            );
            clear_statement_ownerless_page_visibility(*stmt);
            return MYLITE_DONE;
        }
        stmt->ownerless_runtime_statement_active = runtime_statement.dismiss();
    }

    return stmt->has_result ? fetch_statement_row(*stmt) : MYLITE_DONE;
#endif
}

int mylite_reset(mylite_stmt *stmt) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    set_ok(*stmt->db);
    const bool ownerless_reset_perf = stmt->db->ownerless_rw_open;
    std::uint64_t ownerless_reset_start = 0U;
    std::uint64_t ownerless_reset_mysql_start = 0U;
    if (ownerless_reset_perf) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PREPARED_RESET_CALLS, 1U);
        ownerless_reset_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    }
    const bool can_skip_mariadb_reset = stmt->executed && !stmt->has_result && !stmt->has_row;
    if (can_skip_mariadb_reset) {
        clear_statement_ownerless_page_visibility(*stmt);
        stmt->executed = false;
        stmt->has_result = false;
        stmt->has_row = false;
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_RESET_TOTAL_NS,
            ownerless_reset_start
        );
        return MYLITE_OK;
    }
    release_statement_results(*stmt);
    clear_statement_ownerless_page_visibility(*stmt);
    if (stmt->ownerless_native_prepare_per_step && stmt->stmt == nullptr && !stmt->executed &&
        !stmt->has_result && !stmt->has_row) {
        stmt->executed = false;
        stmt->has_result = false;
        stmt->has_row = false;
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_RESET_TOTAL_NS,
            ownerless_reset_start
        );
        return MYLITE_OK;
    }
    if (ownerless_reset_perf) {
        ownerless_reset_mysql_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    }
    if (mysql_stmt_reset(stmt->stmt) != 0) {
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_RESET_MYSQL_NS,
            ownerless_reset_mysql_start
        );
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_RESET_TOTAL_NS,
            ownerless_reset_start
        );
        set_mariadb_statement_error(*stmt);
        return MYLITE_ERROR;
    }
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PREPARED_RESET_MYSQL_NS,
        ownerless_reset_mysql_start
    );
    stmt->executed = false;
    stmt->has_result = false;
    stmt->has_row = false;
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PREPARED_RESET_TOTAL_NS,
        ownerless_reset_start
    );
    return MYLITE_OK;
#endif
}

int mylite_finalize(mylite_stmt *stmt) {
    if (stmt == nullptr) {
        return MYLITE_OK;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    release_statement_results(*stmt);
    clear_statement_ownerless_page_visibility(*stmt);
    if (stmt->stmt != nullptr) {
        static_cast<void>(mysql_stmt_close(stmt->stmt));
    }
#endif
    if (stmt->db != nullptr && stmt->db->active_statement_count > 0U) {
        --stmt->db->active_statement_count;
    }
    delete stmt;
    return MYLITE_OK;
}

unsigned mylite_bind_parameter_count(mylite_stmt *stmt) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    return 0U;
#else
    return stmt != nullptr ? static_cast<unsigned>(stmt->parameters.size()) : 0U;
#endif
}

int mylite_clear_bindings(mylite_stmt *stmt) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    if (stmt->executed) {
        return MYLITE_MISUSE;
    }
    for (unsigned index = 1; index <= stmt->parameters.size(); ++index) {
        const int result = bind_null_value(*stmt, index);
        if (result != MYLITE_OK) {
            return result;
        }
    }
    return MYLITE_OK;
#endif
}

int mylite_bind_null(mylite_stmt *stmt, unsigned index) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    return bind_null_value(*stmt, index);
#endif
}

// Public binding APIs use SQLite-style index, value order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
int mylite_bind_int64(mylite_stmt *stmt, unsigned index, long long value) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    ParameterBinding *parameter = parameter_at(*stmt, index);
    if (parameter == nullptr || stmt->executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->int64_value = value;
    parameter->length = sizeof(parameter->int64_value);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_LONGLONG;
    parameter->bind.buffer = &parameter->int64_value;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    return MYLITE_OK;
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): public binding API uses SQLite-style index,
// value order.
int mylite_bind_uint64(mylite_stmt *stmt, unsigned index, unsigned long long value) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    ParameterBinding *parameter = parameter_at(*stmt, index);
    if (parameter == nullptr || stmt->executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->uint64_value = value;
    parameter->length = sizeof(parameter->uint64_value);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_LONGLONG;
    parameter->bind.buffer = &parameter->uint64_value;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    parameter->bind.is_unsigned = 1;
    return MYLITE_OK;
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): public binding API uses SQLite-style index,
// value order.
int mylite_bind_double(mylite_stmt *stmt, unsigned index, double value) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    ParameterBinding *parameter = parameter_at(*stmt, index);
    if (parameter == nullptr || stmt->executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->double_value = value;
    parameter->length = sizeof(parameter->double_value);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_DOUBLE;
    parameter->bind.buffer = &parameter->double_value;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    return MYLITE_OK;
#endif
}

// NOLINTEND(bugprone-easily-swappable-parameters)

int mylite_bind_text(
    mylite_stmt *stmt,
    unsigned index,
    const char *value,
    std::size_t value_len,
    mylite_destructor destructor
) {
    if (stmt == nullptr || stmt->db == nullptr || (value == nullptr && value_len != 0U)) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value_len;
    (void)destructor;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    const std::size_t resolved_len =
        value_len == MYLITE_NUL_TERMINATED ? std::strlen(value) : value_len;
    return bind_bytes(*stmt, index, value, resolved_len, MYSQL_TYPE_STRING, destructor);
#endif
}

int mylite_bind_blob(
    mylite_stmt *stmt,
    unsigned index,
    const void *value,
    std::size_t value_len,
    mylite_destructor destructor
) {
    if (stmt == nullptr || stmt->db == nullptr || (value == nullptr && value_len != 0U) ||
        value_len == MYLITE_NUL_TERMINATED) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value_len;
    (void)destructor;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    return bind_bytes(*stmt, index, value, value_len, MYSQL_TYPE_BLOB, destructor);
#endif
}

unsigned mylite_column_count(mylite_stmt *stmt) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    return 0U;
#else
    if (stmt == nullptr || stmt->stmt == nullptr) {
        return 0U;
    }
    if (!stmt->columns.empty()) {
        return static_cast<unsigned>(stmt->columns.size());
    }
    return mysql_stmt_field_count(stmt->stmt);
#endif
}

const char *mylite_column_name(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? result_column->name.c_str() : nullptr;
#endif
}

const char *mylite_column_org_name(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? result_column->org_name.c_str() : nullptr;
#endif
}

const char *mylite_column_table(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? result_column->table.c_str() : nullptr;
#endif
}

const char *mylite_column_org_table(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? result_column->org_table.c_str() : nullptr;
#endif
}

mylite_value_type mylite_column_type(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return MYLITE_TYPE_NULL;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? column_type(*result_column) : MYLITE_TYPE_NULL;
#endif
}

long long mylite_column_int64(mylite_stmt *stmt, unsigned column) {
    const char *text = mylite_column_text(stmt, column);
    return text != nullptr ? std::strtoll(text, nullptr, k_decimal_base) : 0;
}

unsigned long long mylite_column_uint64(mylite_stmt *stmt, unsigned column) {
    const char *text = mylite_column_text(stmt, column);
    return text != nullptr ? std::strtoull(text, nullptr, k_decimal_base) : 0U;
}

double mylite_column_double(mylite_stmt *stmt, unsigned column) {
    const char *text = mylite_column_text(stmt, column);
    return text != nullptr ? std::strtod(text, nullptr) : 0.0;
}

const char *mylite_column_text(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = value_column_at(stmt, column);
    if (result_column == nullptr || result_column->is_null != 0) {
        return nullptr;
    }
    return reinterpret_cast<const char *>(result_column->buffer.data());
#endif
}

const void *mylite_column_blob(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = value_column_at(stmt, column);
    if (result_column == nullptr || result_column->is_null != 0) {
        return nullptr;
    }
    return result_column->buffer.data();
#endif
}

std::size_t mylite_column_bytes(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return 0U;
#else
    const ResultColumn *result_column = value_column_at(stmt, column);
    if (result_column == nullptr || result_column->is_null != 0) {
        return 0U;
    }
    return result_column->length;
#endif
}

int mylite_errcode(mylite_db *db) {
    return db != nullptr ? db->errcode : MYLITE_MISUSE;
}

int mylite_extended_errcode(mylite_db *db) {
    return db != nullptr ? db->extended_errcode : MYLITE_MISUSE;
}

unsigned mylite_mariadb_errno(mylite_db *db) {
    return db != nullptr ? db->mariadb_errno : 0;
}

const char *mylite_sqlstate(mylite_db *db) {
    return db != nullptr ? safe_c_str(db->sqlstate) : k_sqlstate_general;
}

const char *mylite_errmsg(mylite_db *db) {
    return db != nullptr ? safe_c_str(db->errmsg) : k_bad_db_handle;
}

unsigned mylite_warning_count(mylite_db *db) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)db;
    return 0U;
#else
    return db != nullptr ? mysql_warning_count(&db->mysql) : 0U;
#endif
}

// NOLINTBEGIN(readability-non-const-parameter): output parameters are part of the public C API.
int mylite_warning(
    mylite_db *db,
    unsigned index,
    mylite_warning_level *level,
    unsigned *code,
    const char **message
) {
    if (db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)level;
    (void)code;
    (void)message;
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    const unsigned warning_count = mysql_warning_count(&db->mysql);
    if (index >= warning_count) {
        return MYLITE_NOTFOUND;
    }

    const std::string sql = "SHOW WARNINGS LIMIT " + std::to_string(index) + ", 1";
    if (mysql_query(&db->mysql, sql.c_str()) != 0) {
        set_mariadb_error(*db);
        return MYLITE_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(&db->mysql);
    if (result == nullptr) {
        set_mariadb_error(*db);
        return MYLITE_ERROR;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == nullptr) {
        mysql_free_result(result);
        return MYLITE_NOTFOUND;
    }

    if (level != nullptr) {
        *level = static_cast<mylite_warning_level>(parse_warning_level(row[0]));
    }
    if (code != nullptr) {
        *code = static_cast<unsigned>(
            std::strtoul(row[1] != nullptr ? row[1] : "0", nullptr, k_decimal_base)
        );
    }
    db->warning_message = row[2] != nullptr ? row[2] : "";
    if (message != nullptr) {
        *message = db->warning_message.c_str();
    }

    mysql_free_result(result);
    set_ok(*db);
    return MYLITE_OK;
#endif
}

// NOLINTEND(readability-non-const-parameter)

long long mylite_changes(mylite_db *db) {
    return db != nullptr ? db->changes : 0;
}

unsigned long long mylite_last_insert_id(mylite_db *db) {
    return db != nullptr ? db->last_insert_id : 0;
}

void mylite_free(void *ptr) {
    std::free(ptr);
}

namespace {

int open_impl(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    EmbeddedOpenPerfScope open_scope(EMBEDDED_OPEN_PERF_OPEN_TOTAL_NS);
    embedded_open_perf_add(EMBEDDED_OPEN_PERF_OPEN_CALLS, 1U);
    std::uint64_t stage_start_ns = embedded_open_perf_start_ns();
#endif
    const int validation_result = validate_open_args(path, out_db, flags, config);
#if MYLITE_WITH_MARIADB_EMBEDDED
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_OPEN_VALIDATE_NS, stage_start_ns);
#endif
    if (validation_result != MYLITE_OK) {
        return validation_result;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)config;
    return MYLITE_ERROR;
#else
    try {
        stage_start_ns = embedded_open_perf_start_ns();
        std::unique_ptr<mylite_db> db(new mylite_db());
        db->database_path = normalize_database_path(path).string();
        db->ownerless_page_observation_token =
            g_ownerless_next_page_observation_token.fetch_add(1, std::memory_order_relaxed);
        if (db->ownerless_page_observation_token == 0U) {
            db->ownerless_page_observation_token =
                g_ownerless_next_page_observation_token.fetch_add(1, std::memory_order_relaxed);
        }
        db->ownerless_rw_open =
            (flags & (MYLITE_OPEN_OWNERLESS_RW | MYLITE_OPEN_SHARED_READONLY)) != 0U;
        db->readonly_open = (flags & MYLITE_OPEN_READONLY) != 0U;
        if (has_config_field(
                config,
                offsetof(mylite_open_config, ownerless_page_log_limit_bytes) +
                    sizeof(config->ownerless_page_log_limit_bytes)
            )) {
            db->ownerless_page_log_limit_bytes =
                static_cast<std::uint64_t>(config->ownerless_page_log_limit_bytes);
        }
        embedded_open_perf_add_elapsed(
            EMBEDDED_OPEN_PERF_OPEN_ALLOCATE_NORMALIZE_NS,
            stage_start_ns
        );

        stage_start_ns = embedded_open_perf_start_ns();
        const int runtime_path_result = validate_runtime_database_path(*db);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_OPEN_RUNTIME_PATH_NS, stage_start_ns);
        if (runtime_path_result != MYLITE_OK) {
            return runtime_path_result;
        }

        stage_start_ns = embedded_open_perf_start_ns();
        const int directory_result = prepare_database_directory(db->database_path, flags);
        embedded_open_perf_add_elapsed(
            EMBEDDED_OPEN_PERF_OPEN_PREPARE_DIRECTORY_NS,
            stage_start_ns
        );
        if (directory_result != MYLITE_OK) {
            return directory_result;
        }

        stage_start_ns = embedded_open_perf_start_ns();
        const int ownerless_platform_result = validate_ownerless_platform_for_database(*db);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_OPEN_PLATFORM_PROBE_NS, stage_start_ns);
        if (ownerless_platform_result != MYLITE_OK) {
            return ownerless_platform_result;
        }

        ScopedConcurrencyLock ownerless_startup_lock;
        const bool ownerless_runtime_open =
            (flags & (MYLITE_OPEN_OWNERLESS_RW | MYLITE_OPEN_SHARED_READONLY)) != 0U;
        if (ownerless_runtime_open && !is_memory_database_path(db->database_path)) {
            const std::filesystem::path concurrency_directory =
                std::filesystem::path(db->database_path) / k_concurrency_dir_name;
            std::error_code error;
            std::filesystem::create_directories(concurrency_directory, error);
            if (error) {
                return MYLITE_IOERR;
            }
            const std::filesystem::path lock_path =
                concurrency_directory / k_concurrency_startup_lock_filename;
            stage_start_ns = embedded_open_perf_start_ns();
            if (!ownerless_startup_lock.acquire(
                    lock_path,
                    k_ownerless_runtime_startup_lock_start,
                    k_ownerless_runtime_startup_lock_length,
                    k_system_tables_lock_wait_timeout_ms
                )) {
                embedded_open_perf_add_elapsed(
                    EMBEDDED_OPEN_PERF_OPEN_STARTUP_LOCK_NS,
                    stage_start_ns
                );
                set_error(*db, MYLITE_BUSY, "database ownerless runtime startup is busy");
                return MYLITE_BUSY;
            }
            embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_OPEN_STARTUP_LOCK_NS, stage_start_ns);
        }

        int runtime_result = MYLITE_OK;
        const bool native_redo_repair_open =
            (flags & MYLITE_OPEN_READWRITE) != 0U && !is_memory_database_path(db->database_path);
        const unsigned runtime_startup_attempts = ownerless_runtime_open || native_redo_repair_open
                                                      ? k_ownerless_runtime_startup_attempts
                                                      : 1U;
        for (unsigned attempt = 0U; attempt < runtime_startup_attempts; ++attempt) {
            stage_start_ns = embedded_open_perf_start_ns();
            runtime_result = start_runtime(*db, flags, config);
            embedded_open_perf_add_elapsed(
                EMBEDDED_OPEN_PERF_OPEN_START_RUNTIME_NS,
                stage_start_ns
            );
            if (runtime_result == MYLITE_OK) {
                break;
            }
            if (runtime_result != MYLITE_ERROR || attempt + 1U == runtime_startup_attempts) {
                return runtime_result;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(k_ownerless_runtime_startup_retry_delay_ms)
            );
        }
        if (runtime_result != MYLITE_OK) {
            return runtime_result;
        }

        stage_start_ns = embedded_open_perf_start_ns();
        const int connect_result = connect_runtime(*db);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_OPEN_CONNECT_RUNTIME_NS, stage_start_ns);
        if (connect_result != MYLITE_OK) {
            close_connection(*db);
            release_runtime();
            return connect_result;
        }

        stage_start_ns = embedded_open_perf_start_ns();
        const int system_tables_result = ensure_core_system_tables(*db);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_OPEN_SYSTEM_TABLES_NS, stage_start_ns);
        if (system_tables_result != MYLITE_OK) {
            close_connection(*db);
            release_runtime();
            return system_tables_result;
        }
        stage_start_ns = embedded_open_perf_start_ns();
        initialize_ownerless_dictionary_generation(*db);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_OPEN_DICTIONARY_NS, stage_start_ns);
        if (db->ownerless_native_startup_refresh_lsn != 0U) {
            const int flush_result = flush_ownerless_dictionary_cache(*db);
            if (flush_result != MYLITE_OK) {
                close_connection(*db);
                release_runtime();
                return flush_result;
            }
        }
        reset_ownerless_application_read_refresh_state(*db);
        if (db->ownerless_native_startup_refresh_lsn != 0U) {
            mylite_ownerless_innodb_enable_external_page_visibility(
                db->ownerless_native_startup_refresh_lsn
            );
            mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_no_skip(
                db->ownerless_native_startup_refresh_lsn
            );
            mylite_ownerless_innodb_evict_dictionary_cache();
            db->ownerless_pending_post_open_clean_page_refresh_lsn =
                db->ownerless_native_startup_refresh_lsn;
            db->ownerless_pending_post_open_clean_page_refresh_visible_boundary = true;
            db->ownerless_local_native_read_lsn = std::max(
                db->ownerless_local_native_read_lsn,
                db->ownerless_native_startup_refresh_lsn
            );
            mylite_ownerless_innodb_evict_clean_external_pages();
        }

        *out_db = db.release();
        return MYLITE_OK;
    } catch (const std::bad_alloc &) {
        return MYLITE_NOMEM;
    } catch (const std::filesystem::filesystem_error &) {
        return MYLITE_IOERR;
    }
#endif
}

int validate_open_args(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    if (out_db == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_db = nullptr;

    if (path == nullptr || path[0] == '\0') {
        return MYLITE_MISUSE;
    }

    if ((flags & ~k_known_open_flags) != 0U) {
        return MYLITE_MISUSE;
    }

    const bool readonly = (flags & MYLITE_OPEN_READONLY) != 0U;
    const bool readwrite = (flags & MYLITE_OPEN_READWRITE) != 0U;
    if (readonly == readwrite) {
        return MYLITE_MISUSE;
    }

    const bool shared_readonly = (flags & MYLITE_OPEN_SHARED_READONLY) != 0U;
    const bool ownerless_rw = (flags & MYLITE_OPEN_OWNERLESS_RW) != 0U;

    if (readonly && ((flags & (MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE)) != 0U)) {
        return MYLITE_MISUSE;
    }

    if (readonly && (!shared_readonly || ownerless_rw)) {
        return MYLITE_MISUSE;
    }

    if (readonly && std::strcmp(path, k_memory_database_path) == 0) {
        return MYLITE_MISUSE;
    }

    if (shared_readonly && !readonly) {
        return MYLITE_MISUSE;
    }

    if ((flags & MYLITE_OPEN_URI) != 0U) {
        return MYLITE_MISUSE;
    }

    if ((flags & MYLITE_OPEN_SHARED_READONLY) != 0U && !shared_readonly_open_available()) {
        return MYLITE_MISUSE;
    }

    if ((flags & MYLITE_OPEN_OWNERLESS_RW) != 0U && !ownerless_rw_open_available()) {
        return MYLITE_MISUSE;
    }

    if (config != nullptr && config->size > 0U) {
        if (has_config_field(
                config,
                offsetof(mylite_open_config, profile) + sizeof(config->profile)
            ) &&
            config->profile != MYLITE_PROFILE_DEFAULT && config->profile != MYLITE_PROFILE_STRICT &&
            config->profile != MYLITE_PROFILE_COMPAT) {
            return MYLITE_MISUSE;
        }

        if (has_config_field(
                config,
                offsetof(mylite_open_config, durability) + sizeof(config->durability)
            ) &&
            config->durability != MYLITE_DURABILITY_OFF &&
            config->durability != MYLITE_DURABILITY_NORMAL &&
            config->durability != MYLITE_DURABILITY_FULL) {
            return MYLITE_MISUSE;
        }
    }

    return MYLITE_OK;
}

bool shared_readonly_open_available(void) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    return true;
#else
    return false;
#endif
}

bool ownerless_rw_open_available(void) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    return true;
#else
    return false;
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
int validate_ownerless_platform_for_database(mylite_db &db) {
    if (!db.ownerless_rw_open || is_memory_database_path(db.database_path)) {
        return MYLITE_OK;
    }

    const std::filesystem::path database_path(db.database_path);
    const std::string database_path_name = database_path.string();
    struct stat database_stat = {};
    if (::stat(database_path_name.c_str(), &database_stat) != 0) {
        set_error(db, MYLITE_IOERR, "database directory could not be inspected");
        return MYLITE_IOERR;
    }
    const std::uint64_t database_device = static_cast<std::uint64_t>(database_stat.st_dev);

    const std::filesystem::path concurrency_directory = database_path / k_concurrency_dir_name;
    std::error_code error;
    std::filesystem::create_directories(concurrency_directory, error);
    if (error) {
        set_error(
            db,
            MYLITE_IOERR,
            "database ownerless concurrency directory could not be created"
        );
        return MYLITE_IOERR;
    }

    const std::filesystem::path probe_metadata_path =
        concurrency_directory / k_ownerless_platform_probe_meta_filename;
    if (ownerless_platform_probe_proof_matches(probe_metadata_path, database_device)) {
        return MYLITE_OK;
    }

    mylite_ownerless_probe_result probe = {};
    const int probe_result = mylite_ownerless_probe_directory(database_path_name.c_str(), &probe);
    if (probe_result != MYLITE_OWNERLESS_PROBE_OK || probe.required_primitives == 0U) {
        set_error(
            db,
            MYLITE_ERROR,
            "ownerless mode requires database-directory MAP_SHARED and byte-range lock support"
        );
        return MYLITE_ERROR;
    }

    const int write_result =
        write_ownerless_platform_probe_proof(probe_metadata_path, database_device);
    if (write_result != MYLITE_OK) {
        set_error(
            db,
            write_result,
            "database ownerless platform probe metadata could not be saved"
        );
        return write_result;
    }

    return MYLITE_OK;
}

bool ownerless_platform_probe_proof_matches(
    const std::filesystem::path &metadata_path,
    std::uint64_t database_device
) {
    std::ifstream metadata(metadata_path, std::ios::binary);
    if (!metadata) {
        return false;
    }

    bool has_format = false;
    bool has_matching_device = false;
    bool has_required_primitives = false;
    for (std::string line; std::getline(metadata, line);) {
        if (line == k_metadata_format_line) {
            has_format = true;
            continue;
        }
        if (line == "required_primitives=1") {
            has_required_primitives = true;
            continue;
        }
        if (line.rfind("database_device=", 0) == 0) {
            const std::string value = line.substr(16U);
            if (is_unsigned_decimal(value)) {
                const unsigned long long device = std::strtoull(value.c_str(), nullptr, 10);
                has_matching_device = device == database_device;
            }
        }
    }
    if (!metadata.eof()) {
        return false;
    }

    return has_format && has_matching_device && has_required_primitives;
}

int write_ownerless_platform_probe_proof(
    const std::filesystem::path &metadata_path,
    std::uint64_t database_device
) {
    std::ofstream metadata(metadata_path, std::ios::binary | std::ios::trunc);
    if (!metadata) {
        return MYLITE_IOERR;
    }

    metadata << k_metadata_format_line << "\n";
    metadata << "database_device=" << database_device << "\n";
    metadata << "required_primitives=1\n";
    return metadata ? MYLITE_OK : MYLITE_IOERR;
}
#endif

int exec_result_impl(
    mylite_db *db,
    const char *sql,
    mylite_exec_result_metadata_callback metadata_callback,
    mylite_exec_result_callback row_callback,
    void *ctx,
    char **errmsg
) {
    if (errmsg != nullptr) {
        *errmsg = nullptr;
    }

    if (db == nullptr || sql == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)metadata_callback;
    (void)row_callback;
    (void)ctx;
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return copy_error_message(*db, errmsg);
#else
    exec_result_perf_add(EXEC_RESULT_PERF_CALLS, 1U);
    set_ok(*db);
    if (reject_unsupported_sql_policy(*db, sql) != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    if (!db->ownerless_rw_open) {
        refresh_ownerless_pending_post_open_clean_pages(*db);
        OwnerlessStatementPlainReadScope native_startup_plain_read(
            db->ownerless_native_startup_refresh_lsn != 0U
        );
        const NativeControlStatement native_control_statement =
            classify_native_control_statement(sql);
        std::uint64_t stage_start_ns = exec_result_perf_start_ns();
        if (native_control_statement != NativeControlStatement::None) {
            exec_result_perf_add(EXEC_RESULT_PERF_NATIVE_CONTROL_CALLS, 1U);
            if (native_control_statement == NativeControlStatement::StartTransaction) {
                exec_result_perf_add(EXEC_RESULT_PERF_NATIVE_CONTROL_START_TRANSACTION_CALLS, 1U);
            } else if (native_control_statement == NativeControlStatement::Commit) {
                exec_result_perf_add(EXEC_RESULT_PERF_NATIVE_CONTROL_COMMIT_CALLS, 1U);
            } else if (native_control_statement == NativeControlStatement::Rollback) {
                exec_result_perf_add(EXEC_RESULT_PERF_NATIVE_CONTROL_ROLLBACK_CALLS, 1U);
            }
            if (native_control_autocommit_is_noop(*db, native_control_statement)) {
                exec_result_perf_add(EXEC_RESULT_PERF_NATIVE_CONTROL_AUTOCOMMIT_NOOPS, 1U);
            } else {
                const int native_control_result =
                    execute_native_control_statement(*db, native_control_statement);
                if (native_control_result != MYLITE_OK) {
                    exec_result_perf_add_elapsed(
                        EXEC_RESULT_PERF_NATIVE_CONTROL_NS,
                        stage_start_ns
                    );
                    exec_result_perf_add(EXEC_RESULT_PERF_NATIVE_CONTROL_ERRORS, 1U);
                    set_mariadb_error(*db);
                    return copy_error_message(*db, errmsg);
                }
            }
            exec_result_perf_add_elapsed(EXEC_RESULT_PERF_NATIVE_CONTROL_NS, stage_start_ns);
            exec_result_perf_add(EXEC_RESULT_PERF_NO_RESULT_SETS, 1U);
            stage_start_ns = exec_result_perf_start_ns();
            db->changes = 0;
            db->last_insert_id = static_cast<unsigned long long>(mysql_insert_id(&db->mysql));
            exec_result_perf_add_elapsed(EXEC_RESULT_PERF_STATUS_UPDATE_NS, stage_start_ns);
            return MYLITE_OK;
        }

        stage_start_ns = exec_result_perf_start_ns();
        if (mysql_query(&db->mysql, sql) != 0) {
            exec_result_perf_add_elapsed(EXEC_RESULT_PERF_MYSQL_QUERY_NS, stage_start_ns);
            exec_result_perf_add(EXEC_RESULT_PERF_MYSQL_QUERY_ERRORS, 1U);
            set_mariadb_error(*db);
            return copy_error_message(*db, errmsg);
        }
        exec_result_perf_add_elapsed(EXEC_RESULT_PERF_MYSQL_QUERY_NS, stage_start_ns);
        stage_start_ns = exec_result_perf_start_ns();
        const my_ulonglong affected_rows = mysql_affected_rows(&db->mysql);
        const unsigned long long insert_id =
            static_cast<unsigned long long>(mysql_insert_id(&db->mysql));
        exec_result_perf_add_elapsed(EXEC_RESULT_PERF_AFFECTED_ROWS_NS, stage_start_ns);

        bool has_result = false;
        stage_start_ns = exec_result_perf_start_ns();
        const int result =
            store_and_emit_result(*db, metadata_callback, row_callback, ctx, &has_result);
        exec_result_perf_add_elapsed(EXEC_RESULT_PERF_STORE_RESULT_NS, stage_start_ns);
        if (result != MYLITE_OK) {
            return copy_error_message(*db, errmsg);
        }
        exec_result_perf_add(
            has_result ? EXEC_RESULT_PERF_RESULT_SETS : EXEC_RESULT_PERF_NO_RESULT_SETS,
            1U
        );
        stage_start_ns = exec_result_perf_start_ns();
        update_current_schema_after_successful_sql(*db, sql);
        exec_result_perf_add_elapsed(EXEC_RESULT_PERF_CURRENT_SCHEMA_NS, stage_start_ns);

        stage_start_ns = exec_result_perf_start_ns();
        db->changes =
            has_result || affected_rows == static_cast<my_ulonglong>(-1)
                ? 0
                : static_cast<long long>(
                      std::min<my_ulonglong>(affected_rows, static_cast<my_ulonglong>(LLONG_MAX))
                  );
        db->last_insert_id = insert_id;
        exec_result_perf_add_elapsed(EXEC_RESULT_PERF_STATUS_UPDATE_NS, stage_start_ns);
        return MYLITE_OK;
    }

    const OwnerlessPageVisibilityScope page_visibility_scope;
    const SqlPolicyTokens policy_tokens = collect_sql_policy_tokens(sql);
    const int pressure_result = enforce_ownerless_page_log_limit_policy(*db, policy_tokens);
    if (pressure_result != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    ScopedOwnerlessRuntimeStatement runtime_statement(*db);
    const bool statement_uses_temporary_table =
        ownerless_statement_uses_temporary_table(*db, policy_tokens);
    OwnerlessStatementLocks statement_locks;
    const int statement_lock_result =
        acquire_ownerless_statement_locks(*db, policy_tokens, statement_locks);
    if (statement_lock_result != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    const bool allow_page_version_reads =
        !statement_uses_temporary_table &&
        !db->ownerless_peer_dictionary_refresh_requires_conservative_write &&
        statement_allows_ownerless_page_version_reads(policy_tokens);
    const bool allow_current_read_refresh =
        !statement_uses_temporary_table &&
        (sql_statement_needs_ownerless_current_read_refresh(policy_tokens) ||
         ownerless_transaction_end_has_local_write(*db, policy_tokens));
    const bool tableless_ownerless_plain_read =
        statement_is_tableless_ownerless_plain_read(policy_tokens);
    bool page_version_reads_enabled = false;
    const int refresh_result = refresh_ownerless_external_pages_before_statement(
        *db,
        allow_page_version_reads,
        !statement_uses_temporary_table && !tableless_ownerless_plain_read &&
            (ownerless_connection_allows_global_refresh(*db, allow_page_version_reads) ||
             allow_current_read_refresh),
        ownerless_dictionary_ddl_statement(policy_tokens),
        &page_version_reads_enabled
    );
    if (refresh_result != MYLITE_OK) {
        if (page_version_reads_enabled) {
            release_ownerless_completed_statement_page_visibility(
                *db,
                !ownerless_connection_is_in_explicit_transaction(*db)
            );
        }
        return copy_error_message(*db, errmsg);
    }
    bool dictionary_ddl_started = false;
    const int dictionary_ddl_result =
        ownerless_begin_dictionary_ddl(*db, policy_tokens, &dictionary_ddl_started);
    if (dictionary_ddl_result != MYLITE_OK) {
        if (page_version_reads_enabled) {
            release_ownerless_completed_statement_page_visibility(
                *db,
                !ownerless_connection_is_in_explicit_transaction(*db)
            );
        }
        return copy_error_message(*db, errmsg);
    }
    OwnerlessStatementDictionaryDdlScope dictionary_ddl_scope(dictionary_ddl_started);
    bool consistent_snapshot_start_pin_registered = false;
    const int consistent_snapshot_pin_result = ensure_ownerless_consistent_snapshot_start_pin(
        *db,
        policy_tokens,
        &consistent_snapshot_start_pin_registered
    );
    if (consistent_snapshot_pin_result != MYLITE_OK) {
        const int dictionary_finish_result =
            ownerless_finish_dictionary_ddl(*db, dictionary_ddl_started);
        if (dictionary_finish_result != MYLITE_OK) {
            set_error(
                *db,
                dictionary_finish_result,
                "ownerless dictionary change could not finish after failed statement"
            );
        }
        if (page_version_reads_enabled) {
            release_ownerless_completed_statement_page_visibility(
                *db,
                !ownerless_connection_is_in_explicit_transaction(*db)
            );
        }
        return copy_error_message(*db, errmsg);
    }
    const bool statement_started_in_explicit_transaction =
        ownerless_connection_is_in_explicit_transaction(*db);
    OwnerlessStatementPageWriteTrackingScope page_write_tracking(*db);
    OwnerlessStatementPlainReadScope plain_read(
        page_version_reads_enabled,
        db->ownerless_local_native_read_lsn != 0U ||
            ownerless_transaction_has_local_write_or_locking_read(*db)
    );
    OwnerlessStatementNativeLifecycleRefreshScope native_lifecycle_refresh(
        dictionary_ddl_started || db->ownerless_peer_dictionary_refresh_requires_conservative_write
    );
    const OwnerlessStatementFastPathPolicy fast_path_policy =
        ownerless_statement_fast_path_policy(*db, sql, policy_tokens);
    update_ownerless_explicit_transaction_visible_fast_proof_before_sql(
        *db,
        policy_tokens,
        fast_path_policy.visible_fast_path
    );
    OwnerlessStatementVisibleFastPathScope visible_fast_path(
        fast_path_policy.visible_fast_path,
        fast_path_policy.append_batch_fast_path,
        fast_path_policy.deferred_page_publish_fast_path,
        ownerless_statement_deferred_latest_checkpoint_coalescing_allowed(
            *db,
            fast_path_policy.append_batch_fast_path,
            statement_started_in_explicit_transaction
        )
    );
    std::uint64_t stage_start_ns = exec_result_perf_start_ns();
    if (mysql_query(&db->mysql, sql) != 0) {
        exec_result_perf_add_elapsed(EXEC_RESULT_PERF_MYSQL_QUERY_NS, stage_start_ns);
        exec_result_perf_add(EXEC_RESULT_PERF_MYSQL_QUERY_ERRORS, 1U);
        set_mariadb_error(*db);
        disqualify_ownerless_explicit_transaction_visible_fast_proof_after_failed_sql(
            *db,
            policy_tokens,
            statement_started_in_explicit_transaction
        );
        if (ownerless_stale_engine_error_allows_retry(
                *db,
                policy_tokens,
                statement_started_in_explicit_transaction
            ) &&
            refresh_ownerless_dictionary_cache_after_stale_engine_error(*db) == MYLITE_OK) {
            set_ok(*db);
            stage_start_ns = exec_result_perf_start_ns();
            if (mysql_query(&db->mysql, sql) == 0) {
                exec_result_perf_add_elapsed(EXEC_RESULT_PERF_MYSQL_QUERY_NS, stage_start_ns);
                goto ownerless_query_success;
            }
            exec_result_perf_add_elapsed(EXEC_RESULT_PERF_MYSQL_QUERY_NS, stage_start_ns);
            exec_result_perf_add(EXEC_RESULT_PERF_MYSQL_QUERY_ERRORS, 1U);
            set_mariadb_error(*db);
        }
        const int dictionary_finish_result =
            ownerless_finish_dictionary_ddl(*db, dictionary_ddl_started);
        if (dictionary_finish_result != MYLITE_OK) {
            set_error(
                *db,
                dictionary_finish_result,
                "ownerless dictionary change could not finish after failed statement"
            );
        }
        if (consistent_snapshot_start_pin_registered) {
            release_ownerless_transaction_page_version_pin(*db);
        }
        if (page_version_reads_enabled) {
            release_ownerless_completed_statement_page_visibility(
                *db,
                !statement_started_in_explicit_transaction
            );
        }
        rollback_failed_ownerless_implicit_statement(
            *db,
            statement_started_in_explicit_transaction
        );
        rollback_active_transaction_after_deadlock(*db);
        return copy_error_message(*db, errmsg);
    }
    exec_result_perf_add_elapsed(EXEC_RESULT_PERF_MYSQL_QUERY_NS, stage_start_ns);
ownerless_query_success:
    refresh_ownerless_pending_post_open_clean_pages(*db);
    stage_start_ns = exec_result_perf_start_ns();
    const my_ulonglong affected_rows = mysql_affected_rows(&db->mysql);
    const unsigned long long insert_id =
        static_cast<unsigned long long>(mysql_insert_id(&db->mysql));
    exec_result_perf_add_elapsed(EXEC_RESULT_PERF_AFFECTED_ROWS_NS, stage_start_ns);

    bool has_result = false;
    stage_start_ns = exec_result_perf_start_ns();
    const int result =
        store_and_emit_result(*db, metadata_callback, row_callback, ctx, &has_result);
    exec_result_perf_add_elapsed(EXEC_RESULT_PERF_STORE_RESULT_NS, stage_start_ns);
    if (result != MYLITE_OK) {
        disqualify_ownerless_explicit_transaction_visible_fast_proof_after_failed_sql(
            *db,
            policy_tokens,
            statement_started_in_explicit_transaction
        );
        if (ownerless_finish_dictionary_ddl(*db, dictionary_ddl_started) != MYLITE_OK) {
            set_error(*db, MYLITE_IOERR, "ownerless dictionary change could not finish");
        }
        if (page_version_reads_enabled) {
            release_ownerless_completed_statement_page_visibility(
                *db,
                !statement_started_in_explicit_transaction
            );
        }
        return copy_error_message(*db, errmsg);
    }
    exec_result_perf_add(
        has_result ? EXEC_RESULT_PERF_RESULT_SETS : EXEC_RESULT_PERF_NO_RESULT_SETS,
        1U
    );
    stage_start_ns = exec_result_perf_start_ns();
    update_current_schema_after_successful_sql(*db, policy_tokens);
    exec_result_perf_add_elapsed(EXEC_RESULT_PERF_CURRENT_SCHEMA_NS, stage_start_ns);
    stage_start_ns = exec_result_perf_start_ns();
    update_ownerless_statement_lock_timeout_after_successful_sql(*db, policy_tokens);
    update_ownerless_temporary_table_state_after_successful_sql(*db, policy_tokens);
    const int transaction_state_result =
        update_ownerless_transaction_state_after_successful_sql(*db, policy_tokens);
    exec_result_perf_add_elapsed(EXEC_RESULT_PERF_STATUS_UPDATE_NS, stage_start_ns);
    if (transaction_state_result != MYLITE_OK) {
        if (page_version_reads_enabled) {
            release_ownerless_completed_statement_page_visibility(
                *db,
                !statement_started_in_explicit_transaction
            );
        }
        return copy_error_message(*db, errmsg);
    }
    const int dictionary_finish_result =
        ownerless_finish_dictionary_ddl(*db, dictionary_ddl_started);
    if (dictionary_finish_result != MYLITE_OK) {
        set_error(*db, dictionary_finish_result, "ownerless dictionary change could not finish");
        if (page_version_reads_enabled) {
            release_ownerless_completed_statement_page_visibility(
                *db,
                !statement_started_in_explicit_transaction
            );
        }
        return copy_error_message(*db, errmsg);
    }
    if (dictionary_ddl_started) {
        mark_ownerless_native_file_op_checkpoint_after_dictionary_ddl(*db, policy_tokens);
    }
    if (!statement_started_in_explicit_transaction ||
        sql_ends_explicit_transaction(policy_tokens) ||
        !db->ownerless_transaction_has_local_write) {
        const int page_write_release_result = release_ownerless_page_write_trx_ids(*db);
        if (page_write_release_result != MYLITE_OK) {
            set_error(
                *db,
                page_write_release_result,
                "ownerless page-write locks could not release"
            );
            if (page_version_reads_enabled) {
                release_ownerless_completed_statement_page_visibility(
                    *db,
                    !statement_started_in_explicit_transaction
                );
            }
            return copy_error_message(*db, errmsg);
        }
    }
    if (dictionary_ddl_started) {
        const int dictionary_flush_result = flush_ownerless_dictionary_cache(*db);
        if (dictionary_flush_result != MYLITE_OK) {
            if (page_version_reads_enabled) {
                release_ownerless_completed_statement_page_visibility(
                    *db,
                    !statement_started_in_explicit_transaction
                );
            }
            return copy_error_message(*db, errmsg);
        }
        clear_ownerless_insert_foreign_key_cache(*db);
        if (ownerless_runtime_has_external_page_version_pin(g_runtime)) {
            db->ownerless_peer_dictionary_refresh_requires_conservative_write = true;
        }
    }
    stage_start_ns = exec_result_perf_start_ns();
    advance_ownerless_handle_read_lsn_after_autocommit_write(
        *db,
        policy_tokens,
        statement_started_in_explicit_transaction
    );
    db->changes =
        has_result || affected_rows == static_cast<my_ulonglong>(-1)
            ? 0
            : static_cast<long long>(
                  std::min<my_ulonglong>(affected_rows, static_cast<my_ulonglong>(LLONG_MAX))
              );
    db->last_insert_id = insert_id;
    exec_result_perf_add_elapsed(EXEC_RESULT_PERF_STATUS_UPDATE_NS, stage_start_ns);
    if (page_version_reads_enabled) {
        release_ownerless_completed_statement_page_visibility(
            *db,
            !statement_started_in_explicit_transaction,
            false
        );
    }
    statement_locks.release();
    maybe_reclaim_ownerless_page_log_after_statement(*db, policy_tokens);
    return MYLITE_OK;
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void update_current_schema_after_successful_sql(mylite_db &db, std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    update_current_schema_after_successful_sql(db, tokens);
}

void update_current_schema_after_successful_sql(mylite_db &db, const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "USE") || tokens.count < 2U) {
        return;
    }
    db.current_schema = std::string(unquoted_identifier_token(tokens.values[1]));
}

#endif

int prepare_impl(
    mylite_db *db,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
) {
    if (out_stmt == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_stmt = nullptr;
    if (tail != nullptr) {
        *tail = nullptr;
    }
    if (db == nullptr || sql == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)sql_len;
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    const std::size_t resolved_len = sql_len == MYLITE_NUL_TERMINATED ? std::strlen(sql) : sql_len;
    if (resolved_len > ULONG_MAX) {
        return MYLITE_MISUSE;
    }
    set_ok(*db);
    const std::string_view sql_view(sql, resolved_len);
    const int policy_result = reject_unsupported_sql_policy(*db, sql_view);
    if (policy_result != MYLITE_OK) {
        return policy_result;
    }
    if (token_equals(first_identifier_token(sql_view), "CALL")) {
        set_error(*db, MYLITE_ERROR, "prepared CALL statements are not supported by MyLite");
        return MYLITE_ERROR;
    }
    if (db->ownerless_rw_open) {
        const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql_view);
        const bool statement_uses_temporary_table =
            ownerless_statement_uses_temporary_table(*db, tokens);
        const bool allow_current_read_refresh =
            !statement_uses_temporary_table &&
            sql_statement_needs_ownerless_current_read_refresh(tokens);
        const int refresh_result = refresh_ownerless_external_pages_before_statement(
            *db,
            false,
            !statement_uses_temporary_table &&
                (ownerless_connection_allows_global_refresh(*db, false) ||
                 allow_current_read_refresh),
            ownerless_dictionary_ddl_statement(tokens),
            nullptr
        );
        if (refresh_result != MYLITE_OK) {
            return refresh_result;
        }
    }

    std::unique_ptr<mylite_stmt> statement(new mylite_stmt());
    statement->db = db;
    if (db->ownerless_rw_open) {
        statement->ownerless_sql_text = std::make_unique<std::string>(sql, resolved_len);
        statement->ownerless_policy_tokens =
            collect_sql_policy_tokens(*statement->ownerless_sql_text);
        statement->ownerless_policy_tokens_valid = true;
    }
    if (statement->ownerless_policy_tokens_valid && ownerless_prepared_write_defers_native_prepare(
                                                        sql_view,
                                                        statement->ownerless_policy_tokens
                                                    )) {
        std::size_t parameter_count = 0;
        if (!count_sql_parameter_markers(sql_view, &parameter_count)) {
            set_error(*db, MYLITE_ERROR, "prepared statement has too many parameter markers");
            return MYLITE_ERROR;
        }
        statement->ownerless_native_prepare_per_step = true;
        statement->parameters.resize(parameter_count);
        for (unsigned index = 1; index <= statement->parameters.size(); ++index) {
            const int result = bind_null_value(*statement, index);
            if (result != MYLITE_OK) {
                return result;
            }
        }
        if (tail != nullptr) {
            *tail = sql + resolved_len;
        }
        ++db->active_statement_count;
        *out_stmt = statement.release();
        set_ok(*db);
        return MYLITE_OK;
    }

    statement->stmt = mysql_stmt_init(&db->mysql);
    if (statement->stmt == nullptr) {
        set_error(*db, MYLITE_NOMEM, "statement could not be allocated");
        return MYLITE_NOMEM;
    }

    my_bool update_max_length = 1;
    static_cast<void>(
        mysql_stmt_attr_set(statement->stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length)
    );

    if (mysql_stmt_prepare(statement->stmt, sql, static_cast<unsigned long>(resolved_len)) != 0) {
        set_mariadb_statement_error(*statement);
        if (retry_ownerless_prepare_after_stale_engine_error(*statement, sql, resolved_len) !=
            MYLITE_OK) {
            static_cast<void>(mysql_stmt_close(statement->stmt));
            statement->stmt = nullptr;
            return MYLITE_ERROR;
        }
    }

    statement->parameters.resize(mysql_stmt_param_count(statement->stmt));
    for (unsigned index = 1; index <= statement->parameters.size(); ++index) {
        const int result = bind_null_value(*statement, index);
        if (result != MYLITE_OK) {
            static_cast<void>(mysql_stmt_close(statement->stmt));
            statement->stmt = nullptr;
            return result;
        }
    }

    if (tail != nullptr) {
        *tail = sql + resolved_len;
    }
    ++db->active_statement_count;
    *out_stmt = statement.release();
    set_ok(*db);
    return MYLITE_OK;
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
int reject_unsupported_sql_policy(mylite_db &db, std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);

    if (is_readonly_rejected_sql_statement(db, tokens)) {
        set_error(db, MYLITE_READONLY, "database is open read-only");
        return MYLITE_READONLY;
    }

    if (is_unsupported_oracle_sql_mode_statement(tokens)) {
        set_error(db, MYLITE_ERROR, "Oracle SQL mode is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_procedure_analyse_statement(tokens)) {
        set_error(db, MYLITE_ERROR, "PROCEDURE ANALYSE is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_vector_runtime_statement(tokens)) {
        set_error(db, MYLITE_ERROR, "vector SQL runtime is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_xml_sql_function_statement(tokens)) {
        set_error(db, MYLITE_ERROR, "XML SQL functions are not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_dynamic_column_statement(tokens)) {
        set_error(db, MYLITE_ERROR, "dynamic columns are not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_table_directory_option_statement(tokens)) {
        set_error(
            db,
            MYLITE_ERROR,
            "table DATA DIRECTORY and INDEX DIRECTORY options are not supported by MyLite"
        );
        return MYLITE_ERROR;
    }

    if (db.ownerless_rw_open && is_unsupported_ownerless_engine_statement(db, sql)) {
        set_error(
            db,
            MYLITE_ERROR,
            "ownerless read/write mode currently supports InnoDB tables only"
        );
        return MYLITE_ERROR;
    }

    if (is_unsupported_server_surface_sql(tokens, db.current_schema)) {
        set_error(db, MYLITE_ERROR, "server-owned SQL surface is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (db.ownerless_rw_open) {
        if (is_unsupported_ownerless_routine_ddl_statement(db, sql)) {
            set_error(
                db,
                MYLITE_ERROR,
                "ownerless read/write mode does not support stored routine DDL"
            );
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_routine_execution_statement(db, sql)) {
            set_error(
                db,
                MYLITE_ERROR,
                "ownerless read/write mode does not support stored routine execution"
            );
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_sequence_statement(db, sql)) {
            set_error(db, MYLITE_ERROR, "ownerless read/write mode does not support sequence SQL");
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_table_admin_statement(db, sql)) {
            set_error(
                db,
                MYLITE_ERROR,
                "ownerless read/write mode does not support table admin SQL"
            );
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_lock_tables_statement(db, sql)) {
            set_error(db, MYLITE_ERROR, "ownerless read/write mode does not support LOCK TABLES");
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_flush_table_lock_statement(db, sql)) {
            set_error(
                db,
                MYLITE_ERROR,
                "ownerless read/write mode does not support FLUSH TABLES locks or export"
            );
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_isolation_statement(db, sql)) {
            set_error(
                db,
                MYLITE_ERROR,
                "ownerless mode does not support READ UNCOMMITTED or isolation variable assignments"
            );
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_partition_statement(db, sql)) {
            set_error(
                db,
                MYLITE_ERROR,
                "ownerless read/write mode does not support partitioned table DDL"
            );
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_tablespace_management_statement(db, sql)) {
            set_error(
                db,
                MYLITE_ERROR,
                "ownerless read/write mode does not support DISCARD/IMPORT TABLESPACE"
            );
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_table_storage_option_statement(db, sql)) {
            set_error(
                db,
                MYLITE_ERROR,
                "ownerless read/write mode does not support unproven table storage options"
            );
            return MYLITE_ERROR;
        }

        if (is_unsupported_ownerless_special_index_statement(db, sql)) {
            set_error(
                db,
                MYLITE_ERROR,
                "ownerless read/write mode does not support FULLTEXT or SPATIAL index DDL"
            );
            return MYLITE_ERROR;
        }
    }

    return MYLITE_OK;
}

bool is_readonly_rejected_sql_statement(const mylite_db &db, const SqlPolicyTokens &tokens) {
    if (!db.readonly_open) {
        return false;
    }

    return sql_statement_requires_write(tokens) ||
           sql_statement_requests_write_transaction(tokens) ||
           sql_statement_uses_locking_read(tokens);
}

bool sql_statement_requires_write(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (first.empty()) {
        return false;
    }

    if (token_in(first, "ALTER", "ANALYZE", "CALL", "CREATE") ||
        token_in(first, "DELETE", "DO", "DROP", "EXECUTE") ||
        token_in(first, "GRANT", "HANDLER", "IMPORT", "INSERT") ||
        token_in(first, "INSTALL", "LOAD", "LOCK", "OPTIMIZE") ||
        token_in(first, "PREPARE", "RENAME", "REPAIR", "REPLACE") ||
        token_in(first, "REVOKE", "TRUNCATE", "UNINSTALL", "UPDATE")) {
        return true;
    }

    if (token_equals(first, "SET")) {
        for (std::size_t index = 1; index < tokens.count; ++index) {
            if (token_in(identifier_token_at(tokens, index), "GLOBAL", "PERSIST", "PERSIST_ONLY")) {
                return true;
            }
        }
    }

    if (token_in(first, "SELECT", "SHOW", "DESCRIBE", "DESC") ||
        token_in(first, "EXPLAIN", "USE", "SET", "START") ||
        token_in(first, "BEGIN", "COMMIT", "ROLLBACK", "VALUES") || token_equals(first, "TABLE")) {
        return false;
    }

    if (!token_equals(first, "WITH")) {
        return true;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_in(identifier_token_at(tokens, index), "DELETE", "INSERT", "REPLACE", "UPDATE")) {
            return true;
        }
    }
    return false;
}

bool ownerless_prepared_write_defers_native_prepare(
    std::string_view sql,
    const SqlPolicyTokens &tokens
) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "DELETE", "INSERT", "REPLACE") && !token_equals(first, "UPDATE")) {
        return false;
    }
    return !sql_contains_identifier_token(sql, "RETURNING");
}

bool count_sql_parameter_markers(std::string_view sql, std::size_t *out_count) {
    if (out_count == nullptr) {
        return false;
    }

    std::size_t offset = 0;
    std::string_view token;
    std::size_t count = 0;
    while (next_sql_token(sql, offset, token)) {
        if (!token_equals(token, "?")) {
            continue;
        }
        if (count == static_cast<std::size_t>(UINT_MAX)) {
            return false;
        }
        ++count;
    }
    *out_count = count;
    return true;
}

bool sql_contains_identifier_token(std::string_view sql, const char *keyword) {
    std::size_t offset = 0;
    std::string_view token;
    while (next_sql_token(sql, offset, token)) {
        if (identifier_token_equals(token, keyword)) {
            return true;
        }
    }
    return false;
}

bool ownerless_insert_values_statement_row_count(std::string_view sql, std::size_t *out_row_count) {
    if (out_row_count == nullptr) {
        return false;
    }
    *out_row_count = 0U;

    std::size_t offset = 0;
    std::string_view token;
    if (!next_sql_token(sql, offset, token) || !identifier_token_equals(token, "INSERT")) {
        return false;
    }

    int paren_depth = 0;
    bool found_values = false;
    while (next_sql_token(sql, offset, token)) {
        if (token_equals(token, "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(token, ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth == 0 &&
            (identifier_token_equals(token, "VALUE") || identifier_token_equals(token, "VALUES"))) {
            found_values = true;
            break;
        }
    }
    if (!found_values) {
        return false;
    }

    bool saw_row = false;
    bool expect_row = true;
    paren_depth = 0;
    while (next_sql_token(sql, offset, token)) {
        if (expect_row) {
            if (token_equals(token, "(")) {
                saw_row = true;
                expect_row = false;
                paren_depth = 1;
                continue;
            }
            return false;
        }

        if (token_equals(token, "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(token, ")")) {
            if (paren_depth == 0) {
                return false;
            }
            --paren_depth;
            continue;
        }
        if (paren_depth != 0) {
            continue;
        }
        if (token_equals(token, ",")) {
            ++(*out_row_count);
            expect_row = true;
            continue;
        }
        if (token_equals(token, ";")) {
            if (!saw_row || expect_row) {
                return false;
            }
            ++(*out_row_count);
            return true;
        }
        return false;
    }

    if (!saw_row || expect_row || paren_depth != 0) {
        return false;
    }
    ++(*out_row_count);
    return true;
}

constexpr std::size_t k_ownerless_append_batch_fast_path_max_insert_values_rows = 1024U;

bool ownerless_transaction_commit_allows_visible_fast_path(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    return token_equals(identifier_token_at(tokens, 0), "COMMIT") &&
           ownerless_connection_is_in_explicit_transaction(db) &&
           db.ownerless_transaction_has_local_write &&
           db.ownerless_transaction_visible_fast_commit_candidate &&
           !db.ownerless_transaction_visible_fast_commit_disqualified &&
           !db.ownerless_peer_dictionary_refresh_requires_conservative_write;
}

OwnerlessStatementFastPathPolicy ownerless_statement_fast_path_policy(
    mylite_db &db,
    std::string_view sql,
    const SqlPolicyTokens &tokens
) {
    OwnerlessStatementFastPathPolicy policy = {};
    if (db.ownerless_peer_dictionary_refresh_requires_conservative_write) {
        return policy;
    }

    std::size_t row_count = 0U;
    if (ownerless_insert_values_statement_row_count(sql, &row_count) && row_count != 0U) {
        bool single_owner_epoch = false;
        {
            const std::lock_guard<std::mutex> guard(g_runtime.mutex);
            single_owner_epoch = ownerless_runtime_in_single_owner_epoch_locked(g_runtime);
        }
        if (ownerless_insert_statement_has_target_column_list(tokens)) {
            if (!single_owner_epoch && ownerless_insert_target_has_auto_increment(db, tokens)) {
                return policy;
            }
        }
        if (!ownerless_insert_target_has_foreign_keys(db, tokens)) {
            policy.visible_fast_path = true;
            policy.append_batch_fast_path =
                row_count <= k_ownerless_append_batch_fast_path_max_insert_values_rows;
            policy.deferred_page_publish_fast_path =
                policy.append_batch_fast_path && single_owner_epoch;
        }
        return policy;
    }

    policy.visible_fast_path = ownerless_transaction_commit_allows_visible_fast_path(db, tokens);
    policy.append_batch_fast_path = policy.visible_fast_path;
    policy.deferred_page_publish_fast_path = policy.visible_fast_path;
    return policy;
}

bool ownerless_statement_deferred_latest_checkpoint_coalescing_allowed(
    const mylite_db &db,
    bool statement_append_batch_fast_path,
    bool statement_started_in_explicit_transaction
) {
    if (!statement_append_batch_fast_path) {
        return false;
    }
    if (!statement_started_in_explicit_transaction) {
        return true;
    }
    return db.ownerless_transaction_visible_fast_commit_candidate &&
           !db.ownerless_transaction_visible_fast_commit_disqualified &&
           !db.ownerless_peer_dictionary_refresh_requires_conservative_write;
}

bool ownerless_insert_target_table(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::string *out_schema,
    std::string *out_table
) {
    if (out_schema == nullptr || out_table == nullptr ||
        !token_equals(identifier_token_at(tokens, 0), "INSERT")) {
        return false;
    }
    out_schema->clear();
    out_table->clear();

    std::size_t index = 1U;
    while (index < tokens.count &&
           ownerless_table_reference_skip_token(ownerless_raw_identifier_token_at(tokens, index))) {
        ++index;
    }
    if (index >= tokens.count || !ownerless_table_identifier_token(tokens.values[index])) {
        return false;
    }

    if (index + 2U < tokens.count && tokens.values[index + 1U] == "." &&
        ownerless_table_identifier_token(tokens.values[index + 2U])) {
        *out_schema = ownerless_normalized_identifier(tokens.values[index]);
        *out_table = ownerless_normalized_identifier(tokens.values[index + 2U]);
    } else {
        *out_schema = ownerless_normalized_identifier(db.current_schema);
        *out_table = ownerless_normalized_identifier(tokens.values[index]);
    }

    return !out_schema->empty() && !out_table->empty() &&
           !ownerless_tracked_temporary_table_name(db, *out_table);
}

bool ownerless_insert_statement_has_target_column_list(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "INSERT")) {
        return false;
    }

    std::size_t index = 1U;
    while (index < tokens.count &&
           ownerless_table_reference_skip_token(ownerless_raw_identifier_token_at(tokens, index))) {
        ++index;
    }
    if (index >= tokens.count || !ownerless_table_identifier_token(tokens.values[index])) {
        return false;
    }

    if (index + 2U < tokens.count && tokens.values[index + 1U] == "." &&
        ownerless_table_identifier_token(tokens.values[index + 2U])) {
        index += 3U;
    } else {
        ++index;
    }
    return index < tokens.count && token_equals(tokens.values[index], "(");
}

std::string ownerless_escape_metadata_literal(mylite_db &db, std::string_view value) {
    std::string escaped((value.size() * 2U) + 1U, '\0');
    const unsigned long escaped_size = mysql_real_escape_string(
        &db.mysql,
        escaped.data(),
        value.data(),
        static_cast<unsigned long>(value.size())
    );
    escaped.resize(escaped_size);
    return escaped;
}

bool ownerless_cached_insert_target_foreign_key_state(
    const mylite_db &db,
    std::string_view schema_name,
    std::string_view table_name,
    bool *out_has_foreign_keys
) {
    if (out_has_foreign_keys == nullptr || db.ownerless_observed_dictionary_generation == 0U) {
        return false;
    }
    for (const OwnerlessInsertForeignKeyCacheEntry &entry : db.ownerless_insert_foreign_key_cache) {
        if (entry.dictionary_generation == db.ownerless_observed_dictionary_generation &&
            entry.schema_name == schema_name && entry.table_name == table_name) {
            *out_has_foreign_keys = entry.has_foreign_keys;
            return true;
        }
    }
    return false;
}

void ownerless_cache_insert_target_foreign_key_state(
    mylite_db &db,
    std::string_view schema_name,
    std::string_view table_name,
    bool has_foreign_keys
) {
    if (db.ownerless_observed_dictionary_generation == 0U) {
        return;
    }
    for (OwnerlessInsertForeignKeyCacheEntry &entry : db.ownerless_insert_foreign_key_cache) {
        if (entry.dictionary_generation == db.ownerless_observed_dictionary_generation &&
            entry.schema_name == schema_name && entry.table_name == table_name) {
            entry.has_foreign_keys = has_foreign_keys;
            return;
        }
    }
    db.ownerless_insert_foreign_key_cache.push_back(
        {std::string(schema_name),
         std::string(table_name),
         db.ownerless_observed_dictionary_generation,
         has_foreign_keys}
    );
}

bool ownerless_insert_target_has_foreign_keys(mylite_db &db, const SqlPolicyTokens &tokens) {
    std::string schema_name;
    std::string table_name;
    if (!ownerless_insert_target_table(db, tokens, &schema_name, &table_name)) {
        return true;
    }

    bool has_foreign_keys = true;
    if (ownerless_cached_insert_target_foreign_key_state(
            db,
            schema_name,
            table_name,
            &has_foreign_keys
        )) {
        return has_foreign_keys;
    }

    const ErrorSnapshot snapshot = capture_error(db);
    const std::string escaped_schema = ownerless_escape_metadata_literal(db, schema_name);
    const std::string escaped_table = ownerless_escape_metadata_literal(db, table_name);
    const std::string sql = "SELECT COUNT(*) FROM information_schema.referential_constraints "
                            "WHERE constraint_schema = '" +
                            escaped_schema + "' AND table_name = '" + escaped_table + "'";

    bool query_succeeded = false;
    if (mysql_query(&db.mysql, sql.c_str()) == 0) {
        MYSQL_RES *result = mysql_store_result(&db.mysql);
        if (result != nullptr) {
            MYSQL_ROW row = mysql_fetch_row(result);
            has_foreign_keys =
                row != nullptr && row[0] != nullptr && std::strtoull(row[0], nullptr, 10) != 0U;
            query_succeeded = row != nullptr && row[0] != nullptr;
            mysql_free_result(result);
        }
    }
    restore_error(db, snapshot);
    if (query_succeeded) {
        ownerless_cache_insert_target_foreign_key_state(
            db,
            schema_name,
            table_name,
            has_foreign_keys
        );
    }
    return has_foreign_keys;
}

bool ownerless_cached_insert_target_auto_increment_state(
    const mylite_db &db,
    std::string_view schema_name,
    std::string_view table_name,
    bool *out_has_auto_increment
) {
    if (out_has_auto_increment == nullptr || db.ownerless_observed_dictionary_generation == 0U) {
        return false;
    }
    for (const OwnerlessInsertAutoIncrementCacheEntry &entry :
         db.ownerless_insert_auto_increment_cache) {
        if (entry.dictionary_generation == db.ownerless_observed_dictionary_generation &&
            entry.schema_name == schema_name && entry.table_name == table_name) {
            *out_has_auto_increment = entry.has_auto_increment;
            return true;
        }
    }
    return false;
}

void ownerless_cache_insert_target_auto_increment_state(
    mylite_db &db,
    std::string_view schema_name,
    std::string_view table_name,
    bool has_auto_increment
) {
    if (db.ownerless_observed_dictionary_generation == 0U) {
        return;
    }
    for (OwnerlessInsertAutoIncrementCacheEntry &entry : db.ownerless_insert_auto_increment_cache) {
        if (entry.dictionary_generation == db.ownerless_observed_dictionary_generation &&
            entry.schema_name == schema_name && entry.table_name == table_name) {
            entry.has_auto_increment = has_auto_increment;
            return;
        }
    }
    db.ownerless_insert_auto_increment_cache.push_back(
        {std::string(schema_name),
         std::string(table_name),
         db.ownerless_observed_dictionary_generation,
         has_auto_increment}
    );
}

bool ownerless_insert_target_has_auto_increment(mylite_db &db, const SqlPolicyTokens &tokens) {
    std::string schema_name;
    std::string table_name;
    if (!ownerless_insert_target_table(db, tokens, &schema_name, &table_name)) {
        return true;
    }

    bool has_auto_increment = true;
    if (ownerless_cached_insert_target_auto_increment_state(
            db,
            schema_name,
            table_name,
            &has_auto_increment
        )) {
        return has_auto_increment;
    }

    const ErrorSnapshot snapshot = capture_error(db);
    const std::string escaped_schema = ownerless_escape_metadata_literal(db, schema_name);
    const std::string escaped_table = ownerless_escape_metadata_literal(db, table_name);
    const std::string sql = "SELECT COUNT(*) FROM information_schema.columns "
                            "WHERE table_schema = '" +
                            escaped_schema + "' AND table_name = '" + escaped_table +
                            "' AND extra LIKE '%auto_increment%'";

    bool query_succeeded = false;
    if (mysql_query(&db.mysql, sql.c_str()) == 0) {
        MYSQL_RES *result = mysql_store_result(&db.mysql);
        if (result != nullptr) {
            MYSQL_ROW row = mysql_fetch_row(result);
            has_auto_increment =
                row != nullptr && row[0] != nullptr && std::strtoull(row[0], nullptr, 10) != 0U;
            query_succeeded = row != nullptr && row[0] != nullptr;
            mysql_free_result(result);
        }
    }
    restore_error(db, snapshot);
    if (query_succeeded) {
        ownerless_cache_insert_target_auto_increment_state(
            db,
            schema_name,
            table_name,
            has_auto_increment
        );
    }
    return has_auto_increment;
}

bool sql_statement_requests_write_transaction(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "SET", "START")) {
        return false;
    }

    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "READ") &&
            token_equals(identifier_token_at(tokens, index + 1U), "WRITE")) {
            return true;
        }
    }
    return false;
}

bool sql_statement_uses_locking_read(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "FOR") &&
            token_equals(identifier_token_at(tokens, index + 1U), "UPDATE")) {
            return true;
        }
        if (index + 3U < tokens.count && token_equals(identifier_token_at(tokens, index), "LOCK") &&
            token_equals(identifier_token_at(tokens, index + 1U), "IN") &&
            token_equals(identifier_token_at(tokens, index + 2U), "SHARE") &&
            token_equals(identifier_token_at(tokens, index + 3U), "MODE")) {
            return true;
        }
    }
    return false;
}

bool sql_statement_needs_ownerless_current_read_refresh(const SqlPolicyTokens &tokens) {
    return sql_statement_requires_write(tokens) || sql_statement_uses_locking_read(tokens);
}

bool ownerless_transaction_end_has_local_write(const mylite_db &db, const SqlPolicyTokens &tokens) {
    return sql_ends_explicit_transaction(tokens) &&
           ownerless_connection_is_in_explicit_transaction(db) &&
           db.ownerless_transaction_has_local_write;
}

bool ownerless_transaction_end_blocks_waiting_native_lock(mylite_db &db) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    void *lock_registry = nullptr;
    std::size_t lock_registry_size = 0;
    void *page_write_lock_registry = nullptr;
    std::size_t page_write_lock_registry_size = 0;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        lock_registry = g_runtime.ownerless_innodb_lock_hook.lock_registry;
        lock_registry_size = g_runtime.ownerless_innodb_lock_hook.lock_registry_size;
        page_write_lock_registry = g_runtime.ownerless_innodb_lock_hook.page_write_lock_registry;
        page_write_lock_registry_size =
            g_runtime.ownerless_innodb_lock_hook.page_write_lock_registry_size;
        owner_id = g_runtime.ownerless_innodb_lock_hook.owner_id;
        owner_generation = g_runtime.ownerless_innodb_lock_hook.owner_generation;
    }
    if (owner_id == 0U || owner_generation == 0U) {
        return false;
    }

    const auto registry_blocks_waiter = [&](void *registry, std::size_t registry_size) {
        if (registry == nullptr || registry_size == 0U) {
            return false;
        }
        int blocks_waiting_lock = 0;
        const int registry_result = mylite_ownerless_innodb_lock_registry_owner_blocks_waiting_lock(
            registry,
            registry_size,
            owner_id,
            owner_id,
            owner_generation,
            &blocks_waiting_lock
        );
        return registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK &&
               blocks_waiting_lock != 0;
    };

    return registry_blocks_waiter(lock_registry, lock_registry_size) ||
           registry_blocks_waiter(page_write_lock_registry, page_write_lock_registry_size);
}

bool is_unsupported_oracle_sql_mode_statement(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "SQL_MODE") &&
            is_sql_mode_assignment_target(tokens, index) &&
            sql_mode_assignment_mentions_oracle(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_procedure_analyse_statement(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "SELECT")) {
        return false;
    }

    for (std::size_t index = 1; index + 2U < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "PROCEDURE") &&
            token_equals(tokens.values[index + 1U], "ANALYSE") &&
            token_equals(tokens.values[index + 2U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_vector_runtime_statement(const SqlPolicyTokens &tokens) {
    return is_unsupported_vector_sql_function_statement(tokens) ||
           is_unsupported_vector_index_statement(tokens);
}

bool is_unsupported_xml_sql_function_statement(const SqlPolicyTokens &tokens) {
    return is_unsupported_xml_sql_function_call(tokens);
}

bool is_unsupported_dynamic_column_statement(const SqlPolicyTokens &tokens) {
    return is_unsupported_dynamic_column_function_call(tokens);
}

bool is_unsupported_table_directory_option_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ALTER", "CREATE")) {
        return false;
    }

    bool found_table = false;
    for (std::size_t index = 1U; index < tokens.count; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token.empty()) {
            continue;
        }
        if (!found_table) {
            found_table = token_equals(token, "TABLE");
            continue;
        }
        if (token_in(token, "DATA", "INDEX") &&
            token_equals(identifier_token_at(tokens, index + 1U), "DIRECTORY")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_engine_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    return sql_sets_non_innodb_storage_engine_variable(tokens) ||
           sql_uses_non_innodb_table_engine(tokens);
}

bool is_unsupported_ownerless_routine_ddl_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ALTER", "CREATE", "DROP")) {
        return false;
    }

    for (std::size_t index = 1U; index < tokens.count && index < 12U; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token_in(token, "FUNCTION", "PACKAGE", "PROCEDURE")) {
            return true;
        }
        if (token_in(token, "DATABASE", "EVENT", "INDEX", "ROLE") ||
            token_in(token, "SCHEMA", "SEQUENCE", "SERVER", "TABLE") ||
            token_in(token, "TRIGGER", "USER", "VIEW")) {
            return false;
        }
    }
    return false;
}

bool is_unsupported_ownerless_routine_execution_statement(
    const mylite_db &db,
    std::string_view sql
) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    return token_equals(identifier_token_at(tokens, 0), "CALL");
}

bool is_unsupported_ownerless_sequence_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (token_in(first, "ALTER", "CREATE", "DROP")) {
        for (std::size_t index = 1U; index < tokens.count && index < 12U; ++index) {
            const std::string_view token = identifier_token_at(tokens, index);
            if (token_equals(token, "SEQUENCE")) {
                return true;
            }
            if (token_in(token, "DATABASE", "EVENT", "FUNCTION", "INDEX") ||
                token_in(token, "PROCEDURE", "ROLE", "SCHEMA", "SERVER") ||
                token_in(token, "TABLE", "TRIGGER", "USER", "VIEW")) {
                return false;
            }
        }
    }

    for (std::size_t index = 0; index < tokens.count; ++index) {
        if (index + 2U < tokens.count && token_in(tokens.values[index], "NEXT", "PREVIOUS") &&
            token_equals(tokens.values[index + 1U], "VALUE") &&
            token_equals(tokens.values[index + 2U], "FOR")) {
            return true;
        }
        if (index + 1U < tokens.count &&
            token_in(tokens.values[index], "LASTVAL", "NEXTVAL", "SETVAL") &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_table_admin_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ANALYZE", "CHECK", "CHECKSUM", "OPTIMIZE") &&
        !token_equals(first, "REPAIR")) {
        return false;
    }

    for (std::size_t index = 1U; index < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "TABLE")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_lock_tables_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    if (token_in(first, "LOCK", "UNLOCK")) {
        return token_in(second, "TABLE", "TABLES");
    }
    return false;
}

bool is_unsupported_ownerless_flush_table_lock_statement(
    const mylite_db &db,
    std::string_view sql
) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "FLUSH")) {
        return false;
    }

    std::size_t table_index = 1U;
    if (token_in(identifier_token_at(tokens, table_index), "LOCAL", "NO_WRITE_TO_BINLOG")) {
        ++table_index;
    }
    if (!token_in(identifier_token_at(tokens, table_index), "TABLE", "TABLES")) {
        return false;
    }

    for (std::size_t index = table_index + 1U; index < tokens.count; ++index) {
        if (index + 2U < tokens.count && token_equals(identifier_token_at(tokens, index), "WITH") &&
            token_equals(identifier_token_at(tokens, index + 1U), "READ") &&
            token_equals(identifier_token_at(tokens, index + 2U), "LOCK")) {
            return true;
        }
        if (index + 1U < tokens.count && token_equals(identifier_token_at(tokens, index), "FOR") &&
            token_equals(identifier_token_at(tokens, index + 1U), "EXPORT")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_isolation_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    OwnerlessTransactionIsolation isolation = OwnerlessTransactionIsolation::RepeatableRead;
    bool session_scope = false;
    return (sql_sets_transaction_isolation(tokens, &isolation, &session_scope) &&
            isolation == OwnerlessTransactionIsolation::ReadUncommitted) ||
           sql_assigns_transaction_isolation_variable(tokens);
}

bool is_unsupported_ownerless_special_index_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ALTER", "CREATE")) {
        return false;
    }

    for (std::size_t index = 1U; index < tokens.count; ++index) {
        if (token_in(tokens.values[index], "FULLTEXT", "SPATIAL")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_partition_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ALTER", "CREATE")) {
        return false;
    }

    bool found_table = false;
    for (std::size_t index = 1U; index < tokens.count; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token.empty()) {
            break;
        }
        if (!found_table) {
            found_table = token_equals(token, "TABLE");
            continue;
        }
        if (token_in(token, "PARTITION", "PARTITIONING", "PARTITIONS") ||
            token_in(token, "SUBPARTITION", "SUBPARTITIONING", "SUBPARTITIONS")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_tablespace_management_statement(
    const mylite_db &db,
    std::string_view sql
) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "ALTER")) {
        return false;
    }

    bool found_table = false;
    for (std::size_t index = 1U; index < tokens.count; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token.empty()) {
            break;
        }
        if (!found_table) {
            found_table = token_equals(token, "TABLE");
            continue;
        }
        if (token_in(token, "DISCARD", "IMPORT") &&
            token_equals(identifier_token_at(tokens, index + 1U), "TABLESPACE")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_table_storage_option_statement(
    const mylite_db &db,
    std::string_view sql
) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    const bool is_create = token_equals(first, "CREATE");
    const bool is_alter = token_equals(first, "ALTER");
    if (!token_in(first, "ALTER", "CREATE")) {
        return false;
    }

    bool found_table = false;
    int paren_depth = 0;
    for (std::size_t index = 0U; index < tokens.count; ++index) {
        const std::string_view raw_token = tokens.values[index];
        if (token_equals(raw_token, "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(raw_token, ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth != 0 || !is_sql_identifier_token(raw_token)) {
            continue;
        }

        const std::string_view token = raw_token;
        if (!found_table) {
            found_table = token_equals(token, "TABLE");
            continue;
        }
        if (is_create && token_in(token, "LIKE", "AS", "SELECT")) {
            return false;
        }
        if (token_in(
                token,
                "PAGE_COMPRESSED",
                "PAGE_COMPRESSION_LEVEL",
                "ENCRYPTED",
                "ENCRYPTION_KEY_ID"
            )) {
            if (token_equals(index + 1U < tokens.count ? tokens.values[index + 1U] : "", "=")) {
                return true;
            }
            continue;
        }
        if (token_equals(token, "TABLESPACE")) {
            const std::string_view previous =
                index > 0U && is_sql_identifier_token(tokens.values[index - 1U])
                    ? tokens.values[index - 1U]
                    : "";
            const bool qualified_table_name =
                index > 0U && token_equals(tokens.values[index - 1U], ".");
            const bool starts_table_definition =
                index + 1U < tokens.count && token_equals(tokens.values[index + 1U], "(");
            if (!qualified_table_name && !starts_table_definition &&
                !(is_alter && token_in(previous, "ADD", "COLUMN", "CHANGE", "MODIFY"))) {
                return true;
            }
        }
    }
    return false;
}

bool sql_sets_non_innodb_storage_engine_variable(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index + 2U < tokens.count; ++index) {
        if (is_ownerless_storage_engine_variable(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index) &&
            !is_ownerless_supported_default_engine(tokens.values[index + 2U])) {
            return true;
        }
    }
    return false;
}

bool sql_uses_non_innodb_table_engine(const SqlPolicyTokens &tokens) {
    if (!sql_statement_can_use_table_engine_option(tokens)) {
        return false;
    }

    int paren_depth = 0;
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(tokens.values[index], ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth != 0 || !identifier_token_equals(tokens.values[index], "ENGINE")) {
            continue;
        }

        std::size_t engine_index = index + 1U;
        if (token_equals(tokens.values[engine_index], "=")) {
            ++engine_index;
        }
        if (engine_index >= tokens.count) {
            continue;
        }
        if (!is_ownerless_supported_table_engine(tokens.values[engine_index])) {
            return true;
        }
    }
    return false;
}

bool sql_statement_can_use_table_engine_option(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "CREATE", "ALTER")) {
        return false;
    }

    for (std::size_t index = 1U; index < 6U; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "TABLE")) {
            return true;
        }
    }
    return false;
}

bool sql_assigns_transaction_isolation_variable(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1U; index + 2U < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], "TX_ISOLATION") ||
            identifier_token_equals(tokens.values[index], "TRANSACTION_ISOLATION")) {
            if (is_system_variable_qualified_token(tokens, index)) {
                return true;
            }
        }
    }
    return false;
}

bool is_ownerless_storage_engine_variable(std::string_view token) {
    return identifier_token_equals(token, "DEFAULT_STORAGE_ENGINE") ||
           identifier_token_equals(token, "STORAGE_ENGINE") ||
           identifier_token_equals(token, "DEFAULT_TMP_STORAGE_ENGINE") ||
           identifier_token_equals(token, "ENFORCE_STORAGE_ENGINE");
}

bool is_ownerless_supported_default_engine(std::string_view engine) {
    return identifier_token_equals(engine, "INNODB") || identifier_token_equals(engine, "DEFAULT");
}

bool is_ownerless_supported_table_engine(std::string_view engine) {
    return identifier_token_equals(engine, "INNODB");
}

bool is_unsupported_server_surface_sql(
    const SqlPolicyTokens &tokens,
    const std::string &current_schema
) {
    if (identifier_token_at(tokens, 0).empty()) {
        return false;
    }

    return is_unsupported_account_or_event_statement(tokens) ||
           is_unsupported_plugin_statement(tokens) || is_unsupported_udf_statement(tokens) ||
           is_unsupported_replication_statement(tokens) ||
           is_unsupported_binlog_statement(tokens) || is_unsupported_xa_statement(tokens) ||
           is_unsupported_replication_function_statement(tokens) ||
           is_unsupported_server_utility_function_statement(tokens) ||
           is_unsupported_sql_handler_statement(tokens) ||
           is_unsupported_select_file_statement(tokens) ||
           is_unsupported_load_file_import_statement(tokens) ||
           is_unsupported_help_statement(tokens) ||
           is_unsupported_static_show_info_statement(tokens) ||
           is_unsupported_processlist_metadata_statement(tokens) ||
           is_unsupported_thread_control_statement(tokens) ||
           is_unsupported_foreign_server_metadata_statement(tokens) ||
           is_unsupported_backup_statement(tokens) ||
           is_unsupported_userstat_diagnostics_statement(tokens, current_schema) ||
           is_unsupported_user_variable_diagnostics_statement(tokens, current_schema) ||
           is_unsupported_statement_profiling_statement(tokens, current_schema) ||
           is_unsupported_query_cache_statement(tokens) ||
           is_unsupported_query_log_statement(tokens) ||
           is_unsupported_optimizer_trace_statement(tokens, current_schema) ||
           is_unsupported_persistent_statistics_statement(tokens) ||
           is_unsupported_server_set_statement(tokens);
}

bool is_unsupported_account_or_event_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);
    const std::string_view fourth = identifier_token_at(tokens, 3);

    if (token_in(first, "GRANT", "REVOKE")) {
        return true;
    }
    if (token_equals(first, "CREATE")) {
        if (token_equals(second, "DEFINER")) {
            return has_identifier_token(tokens, "EVENT", 2);
        }
        if (token_equals(second, "OR") && token_equals(third, "REPLACE")) {
            return token_equals(fourth, "DEFINER")
                       ? has_identifier_token(tokens, "EVENT", 4)
                       : token_in(fourth, "USER", "ROLE", "EVENT", "SERVER");
        }
        return token_in(second, "USER", "ROLE", "EVENT", "SERVER");
    }
    if (token_equals(first, "ALTER")) {
        if (token_equals(second, "DEFINER")) {
            return has_identifier_token(tokens, "EVENT", 2);
        }
        return token_in(second, "USER", "EVENT", "SERVER");
    }
    if (token_equals(first, "DROP")) {
        return token_in(second, "USER", "ROLE", "EVENT", "SERVER");
    }
    if (token_equals(first, "SHOW")) {
        return token_equals(second, "EVENTS") ||
               (token_equals(second, "CREATE") && token_equals(third, "EVENT"));
    }
    return token_equals(first, "RENAME") && token_equals(second, "USER");
}

bool is_unsupported_plugin_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    return (token_equals(first, "INSTALL") || token_equals(first, "UNINSTALL")) &&
           token_in(second, "PLUGIN", "SONAME");
}

bool is_unsupported_udf_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);
    std::size_t function_index = 1;

    if (!token_equals(first, "CREATE")) {
        return false;
    }
    if (token_equals(second, "OR") && token_equals(third, "REPLACE")) {
        function_index = 3;
    }
    if (token_equals(identifier_token_at(tokens, function_index), "AGGREGATE")) {
        ++function_index;
    }
    return token_equals(identifier_token_at(tokens, function_index), "FUNCTION") &&
           has_identifier_token(tokens, "SONAME", function_index + 1U);
}

bool is_unsupported_replication_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "CHANGE") && token_in(second, "MASTER", "REPLICATION")) {
        return true;
    }
    if (token_equals(first, "RESET") && token_equals(second, "MASTER")) {
        return true;
    }
    return token_in(first, "START", "STOP", "RESET") && token_in(second, "SLAVE", "REPLICA");
}

bool is_unsupported_binlog_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    if (token_equals(first, "BINLOG")) {
        return true;
    }
    if (token_equals(first, "SHOW") && token_equals(second, "BINARY")) {
        return token_in(third, "LOGS", "STATUS");
    }
    if (token_equals(first, "SHOW") && token_equals(second, "BINLOG")) {
        return token_equals(third, "EVENTS");
    }
    if (token_equals(first, "SHOW") && token_in(second, "MASTER", "SLAVE", "REPLICA")) {
        return token_equals(third, "STATUS");
    }
    if (token_equals(first, "FLUSH") && token_equals(second, "BINARY")) {
        return token_equals(third, "LOGS");
    }
    if (token_equals(first, "RESET") && token_equals(second, "MASTER")) {
        return true;
    }
    return token_equals(first, "PURGE") && token_in(second, "BINARY", "MASTER");
}

bool is_unsupported_xa_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "XA");
}

bool is_unsupported_replication_function_statement(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "MASTER_GTID_WAIT") ||
             identifier_token_equals(tokens.values[index], "MASTER_POS_WAIT") ||
             identifier_token_equals(tokens.values[index], "BINLOG_GTID_POS") ||
             identifier_token_equals(tokens.values[index], "WSREP_SYNC_WAIT_UPTO_GTID")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_vector_sql_function_statement(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "VEC_DISTANCE") ||
             identifier_token_equals(tokens.values[index], "VEC_DISTANCE_COSINE") ||
             identifier_token_equals(tokens.values[index], "VEC_DISTANCE_EUCLIDEAN") ||
             identifier_token_equals(tokens.values[index], "VEC_FROMTEXT") ||
             identifier_token_equals(tokens.values[index], "VEC_TOTEXT")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_vector_index_statement(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (!identifier_token_equals(tokens.values[index], "VECTOR")) {
            continue;
        }
        if (identifier_token_equals(tokens.values[index + 1U], "KEY") ||
            identifier_token_equals(tokens.values[index + 1U], "INDEX")) {
            return true;
        }
        if (token_equals(tokens.values[index + 1U], "(") && index > 0U &&
            token_in(tokens.values[index - 1U], "(", ",")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_xml_sql_function_call(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "EXTRACTVALUE") ||
             identifier_token_equals(tokens.values[index], "UPDATEXML")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_dynamic_column_function_call(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "COLUMN_ADD") ||
             identifier_token_equals(tokens.values[index], "COLUMN_CHECK") ||
             identifier_token_equals(tokens.values[index], "COLUMN_CREATE") ||
             identifier_token_equals(tokens.values[index], "COLUMN_DELETE") ||
             identifier_token_equals(tokens.values[index], "COLUMN_EXISTS") ||
             identifier_token_equals(tokens.values[index], "COLUMN_GET") ||
             identifier_token_equals(tokens.values[index], "COLUMN_JSON") ||
             identifier_token_equals(tokens.values[index], "COLUMN_LIST")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_server_utility_function_statement(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "BENCHMARK") ||
             identifier_token_equals(tokens.values[index], "GET_LOCK") ||
             identifier_token_equals(tokens.values[index], "IS_FREE_LOCK") ||
             identifier_token_equals(tokens.values[index], "IS_USED_LOCK") ||
             identifier_token_equals(tokens.values[index], "LOAD_FILE") ||
             identifier_token_equals(tokens.values[index], "RELEASE_ALL_LOCKS") ||
             identifier_token_equals(tokens.values[index], "RELEASE_LOCK") ||
             identifier_token_equals(tokens.values[index], "SLEEP") ||
             identifier_token_equals(tokens.values[index], "UUID_SHORT")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_sql_handler_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "HANDLER");
}

bool is_unsupported_select_file_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);

    if (!token_in(first, "SELECT", "WITH")) {
        return false;
    }

    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "INTO") &&
            token_in(tokens.values[index + 1U], "OUTFILE", "DUMPFILE")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_load_file_import_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "LOAD") &&
           token_in(identifier_token_at(tokens, 1), "DATA", "XML");
}

bool is_unsupported_help_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "HELP");
}

bool is_unsupported_static_show_info_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "SHOW") &&
           token_in(identifier_token_at(tokens, 1), "AUTHORS", "CONTRIBUTORS", "PRIVILEGES");
}

bool is_unsupported_processlist_metadata_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    return token_equals(first, "SHOW") &&
           (token_equals(second, "PROCESSLIST") ||
            (token_equals(second, "FULL") && token_equals(third, "PROCESSLIST")));
}

bool is_unsupported_thread_control_statement(const SqlPolicyTokens &tokens) {
    return token_in(identifier_token_at(tokens, 0), "KILL", "SHUTDOWN");
}

bool is_unsupported_foreign_server_metadata_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    return token_equals(first, "SHOW") && token_equals(second, "CREATE") &&
           token_equals(third, "SERVER");
}

bool is_unsupported_backup_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "BACKUP");
}

bool is_unsupported_userstat_diagnostics_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (has_information_schema_userstat_statistics_table(tokens) ||
        has_current_schema_userstat_statistics_table_reference(tokens, current_schema)) {
        return true;
    }
    if (token_equals(first, "FLUSH")) {
        const std::size_t flush_target_index =
            token_in(second, "LOCAL", "NO_WRITE_TO_BINLOG") ? 2U : 1U;
        return is_userstat_statistics_table_token(identifier_token_at(tokens, flush_target_index));
    }
    if (!token_equals(first, "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], "USERSTAT") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_user_variable_diagnostics_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (has_information_schema_table(tokens, "USER_VARIABLES") ||
        has_current_schema_table_reference(tokens, "USER_VARIABLES", current_schema)) {
        return true;
    }
    if (token_equals(first, "SHOW")) {
        return token_equals(second, "USER_VARIABLES");
    }
    if (token_equals(first, "FLUSH")) {
        const std::size_t flush_target_index =
            token_in(second, "LOCAL", "NO_WRITE_TO_BINLOG") ? 2U : 1U;
        return identifier_token_equals(
            identifier_token_at(tokens, flush_target_index),
            "USER_VARIABLES"
        );
    }
    return false;
}

bool is_unsupported_statement_profiling_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (has_information_schema_table(tokens, "PROFILING") ||
        has_current_schema_table_reference(tokens, "PROFILING", current_schema)) {
        return true;
    }
    if (token_equals(first, "SHOW") && token_in(second, "PROFILE", "PROFILES")) {
        return true;
    }
    if (!token_equals(first, "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_in(tokens.values[index], "PROFILING", "PROFILING_HISTORY_SIZE") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_query_cache_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    return (token_equals(first, "FLUSH") || token_equals(first, "RESET")) &&
           token_equals(second, "QUERY") && token_equals(third, "CACHE");
}

bool is_unsupported_query_log_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "FLUSH")) {
        const std::size_t flush_target_index =
            token_in(second, "LOCAL", "NO_WRITE_TO_BINLOG") ? 2U : 1U;
        const std::string_view flush_target = identifier_token_at(tokens, flush_target_index);
        const std::string_view flush_target_tail =
            identifier_token_at(tokens, flush_target_index + 1U);

        return token_equals(flush_target, "LOGS") || (token_in(flush_target, "GENERAL", "SLOW") &&
                                                      token_equals(flush_target_tail, "LOGS"));
    }
    if (!token_equals(first, "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (is_query_log_variable_token(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_optimizer_trace_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    if (has_information_schema_table(tokens, "OPTIMIZER_TRACE") ||
        has_current_schema_table_reference(tokens, "OPTIMIZER_TRACE", current_schema)) {
        return true;
    }
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_in(tokens.values[index], "OPTIMIZER_TRACE", "OPTIMIZER_TRACE_MAX_MEM_SIZE") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_persistent_statistics_statement(const SqlPolicyTokens &tokens) {
    if (token_equals(identifier_token_at(tokens, 0), "ANALYZE") &&
        has_identifier_token(tokens, "PERSISTENT", 1)) {
        return true;
    }
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (is_persistent_statistics_variable_token(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_server_set_statement(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "PASSWORD") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
        if (is_server_variable_token(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

SqlPolicyTokens collect_sql_policy_tokens(std::string_view sql) {
    std::size_t offset = 0;
    SqlPolicyTokens tokens;
    tokens.count = 0;
    while (tokens.count < tokens.values.size() &&
           next_sql_token(sql, offset, tokens.values[tokens.count])) {
        ++tokens.count;
    }
    return tokens;
}

bool next_sql_token(std::string_view sql, std::size_t &offset, std::string_view &token) {
    skip_sql_spacing_and_comments(sql, offset);
    if (offset >= sql.size()) {
        token = {};
        return false;
    }

    const std::size_t start = offset;
    if (sql[offset] == '\'' || sql[offset] == '"' || sql[offset] == '`') {
        skip_quoted_sql_token(sql, offset);
        token = sql.substr(start, offset - start);
        return true;
    }

    if (is_sql_identifier_char(sql[offset])) {
        while (offset < sql.size() && is_sql_identifier_char(sql[offset])) {
            ++offset;
        }
        token = sql.substr(start, offset - start);
        return true;
    }

    ++offset;
    token = sql.substr(start, 1);
    return true;
}

void skip_sql_spacing_and_comments(std::string_view sql, std::size_t &offset) {
    for (;;) {
        while (offset < sql.size() && is_sql_space(sql[offset])) {
            ++offset;
        }
        if (enter_executable_sql_comment(sql, offset)) {
            continue;
        }
        if (skip_dash_sql_comment(sql, offset)) {
            continue;
        }
        if (skip_hash_sql_comment(sql, offset)) {
            continue;
        }
        if (skip_block_sql_comment(sql, offset)) {
            continue;
        }
        return;
    }
}

bool enter_executable_sql_comment(std::string_view sql, std::size_t &offset) {
    const bool is_mysql_comment = offset + 2U < sql.size() && sql[offset] == '/' &&
                                  sql[offset + 1U] == '*' && sql[offset + 2U] == '!';
    const bool is_mariadb_comment =
        offset + 3U < sql.size() && sql[offset] == '/' && sql[offset + 1U] == '*' &&
        (sql[offset + 2U] == 'M' || sql[offset + 2U] == 'm') && sql[offset + 3U] == '!';

    if (is_mysql_comment) {
        offset += 3U;
    } else if (is_mariadb_comment) {
        offset += 4U;
    } else {
        return false;
    }

    while (offset < sql.size() && sql[offset] >= '0' && sql[offset] <= '9') {
        ++offset;
    }
    return true;
}

bool skip_dash_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset + 1U >= sql.size() || sql[offset] != '-' || sql[offset + 1U] != '-') {
        return false;
    }
    offset += 2U;
    while (offset < sql.size() && sql[offset] != '\n') {
        ++offset;
    }
    return true;
}

bool skip_hash_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset >= sql.size() || sql[offset] != '#') {
        return false;
    }
    ++offset;
    while (offset < sql.size() && sql[offset] != '\n') {
        ++offset;
    }
    return true;
}

bool skip_block_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset + 1U >= sql.size() || sql[offset] != '/' || sql[offset + 1U] != '*') {
        return false;
    }
    offset += 2U;
    while (offset + 1U < sql.size() && (sql[offset] != '*' || sql[offset + 1U] != '/')) {
        ++offset;
    }
    if (offset + 1U < sql.size()) {
        offset += 2U;
    }
    return true;
}

void skip_quoted_sql_token(std::string_view sql, std::size_t &offset) {
    const char quote = sql[offset++];
    while (offset < sql.size()) {
        if (sql[offset] == '\\' && offset + 1U < sql.size()) {
            offset += 2U;
            continue;
        }
        if (sql[offset++] == quote) {
            return;
        }
    }
}

bool is_sql_space(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f';
}

bool is_sql_identifier_char(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

bool is_sql_identifier_token(std::string_view token) {
    return !token.empty() && is_sql_identifier_char(token[0]);
}

std::string_view first_identifier_token(std::string_view sql) {
    std::size_t offset = 0;
    std::string_view token;
    while (next_sql_token(sql, offset, token)) {
        if (is_sql_identifier_token(token)) {
            return token;
        }
    }
    return {};
}

std::string_view identifier_token_at(const SqlPolicyTokens &tokens, std::size_t index) {
    std::size_t identifier_index = 0;
    for (std::size_t raw_index = 0; raw_index < tokens.count; ++raw_index) {
        if (!is_sql_identifier_token(tokens.values[raw_index])) {
            continue;
        }
        if (identifier_index == index) {
            return tokens.values[raw_index];
        }
        ++identifier_index;
    }
    return {};
}

std::string_view unquoted_identifier_token(std::string_view token) {
    if (token.size() >= 2U && token.front() == '`' && token.back() == '`') {
        token.remove_prefix(1U);
        token.remove_suffix(1U);
    }
    return token;
}

bool has_identifier_token(
    const SqlPolicyTokens &tokens,
    const char *keyword,
    std::size_t start_index
) {
    for (std::size_t index = start_index; !identifier_token_at(tokens, index).empty(); ++index) {
        if (token_equals(identifier_token_at(tokens, index), keyword)) {
            return true;
        }
    }
    return false;
}

bool has_information_schema_userstat_statistics_table(const SqlPolicyTokens &tokens) {
    return has_information_schema_table(tokens, "CLIENT_STATISTICS") ||
           has_information_schema_table(tokens, "INDEX_STATISTICS") ||
           has_information_schema_table(tokens, "TABLE_STATISTICS") ||
           has_information_schema_table(tokens, "USER_STATISTICS");
}

bool has_current_schema_userstat_statistics_table_reference(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    return has_current_schema_table_reference(tokens, "CLIENT_STATISTICS", current_schema) ||
           has_current_schema_table_reference(tokens, "INDEX_STATISTICS", current_schema) ||
           has_current_schema_table_reference(tokens, "TABLE_STATISTICS", current_schema) ||
           has_current_schema_table_reference(tokens, "USER_STATISTICS", current_schema);
}

bool has_information_schema_table(const SqlPolicyTokens &tokens, const char *table_name) {
    for (std::size_t index = 0; index + 2U < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], "INFORMATION_SCHEMA") &&
            token_equals(tokens.values[index + 1U], ".") &&
            identifier_token_equals(tokens.values[index + 2U], table_name)) {
            return true;
        }
    }
    return false;
}

bool has_current_schema_table_reference(
    const SqlPolicyTokens &tokens,
    const char *table_name,
    std::string_view current_schema
) {
    return identifier_token_equals(current_schema, "INFORMATION_SCHEMA") &&
           has_unqualified_table_reference(tokens, table_name);
}

bool has_unqualified_table_reference(const SqlPolicyTokens &tokens, const char *table_name) {
    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], table_name) &&
            table_reference_keyword(tokens.values[index - 1U])) {
            return true;
        }
    }
    return false;
}

bool is_sql_mode_assignment_target(const SqlPolicyTokens &tokens, std::size_t index) {
    return is_system_variable_qualified_token(tokens, index);
}

bool sql_mode_assignment_mentions_oracle(const SqlPolicyTokens &tokens, std::size_t index) {
    int paren_depth = 0;
    for (std::size_t value_index = index + 2U; value_index < tokens.count; ++value_index) {
        const std::string_view token = tokens.values[value_index];
        if (token_equals(token, "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(token, ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth == 0 && token_equals(token, ",")) {
            return false;
        }
        if (token_contains_sql_mode_name(token, "ORACLE")) {
            return true;
        }
    }
    return false;
}

bool token_contains_sql_mode_name(std::string_view token, const char *mode_name) {
    const std::size_t mode_length = std::strlen(mode_name);
    if (mode_length == 0U || token.size() < mode_length) {
        return false;
    }

    for (std::size_t start = 0; start + mode_length <= token.size(); ++start) {
        bool matches = true;
        for (std::size_t offset = 0; offset < mode_length; ++offset) {
            char left = token[start + offset];
            char right = mode_name[offset];
            if (left >= 'a' && left <= 'z') {
                left = static_cast<char>(left - ('a' - 'A'));
            }
            if (right >= 'a' && right <= 'z') {
                right = static_cast<char>(right - ('a' - 'A'));
            }
            if (left != right) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }

        const bool left_boundary = start == 0U || !is_sql_identifier_char(token[start - 1U]);
        const std::size_t end = start + mode_length;
        const bool right_boundary = end == token.size() || !is_sql_identifier_char(token[end]);
        if (left_boundary && right_boundary) {
            return true;
        }
    }
    return false;
}

bool token_equals(std::string_view token, const char *keyword) {
    const std::size_t keyword_length = std::strlen(keyword);
    if (token.size() != keyword_length) {
        return false;
    }
    for (std::size_t index = 0; index < token.size(); ++index) {
        char left = token[index];
        char right = keyword[index];
        if (left >= 'a' && left <= 'z') {
            left = static_cast<char>(left - ('a' - 'A'));
        }
        if (right >= 'a' && right <= 'z') {
            right = static_cast<char>(right - ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

bool identifier_token_equals(std::string_view token, const char *keyword) {
    return token_equals(unquoted_identifier_token(token), keyword);
}

bool table_reference_keyword(std::string_view token) {
    return token_in(token, "FROM", "JOIN", "UPDATE") ||
           token_in(token, "INTO", "TABLE", "DESC", "DESCRIBE");
}

bool token_in(std::string_view token, const char *first, const char *second) {
    return token_equals(token, first) || token_equals(token, second);
}

bool token_in(std::string_view token, const char *first, const char *second, const char *third) {
    return token_equals(token, first) || token_equals(token, second) || token_equals(token, third);
}

bool token_in(
    std::string_view token,
    const char *first,
    const char *second,
    const char *third,
    const char *fourth
) {
    return token_equals(token, first) || token_equals(token, second) ||
           token_equals(token, third) || token_equals(token, fourth);
}

bool is_userstat_statistics_table_token(std::string_view token) {
    return identifier_token_equals(token, "CLIENT_STATISTICS") ||
           identifier_token_equals(token, "INDEX_STATISTICS") ||
           identifier_token_equals(token, "TABLE_STATISTICS") ||
           identifier_token_equals(token, "USER_STATISTICS");
}

bool is_server_variable_token(std::string_view token) {
    return token_in(token, "EVENT_SCHEDULER", "SQL_LOG_BIN", "LOG_BIN", "BINLOG_FORMAT") ||
           token_in(token, "QUERY_CACHE_SIZE", "QUERY_CACHE_TYPE", "QUERY_CACHE_LIMIT") ||
           token_in(
               token,
               "QUERY_CACHE_MIN_RES_UNIT",
               "QUERY_CACHE_WLOCK_INVALIDATE",
               "QUERY_CACHE_STRIP_COMMENTS"
           ) ||
           token_in(token, "GTID_BINLOG_STATE", "GTID_SLAVE_POS", "GTID_STRICT_MODE") ||
           token_in(token, "GTID_DOMAIN_ID", "GTID_SEQ_NO", "GTID_CLEANUP_BATCH_SIZE") ||
           token_in(
               token,
               "GTID_IGNORE_DUPLICATES",
               "GTID_POS_AUTO_ENGINES",
               "BINLOG_GTID_INDEX"
           ) ||
           token_in(
               token,
               "BINLOG_GTID_INDEX_PAGE_SIZE",
               "BINLOG_GTID_INDEX_SPAN_MIN",
               "WSREP_GTID_DOMAIN_ID"
           ) ||
           token_in(token, "WSREP_GTID_SEQ_NO", "WSREP_GTID_MODE") ||
           token_in(
               token,
               "INNODB_BUFFER_POOL_DUMP_NOW",
               "INNODB_BUFFER_POOL_DUMP_AT_SHUTDOWN",
               "INNODB_BUFFER_POOL_DUMP_PCT"
           ) ||
           token_in(
               token,
               "INNODB_BUFFER_POOL_LOAD_NOW",
               "INNODB_BUFFER_POOL_LOAD_ABORT",
               "INNODB_BUFFER_POOL_LOAD_AT_STARTUP"
           ) ||
           token_equals(token, "INNODB_BUFFER_POOL_LOAD_PAGES_ABORT");
}

bool is_query_log_variable_token(std::string_view token) {
    return token_in(token, "GENERAL_LOG", "GENERAL_LOG_FILE", "LOG_OUTPUT") ||
           token_in(token, "SLOW_QUERY_LOG", "SLOW_QUERY_LOG_FILE", "LOG_SLOW_QUERY") ||
           token_in(token, "LOG_SLOW_QUERY_FILE", "SQL_LOG_OFF", "LONG_QUERY_TIME") ||
           token_in(
               token,
               "MIN_EXAMINED_ROW_LIMIT",
               "LOG_SLOW_MIN_EXAMINED_ROW_LIMIT",
               "LOG_SLOW_RATE_LIMIT"
           ) ||
           token_in(
               token,
               "LOG_SLOW_FILTER",
               "LOG_SLOW_VERBOSITY",
               "LOG_SLOW_DISABLED_STATEMENTS"
           ) ||
           token_in(
               token,
               "LOG_SLOW_ADMIN_STATEMENTS",
               "LOG_SLOW_SLAVE_STATEMENTS",
               "LOG_SLOW_MAX_WARNINGS"
           );
}

bool is_persistent_statistics_variable_token(std::string_view token) {
    return token_in(token, "USE_STAT_TABLES", "HISTOGRAM_SIZE", "HISTOGRAM_TYPE");
}

bool is_system_variable_qualified_token(const SqlPolicyTokens &tokens, std::size_t index) {
    if (index + 1U >= tokens.count || !token_equals(tokens.values[index + 1U], "=")) {
        return false;
    }
    if (is_system_variable_assignment_start(tokens, index)) {
        return true;
    }
    if (token_in(tokens.values[index - 1U], "GLOBAL", "SESSION", "LOCAL")) {
        return is_system_variable_assignment_start(tokens, index - 1U);
    }
    if (index >= 2U && token_equals(tokens.values[index - 1U], "@") &&
        token_equals(tokens.values[index - 2U], "@")) {
        return true;
    }
    return index >= 4U && token_equals(tokens.values[index - 1U], ".") &&
           token_in(tokens.values[index - 2U], "GLOBAL", "SESSION", "LOCAL") &&
           token_equals(tokens.values[index - 3U], "@") &&
           token_equals(tokens.values[index - 4U], "@");
}

bool is_system_variable_assignment_start(const SqlPolicyTokens &tokens, std::size_t index) {
    const std::size_t first_assignment = first_set_assignment_token_index(tokens);
    int paren_depth = 0;

    if (index == first_assignment) {
        return true;
    }
    for (std::size_t token_index = first_assignment; token_index < index; ++token_index) {
        if (token_equals(tokens.values[token_index], "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(tokens.values[token_index], ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth == 0 && first_assignment == 2U &&
            token_equals(tokens.values[token_index], "FOR")) {
            return false;
        }
    }
    return index > 0U && paren_depth == 0 && token_equals(tokens.values[index - 1U], ",");
}

std::size_t first_set_assignment_token_index(const SqlPolicyTokens &tokens) {
    if (tokens.count > 2U && token_equals(tokens.values[1], "STATEMENT") &&
        !token_equals(tokens.values[2], "=")) {
        return 2U;
    }
    return 1U;
}

int validate_runtime_database_path(mylite_db &db) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count > 0U && g_runtime.database_path != db.database_path) {
        set_error(db, MYLITE_BUSY, "embedded runtime is already open for another database");
        return MYLITE_BUSY;
    }
    return MYLITE_OK;
}

int store_and_emit_result(
    mylite_db &db,
    mylite_exec_result_metadata_callback metadata_callback,
    mylite_exec_result_callback callback,
    void *ctx,
    bool *has_result
) {
    MYSQL_RES *result = mysql_store_result(&db.mysql);
    if (result == nullptr) {
        if (mysql_field_count(&db.mysql) != 0U) {
            set_mariadb_error(db);
            return MYLITE_ERROR;
        }
        return drain_remaining_query_results(db);
    }
    *has_result = true;

    const unsigned field_count = mysql_num_fields(result);
    if (field_count > static_cast<unsigned>(INT_MAX)) {
        mysql_free_result(result);
        set_error(db, MYLITE_ERROR, "result has too many columns");
        return MYLITE_ERROR;
    }

    std::vector<mylite_exec_column> columns;
    columns.reserve(field_count);
    const MYSQL_FIELD *fields = mysql_fetch_fields(result);
    for (unsigned i = 0; i < field_count; ++i) {
        mylite_exec_column column = {};
        column.name = fields[i].name;
        column.org_name = fields[i].org_name;
        column.table = fields[i].table;
        column.org_table = fields[i].org_table;
        columns.push_back(column);
    }

    if (metadata_callback != nullptr &&
        metadata_callback(ctx, static_cast<int>(field_count), columns.data()) != 0) {
        mysql_free_result(result);
        static_cast<void>(drain_remaining_query_results(db));
        set_error(db, MYLITE_ERROR, "query metadata callback requested abort");
        return MYLITE_ERROR;
    }

    std::vector<std::size_t> value_lengths;
    if (callback != nullptr) {
        value_lengths.resize(field_count);
    }
    for (MYSQL_ROW row = mysql_fetch_row(result); row != nullptr; row = mysql_fetch_row(result)) {
        if (callback != nullptr) {
            const unsigned long *lengths = mysql_fetch_lengths(result);
            if (lengths == nullptr) {
                mysql_free_result(result);
                static_cast<void>(drain_remaining_query_results(db));
                set_mariadb_error(db);
                return MYLITE_ERROR;
            }
            for (unsigned i = 0; i < field_count; ++i) {
                value_lengths[i] = static_cast<std::size_t>(lengths[i]);
            }
            if (callback(
                    ctx,
                    static_cast<int>(field_count),
                    row,
                    value_lengths.data(),
                    columns.data()
                ) != 0) {
                mysql_free_result(result);
                static_cast<void>(drain_remaining_query_results(db));
                set_error(db, MYLITE_ERROR, "query callback requested abort");
                return MYLITE_ERROR;
            }
        }
    }

    mysql_free_result(result);
    return drain_remaining_query_results(db);
}

int drain_remaining_query_results(mylite_db &db) {
    while (mysql_more_results(&db.mysql) != 0) {
        const int next_result = mysql_next_result(&db.mysql);
        if (next_result > 0) {
            set_mariadb_error(db);
            return MYLITE_ERROR;
        }
        if (next_result < 0) {
            return MYLITE_OK;
        }

        MYSQL_RES *result = mysql_store_result(&db.mysql);
        if (result != nullptr) {
            mysql_free_result(result);
            continue;
        }
        if (mysql_field_count(&db.mysql) != 0U) {
            set_mariadb_error(db);
            return MYLITE_ERROR;
        }
    }
    return MYLITE_OK;
}

NativeControlStatement classify_native_control_statement(std::string_view sql) {
    std::size_t offset = 0;
    std::array<std::string_view, 6> tokens;
    std::size_t token_count = 0;
    if (!next_sql_token(sql, offset, tokens[token_count])) {
        return NativeControlStatement::None;
    }
    ++token_count;
    if (!token_in(tokens[0], "COMMIT", "ROLLBACK", "SET", "START")) {
        return NativeControlStatement::None;
    }

    while (token_count < tokens.size() && next_sql_token(sql, offset, tokens[token_count])) {
        ++token_count;
    }
    std::string_view extra_token;
    if (next_sql_token(sql, offset, extra_token)) {
        return NativeControlStatement::None;
    }

    if (sql_tokens_have_only_optional_trailing_semicolon(tokens, token_count, 1U)) {
        if (token_equals(tokens[0], "COMMIT")) {
            return NativeControlStatement::Commit;
        }
        if (token_equals(tokens[0], "ROLLBACK")) {
            return NativeControlStatement::Rollback;
        }
    }

    if (sql_tokens_have_only_optional_trailing_semicolon(tokens, token_count, 2U) &&
        token_equals(tokens[0], "START") && token_equals(tokens[1], "TRANSACTION")) {
        return NativeControlStatement::StartTransaction;
    }

    if (!sql_tokens_have_only_optional_trailing_semicolon(tokens, token_count, 4U)) {
        return NativeControlStatement::None;
    }
    if (!token_equals(tokens[0], "SET") || !token_equals(tokens[1], "AUTOCOMMIT") ||
        !token_equals(tokens[2], "=")) {
        return NativeControlStatement::None;
    }
    if (token_equals(tokens[3], "0")) {
        return NativeControlStatement::AutocommitOff;
    }
    if (token_equals(tokens[3], "1")) {
        return NativeControlStatement::AutocommitOn;
    }
    return NativeControlStatement::None;
}

bool sql_tokens_have_only_optional_trailing_semicolon(
    const std::array<std::string_view, 6> &tokens,
    std::size_t token_count,
    std::size_t required_count
) {
    if (token_count == required_count) {
        return true;
    }
    return token_count == required_count + 1U && token_equals(tokens[required_count], ";");
}

int execute_native_control_statement(mylite_db &db, NativeControlStatement statement) {
    switch (statement) {
    case NativeControlStatement::Commit:
        return mysql_commit(&db.mysql) == 0 ? MYLITE_OK : MYLITE_ERROR;
    case NativeControlStatement::Rollback:
        return mysql_rollback(&db.mysql) == 0 ? MYLITE_OK : MYLITE_ERROR;
    case NativeControlStatement::AutocommitOff:
        return mysql_autocommit(&db.mysql, 0) == 0 ? MYLITE_OK : MYLITE_ERROR;
    case NativeControlStatement::AutocommitOn:
        return mysql_autocommit(&db.mysql, 1) == 0 ? MYLITE_OK : MYLITE_ERROR;
    case NativeControlStatement::StartTransaction:
        return mylite_embedded_start_transaction(&db.mysql) == 0 ? MYLITE_OK : MYLITE_ERROR;
    case NativeControlStatement::None:
        break;
    }
    return MYLITE_MISUSE;
}

bool native_control_autocommit_is_noop(const mylite_db &db, NativeControlStatement statement) {
    const bool server_autocommit = (db.mysql.server_status & SERVER_STATUS_AUTOCOMMIT) != 0U;
    switch (statement) {
    case NativeControlStatement::AutocommitOff:
        return !server_autocommit;
    case NativeControlStatement::AutocommitOn:
        return server_autocommit;
    case NativeControlStatement::Commit:
    case NativeControlStatement::Rollback:
    case NativeControlStatement::StartTransaction:
    case NativeControlStatement::None:
        break;
    }
    return false;
}

void rollback_active_transaction_after_deadlock(mylite_db &db) {
    if (db.mariadb_errno != k_mariadb_lock_deadlock_errno) {
        return;
    }

    const ErrorSnapshot snapshot = capture_error(db);
    static_cast<void>(rollback_active_transaction(db));
    restore_error(db, snapshot);
}

void rollback_failed_ownerless_implicit_statement(
    mylite_db &db,
    bool statement_started_in_explicit_transaction
) {
    if (!db.ownerless_rw_open || db.mariadb_errno == k_mariadb_lock_deadlock_errno ||
        statement_started_in_explicit_transaction) {
        return;
    }

    const ErrorSnapshot snapshot = capture_error(db);
    const int rollback_result = rollback_active_transaction(db);
    if (rollback_result == MYLITE_OK) {
        mylite_ownerless_innodb_close_current_read_view();
        std::uint64_t latest_lsn = 0;
        const int observe_result = mylite_ownerless_innodb_redo_observe(&latest_lsn);
        if (observe_result == MYLITE_OWNERLESS_INNODB_LOCK_OK && latest_lsn != 0U) {
            const std::uint64_t boundary_lsn =
                std::max(latest_lsn, mylite_ownerless_innodb_current_lsn());
            mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_no_skip(
                boundary_lsn
            );
        }
        mylite_ownerless_innodb_evict_clean_external_pages();
    }
    restore_error(db, snapshot);
}

int rollback_active_transaction(mylite_db &db) {
    if (!db.connected) {
        return MYLITE_OK;
    }
    if (mysql_query(&db.mysql, "ROLLBACK") != 0) {
        set_mariadb_error(db);
        return MYLITE_ERROR;
    }
    const int drain_result = drain_remaining_query_results(db);
    if (drain_result == MYLITE_OK) {
        release_ownerless_transaction_page_version_pin(db);
        set_ownerless_explicit_transaction_active(db, false);
        db.ownerless_transaction_has_local_write = false;
        db.ownerless_transaction_has_locking_read = false;
        reset_ownerless_transaction_visible_fast_proof(db);
        db.ownerless_transaction_snapshot_visible_lsn = 0;
        db.ownerless_transaction_snapshot_visibility_pinned = false;
    }
    return drain_result;
}

ErrorSnapshot capture_error(const mylite_db &db) {
    return {db.errcode, db.extended_errcode, db.mariadb_errno, db.sqlstate, db.errmsg};
}

void restore_error(mylite_db &db, const ErrorSnapshot &snapshot) {
    db.errcode = snapshot.errcode;
    db.extended_errcode = snapshot.extended_errcode;
    db.mariadb_errno = snapshot.mariadb_errno;
    db.sqlstate = snapshot.sqlstate;
    db.errmsg = snapshot.errmsg;
}

int initialize_statement_results(mylite_stmt &stmt, bool release_existing_results) {
    if (release_existing_results && stmt.metadata != nullptr && !stmt.columns.empty()) {
        if (stmt.result_binds_dirty) {
            const int bind_result = refresh_dirty_result_binds(stmt);
            if (bind_result != MYLITE_OK) {
                return bind_result;
            }
        }
        stmt.has_result = true;
        return MYLITE_OK;
    }

    if (release_existing_results) {
        release_statement_results(stmt);
    }

    stmt.metadata = mysql_stmt_result_metadata(stmt.stmt);
    if (stmt.metadata == nullptr) {
        if (mysql_stmt_field_count(stmt.stmt) != 0U) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }
        stmt.has_result = false;
        return MYLITE_OK;
    }

    const unsigned field_count = mysql_num_fields(stmt.metadata);
    const MYSQL_FIELD *fields = mysql_fetch_fields(stmt.metadata);
    stmt.columns.resize(field_count);
    stmt.result_binds.resize(field_count);
    for (unsigned index = 0; index < field_count; ++index) {
        ResultColumn &column = stmt.columns[index];
        column.field_type = fields[index].type;
        column.flags = fields[index].flags;
        column.name = fields[index].name != nullptr ? fields[index].name : "";
        column.org_name = fields[index].org_name != nullptr ? fields[index].org_name : "";
        column.table = fields[index].table != nullptr ? fields[index].table : "";
        column.org_table = fields[index].org_table != nullptr ? fields[index].org_table : "";
        column.length = 0;
        column.is_null = 0;
        column.error = 0;
        column.bind = {};
        column.bind.buffer_type = MYSQL_TYPE_STRING;
        column.bind.length = &column.length;
        column.bind.is_null = &column.is_null;
        column.bind.error = &column.error;
        const unsigned long buffer_length =
            std::min(std::max(fields[index].length, 1UL), k_initial_result_buffer_size);
        if (configure_column_buffer(column, buffer_length) != MYLITE_OK) {
            set_error(*stmt.db, MYLITE_NOMEM, "result column buffer could not be allocated");
            return MYLITE_NOMEM;
        }
        stmt.result_binds[index] = column.bind;
    }

    if (mysql_stmt_bind_result(stmt.stmt, stmt.result_binds.data()) != 0) {
        set_mariadb_statement_error(stmt);
        return MYLITE_ERROR;
    }
    stmt.has_result = true;
    return MYLITE_OK;
}

int fetch_statement_row(mylite_stmt &stmt) {
    const int bind_result = refresh_dirty_result_binds(stmt);
    if (bind_result != MYLITE_OK) {
        clear_statement_ownerless_runtime_activity(stmt);
        clear_statement_ownerless_page_visibility(stmt);
        return bind_result;
    }

    const int fetch_result = mysql_stmt_fetch(stmt.stmt);
    if (fetch_result == MYSQL_NO_DATA) {
        stmt.has_row = false;
        const int drain_result = drain_remaining_statement_results(stmt);
        if (drain_result != MYLITE_OK) {
            clear_statement_ownerless_runtime_activity(stmt);
            clear_statement_ownerless_page_visibility(stmt);
            return drain_result;
        }
        stmt.has_result = false;
        clear_statement_ownerless_runtime_activity(stmt);
        clear_statement_ownerless_page_visibility(stmt);
        return MYLITE_DONE;
    }
    if (fetch_result != 0 && fetch_result != MYSQL_DATA_TRUNCATED) {
        set_mariadb_statement_error(stmt);
        clear_statement_ownerless_runtime_activity(stmt);
        clear_statement_ownerless_page_visibility(stmt);
        return MYLITE_ERROR;
    }
    if (fetch_result == MYSQL_DATA_TRUNCATED) {
        const int truncated_result = fetch_truncated_statement_columns(stmt);
        if (truncated_result != MYLITE_OK) {
            clear_statement_ownerless_runtime_activity(stmt);
            clear_statement_ownerless_page_visibility(stmt);
            return truncated_result;
        }
    }

    for (ResultColumn &column : stmt.columns) {
        if (column.length < column.buffer.size()) {
            column.buffer[column.length] = 0U;
        } else if (!column.buffer.empty()) {
            column.buffer.back() = 0U;
        }
    }
    stmt.has_row = true;
    return MYLITE_ROW;
}

int drain_remaining_statement_results(mylite_stmt &stmt) {
    for (;;) {
        const int next_result = mysql_stmt_next_result(stmt.stmt);
        if (next_result < 0) {
            return MYLITE_OK;
        }
        if (next_result > 0) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }

        if (mysql_stmt_field_count(stmt.stmt) == 0U) {
            continue;
        }
        if (mysql_stmt_store_result(stmt.stmt) != 0) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }
        if (mysql_stmt_free_result(stmt.stmt) != 0) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }
    }
    return MYLITE_OK;
}

int refresh_dirty_result_binds(mylite_stmt &stmt) {
    if (!stmt.result_binds_dirty) {
        return MYLITE_OK;
    }

    if (mysql_stmt_bind_result(stmt.stmt, stmt.result_binds.data()) != 0) {
        set_mariadb_statement_error(stmt);
        return MYLITE_ERROR;
    }
    for (ResultColumn &column : stmt.columns) {
        std::vector<unsigned char>().swap(column.retired_buffer);
    }
    stmt.result_binds_dirty = false;
    return MYLITE_OK;
}

int fetch_truncated_statement_columns(mylite_stmt &stmt) {
    for (unsigned index = 0; index < stmt.columns.size(); ++index) {
        ResultColumn &column = stmt.columns[index];
        if (column.is_null != 0 || column.error == 0) {
            continue;
        }
        if (column.length == ULONG_MAX) {
            set_error(*stmt.db, MYLITE_ERROR, "result column is too large");
            return MYLITE_ERROR;
        }

        const unsigned long buffer_length = std::max(column.length, 1UL);
        std::vector<unsigned char> buffer;
        if (allocate_column_buffer(buffer, buffer_length) != MYLITE_OK) {
            set_error(*stmt.db, MYLITE_NOMEM, "result column buffer could not be allocated");
            return MYLITE_NOMEM;
        }

        MYSQL_BIND fetch_bind = column.bind;
        fetch_bind.buffer = buffer.data();
        fetch_bind.buffer_length = buffer_length;
        if (mysql_stmt_fetch_column(stmt.stmt, &fetch_bind, index, 0) != 0) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }

        column.retired_buffer = std::move(column.buffer);
        column.buffer = std::move(buffer);
        column.bind = fetch_bind;
        column.bind.buffer = column.buffer.data();
        stmt.result_binds[index] = column.bind;
        stmt.result_binds_dirty = true;
    }
    return MYLITE_OK;
}

int configure_column_buffer(ResultColumn &column, unsigned long buffer_length) {
    const int result = allocate_column_buffer(column.buffer, buffer_length);
    if (result != MYLITE_OK) {
        return result;
    }
    column.bind.buffer = column.buffer.data();
    column.bind.buffer_length = buffer_length;
    return MYLITE_OK;
}

int allocate_column_buffer(std::vector<unsigned char> &buffer, unsigned long buffer_length) {
    if (buffer_length == ULONG_MAX) {
        return MYLITE_NOMEM;
    }

    try {
        buffer.assign(buffer_length + 1UL, 0U);
    } catch (const std::bad_alloc &) {
        return MYLITE_NOMEM;
    }
    return MYLITE_OK;
}

void release_statement_results(mylite_stmt &stmt) {
    clear_statement_ownerless_runtime_activity(stmt);
    if (stmt.stmt != nullptr && stmt.has_result) {
        static_cast<void>(mysql_stmt_free_result(stmt.stmt));
    }
    if (stmt.metadata != nullptr) {
        mysql_free_result(stmt.metadata);
        stmt.metadata = nullptr;
    }
    stmt.columns.clear();
    stmt.result_binds.clear();
    stmt.result_binds_dirty = false;
    stmt.has_result = false;
    stmt.has_row = false;
}

void clear_statement_ownerless_runtime_activity(mylite_stmt &stmt) {
    if (!stmt.ownerless_runtime_statement_active || stmt.db == nullptr) {
        return;
    }
    end_ownerless_runtime_statement(*stmt.db);
    stmt.ownerless_runtime_statement_active = false;
}

void enable_statement_ownerless_page_visibility(mylite_stmt &stmt, bool enabled) {
    if (!enabled || stmt.ownerless_page_visibility_enabled) {
        return;
    }
    stmt.ownerless_page_visibility_enabled = true;
    if (stmt.db != nullptr) {
        ++stmt.db->ownerless_active_page_visibility_statement_count;
    }
}

void clear_statement_ownerless_page_visibility(mylite_stmt &stmt) {
    if (!stmt.ownerless_page_visibility_enabled) {
        return;
    }
    stmt.ownerless_page_visibility_enabled = false;
    if (stmt.db != nullptr && stmt.db->ownerless_active_page_visibility_statement_count > 0U) {
        --stmt.db->ownerless_active_page_visibility_statement_count;
    }
    if (stmt.db == nullptr) {
        mylite_ownerless_innodb_clear_external_page_visibility();
    } else if (stmt.db->ownerless_active_page_visibility_statement_count == 0U) {
        release_ownerless_completed_statement_page_visibility(
            *stmt.db,
            !ownerless_connection_is_in_explicit_transaction(*stmt.db)
        );
    }
}

int prepare_ownerless_replacement_native_statement(
    mylite_stmt &stmt,
    const char *sql,
    std::size_t sql_len,
    bool validate_parameter_count,
    MYSQL_STMT **out_stmt
) {
    if (stmt.db == nullptr || sql == nullptr || out_stmt == nullptr || sql_len > ULONG_MAX) {
        return MYLITE_MISUSE;
    }

    *out_stmt = nullptr;
    MYSQL_STMT *replacement = mysql_stmt_init(&stmt.db->mysql);
    if (replacement == nullptr) {
        set_error(*stmt.db, MYLITE_NOMEM, "statement could not be allocated");
        return MYLITE_NOMEM;
    }

    my_bool update_max_length = 1;
    static_cast<void>(
        mysql_stmt_attr_set(replacement, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length)
    );

    if (mysql_stmt_prepare(replacement, sql, static_cast<unsigned long>(sql_len)) != 0) {
        set_mariadb_statement_error(*stmt.db, replacement);
        static_cast<void>(mysql_stmt_close(replacement));
        return MYLITE_ERROR;
    }
    if (validate_parameter_count && mysql_stmt_param_count(replacement) != stmt.parameters.size()) {
        static_cast<void>(mysql_stmt_close(replacement));
        set_error(
            *stmt.db,
            MYLITE_ERROR,
            "ownerless prepared statement parameter metadata changed during retry"
        );
        return MYLITE_ERROR;
    }

    *out_stmt = replacement;
    return MYLITE_OK;
}

int retry_ownerless_prepare_after_stale_engine_error(
    mylite_stmt &stmt,
    const char *sql,
    std::size_t sql_len
) {
    if (stmt.db == nullptr || stmt.stmt == nullptr || !stmt.ownerless_policy_tokens_valid) {
        return MYLITE_ERROR;
    }
    if (!ownerless_stale_engine_error_allows_retry(
            *stmt.db,
            stmt.ownerless_policy_tokens,
            ownerless_connection_is_in_explicit_transaction(*stmt.db)
        )) {
        return MYLITE_ERROR;
    }

    const int refresh_result =
        refresh_ownerless_dictionary_cache_after_stale_engine_error(*stmt.db);
    if (refresh_result != MYLITE_OK) {
        return refresh_result;
    }

    MYSQL_STMT *replacement = nullptr;
    const int prepare_result =
        prepare_ownerless_replacement_native_statement(stmt, sql, sql_len, false, &replacement);
    if (prepare_result != MYLITE_OK) {
        return prepare_result;
    }

    static_cast<void>(mysql_stmt_close(stmt.stmt));
    stmt.stmt = replacement;
    set_ok(*stmt.db);
    return MYLITE_OK;
}

int retry_ownerless_prepared_execute_after_stale_engine_error(mylite_stmt &stmt) {
    if (stmt.db == nullptr || stmt.stmt == nullptr || stmt.ownerless_sql_text == nullptr ||
        stmt.ownerless_native_prepare_per_step) {
        return MYLITE_ERROR;
    }

    const int refresh_result =
        refresh_ownerless_dictionary_cache_after_stale_engine_error(*stmt.db);
    if (refresh_result != MYLITE_OK) {
        return refresh_result;
    }

    MYSQL_STMT *replacement = nullptr;
    const std::string &sql = *stmt.ownerless_sql_text;
    const int prepare_result = prepare_ownerless_replacement_native_statement(
        stmt,
        sql.c_str(),
        sql.size(),
        true,
        &replacement
    );
    if (prepare_result != MYLITE_OK) {
        return prepare_result;
    }

    release_statement_results(stmt);
    static_cast<void>(mysql_stmt_close(stmt.stmt));
    stmt.stmt = replacement;

    const int bind_result = bind_parameters(stmt);
    if (bind_result != MYLITE_OK) {
        return bind_result;
    }
    return initialize_statement_results(stmt, true);
}

ParameterBinding *parameter_at(mylite_stmt &stmt, unsigned index) {
    if (index == 0U || index > stmt.parameters.size()) {
        return nullptr;
    }
    return &stmt.parameters[index - 1U];
}

int bind_null_value(mylite_stmt &stmt, unsigned index) {
    ParameterBinding *parameter = parameter_at(stmt, index);
    if (parameter == nullptr || stmt.executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->length = 0;
    parameter->is_null = 1;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_NULL;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    return MYLITE_OK;
}

int bind_bytes(
    mylite_stmt &stmt,
    unsigned index,
    const void *value,
    std::size_t value_len,
    enum enum_field_types buffer_type,
    mylite_destructor destructor
) {
    ParameterBinding *parameter = parameter_at(stmt, index);
    if (parameter == nullptr || stmt.executed || value_len > ULONG_MAX) {
        return MYLITE_MISUSE;
    }

    const auto *bytes = static_cast<const unsigned char *>(value);
    parameter->bytes.clear();
    if (value_len > 0U) {
        parameter->bytes.assign(bytes, bytes + value_len);
    }
    if (parameter->bytes.empty()) {
        parameter->bytes.push_back(0U);
    }
    parameter->length = static_cast<unsigned long>(value_len);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = buffer_type;
    bind_parameter_buffer(*parameter);

    // MYLITE_TRANSIENT mirrors SQLite's public -1 destructor sentinel.
    // NOLINTBEGIN(performance-no-int-to-ptr)
    if (value != nullptr && destructor != MYLITE_STATIC && destructor != MYLITE_TRANSIENT) {
        destructor(const_cast<void *>(value));
    }
    // NOLINTEND(performance-no-int-to-ptr)
    return MYLITE_OK;
}

void bind_parameter_buffer(ParameterBinding &parameter) {
    parameter.bind.buffer =
        parameter.bytes.empty() ? nullptr : static_cast<void *>(parameter.bytes.data());
    parameter.bind.buffer_length = parameter.length;
    parameter.bind.length = &parameter.length;
    parameter.bind.is_null = &parameter.is_null;
    parameter.bind.error = &parameter.error;
}

int prepare_ownerless_ephemeral_native_statement(mylite_stmt &stmt) {
    if (!stmt.ownerless_native_prepare_per_step) {
        return MYLITE_OK;
    }
    if (stmt.stmt != nullptr) {
        return MYLITE_OK;
    }
    if (stmt.db == nullptr || stmt.ownerless_sql_text == nullptr) {
        return MYLITE_MISUSE;
    }

    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_PREPARE_CALLS, 1U);
    const std::uint64_t native_prepare_start =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;

    stmt.stmt = mysql_stmt_init(&stmt.db->mysql);
    if (stmt.stmt == nullptr) {
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_PREPARE_NS,
            native_prepare_start
        );
        set_error(*stmt.db, MYLITE_NOMEM, "statement could not be allocated");
        return MYLITE_NOMEM;
    }

    my_bool update_max_length = 1;
    static_cast<void>(
        mysql_stmt_attr_set(stmt.stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length)
    );

    const std::string &sql = *stmt.ownerless_sql_text;
    if (mysql_stmt_prepare(stmt.stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_PREPARE_NS,
            native_prepare_start
        );
        set_mariadb_statement_error(stmt);
        close_ownerless_ephemeral_native_statement(stmt);
        return MYLITE_ERROR;
    }

    if (mysql_stmt_param_count(stmt.stmt) != stmt.parameters.size()) {
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_PREPARE_NS,
            native_prepare_start
        );
        close_ownerless_ephemeral_native_statement(stmt);
        set_error(
            *stmt.db,
            MYLITE_ERROR,
            "ownerless prepared statement parameter metadata changed during execution"
        );
        return MYLITE_ERROR;
    }
    if (mysql_stmt_field_count(stmt.stmt) != 0U) {
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_PREPARE_NS,
            native_prepare_start
        );
        close_ownerless_ephemeral_native_statement(stmt);
        set_error(
            *stmt.db,
            MYLITE_ERROR,
            "ownerless ephemeral prepared write unexpectedly returned result metadata"
        );
        return MYLITE_ERROR;
    }

    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_PREPARE_NS,
        native_prepare_start
    );
    return MYLITE_OK;
}

void close_ownerless_ephemeral_native_statement(mylite_stmt &stmt) {
    if (stmt.ownerless_native_prepare_per_step && stmt.stmt != nullptr) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_CLOSE_CALLS, 1U);
        const std::uint64_t native_close_start =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        static_cast<void>(mysql_stmt_close(stmt.stmt));
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PREPARED_STEP_NATIVE_CLOSE_NS,
            native_close_start
        );
        stmt.stmt = nullptr;
    }
}

int build_ownerless_prepared_text_sql(mylite_stmt &stmt, std::string &out_sql) {
    if (stmt.db == nullptr || stmt.ownerless_sql_text == nullptr) {
        return MYLITE_MISUSE;
    }

    try {
        const std::string_view sql(*stmt.ownerless_sql_text);
        out_sql.clear();
        out_sql.reserve(sql.size() + (stmt.parameters.size() * 16U));

        std::size_t offset = 0;
        std::size_t append_offset = 0;
        std::size_t parameter_index = 0;
        std::string_view token;
        while (next_sql_token(sql, offset, token)) {
            if (token.size() != 1U || token[0] != '?') {
                continue;
            }
            if (parameter_index >= stmt.parameters.size()) {
                set_error(
                    *stmt.db,
                    MYLITE_ERROR,
                    "ownerless prepared statement has too many parameter markers"
                );
                return MYLITE_ERROR;
            }
            const std::size_t token_start = static_cast<std::size_t>(token.data() - sql.data());
            out_sql.append(sql.data() + append_offset, token_start - append_offset);
            const int append_result = append_ownerless_prepared_parameter_sql(
                stmt,
                stmt.parameters[parameter_index],
                out_sql
            );
            if (append_result != MYLITE_OK) {
                return append_result;
            }
            append_offset = token_start + token.size();
            ++parameter_index;
        }

        if (parameter_index != stmt.parameters.size()) {
            set_error(
                *stmt.db,
                MYLITE_ERROR,
                "ownerless prepared statement has too few parameter markers"
            );
            return MYLITE_ERROR;
        }
        out_sql.append(sql.data() + append_offset, sql.size() - append_offset);
        if (out_sql.size() > ULONG_MAX) {
            set_error(*stmt.db, MYLITE_MISUSE, "ownerless prepared SQL is too large");
            return MYLITE_MISUSE;
        }
        return MYLITE_OK;
    } catch (const std::bad_alloc &) {
        set_error(*stmt.db, MYLITE_NOMEM, "ownerless prepared statement SQL could not be built");
        return MYLITE_NOMEM;
    }
}

int append_ownerless_prepared_parameter_sql(
    mylite_stmt &stmt,
    const ParameterBinding &parameter,
    std::string &out_sql
) {
    if (stmt.db == nullptr) {
        return MYLITE_MISUSE;
    }
    if (parameter.is_null != 0 || parameter.bind.buffer_type == MYSQL_TYPE_NULL) {
        out_sql.append("NULL");
        return MYLITE_OK;
    }

    switch (parameter.bind.buffer_type) {
    case MYSQL_TYPE_LONGLONG:
        if (parameter.bind.is_unsigned != 0) {
            out_sql.append(std::to_string(parameter.uint64_value));
        } else {
            out_sql.append(std::to_string(parameter.int64_value));
        }
        return MYLITE_OK;
    case MYSQL_TYPE_DOUBLE: {
        if (!std::isfinite(parameter.double_value)) {
            set_error(
                *stmt.db,
                MYLITE_MISUSE,
                "ownerless prepared text execution does not support non-finite double values"
            );
            return MYLITE_MISUSE;
        }
        std::array<char, 64> buffer = {};
        const int length =
            std::snprintf(buffer.data(), buffer.size(), "%.17g", parameter.double_value);
        if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size()) {
            set_error(*stmt.db, MYLITE_ERROR, "ownerless prepared double value could not be built");
            return MYLITE_ERROR;
        }
        out_sql.append(buffer.data(), static_cast<std::size_t>(length));
        return MYLITE_OK;
    }
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
        return append_ownerless_prepared_text_literal(stmt, parameter, out_sql);
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
        return append_ownerless_prepared_blob_literal(parameter, out_sql);
    default:
        set_error(
            *stmt.db,
            MYLITE_ERROR,
            "ownerless prepared text execution does not support this parameter type"
        );
        return MYLITE_ERROR;
    }
}

int append_ownerless_prepared_text_literal(
    mylite_stmt &stmt,
    const ParameterBinding &parameter,
    std::string &out_sql
) {
    if (stmt.db == nullptr) {
        return MYLITE_MISUSE;
    }
    const std::size_t input_length = static_cast<std::size_t>(parameter.length);
    if (input_length > (std::numeric_limits<std::size_t>::max() - 1U) / 2U) {
        set_error(*stmt.db, MYLITE_NOMEM, "ownerless prepared text value is too large");
        return MYLITE_NOMEM;
    }

    std::vector<char> escaped((input_length * 2U) + 1U);
    const char empty_source = '\0';
    const char *source = parameter.bytes.empty()
                             ? &empty_source
                             : reinterpret_cast<const char *>(parameter.bytes.data());
    const unsigned long escaped_length =
        mysql_real_escape_string(&stmt.db->mysql, escaped.data(), source, parameter.length);
    out_sql.push_back('\'');
    out_sql.append(escaped.data(), static_cast<std::size_t>(escaped_length));
    out_sql.push_back('\'');
    return MYLITE_OK;
}

int append_ownerless_prepared_blob_literal(
    const ParameterBinding &parameter,
    std::string &out_sql
) {
    static constexpr char k_hex_digits[] = "0123456789ABCDEF";
    out_sql.append("X'");
    for (std::size_t index = 0; index < static_cast<std::size_t>(parameter.length); ++index) {
        const unsigned char value = parameter.bytes[index];
        out_sql.push_back(k_hex_digits[value >> 4U]);
        out_sql.push_back(k_hex_digits[value & 0x0FU]);
    }
    out_sql.push_back('\'');
    return MYLITE_OK;
}

int bind_parameters(mylite_stmt &stmt) {
    if (stmt.parameters.empty()) {
        return MYLITE_OK;
    }
    stmt.parameter_binds.resize(stmt.parameters.size());
    for (std::size_t index = 0; index < stmt.parameters.size(); ++index) {
        stmt.parameter_binds[index] = stmt.parameters[index].bind;
    }
    if (mysql_stmt_bind_param(stmt.stmt, stmt.parameter_binds.data()) != 0) {
        set_mariadb_statement_error(stmt);
        return MYLITE_ERROR;
    }
    return MYLITE_OK;
}

mylite_value_type column_type(const ResultColumn &column) {
    if (column.is_null != 0) {
        return MYLITE_TYPE_NULL;
    }

    switch (column.field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
        return (column.flags & UNSIGNED_FLAG) != 0U ? MYLITE_TYPE_UINT64 : MYLITE_TYPE_INT64;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
        return MYLITE_TYPE_DOUBLE;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
        return (column.flags & BINARY_FLAG) != 0U ? MYLITE_TYPE_BLOB : MYLITE_TYPE_TEXT;
    default:
        return MYLITE_TYPE_TEXT;
    }
}

const ResultColumn *metadata_column_at(const mylite_stmt *stmt, unsigned column) {
    if (stmt == nullptr || column >= stmt->columns.size()) {
        return nullptr;
    }
    return &stmt->columns[column];
}

const ResultColumn *value_column_at(const mylite_stmt *stmt, unsigned column) {
    if (stmt == nullptr || !stmt->has_row || column >= stmt->columns.size()) {
        return nullptr;
    }
    return &stmt->columns[column];
}

int prepare_database_directory(const std::filesystem::path &database_path, unsigned flags) {
    if (is_memory_database_path(database_path)) {
        return MYLITE_OK;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(database_path, error);
    if (error) {
        return MYLITE_IOERR;
    }

    if ((flags & MYLITE_OPEN_EXCLUSIVE) != 0U && exists) {
        return MYLITE_ERROR;
    }

    if (!exists && (flags & MYLITE_OPEN_CREATE) == 0U) {
        return MYLITE_NOTFOUND;
    }

    if (exists) {
        const bool is_directory = std::filesystem::is_directory(database_path, error);
        if (error || !is_directory) {
            return MYLITE_IOERR;
        }
        return prepare_existing_database_directory(database_path, flags);
    }

    std::filesystem::create_directories(database_path, error);
    if (error) {
        return MYLITE_IOERR;
    }
    initialize_database_layout(database_path);

    return MYLITE_OK;
}

int prepare_existing_database_directory(
    const std::filesystem::path &database_path,
    unsigned flags
) {
    const std::filesystem::path metadata_path = database_path / k_meta_filename;
    std::error_code error;
    const bool metadata_exists = std::filesystem::exists(metadata_path, error);
    if (error) {
        return MYLITE_IOERR;
    }

    if (!metadata_exists) {
        const bool empty = database_directory_is_empty(database_path, error);
        if (error) {
            return MYLITE_IOERR;
        }
        if (empty && (flags & MYLITE_OPEN_CREATE) != 0U) {
            initialize_database_layout(database_path);
            return MYLITE_OK;
        }
        return empty ? MYLITE_NOTFOUND : MYLITE_CORRUPT;
    }

    const int layout_result = validate_database_layout(database_path);
    if (layout_result != MYLITE_OK) {
        return layout_result;
    }
    return MYLITE_OK;
}

int validate_database_layout(const std::filesystem::path &database_path) {
    const std::filesystem::path metadata_path = database_path / k_meta_filename;
    std::error_code error;

    const bool metadata_is_file = std::filesystem::is_regular_file(metadata_path, error);
    if (error || !metadata_is_file) {
        return error ? MYLITE_IOERR : MYLITE_CORRUPT;
    }

    const int metadata_result = validate_database_metadata(metadata_path);
    if (metadata_result != MYLITE_OK) {
        return metadata_result;
    }

    const int data_result = validate_layout_directory(database_path / k_datadir_name);
    if (data_result != MYLITE_OK) {
        return data_result;
    }

    const int tmp_result = validate_layout_directory(database_path / k_tmpdir_name);
    if (tmp_result != MYLITE_OK) {
        return tmp_result;
    }

    return MYLITE_OK;
}

int validate_layout_directory(const std::filesystem::path &directory) {
    std::error_code error;
    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        return MYLITE_IOERR;
    }
    if (!exists) {
        return MYLITE_CORRUPT;
    }

    const bool is_directory = std::filesystem::is_directory(directory, error);
    if (error) {
        return MYLITE_IOERR;
    }
    return is_directory ? MYLITE_OK : MYLITE_CORRUPT;
}

int validate_database_metadata(const std::filesystem::path &metadata_path) {
    std::ifstream metadata(metadata_path, std::ios::binary);
    if (!metadata) {
        return MYLITE_IOERR;
    }

    bool has_format = false;
    bool has_mariadb_base = false;
    const std::string mariadb_base_line = std::string("mariadb_base=") + k_mariadb_base_ref;
    for (std::string line; std::getline(metadata, line);) {
        if (line == k_metadata_format_line) {
            has_format = true;
            continue;
        }
        if (line == mariadb_base_line) {
            has_mariadb_base = true;
        }
    }
    if (!metadata.eof()) {
        return MYLITE_IOERR;
    }

    return has_format && has_mariadb_base ? MYLITE_OK : MYLITE_CORRUPT;
}

bool ownerless_concurrency_runtime_files_exist(const std::filesystem::path &database_path) {
    const std::filesystem::path concurrency_directory = database_path / k_concurrency_dir_name;
    std::error_code error;
    const bool directory_exists = std::filesystem::exists(concurrency_directory, error);
    if (error) {
        return true;
    }
    if (!directory_exists) {
        return false;
    }
    if (!std::filesystem::is_directory(concurrency_directory, error) || error) {
        return true;
    }

    static constexpr std::array<const char *, 5> k_runtime_files = {
        k_concurrency_meta_filename,
        k_concurrency_shm_filename,
        k_concurrency_wal_filename,
        k_concurrency_checkpoint_filename,
        k_concurrency_redo_header_filename,
    };
    for (const char *filename : k_runtime_files) {
        error.clear();
        if (std::filesystem::exists(concurrency_directory / filename, error)) {
            return true;
        }
        if (error) {
            return true;
        }
    }
    return false;
}

int prepare_concurrency_metadata(const std::filesystem::path &database_path) {
    const std::filesystem::path concurrency_directory = database_path / k_concurrency_dir_name;
    const std::filesystem::path metadata_path = concurrency_directory / k_concurrency_meta_filename;
    const std::filesystem::path lock_path = concurrency_directory / k_concurrency_lock_filename;
    std::error_code error;

    std::filesystem::create_directories(concurrency_directory, error);
    if (error) {
        return MYLITE_IOERR;
    }

    const int lock_fd = acquire_concurrency_lock(
        lock_path,
        k_persisted_config_lock_start,
        k_persisted_config_lock_length
    );
    if (lock_fd < 0) {
        return MYLITE_IOERR;
    }

    const bool metadata_exists = std::filesystem::exists(metadata_path, error);
    if (error) {
        release_concurrency_lock(
            lock_fd,
            k_persisted_config_lock_start,
            k_persisted_config_lock_length
        );
        return MYLITE_IOERR;
    }
    if (!metadata_exists) {
        try {
            write_concurrency_metadata(metadata_path);
        } catch (...) {
            release_concurrency_lock(
                lock_fd,
                k_persisted_config_lock_start,
                k_persisted_config_lock_length
            );
            throw;
        }
        release_concurrency_lock(
            lock_fd,
            k_persisted_config_lock_start,
            k_persisted_config_lock_length
        );
        return MYLITE_OK;
    }

    const bool metadata_is_file = std::filesystem::is_regular_file(metadata_path, error);
    if (error || !metadata_is_file) {
        release_concurrency_lock(
            lock_fd,
            k_persisted_config_lock_start,
            k_persisted_config_lock_length
        );
        return error ? MYLITE_IOERR : MYLITE_CORRUPT;
    }
    const int metadata_result = validate_concurrency_metadata(metadata_path);
    release_concurrency_lock(
        lock_fd,
        k_persisted_config_lock_start,
        k_persisted_config_lock_length
    );
    return metadata_result;
}

int acquire_concurrency_lock(const std::filesystem::path &lock_path, off_t start, off_t length) {
    return acquire_concurrency_lock(lock_path, start, length, F_WRLCK);
}

int acquire_concurrency_lock(
    const std::filesystem::path &lock_path,
    off_t start,
    off_t length,
    short lock_type
) {
    return acquire_concurrency_lock(
        lock_path,
        start,
        length,
        lock_type,
        k_concurrency_lock_wait_timeout_ms
    );
}

int acquire_concurrency_lock(
    const std::filesystem::path &lock_path,
    off_t start,
    off_t length,
    short lock_type,
    unsigned timeout_ms
) {
    const std::string lock_name = lock_path.string();
    const int lock_fd = ::open(lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        return -1;
    }

    if (acquire_fd_range_lock(lock_fd, start, length, lock_type, timeout_ms)) {
        return lock_fd;
    }
    static_cast<void>(::close(lock_fd));
    return -1;
}

bool acquire_fd_range_lock(
    int fd,
    off_t start,
    off_t length,
    short lock_type,
    unsigned timeout_ms
) {
    if (fd < 0) {
        return false;
    }

    struct flock lock = {};
    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    unsigned poll_interval_ms = k_lock_poll_initial_interval_ms;
    for (;;) {
        if (::fcntl(fd, F_SETLK, &lock) == 0) {
            return true;
        }
        if (errno != EACCES && errno != EAGAIN) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
        poll_interval_ms = std::min(poll_interval_ms * 2U, k_lock_poll_max_interval_ms);
    }
}

void release_concurrency_lock(int lock_fd, off_t start, off_t length) {
    if (lock_fd < 0) {
        return;
    }

    struct flock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    static_cast<void>(::fcntl(lock_fd, F_SETLK, &lock));
    static_cast<void>(::close(lock_fd));
}

int prepare_concurrency_shared_memory(
    const std::filesystem::path &database_path,
    bool allow_recovery_rebuild
) {
    const std::filesystem::path concurrency_directory = database_path / k_concurrency_dir_name;
    const std::filesystem::path metadata_path = concurrency_directory / k_concurrency_meta_filename;
    const std::filesystem::path lock_path = concurrency_directory / k_concurrency_lock_filename;
    const std::filesystem::path shm_path = concurrency_directory / k_concurrency_shm_filename;
    const std::filesystem::path wal_path = concurrency_directory / k_concurrency_wal_filename;
    const std::filesystem::path checkpoint_path =
        concurrency_directory / k_concurrency_checkpoint_filename;
    std::string database_uuid;
    const int uuid_result = read_concurrency_database_uuid(metadata_path, database_uuid);
    if (uuid_result != MYLITE_OK) {
        return uuid_result;
    }

    const int recovery_lock_fd =
        acquire_concurrency_lock(lock_path, k_recovery_lock_start, k_recovery_lock_length);
    if (recovery_lock_fd < 0) {
        return MYLITE_IOERR;
    }

    const int recovery_files_result =
        prepare_concurrency_recovery_files(concurrency_directory, database_uuid);
    if (recovery_files_result != MYLITE_OK) {
        release_concurrency_lock(recovery_lock_fd, k_recovery_lock_start, k_recovery_lock_length);
        return recovery_files_result;
    }

    const int resize_lock_fd =
        acquire_concurrency_lock(lock_path, k_shm_resize_lock_start, k_shm_resize_lock_length);
    if (resize_lock_fd < 0) {
        release_concurrency_lock(recovery_lock_fd, k_recovery_lock_start, k_recovery_lock_length);
        return MYLITE_IOERR;
    }
    const auto release_layout_locks = [&]() {
        release_concurrency_lock(resize_lock_fd, k_shm_resize_lock_start, k_shm_resize_lock_length);
        release_concurrency_lock(recovery_lock_fd, k_recovery_lock_start, k_recovery_lock_length);
    };

    const std::string shm_name = shm_path.string();
    const int shm_fd = ::open(shm_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (shm_fd < 0) {
        release_layout_locks();
        return MYLITE_IOERR;
    }
    const std::string wal_name = wal_path.string();
    const int wal_fd = ::open(wal_name.c_str(), O_RDWR | O_CLOEXEC);
    if (wal_fd < 0) {
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return MYLITE_IOERR;
    }
    const std::string checkpoint_name = checkpoint_path.string();
    const int checkpoint_fd = ::open(checkpoint_name.c_str(), O_RDWR | O_CLOEXEC);
    if (checkpoint_fd < 0) {
        static_cast<void>(::close(wal_fd));
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return MYLITE_IOERR;
    }

    struct stat shm_stat = {};
    if (::fstat(shm_fd, &shm_stat) != 0) {
        static_cast<void>(::close(checkpoint_fd));
        static_cast<void>(::close(wal_fd));
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return MYLITE_IOERR;
    }
    const bool initial_shared_memory = shm_stat.st_size == 0;
    if (shm_stat.st_size < k_minimum_concurrency_shm_size &&
        ::ftruncate(shm_fd, k_minimum_concurrency_shm_size) != 0) {
        static_cast<void>(::close(checkpoint_fd));
        static_cast<void>(::close(wal_fd));
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return MYLITE_IOERR;
    }
    const off_t shm_size =
        std::max(shm_stat.st_size, static_cast<off_t>(k_minimum_concurrency_shm_size));
    const ConcurrencyShmFileIdentity shm_identity = concurrency_shm_file_identity(shm_stat);
    const int layout_result = prepare_concurrency_shm_layout(
        database_path,
        shm_fd,
        wal_fd,
        checkpoint_fd,
        shm_size,
        shm_identity,
        database_uuid,
        allow_recovery_rebuild,
        initial_shared_memory
    );
    if (layout_result != MYLITE_OK) {
        static_cast<void>(::close(checkpoint_fd));
        static_cast<void>(::close(wal_fd));
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return layout_result;
    }

    static_cast<void>(::close(checkpoint_fd));
    static_cast<void>(::close(wal_fd));
    static_cast<void>(::close(shm_fd));
    release_layout_locks();
    return MYLITE_OK;
}

int read_concurrency_database_uuid(
    const std::filesystem::path &metadata_path,
    std::string &database_uuid
) {
    std::ifstream metadata(metadata_path, std::ios::binary);
    if (!metadata) {
        return MYLITE_IOERR;
    }

    for (std::string line; std::getline(metadata, line);) {
        if (line.rfind("database_uuid=", 0) != 0) {
            continue;
        }
        const std::string_view uuid = std::string_view(line).substr(14U);
        if (!is_database_uuid(uuid)) {
            return MYLITE_CORRUPT;
        }
        database_uuid.assign(uuid);
        return MYLITE_OK;
    }
    if (!metadata.eof()) {
        return MYLITE_IOERR;
    }
    return MYLITE_CORRUPT;
}

int prepare_concurrency_recovery_files(
    const std::filesystem::path &concurrency_directory,
    std::string_view database_uuid
) {
    const int wal_result = prepare_concurrency_recovery_file(
        concurrency_directory / k_concurrency_wal_filename,
        k_concurrency_wal_magic,
        database_uuid
    );
    if (wal_result != MYLITE_OK) {
        return wal_result;
    }
    return prepare_concurrency_checkpoint_file(
        concurrency_directory / k_concurrency_checkpoint_filename,
        database_uuid
    );
}

int prepare_concurrency_checkpoint_file(
    const std::filesystem::path &file_path,
    std::string_view database_uuid
) {
    const int header_result =
        prepare_concurrency_recovery_file(file_path, k_concurrency_checkpoint_magic, database_uuid);
    if (header_result != MYLITE_OK) {
        return header_result;
    }

    const std::string file_name = file_path.string();
    const int file_fd = ::open(file_name.c_str(), O_RDWR | O_CLOEXEC);
    if (file_fd < 0) {
        return MYLITE_IOERR;
    }
    struct stat file_stat = {};
    if (::fstat(file_fd, &file_stat) != 0) {
        static_cast<void>(::close(file_fd));
        return MYLITE_IOERR;
    }
    const bool checkpoint_file_needs_resize = file_stat.st_size < k_concurrency_checkpoint_file_end;
    if (checkpoint_file_needs_resize) {
        if (::ftruncate(file_fd, k_concurrency_checkpoint_file_end) != 0 || ::fsync(file_fd) != 0) {
            static_cast<void>(::close(file_fd));
            return MYLITE_IOERR;
        }
    }
    static_cast<void>(::close(file_fd));
    return MYLITE_OK;
}

int prepare_concurrency_recovery_file(
    const std::filesystem::path &file_path,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
) {
    const std::string file_name = file_path.string();
    const int file_fd = ::open(file_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (file_fd < 0) {
        return MYLITE_IOERR;
    }

    struct stat file_stat = {};
    if (::fstat(file_fd, &file_stat) != 0) {
        static_cast<void>(::close(file_fd));
        return MYLITE_IOERR;
    }
    if (file_stat.st_size < static_cast<off_t>(k_concurrency_recovery_header_size) &&
        ::ftruncate(file_fd, static_cast<off_t>(k_concurrency_recovery_header_size)) != 0) {
        static_cast<void>(::close(file_fd));
        return MYLITE_IOERR;
    }

    std::array<unsigned char, k_concurrency_recovery_header_size> header = {};
    if (!read_exact_at(file_fd, header.data(), header.size(), 0)) {
        static_cast<void>(::close(file_fd));
        return MYLITE_IOERR;
    }
    if (!concurrency_recovery_header_matches(header, magic, database_uuid)) {
        build_concurrency_recovery_header(header, magic, database_uuid);
        if (!write_exact_at(file_fd, header.data(), header.size(), 0) || ::fsync(file_fd) != 0) {
            static_cast<void>(::close(file_fd));
            return MYLITE_IOERR;
        }
    }

    static_cast<void>(::close(file_fd));
    return MYLITE_OK;
}

bool concurrency_recovery_header_matches(
    const std::array<unsigned char, k_concurrency_recovery_header_size> &header,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
) {
    if (database_uuid.size() != k_database_uuid_size) {
        return false;
    }
    if (std::memcmp(
            header.data() + k_concurrency_recovery_magic_offset,
            magic.data(),
            magic.size()
        ) != 0) {
        return false;
    }
    return load_le32(header.data(), k_concurrency_recovery_format_offset) ==
               k_concurrency_recovery_format_version &&
           load_le32(header.data(), k_concurrency_recovery_header_size_offset) ==
               k_concurrency_recovery_header_size &&
           load_le32(header.data(), k_concurrency_recovery_byte_order_offset) ==
               k_concurrency_shm_byte_order &&
           load_le32(header.data(), k_concurrency_recovery_flags_offset) == 0U &&
           load_le64(header.data(), k_concurrency_recovery_generation_offset) == 0U &&
           std::memcmp(
               header.data() + k_concurrency_recovery_database_uuid_offset,
               database_uuid.data(),
               database_uuid.size()
           ) == 0;
}

void build_concurrency_recovery_header(
    std::array<unsigned char, k_concurrency_recovery_header_size> &header,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
) {
    header.fill(0U);
    std::memcpy(header.data() + k_concurrency_recovery_magic_offset, magic.data(), magic.size());
    store_le32(
        header.data(),
        k_concurrency_recovery_format_offset,
        k_concurrency_recovery_format_version
    );
    store_le32(
        header.data(),
        k_concurrency_recovery_header_size_offset,
        static_cast<std::uint32_t>(k_concurrency_recovery_header_size)
    );
    store_le32(
        header.data(),
        k_concurrency_recovery_byte_order_offset,
        k_concurrency_shm_byte_order
    );
    store_le32(header.data(), k_concurrency_recovery_flags_offset, 0U);
    store_le64(header.data(), k_concurrency_recovery_generation_offset, 0U);
    std::memcpy(
        header.data() + k_concurrency_recovery_database_uuid_offset,
        database_uuid.data(),
        database_uuid.size()
    );
}

int prepare_concurrency_shm_layout(
    const std::filesystem::path &database_path,
    int shm_fd,
    int page_log_fd,
    int checkpoint_fd,
    off_t shm_size,
    const ConcurrencyShmFileIdentity &shm_identity,
    std::string_view database_uuid,
    bool allow_recovery_rebuild,
    bool initial_shared_memory
) {
    std::array<unsigned char, k_concurrency_shm_header_size> header = {};
    if (!read_exact_at(shm_fd, header.data(), header.size(), 0)) {
        return MYLITE_IOERR;
    }

    std::uint64_t recovery_generation = 0;
    const bool identity_matches = concurrency_shm_header_identity_matches(header, database_uuid);
    const bool file_identity_matches =
        identity_matches &&
        load_le64(header.data(), k_concurrency_shm_device_offset) == shm_identity.device &&
        load_le64(header.data(), k_concurrency_shm_inode_offset) == shm_identity.inode;
    const bool layout_matches =
        concurrency_shm_header_layout_matches(header, shm_size, shm_identity, database_uuid);
    bool increment_recovery_generation = false;
    if (identity_matches) {
        recovery_generation =
            load_le64(header.data(), k_concurrency_shm_recovery_generation_offset);
        if (!file_identity_matches) {
            increment_recovery_generation = true;
        }
        const std::uint32_t state = load_le32(header.data(), k_concurrency_shm_state_offset);
        if (state == k_concurrency_shm_state_rebuilding) {
            increment_recovery_generation = true;
        }
    }

    bool rebuild_segments = !layout_matches;
    bool stale_reader_rebuild = false;
    std::uint64_t preserved_next_trx_id = k_concurrency_initial_trx_id;
    const bool segments_match =
        !rebuild_segments && concurrency_shm_segments_match(shm_fd, shm_size);
    if (identity_matches) {
        preserved_next_trx_id =
            std::max(preserved_next_trx_id, read_concurrency_trx_next_id_from_shm(shm_fd));
    }
    if (!rebuild_segments && !segments_match) {
        std::uint64_t active_count = 0;
        const int active_count_result =
            read_concurrency_process_active_count(shm_fd, &active_count);
        if (active_count_result != MYLITE_OK) {
            return MYLITE_BUSY;
        }
        std::uint64_t live_count = active_count;
        if (active_count > 0U) {
            const int live_count_result = read_concurrency_process_live_count(shm_fd, &live_count);
            if (live_count_result != MYLITE_OK) {
                return MYLITE_BUSY;
            }
        }
        if (live_count > 0U) {
            return MYLITE_BUSY;
        }
        const std::uint32_t state = load_le32(header.data(), k_concurrency_shm_state_offset);
        stale_reader_rebuild =
            (state == k_concurrency_shm_state_clean || state == k_concurrency_shm_state_dirty) &&
            concurrency_shm_has_stale_reader_state_without_recovery(shm_fd, shm_size);
        increment_recovery_generation = true;
        rebuild_segments = true;
    }
    if (!rebuild_segments) {
        const std::uint32_t state = load_le32(header.data(), k_concurrency_shm_state_offset);
        if (state == k_concurrency_shm_state_clean || state == k_concurrency_shm_state_dirty) {
            std::uint64_t active_count = 0;
            const int active_count_result =
                read_concurrency_process_active_count(shm_fd, &active_count);
            if (active_count_result != MYLITE_OK) {
                return active_count_result;
            }
            std::uint64_t live_count = active_count;
            if (active_count > 0U) {
                const int live_count_result =
                    read_concurrency_process_live_count(shm_fd, &live_count);
                if (live_count_result != MYLITE_OK) {
                    return live_count_result;
                }
            }
            const bool no_live_processes = active_count == 0U || live_count == 0U;
            const bool dirty_shm_has_no_live_process =
                state == k_concurrency_shm_state_dirty && no_live_processes;
            const bool clean_shm_has_only_dead_processes =
                state == k_concurrency_shm_state_clean && active_count > 0U && live_count == 0U;
            if (dirty_shm_has_no_live_process) {
                increment_recovery_generation = true;
                rebuild_segments = true;
                stale_reader_rebuild =
                    concurrency_shm_has_stale_reader_state_without_recovery(shm_fd, shm_size);
            } else if (clean_shm_has_only_dead_processes) {
                increment_recovery_generation = true;
                rebuild_segments = true;
                stale_reader_rebuild = true;
            }
        } else if (state != k_concurrency_shm_state_clean) {
            rebuild_segments = true;
        }
    }

    if (rebuild_segments) {
        if (!allow_recovery_rebuild && !initial_shared_memory &&
            concurrency_shm_rebuild_requires_recovery(shm_fd, shm_size)) {
            return MYLITE_BUSY;
        }
        if (allow_recovery_rebuild) {
            if (stale_reader_rebuild) {
                const int discard_result = discard_stale_reader_page_log(page_log_fd);
                if (discard_result != MYLITE_OK) {
                    return discard_result;
                }
            } else {
                const int replay_result =
                    replay_concurrency_tablespaces(database_path, page_log_fd, checkpoint_fd);
                if (replay_result != MYLITE_OK) {
                    return replay_result;
                }
            }
        }
        if (increment_recovery_generation) {
            ++recovery_generation;
        }
        build_concurrency_shm_header(
            header,
            shm_size,
            shm_identity,
            database_uuid,
            recovery_generation
        );
        store_le32(
            header.data(),
            k_concurrency_shm_state_offset,
            k_concurrency_shm_state_rebuilding
        );
        if (!write_exact_at(shm_fd, header.data(), header.size(), 0)) {
            return MYLITE_IOERR;
        }
        const int segment_result = initialize_concurrency_shm_segments(
            shm_fd,
            page_log_fd,
            checkpoint_fd,
            preserved_next_trx_id
        );
        if (segment_result != MYLITE_OK) {
            return segment_result;
        }
        if (!update_concurrency_shm_state(shm_fd, k_concurrency_shm_state_clean)) {
            return MYLITE_IOERR;
        }
    }
    return validate_concurrency_shm_mapping(shm_fd, shm_size, database_uuid);
}

int replay_concurrency_tablespaces(
    const std::filesystem::path &database_path,
    int page_log_fd,
    int checkpoint_fd
) {
    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(checkpoint_fd, &latest_lsn, &visible_lsn)) {
        return MYLITE_IOERR;
    }
    if (visible_lsn == 0U) {
        return MYLITE_OK;
    }

    const std::filesystem::path datadir = database_path / k_datadir_name;
    const std::string datadir_name = datadir.string();
    const int replay_result = mylite_ownerless_tablespace_replay_apply_with_flags(
        datadir_name.c_str(),
        page_log_fd,
        k_concurrency_recovery_header_size,
        visible_lsn,
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES |
            MYLITE_OWNERLESS_TABLESPACE_REPLAY_KEEP_NATIVE_SAME_LSN
    );
    if (replay_result != MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK) {
        return MYLITE_IOERR;
    }

    /*
     * Keep complete page-version records after materializing them into native
     * tablespaces. Native InnoDB startup can still replay an older local redo
     * view before the exclusive runtime has reconciled its checkpoint, so the
     * page-version WAL remains the authoritative ownerless recovery view.
     */
    const int checkpoint_result = mylite_ownerless_page_log_checkpoint_at(
        page_log_fd,
        k_concurrency_recovery_header_size,
        0U,
        nullptr,
        nullptr
    );
    return checkpoint_result == MYLITE_OWNERLESS_PAGE_LOG_OK ? MYLITE_OK : MYLITE_IOERR;
}

int discard_stale_reader_page_log(int page_log_fd) {
    // The checkpoint primitive retains records above the safe LSN; max means
    // every complete retained reader-boundary record is safe to discard.
    const int checkpoint_result = mylite_ownerless_page_log_checkpoint_at(
        page_log_fd,
        k_concurrency_recovery_header_size,
        std::numeric_limits<std::uint64_t>::max(),
        nullptr,
        nullptr
    );
    return checkpoint_result == MYLITE_OWNERLESS_PAGE_LOG_OK ? MYLITE_OK : MYLITE_IOERR;
}

bool concurrency_shm_header_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    const ConcurrencyShmFileIdentity &shm_identity,
    std::string_view database_uuid
) {
    const std::uint32_t state = load_le32(header.data(), k_concurrency_shm_state_offset);
    return concurrency_shm_header_layout_matches(header, shm_size, shm_identity, database_uuid) &&
           (state == k_concurrency_shm_state_clean || state == k_concurrency_shm_state_dirty);
}

bool concurrency_shm_header_layout_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    const ConcurrencyShmFileIdentity &shm_identity,
    std::string_view database_uuid
) {
    if (shm_size < static_cast<off_t>(k_concurrency_shm_header_size) ||
        database_uuid.size() != k_database_uuid_size) {
        return false;
    }
    return concurrency_shm_header_identity_matches(header, database_uuid) &&
           load_le64(header.data(), k_concurrency_shm_mapping_size_offset) ==
               static_cast<std::uint64_t>(shm_size) &&
           load_le64(header.data(), k_concurrency_shm_generation_offset) == 0U &&
           load_le64(header.data(), k_concurrency_shm_device_offset) == shm_identity.device &&
           load_le64(header.data(), k_concurrency_shm_inode_offset) == shm_identity.inode &&
           load_le32(header.data(), k_concurrency_shm_segment_table_offset) ==
               k_concurrency_shm_segment_table_start &&
           load_le32(header.data(), k_concurrency_shm_segment_count_offset) ==
               k_concurrency_shm_segment_count;
}

bool concurrency_shm_segments_match(int shm_fd, off_t shm_size) {
    if (shm_size <
        static_cast<off_t>(
            k_concurrency_page_pin_registry_offset + k_concurrency_page_pin_registry_segment_size
        )) {
        return false;
    }

    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> process_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> wait_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> mdl_lock_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> trx_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> read_view_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> innodb_lock_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> redo_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> page_index_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> dictionary_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> page_write_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> autoinc_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> page_pin_segment = {};
    std::array<unsigned char, k_concurrency_process_registry_header_size> registry = {};
    std::array<unsigned char, k_concurrency_wait_channel_header_size> wait_channels = {};
    std::array<unsigned char, k_concurrency_mdl_lock_table_header_size> mdl_lock_table = {};
    std::array<unsigned char, k_concurrency_trx_registry_header_size> trx_registry = {};
    std::array<unsigned char, k_concurrency_read_view_registry_header_size> read_view_registry = {};
    std::array<unsigned char, k_concurrency_innodb_lock_registry_header_size> innodb_lock_registry =
        {};
    std::array<unsigned char, k_concurrency_innodb_lock_registry_header_size>
        page_write_lock_registry = {};
    std::array<unsigned char, MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE> autoinc_registry = {};
    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE> page_index = {};
    using PagePinRegistryHeader =
        std::array<unsigned char, MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_HEADER_SIZE>;
    PagePinRegistryHeader page_pin_registry = {};

    if (!read_exact_at(
            shm_fd,
            process_segment.data(),
            process_segment.size(),
            static_cast<off_t>(k_concurrency_shm_segment_table_start)
        ) ||
        !read_exact_at(
            shm_fd,
            wait_segment.data(),
            wait_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start + k_concurrency_shm_segment_descriptor_size
            )
        ) ||
        !read_exact_at(
            shm_fd,
            mdl_lock_segment.data(),
            mdl_lock_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (2U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            trx_segment.data(),
            trx_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (3U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            read_view_segment.data(),
            read_view_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (4U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            innodb_lock_segment.data(),
            innodb_lock_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (5U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            redo_segment.data(),
            redo_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (6U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            page_index_segment.data(),
            page_index_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (7U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            dictionary_segment.data(),
            dictionary_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (8U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            page_write_segment.data(),
            page_write_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (9U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            autoinc_segment.data(),
            autoinc_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (10U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            page_pin_segment.data(),
            page_pin_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (11U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            registry.data(),
            registry.size(),
            static_cast<off_t>(k_concurrency_process_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            wait_channels.data(),
            wait_channels.size(),
            static_cast<off_t>(k_concurrency_wait_channel_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            mdl_lock_table.data(),
            mdl_lock_table.size(),
            static_cast<off_t>(k_concurrency_mdl_lock_table_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            trx_registry.data(),
            trx_registry.size(),
            static_cast<off_t>(k_concurrency_trx_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            read_view_registry.data(),
            read_view_registry.size(),
            static_cast<off_t>(k_concurrency_read_view_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            innodb_lock_registry.data(),
            innodb_lock_registry.size(),
            static_cast<off_t>(k_concurrency_innodb_lock_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            page_index.data(),
            page_index.size(),
            static_cast<off_t>(k_concurrency_page_index_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            page_write_lock_registry.data(),
            page_write_lock_registry.size(),
            static_cast<off_t>(k_concurrency_page_write_lock_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            autoinc_registry.data(),
            autoinc_registry.size(),
            static_cast<off_t>(k_concurrency_autoinc_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            page_pin_registry.data(),
            page_pin_registry.size(),
            static_cast<off_t>(k_concurrency_page_pin_registry_offset)
        )) {
        return false;
    }

    const std::uint64_t process_active_count =
        load_le64(registry.data(), k_concurrency_registry_active_count_offset);
    const std::uint64_t mdl_lock_active_count =
        load_le64(mdl_lock_table.data(), k_concurrency_mdl_lock_header_active_count_offset);
    const std::uint64_t trx_active_count =
        load_le64(trx_registry.data(), k_concurrency_trx_header_active_count_offset);
    const std::uint64_t trx_next_id =
        load_le64(trx_registry.data(), k_concurrency_trx_header_next_id_offset);
    const std::uint64_t trx_oldest_active =
        load_le64(trx_registry.data(), k_concurrency_trx_header_oldest_active_offset);
    const std::uint64_t read_view_active_count =
        load_le64(read_view_registry.data(), k_concurrency_read_view_header_active_count_offset);
    const std::uint64_t innodb_lock_active_count = load_le64(
        innodb_lock_registry.data(),
        k_concurrency_innodb_lock_header_active_count_offset
    );
    const std::uint64_t innodb_lock_waiting_count = load_le64(
        innodb_lock_registry.data(),
        k_concurrency_innodb_lock_header_waiting_count_offset
    );
    const std::uint32_t innodb_lock_occupied_limit = load_le32(
        innodb_lock_registry.data(),
        k_concurrency_innodb_lock_header_occupied_limit_offset
    );
    const std::uint64_t page_write_lock_active_count = load_le64(
        page_write_lock_registry.data(),
        k_concurrency_innodb_lock_header_active_count_offset
    );
    const std::uint64_t page_write_lock_waiting_count = load_le64(
        page_write_lock_registry.data(),
        k_concurrency_innodb_lock_header_waiting_count_offset
    );
    const std::uint32_t page_write_lock_occupied_limit = load_le32(
        page_write_lock_registry.data(),
        k_concurrency_innodb_lock_header_occupied_limit_offset
    );
    const std::uint64_t page_index_active_count =
        load_le64(page_index.data(), k_concurrency_page_index_header_active_count_offset);
    const std::uint64_t page_pin_active_count =
        load_le64(page_pin_registry.data(), k_concurrency_page_pin_header_active_count_offset);

    return load_le32(process_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_process_registry_segment_type &&
           load_le32(process_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_process_registry_segment_version &&
           load_le64(process_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_process_registry_offset &&
           load_le64(process_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_process_registry_size &&
           load_le32(wait_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_wait_channel_segment_type &&
           load_le32(wait_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_wait_channel_segment_version &&
           load_le64(wait_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_wait_channel_offset &&
           load_le64(wait_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_wait_channel_segment_size &&
           load_le32(mdl_lock_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_mdl_lock_table_segment_type &&
           load_le32(mdl_lock_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_mdl_lock_table_segment_version &&
           load_le64(mdl_lock_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_mdl_lock_table_offset &&
           load_le64(mdl_lock_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_mdl_lock_table_segment_size &&
           load_le32(trx_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_trx_registry_segment_type &&
           load_le32(trx_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_trx_registry_segment_version &&
           load_le64(trx_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_trx_registry_offset &&
           load_le64(trx_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_trx_registry_segment_size &&
           load_le32(read_view_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_read_view_registry_segment_type &&
           load_le32(read_view_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_read_view_registry_segment_version &&
           load_le64(read_view_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_read_view_registry_offset &&
           load_le64(read_view_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_read_view_registry_segment_size &&
           load_le32(innodb_lock_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_innodb_lock_registry_segment_type &&
           load_le32(innodb_lock_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_innodb_lock_registry_segment_version &&
           load_le64(innodb_lock_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_innodb_lock_registry_offset &&
           load_le64(innodb_lock_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_innodb_lock_registry_segment_size &&
           load_le32(redo_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_redo_state_segment_type &&
           load_le32(redo_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_redo_state_segment_version &&
           load_le64(redo_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_redo_state_offset &&
           load_le64(redo_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_redo_state_segment_size &&
           load_le32(page_index_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_page_index_segment_type &&
           load_le32(page_index_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_page_index_segment_version &&
           load_le64(page_index_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_page_index_offset &&
           load_le64(page_index_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_page_index_segment_size &&
           load_le32(dictionary_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_dictionary_state_segment_type &&
           load_le32(dictionary_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_dictionary_state_segment_version &&
           load_le64(dictionary_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_dictionary_state_offset &&
           load_le64(dictionary_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_dictionary_state_segment_size &&
           load_le32(page_write_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_page_write_lock_registry_segment_type &&
           load_le32(page_write_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_page_write_lock_registry_segment_version &&
           load_le64(page_write_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_page_write_lock_registry_offset &&
           load_le64(page_write_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_page_write_lock_registry_segment_size &&
           load_le32(autoinc_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_autoinc_registry_segment_type &&
           load_le32(autoinc_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_autoinc_registry_segment_version &&
           load_le64(autoinc_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_autoinc_registry_offset &&
           load_le64(autoinc_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_autoinc_registry_segment_size &&
           load_le32(page_pin_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_page_pin_registry_segment_type &&
           load_le32(page_pin_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_page_pin_registry_segment_version &&
           load_le64(page_pin_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_page_pin_registry_offset &&
           load_le64(page_pin_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_page_pin_registry_segment_size &&
           load_le32(registry.data(), k_concurrency_registry_slot_count_offset) ==
               k_concurrency_process_slot_count &&
           load_le32(registry.data(), k_concurrency_registry_slot_size_offset) ==
               k_concurrency_process_slot_size &&
           process_active_count <= k_concurrency_process_slot_count &&
           load_le32(wait_channels.data(), k_concurrency_wait_header_channel_count_offset) ==
               k_concurrency_wait_channel_count &&
           load_le32(wait_channels.data(), k_concurrency_wait_header_channel_size_offset) ==
               k_concurrency_wait_channel_size &&
           load_le32(mdl_lock_table.data(), k_concurrency_mdl_lock_header_entry_count_offset) ==
               k_concurrency_mdl_lock_table_entry_count &&
           load_le32(mdl_lock_table.data(), k_concurrency_mdl_lock_header_entry_size_offset) ==
               k_concurrency_mdl_lock_table_entry_size &&
           mdl_lock_active_count <= k_concurrency_mdl_lock_table_entry_count &&
           load_le32(trx_registry.data(), k_concurrency_trx_header_slot_count_offset) ==
               k_concurrency_trx_slot_count &&
           load_le32(trx_registry.data(), k_concurrency_trx_header_slot_size_offset) ==
               k_concurrency_trx_slot_size &&
           trx_active_count <= k_concurrency_trx_slot_count &&
           trx_next_id >= k_concurrency_initial_trx_id &&
           (trx_active_count == 0U || process_active_count > 0U) &&
           ((trx_active_count == 0U && trx_oldest_active == 0U) ||
            (trx_active_count > 0U && trx_oldest_active >= k_concurrency_initial_trx_id &&
             trx_oldest_active < trx_next_id)) &&
           load_le32(read_view_registry.data(), k_concurrency_read_view_header_slot_count_offset) ==
               k_concurrency_read_view_slot_count &&
           load_le32(read_view_registry.data(), k_concurrency_read_view_header_slot_size_offset) ==
               k_concurrency_read_view_slot_size &&
           read_view_active_count <= k_concurrency_read_view_slot_count &&
           (read_view_active_count == 0U || process_active_count > 0U) &&
           load_le32(
               innodb_lock_registry.data(),
               k_concurrency_innodb_lock_header_slot_count_offset
           ) == k_concurrency_innodb_lock_slot_count &&
           load_le32(
               innodb_lock_registry.data(),
               k_concurrency_innodb_lock_header_slot_size_offset
           ) == k_concurrency_innodb_lock_slot_size &&
           innodb_lock_active_count <= k_concurrency_innodb_lock_slot_count &&
           innodb_lock_waiting_count <= k_concurrency_innodb_lock_slot_count &&
           innodb_lock_occupied_limit <= k_concurrency_innodb_lock_slot_count &&
           innodb_lock_active_count + innodb_lock_waiting_count <= innodb_lock_occupied_limit &&
           (innodb_lock_active_count == 0U || process_active_count > 0U) &&
           (innodb_lock_waiting_count == 0U || process_active_count > 0U) &&
           load_le32(page_index.data(), k_concurrency_page_index_header_entry_count_offset) ==
               k_concurrency_page_index_entry_count &&
           load_le32(page_index.data(), k_concurrency_page_index_header_entry_size_offset) ==
               MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE &&
           page_index_active_count <= k_concurrency_page_index_entry_count &&
           load_le32(
               page_write_lock_registry.data(),
               k_concurrency_innodb_lock_header_slot_count_offset
           ) == k_concurrency_page_write_lock_slot_count &&
           load_le32(
               page_write_lock_registry.data(),
               k_concurrency_innodb_lock_header_slot_size_offset
           ) == k_concurrency_page_write_lock_slot_size &&
           page_write_lock_active_count <= k_concurrency_page_write_lock_slot_count &&
           page_write_lock_waiting_count <= k_concurrency_page_write_lock_slot_count &&
           page_write_lock_occupied_limit <= k_concurrency_page_write_lock_slot_count &&
           page_write_lock_active_count + page_write_lock_waiting_count <=
               page_write_lock_occupied_limit &&
           (page_write_lock_active_count == 0U || process_active_count > 0U) &&
           (page_write_lock_waiting_count == 0U || process_active_count > 0U) &&
           load_le32(autoinc_registry.data(), k_concurrency_registry_slot_count_offset) ==
               k_concurrency_autoinc_slot_count &&
           load_le32(autoinc_registry.data(), k_concurrency_registry_slot_size_offset) ==
               MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE &&
           load_le32(page_pin_registry.data(), k_concurrency_page_pin_header_slot_count_offset) ==
               k_concurrency_page_pin_slot_count &&
           load_le32(page_pin_registry.data(), k_concurrency_page_pin_header_slot_size_offset) ==
               MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE &&
           page_pin_active_count <= k_concurrency_page_pin_slot_count &&
           (page_pin_active_count == 0U || process_active_count > 0U);
}

bool concurrency_shm_has_stale_reader_state_without_recovery(int shm_fd, off_t shm_size) {
    if (shm_size < static_cast<off_t>(
                       k_concurrency_page_pin_registry_offset +
                       k_concurrency_page_pin_registry_segment_size
                   ) ||
        static_cast<std::uintmax_t>(shm_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }

    const std::size_t mapping_size = static_cast<std::size_t>(shm_size);
    void *mapping = ::mmap(nullptr, mapping_size, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (mapping == MAP_FAILED) {
        return false;
    }

    const auto *base = static_cast<const unsigned char *>(mapping);
    const auto *read_view_registry = base + k_concurrency_read_view_registry_offset;
    const auto *innodb_lock_registry = base + k_concurrency_innodb_lock_registry_offset;
    const auto *page_write_lock_registry = base + k_concurrency_page_write_lock_registry_offset;
    const auto *page_pin_registry = base + k_concurrency_page_pin_registry_offset;
    const std::uint64_t read_view_active_count =
        load_le64(read_view_registry, k_concurrency_read_view_header_active_count_offset);
    const std::uint64_t innodb_lock_active_count =
        load_le64(innodb_lock_registry, k_concurrency_innodb_lock_header_active_count_offset);
    const std::uint64_t innodb_lock_waiting_count =
        load_le64(innodb_lock_registry, k_concurrency_innodb_lock_header_waiting_count_offset);
    const std::uint32_t innodb_lock_occupied_limit =
        load_le32(innodb_lock_registry, k_concurrency_innodb_lock_header_occupied_limit_offset);
    const std::uint64_t page_write_lock_active_count =
        load_le64(page_write_lock_registry, k_concurrency_innodb_lock_header_active_count_offset);
    const std::uint64_t page_write_lock_waiting_count =
        load_le64(page_write_lock_registry, k_concurrency_innodb_lock_header_waiting_count_offset);
    const std::uint32_t page_write_lock_occupied_limit =
        load_le32(page_write_lock_registry, k_concurrency_innodb_lock_header_occupied_limit_offset);
    const std::uint64_t page_pin_active_count =
        load_le64(page_pin_registry, k_concurrency_page_pin_header_active_count_offset);
    const bool reader_state_is_valid =
        load_le32(read_view_registry, k_concurrency_read_view_header_slot_count_offset) ==
            k_concurrency_read_view_slot_count &&
        load_le32(read_view_registry, k_concurrency_read_view_header_slot_size_offset) ==
            k_concurrency_read_view_slot_size &&
        read_view_active_count <= k_concurrency_read_view_slot_count &&
        load_le32(page_pin_registry, k_concurrency_page_pin_header_slot_count_offset) ==
            k_concurrency_page_pin_slot_count &&
        load_le32(page_pin_registry, k_concurrency_page_pin_header_slot_size_offset) ==
            MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE &&
        page_pin_active_count <= k_concurrency_page_pin_slot_count;
    const bool lock_state_is_valid =
        load_le32(innodb_lock_registry, k_concurrency_innodb_lock_header_slot_count_offset) ==
            k_concurrency_innodb_lock_slot_count &&
        load_le32(innodb_lock_registry, k_concurrency_innodb_lock_header_slot_size_offset) ==
            k_concurrency_innodb_lock_slot_size &&
        innodb_lock_active_count <= k_concurrency_innodb_lock_slot_count &&
        innodb_lock_waiting_count <= k_concurrency_innodb_lock_slot_count &&
        innodb_lock_occupied_limit <= k_concurrency_innodb_lock_slot_count &&
        innodb_lock_active_count + innodb_lock_waiting_count <= innodb_lock_occupied_limit &&
        load_le32(page_write_lock_registry, k_concurrency_innodb_lock_header_slot_count_offset) ==
            k_concurrency_page_write_lock_slot_count &&
        load_le32(page_write_lock_registry, k_concurrency_innodb_lock_header_slot_size_offset) ==
            k_concurrency_page_write_lock_slot_size &&
        page_write_lock_active_count <= k_concurrency_page_write_lock_slot_count &&
        page_write_lock_waiting_count <= k_concurrency_page_write_lock_slot_count &&
        page_write_lock_occupied_limit <= k_concurrency_page_write_lock_slot_count &&
        page_write_lock_active_count + page_write_lock_waiting_count <=
            page_write_lock_occupied_limit;
    if (!reader_state_is_valid || !lock_state_is_valid ||
        (read_view_active_count == 0U && page_pin_active_count == 0U)) {
        static_cast<void>(::munmap(mapping, mapping_size));
        return false;
    }

    mylite_ownerless_dictionary_state_snapshot dictionary_snapshot = {};
    mylite_ownerless_redo_state_snapshot redo_snapshot = {};
    const bool dictionary_is_idle = mylite_ownerless_dictionary_state_read_snapshot(
                                        base + k_concurrency_dictionary_state_offset,
                                        k_concurrency_dictionary_state_segment_size,
                                        &dictionary_snapshot
                                    ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK &&
                                    (dictionary_snapshot.generation & 1U) == 0U &&
                                    dictionary_snapshot.active_owner_id == 0U;
    const bool redo_is_idle =
        mylite_ownerless_redo_state_read_snapshot(
            base + k_concurrency_redo_state_offset,
            k_concurrency_redo_state_segment_size,
            &redo_snapshot
        ) == MYLITE_OWNERLESS_REDO_STATE_OK &&
        redo_snapshot.refcount == 0U && redo_snapshot.active_reservation_count == 0U &&
        redo_snapshot.latch_state != MYLITE_OWNERLESS_LATCH_STATE_LOCKED &&
        redo_snapshot.progress_latch_state != MYLITE_OWNERLESS_LATCH_STATE_LOCKED;
    const bool has_native_write_state =
        innodb_lock_active_count > 0U || innodb_lock_waiting_count > 0U ||
        page_write_lock_active_count > 0U || page_write_lock_waiting_count > 0U ||
        !dictionary_is_idle || !redo_is_idle;
    const bool stale_reader_state = !has_native_write_state;

    if (::munmap(mapping, mapping_size) != 0) {
        return false;
    }
    return stale_reader_state;
}

bool concurrency_shm_rebuild_requires_recovery(int shm_fd, off_t shm_size) {
    if (shm_size < static_cast<off_t>(
                       k_concurrency_page_pin_registry_offset +
                       k_concurrency_page_pin_registry_segment_size
                   ) ||
        static_cast<std::uintmax_t>(shm_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return true;
    }

    const std::size_t mapping_size = static_cast<std::size_t>(shm_size);
    void *mapping = ::mmap(nullptr, mapping_size, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (mapping == MAP_FAILED) {
        return true;
    }

    const auto *base = static_cast<const unsigned char *>(mapping);
    const auto active_count_at = [&](std::size_t segment_offset, std::size_t count_offset) {
        return load_le64(base + segment_offset, count_offset);
    };
    const bool has_recovery_sensitive_entries =
        active_count_at(
            k_concurrency_mdl_lock_table_offset,
            k_concurrency_mdl_lock_header_active_count_offset
        ) > 0U ||
        active_count_at(
            k_concurrency_trx_registry_offset,
            k_concurrency_trx_header_active_count_offset
        ) > 0U ||
        active_count_at(
            k_concurrency_innodb_lock_registry_offset,
            k_concurrency_innodb_lock_header_active_count_offset
        ) > 0U ||
        active_count_at(
            k_concurrency_page_write_lock_registry_offset,
            k_concurrency_innodb_lock_header_active_count_offset
        ) > 0U;

    bool requires_recovery = has_recovery_sensitive_entries;
    if (!requires_recovery) {
        mylite_ownerless_dictionary_state_snapshot dictionary_snapshot = {};
        requires_recovery = mylite_ownerless_dictionary_state_read_snapshot(
                                base + k_concurrency_dictionary_state_offset,
                                k_concurrency_dictionary_state_segment_size,
                                &dictionary_snapshot
                            ) != MYLITE_OWNERLESS_DICTIONARY_STATE_OK ||
                            (dictionary_snapshot.generation & 1U) != 0U ||
                            dictionary_snapshot.active_owner_id != 0U;
    }
    if (!requires_recovery) {
        mylite_ownerless_redo_state_snapshot redo_snapshot = {};
        requires_recovery =
            mylite_ownerless_redo_state_read_snapshot(
                base + k_concurrency_redo_state_offset,
                k_concurrency_redo_state_segment_size,
                &redo_snapshot
            ) != MYLITE_OWNERLESS_REDO_STATE_OK ||
            redo_snapshot.refcount != 0U || redo_snapshot.active_reservation_count != 0U ||
            redo_snapshot.latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED ||
            redo_snapshot.progress_latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED;
    }

    if (::munmap(mapping, mapping_size) != 0) {
        return true;
    }
    return requires_recovery;
}

bool concurrency_shm_header_identity_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    std::string_view database_uuid
) {
    if (database_uuid.size() != k_database_uuid_size) {
        return false;
    }
    if (std::memcmp(
            header.data() + k_concurrency_shm_magic_offset,
            k_concurrency_shm_magic.data(),
            k_concurrency_shm_magic.size()
        ) != 0) {
        return false;
    }
    return load_le32(header.data(), k_concurrency_shm_format_offset) ==
               k_concurrency_shm_format_version &&
           load_le32(header.data(), k_concurrency_shm_min_format_offset) ==
               k_concurrency_shm_header_version_min &&
           load_le32(header.data(), k_concurrency_shm_header_size_offset) ==
               k_concurrency_shm_header_size &&
           load_le32(header.data(), k_concurrency_shm_byte_order_offset) ==
               k_concurrency_shm_byte_order &&
           load_le32(header.data(), k_concurrency_shm_flags_offset) == 0U &&
           std::memcmp(
               header.data() + k_concurrency_shm_database_uuid_offset,
               database_uuid.data(),
               database_uuid.size()
           ) == 0;
}

void build_concurrency_shm_header(
    std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    const ConcurrencyShmFileIdentity &shm_identity,
    std::string_view database_uuid,
    std::uint64_t recovery_generation
) {
    header.fill(0U);
    std::memcpy(
        header.data() + k_concurrency_shm_magic_offset,
        k_concurrency_shm_magic.data(),
        k_concurrency_shm_magic.size()
    );
    store_le32(header.data(), k_concurrency_shm_format_offset, k_concurrency_shm_format_version);
    store_le32(
        header.data(),
        k_concurrency_shm_min_format_offset,
        k_concurrency_shm_header_version_min
    );
    store_le32(
        header.data(),
        k_concurrency_shm_header_size_offset,
        static_cast<std::uint32_t>(k_concurrency_shm_header_size)
    );
    store_le32(header.data(), k_concurrency_shm_byte_order_offset, k_concurrency_shm_byte_order);
    store_le32(header.data(), k_concurrency_shm_flags_offset, 0U);
    store_le32(header.data(), k_concurrency_shm_state_offset, k_concurrency_shm_state_clean);
    store_le64(
        header.data(),
        k_concurrency_shm_mapping_size_offset,
        static_cast<std::uint64_t>(shm_size)
    );
    store_le64(header.data(), k_concurrency_shm_generation_offset, 0U);
    store_le64(header.data(), k_concurrency_shm_recovery_generation_offset, recovery_generation);
    store_le32(
        header.data(),
        k_concurrency_shm_segment_table_offset,
        static_cast<std::uint32_t>(k_concurrency_shm_segment_table_start)
    );
    store_le32(
        header.data(),
        k_concurrency_shm_segment_count_offset,
        k_concurrency_shm_segment_count
    );
    std::memcpy(
        header.data() + k_concurrency_shm_database_uuid_offset,
        database_uuid.data(),
        database_uuid.size()
    );
    store_le64(header.data(), k_concurrency_shm_device_offset, shm_identity.device);
    store_le64(header.data(), k_concurrency_shm_inode_offset, shm_identity.inode);
}

ConcurrencyShmFileIdentity concurrency_shm_file_identity(const struct stat &shm_stat) {
    return ConcurrencyShmFileIdentity{
        static_cast<std::uint64_t>(shm_stat.st_dev),
        static_cast<std::uint64_t>(shm_stat.st_ino),
    };
}

int initialize_concurrency_shm_segments(
    int shm_fd,
    int page_log_fd,
    int checkpoint_fd,
    std::uint64_t next_trx_id
) {
    if (!write_concurrency_segment_descriptor(
            shm_fd,
            0U,
            k_concurrency_process_registry_segment_type,
            k_concurrency_process_registry_segment_version,
            k_concurrency_process_registry_offset,
            k_concurrency_process_registry_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            1U,
            k_concurrency_wait_channel_segment_type,
            k_concurrency_wait_channel_segment_version,
            k_concurrency_wait_channel_offset,
            k_concurrency_wait_channel_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            2U,
            k_concurrency_mdl_lock_table_segment_type,
            k_concurrency_mdl_lock_table_segment_version,
            k_concurrency_mdl_lock_table_offset,
            k_concurrency_mdl_lock_table_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            3U,
            k_concurrency_trx_registry_segment_type,
            k_concurrency_trx_registry_segment_version,
            k_concurrency_trx_registry_offset,
            k_concurrency_trx_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            4U,
            k_concurrency_read_view_registry_segment_type,
            k_concurrency_read_view_registry_segment_version,
            k_concurrency_read_view_registry_offset,
            k_concurrency_read_view_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            5U,
            k_concurrency_innodb_lock_registry_segment_type,
            k_concurrency_innodb_lock_registry_segment_version,
            k_concurrency_innodb_lock_registry_offset,
            k_concurrency_innodb_lock_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            6U,
            k_concurrency_redo_state_segment_type,
            k_concurrency_redo_state_segment_version,
            k_concurrency_redo_state_offset,
            k_concurrency_redo_state_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            7U,
            k_concurrency_page_index_segment_type,
            k_concurrency_page_index_segment_version,
            k_concurrency_page_index_offset,
            k_concurrency_page_index_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            8U,
            k_concurrency_dictionary_state_segment_type,
            k_concurrency_dictionary_state_segment_version,
            k_concurrency_dictionary_state_offset,
            k_concurrency_dictionary_state_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            9U,
            k_concurrency_page_write_lock_registry_segment_type,
            k_concurrency_page_write_lock_registry_segment_version,
            k_concurrency_page_write_lock_registry_offset,
            k_concurrency_page_write_lock_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            10U,
            k_concurrency_autoinc_registry_segment_type,
            k_concurrency_autoinc_registry_segment_version,
            k_concurrency_autoinc_registry_offset,
            k_concurrency_autoinc_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            11U,
            k_concurrency_page_pin_registry_segment_type,
            k_concurrency_page_pin_registry_segment_version,
            k_concurrency_page_pin_registry_offset,
            k_concurrency_page_pin_registry_segment_size
        )) {
        return MYLITE_IOERR;
    }

    const int registry_result = initialize_concurrency_process_registry(shm_fd);
    if (registry_result != MYLITE_OK) {
        return registry_result;
    }
    const int wait_channel_result = initialize_concurrency_wait_channels(shm_fd);
    if (wait_channel_result != MYLITE_OK) {
        return wait_channel_result;
    }
    const int mdl_lock_result = initialize_concurrency_mdl_lock_table(shm_fd);
    if (mdl_lock_result != MYLITE_OK) {
        return mdl_lock_result;
    }
    const int trx_result = initialize_concurrency_trx_registry(shm_fd, next_trx_id);
    if (trx_result != MYLITE_OK) {
        return trx_result;
    }
    const int read_view_result = initialize_concurrency_read_view_registry(shm_fd);
    if (read_view_result != MYLITE_OK) {
        return read_view_result;
    }
    const int innodb_lock_result = initialize_concurrency_innodb_lock_registry(shm_fd);
    if (innodb_lock_result != MYLITE_OK) {
        return innodb_lock_result;
    }
    const int redo_result = initialize_concurrency_redo_state(shm_fd, checkpoint_fd);
    if (redo_result != MYLITE_OK) {
        return redo_result;
    }
    const int page_index_result = initialize_concurrency_page_index(shm_fd, page_log_fd);
    if (page_index_result != MYLITE_OK) {
        return page_index_result;
    }
    const int dictionary_result = initialize_concurrency_dictionary_state(shm_fd);
    if (dictionary_result != MYLITE_OK) {
        return dictionary_result;
    }
    const int page_write_result = initialize_concurrency_page_write_lock_registry(shm_fd);
    if (page_write_result != MYLITE_OK) {
        return page_write_result;
    }
    const int autoinc_result = initialize_concurrency_autoinc_registry(shm_fd);
    if (autoinc_result != MYLITE_OK) {
        return autoinc_result;
    }
    return initialize_concurrency_page_pin_registry(shm_fd);
}

bool write_concurrency_segment_descriptor(
    int shm_fd,
    std::uint32_t index,
    std::uint32_t type,
    std::uint32_t version,
    std::uint64_t offset,
    std::uint64_t length
) {
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> segment = {};
    store_le32(segment.data(), k_concurrency_shm_segment_type_offset, type);
    store_le32(segment.data(), k_concurrency_shm_segment_version_offset, version);
    store_le64(segment.data(), k_concurrency_shm_segment_data_offset, offset);
    store_le64(segment.data(), k_concurrency_shm_segment_length_offset, length);
    store_le64(segment.data(), k_concurrency_shm_segment_generation_offset, 0U);
    return write_exact_at(
        shm_fd,
        segment.data(),
        segment.size(),
        static_cast<off_t>(
            k_concurrency_shm_segment_table_start +
            (index * k_concurrency_shm_segment_descriptor_size)
        )
    );
}

int initialize_concurrency_process_registry(int shm_fd) {
    std::array<unsigned char, k_concurrency_process_registry_size> registry = {};
    if (mylite_ownerless_process_registry_initialize(
            registry.data(),
            registry.size(),
            k_concurrency_process_slot_count
        ) != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               registry.data(),
               registry.size(),
               static_cast<off_t>(k_concurrency_process_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_wait_channels(int shm_fd) {
    std::array<unsigned char, k_concurrency_wait_channel_segment_size> wait_channels = {};
    store_le32(
        wait_channels.data(),
        k_concurrency_wait_header_channel_count_offset,
        k_concurrency_wait_channel_count
    );
    store_le32(
        wait_channels.data(),
        k_concurrency_wait_header_channel_size_offset,
        static_cast<std::uint32_t>(k_concurrency_wait_channel_size)
    );
    store_le64(wait_channels.data(), k_concurrency_wait_header_generation_offset, 0U);
    return write_exact_at(
               shm_fd,
               wait_channels.data(),
               wait_channels.size(),
               static_cast<off_t>(k_concurrency_wait_channel_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_mdl_lock_table(int shm_fd) {
    std::array<unsigned char, k_concurrency_mdl_lock_table_segment_size> lock_table = {};
    store_le32(
        lock_table.data(),
        k_concurrency_mdl_lock_header_entry_count_offset,
        k_concurrency_mdl_lock_table_entry_count
    );
    store_le32(
        lock_table.data(),
        k_concurrency_mdl_lock_header_entry_size_offset,
        static_cast<std::uint32_t>(k_concurrency_mdl_lock_table_entry_size)
    );
    store_le64(lock_table.data(), k_concurrency_mdl_lock_header_generation_offset, 0U);
    store_le64(lock_table.data(), k_concurrency_mdl_lock_header_active_count_offset, 0U);
    return write_exact_at(
               shm_fd,
               lock_table.data(),
               lock_table.size(),
               static_cast<off_t>(k_concurrency_mdl_lock_table_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

std::uint64_t read_concurrency_trx_next_id_from_shm(int shm_fd) {
    std::array<unsigned char, k_concurrency_trx_registry_header_size> trx_registry = {};
    if (!read_exact_at(
            shm_fd,
            trx_registry.data(),
            trx_registry.size(),
            static_cast<off_t>(k_concurrency_trx_registry_offset)
        )) {
        return 0U;
    }
    if (load_le32(trx_registry.data(), k_concurrency_trx_header_slot_count_offset) !=
            k_concurrency_trx_slot_count ||
        load_le32(trx_registry.data(), k_concurrency_trx_header_slot_size_offset) !=
            k_concurrency_trx_slot_size) {
        return 0U;
    }
    return load_le64(trx_registry.data(), k_concurrency_trx_header_next_id_offset);
}

int initialize_concurrency_trx_registry(int shm_fd, std::uint64_t next_trx_id) {
    std::array<unsigned char, k_concurrency_trx_registry_segment_size> trx_registry = {};
    if (mylite_ownerless_trx_registry_initialize(
            trx_registry.data(),
            trx_registry.size(),
            k_concurrency_trx_slot_count,
            std::max(next_trx_id, k_concurrency_initial_trx_id)
        ) != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               trx_registry.data(),
               trx_registry.size(),
               static_cast<off_t>(k_concurrency_trx_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_read_view_registry(int shm_fd) {
    std::array<unsigned char, k_concurrency_read_view_registry_segment_size> read_view_registry{};
    if (mylite_ownerless_read_view_registry_initialize(
            read_view_registry.data(),
            read_view_registry.size(),
            k_concurrency_read_view_slot_count
        ) != MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               read_view_registry.data(),
               read_view_registry.size(),
               static_cast<off_t>(k_concurrency_read_view_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_page_pin_registry(int shm_fd) {
    std::array<unsigned char, k_concurrency_page_pin_registry_segment_size> page_pin_registry{};
    if (mylite_ownerless_page_pin_registry_initialize(
            page_pin_registry.data(),
            page_pin_registry.size(),
            k_concurrency_page_pin_slot_count
        ) != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               page_pin_registry.data(),
               page_pin_registry.size(),
               static_cast<off_t>(k_concurrency_page_pin_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_innodb_lock_registry(int shm_fd) {
    const std::size_t registry_size = k_concurrency_innodb_lock_registry_segment_size;
    std::vector<unsigned char> innodb_lock_registry(registry_size);
    if (mylite_ownerless_innodb_lock_registry_initialize(
            innodb_lock_registry.data(),
            innodb_lock_registry.size(),
            k_concurrency_innodb_lock_slot_count
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               innodb_lock_registry.data(),
               innodb_lock_registry.size(),
               static_cast<off_t>(k_concurrency_innodb_lock_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_page_write_lock_registry(int shm_fd) {
    std::vector<unsigned char> page_write_lock_registry(
        k_concurrency_page_write_lock_registry_segment_size
    );
    if (mylite_ownerless_innodb_lock_registry_initialize(
            page_write_lock_registry.data(),
            page_write_lock_registry.size(),
            k_concurrency_page_write_lock_slot_count
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               page_write_lock_registry.data(),
               page_write_lock_registry.size(),
               static_cast<off_t>(k_concurrency_page_write_lock_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_redo_state(int shm_fd, int checkpoint_fd) {
    std::array<unsigned char, k_concurrency_redo_state_segment_size> redo_state = {};
    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(checkpoint_fd, &latest_lsn, &visible_lsn)) {
        return MYLITE_IOERR;
    }
    if (visible_lsn > latest_lsn) {
        latest_lsn = visible_lsn;
    }
    if (mylite_ownerless_redo_state_initialize(
            redo_state.data(),
            redo_state.size(),
            latest_lsn,
            visible_lsn
        ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               redo_state.data(),
               redo_state.size(),
               static_cast<off_t>(k_concurrency_redo_state_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_page_index(int shm_fd, int page_log_fd) {
    std::array<unsigned char, k_concurrency_page_index_segment_size> page_index = {};
    if (mylite_ownerless_page_index_initialize(
            page_index.data(),
            page_index.size(),
            k_concurrency_page_index_entry_count
        ) != MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        return MYLITE_IOERR;
    }
    const int replay_result =
        replay_concurrency_page_index(page_index.data(), page_index.size(), page_log_fd);
    if (replay_result != MYLITE_OK) {
        return replay_result;
    }
    return write_exact_at(
               shm_fd,
               page_index.data(),
               page_index.size(),
               static_cast<off_t>(k_concurrency_page_index_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

void reclaim_ownerless_page_log_after_native_checkpoint(RuntimeState &runtime) {
    if (runtime.readonly_mode || runtime.concurrency_wal_fd < 0 ||
        runtime.concurrency_checkpoint_fd < 0 || runtime.concurrency_shm_fd < 0 ||
        runtime.concurrency_process_slot_generation == 0U) {
        return;
    }
    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(
            runtime.concurrency_checkpoint_fd,
            &latest_lsn,
            &visible_lsn
        )) {
        return;
    }
    if (visible_lsn == 0U) {
        static_cast<void>(clear_ownerless_native_file_op_checkpoint_without_page_log(runtime));
        return;
    }

    const bool no_live_peers = ownerless_runtime_has_no_live_peers(runtime);
    if (!no_live_peers && ownerless_runtime_has_live_shared_readonly_peer(runtime)) {
        return;
    }
    const bool consumed_current_page_version_wal =
        runtime.ownerless_runtime_consumed_current_page_version_wal.load(std::memory_order_relaxed);
    if (!no_live_peers && runtime.ownerless_runtime_has_local_write &&
        !consumed_current_page_version_wal) {
        return;
    }
    bool native_file_op_checkpoint_marker_needed = false;
    bool autoinc_checkpoint_needed = false;
    if (no_live_peers) {
        static_cast<void>(read_concurrency_native_file_op_checkpoint_needed(
            runtime.concurrency_checkpoint_fd,
            &native_file_op_checkpoint_marker_needed
        ));
        autoinc_checkpoint_needed = ownerless_autoinc_checkpoint_pending(runtime);
        const bool force_native_checkpoint =
            native_file_op_checkpoint_marker_needed || autoinc_checkpoint_needed;
        const bool retained_page_log_records =
            ownerless_page_log_has_uncheckpointed_records(runtime);
        static_cast<void>(advance_ownerless_no_live_page_visible_lsn_for_reclaim(
            runtime,
            latest_lsn,
            visible_lsn,
            &visible_lsn,
            force_native_checkpoint || retained_page_log_records
        ));
        latest_lsn = std::max(latest_lsn, visible_lsn);
        static_cast<void>(
            seed_ownerless_runtime_redo_state_checkpoint(runtime, latest_lsn, visible_lsn)
        );
    }

    OwnerlessStatementLocks live_reclaim_statement_locks;
    if (!no_live_peers &&
        !acquire_ownerless_live_reclaim_statement_gate(runtime, live_reclaim_statement_locks)) {
        return;
    }

    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    if (!no_live_peers &&
        snapshot_ownerless_page_version_pins(runtime, &active_pin_count, &oldest_pin_lsn) !=
            MYLITE_OK) {
        return;
    }
    if (!no_live_peers && !ownerless_runtime_live_reclaim_has_no_native_write_state(runtime)) {
        return;
    }
    const bool active_pin_reclaim = !no_live_peers && active_pin_count > 0U;
    if (active_pin_reclaim && (oldest_pin_lsn == 0U || oldest_pin_lsn > visible_lsn)) {
        return;
    }
    if (active_pin_reclaim) {
        // Active pins retain the WAL until release; native checkpointing here can
        // rewrite redo state that a concurrent ownerless startup must read.
        return;
    }
    if (!no_live_peers && !ownerless_live_peer_page_log_reclaim_safe(runtime, visible_lsn)) {
        return;
    }
    const bool single_owner_epoch =
        no_live_peers && ownerless_runtime_in_single_owner_epoch_locked(runtime);
    if (!no_live_peers && ownerless_page_log_has_uncheckpointed_records(runtime)) {
        static_cast<void>(mylite_ownerless_innodb_make_checkpoint());
    }

    const bool consumed_page_version_wal =
        runtime.ownerless_runtime_consumed_page_version_wal.load(std::memory_order_relaxed);
    const bool reader_only_page_version_consumer =
        no_live_peers && consumed_page_version_wal && !runtime.ownerless_runtime_has_local_write;
    const bool skip_external_refresh =
        no_live_peers &&
        ((single_owner_epoch && !consumed_page_version_wal) || reader_only_page_version_consumer);
    const bool native_checkpoint_marker_needed =
        native_file_op_checkpoint_marker_needed || autoinc_checkpoint_needed;
    const bool require_native_page_lsn_proof = !no_live_peers || !native_checkpoint_marker_needed;
    /*
     * A runtime that only read peer page-version WAL does not own the native
     * dirty-page handoff. A newer native page LSN is therefore not proof that
     * disk contains the retained payload.
     */
    const bool allow_no_live_consumed_native_successor =
        no_live_peers && consumed_page_version_wal && runtime.ownerless_runtime_has_local_write;
    if (!prepare_ownerless_page_log_native_checkpoint_for_reclaim(
            runtime,
            visible_lsn,
            skip_external_refresh,
            require_native_page_lsn_proof,
            allow_no_live_consumed_native_successor
        )) {
        return;
    }
    if (no_live_peers) {
        std::uint64_t refreshed_latest_lsn = 0;
        std::uint64_t refreshed_visible_lsn = 0;
        if (read_concurrency_checkpoint_lsn(
                runtime.concurrency_checkpoint_fd,
                &refreshed_latest_lsn,
                &refreshed_visible_lsn
            ) &&
            refreshed_visible_lsn > visible_lsn) {
            visible_lsn = refreshed_visible_lsn;
            latest_lsn = std::max(latest_lsn, visible_lsn);
            static_cast<void>(
                seed_ownerless_runtime_redo_state_checkpoint(runtime, latest_lsn, visible_lsn)
            );
        }
    }
    if (native_file_op_checkpoint_marker_needed) {
        static_cast<void>(
            clear_concurrency_native_file_op_checkpoint_needed(runtime.concurrency_checkpoint_fd)
        );
    }
    if (autoinc_checkpoint_needed) {
        static_cast<void>(clear_ownerless_autoinc_checkpoint_pending(runtime));
    }

    const auto checkpoint_page_log_at_visible_lsn = [&](std::uint64_t checkpoint_visible_lsn) {
        OwnerlessPageLogReclaimContext reclaim_context = {};
        reclaim_context.runtime = &runtime;
        reclaim_context.visible_lsn = checkpoint_visible_lsn;
        return mylite_ownerless_page_log_checkpoint_with_completion_at(
            runtime.concurrency_wal_fd,
            k_concurrency_recovery_header_size,
            checkpoint_visible_lsn,
            collect_ownerless_reclaimed_page_index_record,
            replace_ownerless_page_index_after_reclaim,
            &reclaim_context
        );
    };

    const int checkpoint_result = checkpoint_page_log_at_visible_lsn(visible_lsn);
    if (checkpoint_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return;
    }
    if (no_live_peers && ownerless_page_log_has_uncheckpointed_records(runtime)) {
        std::uint64_t refreshed_latest_lsn = 0;
        std::uint64_t refreshed_visible_lsn = 0;
        if (read_concurrency_checkpoint_lsn(
                runtime.concurrency_checkpoint_fd,
                &refreshed_latest_lsn,
                &refreshed_visible_lsn
            ) &&
            refreshed_visible_lsn > visible_lsn) {
            const std::uint64_t refreshed_latest =
                std::max(refreshed_latest_lsn, refreshed_visible_lsn);
            static_cast<void>(seed_ownerless_runtime_redo_state_checkpoint(
                runtime,
                refreshed_latest,
                refreshed_visible_lsn
            ));
            static_cast<void>(checkpoint_page_log_at_visible_lsn(refreshed_visible_lsn));
        }
    }
}

bool ownerless_runtime_in_single_owner_epoch_locked(RuntimeState &runtime) {
    if (runtime.concurrency_process_slot_generation == 0U) {
        return false;
    }
    unsigned char *registry = runtime_process_registry(runtime);
    if (registry == nullptr) {
        return false;
    }

    const std::uint64_t active_count = mylite_ownerless_process_registry_active_count(registry);
    const std::uint64_t registry_generation =
        mylite_ownerless_process_registry_generation(registry);
    return active_count == 1U && registry_generation == runtime.concurrency_process_slot_generation;
}

bool ownerless_page_log_payload_bytes(RuntimeState &runtime, std::uint64_t *out_page_log_bytes) {
    if (out_page_log_bytes != nullptr) {
        *out_page_log_bytes = 0U;
    }
    if (runtime.concurrency_wal_fd < 0 || out_page_log_bytes == nullptr) {
        return false;
    }

    struct stat wal_stat = {};
    if (::fstat(runtime.concurrency_wal_fd, &wal_stat) != 0) {
        return false;
    }
    if (wal_stat.st_size <= static_cast<off_t>(k_concurrency_recovery_header_size)) {
        return true;
    }

    *out_page_log_bytes = static_cast<std::uint64_t>(
        wal_stat.st_size - static_cast<off_t>(k_concurrency_recovery_header_size)
    );
    return true;
}

bool ownerless_page_log_checkpoint_due(RuntimeState &runtime) {
    if (runtime.concurrency_wal_fd < 0) {
        return false;
    }

    std::uint64_t page_log_bytes = 0;
    if (!ownerless_page_log_payload_bytes(runtime, &page_log_bytes)) {
        return false;
    }
    return page_log_bytes >= MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES;
}

bool ownerless_page_log_has_uncheckpointed_records(RuntimeState &runtime) {
    if (runtime.concurrency_wal_fd < 0) {
        return false;
    }

    struct stat wal_stat = {};
    return ::fstat(runtime.concurrency_wal_fd, &wal_stat) == 0 &&
           wal_stat.st_size > static_cast<off_t>(k_concurrency_recovery_header_size);
}

bool ownerless_page_log_has_payload_records(RuntimeState &runtime) {
    if (runtime.concurrency_wal_fd < 0) {
        return false;
    }

    struct stat wal_stat = {};
    return ::fstat(runtime.concurrency_wal_fd, &wal_stat) == 0 &&
           wal_stat.st_size > static_cast<off_t>(k_empty_ownerless_page_log_size);
}

bool clear_ownerless_native_file_op_checkpoint_without_page_log(RuntimeState &runtime) {
    if (runtime.readonly_mode || runtime.concurrency_checkpoint_fd < 0 ||
        runtime.concurrency_shm_fd < 0 || runtime.concurrency_process_slot_generation == 0U ||
        !ownerless_runtime_has_no_live_peers(runtime)) {
        return false;
    }

    bool native_file_op_checkpoint_needed = false;
    const bool autoinc_checkpoint_needed = ownerless_autoinc_checkpoint_pending(runtime);
    if (!read_concurrency_native_file_op_checkpoint_needed(
            runtime.concurrency_checkpoint_fd,
            &native_file_op_checkpoint_needed
        ) ||
        (!native_file_op_checkpoint_needed && !autoinc_checkpoint_needed)) {
        return false;
    }

    if (mylite_ownerless_innodb_make_checkpoint() != MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        return false;
    }
    bool cleared = true;
    if (native_file_op_checkpoint_needed) {
        cleared =
            clear_concurrency_native_file_op_checkpoint_needed(runtime.concurrency_checkpoint_fd) &&
            cleared;
    }
    if (autoinc_checkpoint_needed) {
        cleared = clear_ownerless_autoinc_checkpoint_pending(runtime) && cleared;
    }
    return cleared;
}

bool ownerless_autoinc_checkpoint_pending(RuntimeState &runtime) {
    void *registry = runtime.ownerless_innodb_lock_hook.autoinc_registry;
    const std::size_t registry_size = runtime.ownerless_innodb_lock_hook.autoinc_registry_size;
    if (registry == nullptr || registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.owner_id == 0U ||
        runtime.ownerless_innodb_lock_hook.owner_generation == 0U) {
        return false;
    }

    int pending = 0;
    return mylite_ownerless_autoinc_registry_checkpoint_pending(
               registry,
               registry_size,
               runtime.ownerless_innodb_lock_hook.owner_id,
               runtime.ownerless_innodb_lock_hook.owner_generation,
               &pending
           ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK &&
           pending != 0;
}

bool clear_ownerless_autoinc_checkpoint_pending(RuntimeState &runtime) {
    void *registry = runtime.ownerless_innodb_lock_hook.autoinc_registry;
    const std::size_t registry_size = runtime.ownerless_innodb_lock_hook.autoinc_registry_size;
    if (registry == nullptr || registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.owner_id == 0U ||
        runtime.ownerless_innodb_lock_hook.owner_generation == 0U) {
        return false;
    }

    return mylite_ownerless_autoinc_registry_clear_checkpoint_pending(
               registry,
               registry_size,
               runtime.ownerless_innodb_lock_hook.owner_id,
               runtime.ownerless_innodb_lock_hook.owner_generation
           ) == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK;
}

bool seed_ownerless_runtime_redo_state_checkpoint(
    RuntimeState &runtime,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
) {
    if (runtime.ownerless_innodb_lock_hook.redo_state == nullptr ||
        runtime.ownerless_innodb_lock_hook.redo_state_size <
            k_concurrency_redo_state_segment_size ||
        (latest_lsn == 0U && visible_lsn == 0U)) {
        return false;
    }
    if (visible_lsn > latest_lsn) {
        latest_lsn = visible_lsn;
    }
    return mylite_ownerless_redo_state_seed_checkpoint(
               runtime.ownerless_innodb_lock_hook.redo_state,
               runtime.ownerless_innodb_lock_hook.redo_state_size,
               latest_lsn,
               visible_lsn
           ) == MYLITE_OWNERLESS_REDO_STATE_OK;
}

int seed_ownerless_native_checkpoint_baseline(
    RuntimeState &runtime,
    std::uint64_t *out_baseline_lsn
) {
    if (out_baseline_lsn == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_baseline_lsn = 0U;
    if (runtime.concurrency_checkpoint_fd < 0 ||
        runtime.ownerless_innodb_lock_hook.redo_state == nullptr ||
        runtime.ownerless_innodb_lock_hook.redo_state_size <
            k_concurrency_redo_state_segment_size) {
        return MYLITE_IOERR;
    }

    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(
            runtime.concurrency_checkpoint_fd,
            &latest_lsn,
            &visible_lsn
        )) {
        return MYLITE_IOERR;
    }
    if (latest_lsn != 0U || visible_lsn != 0U) {
        *out_baseline_lsn = visible_lsn != 0U ? visible_lsn : latest_lsn;
        return MYLITE_OK;
    }
    if (ownerless_page_log_has_payload_records(runtime)) {
        return MYLITE_OK;
    }

    const std::uint64_t native_checkpoint_lsn = mylite_ownerless_innodb_checkpoint_lsn();
    if (native_checkpoint_lsn == 0U) {
        return MYLITE_OK;
    }
    if (!update_concurrency_checkpoint_lsn(
            runtime.concurrency_checkpoint_fd,
            native_checkpoint_lsn,
            native_checkpoint_lsn,
            true
        )) {
        return MYLITE_IOERR;
    }

    if (mylite_ownerless_redo_state_seed_checkpoint(
            runtime.ownerless_innodb_lock_hook.redo_state,
            runtime.ownerless_innodb_lock_hook.redo_state_size,
            native_checkpoint_lsn,
            native_checkpoint_lsn
        ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return MYLITE_IOERR;
    }

    *out_baseline_lsn = native_checkpoint_lsn;
    return MYLITE_OK;
}

bool ownerless_statement_checkpoint_has_no_active_pins(RuntimeState &runtime) {
    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    if (snapshot_ownerless_page_version_pins(runtime, &active_pin_count, &oldest_pin_lsn) !=
        MYLITE_OK) {
        return false;
    }
    if (active_pin_count == 0U) {
        return true;
    }
    if (oldest_pin_lsn == 0U || runtime.concurrency_process_slot_generation == 0U ||
        runtime.ownerless_innodb_lock_hook.redo_state == nullptr ||
        runtime.ownerless_innodb_lock_hook.redo_state_size <
            k_concurrency_redo_state_visible_lsn_offset + sizeof(std::uint64_t)) {
        return false;
    }

    void *page_pin_registry = runtime_page_pin_registry(runtime);
    if (page_pin_registry == nullptr) {
        return false;
    }
    const std::uint32_t owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    std::uint32_t owner_active_pin_count = 0;
    const int owner_pin_count_result = mylite_ownerless_page_pin_registry_owner_active_count(
        page_pin_registry,
        k_concurrency_page_pin_registry_segment_size,
        owner_id,
        owner_id,
        runtime.concurrency_process_slot_generation,
        &owner_active_pin_count
    );
    if (owner_pin_count_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK ||
        owner_active_pin_count != active_pin_count) {
        return false;
    }

    const auto *redo_state =
        static_cast<const unsigned char *>(runtime.ownerless_innodb_lock_hook.redo_state);
    const std::uint64_t visible_lsn =
        load_shared64(redo_state, k_concurrency_redo_state_visible_lsn_offset);
    return visible_lsn != 0U && oldest_pin_lsn >= visible_lsn;
}

void ownerless_checkpoint_scheduler_loop(RuntimeState *runtime) {
    if (runtime == nullptr || mysql_thread_init() != 0) {
        return;
    }

    std::unique_lock<std::mutex> lock(runtime->mutex);
    while (true) {
        if (runtime->ownerless_checkpoint_scheduler_cv.wait_for(
                lock,
                k_ownerless_checkpoint_scheduler_interval,
                [runtime] { return runtime->ownerless_checkpoint_scheduler_stop; }
            )) {
            break;
        }
        if (runtime->ownerless_checkpoint_scheduler_stop) {
            break;
        }
        if (runtime->ref_count == 0U || !runtime->ownerless_rw_mode || runtime->readonly_mode ||
            runtime->ownerless_active_statement_count > 0U ||
            runtime->ownerless_active_explicit_transaction_count > 0U ||
            runtime->concurrency_process_slot_generation == 0U ||
            !ownerless_page_log_checkpoint_due(*runtime) ||
            !ownerless_statement_checkpoint_has_no_active_pins(*runtime)) {
            continue;
        }
        const auto last_statement_activity = runtime->ownerless_last_statement_activity;
        if (last_statement_activity.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() - last_statement_activity <
                k_ownerless_checkpoint_scheduler_interval) {
            continue;
        }

        reclaim_ownerless_page_log_after_native_checkpoint(*runtime);
    }
    lock.unlock();
    mysql_thread_end();
}

int start_ownerless_checkpoint_scheduler(RuntimeState &runtime) {
    if (!runtime.ownerless_rw_mode || runtime.readonly_mode ||
        is_memory_database_path(runtime.database_path) ||
        runtime.concurrency_process_slot_generation == 0U ||
        runtime.ownerless_checkpoint_scheduler_thread.joinable()) {
        return MYLITE_OK;
    }

    runtime.ownerless_checkpoint_scheduler_stop = false;
    runtime.ownerless_last_statement_reclaim_attempt = std::chrono::steady_clock::now();
    runtime.ownerless_last_statement_activity = runtime.ownerless_last_statement_reclaim_attempt;
    try {
        runtime.ownerless_checkpoint_scheduler_thread =
            std::thread(ownerless_checkpoint_scheduler_loop, &runtime);
    } catch (const std::system_error &) {
        runtime.ownerless_last_statement_reclaim_attempt = {};
        runtime.ownerless_last_statement_activity = {};
        return MYLITE_ERROR;
    }
    return MYLITE_OK;
}

void stop_ownerless_checkpoint_scheduler(
    RuntimeState &runtime,
    std::unique_lock<std::mutex> &lock
) {
    if (!runtime.ownerless_checkpoint_scheduler_thread.joinable()) {
        runtime.ownerless_checkpoint_scheduler_stop = false;
        return;
    }

    runtime.ownerless_checkpoint_scheduler_stop = true;
    runtime.ownerless_checkpoint_scheduler_cv.notify_all();
    std::thread scheduler_thread = std::move(runtime.ownerless_checkpoint_scheduler_thread);
    lock.unlock();
    scheduler_thread.join();
    lock.lock();
    runtime.ownerless_checkpoint_scheduler_stop = false;
}

bool begin_ownerless_runtime_statement(mylite_db &db) {
    if (!db.ownerless_rw_open || db.readonly_open) {
        return false;
    }

    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count == 0U || !g_runtime.ownerless_rw_mode || g_runtime.readonly_mode) {
        return false;
    }
    ++g_runtime.ownerless_active_statement_count;
    g_runtime.ownerless_last_statement_activity = std::chrono::steady_clock::now();
    return true;
}

void end_ownerless_runtime_statement(mylite_db &db) {
    if (!db.ownerless_rw_open || db.readonly_open) {
        return;
    }

    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ownerless_active_statement_count > 0U) {
        --g_runtime.ownerless_active_statement_count;
    }
    g_runtime.ownerless_last_statement_activity = std::chrono::steady_clock::now();
    g_runtime.ownerless_checkpoint_scheduler_cv.notify_all();
}

void publish_ownerless_explicit_transaction_count_locked(RuntimeState &runtime) {
    unsigned char *slot = runtime_process_slot(runtime);
    if (slot == nullptr) {
        return;
    }
    store_le64(
        slot,
        k_concurrency_process_slot_explicit_transaction_count_offset,
        runtime.ownerless_active_explicit_transaction_count
    );
}

void set_ownerless_explicit_transaction_active(mylite_db &db, bool active) {
    if (db.ownerless_explicit_transaction_active == active) {
        return;
    }

    db.ownerless_explicit_transaction_active = active;
    if (!db.ownerless_rw_open || db.readonly_open) {
        return;
    }

    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (active) {
        ++g_runtime.ownerless_active_explicit_transaction_count;
    } else if (g_runtime.ownerless_active_explicit_transaction_count > 0U) {
        --g_runtime.ownerless_active_explicit_transaction_count;
    }
    publish_ownerless_explicit_transaction_count_locked(g_runtime);
    g_runtime.ownerless_checkpoint_scheduler_cv.notify_all();
}

void advance_ownerless_handle_read_lsn_after_autocommit_write(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool statement_started_in_explicit_transaction
) {
    if (!db.ownerless_rw_open || db.readonly_open || statement_started_in_explicit_transaction) {
        return;
    }
    if (!sql_statement_requires_write(tokens) && !ownerless_dictionary_ddl_statement(tokens)) {
        return;
    }

    std::uint64_t latest_lsn = 0;
    bool local_native_only = false;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        if (g_runtime.ownerless_innodb_lock_hook.redo_state == nullptr ||
            g_runtime.ownerless_innodb_lock_hook.redo_state_size <
                k_concurrency_redo_state_segment_size) {
            return;
        }
        mylite_ownerless_redo_state_snapshot snapshot = {};
        if (mylite_ownerless_redo_state_read_snapshot(
                g_runtime.ownerless_innodb_lock_hook.redo_state,
                g_runtime.ownerless_innodb_lock_hook.redo_state_size,
                &snapshot
            ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
            return;
        }
        latest_lsn = snapshot.latest_lsn;
        local_native_only =
            ownerless_runtime_in_single_owner_epoch_locked(g_runtime) &&
            !g_runtime.ownerless_runtime_started_with_page_version_wal &&
            !g_runtime.ownerless_runtime_consumed_page_version_wal.load(std::memory_order_relaxed);
    }
    if (latest_lsn != 0U) {
        db.ownerless_local_native_read_lsn =
            std::max(db.ownerless_local_native_read_lsn, latest_lsn);
        if (!local_native_only) {
            db.ownerless_page_version_read_lsn =
                std::max(db.ownerless_page_version_read_lsn, latest_lsn);
        }
    }
}

void maybe_reclaim_ownerless_page_log_after_statement(
    mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    if (!db.ownerless_rw_open || db.readonly_open) {
        return;
    }
    const bool statement_writes =
        sql_statement_requires_write(tokens) || ownerless_dictionary_ddl_statement(tokens);
    const bool ends_explicit_transaction = sql_ends_explicit_transaction(tokens);
    if (!statement_writes && !ends_explicit_transaction) {
        return;
    }

    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (statement_writes) {
        g_runtime.ownerless_runtime_has_local_write = true;
    }
    if (ownerless_connection_is_in_explicit_transaction(db)) {
        return;
    }
    std::uint64_t page_log_bytes = 0;
    if (g_runtime.ref_count == 0U || !g_runtime.ownerless_rw_mode ||
        g_runtime.ownerless_active_explicit_transaction_count > 0U ||
        !ownerless_statement_checkpoint_has_no_active_pins(g_runtime) ||
        !ownerless_page_log_payload_bytes(g_runtime, &page_log_bytes) ||
        page_log_bytes < MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES) {
        return;
    }

    if (ownerless_runtime_in_single_owner_epoch_locked(g_runtime) &&
        page_log_bytes < MYLITE_OWNERLESS_SINGLE_OWNER_FOREGROUND_RECLAIM_MIN_BYTES) {
        bool native_file_op_checkpoint_needed = true;
        if (g_runtime.concurrency_checkpoint_fd >= 0 &&
            read_concurrency_native_file_op_checkpoint_needed(
                g_runtime.concurrency_checkpoint_fd,
                &native_file_op_checkpoint_needed
            ) &&
            !native_file_op_checkpoint_needed) {
            return;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_runtime.ownerless_last_statement_reclaim_attempt.time_since_epoch().count() != 0 &&
        now - g_runtime.ownerless_last_statement_reclaim_attempt <
            k_ownerless_checkpoint_scheduler_interval) {
        return;
    }
    g_runtime.ownerless_last_statement_reclaim_attempt = now;

    reclaim_ownerless_page_log_after_native_checkpoint(g_runtime);
}

void mark_ownerless_native_file_op_checkpoint_after_dictionary_ddl(
    mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    if (!db.ownerless_rw_open || db.readonly_open) {
        return;
    }
    const bool file_rename_redo = mylite_ownerless_innodb_take_file_rename_redo() != 0;
    if (!file_rename_redo && !ownerless_dictionary_ddl_needs_native_file_op_checkpoint(tokens)) {
        return;
    }
    if (file_rename_redo &&
        mylite_ownerless_innodb_make_checkpoint() == MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        return;
    }

    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count == 0U || !g_runtime.ownerless_rw_mode ||
        g_runtime.concurrency_checkpoint_fd < 0) {
        return;
    }
    static_cast<void>(
        mark_concurrency_native_file_op_checkpoint_needed(g_runtime.concurrency_checkpoint_fd)
    );
}

bool advance_ownerless_no_live_page_visible_lsn_for_reclaim(
    RuntimeState &runtime,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    std::uint64_t *out_visible_lsn,
    bool force_native_checkpoint
) {
    if (out_visible_lsn == nullptr) {
        return false;
    }
    *out_visible_lsn = visible_lsn;
    if (latest_lsn == 0U || latest_lsn <= visible_lsn || runtime.concurrency_checkpoint_fd < 0) {
        if (force_native_checkpoint && latest_lsn != 0U && runtime.concurrency_checkpoint_fd >= 0) {
            const std::uint64_t checkpoint_lsn = std::max(latest_lsn, visible_lsn);
            if (mylite_ownerless_innodb_advance_external_lsn(checkpoint_lsn) !=
                MYLITE_OWNERLESS_INNODB_LOCK_OK) {
                return false;
            }
            mylite_ownerless_innodb_publish_buffer_pool_pages_to_lsn(checkpoint_lsn);
            mylite_ownerless_innodb_flush_dirty_pages_to_lsn(checkpoint_lsn);
            static_cast<void>(mylite_ownerless_innodb_make_checkpoint());
            std::uint64_t refreshed_latest_lsn = 0;
            std::uint64_t refreshed_visible_lsn = 0;
            if (read_concurrency_checkpoint_lsn(
                    runtime.concurrency_checkpoint_fd,
                    &refreshed_latest_lsn,
                    &refreshed_visible_lsn
                ) &&
                refreshed_visible_lsn > visible_lsn) {
                *out_visible_lsn = refreshed_visible_lsn;
            }
        }
        return true;
    }

    if (mylite_ownerless_innodb_advance_external_lsn(latest_lsn) !=
        MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        return false;
    }
    mylite_ownerless_innodb_publish_buffer_pool_pages_to_lsn(latest_lsn);
    mylite_ownerless_innodb_flush_dirty_pages_to_lsn(latest_lsn);
    if (force_native_checkpoint) {
        /*
         * The no-live path has published the ownerless page view and flushed
         * native dirty pages. It can now let MariaDB publish a FILE_CHECKPOINT,
         * including completed DDL file-operation boundaries, before the shared
         * visible LSN is advanced to that native checkpoint.
         */
        static_cast<void>(mylite_ownerless_innodb_make_checkpoint());
    }

    std::uint64_t refreshed_latest_lsn = 0;
    std::uint64_t refreshed_visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(
            runtime.concurrency_checkpoint_fd,
            &refreshed_latest_lsn,
            &refreshed_visible_lsn
        )) {
        return false;
    }
    if (refreshed_visible_lsn > visible_lsn) {
        *out_visible_lsn = refreshed_visible_lsn;
    }
    if (*out_visible_lsn >= latest_lsn) {
        return true;
    }

    if (mylite_ownerless_innodb_checkpoint_covers_lsn(latest_lsn) !=
        MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        return *out_visible_lsn > visible_lsn;
    }
    if (!update_concurrency_checkpoint_lsn(
            runtime.concurrency_checkpoint_fd,
            latest_lsn,
            latest_lsn,
            true
        )) {
        return *out_visible_lsn > visible_lsn;
    }

    *out_visible_lsn = latest_lsn;
    return true;
}

bool prepare_ownerless_page_log_native_checkpoint_for_reclaim(
    RuntimeState &runtime,
    std::uint64_t visible_lsn,
    bool skip_external_refresh,
    bool require_native_page_lsn_proof,
    bool allow_no_live_consumed_native_successor
) {
    if (mylite_ownerless_innodb_advance_external_lsn(visible_lsn) !=
        MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        return false;
    }
    if (!skip_external_refresh) {
        mylite_ownerless_innodb_refresh_external_pages(visible_lsn);
    }
    if (mylite_ownerless_innodb_checkpoint_covers_lsn(visible_lsn) !=
        MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        return false;
    }

#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("native-checkpoint-before-reclaim");
#  endif

    if (require_native_page_lsn_proof && !ownerless_page_log_has_native_page_lsn_proof(
                                             runtime,
                                             visible_lsn,
                                             allow_no_live_consumed_native_successor
                                         )) {
        return false;
    }

    const std::uint32_t owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    return mylite_ownerless_page_index_require_wal_scan(
               runtime_page_index(runtime),
               k_concurrency_page_index_segment_size,
               owner_id,
               runtime.concurrency_process_slot_generation
           ) == MYLITE_OWNERLESS_PAGE_INDEX_OK;
}

bool ownerless_page_log_has_native_page_lsn_proof(
    RuntimeState &runtime,
    std::uint64_t visible_lsn,
    bool allow_no_live_consumed_native_successor
) {
    if (runtime.concurrency_wal_fd < 0) {
        return false;
    }
    if (!ownerless_page_log_has_uncheckpointed_records(runtime)) {
        return true;
    }

    OwnerlessNativePageCheckpointProofContext proof = {};
    proof.runtime = &runtime;
    proof.visible_lsn = visible_lsn;
    const int replay_result = mylite_ownerless_page_log_replay_at(
        runtime.concurrency_wal_fd,
        k_concurrency_recovery_header_size,
        collect_ownerless_native_page_checkpoint_record,
        &proof
    );
    if (replay_result != MYLITE_OWNERLESS_PAGE_LOG_OK || proof.blocked) {
        return false;
    }

    std::vector<OwnerlessNativePageCheckpointRecord> latest_records;
    std::sort(proof.records.begin(), proof.records.end(), [](const auto &left, const auto &right) {
        if (left.space_id != right.space_id) {
            return left.space_id < right.space_id;
        }
        return left.page_no < right.page_no;
    });
    for (const OwnerlessNativePageCheckpointRecord &record : proof.records) {
        if (!latest_records.empty() && latest_records.back().space_id == record.space_id &&
            latest_records.back().page_no == record.page_no) {
            if (ownerless_native_page_checkpoint_record_is_better(record, latest_records.back())) {
                latest_records.back() = record;
            }
            continue;
        }
        latest_records.push_back(record);
    }

    for (const OwnerlessNativePageCheckpointRecord &record : latest_records) {
        if (!verify_ownerless_native_page_checkpoint_latest_record(
                runtime,
                record,
                visible_lsn,
                allow_no_live_consumed_native_successor
            )) {
            return false;
        }
    }
    return true;
}

bool ownerless_live_peer_page_log_reclaim_safe(RuntimeState &runtime, std::uint64_t visible_lsn) {
    if (runtime.concurrency_wal_fd < 0) {
        return false;
    }
    if (!ownerless_page_log_has_uncheckpointed_records(runtime)) {
        return true;
    }

    OwnerlessNativePageCheckpointProofContext proof = {};
    proof.runtime = &runtime;
    proof.visible_lsn = visible_lsn;
    const int replay_result = mylite_ownerless_page_log_replay_at(
        runtime.concurrency_wal_fd,
        k_concurrency_recovery_header_size,
        collect_ownerless_native_page_checkpoint_record,
        &proof
    );
    if (replay_result != MYLITE_OWNERLESS_PAGE_LOG_OK || proof.blocked) {
        return false;
    }

    // Native checkpoint proof is process-local. A live peer can still read an
    // older data/index page from its native file view, so user page images stay
    // in the WAL until no-live reclaim can make the native file authoritative.
    return proof.records.empty();
}

bool ownerless_page_log_record_is_native_support_state(
    RuntimeState &runtime,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset
) {
    if (runtime.concurrency_wal_fd < 0) {
        return false;
    }

    std::vector<unsigned char> page(k_innodb_page_size_max);
    std::uint32_t page_size = 0;
    std::uint64_t record_page_lsn = 0;
    std::uint64_t record_commit_lsn = 0;
    const int record_result = mylite_ownerless_page_log_read_page_under_read_lock_at(
        runtime.concurrency_wal_fd,
        k_concurrency_recovery_header_size,
        record_offset,
        space_id,
        page_no,
        page.data(),
        static_cast<std::uint32_t>(page.size()),
        &page_size,
        &record_page_lsn,
        &record_commit_lsn
    );
    if (record_result != MYLITE_OWNERLESS_PAGE_LOG_OK || record_page_lsn != page_lsn ||
        record_commit_lsn != commit_lsn ||
        page_size < k_innodb_fil_page_type_offset + sizeof(std::uint16_t)) {
        return false;
    }

    return ownerless_page_image_is_native_support_state(page.data(), page_size);
}

bool ownerless_page_image_is_native_support_state(const void *page, std::uint32_t page_size) {
    if (page == nullptr || page_size < k_innodb_fil_page_type_offset + sizeof(std::uint16_t)) {
        return false;
    }

    const auto *bytes = static_cast<const unsigned char *>(page);
    const std::uint16_t page_type =
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[k_innodb_fil_page_type_offset]) << 8U
        ) |
        static_cast<std::uint16_t>(bytes[k_innodb_fil_page_type_offset + 1]);
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

bool ownerless_file_per_table_page_matches(
    RuntimeState &runtime,
    const OwnerlessNativePageCheckpointRecord &record
) {
    if (record.space_id <= 3U || runtime.database_path.empty()) {
        return false;
    }

    std::vector<unsigned char> page(k_innodb_page_size_max);
    std::uint32_t page_size = 0;
    std::uint64_t record_page_lsn = 0;
    std::uint64_t record_commit_lsn = 0;
    const int record_result = mylite_ownerless_page_log_read_page_under_read_lock_at(
        runtime.concurrency_wal_fd,
        k_concurrency_recovery_header_size,
        record.record_offset,
        record.space_id,
        record.page_no,
        page.data(),
        static_cast<std::uint32_t>(page.size()),
        &page_size,
        &record_page_lsn,
        &record_commit_lsn
    );
    if (record_result != MYLITE_OWNERLESS_PAGE_LOG_OK || record_page_lsn != record.page_lsn ||
        record_commit_lsn != record.commit_lsn || page_size == 0U ||
        page_size > k_innodb_page_size_max) {
        return false;
    }

    const std::filesystem::path datadir =
        std::filesystem::path(runtime.database_path) / k_datadir_name;
    std::error_code error;
    if (!std::filesystem::is_directory(datadir, error) || error) {
        return false;
    }

    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(datadir, options, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return false;
    }

    for (; it != end; it.increment(error)) {
        if (error) {
            return false;
        }
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || entry_error || it->path().extension() != ".ibd") {
            continue;
        }

        std::ifstream file(it->path(), std::ios::binary);
        if (!file) {
            return false;
        }

        std::array<unsigned char, k_innodb_fil_page_space_id_offset + sizeof(std::uint32_t)>
            page_header = {};
        file.read(
            reinterpret_cast<char *>(page_header.data()),
            static_cast<std::streamsize>(page_header.size())
        );
        if (file.gcount() != static_cast<std::streamsize>(page_header.size())) {
            return false;
        }
        if (load_be32(page_header.data(), k_innodb_fil_page_space_id_offset) != record.space_id) {
            continue;
        }

        const std::uint64_t page_offset =
            static_cast<std::uint64_t>(record.page_no) * static_cast<std::uint64_t>(page_size);
        if (record.page_no != 0U && page_offset / record.page_no != page_size) {
            return false;
        }
        std::vector<unsigned char> disk_page(page_size);
        file.seekg(static_cast<std::streamoff>(page_offset), std::ios::beg);
        if (!file) {
            return false;
        }
        file.read(
            reinterpret_cast<char *>(disk_page.data()),
            static_cast<std::streamsize>(disk_page.size())
        );
        if (file.gcount() != static_cast<std::streamsize>(disk_page.size())) {
            return false;
        }
        return load_be64(disk_page.data(), 16) == record.page_lsn &&
               std::memcmp(disk_page.data(), page.data(), page_size) == 0;
    }

    return false;
}

bool ownerless_file_per_table_page_is_discarded(
    RuntimeState &runtime,
    const OwnerlessNativePageCheckpointRecord &record
) {
    if (record.space_id <= 3U || runtime.database_path.empty() || runtime.concurrency_wal_fd < 0) {
        return false;
    }

    std::vector<unsigned char> page(k_ownerless_native_page_proof_capacity);
    std::uint32_t page_size = 0;
    std::uint64_t record_page_lsn = 0;
    std::uint64_t record_commit_lsn = 0;
    const int record_result = mylite_ownerless_page_log_read_page_under_read_lock_at(
        runtime.concurrency_wal_fd,
        k_concurrency_recovery_header_size,
        record.record_offset,
        record.space_id,
        record.page_no,
        page.data(),
        static_cast<std::uint32_t>(page.size()),
        &page_size,
        &record_page_lsn,
        &record_commit_lsn
    );
    if (record_result != MYLITE_OWNERLESS_PAGE_LOG_OK || record_page_lsn != record.page_lsn ||
        record_commit_lsn != record.commit_lsn || page_size == 0U ||
        page_size > k_innodb_page_size_max) {
        return false;
    }

    const std::filesystem::path datadir =
        std::filesystem::path(runtime.database_path) / k_datadir_name;
    std::error_code error;
    if (!std::filesystem::is_directory(datadir, error) || error) {
        return false;
    }

    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(datadir, options, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return false;
    }

    for (; it != end; it.increment(error)) {
        if (error) {
            return false;
        }
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || entry_error || it->path().extension() != ".ibd") {
            continue;
        }

        std::array<unsigned char, k_innodb_fil_page_space_id_offset + sizeof(std::uint32_t)>
            page_header = {};
        std::ifstream file(it->path(), std::ios::binary);
        if (!file) {
            return false;
        }
        file.read(
            reinterpret_cast<char *>(page_header.data()),
            static_cast<std::streamsize>(page_header.size())
        );
        if (file.gcount() != static_cast<std::streamsize>(page_header.size())) {
            return false;
        }
        if (load_be32(page_header.data(), k_innodb_fil_page_space_id_offset) != record.space_id) {
            continue;
        }

        const std::uint64_t page_offset =
            static_cast<std::uint64_t>(record.page_no) * static_cast<std::uint64_t>(page_size);
        if (record.page_no != 0U && page_offset / record.page_no != page_size) {
            return false;
        }
        std::error_code size_error;
        const auto file_size_value = std::filesystem::file_size(it->path(), size_error);
        if (size_error) {
            return false;
        }
        const std::uint64_t file_size = static_cast<std::uint64_t>(file_size_value);
        if (file_size <= page_offset) {
            return true;
        }
        if (file_size - page_offset < page_size) {
            return false;
        }

        std::vector<unsigned char> disk_page(page_size);
        file.seekg(static_cast<std::streamoff>(page_offset), std::ios::beg);
        if (!file) {
            return false;
        }
        file.read(
            reinterpret_cast<char *>(disk_page.data()),
            static_cast<std::streamsize>(disk_page.size())
        );
        if (file.gcount() != static_cast<std::streamsize>(disk_page.size())) {
            return false;
        }
        return std::all_of(disk_page.begin(), disk_page.end(), [](unsigned char byte) {
            return byte == 0U;
        });
    }

    return false;
}

bool ownerless_file_per_table_space_is_absent(RuntimeState &runtime, std::uint32_t space_id) {
    if (space_id <= 3U || runtime.database_path.empty()) {
        return false;
    }

    const std::filesystem::path datadir =
        std::filesystem::path(runtime.database_path) / k_datadir_name;
    std::error_code error;
    if (!std::filesystem::is_directory(datadir, error) || error) {
        return false;
    }

    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(datadir, options, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return false;
    }

    for (; it != end; it.increment(error)) {
        if (error) {
            return false;
        }
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || entry_error || it->path().extension() != ".ibd") {
            continue;
        }

        std::array<unsigned char, k_innodb_fil_page_space_id_offset + sizeof(std::uint32_t)>
            page_header = {};
        std::ifstream file(it->path(), std::ios::binary);
        if (!file) {
            return false;
        }
        file.read(
            reinterpret_cast<char *>(page_header.data()),
            static_cast<std::streamsize>(page_header.size())
        );
        if (file.gcount() != static_cast<std::streamsize>(page_header.size())) {
            return false;
        }
        if (load_be32(page_header.data(), k_innodb_fil_page_space_id_offset) == space_id) {
            return false;
        }
    }

    return true;
}

bool ownerless_native_page_checkpoint_record_is_better(
    const OwnerlessNativePageCheckpointRecord &candidate,
    const OwnerlessNativePageCheckpointRecord &current
) {
    return candidate.commit_lsn > current.commit_lsn ||
           (candidate.commit_lsn == current.commit_lsn && candidate.page_lsn > current.page_lsn);
}

bool verify_ownerless_native_page_checkpoint_latest_record(
    RuntimeState &runtime,
    const OwnerlessNativePageCheckpointRecord &record,
    std::uint64_t visible_lsn,
    bool allow_no_live_consumed_native_successor
) {
    std::uint64_t disk_page_lsn = 0;
    const int disk_lsn_result =
        mylite_ownerless_innodb_disk_page_lsn(record.space_id, record.page_no, &disk_page_lsn);
    if (disk_lsn_result == MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        if (disk_page_lsn == record.page_lsn) {
            std::vector<unsigned char> page(k_ownerless_native_page_proof_capacity);
            std::uint32_t page_size = 0;
            std::uint64_t record_page_lsn = 0;
            std::uint64_t record_commit_lsn = 0;
            const int record_result = mylite_ownerless_page_log_read_page_under_read_lock_at(
                runtime.concurrency_wal_fd,
                k_concurrency_recovery_header_size,
                record.record_offset,
                record.space_id,
                record.page_no,
                page.data(),
                static_cast<std::uint32_t>(page.size()),
                &page_size,
                &record_page_lsn,
                &record_commit_lsn
            );
            if (record_result == MYLITE_OWNERLESS_PAGE_LOG_OK &&
                record_page_lsn == record.page_lsn && record_commit_lsn == record.commit_lsn) {
                std::uint64_t matched_disk_page_lsn = 0;
                int disk_matches = 0;
                const int match_result = mylite_ownerless_innodb_disk_page_matches(
                    record.space_id,
                    record.page_no,
                    page.data(),
                    page_size,
                    &matched_disk_page_lsn,
                    &disk_matches
                );
                if (match_result == MYLITE_OWNERLESS_INNODB_LOCK_OK &&
                    matched_disk_page_lsn == record.page_lsn && disk_matches != 0) {
                    return true;
                }
                if (match_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
                    match_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE) {
                    return false;
                }
            } else {
                const bool expected_record_status =
                    record_result == MYLITE_OWNERLESS_PAGE_LOG_OK ||
                    record_result == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND ||
                    record_result == MYLITE_OWNERLESS_PAGE_LOG_FULL;
                if (!expected_record_status) {
                    return false;
                }
            }
        }
        if (allow_no_live_consumed_native_successor && record.external_snapshot_lineage_record &&
            record.space_id > 3U && disk_page_lsn > record.page_lsn &&
            disk_page_lsn <= visible_lsn) {
            return true;
        }
    }
    if (disk_lsn_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        disk_lsn_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE) {
        return false;
    }
    if (ownerless_file_per_table_page_matches(runtime, record)) {
        return true;
    }
    if (ownerless_file_per_table_page_is_discarded(runtime, record)) {
        return true;
    }
    return disk_lsn_result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE &&
           ownerless_file_per_table_space_is_absent(runtime, record.space_id);
}

int collect_ownerless_native_page_checkpoint_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
) {
    auto *proof = static_cast<OwnerlessNativePageCheckpointProofContext *>(context);
    if (proof == nullptr || proof->runtime == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (proof->blocked || commit_lsn > proof->visible_lsn || page_lsn == 0U || space_id == 0U ||
        page_no < 3U) {
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }
    std::uint32_t metadata_flags = 0U;
    const int metadata_result = mylite_ownerless_page_log_record_metadata_flags_at(
        proof->runtime->concurrency_wal_fd,
        record_offset,
        &metadata_flags
    );
    if (metadata_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        proof->blocked = true;
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }
    if ((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_SNAPSHOT_BOUNDARY) != 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }
    const bool external_snapshot_lineage =
        (metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_EXTERNAL_SNAPSHOT_LINEAGE) != 0U;
    if ((metadata_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_NATIVE_SUPPORT_STATE) != 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }
    if (ownerless_page_log_record_is_native_support_state(
            *proof->runtime,
            space_id,
            page_no,
            page_lsn,
            commit_lsn,
            record_offset
        )) {
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }
    OwnerlessNativePageCheckpointRecord
        record{space_id, page_no, page_lsn, commit_lsn, record_offset, external_snapshot_lineage};
    proof->records.push_back(record);
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

bool ownerless_runtime_has_no_live_peers(RuntimeState &runtime) {
    std::uint64_t active_count = 0;
    if (read_concurrency_process_active_count(runtime.concurrency_shm_fd, &active_count) !=
        MYLITE_OK) {
        return false;
    }
    if (active_count != 1U) {
        return false;
    }

    std::uint64_t live_count = 0;
    if (read_concurrency_process_live_count(runtime.concurrency_shm_fd, &live_count) != MYLITE_OK) {
        return false;
    }
    return live_count == 1U;
}

bool ownerless_runtime_has_live_shared_readonly_peer(RuntimeState &runtime) {
    unsigned char *registry = runtime_process_registry(runtime);
    if (registry == nullptr) {
        return true;
    }

    for (std::uint32_t index = 0; index < k_concurrency_process_slot_count; ++index) {
        unsigned char *slot = registry + k_concurrency_process_registry_header_size +
                              (index * k_concurrency_process_slot_size);
        if (load_le32(slot, k_concurrency_process_slot_state_offset) !=
            MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE) {
            continue;
        }
        if (load_le32(slot, k_concurrency_process_slot_open_mode_offset) !=
            k_concurrency_process_open_mode_shared_readonly) {
            continue;
        }
        if (ownerless_process_is_alive(
                load_le64(slot, k_concurrency_process_slot_pid_offset),
                nullptr
            ) != 0) {
            return true;
        }
    }
    return false;
}

bool ownerless_runtime_has_no_live_explicit_transactions(RuntimeState &runtime) {
    unsigned char *registry = runtime_process_registry(runtime);
    if (registry == nullptr) {
        return false;
    }

    for (std::uint32_t index = 0; index < k_concurrency_process_slot_count; ++index) {
        unsigned char *slot = registry + k_concurrency_process_registry_header_size +
                              (index * k_concurrency_process_slot_size);
        if (load_le32(slot, k_concurrency_process_slot_state_offset) !=
            MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE) {
            continue;
        }
        if (ownerless_process_is_alive(
                load_le64(slot, k_concurrency_process_slot_pid_offset),
                nullptr
            ) == 0) {
            continue;
        }
        if (load_le64(slot, k_concurrency_process_slot_explicit_transaction_count_offset) > 0U) {
            return false;
        }
    }
    return true;
}

bool ownerless_process_registry_has_other_live_explicit_transactions(
    const void *registry,
    std::size_t registry_size,
    std::uint32_t owner_id
) {
    if (registry == nullptr || registry_size < k_concurrency_process_registry_size ||
        owner_id == 0U) {
        return true;
    }

    const auto *bytes = static_cast<const unsigned char *>(registry);
    for (std::uint32_t index = 0; index < k_concurrency_process_slot_count; ++index) {
        const std::size_t slot_offset =
            k_concurrency_process_registry_header_size +
            (static_cast<std::size_t>(index) * k_concurrency_process_slot_size);
        if (slot_offset + k_concurrency_process_slot_size > registry_size) {
            return true;
        }
        const unsigned char *slot = bytes + slot_offset;
        if (load_le32(slot, k_concurrency_process_slot_state_offset) !=
            MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE) {
            continue;
        }
        if (ownerless_owner_id_from_slot_index(index) == owner_id) {
            continue;
        }
        if (ownerless_process_is_alive(
                load_le64(slot, k_concurrency_process_slot_pid_offset),
                nullptr
            ) == 0) {
            continue;
        }
        if (load_le64(slot, k_concurrency_process_slot_explicit_transaction_count_offset) > 0U) {
            return true;
        }
    }
    return false;
}

bool ownerless_trx_registry_has_other_active_transactions(OwnerlessInnoDBLockHookContext *hook) {
    if (hook == nullptr || hook->trx_registry == nullptr || hook->trx_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return true;
    }

    const std::uint64_t active_count =
        mylite_ownerless_trx_registry_active_count(hook->trx_registry);
    if (active_count == 0U) {
        return false;
    }

    std::uint32_t owner_active_count = 0;
    const int owner_count_result = mylite_ownerless_trx_registry_owner_active_count(
        hook->trx_registry,
        hook->trx_registry_size,
        hook->owner_id,
        hook->owner_id,
        hook->owner_generation,
        &owner_active_count
    );
    if (owner_count_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return true;
    }
    return active_count > owner_active_count;
}

bool ownerless_runtime_live_reclaim_has_no_native_write_state(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr) {
        return false;
    }
    if (!ownerless_runtime_has_no_live_explicit_transactions(runtime)) {
        return false;
    }

    const auto *base = static_cast<const unsigned char *>(runtime.concurrency_shm_mapping);
    const auto active_count_at = [&](std::size_t segment_offset, std::size_t count_offset) {
        return load_shared64(base + segment_offset, count_offset);
    };
    const std::uint64_t active_trx_count = active_count_at(
        k_concurrency_trx_registry_offset,
        k_concurrency_trx_header_active_count_offset
    );
    const std::uint64_t innodb_lock_active_count = active_count_at(
        k_concurrency_innodb_lock_registry_offset,
        k_concurrency_innodb_lock_header_active_count_offset
    );
    const std::uint64_t innodb_lock_waiting_count = active_count_at(
        k_concurrency_innodb_lock_registry_offset,
        k_concurrency_innodb_lock_header_waiting_count_offset
    );
    const std::uint64_t page_write_lock_active_count = active_count_at(
        k_concurrency_page_write_lock_registry_offset,
        k_concurrency_innodb_lock_header_active_count_offset
    );
    const std::uint64_t page_write_lock_waiting_count = active_count_at(
        k_concurrency_page_write_lock_registry_offset,
        k_concurrency_innodb_lock_header_waiting_count_offset
    );
    if (active_trx_count > 0U || innodb_lock_active_count > 0U || innodb_lock_waiting_count > 0U ||
        page_write_lock_active_count > 0U || page_write_lock_waiting_count > 0U) {
        return false;
    }

    mylite_ownerless_dictionary_state_snapshot dictionary_snapshot = {};
    if (mylite_ownerless_dictionary_state_read_snapshot(
            base + k_concurrency_dictionary_state_offset,
            k_concurrency_dictionary_state_segment_size,
            &dictionary_snapshot
        ) != MYLITE_OWNERLESS_DICTIONARY_STATE_OK ||
        (dictionary_snapshot.generation & 1U) != 0U || dictionary_snapshot.active_owner_id != 0U) {
        return false;
    }

    mylite_ownerless_redo_state_snapshot redo_snapshot = {};
    if (mylite_ownerless_redo_state_read_snapshot(
            base + k_concurrency_redo_state_offset,
            k_concurrency_redo_state_segment_size,
            &redo_snapshot
        ) != MYLITE_OWNERLESS_REDO_STATE_OK ||
        redo_snapshot.refcount != 0U || redo_snapshot.active_reservation_count != 0U ||
        redo_snapshot.latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED ||
        redo_snapshot.progress_latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED) {
        return false;
    }

    return true;
}

int snapshot_ownerless_page_version_pins(
    RuntimeState &runtime,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
) {
    void *page_pin_registry = runtime_page_pin_registry(runtime);
    if (page_pin_registry == nullptr || runtime.concurrency_process_slot_generation == 0U ||
        out_active_count == nullptr || out_oldest_read_lsn == nullptr) {
        return MYLITE_IOERR;
    }

    const std::uint32_t owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    const int registry_result = mylite_ownerless_page_pin_registry_snapshot_oldest(
        page_pin_registry,
        k_concurrency_page_pin_registry_segment_size,
        owner_id,
        runtime.concurrency_process_slot_generation,
        out_active_count,
        out_oldest_read_lsn
    );
    return registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK ? MYLITE_OK : MYLITE_IOERR;
}

bool ownerless_runtime_has_external_page_version_pin(RuntimeState &runtime) {
    void *page_pin_registry = runtime_page_pin_registry(runtime);
    if (page_pin_registry == nullptr || runtime.concurrency_process_slot_generation == 0U) {
        return false;
    }

    const std::uint32_t owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    if (owner_id == 0U) {
        return false;
    }

    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    const int snapshot_result = mylite_ownerless_page_pin_registry_snapshot_oldest(
        page_pin_registry,
        k_concurrency_page_pin_registry_segment_size,
        owner_id,
        runtime.concurrency_process_slot_generation,
        &active_pin_count,
        &oldest_pin_lsn
    );
    if (snapshot_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK || active_pin_count == 0U ||
        oldest_pin_lsn == 0U) {
        return false;
    }

    std::uint32_t owner_active_pin_count = 0;
    const int owner_pin_count_result = mylite_ownerless_page_pin_registry_owner_active_count(
        page_pin_registry,
        k_concurrency_page_pin_registry_segment_size,
        owner_id,
        owner_id,
        runtime.concurrency_process_slot_generation,
        &owner_active_pin_count
    );
    return owner_pin_count_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK &&
           active_pin_count > owner_active_pin_count;
}

int initialize_concurrency_dictionary_state(int shm_fd) {
    std::array<unsigned char, k_concurrency_dictionary_state_segment_size> dictionary_state = {};
    if (mylite_ownerless_dictionary_state_initialize(
            dictionary_state.data(),
            dictionary_state.size()
        ) != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               dictionary_state.data(),
               dictionary_state.size(),
               static_cast<off_t>(k_concurrency_dictionary_state_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_autoinc_registry(int shm_fd) {
    std::vector<unsigned char> autoinc_registry(k_concurrency_autoinc_registry_segment_size);
    if (mylite_ownerless_autoinc_registry_initialize(
            autoinc_registry.data(),
            autoinc_registry.size(),
            k_concurrency_autoinc_slot_count
        ) != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               autoinc_registry.data(),
               autoinc_registry.size(),
               static_cast<off_t>(k_concurrency_autoinc_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int replay_concurrency_page_index(void *page_index, std::size_t page_index_size, int page_log_fd) {
    if (page_index == nullptr || page_log_fd < 0) {
        return MYLITE_IOERR;
    }

    OwnerlessPageIndexRebuildContext context = {};
    context.page_index = page_index;
    context.page_index_size = page_index_size;
    context.owner_id = k_concurrency_bootstrap_latch_owner_id;
    context.owner_generation = static_cast<std::uint64_t>(::getpid());
    const int replay_result = mylite_ownerless_page_log_replay_at(
        page_log_fd,
        k_concurrency_recovery_header_size,
        replay_concurrency_page_index_record,
        &context
    );
    return replay_result == MYLITE_OWNERLESS_PAGE_LOG_OK ? MYLITE_OK : MYLITE_IOERR;
}

int replay_concurrency_page_index_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
) {
    auto *rebuild = static_cast<OwnerlessPageIndexRebuildContext *>(context);
    if (rebuild == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return page_log_result_from_page_index_result(mylite_ownerless_page_index_publish(
        rebuild->page_index,
        rebuild->page_index_size,
        rebuild->owner_id,
        rebuild->owner_generation,
        space_id,
        page_no,
        commit_lsn,
        page_lsn,
        record_offset
    ));
}

int collect_ownerless_reclaimed_page_index_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
) {
    auto *reclaim = static_cast<OwnerlessPageLogReclaimContext *>(context);
    if (reclaim == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const mylite_ownerless_page_index_record retained_record{
        space_id,
        page_no,
        commit_lsn,
        page_lsn,
        record_offset,
    };
    reclaim->retained_records.push_back(retained_record);
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int replace_ownerless_page_index_after_reclaim(void *context) {
    auto *reclaim = static_cast<OwnerlessPageLogReclaimContext *>(context);
    if (reclaim == nullptr || reclaim->runtime == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    RuntimeState &runtime = *reclaim->runtime;
    const std::uint32_t owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    const mylite_ownerless_page_index_record *records =
        reclaim->retained_records.empty() ? nullptr : reclaim->retained_records.data();
    const int replace_result = mylite_ownerless_page_index_replace(
        runtime_page_index(runtime),
        k_concurrency_page_index_segment_size,
        owner_id,
        runtime.concurrency_process_slot_generation,
        records,
        reclaim->retained_records.size()
    );
    if (replace_result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }

    static_cast<void>(mylite_ownerless_page_index_require_wal_scan(
        runtime_page_index(runtime),
        k_concurrency_page_index_segment_size,
        owner_id,
        runtime.concurrency_process_slot_generation
    ));
    return page_log_result_from_page_index_result(replace_result);
}

int page_log_result_from_page_index_result(int result) {
    switch (result) {
    case MYLITE_OWNERLESS_PAGE_INDEX_OK:
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    case MYLITE_OWNERLESS_PAGE_INDEX_FULL:
        return MYLITE_OWNERLESS_PAGE_LOG_FULL;
    default:
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
}

int read_concurrency_process_active_count(int shm_fd, std::uint64_t *out_active_count) {
    if (out_active_count == nullptr) {
        return MYLITE_IOERR;
    }

    struct stat shm_stat = {};
    if (::fstat(shm_fd, &shm_stat) != 0 ||
        shm_stat.st_size <
            static_cast<off_t>(
                k_concurrency_process_registry_offset + k_concurrency_process_registry_size
            ) ||
        static_cast<std::uintmax_t>(shm_stat.st_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return MYLITE_IOERR;
    }

    void *mapping = ::mmap(
        nullptr,
        static_cast<std::size_t>(shm_stat.st_size),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );
    if (mapping == MAP_FAILED) {
        return MYLITE_IOERR;
    }

    const auto *registry =
        static_cast<const unsigned char *>(mapping) + k_concurrency_process_registry_offset;
    *out_active_count = mylite_ownerless_process_registry_active_count(registry);
    if (::munmap(mapping, static_cast<std::size_t>(shm_stat.st_size)) != 0) {
        return MYLITE_IOERR;
    }
    return MYLITE_OK;
}

int read_concurrency_process_live_count(int shm_fd, std::uint64_t *out_live_count) {
    if (out_live_count == nullptr) {
        return MYLITE_IOERR;
    }

    struct stat shm_stat = {};
    if (::fstat(shm_fd, &shm_stat) != 0 ||
        shm_stat.st_size <
            static_cast<off_t>(
                k_concurrency_process_registry_offset + k_concurrency_process_registry_size
            ) ||
        static_cast<std::uintmax_t>(shm_stat.st_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return MYLITE_IOERR;
    }

    std::array<unsigned char, k_concurrency_process_registry_header_size> registry_header = {};
    if (!read_exact_at(
            shm_fd,
            registry_header.data(),
            registry_header.size(),
            static_cast<off_t>(k_concurrency_process_registry_offset)
        )) {
        return MYLITE_IOERR;
    }
    if (load_le32(registry_header.data(), k_concurrency_registry_slot_count_offset) !=
            k_concurrency_process_slot_count ||
        load_le32(registry_header.data(), k_concurrency_registry_slot_size_offset) !=
            k_concurrency_process_slot_size) {
        return MYLITE_IOERR;
    }

    void *mapping = ::mmap(
        nullptr,
        static_cast<std::size_t>(shm_stat.st_size),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );
    if (mapping == MAP_FAILED) {
        return MYLITE_IOERR;
    }

    auto *registry = static_cast<unsigned char *>(mapping) + k_concurrency_process_registry_offset;
    std::uint64_t live_count = 0;
    const int registry_result = mylite_ownerless_process_registry_live_count(
        registry,
        k_concurrency_process_registry_size,
        ownerless_process_is_alive,
        nullptr,
        &live_count
    );
    const int unmap_result = ::munmap(mapping, static_cast<std::size_t>(shm_stat.st_size));
    if (registry_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK || unmap_result != 0) {
        return MYLITE_IOERR;
    }

    *out_live_count = live_count;
    return MYLITE_OK;
}

int validate_concurrency_shm_mapping(int shm_fd, off_t shm_size, std::string_view database_uuid) {
    struct stat shm_stat = {};
    if (::fstat(shm_fd, &shm_stat) != 0) {
        return MYLITE_IOERR;
    }
    const ConcurrencyShmFileIdentity shm_identity = concurrency_shm_file_identity(shm_stat);
    if (shm_size < static_cast<off_t>(k_minimum_concurrency_shm_size) ||
        static_cast<std::uintmax_t>(shm_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return MYLITE_IOERR;
    }

    const std::size_t mapping_size = static_cast<std::size_t>(shm_size);
    void *mapping = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (mapping == MAP_FAILED) {
        return MYLITE_IOERR;
    }

    std::array<unsigned char, k_concurrency_shm_header_size> header = {};
    std::memcpy(header.data(), mapping, header.size());
    const bool valid =
        concurrency_shm_header_matches(header, shm_size, shm_identity, database_uuid) &&
        concurrency_shm_segments_match(shm_fd, shm_size);
    const int unmap_result = ::munmap(mapping, mapping_size);
    return valid && unmap_result == 0 ? MYLITE_OK : MYLITE_IOERR;
}

int map_concurrency_shared_memory_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
) {
    const std::filesystem::path shm_path =
        database_path / k_concurrency_dir_name / k_concurrency_shm_filename;
    const std::string shm_name = shm_path.string();
    const int shm_fd = ::open(shm_name.c_str(), O_RDWR | O_CLOEXEC);
    if (shm_fd < 0) {
        return MYLITE_IOERR;
    }

    struct stat shm_stat = {};
    if (::fstat(shm_fd, &shm_stat) != 0 ||
        shm_stat.st_size < static_cast<off_t>(
                               k_concurrency_page_pin_registry_offset +
                               k_concurrency_page_pin_registry_segment_size
                           ) ||
        static_cast<std::uintmax_t>(shm_stat.st_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        static_cast<void>(::close(shm_fd));
        return MYLITE_IOERR;
    }

    const std::size_t mapping_size = static_cast<std::size_t>(shm_stat.st_size);
    void *mapping = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (mapping == MAP_FAILED) {
        static_cast<void>(::close(shm_fd));
        return MYLITE_IOERR;
    }

    runtime.concurrency_shm_fd = shm_fd;
    runtime.concurrency_shm_mapping = mapping;
    runtime.concurrency_shm_mapping_size = mapping_size;
    runtime.ownerless_mdl_hook.lock_table =
        static_cast<unsigned char *>(mapping) + k_concurrency_mdl_lock_table_offset;
    runtime.ownerless_mdl_hook.lock_table_size = k_concurrency_mdl_lock_table_segment_size;
    runtime.ownerless_trx_hook.trx_registry =
        static_cast<unsigned char *>(mapping) + k_concurrency_trx_registry_offset;
    runtime.ownerless_trx_hook.trx_registry_size = k_concurrency_trx_registry_segment_size;
    runtime.ownerless_read_view_hook.read_view_registry =
        static_cast<unsigned char *>(mapping) + k_concurrency_read_view_registry_offset;
    runtime.ownerless_read_view_hook.read_view_registry_size =
        k_concurrency_read_view_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.lock_registry =
        static_cast<unsigned char *>(mapping) + k_concurrency_innodb_lock_registry_offset;
    runtime.ownerless_innodb_lock_hook.lock_registry_size =
        k_concurrency_innodb_lock_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.process_registry = runtime_process_registry(runtime);
    runtime.ownerless_innodb_lock_hook.process_registry_size = k_concurrency_process_registry_size;
    runtime.ownerless_innodb_lock_hook.trx_registry = runtime_trx_registry(runtime);
    runtime.ownerless_innodb_lock_hook.trx_registry_size = k_concurrency_trx_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.autoinc_registry = runtime_autoinc_registry(runtime);
    runtime.ownerless_innodb_lock_hook.autoinc_registry_size =
        k_concurrency_autoinc_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.page_write_lock_registry =
        static_cast<unsigned char *>(mapping) + k_concurrency_page_write_lock_registry_offset;
    runtime.ownerless_innodb_lock_hook.page_write_lock_registry_size =
        k_concurrency_page_write_lock_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.page_pin_registry = runtime_page_pin_registry(runtime);
    runtime.ownerless_innodb_lock_hook.page_pin_registry_size =
        k_concurrency_page_pin_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.redo_state =
        static_cast<unsigned char *>(mapping) + k_concurrency_redo_state_offset;
    runtime.ownerless_innodb_lock_hook.redo_state_size = k_concurrency_redo_state_segment_size;
    runtime.ownerless_innodb_lock_hook.page_index = runtime_page_index(runtime);
    runtime.ownerless_innodb_lock_hook.page_index_size = k_concurrency_page_index_segment_size;
    runtime.ownerless_innodb_lock_hook.database_path = runtime.database_path.c_str();
    const int slot_result = allocate_concurrency_process_slot(runtime);
    if (slot_result != MYLITE_OK) {
        runtime.ownerless_mdl_hook = {};
        runtime.ownerless_trx_hook = {};
        runtime.ownerless_read_view_hook = {};
        runtime.ownerless_innodb_lock_hook = {};
        reset_ownerless_page_log_sync_anchor();
        runtime.concurrency_shm_mapping = nullptr;
        runtime.concurrency_shm_mapping_size = 0;
        runtime.concurrency_shm_fd = -1;
        static_cast<void>(::munmap(mapping, mapping_size));
        static_cast<void>(::close(shm_fd));
        return slot_result;
    }

    return MYLITE_OK;
}

int open_concurrency_page_log_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
) {
    const std::filesystem::path wal_path =
        database_path / k_concurrency_dir_name / k_concurrency_wal_filename;
    const std::string wal_name = wal_path.string();
    const int wal_fd = ::open(wal_name.c_str(), O_RDWR | O_CLOEXEC);
    if (wal_fd < 0) {
        return MYLITE_IOERR;
    }

    const int log_result =
        mylite_ownerless_page_log_initialize_at(wal_fd, k_concurrency_recovery_header_size);
    if (log_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        static_cast<void>(::close(wal_fd));
        return MYLITE_IOERR;
    }

    runtime.concurrency_wal_fd = wal_fd;
    runtime.ownerless_innodb_lock_hook.page_log_fd = wal_fd;
    runtime.ownerless_innodb_lock_hook.page_log_offset = k_concurrency_recovery_header_size;
    reset_ownerless_page_log_sync_anchor();
    return MYLITE_OK;
}

int open_concurrency_checkpoint_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
) {
    const std::filesystem::path checkpoint_path =
        database_path / k_concurrency_dir_name / k_concurrency_checkpoint_filename;
    const std::string checkpoint_name = checkpoint_path.string();
    const int checkpoint_fd = ::open(checkpoint_name.c_str(), O_RDWR | O_CLOEXEC);
    if (checkpoint_fd < 0) {
        return MYLITE_IOERR;
    }
    reset_ownerless_checkpoint_lsn_sync_anchor();

    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(checkpoint_fd, &latest_lsn, &visible_lsn)) {
        static_cast<void>(::close(checkpoint_fd));
        return MYLITE_IOERR;
    }
    if (mylite_ownerless_redo_state_seed_checkpoint(
            runtime.ownerless_innodb_lock_hook.redo_state,
            runtime.ownerless_innodb_lock_hook.redo_state_size,
            latest_lsn,
            visible_lsn
        ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
        static_cast<void>(::close(checkpoint_fd));
        return MYLITE_IOERR;
    }

    runtime.concurrency_checkpoint_fd = checkpoint_fd;
    runtime.ownerless_innodb_lock_hook.checkpoint_fd = checkpoint_fd;
    return MYLITE_OK;
}

int allocate_concurrency_process_slot(RuntimeState &runtime) {
    unsigned char *registry = runtime_process_registry(runtime);
    if (registry == nullptr || runtime.concurrency_shm_fd < 0) {
        return MYLITE_IOERR;
    }
    if (!update_concurrency_shm_state(runtime.concurrency_shm_fd, k_concurrency_shm_state_dirty)) {
        return MYLITE_IOERR;
    }

    OwnerlessProcessCleanupContext cleanup_context = {};
    cleanup_context.lock_table = runtime.ownerless_mdl_hook.lock_table;
    cleanup_context.lock_table_size = runtime.ownerless_mdl_hook.lock_table_size;
    cleanup_context.trx_registry = runtime_trx_registry(runtime);
    cleanup_context.trx_registry_size = k_concurrency_trx_registry_segment_size;
    cleanup_context.read_view_registry = runtime_read_view_registry(runtime);
    cleanup_context.read_view_registry_size = k_concurrency_read_view_registry_segment_size;
    cleanup_context.page_pin_registry = runtime_page_pin_registry(runtime);
    cleanup_context.page_pin_registry_size = k_concurrency_page_pin_registry_segment_size;
    cleanup_context.innodb_lock_registry = runtime_innodb_lock_registry(runtime);
    cleanup_context.innodb_lock_registry_size = k_concurrency_innodb_lock_registry_segment_size;
    cleanup_context.page_write_lock_registry = runtime_page_write_lock_registry(runtime);
    cleanup_context.page_write_lock_registry_size =
        k_concurrency_page_write_lock_registry_segment_size;
    cleanup_context.redo_state = runtime_redo_state(runtime);
    cleanup_context.redo_state_size = k_concurrency_redo_state_segment_size;
    cleanup_context.dictionary_state = runtime_dictionary_state(runtime);
    cleanup_context.dictionary_state_size = k_concurrency_dictionary_state_segment_size;
    cleanup_context.latch_owner_id = k_concurrency_bootstrap_latch_owner_id;
    cleanup_context.latch_owner_generation = static_cast<std::uint64_t>(::getpid());
    std::uint32_t cleaned_slots = 0;
    int registry_result = mylite_ownerless_process_registry_cleanup_dead_with_callback(
        registry,
        k_concurrency_process_registry_size,
        ownerless_process_is_alive,
        nullptr,
        ownerless_process_cleanup_dead_owner_state,
        &cleanup_context,
        &cleaned_slots
    );
    if (registry_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        static_cast<void>(
            update_concurrency_shm_state(runtime.concurrency_shm_fd, k_concurrency_shm_state_clean)
        );
        return mylite_result_from_process_registry_result(registry_result);
    }

    std::uint32_t slot_index = 0;
    std::uint64_t slot_generation = 0;
    registry_result = mylite_ownerless_process_registry_allocate(
        registry,
        k_concurrency_process_registry_size,
        static_cast<std::uint64_t>(::getpid()),
        runtime.readonly_mode ? k_concurrency_process_open_mode_shared_readonly
                              : k_concurrency_process_open_mode_exclusive,
        load_le64(
            static_cast<unsigned char *>(runtime.concurrency_shm_mapping),
            k_concurrency_shm_generation_offset
        ),
        &slot_index,
        &slot_generation
    );
    if (registry_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        static_cast<void>(
            update_concurrency_shm_state(runtime.concurrency_shm_fd, k_concurrency_shm_state_clean)
        );
        return mylite_result_from_process_registry_result(registry_result);
    }

    registry_result = mylite_ownerless_process_registry_heartbeat(
        registry,
        k_concurrency_process_registry_size,
        slot_index,
        slot_generation,
        current_time_milliseconds()
    );
    if (registry_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        static_cast<void>(mylite_ownerless_process_registry_release(
            registry,
            k_concurrency_process_registry_size,
            slot_index,
            slot_generation
        ));
        static_cast<void>(
            update_concurrency_shm_state(runtime.concurrency_shm_fd, k_concurrency_shm_state_clean)
        );
        return mylite_result_from_process_registry_result(registry_result);
    }

    unsigned char *slot = registry + k_concurrency_process_registry_header_size +
                          (slot_index * k_concurrency_process_slot_size);
    store_le64(
        slot,
        k_concurrency_process_slot_wait_channel_offset,
        k_concurrency_wait_channel_offset + k_concurrency_wait_channel_header_size
    );
    store_le64(
        slot,
        k_concurrency_process_slot_wait_channel_count_offset,
        k_concurrency_wait_channel_count
    );

    runtime.concurrency_process_slot_index = slot_index;
    runtime.concurrency_process_slot_generation = slot_generation;
    const std::uint32_t owner_id = ownerless_owner_id_from_slot_index(slot_index);
    runtime.ownerless_mdl_hook.owner_id = owner_id;
    runtime.ownerless_mdl_hook.owner_generation = slot_generation;
    runtime.ownerless_trx_hook.owner_id = owner_id;
    runtime.ownerless_trx_hook.owner_generation = slot_generation;
    runtime.ownerless_read_view_hook.owner_id = owner_id;
    runtime.ownerless_read_view_hook.owner_generation = slot_generation;
    runtime.ownerless_innodb_lock_hook.owner_id = owner_id;
    runtime.ownerless_innodb_lock_hook.owner_generation = slot_generation;
    return MYLITE_OK;
}

int install_ownerless_runtime_lifecycle_hooks(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_process_slot_generation == 0U) {
        return MYLITE_IOERR;
    }

    mylite_ownerless_runtime_set_hooks(ownerless_runtime_may_delete_shared_file_hook, &runtime);
    return MYLITE_OK;
}

int install_ownerless_innodb_lock_hooks(RuntimeState &runtime) {
    if (runtime.ownerless_innodb_lock_hook.lock_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.lock_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.process_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.process_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.trx_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.trx_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.autoinc_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.autoinc_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_write_lock_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_write_lock_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_pin_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_pin_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.redo_state == nullptr ||
        runtime.ownerless_innodb_lock_hook.redo_state_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_index == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_index_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_log_fd < 0 ||
        runtime.ownerless_innodb_lock_hook.page_log_offset == 0U ||
        runtime.ownerless_innodb_lock_hook.checkpoint_fd < 0 ||
        runtime.ownerless_innodb_lock_hook.database_path == nullptr ||
        runtime.ownerless_innodb_lock_hook.owner_id == 0U ||
        runtime.ownerless_innodb_lock_hook.owner_generation == 0U) {
        return MYLITE_IOERR;
    }

    mylite_ownerless_innodb_lock_set_hooks(
        ownerless_innodb_lock_acquire_table_hook,
        ownerless_innodb_lock_release_table_hook,
        ownerless_innodb_lock_wait_table_hook,
        ownerless_innodb_lock_acquire_record_hook,
        ownerless_innodb_lock_release_record_hook,
        ownerless_innodb_lock_acquire_page_write_hook,
        ownerless_innodb_lock_release_page_write_hook,
        ownerless_innodb_lock_release_page_writes_hook,
        ownerless_innodb_lock_wait_record_hook,
        ownerless_innodb_lock_wait_until_table_hook,
        ownerless_innodb_lock_wait_until_record_hook,
        ownerless_innodb_lock_before_record_wait_hook,
        ownerless_innodb_lock_clear_wait_hook,
        ownerless_innodb_redo_enter_hook,
        ownerless_innodb_redo_observe_hook,
        ownerless_innodb_redo_reserve_hook,
        ownerless_innodb_redo_written_hook,
        ownerless_innodb_redo_leave_hook,
        ownerless_innodb_pages_visible_hook,
        ownerless_innodb_page_publish_hook,
        ownerless_innodb_page_read_hook,
        ownerless_innodb_skip_external_page_refresh_hook,
        &runtime.ownerless_innodb_lock_hook
    );
    mylite_ownerless_innodb_lock_set_page_publish_batch_hooks(
        ownerless_innodb_page_publish_batch_begin_hook,
        ownerless_innodb_page_publish_batch_end_hook
    );
    mylite_ownerless_innodb_lock_set_history_proof_publish_pair_hook(
        ownerless_innodb_history_proof_publish_pair_hook
    );
    mylite_ownerless_innodb_lock_set_redo_written_leave_hook(
        ownerless_innodb_redo_written_leave_hook
    );
    mylite_ownerless_innodb_autoinc_set_hooks(
        ownerless_innodb_autoinc_read_hook,
        ownerless_innodb_autoinc_publish_hook,
        &runtime.ownerless_innodb_lock_hook
    );
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    mylite_ownerless_innodb_set_test_faults_enabled(1);
#  else
    mylite_ownerless_innodb_set_test_faults_enabled(0);
#  endif
    return MYLITE_OK;
}

int install_ownerless_runtime_hooks(RuntimeState &runtime) {
    if (runtime.ownerless_mdl_hook.lock_table == nullptr ||
        runtime.ownerless_mdl_hook.lock_table_size == 0U ||
        runtime.ownerless_mdl_hook.owner_id == 0U ||
        runtime.ownerless_mdl_hook.owner_generation == 0U ||
        runtime.ownerless_trx_hook.trx_registry == nullptr ||
        runtime.ownerless_trx_hook.trx_registry_size == 0U ||
        runtime.ownerless_trx_hook.owner_id == 0U ||
        runtime.ownerless_trx_hook.owner_generation == 0U ||
        runtime.ownerless_read_view_hook.read_view_registry == nullptr ||
        runtime.ownerless_read_view_hook.read_view_registry_size == 0U ||
        runtime.ownerless_read_view_hook.owner_id == 0U ||
        runtime.ownerless_read_view_hook.owner_generation == 0U ||
        runtime.ownerless_innodb_lock_hook.lock_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.lock_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.process_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.process_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.autoinc_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.autoinc_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_write_lock_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_write_lock_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_pin_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_pin_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.redo_state == nullptr ||
        runtime.ownerless_innodb_lock_hook.redo_state_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_index == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_index_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_log_fd < 0 ||
        runtime.ownerless_innodb_lock_hook.page_log_offset == 0U ||
        runtime.ownerless_innodb_lock_hook.checkpoint_fd < 0 ||
        runtime.ownerless_innodb_lock_hook.database_path == nullptr ||
        runtime.ownerless_innodb_lock_hook.owner_id == 0U ||
        runtime.ownerless_innodb_lock_hook.owner_generation == 0U) {
        return MYLITE_IOERR;
    }

    const std::uint64_t local_max_trx_id =
        std::max<std::uint64_t>(mylite_ownerless_trx_local_max_id(), k_concurrency_initial_trx_id);
    const int seed_result = mylite_ownerless_trx_registry_ensure_next_id_at_least(
        runtime.ownerless_trx_hook.trx_registry,
        runtime.ownerless_trx_hook.trx_registry_size,
        runtime.ownerless_trx_hook.owner_id,
        runtime.ownerless_trx_hook.owner_generation,
        local_max_trx_id
    );
    if (seed_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return MYLITE_IOERR;
    }

    mylite_ownerless_mdl_set_hooks(
        ownerless_mdl_acquire_hook,
        ownerless_mdl_release_hook,
        &runtime.ownerless_mdl_hook
    );
    mylite_ownerless_trx_set_hooks(
        ownerless_trx_allocate_hook,
        ownerless_trx_register_hook,
        ownerless_trx_assign_no_hook,
        ownerless_trx_deregister_hook,
        ownerless_trx_snapshot_hook,
        &runtime.ownerless_trx_hook
    );
    mylite_ownerless_read_view_set_hooks(
        ownerless_read_view_register_hook,
        ownerless_read_view_deregister_hook,
        ownerless_read_view_snapshot_hook,
        &runtime.ownerless_read_view_hook
    );
    return install_ownerless_innodb_lock_hooks(runtime);
}

int refresh_ownerless_external_pages_before_statement(
    mylite_db &db,
    bool allow_page_version_reads,
    bool allow_global_refresh,
    bool force_native_flush,
    bool *out_page_version_reads_enabled
) {
    OwnerlessDatabasePerfCountedScope refresh_perf_scope(
        OWNERLESS_DATABASE_PERF_REFRESH_CALLS,
        OWNERLESS_DATABASE_PERF_REFRESH_TOTAL_NS
    );
    const bool refresh_perf_enabled = ownerless_database_perf_stats_are_enabled();
    const auto refresh_perf_start = [refresh_perf_enabled]() -> std::uint64_t {
        return refresh_perf_enabled ? ownerless_database_perf_now_ns() : 0U;
    };
    const auto refresh_perf_add_elapsed =
        [refresh_perf_enabled](OwnerlessDatabasePerfStatIndex index, std::uint64_t start_ns) {
            if (refresh_perf_enabled && start_ns != 0U) {
                ownerless_database_perf_stats[index].fetch_add(
                    ownerless_database_perf_now_ns() - start_ns,
                    std::memory_order_relaxed
                );
            }
        };
    const auto refresh_perf_add = [refresh_perf_enabled](OwnerlessDatabasePerfStatIndex index) {
        if (refresh_perf_enabled) {
            ownerless_database_perf_stats[index].fetch_add(1U, std::memory_order_relaxed);
        }
    };

    if (out_page_version_reads_enabled != nullptr) {
        *out_page_version_reads_enabled = false;
    }
    const std::uint64_t observation_token = db.ownerless_page_observation_token;
    mylite_ownerless_innodb_set_external_page_observation_token(observation_token);
    mylite_ownerless_innodb_clear_external_page_visibility();
    if (!allow_page_version_reads) {
        release_ownerless_handle_page_version_pin(db);
    }

    std::uint64_t refresh_stage_start = refresh_perf_start();
    const int dictionary_result =
        refresh_ownerless_dictionary_before_statement(db, allow_global_refresh);
    refresh_perf_add_elapsed(OWNERLESS_DATABASE_PERF_REFRESH_DICTIONARY_NS, refresh_stage_start);
    if (dictionary_result != MYLITE_OK) {
        return dictionary_result;
    }

    if (!allow_page_version_reads && !force_native_flush) {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        if (ownerless_runtime_in_single_owner_epoch_locked(g_runtime)) {
            return MYLITE_OK;
        }
    }

    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    std::uint64_t visible_generation = 0;
    std::uint64_t process_generation = 0;
    std::uint64_t active_trx_count = 0;
    std::uint32_t active_redo_reservation_count = 0;
    bool no_live_explicit_transactions = false;
    bool no_other_live_explicit_transactions = false;
    bool no_other_active_transactions = false;
    bool single_owner_epoch = false;
    bool runtime_started_with_page_version_wal = false;
    bool runtime_consumed_page_version_wal = false;
    void *page_pin_registry = nullptr;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    refresh_stage_start = refresh_perf_start();
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        if (g_runtime.concurrency_shm_mapping == nullptr ||
            g_runtime.ownerless_innodb_lock_hook.redo_state == nullptr) {
            refresh_perf_add_elapsed(
                OWNERLESS_DATABASE_PERF_REFRESH_SHARED_SNAPSHOT_NS,
                refresh_stage_start
            );
            return MYLITE_OK;
        }
        mylite_ownerless_redo_state_snapshot redo_snapshot = {};
        if (mylite_ownerless_redo_state_read_snapshot(
                g_runtime.ownerless_innodb_lock_hook.redo_state,
                g_runtime.ownerless_innodb_lock_hook.redo_state_size,
                &redo_snapshot
            ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
            refresh_perf_add_elapsed(
                OWNERLESS_DATABASE_PERF_REFRESH_SHARED_SNAPSHOT_NS,
                refresh_stage_start
            );
            return MYLITE_IOERR;
        }
        latest_lsn = redo_snapshot.latest_lsn;
        visible_lsn = redo_snapshot.visible_lsn;
        visible_generation = redo_snapshot.visible_generation;
        active_redo_reservation_count = redo_snapshot.active_reservation_count;
        unsigned char *trx_registry = runtime_trx_registry(g_runtime);
        if (trx_registry != nullptr) {
            active_trx_count =
                load_shared64(trx_registry, k_concurrency_trx_header_active_count_offset);
        }
        unsigned char *registry = runtime_process_registry(g_runtime);
        if (registry != nullptr) {
            const std::uint64_t process_active_count =
                mylite_ownerless_process_registry_active_count(registry);
            process_generation = mylite_ownerless_process_registry_generation(registry);
            single_owner_epoch =
                process_active_count == 1U &&
                process_generation == g_runtime.concurrency_process_slot_generation;
            owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
            if (single_owner_epoch) {
                const std::size_t owner_slot_offset =
                    k_concurrency_process_registry_header_size +
                    (static_cast<std::size_t>(g_runtime.concurrency_process_slot_index) *
                     k_concurrency_process_slot_size);
                const std::uint64_t owner_explicit_transaction_count =
                    owner_slot_offset + k_concurrency_process_slot_size <=
                            k_concurrency_process_registry_size
                        ? load_le64(
                              registry + owner_slot_offset,
                              k_concurrency_process_slot_explicit_transaction_count_offset
                          )
                        : 1U;
                no_other_live_explicit_transactions = true;
                no_live_explicit_transactions = owner_explicit_transaction_count == 0U;
            } else {
                no_other_live_explicit_transactions =
                    !ownerless_process_registry_has_other_live_explicit_transactions(
                        registry,
                        k_concurrency_process_registry_size,
                        owner_id
                    );
            }
        }
        owner_generation = g_runtime.concurrency_process_slot_generation;
        page_pin_registry = runtime_page_pin_registry(g_runtime);
        no_other_active_transactions =
            single_owner_epoch || !ownerless_trx_registry_has_other_active_transactions(
                                      &g_runtime.ownerless_innodb_lock_hook
                                  );
        if (!single_owner_epoch) {
            no_live_explicit_transactions =
                ownerless_runtime_has_no_live_explicit_transactions(g_runtime);
        }
        runtime_started_with_page_version_wal =
            g_runtime.ownerless_runtime_started_with_page_version_wal;
        runtime_consumed_page_version_wal =
            g_runtime.ownerless_runtime_consumed_page_version_wal.load(std::memory_order_relaxed);
    }
    refresh_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_REFRESH_SHARED_SNAPSHOT_NS,
        refresh_stage_start
    );

    const std::uint64_t live_read_lsn = std::max(latest_lsn, visible_lsn);
    std::uint64_t refresh_lsn = visible_lsn;
    std::uint64_t page_version_read_lsn = allow_page_version_reads ? visible_lsn : 0U;
    const bool explicit_transaction = ownerless_connection_is_in_explicit_transaction(db);
    bool external_page_version_pin_retains_visible_boundary = false;
    if (allow_page_version_reads && !explicit_transaction && !single_owner_epoch &&
        page_pin_registry != nullptr && owner_id != 0U && owner_generation != 0U) {
        refresh_stage_start = refresh_perf_start();
        std::uint32_t active_pin_count = 0;
        std::uint64_t oldest_pin_lsn = 0;
        std::uint32_t owner_active_pin_count = 0;
        const int snapshot_result = mylite_ownerless_page_pin_registry_snapshot_oldest(
            page_pin_registry,
            k_concurrency_page_pin_registry_segment_size,
            owner_id,
            owner_generation,
            &active_pin_count,
            &oldest_pin_lsn
        );
        const int owner_count_result = mylite_ownerless_page_pin_registry_owner_active_count(
            page_pin_registry,
            k_concurrency_page_pin_registry_segment_size,
            owner_id,
            owner_id,
            owner_generation,
            &owner_active_pin_count
        );
        external_page_version_pin_retains_visible_boundary =
            snapshot_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK &&
            owner_count_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK &&
            active_pin_count > owner_active_pin_count && oldest_pin_lsn != 0U &&
            oldest_pin_lsn <= visible_lsn;
        refresh_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_REFRESH_PIN_SNAPSHOT_NS,
            refresh_stage_start
        );
    }
    const bool ownerless_native_write_state_active =
        active_trx_count != 0U || active_redo_reservation_count != 0U;
    if (allow_page_version_reads && !explicit_transaction &&
        (single_owner_epoch || external_page_version_pin_retains_visible_boundary) &&
        no_other_active_transactions && active_trx_count == 0U &&
        active_redo_reservation_count == 0U && latest_lsn > visible_lsn) {
        page_version_read_lsn = live_read_lsn;
    }
    if (explicit_transaction && allow_page_version_reads) {
        if (db.ownerless_transaction_snapshot_visibility_pinned) {
            page_version_read_lsn = db.ownerless_transaction_snapshot_visible_lsn;
            const int pin_result =
                ensure_ownerless_transaction_page_version_pin(db, page_version_read_lsn);
            if (pin_result != MYLITE_OK) {
                return pin_result;
            }
        } else if (ownerless_transaction_pins_consistent_reads(db)) {
            const bool consistent_read_can_use_live_lsn = no_other_live_explicit_transactions &&
                                                          no_other_active_transactions &&
                                                          active_redo_reservation_count == 0U;
            page_version_read_lsn = consistent_read_can_use_live_lsn ? live_read_lsn : visible_lsn;
            page_version_read_lsn =
                ownerless_monotonic_page_version_read_lsn(db, page_version_read_lsn);
            const int pin_result =
                ensure_ownerless_transaction_page_version_pin(db, page_version_read_lsn);
            if (pin_result != MYLITE_OK) {
                return pin_result;
            }
            db.ownerless_transaction_snapshot_visible_lsn = page_version_read_lsn;
            db.ownerless_transaction_snapshot_visibility_pinned = true;
        } else {
            if (!ownerless_transaction_has_local_write_or_locking_read(db) &&
                no_other_live_explicit_transactions && no_other_active_transactions &&
                active_redo_reservation_count == 0U && latest_lsn > visible_lsn) {
                page_version_read_lsn = live_read_lsn;
            }
            page_version_read_lsn =
                ownerless_monotonic_page_version_read_lsn(db, page_version_read_lsn);
        }
    } else if (allow_page_version_reads) {
        page_version_read_lsn =
            ownerless_monotonic_page_version_read_lsn(db, page_version_read_lsn);
    }
    if (refresh_lsn == 0U && page_version_read_lsn == 0U) {
        if (allow_page_version_reads) {
            refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_BASELINE_PIN_CALLS);
            refresh_stage_start = refresh_perf_start();
            const int baseline_pin_result = ensure_ownerless_handle_page_version_pin(
                db,
                k_ownerless_baseline_page_version_read_pin_lsn
            );
            refresh_perf_add_elapsed(
                OWNERLESS_DATABASE_PERF_REFRESH_BASELINE_PIN_NS,
                refresh_stage_start
            );
            if (baseline_pin_result != MYLITE_OK) {
                return baseline_pin_result;
            }
            // The baseline pin has no external page boundary, but it still
            // marks this statement as an ownerless plain read.
            if (out_page_version_reads_enabled != nullptr) {
                *out_page_version_reads_enabled = true;
            }
        }
        return refresh_ownerless_dictionary_before_statement(db, allow_global_refresh);
    }

    const bool process_generation_changed =
        process_generation != 0U &&
        process_generation != db.ownerless_clean_pages_evicted_generation;
    const bool visible_generation_changed =
        visible_generation != 0U &&
        visible_generation != db.ownerless_clean_pages_evicted_visible_generation;
    const bool local_native_read_covers_page_version =
        db.ownerless_local_native_read_lsn >= page_version_read_lsn;
    const bool local_native_current_read =
        allow_page_version_reads && !explicit_transaction && page_version_read_lsn != 0U &&
        local_native_read_covers_page_version && !runtime_started_with_page_version_wal &&
        !runtime_consumed_page_version_wal && !ownerless_native_write_state_active &&
        no_other_active_transactions && !external_page_version_pin_retains_visible_boundary;
    const bool page_version_reads_enabled = allow_page_version_reads && !local_native_current_read;
    if (local_native_current_read) {
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_LOCAL_NATIVE_CURRENT_READ);
    }
    if (page_version_reads_enabled) {
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_PAGE_VERSION_READS_ENABLED);
    }
    if (out_page_version_reads_enabled != nullptr) {
        *out_page_version_reads_enabled = page_version_reads_enabled;
    }
    const bool handle_page_version_pin_advancing =
        page_version_reads_enabled && db.ownerless_page_version_read_pin_registered &&
        db.ownerless_page_version_read_pin_lsn != 0U &&
        db.ownerless_page_version_read_pin_lsn < page_version_read_lsn;
    const bool current_page_version_read =
        page_version_reads_enabled && !explicit_transaction &&
        page_version_read_lsn == live_read_lsn && !ownerless_native_write_state_active &&
        no_other_active_transactions &&
        ((no_live_explicit_transactions && no_other_live_explicit_transactions) ||
         external_page_version_pin_retains_visible_boundary);
    const bool retained_page_version_read =
        page_version_reads_enabled &&
        ((db.ownerless_page_version_read_lsn != 0U &&
          db.ownerless_page_version_read_lsn <= page_version_read_lsn) ||
         (db.ownerless_local_native_read_lsn != 0U &&
          db.ownerless_local_native_read_lsn >= page_version_read_lsn));
    if (page_version_reads_enabled && page_version_read_lsn != 0U &&
        (page_version_read_lsn > db.ownerless_clean_pages_evicted_lsn ||
         process_generation_changed || visible_generation_changed ||
         handle_page_version_pin_advancing)) {
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_ADVANCE_TRX_HORIZON_CALLS);
        refresh_stage_start = refresh_perf_start();
        advance_ownerless_local_trx_horizon(g_runtime);
        refresh_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_REFRESH_ADVANCE_TRX_HORIZON_NS,
            refresh_stage_start
        );
    }
    if (!explicit_transaction && allow_page_version_reads) {
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_CLOSE_READ_VIEW_CALLS);
        refresh_stage_start = refresh_perf_start();
        mylite_ownerless_innodb_close_current_read_view();
        refresh_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_REFRESH_CLOSE_READ_VIEW_NS,
            refresh_stage_start
        );
    }

    if (local_native_current_read) {
        db.ownerless_observed_lsn = std::max(db.ownerless_observed_lsn, refresh_lsn);
        db.ownerless_observed_visible_lsn =
            std::max(db.ownerless_observed_visible_lsn, refresh_lsn);
        db.ownerless_clean_pages_evicted_lsn =
            std::max(db.ownerless_clean_pages_evicted_lsn, page_version_read_lsn);
        db.ownerless_clean_pages_evicted_generation = process_generation;
        db.ownerless_clean_pages_evicted_visible_generation = visible_generation;
    } else if (allow_global_refresh && force_native_flush && refresh_lsn != 0U) {
        const std::uint64_t native_flush_lsn =
            active_trx_count == 0U && active_redo_reservation_count == 0U ? live_read_lsn
                                                                          : refresh_lsn;
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_NATIVE_FLUSH_CALLS);
        refresh_stage_start = refresh_perf_start();
        mylite_ownerless_innodb_flush_dirty_pages_for_page_writes(native_flush_lsn);
        refresh_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_REFRESH_NATIVE_FLUSH_NS,
            refresh_stage_start
        );
        db.ownerless_observed_lsn = std::max(db.ownerless_observed_lsn, refresh_lsn);
    } else if (allow_global_refresh && refresh_lsn > db.ownerless_observed_lsn) {
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_EXTERNAL_REFRESH_CALLS);
        refresh_stage_start = refresh_perf_start();
        if (!allow_page_version_reads && !force_native_flush) {
            if (!explicit_transaction &&
                db.ownerless_peer_dictionary_refresh_uses_native_visible_boundary) {
                mylite_ownerless_innodb_refresh_buffer_pool_pages_native_visible_boundary(
                    refresh_lsn
                );
            } else {
                mylite_ownerless_innodb_refresh_buffer_pool_pages_preserve(refresh_lsn);
            }
        } else if (retained_page_version_read) {
            mylite_ownerless_innodb_refresh_external_pages_retained(refresh_lsn);
        } else {
            mylite_ownerless_innodb_refresh_external_pages(refresh_lsn);
        }
        refresh_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_REFRESH_EXTERNAL_REFRESH_NS,
            refresh_stage_start
        );
        db.ownerless_observed_lsn = refresh_lsn;
    }
    if (page_version_reads_enabled && page_version_read_lsn != 0U &&
        !db.ownerless_transaction_snapshot_visibility_pinned) {
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_HANDLE_PIN_CALLS);
        refresh_stage_start = refresh_perf_start();
        const int read_pin_result =
            ensure_ownerless_handle_page_version_pin(db, page_version_read_lsn);
        refresh_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_REFRESH_HANDLE_PIN_NS,
            refresh_stage_start
        );
        if (read_pin_result != MYLITE_OK) {
            return read_pin_result;
        }
    }
    if (page_version_reads_enabled && page_version_read_lsn != 0U &&
        (page_version_read_lsn > db.ownerless_clean_pages_evicted_lsn ||
         process_generation_changed || visible_generation_changed)) {
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_CLEAN_PAGE_REFRESH_CALLS);
        refresh_stage_start = refresh_perf_start();
        if (!explicit_transaction && allow_global_refresh) {
            if (process_generation_changed || visible_generation_changed ||
                handle_page_version_pin_advancing) {
                if (retained_page_version_read) {
                    mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_retained_no_skip(
                        page_version_read_lsn
                    );
                } else {
                    mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_no_skip(
                        page_version_read_lsn
                    );
                }
                db.ownerless_preserve_native_recovery_pages = false;
                db.ownerless_pending_post_open_clean_page_refresh_lsn = page_version_read_lsn;
                db.ownerless_pending_post_open_clean_page_refresh_visible_boundary = true;
                db.ownerless_pending_post_open_clean_page_refresh_current_boundary =
                    current_page_version_read;
            } else {
                mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read(
                    page_version_read_lsn
                );
            }
        } else {
            mylite_ownerless_innodb_refresh_buffer_pool_pages(page_version_read_lsn);
        }
        refresh_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_REFRESH_CLEAN_PAGE_REFRESH_NS,
            refresh_stage_start
        );
        db.ownerless_clean_pages_evicted_lsn = page_version_read_lsn;
        db.ownerless_clean_pages_evicted_generation = process_generation;
        db.ownerless_clean_pages_evicted_visible_generation = visible_generation;
    }

    if (!local_native_current_read && allow_global_refresh &&
        refresh_lsn > db.ownerless_observed_visible_lsn) {
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_VISIBILITY_PUSH_CALLS);
        refresh_stage_start = refresh_perf_start();
        const std::uint64_t previous_visible_lsn =
            mylite_ownerless_innodb_push_external_page_visibility(refresh_lsn);
        mylite_ownerless_innodb_refresh_external_space_headers();
        mylite_ownerless_innodb_restore_external_page_visibility(previous_visible_lsn);
        refresh_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_REFRESH_VISIBILITY_PUSH_NS,
            refresh_stage_start
        );
        db.ownerless_observed_visible_lsn = refresh_lsn;
    }

    if (page_version_reads_enabled && page_version_read_lsn != 0U) {
        refresh_perf_add(OWNERLESS_DATABASE_PERF_REFRESH_VISIBILITY_ENABLE_CALLS);
        refresh_stage_start = refresh_perf_start();
        if (current_page_version_read) {
            mylite_ownerless_innodb_enable_current_external_page_visibility(page_version_read_lsn);
        } else {
            mylite_ownerless_innodb_enable_external_page_visibility(page_version_read_lsn);
        }
        mylite_ownerless_innodb_set_retained_external_page_visibility(
            retained_page_version_read ? 1 : 0
        );
        db.ownerless_page_version_read_lsn =
            std::max(db.ownerless_page_version_read_lsn, page_version_read_lsn);
        refresh_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_REFRESH_VISIBILITY_ENABLE_NS,
            refresh_stage_start
        );
    }
    return MYLITE_OK;
}

int read_ownerless_pressure_state(mylite_db &db, OwnerlessPressureState &state) {
    state = OwnerlessPressureState{};
    if (!db.ownerless_rw_open) {
        return MYLITE_OK;
    }
    state.page_log_limit_bytes = db.ownerless_page_log_limit_bytes;

    void *page_pin_registry = nullptr;
    int page_log_fd = -1;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        page_pin_registry = runtime_page_pin_registry(g_runtime);
        page_log_fd = g_runtime.concurrency_wal_fd;
        if (g_runtime.concurrency_process_slot_generation != 0U) {
            owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
            owner_generation = g_runtime.concurrency_process_slot_generation;
        }
    }
    if (page_pin_registry == nullptr || page_log_fd < 0 || owner_id == 0U ||
        owner_generation == 0U) {
        set_error(db, MYLITE_IOERR, "ownerless page-version pressure state is unavailable");
        return MYLITE_IOERR;
    }

    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    const int pin_result = mylite_ownerless_page_pin_registry_snapshot_oldest(
        page_pin_registry,
        k_concurrency_page_pin_registry_segment_size,
        owner_id,
        owner_generation,
        &active_pin_count,
        &oldest_pin_lsn
    );
    if (pin_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        set_error(db, MYLITE_IOERR, "ownerless page-version pressure state could not be read");
        return MYLITE_IOERR;
    }

    struct stat page_log_stat = {};
    if (::fstat(page_log_fd, &page_log_stat) != 0 || page_log_stat.st_size < 0) {
        set_error(db, MYLITE_IOERR, "ownerless page-version WAL size could not be read");
        return MYLITE_IOERR;
    }

    state.active_pin_count = active_pin_count;
    state.oldest_pin_lsn = active_pin_count != 0U ? oldest_pin_lsn : 0U;
    state.page_log_bytes = static_cast<std::uint64_t>(page_log_stat.st_size);
    state.page_log_limit_reached = state.active_pin_count != 0U && state.oldest_pin_lsn != 0U &&
                                   state.page_log_limit_bytes != 0U &&
                                   state.page_log_bytes > k_empty_ownerless_page_log_size &&
                                   state.page_log_bytes >= state.page_log_limit_bytes;
    return MYLITE_OK;
}

int enforce_ownerless_page_log_limit_policy(mylite_db &db, const SqlPolicyTokens &tokens) {
    if (!db.ownerless_rw_open || db.ownerless_page_log_limit_bytes == 0U ||
        !sql_statement_requires_write(tokens)) {
        return MYLITE_OK;
    }

    release_ownerless_handle_page_version_pin(db);
    mylite_ownerless_innodb_close_current_read_view();

    OwnerlessPressureState state;
    const int pressure_result = read_ownerless_pressure_state(db, state);
    if (pressure_result != MYLITE_OK) {
        return pressure_result;
    }
    if (!state.page_log_limit_reached) {
        return MYLITE_OK;
    }

    set_error(db, MYLITE_BUSY, "ownerless page-version WAL pressure limit reached");
    return MYLITE_BUSY;
}

int refresh_ownerless_dictionary_before_statement(mylite_db &db, bool allow_global_refresh) {
    if (!db.ownerless_rw_open || !allow_global_refresh) {
        return MYLITE_OK;
    }

    void *dictionary_state = nullptr;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        dictionary_state = runtime_dictionary_state(g_runtime);
    }
    if (dictionary_state == nullptr) {
        return MYLITE_OK;
    }

    if (ownerless_observed_dictionary_generation_ready(db, dictionary_state)) {
        return MYLITE_OK;
    }

    std::uint64_t generation = 0;
    const int wait_result = mylite_ownerless_dictionary_state_wait_ready(
        dictionary_state,
        k_concurrency_dictionary_state_segment_size,
        ownerless_process_is_alive,
        nullptr,
        k_concurrency_lock_wait_timeout_ms,
        &generation
    );
    if (wait_result != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        set_error(db, MYLITE_BUSY, "ownerless dictionary change is still active or needs recovery");
        return ownerless_dictionary_result_from_state_result(wait_result);
    }

    const std::uint64_t observed_generation = db.ownerless_observed_dictionary_generation;
    if (generation == observed_generation) {
        return MYLITE_OK;
    }
    static_cast<void>(mylite_ownerless_innodb_refresh_to_latest_external_lsn());
    const int flush_result = flush_ownerless_dictionary_cache(db);
    if (flush_result == MYLITE_OK) {
        release_ownerless_handle_page_version_pin(db);
        mylite_ownerless_innodb_close_current_read_view();
        mylite_ownerless_innodb_clear_external_page_observations();
        db.ownerless_page_version_read_lsn = 0;
        db.ownerless_local_native_read_lsn = 0;
        db.ownerless_pending_post_open_clean_page_refresh_lsn = 0;
        db.ownerless_pending_post_open_clean_page_refresh_visible_boundary = false;
        db.ownerless_pending_post_open_clean_page_refresh_current_boundary = false;
        std::uint64_t latest_lsn = 0;
        const int observe_result = mylite_ownerless_innodb_redo_observe(&latest_lsn);
        if (observe_result == MYLITE_OWNERLESS_INNODB_LOCK_OK && latest_lsn != 0U) {
            mylite_ownerless_innodb_refresh_external_space_headers();
            mylite_ownerless_innodb_evict_clean_external_pages();
            db.ownerless_observed_lsn = std::max(db.ownerless_observed_lsn, latest_lsn);
            db.ownerless_observed_visible_lsn =
                std::max(db.ownerless_observed_visible_lsn, latest_lsn);
            db.ownerless_clean_pages_evicted_lsn =
                std::max(db.ownerless_clean_pages_evicted_lsn, latest_lsn);
            std::uint64_t process_generation = 0;
            std::uint64_t visible_generation = 0;
            {
                const std::lock_guard<std::mutex> guard(g_runtime.mutex);
                unsigned char *registry = runtime_process_registry(g_runtime);
                if (registry != nullptr) {
                    process_generation = mylite_ownerless_process_registry_generation(registry);
                }
                if (g_runtime.ownerless_innodb_lock_hook.redo_state != nullptr) {
                    mylite_ownerless_redo_state_snapshot redo_snapshot = {};
                    if (mylite_ownerless_redo_state_read_snapshot(
                            g_runtime.ownerless_innodb_lock_hook.redo_state,
                            g_runtime.ownerless_innodb_lock_hook.redo_state_size,
                            &redo_snapshot
                        ) == MYLITE_OWNERLESS_REDO_STATE_OK) {
                        visible_generation = redo_snapshot.visible_generation;
                    }
                }
            }
            if (process_generation != 0U) {
                db.ownerless_clean_pages_evicted_generation = process_generation;
            }
            if (visible_generation != 0U) {
                db.ownerless_clean_pages_evicted_visible_generation = visible_generation;
            }
        }
        clear_ownerless_insert_foreign_key_cache(db);
        if (db.ownerless_observed_dictionary_generation_initialized) {
            db.ownerless_peer_dictionary_refresh_requires_conservative_write = true;
            db.ownerless_peer_dictionary_refresh_uses_native_visible_boundary = true;
        }
        db.ownerless_observed_dictionary_generation = generation;
        db.ownerless_observed_dictionary_generation_initialized = true;
    }
    return flush_result;
}

bool ownerless_observed_dictionary_generation_ready(const mylite_db &db, void *dictionary_state) {
    if (!db.ownerless_observed_dictionary_generation_initialized || dictionary_state == nullptr) {
        return false;
    }

    mylite_ownerless_dictionary_state_snapshot snapshot = {};
    if (mylite_ownerless_dictionary_state_read_snapshot(
            dictionary_state,
            k_concurrency_dictionary_state_segment_size,
            &snapshot
        ) != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        return false;
    }
    return snapshot.active_owner_id == 0U && (snapshot.generation & 1U) == 0U &&
           snapshot.generation == db.ownerless_observed_dictionary_generation;
}

int flush_ownerless_dictionary_cache(mylite_db &db) {
    if (mysql_query(&db.mysql, "FLUSH TABLES") != 0) {
        set_mariadb_error(db);
        return MYLITE_ERROR;
    }
    const int drain_result = drain_remaining_query_results(db);
    if (drain_result != MYLITE_OK) {
        return drain_result;
    }
    mylite_ownerless_innodb_evict_dictionary_cache();
    return MYLITE_OK;
}

int refresh_ownerless_dictionary_cache_after_stale_engine_error(mylite_db &db) {
    if (!db.ownerless_rw_open) {
        return MYLITE_OK;
    }

    static_cast<void>(mylite_ownerless_innodb_refresh_to_latest_external_lsn());
    const int flush_result = flush_ownerless_dictionary_cache(db);
    if (flush_result == MYLITE_OK) {
        clear_ownerless_insert_foreign_key_cache(db);
    }
    return flush_result;
}

void clear_ownerless_insert_foreign_key_cache(mylite_db &db) {
    db.ownerless_insert_foreign_key_cache.clear();
    db.ownerless_insert_auto_increment_cache.clear();
}

void initialize_ownerless_dictionary_generation(mylite_db &db) {
    if (!db.ownerless_rw_open) {
        return;
    }

    void *dictionary_state = nullptr;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        dictionary_state = runtime_dictionary_state(g_runtime);
    }
    if (dictionary_state == nullptr) {
        return;
    }

    std::uint64_t generation = 0;
    if (mylite_ownerless_dictionary_state_wait_ready(
            dictionary_state,
            k_concurrency_dictionary_state_segment_size,
            ownerless_process_is_alive,
            nullptr,
            k_concurrency_lock_wait_timeout_ms,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        db.ownerless_observed_dictionary_generation = generation;
        db.ownerless_observed_dictionary_generation_initialized = true;
    }
}

bool ownerless_temporary_table_ddl_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (token_equals(first, "CREATE")) {
        for (std::size_t index = 1; index < tokens.count; ++index) {
            const std::string_view token = identifier_token_at(tokens, index);
            if (token_equals(token, "TEMPORARY")) {
                return token_equals(identifier_token_at(tokens, index + 1U), "TABLE");
            }
            if (token_equals(token, "TABLE")) {
                return false;
            }
        }
        return false;
    }

    if (!token_equals(first, "DROP")) {
        return false;
    }
    for (std::size_t index = 1; index < tokens.count; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token_equals(token, "TEMPORARY")) {
            return token_equals(identifier_token_at(tokens, index + 1U), "TABLE");
        }
        if (token_equals(token, "TABLE")) {
            return false;
        }
    }
    return false;
}

bool ownerless_dictionary_ddl_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    return token_in(first, "ALTER", "CREATE", "DROP", "RENAME") || token_equals(first, "TRUNCATE");
}

bool ownerless_stale_engine_error_allows_retry(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool statement_started_in_explicit_transaction
) {
    return db.ownerless_rw_open && !statement_started_in_explicit_transaction &&
           db.mariadb_errno == k_mariadb_no_such_table_in_engine_errno &&
           statement_allows_ownerless_page_version_reads(tokens);
}

bool ownerless_dictionary_ddl_needs_native_file_op_checkpoint(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "ALTER") ||
        !has_identifier_token(tokens, "TABLE", 1)) {
        return false;
    }

    bool has_auto_increment = false;
    for (std::size_t index = 1; index < tokens.count; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        has_auto_increment = has_auto_increment || token_equals(token, "AUTO_INCREMENT");
    }
    return has_auto_increment;
}

bool ownerless_table_identifier_token(std::string_view token) {
    return is_sql_identifier_token(token) ||
           (token.size() >= 2U && token.front() == '`' && token.back() == '`');
}

bool ownerless_statement_uses_tracked_temporary_table(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    if (db.ownerless_temporary_table_names.empty()) {
        return false;
    }

    for (std::size_t index = 0; index < tokens.count; ++index) {
        if (!ownerless_table_identifier_token(tokens.values[index])) {
            continue;
        }
        const std::string_view token = unquoted_identifier_token(tokens.values[index]);
        for (const std::string &table_name : db.ownerless_temporary_table_names) {
            if (token_equals(token, table_name.c_str())) {
                return true;
            }
        }
    }
    return false;
}

bool ownerless_statement_uses_temporary_table(const mylite_db &db, const SqlPolicyTokens &tokens) {
    return ownerless_temporary_table_ddl_statement(tokens) ||
           ownerless_statement_uses_tracked_temporary_table(db, tokens);
}

std::string ownerless_temporary_table_name_from_ddl(const SqlPolicyTokens &tokens) {
    bool table_keyword_seen = false;
    std::string table_name;
    for (std::size_t index = 0; index < tokens.count; ++index) {
        const std::string_view token = tokens.values[index];
        if (!table_keyword_seen) {
            table_keyword_seen = ownerless_table_identifier_token(token) &&
                                 token_equals(unquoted_identifier_token(token), "TABLE");
            continue;
        }
        if (token == "(" || token == "," || token == ";") {
            break;
        }
        if (!ownerless_table_identifier_token(token)) {
            continue;
        }
        const std::string_view identifier = unquoted_identifier_token(token);
        if (token_in(identifier, "IF", "NOT", "EXISTS")) {
            continue;
        }
        table_name.assign(identifier.data(), identifier.size());
    }
    return table_name;
}

void update_ownerless_temporary_table_state_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    if (!db.ownerless_rw_open || !ownerless_temporary_table_ddl_statement(tokens)) {
        return;
    }

    const std::string table_name = ownerless_temporary_table_name_from_ddl(tokens);
    if (table_name.empty()) {
        return;
    }

    const std::string_view first = identifier_token_at(tokens, 0);
    if (token_equals(first, "CREATE")) {
        const bool already_tracked = std::any_of(
            db.ownerless_temporary_table_names.begin(),
            db.ownerless_temporary_table_names.end(),
            [&](const std::string &tracked_name) {
                return token_equals(table_name, tracked_name.c_str());
            }
        );
        if (!already_tracked) {
            db.ownerless_temporary_table_names.push_back(table_name);
        }
        return;
    }

    if (!token_equals(first, "DROP")) {
        return;
    }
    db.ownerless_temporary_table_names.erase(
        std::remove_if(
            db.ownerless_temporary_table_names.begin(),
            db.ownerless_temporary_table_names.end(),
            [&](const std::string &tracked_name) {
                return token_equals(table_name, tracked_name.c_str());
            }
        ),
        db.ownerless_temporary_table_names.end()
    );
}

char ownerless_ascii_lower(char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

std::string ownerless_normalized_identifier(std::string_view token) {
    token = unquoted_identifier_token(token);
    std::string normalized;
    normalized.reserve(token.size());
    for (char value : token) {
        normalized.push_back(ownerless_ascii_lower(value));
    }
    return normalized;
}

bool ownerless_tracked_temporary_table_name(const mylite_db &db, std::string_view table_name) {
    for (const std::string &tracked_name : db.ownerless_temporary_table_names) {
        if (token_equals(table_name, tracked_name.c_str())) {
            return true;
        }
    }
    return false;
}

bool ownerless_token_in_any(std::string_view token, std::initializer_list<const char *> keywords) {
    for (const char *keyword : keywords) {
        if (token_equals(token, keyword)) {
            return true;
        }
    }
    return false;
}

bool ownerless_table_reference_stop_token(std::string_view token) {
    return ownerless_token_in_any(
        token,
        {"WHERE",
         "SET",
         "VALUES",
         "VALUE",
         "ON",
         "ORDER",
         "GROUP",
         "HAVING",
         "LIMIT",
         "RETURNING",
         "PROCEDURE",
         "FOR",
         "LOCK",
         "UNION",
         "EXCEPT",
         "INTERSECT",
         "WINDOW",
         "PARTITION"}
    );
}

bool ownerless_table_reference_skip_token(std::string_view token) {
    return ownerless_token_in_any(
        token,
        {"LOW_PRIORITY",
         "DELAYED",
         "HIGH_PRIORITY",
         "IGNORE",
         "QUICK",
         "FROM",
         "INTO",
         "TABLE",
         "ONLY",
         "AS"}
    );
}

std::string_view ownerless_raw_identifier_token_at(
    const SqlPolicyTokens &tokens,
    std::size_t index
) {
    if (index >= tokens.count || !ownerless_table_identifier_token(tokens.values[index])) {
        return {};
    }
    return unquoted_identifier_token(tokens.values[index]);
}

std::size_t ownerless_add_table_statement_lock_key(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t index,
    std::vector<std::string> &keys
) {
    if (index >= tokens.count || !ownerless_table_identifier_token(tokens.values[index])) {
        return index + 1U;
    }

    std::string schema_name;
    std::string table_name;
    std::size_t next_index = index + 1U;
    if (index + 2U < tokens.count && tokens.values[index + 1U] == "." &&
        ownerless_table_identifier_token(tokens.values[index + 2U])) {
        schema_name = ownerless_normalized_identifier(tokens.values[index]);
        table_name = ownerless_normalized_identifier(tokens.values[index + 2U]);
        next_index = index + 3U;
    } else {
        schema_name = ownerless_normalized_identifier(db.current_schema);
        table_name = ownerless_normalized_identifier(tokens.values[index]);
    }

    if (table_name.empty() || ownerless_table_reference_stop_token(table_name) ||
        ownerless_table_reference_skip_token(table_name) ||
        ownerless_tracked_temporary_table_name(db, table_name)) {
        return next_index;
    }

    std::string key;
    if (!schema_name.empty()) {
        key = schema_name + ".";
    }
    key += table_name;
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        keys.push_back(std::move(key));
    }
    return next_index;
}

std::size_t ownerless_collect_table_references_until_clause(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t index,
    std::vector<std::string> &keys
) {
    while (index < tokens.count) {
        const std::string_view token = ownerless_raw_identifier_token_at(tokens, index);
        if (ownerless_table_reference_stop_token(token)) {
            break;
        }
        if (ownerless_table_reference_skip_token(token) || ownerless_token_in_any(
                                                               token,
                                                               {"JOIN",
                                                                "INNER",
                                                                "LEFT",
                                                                "RIGHT",
                                                                "FULL",
                                                                "CROSS",
                                                                "OUTER",
                                                                "STRAIGHT_JOIN",
                                                                "NATURAL",
                                                                "USE",
                                                                "FORCE",
                                                                "IGNORE",
                                                                "KEY",
                                                                "INDEX"}
                                                           )) {
            ++index;
            continue;
        }
        if (tokens.values[index] == "," || tokens.values[index] == "(" ||
            tokens.values[index] == ")") {
            ++index;
            continue;
        }
        if (ownerless_table_identifier_token(tokens.values[index])) {
            index = ownerless_add_table_statement_lock_key(db, tokens, index, keys);
            continue;
        }
        ++index;
    }
    return index;
}

std::size_t ownerless_first_write_keyword_index(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index < tokens.count; ++index) {
        const std::string_view token = ownerless_raw_identifier_token_at(tokens, index);
        if (token_in(token, "DELETE", "INSERT", "LOAD", "REPLACE") ||
            token_equals(token, "UPDATE")) {
            return index;
        }
    }
    return tokens.count;
}

void ownerless_collect_update_statement_lock_keys(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t update_index,
    std::vector<std::string> &keys
) {
    bool direct_table_seen = false;
    for (std::size_t index = update_index + 1U; index < tokens.count;) {
        const std::string_view token = ownerless_raw_identifier_token_at(tokens, index);
        if (token_equals(token, "SET")) {
            return;
        }
        if (ownerless_table_reference_skip_token(token)) {
            ++index;
            continue;
        }
        if (ownerless_token_in_any(
                token,
                {"JOIN",
                 "INNER",
                 "LEFT",
                 "RIGHT",
                 "FULL",
                 "CROSS",
                 "OUTER",
                 "STRAIGHT_JOIN",
                 "NATURAL"}
            )) {
            index = ownerless_collect_table_references_until_clause(db, tokens, index + 1U, keys);
            continue;
        }
        if (!direct_table_seen && ownerless_table_identifier_token(tokens.values[index])) {
            index = ownerless_add_table_statement_lock_key(db, tokens, index, keys);
            direct_table_seen = true;
            continue;
        }
        if (tokens.values[index] == ",") {
            direct_table_seen = false;
        }
        ++index;
    }
}

void ownerless_collect_insert_statement_lock_keys(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t insert_index,
    std::vector<std::string> &keys
) {
    std::size_t index = insert_index + 1U;
    while (index < tokens.count &&
           ownerless_table_reference_skip_token(ownerless_raw_identifier_token_at(tokens, index))) {
        ++index;
    }
    if (index < tokens.count) {
        ownerless_add_table_statement_lock_key(db, tokens, index, keys);
    }
}

void ownerless_collect_delete_statement_lock_keys(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t delete_index,
    std::vector<std::string> &keys
) {
    for (std::size_t index = delete_index + 1U; index < tokens.count; ++index) {
        const std::string_view token = ownerless_raw_identifier_token_at(tokens, index);
        if (token_in(token, "FROM", "USING")) {
            ownerless_collect_table_references_until_clause(db, tokens, index + 1U, keys);
            return;
        }
    }
}

void ownerless_collect_load_statement_lock_keys(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t load_index,
    std::vector<std::string> &keys
) {
    for (std::size_t index = load_index + 1U; index + 1U < tokens.count; ++index) {
        if (token_equals(ownerless_raw_identifier_token_at(tokens, index), "INTO") &&
            token_equals(ownerless_raw_identifier_token_at(tokens, index + 1U), "TABLE")) {
            ownerless_add_table_statement_lock_key(db, tokens, index + 2U, keys);
            return;
        }
    }
}

std::uint64_t ownerless_statement_lock_hash(std::string_view key) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (char value : key) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<OwnerlessStatementLockRequest> ownerless_autocommit_write_statement_lock_requests(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    std::vector<OwnerlessStatementLockRequest> requests;
    if (ownerless_connection_is_in_explicit_transaction(db) ||
        ownerless_dictionary_ddl_statement(tokens) || !sql_statement_requires_write(tokens)) {
        return requests;
    }

    const bool temporary_statement = ownerless_temporary_table_ddl_statement(tokens) ||
                                     ownerless_statement_uses_tracked_temporary_table(db, tokens);
    std::vector<std::string> keys;
    const std::size_t write_index = ownerless_first_write_keyword_index(tokens);
    if (write_index < tokens.count) {
        const std::string_view write_keyword =
            ownerless_raw_identifier_token_at(tokens, write_index);
        if (token_equals(write_keyword, "UPDATE")) {
            ownerless_collect_update_statement_lock_keys(db, tokens, write_index, keys);
        } else if (token_in(write_keyword, "INSERT", "REPLACE")) {
            ownerless_collect_insert_statement_lock_keys(db, tokens, write_index, keys);
        } else if (token_equals(write_keyword, "DELETE")) {
            ownerless_collect_delete_statement_lock_keys(db, tokens, write_index, keys);
        } else if (token_equals(write_keyword, "LOAD")) {
            ownerless_collect_load_statement_lock_keys(db, tokens, write_index, keys);
        }
    }

    if (keys.empty()) {
        if (!temporary_statement) {
            requests.push_back(
                {k_global_write_statement_lock_start, k_global_write_statement_lock_length, F_WRLCK}
            );
        }
        return requests;
    }

    std::vector<off_t> table_offsets;
    table_offsets.reserve(keys.size());
    for (const std::string &key : keys) {
        const std::uint64_t slot =
            ownerless_statement_lock_hash(key) % k_table_statement_lock_slot_count;
        table_offsets.push_back(k_table_statement_lock_start + static_cast<off_t>(slot));
    }
    std::sort(table_offsets.begin(), table_offsets.end());
    table_offsets.erase(
        std::unique(table_offsets.begin(), table_offsets.end()),
        table_offsets.end()
    );

    requests.push_back(
        {k_global_write_statement_lock_start, k_global_write_statement_lock_length, F_RDLCK}
    );
    for (const off_t table_offset : table_offsets) {
        requests.push_back({table_offset, k_table_statement_lock_length, F_WRLCK});
    }
    return requests;
}

int ownerless_statement_lock_fd(mylite_db &db) {
    const std::filesystem::path lock_path = std::filesystem::path(db.database_path) /
                                            k_concurrency_dir_name / k_statement_lock_filename;
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ownerless_statement_lock_fd >= 0) {
        return g_runtime.ownerless_statement_lock_fd;
    }

    const std::string lock_name = lock_path.string();
    const int lock_fd = ::open(lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        set_error(db, MYLITE_IOERR, "ownerless statement lock file could not be opened");
        return -1;
    }

    g_runtime.ownerless_statement_lock_fd = lock_fd;
    return g_runtime.ownerless_statement_lock_fd;
}

int ownerless_runtime_statement_lock_fd(RuntimeState &runtime) {
    if (runtime.ownerless_statement_lock_fd >= 0) {
        return runtime.ownerless_statement_lock_fd;
    }

    const std::filesystem::path lock_path = std::filesystem::path(runtime.database_path) /
                                            k_concurrency_dir_name / k_statement_lock_filename;
    const std::string lock_name = lock_path.string();
    const int lock_fd = ::open(lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        return -1;
    }

    runtime.ownerless_statement_lock_fd = lock_fd;
    return runtime.ownerless_statement_lock_fd;
}

bool acquire_ownerless_live_reclaim_statement_gate(
    RuntimeState &runtime,
    OwnerlessStatementLocks &lock
) {
    lock.release();
    const int lock_fd = ownerless_runtime_statement_lock_fd(runtime);
    if (lock_fd < 0) {
        return false;
    }

    if (!acquire_fd_range_lock(
            lock_fd,
            k_dictionary_statement_lock_start,
            k_dictionary_statement_lock_length,
            F_WRLCK,
            0U
        )) {
        return false;
    }
    lock.add(lock_fd, k_dictionary_statement_lock_start, k_dictionary_statement_lock_length);
    if (!acquire_fd_range_lock(
            lock_fd,
            k_global_write_statement_lock_start,
            k_global_write_statement_lock_length,
            F_WRLCK,
            0U
        )) {
        lock.release();
        return false;
    }
    lock.add(lock_fd, k_global_write_statement_lock_start, k_global_write_statement_lock_length);
    return true;
}

int acquire_ownerless_statement_locks(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    OwnerlessStatementLocks &lock
) {
    lock.release();
    if (!db.ownerless_rw_open) {
        return MYLITE_OK;
    }

    const bool dictionary_ddl = ownerless_dictionary_ddl_statement(tokens);
    const bool transaction_end_with_local_write =
        ownerless_transaction_end_has_local_write(db, tokens);
    const bool statement_requires_read_lock = transaction_end_with_local_write ||
                                              sql_statement_requires_write(tokens) ||
                                              sql_statement_uses_locking_read(tokens);
    short lock_type = F_UNLCK;
    if (dictionary_ddl) {
        lock_type = F_WRLCK;
    } else if (statement_requires_read_lock) {
        lock_type = F_RDLCK;
    } else {
        return MYLITE_OK;
    }

    const int lock_fd = ownerless_statement_lock_fd(db);
    if (lock_fd < 0) {
        return MYLITE_IOERR;
    }
    const unsigned statement_lock_timeout_ms = db.ownerless_statement_lock_wait_timeout_ms;
    if (!acquire_fd_range_lock(
            lock_fd,
            k_dictionary_statement_lock_start,
            k_dictionary_statement_lock_length,
            lock_type,
            statement_lock_timeout_ms
        )) {
        set_error(db, MYLITE_BUSY, "ownerless dictionary statement lock is busy");
        return MYLITE_BUSY;
    }

    lock.add(lock_fd, k_dictionary_statement_lock_start, k_dictionary_statement_lock_length);
    std::vector<OwnerlessStatementLockRequest> table_write_locks;
    if (transaction_end_with_local_write) {
        const OwnerlessStatementLockRequest request{
            k_global_write_statement_lock_start,
            k_global_write_statement_lock_length,
            F_WRLCK
        };
        if (acquire_fd_range_lock(lock_fd, request.start, request.length, request.lock_type, 0U)) {
            lock.add(lock_fd, request.start, request.length);
        } else if (!ownerless_transaction_end_blocks_waiting_native_lock(db)) {
            if (!acquire_fd_range_lock(
                    lock_fd,
                    request.start,
                    request.length,
                    request.lock_type,
                    statement_lock_timeout_ms
                )) {
                set_error(db, MYLITE_BUSY, "ownerless table write statement lock is busy");
                return MYLITE_BUSY;
            }
            lock.add(lock_fd, request.start, request.length);
        }
        return MYLITE_OK;
    } else {
        table_write_locks = ownerless_autocommit_write_statement_lock_requests(db, tokens);
    }
    for (const OwnerlessStatementLockRequest &request : table_write_locks) {
        if (!acquire_fd_range_lock(
                lock_fd,
                request.start,
                request.length,
                request.lock_type,
                statement_lock_timeout_ms
            )) {
            set_error(db, MYLITE_BUSY, "ownerless table write statement lock is busy");
            return MYLITE_BUSY;
        }
        lock.add(lock_fd, request.start, request.length);
    }
    return MYLITE_OK;
}

int ownerless_begin_dictionary_ddl(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool *out_ddl_started
) {
    if (out_ddl_started == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_ddl_started = false;
    if (!db.ownerless_rw_open || !ownerless_dictionary_ddl_statement(tokens)) {
        return MYLITE_OK;
    }

    void *dictionary_state = nullptr;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        dictionary_state = runtime_dictionary_state(g_runtime);
        owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
        owner_generation = g_runtime.concurrency_process_slot_generation;
    }
    if (dictionary_state == nullptr || owner_id == 0U || owner_generation == 0U) {
        set_error(db, MYLITE_IOERR, "ownerless dictionary state is unavailable");
        return MYLITE_IOERR;
    }

    std::uint64_t generation = 0;
    const int begin_result = mylite_ownerless_dictionary_state_begin_ddl(
        dictionary_state,
        k_concurrency_dictionary_state_segment_size,
        owner_id,
        owner_generation,
        static_cast<std::uint64_t>(::getpid()),
        k_concurrency_lock_wait_timeout_ms,
        &generation
    );
    if (begin_result != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        set_error(db, MYLITE_BUSY, "ownerless dictionary change could not start");
        return ownerless_dictionary_result_from_state_result(begin_result);
    }

    db.ownerless_observed_dictionary_generation = generation;
    db.ownerless_observed_dictionary_generation_initialized = true;
    clear_ownerless_insert_foreign_key_cache(db);
    *out_ddl_started = true;
    static_cast<void>(mylite_ownerless_innodb_take_file_rename_redo());
    pause_for_ownerless_test_fault("dictionary-after-begin");
    return MYLITE_OK;
}

int ownerless_finish_dictionary_ddl(mylite_db &db, bool ddl_started) {
    if (!ddl_started) {
        return MYLITE_OK;
    }

    pause_for_ownerless_test_fault("dictionary-before-finish");

    void *dictionary_state = nullptr;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        dictionary_state = runtime_dictionary_state(g_runtime);
        owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
        owner_generation = g_runtime.concurrency_process_slot_generation;
    }
    if (dictionary_state == nullptr || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_IOERR;
    }

    std::uint64_t generation = 0;
    const int finish_result = mylite_ownerless_dictionary_state_finish_ddl(
        dictionary_state,
        k_concurrency_dictionary_state_segment_size,
        owner_id,
        owner_generation,
        &generation
    );
    if (finish_result == MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        clear_ownerless_insert_foreign_key_cache(db);
        db.ownerless_observed_dictionary_generation = generation;
        db.ownerless_observed_dictionary_generation_initialized = true;
        pause_for_ownerless_test_fault("dictionary-after-finish");
        return MYLITE_OK;
    }
    return ownerless_dictionary_result_from_state_result(finish_result);
}

int ownerless_dictionary_result_from_state_result(int state_result) {
    if (state_result == MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        return MYLITE_OK;
    }
    if (state_result == MYLITE_OWNERLESS_DICTIONARY_STATE_BUSY ||
        state_result == MYLITE_OWNERLESS_DICTIONARY_STATE_TIMEOUT) {
        return MYLITE_BUSY;
    }
    return MYLITE_IOERR;
}

bool statement_allows_ownerless_page_version_reads(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "SELECT", "WITH")) {
        return false;
    }
    if (!ownerless_select_statement_has_table_reference(tokens)) {
        return false;
    }

    return !has_identifier_token(tokens, "UPDATE", 1) &&
           !has_identifier_token(tokens, "SHARE", 1) && !has_identifier_token(tokens, "LOCK", 1);
}

bool statement_is_tableless_ownerless_plain_read(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "SELECT", "WITH")) {
        return false;
    }
    if (ownerless_select_statement_has_table_reference(tokens)) {
        return false;
    }
    return !sql_statement_uses_locking_read(tokens);
}

bool ownerless_select_statement_has_table_reference(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 1U; index < tokens.count; ++index) {
        if (token_in(tokens.values[index], "FROM", "JOIN")) {
            return true;
        }
    }
    return false;
}

bool ownerless_connection_is_in_explicit_transaction(const mylite_db &db) {
    if (db.ownerless_explicit_transaction_active) {
        return true;
    }

    const bool server_in_transaction = (db.mysql.server_status & SERVER_STATUS_IN_TRANS) != 0U;
    const bool server_autocommit = (db.mysql.server_status & SERVER_STATUS_AUTOCOMMIT) != 0U;
    return server_in_transaction && !server_autocommit;
}

bool ownerless_transaction_has_local_write_or_locking_read(const mylite_db &db) {
    return db.ownerless_transaction_has_local_write || db.ownerless_transaction_has_locking_read;
}

void reset_ownerless_transaction_visible_fast_proof(mylite_db &db) {
    db.ownerless_transaction_visible_fast_commit_candidate = false;
    db.ownerless_transaction_visible_fast_commit_disqualified = false;
}

void disqualify_ownerless_transaction_visible_fast_proof(mylite_db &db) {
    db.ownerless_transaction_visible_fast_commit_candidate = false;
    db.ownerless_transaction_visible_fast_commit_disqualified = true;
}

void update_ownerless_explicit_transaction_visible_fast_proof_before_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool statement_visible_fast_path
) {
    if (!ownerless_connection_is_in_explicit_transaction(db) ||
        sql_ends_explicit_transaction(tokens)) {
        return;
    }

    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    if (token_equals(first, "SAVEPOINT") ||
        (token_equals(first, "RELEASE") && token_equals(second, "SAVEPOINT")) ||
        (token_equals(first, "ROLLBACK") && token_equals(second, "TO"))) {
        disqualify_ownerless_transaction_visible_fast_proof(db);
        return;
    }

    if (sql_statement_uses_locking_read(tokens)) {
        disqualify_ownerless_transaction_visible_fast_proof(db);
        return;
    }

    if (!sql_statement_requires_write(tokens)) {
        return;
    }

    if (!statement_visible_fast_path) {
        disqualify_ownerless_transaction_visible_fast_proof(db);
        return;
    }

    if (!db.ownerless_transaction_visible_fast_commit_disqualified) {
        db.ownerless_transaction_visible_fast_commit_candidate = true;
    }
}

void disqualify_ownerless_explicit_transaction_visible_fast_proof_after_failed_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool statement_started_in_explicit_transaction
) {
    if (!statement_started_in_explicit_transaction) {
        return;
    }
    if (sql_statement_requires_write(tokens) || sql_statement_uses_locking_read(tokens)) {
        disqualify_ownerless_transaction_visible_fast_proof(db);
    }
}

bool ownerless_connection_allows_global_refresh(
    const mylite_db &db,
    bool allow_page_version_reads
) {
    return !ownerless_connection_is_in_explicit_transaction(db) ||
           (allow_page_version_reads && !ownerless_transaction_has_local_write_or_locking_read(db));
}

std::uint64_t ownerless_handle_observed_read_lsn(const mylite_db &db) {
    return std::max(
        std::max(db.ownerless_observed_visible_lsn, db.ownerless_page_version_read_lsn),
        db.ownerless_local_native_read_lsn
    );
}

std::uint64_t ownerless_monotonic_page_version_read_lsn(
    const mylite_db &db,
    std::uint64_t read_lsn
) {
    if (read_lsn == 0U) {
        return 0U;
    }
    return std::max(
        std::max(read_lsn, db.ownerless_page_version_read_lsn),
        db.ownerless_local_native_read_lsn
    );
}

int update_ownerless_transaction_state_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    update_ownerless_transaction_isolation_after_successful_sql(db, tokens);
    const bool statement_writes = sql_statement_requires_write(tokens);
    const bool statement_uses_locking_read = sql_statement_uses_locking_read(tokens);

    if (sql_starts_explicit_transaction(tokens)) {
        const bool consistent_snapshot = sql_starts_consistent_snapshot_transaction(tokens);
        db.ownerless_active_transaction_isolation =
            db.ownerless_next_transaction_isolation_set
                ? db.ownerless_next_transaction_isolation
                : db.ownerless_session_transaction_isolation;
        db.ownerless_next_transaction_isolation_set = false;
        set_ownerless_explicit_transaction_active(db, true);
        db.ownerless_transaction_has_local_write = false;
        db.ownerless_transaction_has_locking_read = false;
        reset_ownerless_transaction_visible_fast_proof(db);
        db.ownerless_transaction_snapshot_visible_lsn =
            consistent_snapshot && db.ownerless_transaction_snapshot_pin_registered
                ? db.ownerless_transaction_snapshot_pin_lsn
                : (consistent_snapshot ? ownerless_handle_observed_read_lsn(db) : 0U);
        db.ownerless_transaction_snapshot_visibility_pinned = consistent_snapshot;
        if (consistent_snapshot) {
            const int pin_result = ensure_ownerless_transaction_page_version_pin(
                db,
                db.ownerless_transaction_snapshot_visible_lsn
            );
            if (pin_result != MYLITE_OK) {
                static_cast<void>(rollback_active_transaction(db));
                return pin_result;
            }
        }
        return MYLITE_OK;
    }
    if (sql_ends_explicit_transaction(tokens)) {
        release_ownerless_transaction_page_version_pin(db);
        set_ownerless_explicit_transaction_active(db, sql_chains_transaction(tokens));
        db.ownerless_active_transaction_isolation = db.ownerless_session_transaction_isolation;
        db.ownerless_transaction_has_local_write = false;
        db.ownerless_transaction_has_locking_read = false;
        reset_ownerless_transaction_visible_fast_proof(db);
        db.ownerless_transaction_snapshot_visible_lsn = 0;
        db.ownerless_transaction_snapshot_visibility_pinned = false;
        return MYLITE_OK;
    }
    if (ownerless_connection_is_in_explicit_transaction(db)) {
        if (statement_writes) {
            db.ownerless_transaction_has_local_write = true;
        }
        if (statement_uses_locking_read) {
            db.ownerless_transaction_has_locking_read = true;
        }
    }
    if ((db.mysql.server_status & SERVER_STATUS_IN_TRANS) == 0U &&
        (db.mysql.server_status & SERVER_STATUS_AUTOCOMMIT) != 0U) {
        release_ownerless_transaction_page_version_pin(db);
        set_ownerless_explicit_transaction_active(db, false);
        db.ownerless_transaction_has_local_write = false;
        db.ownerless_transaction_has_locking_read = false;
        reset_ownerless_transaction_visible_fast_proof(db);
        db.ownerless_transaction_snapshot_visible_lsn = 0;
        db.ownerless_transaction_snapshot_visibility_pinned = false;
        db.ownerless_active_transaction_isolation = db.ownerless_session_transaction_isolation;
    }
    return MYLITE_OK;
}

bool ownerless_transaction_pins_consistent_reads(const mylite_db &db) {
    return db.ownerless_active_transaction_isolation ==
               OwnerlessTransactionIsolation::RepeatableRead ||
           db.ownerless_active_transaction_isolation == OwnerlessTransactionIsolation::Serializable;
}

int ensure_ownerless_consistent_snapshot_start_pin(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool *out_pin_registered
) {
    if (out_pin_registered == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_pin_registered = false;
    if (!db.ownerless_rw_open || !sql_starts_consistent_snapshot_transaction(tokens) ||
        db.ownerless_transaction_snapshot_pin_registered) {
        return MYLITE_OK;
    }

    std::uint64_t read_lsn = ownerless_handle_observed_read_lsn(db);
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        if (g_runtime.ownerless_innodb_lock_hook.redo_state != nullptr) {
            mylite_ownerless_redo_state_snapshot redo_snapshot = {};
            if (mylite_ownerless_redo_state_read_snapshot(
                    g_runtime.ownerless_innodb_lock_hook.redo_state,
                    g_runtime.ownerless_innodb_lock_hook.redo_state_size,
                    &redo_snapshot
                ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
                set_error(
                    db,
                    MYLITE_IOERR,
                    "database ownerless redo state is unavailable for consistent snapshot"
                );
                return MYLITE_IOERR;
            }

            std::uint64_t active_trx_count = 0;
            unsigned char *trx_registry = runtime_trx_registry(g_runtime);
            if (trx_registry != nullptr) {
                active_trx_count =
                    load_shared64(trx_registry, k_concurrency_trx_header_active_count_offset);
            }
            read_lsn = std::max(read_lsn, redo_snapshot.visible_lsn);
            if (active_trx_count == 0U && redo_snapshot.active_reservation_count == 0U) {
                read_lsn = std::max(
                    read_lsn,
                    std::max(redo_snapshot.latest_lsn, redo_snapshot.visible_lsn)
                );
            }
        }
    }
    read_lsn = ownerless_monotonic_page_version_read_lsn(db, read_lsn);
    if (read_lsn == 0U && !db.readonly_open) {
        std::uint64_t baseline_lsn = 0;
        const int baseline_result =
            seed_ownerless_native_checkpoint_baseline(g_runtime, &baseline_lsn);
        if (baseline_result != MYLITE_OK) {
            set_error(
                db,
                baseline_result,
                "database ownerless native checkpoint baseline is invalid"
            );
            return baseline_result;
        }
        if (baseline_lsn != 0U) {
            read_lsn = baseline_lsn;
            db.ownerless_observed_visible_lsn = baseline_lsn;
        }
    }

    const int pin_result = ensure_ownerless_transaction_page_version_pin(db, read_lsn);
    if (pin_result != MYLITE_OK) {
        return pin_result;
    }
    if (db.ownerless_transaction_snapshot_pin_registered) {
        *out_pin_registered = true;
        pause_for_ownerless_test_fault("consistent-snapshot-after-pin");
    }
    return MYLITE_OK;
}

int open_ownerless_page_version_pin(
    mylite_db &db,
    std::uint64_t read_lsn,
    std::uint32_t *out_slot,
    std::uint64_t *out_generation
) {
    if (out_slot == nullptr || out_generation == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_slot = 0U;
    *out_generation = 0U;
    void *page_pin_registry = nullptr;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        page_pin_registry = runtime_page_pin_registry(g_runtime);
        if (g_runtime.concurrency_process_slot_generation != 0U) {
            owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
            owner_generation = g_runtime.concurrency_process_slot_generation;
        }
    }
    if (page_pin_registry == nullptr || owner_id == 0U || owner_generation == 0U ||
        read_lsn == 0U) {
        set_error(db, MYLITE_IOERR, "ownerless page-version snapshot registry is unavailable");
        return MYLITE_IOERR;
    }

    const int registry_result = mylite_ownerless_page_pin_registry_open(
        page_pin_registry,
        k_concurrency_page_pin_registry_segment_size,
        owner_id,
        owner_generation,
        read_lsn,
        out_slot,
        out_generation
    );
    if (registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return MYLITE_OK;
    }

    const int result = registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_FULL ||
                               registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_TIMEOUT
                           ? MYLITE_BUSY
                           : MYLITE_IOERR;
    set_error(
        db,
        result,
        result == MYLITE_BUSY ? "ownerless page-version snapshot registry is busy"
                              : "ownerless page-version snapshot registry failed"
    );
    return result;
}

bool close_ownerless_page_version_pin(mylite_db &db, std::uint32_t slot, std::uint64_t generation) {
    (void)db;
    if (generation == 0U) {
        return true;
    }
    void *page_pin_registry = nullptr;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        page_pin_registry = runtime_page_pin_registry(g_runtime);
        if (g_runtime.concurrency_process_slot_generation != 0U) {
            owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
            owner_generation = g_runtime.concurrency_process_slot_generation;
        }
    }

    int registry_result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_NOT_FOUND;
    if (page_pin_registry != nullptr && owner_id != 0U && owner_generation != 0U) {
        registry_result = mylite_ownerless_page_pin_registry_close(
            page_pin_registry,
            k_concurrency_page_pin_registry_segment_size,
            owner_id,
            owner_generation,
            slot,
            generation
        );
    }
    return registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK ||
           registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_NOT_FOUND;
}

int ensure_ownerless_handle_page_version_pin(mylite_db &db, std::uint64_t read_lsn) {
    if (!db.ownerless_rw_open || read_lsn == 0U) {
        return MYLITE_OK;
    }
    if (db.ownerless_page_version_read_pin_registered &&
        db.ownerless_page_version_read_pin_lsn >= read_lsn) {
        return MYLITE_OK;
    }

    std::uint32_t slot_index = 0;
    std::uint64_t slot_generation = 0;
    const int pin_result =
        open_ownerless_page_version_pin(db, read_lsn, &slot_index, &slot_generation);
    if (pin_result != MYLITE_OK) {
        return pin_result;
    }

    const bool old_registered = db.ownerless_page_version_read_pin_registered;
    const std::uint32_t old_slot = db.ownerless_page_version_read_pin_slot;
    const std::uint64_t old_generation = db.ownerless_page_version_read_pin_generation;
    db.ownerless_page_version_read_pin_registered = true;
    db.ownerless_page_version_read_pin_slot = slot_index;
    db.ownerless_page_version_read_pin_generation = slot_generation;
    db.ownerless_page_version_read_pin_lsn = read_lsn;
    if (old_registered) {
        static_cast<void>(close_ownerless_page_version_pin(db, old_slot, old_generation));
    }
    return MYLITE_OK;
}

void release_ownerless_handle_page_version_pin(mylite_db &db) {
    if (!db.ownerless_page_version_read_pin_registered) {
        return;
    }

    if (close_ownerless_page_version_pin(
            db,
            db.ownerless_page_version_read_pin_slot,
            db.ownerless_page_version_read_pin_generation
        )) {
        db.ownerless_page_version_read_pin_registered = false;
        db.ownerless_page_version_read_pin_slot = 0;
        db.ownerless_page_version_read_pin_generation = 0;
        db.ownerless_page_version_read_pin_lsn = 0;
    }
}

void release_ownerless_completed_statement_page_visibility(
    mylite_db &db,
    bool close_current_read_view,
    bool release_handle_pin
) {
    if (db.ownerless_active_page_visibility_statement_count != 0U) {
        return;
    }
    if (release_handle_pin) {
        release_ownerless_handle_page_version_pin(db);
    }
    mylite_ownerless_innodb_clear_external_page_visibility();
    if (close_current_read_view) {
        mylite_ownerless_innodb_close_current_read_view();
    }
}

void reset_ownerless_application_read_refresh_state(mylite_db &db) {
    if (!db.ownerless_rw_open) {
        return;
    }

    release_ownerless_handle_page_version_pin(db);
    mylite_ownerless_innodb_close_current_read_view();
    mylite_ownerless_innodb_clear_external_page_observations();
    mylite_ownerless_innodb_evict_dictionary_cache();
    db.ownerless_observed_lsn = 0;
    db.ownerless_observed_visible_lsn = 0;
    db.ownerless_page_version_read_lsn = 0;
    db.ownerless_local_native_read_lsn = 0;
    db.ownerless_pending_post_open_clean_page_refresh_lsn = 0;
    db.ownerless_pending_post_open_clean_page_refresh_visible_boundary = false;
    db.ownerless_pending_post_open_clean_page_refresh_current_boundary = false;
    db.ownerless_clean_pages_evicted_lsn = 0;
    db.ownerless_clean_pages_evicted_generation = 0;
    db.ownerless_clean_pages_evicted_visible_generation = 0;
}

void refresh_ownerless_pending_post_open_clean_pages(mylite_db &db) {
    const std::uint64_t refresh_lsn = db.ownerless_pending_post_open_clean_page_refresh_lsn;
    if (refresh_lsn == 0U) {
        return;
    }

    const std::uint64_t observation_token = db.ownerless_page_observation_token;
    mylite_ownerless_innodb_set_external_page_observation_token(observation_token);
    db.ownerless_pending_post_open_clean_page_refresh_lsn = 0;
    const bool visible_boundary_refresh =
        db.ownerless_pending_post_open_clean_page_refresh_visible_boundary;
    const bool current_boundary_refresh =
        db.ownerless_pending_post_open_clean_page_refresh_current_boundary;
    db.ownerless_pending_post_open_clean_page_refresh_visible_boundary = false;
    db.ownerless_pending_post_open_clean_page_refresh_current_boundary = false;
    (void)current_boundary_refresh;
    const bool retained_page_version_read = db.ownerless_page_version_read_lsn != 0U &&
                                            db.ownerless_page_version_read_lsn <= refresh_lsn;
    mylite_ownerless_innodb_enable_current_external_page_visibility(refresh_lsn);
    mylite_ownerless_innodb_set_retained_external_page_visibility(
        retained_page_version_read ? 1 : 0
    );
    if (visible_boundary_refresh && retained_page_version_read) {
        mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_retained_no_skip(
            refresh_lsn
        );
    } else if (visible_boundary_refresh) {
        mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_no_skip(
            refresh_lsn
        );
    } else {
        mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_no_skip(refresh_lsn);
    }
}

int ensure_ownerless_transaction_page_version_pin(mylite_db &db, std::uint64_t read_lsn) {
    if (!db.ownerless_rw_open || read_lsn == 0U) {
        return MYLITE_OK;
    }
    if (db.ownerless_transaction_snapshot_pin_registered &&
        db.ownerless_transaction_snapshot_pin_lsn == read_lsn) {
        return MYLITE_OK;
    }

    std::uint32_t slot_index = 0;
    std::uint64_t slot_generation = 0;
    const int pin_result =
        open_ownerless_page_version_pin(db, read_lsn, &slot_index, &slot_generation);
    if (pin_result != MYLITE_OK) {
        return pin_result;
    }

    const bool old_registered = db.ownerless_transaction_snapshot_pin_registered;
    const std::uint32_t old_slot = db.ownerless_transaction_snapshot_pin_slot;
    const std::uint64_t old_generation = db.ownerless_transaction_snapshot_pin_generation;
    db.ownerless_transaction_snapshot_pin_registered = true;
    db.ownerless_transaction_snapshot_pin_slot = slot_index;
    db.ownerless_transaction_snapshot_pin_generation = slot_generation;
    db.ownerless_transaction_snapshot_pin_lsn = read_lsn;
    if (old_registered) {
        static_cast<void>(close_ownerless_page_version_pin(db, old_slot, old_generation));
    }
    return MYLITE_OK;
}

void release_ownerless_transaction_page_version_pin(mylite_db &db) {
    if (!db.ownerless_transaction_snapshot_pin_registered) {
        return;
    }

    if (close_ownerless_page_version_pin(
            db,
            db.ownerless_transaction_snapshot_pin_slot,
            db.ownerless_transaction_snapshot_pin_generation
        )) {
        db.ownerless_transaction_snapshot_pin_registered = false;
        db.ownerless_transaction_snapshot_pin_slot = 0;
        db.ownerless_transaction_snapshot_pin_generation = 0;
        db.ownerless_transaction_snapshot_pin_lsn = 0;
    }
}

void update_ownerless_transaction_isolation_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    OwnerlessTransactionIsolation isolation = OwnerlessTransactionIsolation::RepeatableRead;
    bool session_scope = false;
    if (!sql_sets_transaction_isolation(tokens, &isolation, &session_scope)) {
        return;
    }

    if (session_scope) {
        db.ownerless_session_transaction_isolation = isolation;
        if (!ownerless_connection_is_in_explicit_transaction(db)) {
            db.ownerless_active_transaction_isolation = isolation;
        }
        return;
    }

    db.ownerless_next_transaction_isolation = isolation;
    db.ownerless_next_transaction_isolation_set = true;
}

void update_ownerless_statement_lock_timeout_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    unsigned timeout_ms = 0U;
    if (sql_sets_ownerless_statement_lock_timeout(tokens, &timeout_ms)) {
        db.ownerless_statement_lock_wait_timeout_ms = timeout_ms;
    }
}

bool sql_sets_ownerless_statement_lock_timeout(const SqlPolicyTokens &tokens, unsigned *out_ms) {
    if (out_ms == nullptr || !token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1U; index + 2U < tokens.count; ++index) {
        if (!identifier_token_equals(tokens.values[index], "LOCK_WAIT_TIMEOUT") ||
            !is_system_variable_qualified_token(tokens, index)) {
            continue;
        }
        if (index > 0U && token_equals(tokens.values[index - 1U], "GLOBAL") &&
            is_system_variable_assignment_start(tokens, index - 1U)) {
            continue;
        }
        if (index >= 4U && token_equals(tokens.values[index - 1U], ".") &&
            token_equals(tokens.values[index - 2U], "GLOBAL") &&
            token_equals(tokens.values[index - 3U], "@") &&
            token_equals(tokens.values[index - 4U], "@")) {
            continue;
        }

        const std::string_view value = tokens.values[index + 2U];
        if (identifier_token_equals(value, "DEFAULT")) {
            *out_ms = k_statement_lock_wait_timeout_ms;
            return true;
        }
        if (!is_unsigned_decimal(value)) {
            return false;
        }

        std::uint64_t seconds = 0U;
        for (const char digit : value) {
            const std::uint64_t next = static_cast<std::uint64_t>(digit - '0');
            if (seconds > (std::numeric_limits<std::uint64_t>::max() - next) / 10U) {
                *out_ms = std::numeric_limits<unsigned>::max();
                return true;
            }
            seconds = seconds * 10U + next;
        }

        constexpr std::uint64_t k_milliseconds_per_second = 1000U;
        constexpr std::uint64_t k_max_timeout_ms =
            static_cast<std::uint64_t>(std::numeric_limits<unsigned>::max());
        if (seconds >= k_max_timeout_ms / k_milliseconds_per_second) {
            *out_ms = std::numeric_limits<unsigned>::max();
        } else {
            *out_ms = static_cast<unsigned>(seconds * k_milliseconds_per_second);
        }
        return true;
    }
    return false;
}

bool sql_sets_transaction_isolation(
    const SqlPolicyTokens &tokens,
    OwnerlessTransactionIsolation *out_isolation,
    bool *out_session_scope
) {
    if (out_isolation == nullptr || out_session_scope == nullptr ||
        !token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    std::size_t index = 1;
    bool session_scope = false;
    const std::string_view scope = identifier_token_at(tokens, index);
    if (token_in(scope, "SESSION", "LOCAL")) {
        session_scope = true;
        ++index;
    } else if (token_equals(scope, "GLOBAL")) {
        return false;
    }

    if (!token_equals(identifier_token_at(tokens, index), "TRANSACTION") ||
        !token_equals(identifier_token_at(tokens, index + 1U), "ISOLATION") ||
        !token_equals(identifier_token_at(tokens, index + 2U), "LEVEL")) {
        return false;
    }

    const std::string_view first = identifier_token_at(tokens, index + 3U);
    const std::string_view second = identifier_token_at(tokens, index + 4U);
    if (token_equals(first, "READ") && token_equals(second, "UNCOMMITTED")) {
        *out_isolation = OwnerlessTransactionIsolation::ReadUncommitted;
    } else if (token_equals(first, "READ") && token_equals(second, "COMMITTED")) {
        *out_isolation = OwnerlessTransactionIsolation::ReadCommitted;
    } else if (token_equals(first, "REPEATABLE") && token_equals(second, "READ")) {
        *out_isolation = OwnerlessTransactionIsolation::RepeatableRead;
    } else if (token_equals(first, "SERIALIZABLE")) {
        *out_isolation = OwnerlessTransactionIsolation::Serializable;
    } else {
        return false;
    }

    *out_session_scope = session_scope;
    return true;
}

bool sql_starts_consistent_snapshot_transaction(const SqlPolicyTokens &tokens) {
    if (!sql_starts_explicit_transaction(tokens)) {
        return false;
    }

    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "CONSISTENT") &&
            token_equals(identifier_token_at(tokens, index + 1U), "SNAPSHOT")) {
            return true;
        }
    }
    return false;
}

bool sql_starts_explicit_transaction(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "START")) {
        return token_equals(second, "TRANSACTION");
    }
    if (!token_equals(first, "BEGIN")) {
        return false;
    }
    return tokens.count == 1U || token_equals(second, "WORK");
}

bool sql_ends_explicit_transaction(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "COMMIT")) {
        return true;
    }
    return token_equals(first, "ROLLBACK") && !token_equals(second, "TO");
}

bool sql_chains_transaction(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 1; index + 1U < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "AND") &&
            token_equals(identifier_token_at(tokens, index + 1U), "CHAIN")) {
            return true;
        }
    }
    return false;
}

void unmap_concurrency_shared_memory_for_runtime(RuntimeState &runtime) {
    reset_ownerless_runtime_hooks(runtime);
    release_concurrency_owner_state(runtime);
    release_concurrency_process_slot(runtime);

    if (runtime.concurrency_shm_mapping != nullptr) {
        static_cast<void>(
            ::munmap(runtime.concurrency_shm_mapping, runtime.concurrency_shm_mapping_size)
        );
        runtime.concurrency_shm_mapping = nullptr;
        runtime.concurrency_shm_mapping_size = 0;
    }
    if (runtime.concurrency_shm_fd >= 0) {
        static_cast<void>(::close(runtime.concurrency_shm_fd));
        runtime.concurrency_shm_fd = -1;
    }
    if (runtime.concurrency_wal_fd >= 0) {
        static_cast<void>(::close(runtime.concurrency_wal_fd));
        runtime.concurrency_wal_fd = -1;
    }
    if (runtime.concurrency_checkpoint_fd >= 0) {
        static_cast<void>(::close(runtime.concurrency_checkpoint_fd));
        runtime.concurrency_checkpoint_fd = -1;
    }
    if (runtime.ownerless_statement_lock_fd >= 0) {
        static_cast<void>(::close(runtime.ownerless_statement_lock_fd));
        runtime.ownerless_statement_lock_fd = -1;
    }
}

void reset_ownerless_runtime_hooks(RuntimeState &runtime) {
    mylite_ownerless_runtime_reset_hooks();
    reset_ownerless_native_shutdown_hooks(runtime);
    clear_ownerless_native_hook_contexts(runtime);
}

void reset_ownerless_native_shutdown_hooks(RuntimeState &runtime) {
    advance_ownerless_local_trx_horizon(runtime);
    mylite_ownerless_innodb_clear_external_page_visibility();
    mylite_ownerless_innodb_lock_reset_hooks();
    mylite_ownerless_read_view_reset_hooks();
    mylite_ownerless_trx_reset_hooks();
    mylite_ownerless_mdl_reset_hooks();
    (void)runtime;
}

void advance_ownerless_local_trx_horizon(RuntimeState &runtime) {
    OwnerlessTrxHookContext &hook = runtime.ownerless_trx_hook;
    if (hook.trx_registry == nullptr || hook.trx_registry_size == 0U || hook.owner_id == 0U ||
        hook.owner_generation == 0U) {
        return;
    }

    std::uint32_t trx_id_count = 0;
    std::uint64_t next_trx_id = 0;
    std::uint64_t min_trx_no = 0;
    const int snapshot_result = mylite_ownerless_trx_registry_snapshot_read_view(
        hook.trx_registry,
        hook.trx_registry_size,
        nullptr,
        0U,
        hook.owner_id,
        hook.owner_generation,
        &trx_id_count,
        &next_trx_id,
        &min_trx_no
    );
    if ((snapshot_result == MYLITE_OWNERLESS_TRX_REGISTRY_OK ||
         snapshot_result == MYLITE_OWNERLESS_TRX_REGISTRY_FULL) &&
        next_trx_id != 0U) {
        mylite_ownerless_trx_advance_local_max_id_at_least(next_trx_id);
    }
}

void clear_ownerless_native_hook_contexts(RuntimeState &runtime) {
    ownerless_page_log_append_batch_release_current();
    reset_ownerless_page_log_sync_anchor();
    reset_ownerless_checkpoint_lsn_sync_anchor();
    reset_ownerless_checkpoint_lsn_generation_cache();
    runtime.ownerless_innodb_lock_hook = {};
    runtime.ownerless_read_view_hook = {};
    runtime.ownerless_trx_hook = {};
    runtime.ownerless_mdl_hook = {};
}

void reset_ownerless_page_log_sync_anchor() {
    std::lock_guard<std::mutex> guard(g_ownerless_page_log_sync_anchor_mutex);
    g_ownerless_page_log_sync_anchor = {};
}

void reset_ownerless_checkpoint_lsn_sync_anchor() {
    std::lock_guard<std::mutex> guard(g_ownerless_checkpoint_lsn_sync_anchor_mutex);
    g_ownerless_checkpoint_lsn_sync_anchor = {};
}

void reset_ownerless_checkpoint_lsn_generation_cache() {
    std::lock_guard<std::mutex> guard(g_ownerless_checkpoint_lsn_generation_cache_mutex);
    g_ownerless_checkpoint_lsn_generation_cache = {};
}

bool ownerless_checkpoint_lsn_sync_anchor_matches(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
) {
    std::lock_guard<std::mutex> guard(g_ownerless_checkpoint_lsn_sync_anchor_mutex);
    return g_ownerless_checkpoint_lsn_sync_anchor.fd == checkpoint_fd &&
           g_ownerless_checkpoint_lsn_sync_anchor.latest_lsn == latest_lsn &&
           g_ownerless_checkpoint_lsn_sync_anchor.visible_lsn == visible_lsn;
}

void ownerless_checkpoint_lsn_sync_anchor_store(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
) {
    std::lock_guard<std::mutex> guard(g_ownerless_checkpoint_lsn_sync_anchor_mutex);
    g_ownerless_checkpoint_lsn_sync_anchor.fd = checkpoint_fd;
    g_ownerless_checkpoint_lsn_sync_anchor.latest_lsn = latest_lsn;
    g_ownerless_checkpoint_lsn_sync_anchor.visible_lsn = visible_lsn;
}

int sync_ownerless_page_log_if_changed(OwnerlessInnoDBLockHookContext *hook) {
    if (hook == nullptr || hook->page_log_fd < 0 || hook->page_log_offset == 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    std::lock_guard<std::mutex> guard(g_ownerless_page_log_sync_anchor_mutex);
    std::uint64_t known_synced_end_offset = 0;
    std::uint64_t known_synced_generation = 0;
    if (g_ownerless_page_log_sync_anchor.fd == hook->page_log_fd &&
        g_ownerless_page_log_sync_anchor.log_offset == hook->page_log_offset) {
        known_synced_end_offset = g_ownerless_page_log_sync_anchor.end_offset;
        known_synced_generation = g_ownerless_page_log_sync_anchor.generation;
    }

    std::uint64_t current_end_offset = 0;
    std::uint64_t current_generation = 0;
    int synced = 0;
    const int result = mylite_ownerless_page_log_sync_initialized_if_changed_at(
        hook->page_log_fd,
        hook->page_log_offset,
        known_synced_end_offset,
        known_synced_generation,
        &current_end_offset,
        &current_generation,
        &synced
    );
    (void)synced;
    if (result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        g_ownerless_page_log_sync_anchor.fd = hook->page_log_fd;
        g_ownerless_page_log_sync_anchor.log_offset = hook->page_log_offset;
        g_ownerless_page_log_sync_anchor.end_offset = current_end_offset;
        g_ownerless_page_log_sync_anchor.generation = current_generation;
    }
    return result;
}

void release_concurrency_owner_state(RuntimeState &runtime) {
    if (runtime.concurrency_process_slot_generation == 0U) {
        return;
    }

    OwnerlessProcessCleanupContext cleanup_context = {};
    if (runtime.concurrency_shm_mapping != nullptr) {
        auto *mapping = static_cast<unsigned char *>(runtime.concurrency_shm_mapping);
        cleanup_context.lock_table = mapping + k_concurrency_mdl_lock_table_offset;
    }
    cleanup_context.lock_table_size = k_concurrency_mdl_lock_table_segment_size;
    cleanup_context.trx_registry = runtime_trx_registry(runtime);
    cleanup_context.trx_registry_size = k_concurrency_trx_registry_segment_size;
    cleanup_context.read_view_registry = runtime_read_view_registry(runtime);
    cleanup_context.read_view_registry_size = k_concurrency_read_view_registry_segment_size;
    cleanup_context.page_pin_registry = runtime_page_pin_registry(runtime);
    cleanup_context.page_pin_registry_size = k_concurrency_page_pin_registry_segment_size;
    cleanup_context.innodb_lock_registry = runtime_innodb_lock_registry(runtime);
    cleanup_context.innodb_lock_registry_size = k_concurrency_innodb_lock_registry_segment_size;
    cleanup_context.page_write_lock_registry = runtime_page_write_lock_registry(runtime);
    cleanup_context.page_write_lock_registry_size =
        k_concurrency_page_write_lock_registry_segment_size;
    cleanup_context.redo_state = runtime_redo_state(runtime);
    cleanup_context.redo_state_size = k_concurrency_redo_state_segment_size;
    cleanup_context.dictionary_state = runtime_dictionary_state(runtime);
    cleanup_context.dictionary_state_size = k_concurrency_dictionary_state_segment_size;
    cleanup_context.latch_owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    cleanup_context.latch_owner_generation = runtime.concurrency_process_slot_generation;
    static_cast<void>(ownerless_process_cleanup_owner_state(
        runtime.concurrency_process_slot_index,
        runtime.concurrency_process_slot_generation,
        static_cast<std::uint64_t>(::getpid()),
        &cleanup_context
    ));
}

void release_concurrency_process_slot(RuntimeState &runtime) {
    if (runtime.concurrency_process_slot_generation == 0U) {
        return;
    }

    unsigned char *registry = runtime_process_registry(runtime);
    if (registry != nullptr) {
        const int registry_result = mylite_ownerless_process_registry_release(
            registry,
            k_concurrency_process_registry_size,
            runtime.concurrency_process_slot_index,
            runtime.concurrency_process_slot_generation
        );
        if (registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK &&
            runtime.concurrency_shm_fd >= 0 &&
            mylite_ownerless_process_registry_active_count(registry) == 0U) {
            static_cast<void>(update_concurrency_shm_state(
                runtime.concurrency_shm_fd,
                k_concurrency_shm_state_clean
            ));
        }
    }

    runtime.concurrency_process_slot_index = 0;
    runtime.concurrency_process_slot_generation = 0;
}

int ownerless_mdl_acquire_hook(
    const mylite_ownerless_mdl_key_view *key,
    double lock_wait_timeout,
    void *ctx
) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_MDL_ACQUIRE_CALLS,
        OWNERLESS_DATABASE_PERF_MDL_ACQUIRE_NS
    );
    if (key == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_MDL_ERROR;
    }

    auto *hook = static_cast<OwnerlessMdlHookContext *>(ctx);
    if (hook->lock_table == nullptr || hook->lock_table_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_MDL_ERROR;
    }

    if (key->ownerless_mode != MYLITE_OWNERLESS_MDL_MODE_NONE) {
        return ownerless_mdl_result_from_lock_table_result(mylite_ownerless_mdl_acquire_mode(
            hook->lock_table,
            hook->lock_table_size,
            hook->owner_id,
            hook->owner_generation,
            key->namespace_id,
            key->database_name,
            key->object_name,
            key->ownerless_mode,
            ownerless_mdl_timeout_ms(lock_wait_timeout)
        ));
    }
    return MYLITE_OWNERLESS_MDL_OK;
}

void ownerless_mdl_release_hook(const mylite_ownerless_mdl_key_view *key, void *ctx) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_MDL_RELEASE_CALLS,
        OWNERLESS_DATABASE_PERF_MDL_RELEASE_NS
    );
    if (key == nullptr || ctx == nullptr) {
        return;
    }

    auto *hook = static_cast<OwnerlessMdlHookContext *>(ctx);
    if (hook->lock_table == nullptr || hook->lock_table_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return;
    }

    if (key->ownerless_mode != MYLITE_OWNERLESS_MDL_MODE_NONE) {
        static_cast<void>(mylite_ownerless_mdl_release_mode(
            hook->lock_table,
            hook->lock_table_size,
            hook->owner_id,
            hook->owner_generation,
            key->namespace_id,
            key->database_name,
            key->object_name,
            key->ownerless_mode
        ));
    }
}

unsigned ownerless_mdl_timeout_ms(double lock_wait_timeout) {
    if (lock_wait_timeout <= 0.0) {
        return 0U;
    }

    constexpr double k_milliseconds_per_second = 1000.0;
    constexpr double k_max_timeout_ms = static_cast<double>(std::numeric_limits<unsigned>::max());
    const double timeout_ms = lock_wait_timeout * k_milliseconds_per_second;
    if (timeout_ms >= k_max_timeout_ms) {
        return std::numeric_limits<unsigned>::max();
    }
    return static_cast<unsigned>(std::max(timeout_ms, 1.0));
}

int ownerless_mdl_result_from_lock_table_result(int lock_table_result) {
    if (lock_table_result == MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return MYLITE_OWNERLESS_MDL_OK;
    }
    if (lock_table_result == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT) {
        return MYLITE_OWNERLESS_MDL_TIMEOUT;
    }
    return MYLITE_OWNERLESS_MDL_ERROR;
}

int ownerless_trx_allocate_hook(std::uint64_t *out_trx_id, void *ctx) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_TRX_ALLOCATE_CALLS,
        OWNERLESS_DATABASE_PERF_TRX_ALLOCATE_NS
    );
    if (out_trx_id == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    return ownerless_trx_result_from_registry_result(mylite_ownerless_trx_registry_allocate_id(
        hook->trx_registry,
        hook->trx_registry_size,
        hook->owner_id,
        hook->owner_generation,
        out_trx_id
    ));
}

int ownerless_trx_register_hook(std::uint64_t *out_trx_id, void *ctx) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_TRX_REGISTER_CALLS,
        OWNERLESS_DATABASE_PERF_TRX_REGISTER_NS
    );
    if (out_trx_id == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    std::uint32_t slot_index = 0;
    std::uint64_t slot_generation = 0;
    const int result =
        ownerless_trx_result_from_registry_result(mylite_ownerless_trx_registry_begin(
            hook->trx_registry,
            hook->trx_registry_size,
            hook->owner_id,
            hook->owner_generation,
            out_trx_id,
            &slot_index,
            &slot_generation
        ));
    if (result == MYLITE_OWNERLESS_TRX_OK) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("trx-after-register");
#  endif
    }
    return result;
}

int ownerless_trx_assign_no_hook(std::uint64_t trx_id, std::uint64_t *out_trx_no, void *ctx) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_TRX_ASSIGN_NO_CALLS,
        OWNERLESS_DATABASE_PERF_TRX_ASSIGN_NO_NS
    );
    if (out_trx_no == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    int registry_result = mylite_ownerless_trx_registry_assign_new_no(
        hook->trx_registry,
        hook->trx_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        out_trx_no
    );
    if (registry_result == MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND) {
        registry_result = mylite_ownerless_trx_registry_allocate_id(
            hook->trx_registry,
            hook->trx_registry_size,
            hook->owner_id,
            hook->owner_generation,
            out_trx_no
        );
    }
    return ownerless_trx_result_from_registry_result(registry_result);
}

int ownerless_trx_deregister_hook(std::uint64_t trx_id, void *ctx) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_TRX_DEREGISTER_CALLS,
        OWNERLESS_DATABASE_PERF_TRX_DEREGISTER_NS
    );
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    return ownerless_trx_deregister_result_from_registry_result(
        mylite_ownerless_trx_registry_end_by_id(
            hook->trx_registry,
            hook->trx_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id
        )
    );
}

int ownerless_trx_snapshot_hook(
    std::uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_min_trx_no,
    void *ctx
) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_TRX_SNAPSHOT_CALLS,
        OWNERLESS_DATABASE_PERF_TRX_SNAPSHOT_NS
    );
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    return ownerless_trx_result_from_registry_result(
        mylite_ownerless_trx_registry_snapshot_read_view(
            hook->trx_registry,
            hook->trx_registry_size,
            out_trx_ids,
            trx_id_capacity,
            hook->owner_id,
            hook->owner_generation,
            out_trx_id_count,
            out_next_trx_id,
            out_min_trx_no
        )
    );
}

int ownerless_trx_result_from_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return MYLITE_OWNERLESS_TRX_OK;
    }
    if (registry_result == MYLITE_OWNERLESS_TRX_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_TRX_FULL;
    }
    return MYLITE_OWNERLESS_TRX_ERROR;
}

int ownerless_trx_deregister_result_from_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND) {
        return MYLITE_OWNERLESS_TRX_OK;
    }
    return ownerless_trx_result_from_registry_result(registry_result);
}

int ownerless_read_view_register_hook(
    std::uint64_t low_limit_id,
    std::uint64_t low_limit_no,
    const std::uint64_t *trx_ids,
    unsigned int trx_id_count,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation,
    void *ctx
) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_READ_VIEW_REGISTER_CALLS,
        OWNERLESS_DATABASE_PERF_READ_VIEW_REGISTER_NS
    );
    if (out_slot_index == nullptr || out_slot_generation == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    auto *hook = static_cast<OwnerlessReadViewHookContext *>(ctx);
    if (hook->read_view_registry == nullptr || hook->read_view_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    return ownerless_read_view_result_from_registry_result(mylite_ownerless_read_view_registry_open(
        hook->read_view_registry,
        hook->read_view_registry_size,
        hook->owner_id,
        hook->owner_generation,
        low_limit_id,
        low_limit_no,
        trx_ids,
        trx_id_count,
        out_slot_index,
        out_slot_generation
    ));
}

int ownerless_read_view_deregister_hook(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    void *ctx
) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_READ_VIEW_DEREGISTER_CALLS,
        OWNERLESS_DATABASE_PERF_READ_VIEW_DEREGISTER_NS
    );
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    auto *hook = static_cast<OwnerlessReadViewHookContext *>(ctx);
    if (hook->read_view_registry == nullptr || hook->read_view_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    return ownerless_read_view_result_from_registry_result(
        mylite_ownerless_read_view_registry_close(
            hook->read_view_registry,
            hook->read_view_registry_size,
            hook->owner_id,
            hook->owner_generation,
            slot_index,
            slot_generation
        )
    );
}

int ownerless_read_view_snapshot_hook(
    std::uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    std::uint64_t *out_low_limit_id,
    std::uint64_t *out_low_limit_no,
    void *ctx
) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_READ_VIEW_SNAPSHOT_CALLS,
        OWNERLESS_DATABASE_PERF_READ_VIEW_SNAPSHOT_NS
    );
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    auto *hook = static_cast<OwnerlessReadViewHookContext *>(ctx);
    if (hook->read_view_registry == nullptr || hook->read_view_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    return ownerless_read_view_result_from_registry_result(
        mylite_ownerless_read_view_registry_snapshot_oldest(
            hook->read_view_registry,
            hook->read_view_registry_size,
            out_trx_ids,
            trx_id_capacity,
            hook->owner_id,
            hook->owner_generation,
            out_trx_id_count,
            out_low_limit_id,
            out_low_limit_no
        )
    );
}

int ownerless_read_view_result_from_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK) {
        return MYLITE_OWNERLESS_READ_VIEW_OK;
    }
    if (registry_result == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_READ_VIEW_FULL;
    }
    return MYLITE_OWNERLESS_READ_VIEW_ERROR;
}

int ownerless_innodb_lock_acquire_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned int timeout_ms,
    void *ctx
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_TABLE_LOCK_ACQUIRE_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_TABLE_LOCK_ACQUIRE_CALLS, 1U);
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int registry_result = mylite_ownerless_innodb_lock_registry_reserve_table(
        hook->lock_registry,
        hook->lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        table_id,
        mode,
        timeout_ms
    );
    return ownerless_innodb_lock_result_from_registry_result(registry_result);
}

int ownerless_innodb_lock_release_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    void *ctx
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_TABLE_LOCK_RELEASE_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_TABLE_LOCK_RELEASE_CALLS, 1U);
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int registry_result = mylite_ownerless_innodb_lock_registry_release_table(
        hook->lock_registry,
        hook->lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        table_id,
        mode
    );
    return ownerless_innodb_lock_result_from_registry_result(registry_result);
}

int ownerless_innodb_lock_wait_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    std::uint64_t blocker_trx_id,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("table-lock-wait");
#  endif
    const int registry_result = mylite_ownerless_innodb_lock_registry_wait_for_table(
        hook->lock_registry,
        hook->lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        table_id,
        mode,
        hook->owner_id,
        blocker_trx_id
    );
    return ownerless_innodb_lock_result_from_registry_result(registry_result);
}

int ownerless_innodb_lock_wait_until_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned int timeout_ms,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int registry_result = mylite_ownerless_innodb_lock_registry_wait_until_table_available(
        hook->lock_registry,
        hook->lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        table_id,
        mode,
        timeout_ms
    );
    return ownerless_innodb_lock_result_from_registry_result(registry_result);
}

int ownerless_innodb_lock_acquire_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    void *ctx
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_RECORD_LOCK_ACQUIRE_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_RECORD_LOCK_ACQUIRE_CALLS, 1U);
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    const int registry_result = mylite_ownerless_innodb_lock_registry_reserve_record(
        hook->lock_registry,
        hook->lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        index_id,
        space_id,
        page_no,
        normalized_heap_no,
        mode,
        normalized_flags,
        timeout_ms
    );
    const int result = ownerless_innodb_lock_result_from_registry_result(registry_result);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("record-lock-after-acquire");
#  endif
    }
    return result;
}

int ownerless_innodb_lock_release_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_RECORD_LOCK_RELEASE_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_RECORD_LOCK_RELEASE_CALLS, 1U);
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    const int registry_result = mylite_ownerless_innodb_lock_registry_release_record(
        hook->lock_registry,
        hook->lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        index_id,
        space_id,
        page_no,
        normalized_heap_no,
        mode,
        normalized_flags
    );
    return ownerless_innodb_lock_result_from_registry_result(registry_result);
}

int ownerless_innodb_lock_acquire_page_write_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    std::uint32_t *out_acquire_flags,
    void *ctx
) {
    if (out_acquire_flags != nullptr) {
        *out_acquire_flags = 0U;
    }
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->page_write_lock_registry == nullptr || hook->page_write_lock_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    int registry_result = mylite_ownerless_innodb_lock_registry_acquire_record_with_flags(
        hook->page_write_lock_registry,
        hook->page_write_lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        index_id,
        space_id,
        page_no,
        heap_no,
        mode,
        flags,
        0U,
        nullptr
    );
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT &&
        hook->lock_registry != nullptr && hook->lock_registry_size != 0U) {
        registry_result =
            mylite_ownerless_innodb_lock_registry_wait_until_record_available_with_cycle_registry(
                hook->page_write_lock_registry,
                hook->page_write_lock_registry_size,
                hook->lock_registry,
                hook->lock_registry_size,
                hook->owner_id,
                hook->owner_generation,
                trx_id,
                index_id,
                space_id,
                page_no,
                heap_no,
                mode,
                flags,
                timeout_ms
            );
        if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            registry_result = mylite_ownerless_innodb_lock_registry_acquire_record_with_flags(
                hook->page_write_lock_registry,
                hook->page_write_lock_registry_size,
                hook->owner_id,
                hook->owner_generation,
                trx_id,
                index_id,
                space_id,
                page_no,
                heap_no,
                mode,
                flags,
                0U,
                nullptr
            );
            if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK &&
                out_acquire_flags != nullptr) {
                *out_acquire_flags |= MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_WAITED;
            }
        }
    } else if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT) {
        std::uint32_t registry_acquire_flags = 0U;
        registry_result = mylite_ownerless_innodb_lock_registry_acquire_record_with_flags(
            hook->page_write_lock_registry,
            hook->page_write_lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            index_id,
            space_id,
            page_no,
            heap_no,
            mode,
            flags,
            timeout_ms,
            &registry_acquire_flags
        );
        if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK &&
            out_acquire_flags != nullptr &&
            (registry_acquire_flags & MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ACQUIRE_WAITED) != 0U) {
            *out_acquire_flags |= MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_WAITED;
        }
    }
    const int result = ownerless_innodb_lock_result_from_registry_result(registry_result);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK && ownerless_current_statement_db != nullptr) {
        record_ownerless_page_write_trx_id(*ownerless_current_statement_db, trx_id);
    }
    return result;
}

int ownerless_innodb_lock_release_page_write_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->page_write_lock_registry == nullptr || hook->page_write_lock_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_release_record(
            hook->page_write_lock_registry,
            hook->page_write_lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            index_id,
            space_id,
            page_no,
            heap_no,
            mode,
            flags
        )
    );
}

int ownerless_innodb_lock_release_page_writes_hook(std::uint64_t trx_id, void *ctx) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->page_write_lock_registry == nullptr || hook->page_write_lock_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t released_locks = 0;
    const int registry_result = mylite_ownerless_innodb_lock_registry_release_transaction_records(
        hook->page_write_lock_registry,
        hook->page_write_lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        MYLITE_OWNERLESS_INNODB_PAGE_WRITE_INDEX_ID,
        MYLITE_OWNERLESS_INNODB_PAGE_WRITE_HEAP_NO,
        MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
        0U,
        &released_locks
    );
    if ((registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK ||
         registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_NOT_FOUND) &&
        ownerless_current_statement_db != nullptr) {
        forget_ownerless_page_write_trx_id(*ownerless_current_statement_db, trx_id);
    }
    return ownerless_innodb_lock_result_from_registry_result(registry_result);
}

void record_ownerless_page_write_trx_id(mylite_db &db, std::uint64_t trx_id) {
    if (trx_id == 0U) {
        return;
    }
    auto &trx_ids = db.ownerless_page_write_trx_ids;
    if (std::find(trx_ids.begin(), trx_ids.end(), trx_id) == trx_ids.end()) {
        trx_ids.push_back(trx_id);
    }
}

void forget_ownerless_page_write_trx_id(mylite_db &db, std::uint64_t trx_id) {
    if (trx_id == 0U) {
        return;
    }
    auto &trx_ids = db.ownerless_page_write_trx_ids;
    const std::size_t original_size = trx_ids.size();
    trx_ids.erase(std::remove(trx_ids.begin(), trx_ids.end(), trx_id), trx_ids.end());
    if (trx_ids.size() != original_size) {
        ownerless_database_perf_add(
            OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_NATIVE_CLEARED,
            original_size - trx_ids.size()
        );
    }
}

int release_ownerless_page_write_trx_ids(mylite_db &db) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_CALLS,
        OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_NS
    );
    if (db.ownerless_page_write_trx_ids.empty()) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_EMPTY, 1U);
        return MYLITE_OK;
    }
    ownerless_database_perf_add(
        OWNERLESS_DATABASE_PERF_PAGE_WRITE_TRACKED_RELEASE_TRX_IDS,
        db.ownerless_page_write_trx_ids.size()
    );

    void *page_write_lock_registry = nullptr;
    std::size_t page_write_lock_registry_size = 0U;
    std::uint32_t owner_id = 0U;
    std::uint64_t owner_generation = 0U;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        page_write_lock_registry = g_runtime.ownerless_innodb_lock_hook.page_write_lock_registry;
        page_write_lock_registry_size =
            g_runtime.ownerless_innodb_lock_hook.page_write_lock_registry_size;
        owner_id = g_runtime.ownerless_innodb_lock_hook.owner_id;
        owner_generation = g_runtime.ownerless_innodb_lock_hook.owner_generation;
    }

    if (page_write_lock_registry == nullptr || page_write_lock_registry_size == 0U ||
        owner_id == 0U || owner_generation == 0U) {
        return MYLITE_IOERR;
    }

    for (const std::uint64_t trx_id : db.ownerless_page_write_trx_ids) {
        std::uint32_t released_locks = 0U;
        const int registry_result =
            mylite_ownerless_innodb_lock_registry_release_transaction_records(
                page_write_lock_registry,
                page_write_lock_registry_size,
                owner_id,
                owner_generation,
                trx_id,
                MYLITE_OWNERLESS_INNODB_PAGE_WRITE_INDEX_ID,
                MYLITE_OWNERLESS_INNODB_PAGE_WRITE_HEAP_NO,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                0U,
                &released_locks
            );
        if (registry_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK &&
            registry_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_NOT_FOUND) {
            return ownerless_innodb_lock_result_from_registry_result(registry_result) ==
                           MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT
                       ? MYLITE_BUSY
                       : MYLITE_IOERR;
        }
    }
    db.ownerless_page_write_trx_ids.clear();
    return MYLITE_OK;
}

int ownerless_innodb_lock_wait_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    std::uint64_t blocker_trx_id,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    const int registry_result = mylite_ownerless_innodb_lock_registry_wait_for_record(
        hook->lock_registry,
        hook->lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        index_id,
        space_id,
        page_no,
        normalized_heap_no,
        mode,
        normalized_flags,
        hook->owner_id,
        blocker_trx_id
    );
    const int result = ownerless_innodb_lock_result_from_registry_result(registry_result);
    return result;
}

int ownerless_innodb_lock_wait_until_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    const int registry_result =
        hook->page_write_lock_registry != nullptr && hook->page_write_lock_registry_size != 0U
            ? mylite_ownerless_innodb_lock_registry_wait_until_record_available_with_cycle_registry(
                  hook->lock_registry,
                  hook->lock_registry_size,
                  hook->page_write_lock_registry,
                  hook->page_write_lock_registry_size,
                  hook->owner_id,
                  hook->owner_generation,
                  trx_id,
                  index_id,
                  space_id,
                  page_no,
                  normalized_heap_no,
                  mode,
                  normalized_flags,
                  timeout_ms
              )
            : mylite_ownerless_innodb_lock_registry_wait_until_record_available(
                  hook->lock_registry,
                  hook->lock_registry_size,
                  hook->owner_id,
                  hook->owner_generation,
                  trx_id,
                  index_id,
                  space_id,
                  page_no,
                  normalized_heap_no,
                  mode,
                  normalized_flags,
                  timeout_ms
              );
    const int result = ownerless_innodb_lock_result_from_registry_result(registry_result);
    return result;
}

int ownerless_innodb_lock_before_record_wait_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
) {
    if (ctx == nullptr || trx_id == 0U || index_id == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    (void)space_id;
    (void)page_no;
    (void)normalized_heap_no;
    (void)normalized_flags;

#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("record-lock-before-grant");
#  endif
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

void normalize_ownerless_record_lock_resource(
    std::uint32_t mode,
    std::uint32_t *heap_no,
    std::uint32_t *flags
) {
    if (heap_no == nullptr || flags == nullptr ||
        !ownerless_record_lock_uses_page_resource(mode, *flags)) {
        return;
    }

    *heap_no = k_ownerless_innodb_record_page_heap_no;
    *flags = 0U;
}

bool ownerless_record_lock_uses_page_resource(std::uint32_t mode, std::uint32_t flags) {
    if (mode != MYLITE_OWNERLESS_INNODB_LOCK_MODE_X) {
        return false;
    }
    return flags == 0U;
}

int ownerless_innodb_lock_clear_wait_hook(std::uint64_t trx_id, void *ctx) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t cleared_waits = 0;
    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_clear_wait(
            hook->lock_registry,
            hook->lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            &cleared_waits
        )
    );
}

int ownerless_innodb_autoinc_read_hook(
    std::uint64_t table_id,
    std::uint64_t seed_next_value,
    std::uint64_t *out_next_value,
    void *ctx
) {
    if (ctx == nullptr || out_next_value == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->autoinc_registry == nullptr || hook->autoinc_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_autoinc_registry_read_or_seed(
        hook->autoinc_registry,
        hook->autoinc_registry_size,
        hook->owner_id,
        hook->owner_generation,
        table_id,
        seed_next_value,
        out_next_value
    );
    if (result == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_autoinc_publish_hook(
    std::uint64_t table_id,
    std::uint64_t next_value,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->autoinc_registry == nullptr || hook->autoinc_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_autoinc_registry_publish(
        hook->autoinc_registry,
        hook->autoinc_registry_size,
        hook->owner_id,
        hook->owner_generation,
        table_id,
        next_value
    );
    if (result == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_redo_enter_hook(std::uint64_t *out_latest_lsn, void *ctx) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_REDO_ENTER_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_REDO_ENTER_CALLS, 1U);
    if (ctx == nullptr || out_latest_lsn == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (!hook->page_versioning_enabled) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_redo_state_enter(
        hook->redo_state,
        hook->redo_state_size,
        hook->owner_id,
        hook->owner_generation,
        30000U,
        out_latest_lsn
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_redo_observe_hook(std::uint64_t *out_latest_lsn, void *ctx) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_REDO_OBSERVE_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_REDO_OBSERVE_CALLS, 1U);
    if (ctx == nullptr || out_latest_lsn == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (!hook->page_versioning_enabled) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    mylite_ownerless_redo_state_snapshot snapshot = {};
    if (mylite_ownerless_redo_state_read_snapshot(
            hook->redo_state,
            hook->redo_state_size,
            &snapshot
        ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    *out_latest_lsn = snapshot.latest_lsn;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

int ownerless_innodb_redo_reserve_hook(
    std::uint64_t current_lsn,
    std::uint64_t length,
    std::uint64_t *out_start_lsn,
    std::uint64_t *out_end_lsn,
    void *ctx
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_REDO_RESERVE_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_REDO_RESERVE_CALLS, 1U);
    if (ctx == nullptr || length == 0U || out_start_lsn == nullptr || out_end_lsn == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (!hook->page_versioning_enabled) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_redo_state_reserve(
        hook->redo_state,
        hook->redo_state_size,
        hook->owner_id,
        hook->owner_generation,
        current_lsn,
        length,
        out_start_lsn,
        out_end_lsn
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK) {
        pause_for_ownerless_test_fault("redo-after-reserve");
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_redo_written_hook(
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t *out_written_lsn,
    void *ctx
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_REDO_WRITTEN_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_REDO_WRITTEN_CALLS, 1U);
    if (ctx == nullptr || start_lsn == 0U || end_lsn <= start_lsn) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (!hook->page_versioning_enabled) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_redo_state_complete_write(
        hook->redo_state,
        hook->redo_state_size,
        hook->owner_id,
        hook->owner_generation,
        start_lsn,
        end_lsn,
        out_written_lsn
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("redo-after-written");
#  endif
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_redo_written_leave_hook(
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t latest_lsn,
    std::uint64_t *out_written_lsn,
    void *ctx
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_REDO_LEAVE_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_REDO_WRITTEN_CALLS, 1U);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_REDO_LEAVE_CALLS, 1U);
    if (ctx == nullptr || start_lsn == 0U || end_lsn <= start_lsn) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (!hook->page_versioning_enabled) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint64_t advanced_latest_lsn = 0U;
    const int result = mylite_ownerless_redo_state_complete_write_and_leave(
        hook->redo_state,
        hook->redo_state_size,
        hook->owner_id,
        hook->owner_generation,
        start_lsn,
        end_lsn,
        latest_lsn,
        out_written_lsn,
        &advanced_latest_lsn,
        nullptr
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK) {
        if (advanced_latest_lsn != 0U) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
            pause_for_ownerless_test_fault("redo-before-checkpoint");
#  endif
            ownerless_persist_redo_checkpoint(hook, advanced_latest_lsn, 0U, false);
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
            pause_for_ownerless_test_fault("redo-after-checkpoint");
#  endif
        }
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

void ownerless_innodb_redo_leave_hook(std::uint64_t latest_lsn, void *ctx) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_REDO_LEAVE_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_REDO_LEAVE_CALLS, 1U);
    if (ctx == nullptr) {
        return;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (!hook->page_versioning_enabled) {
        return;
    }
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return;
    }

    std::uint64_t advanced_latest_lsn = 0U;
    const int result = mylite_ownerless_redo_state_leave(
        hook->redo_state,
        hook->redo_state_size,
        hook->owner_id,
        hook->owner_generation,
        latest_lsn,
        &advanced_latest_lsn,
        nullptr
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK && advanced_latest_lsn != 0U) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("redo-before-checkpoint");
#  endif
        ownerless_persist_redo_checkpoint(hook, advanced_latest_lsn, 0U, false);
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("redo-after-checkpoint");
#  endif
    }
}

void ownerless_innodb_pages_visible_hook(std::uint64_t visible_lsn, void *ctx) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_TOTAL_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_CALLS, 1U);
    if (ctx == nullptr || visible_lsn == 0U) {
        return;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (!hook->page_versioning_enabled) {
        return;
    }
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size) {
        return;
    }
    const bool other_trx = ownerless_trx_registry_has_other_active_transactions(hook);
    const bool other_explicit = ownerless_process_registry_has_other_live_explicit_transactions(
        hook->process_registry,
        hook->process_registry_size,
        hook->owner_id
    );
    if (other_trx || other_explicit) {
        return;
    }
    std::uint64_t stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    ownerless_page_log_append_batch_release_for_snapshot(hook);
    const int sync_result = sync_ownerless_page_log_if_changed(hook);
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_SYNC_NS,
        stage_start_ns
    );
    if (sync_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return;
    }

    std::uint64_t latest_lsn = 0U;
    std::uint64_t published_visible_lsn = 0U;
    stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    const int publish_result = mylite_ownerless_redo_state_publish_visible(
        hook->redo_state,
        hook->redo_state_size,
        visible_lsn,
        &latest_lsn,
        &published_visible_lsn
    );
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_REDO_STATE_NS,
        stage_start_ns
    );
    if (publish_result != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return;
    }
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("pages-visible-before-checkpoint");
#  endif
    stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    ownerless_persist_redo_checkpoint(hook, latest_lsn, published_visible_lsn, true);
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PAGES_VISIBLE_CHECKPOINT_NS,
        stage_start_ns
    );
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("pages-visible-after-checkpoint");
#  endif
}

bool ownerless_test_fault_is_configured() {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    const char *fault_name = std::getenv("MYLITE_OWNERLESS_TEST_FAULT");
    return fault_name != nullptr && fault_name[0] != '\0';
#  else
    return false;
#  endif
}

bool ownerless_checkpoint_update_allows_deferred_latest_coalescing(
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
) {
    return !durable && latest_lsn != 0U && visible_lsn == 0U &&
           ownerless_statement_defers_page_log_append_batch &&
           ownerless_statement_allows_deferred_latest_checkpoint_coalescing &&
           !ownerless_test_fault_is_configured();
}

void ownerless_persist_redo_checkpoint(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
) {
    if (hook == nullptr || hook->checkpoint_fd < 0) {
        return;
    }
    const bool coalesce_deferred_latest =
        ownerless_checkpoint_update_allows_deferred_latest_coalescing(
            latest_lsn,
            visible_lsn,
            durable
        );
    if (coalesce_deferred_latest && ownerless_statement_latest_checkpoint_preserved) {
        ownerless_database_perf_add(
            OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_DEFERRED_LATEST_COALESCED,
            1U
        );
        return;
    }
    bool ok = false;
    if (hook->redo_state != nullptr &&
        hook->redo_state_size >= k_concurrency_redo_state_segment_size) {
        ok = update_concurrency_checkpoint_lsn_from_redo_state(
            hook->checkpoint_fd,
            hook->redo_state,
            hook->redo_state_size,
            hook->process_registry,
            hook->process_registry_size,
            hook->owner_generation,
            latest_lsn,
            visible_lsn,
            durable
        );
    } else {
        ok = update_concurrency_checkpoint_lsn(
            hook->checkpoint_fd,
            latest_lsn,
            visible_lsn,
            durable
        );
    }
    if (ok && coalesce_deferred_latest) {
        ownerless_statement_latest_checkpoint_preserved = true;
    }
}

void ownerless_innodb_page_publish_batch_begin_hook(void *ctx) {
    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook == nullptr || ownerless_page_log_append_batch.session.active != 0 ||
        ownerless_test_fault_is_configured() || !hook->page_versioning_enabled ||
        hook->page_log_fd < 0 || hook->page_log_offset == 0U) {
        return;
    }

    ownerless_page_log_append_batch.hook = hook;
}

void ownerless_innodb_page_publish_batch_end_hook(void *ctx) {
    (void)ctx;
    if (ownerless_page_log_append_batch.session.active == 0) {
        ownerless_page_log_append_batch.hook = nullptr;
        return;
    }
    if (ownerless_page_log_append_batch.hook != nullptr &&
        ownerless_statement_defers_page_log_append_batch) {
        return;
    }
    ownerless_page_log_append_batch_release_current();
}

void ownerless_page_log_append_batch_release_current() {
    if (ownerless_page_log_append_batch.session.active == 0) {
        ownerless_page_log_append_batch.hook = nullptr;
        return;
    }
    OwnerlessInnoDBLockHookContext *hook = ownerless_page_log_append_batch.hook;
    const int fd = hook != nullptr ? hook->page_log_fd : -1;
    mylite_ownerless_page_log_append_session_end(fd, &ownerless_page_log_append_batch.session);
    ownerless_page_log_append_batch.hook = nullptr;
}

void ownerless_page_log_append_batch_release_for_snapshot(OwnerlessInnoDBLockHookContext *hook) {
    if (ownerless_page_log_append_batch.session.active == 0 ||
        ownerless_page_log_append_batch.hook != hook) {
        return;
    }
    mylite_ownerless_page_log_append_session_end(
        hook->page_log_fd,
        &ownerless_page_log_append_batch.session
    );
    ownerless_page_log_append_batch.hook = nullptr;
}

int append_ownerless_page_version(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t visible_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t page_checksum,
    bool native_support_page,
    bool external_snapshot_lineage,
    std::uint32_t publish_flags,
    std::uint64_t *out_record_offset
) {
    std::uint32_t append_options = 0U;
    if (!external_snapshot_lineage &&
        (publish_flags & MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_HISTORY_RSEG) != 0U) {
        append_options |= MYLITE_OWNERLESS_PAGE_LOG_APPEND_HISTORY_RSEG_DELTA;
    }
    if (native_support_page) {
        append_options |= MYLITE_OWNERLESS_PAGE_LOG_APPEND_NATIVE_SUPPORT_STATE;
    }
    if ((publish_flags & MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_PROOF_ONLY) != 0U) {
        append_options |= MYLITE_OWNERLESS_PAGE_LOG_APPEND_PROOF_ONLY;
    }
    if (ownerless_page_log_append_batch.hook == hook) {
        if (ownerless_page_log_append_batch.session.active == 0) {
            const int begin_result = mylite_ownerless_page_log_append_session_begin_initialized_at(
                hook->page_log_fd,
                hook->page_log_offset,
                &ownerless_page_log_append_batch.session
            );
            if (begin_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
                ownerless_page_log_append_batch.hook = nullptr;
            }
        }
    }

    if (ownerless_page_log_append_batch.session.active != 0 &&
        ownerless_page_log_append_batch.hook == hook) {
        if (external_snapshot_lineage) {
            return mylite_ownerless_page_log_append_external_snapshot_lineage_session_append_with_checksum_and_options(
                hook->page_log_fd,
                &ownerless_page_log_append_batch.session,
                space_id,
                page_no,
                page_lsn,
                visible_lsn,
                page,
                page_size,
                page_checksum,
                append_options,
                out_record_offset
            );
        }
        return mylite_ownerless_page_log_append_session_append_with_checksum_and_options(
            hook->page_log_fd,
            &ownerless_page_log_append_batch.session,
            space_id,
            page_no,
            page_lsn,
            visible_lsn,
            page,
            page_size,
            page_checksum,
            append_options,
            out_record_offset
        );
    }

    if (external_snapshot_lineage) {
        return mylite_ownerless_page_log_append_external_snapshot_lineage_initialized_at_with_checksum_and_options(
            hook->page_log_fd,
            hook->page_log_offset,
            space_id,
            page_no,
            page_lsn,
            visible_lsn,
            page,
            page_size,
            page_checksum,
            append_options,
            out_record_offset
        );
    }
    return mylite_ownerless_page_log_append_initialized_at_with_checksum_and_options(
        hook->page_log_fd,
        hook->page_log_offset,
        space_id,
        page_no,
        page_lsn,
        visible_lsn,
        page,
        page_size,
        page_checksum,
        append_options,
        out_record_offset
    );
}

int ownerless_innodb_history_proof_publish_pair_hook(
    std::uint32_t space_id,
    std::uint32_t rseg_page_no,
    std::uint64_t rseg_page_lsn,
    const void *rseg_page,
    std::uint32_t rseg_page_size,
    std::uint32_t undo_page_no,
    std::uint64_t undo_page_lsn,
    const void *undo_page,
    std::uint32_t undo_page_size,
    std::uint64_t visible_lsn,
    void *ctx
) {
    OwnerlessDatabasePerfCountedScope perf_scope(
        OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_CALLS,
        OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_NS
    );
    if (ctx == nullptr || visible_lsn == 0U || rseg_page_no == undo_page_no ||
        rseg_page_lsn == 0U || undo_page_lsn == 0U || rseg_page_size == 0U ||
        undo_page_size == 0U) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_FAILED, 1U);
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (!hook->page_versioning_enabled || hook->page_log_fd < 0 || hook->page_log_offset == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_UNAVAILABLE, 1U);
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (ownerless_test_fault_is_configured()) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_UNAVAILABLE, 1U);
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (ownerless_test_fails_native_support_page_publish(true)) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_FAILED, 1U);
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    if (ownerless_page_log_append_batch.hook != nullptr &&
        ownerless_page_log_append_batch.hook != hook) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_UNAVAILABLE, 1U);
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }

    const bool had_batch = ownerless_page_log_append_batch.hook == hook ||
                           ownerless_page_log_append_batch.session.active != 0;
    if (!had_batch) {
        ownerless_page_log_append_batch.hook = hook;
    }

    const auto finish_pair_batch = [hook, had_batch]() {
        if (had_batch) {
            return;
        }
        if (ownerless_page_log_append_batch.session.active == 0) {
            ownerless_page_log_append_batch.hook = nullptr;
            return;
        }
        if (!ownerless_statement_defers_page_log_append_batch) {
            ownerless_page_log_append_batch_release_current();
        }
    };

    std::uint64_t record_offset = 0U;
    std::uint64_t stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    int append_result = append_ownerless_page_version(
        hook,
        space_id,
        rseg_page_no,
        rseg_page_lsn,
        visible_lsn,
        rseg_page,
        rseg_page_size,
        0U,
        true,
        false,
        MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_PROOF_ONLY |
            MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_HISTORY_RSEG,
        &record_offset
    );
    if (append_result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        append_result = append_ownerless_page_version(
            hook,
            space_id,
            undo_page_no,
            undo_page_lsn,
            visible_lsn,
            undo_page,
            undo_page_size,
            0U,
            true,
            false,
            MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_PROOF_ONLY,
            &record_offset
        );
    }
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_APPEND_NS,
        stage_start_ns
    );
    finish_pair_batch();

    if (append_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_FAILED, 1U);
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_HISTORY_PROOF_PAIR_SUCCEEDED, 1U);
    ownerless_database_perf_add(
        OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_INDEX_SKIPPED_NATIVE_SUPPORT,
        2U
    );
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

bool publish_ownerless_snapshot_boundary_if_needed(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t visible_lsn,
    std::uint32_t page_size
) {
    if (hook == nullptr || hook->page_pin_registry == nullptr ||
        hook->page_pin_registry_size == 0U || hook->page_index == nullptr ||
        hook->page_index_size == 0U || hook->page_log_fd < 0 || hook->page_log_offset == 0U ||
        hook->database_path == nullptr || hook->owner_id == 0U || hook->owner_generation == 0U ||
        visible_lsn == 0U || page_size == 0U || page_size > k_innodb_page_size_max) {
        return false;
    }
    if (mylite_ownerless_page_pin_registry_active_count(hook->page_pin_registry) == 0U) {
        return false;
    }
    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    const int pin_result = mylite_ownerless_page_pin_registry_snapshot_oldest(
        hook->page_pin_registry,
        hook->page_pin_registry_size,
        hook->owner_id,
        hook->owner_generation,
        &active_pin_count,
        &oldest_pin_lsn
    );
    if (pin_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK || active_pin_count == 0U ||
        oldest_pin_lsn == 0U || oldest_pin_lsn >= visible_lsn) {
        return false;
    }
    std::uint32_t owner_active_pin_count = 0;
    const int owner_pin_count_result = mylite_ownerless_page_pin_registry_owner_active_count(
        hook->page_pin_registry,
        hook->page_pin_registry_size,
        hook->owner_id,
        hook->owner_id,
        hook->owner_generation,
        &owner_active_pin_count
    );
    const bool external_snapshot_pin_active =
        owner_pin_count_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK &&
        active_pin_count > owner_active_pin_count;
    if (external_snapshot_pin_active) {
        g_runtime.ownerless_runtime_consumed_external_snapshot_page_version_wal.store(
            true,
            std::memory_order_relaxed
        );
    }
    if (mylite_ownerless_innodb_statement_suppress_native_lifecycle_refresh() != 0) {
        return external_snapshot_pin_active;
    }
    ownerless_page_log_append_batch_release_for_snapshot(hook);

    std::unique_ptr<unsigned char[]> page(new (std::nothrow) unsigned char[page_size]);
    if (page == nullptr) {
        return external_snapshot_pin_active;
    }

    std::uint32_t existing_page_size = 0;
    std::uint64_t existing_page_lsn = 0;
    std::uint64_t existing_commit_lsn = 0;
    const int existing_result = mylite_ownerless_page_log_find_latest_at(
        hook->page_log_fd,
        hook->page_log_offset,
        space_id,
        page_no,
        oldest_pin_lsn,
        page.get(),
        page_size,
        &existing_page_size,
        &existing_page_lsn,
        &existing_commit_lsn
    );
    if (existing_result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return external_snapshot_pin_active;
    }
    if (existing_result != MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND) {
        return external_snapshot_pin_active;
    }

    const std::filesystem::path datadir =
        std::filesystem::path(hook->database_path) / k_datadir_name;
    const std::string datadir_name = datadir.string();
    std::uint32_t boundary_page_size = 0;
    std::uint64_t boundary_page_lsn = 0;
    const int read_result = mylite_ownerless_tablespace_read_page_at_or_before(
        datadir_name.c_str(),
        space_id,
        page_no,
        page_size,
        oldest_pin_lsn,
        page.get(),
        page_size,
        &boundary_page_size,
        &boundary_page_lsn
    );
    if (read_result != MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK || boundary_page_size == 0U ||
        boundary_page_lsn == 0U || boundary_page_lsn > oldest_pin_lsn) {
        return external_snapshot_pin_active;
    }

    std::uint64_t record_offset = 0;
    const int append_result = mylite_ownerless_page_log_append_snapshot_boundary_initialized_at(
        hook->page_log_fd,
        hook->page_log_offset,
        space_id,
        page_no,
        boundary_page_lsn,
        oldest_pin_lsn,
        page.get(),
        boundary_page_size,
        &record_offset
    );
    if (append_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return external_snapshot_pin_active;
    }
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_BOUNDARY_APPEND_CALLS, 1U);

    const int publish_result = mylite_ownerless_page_index_publish(
        hook->page_index,
        hook->page_index_size,
        hook->owner_id,
        hook->owner_generation,
        space_id,
        page_no,
        oldest_pin_lsn,
        boundary_page_lsn,
        record_offset
    );
    if (publish_result != MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        static_cast<void>(mylite_ownerless_page_index_require_wal_scan(
            hook->page_index,
            hook->page_index_size,
            hook->owner_id,
            hook->owner_generation
        ));
    }
    return external_snapshot_pin_active;
}

bool ownerless_page_publish_would_regress_physical_lsn(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t visible_lsn,
    std::uint64_t page_lsn,
    std::uint32_t page_size
) {
    if (hook == nullptr || hook->page_index == nullptr || hook->page_index_size == 0U ||
        hook->page_log_fd < 0 || hook->page_log_offset == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U || visible_lsn == 0U || page_lsn == 0U || page_size == 0U ||
        page_size > k_innodb_page_size_max) {
        return false;
    }
    if (mylite_ownerless_innodb_statement_visible_fast_path() == 0) {
        return false;
    }
    if (page_no > 2U) {
        return false;
    }
    if (page_size != k_innodb_page_size) {
        return false;
    }

    std::uint64_t existing_record_offset = 0;
    std::uint64_t existing_page_lsn = 0;
    std::uint64_t existing_commit_lsn = 0;
    const int index_result = mylite_ownerless_page_index_find(
        hook->page_index,
        hook->page_index_size,
        hook->owner_id,
        hook->owner_generation,
        space_id,
        page_no,
        visible_lsn,
        &existing_record_offset,
        &existing_page_lsn,
        &existing_commit_lsn
    );
    if (index_result != MYLITE_OWNERLESS_PAGE_INDEX_OK || existing_page_lsn <= page_lsn) {
        return false;
    }

    unsigned char *existing_page_buffer = new (std::nothrow) unsigned char[k_innodb_page_size_max];
    std::unique_ptr<unsigned char[]> existing_page(existing_page_buffer);
    if (existing_page == nullptr) {
        return false;
    }
    std::uint32_t existing_page_size = 0;
    std::uint64_t record_page_lsn = 0;
    std::uint64_t record_commit_lsn = 0;
    const int read_result = mylite_ownerless_page_log_read_page_at(
        hook->page_log_fd,
        hook->page_log_offset,
        existing_record_offset,
        space_id,
        page_no,
        existing_page.get(),
        k_innodb_page_size_max,
        &existing_page_size,
        &record_page_lsn,
        &record_commit_lsn
    );
    return read_result == MYLITE_OWNERLESS_PAGE_LOG_OK && record_page_lsn == existing_page_lsn &&
           record_commit_lsn == existing_commit_lsn && existing_page_size == page_size;
}

bool ownerless_test_fails_native_support_page_publish(bool native_support_page) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    if (!native_support_page || mylite_ownerless_innodb_test_faults_enabled_fast() == 0) {
        return false;
    }
    const char *value = std::getenv("MYLITE_OWNERLESS_TEST_FAIL_NATIVE_SUPPORT_PAGE_PUBLISH");
    return value != nullptr && std::strcmp(value, "1") == 0;
#  else
    (void)native_support_page;
    return false;
#  endif
}

int ownerless_innodb_page_publish_hook(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t visible_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint32_t publish_flags,
    void *ctx
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_TOTAL_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_CALLS, 1U);
    if (ctx == nullptr || page == nullptr || page_size == 0U || visible_lsn == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    constexpr std::uint32_t known_publish_flags =
        MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_HISTORY_RSEG |
        MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_PROOF_ONLY;
    if ((publish_flags & ~known_publish_flags) != 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    const bool proof_only_publish =
        (publish_flags & MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_PROOF_ONLY) != 0U;
    if (proof_only_publish && page_lsn == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (!hook->page_versioning_enabled) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (hook->page_index == nullptr || hook->page_index_size == 0U || hook->page_log_fd < 0 ||
        hook->page_log_offset == 0U || hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    if (ownerless_page_publish_would_regress_physical_lsn(
            hook,
            space_id,
            page_no,
            visible_lsn,
            page_lsn,
            page_size
        )) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }

    std::uint64_t record_offset = 0;
    std::uint64_t stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    const bool native_support_page =
        proof_only_publish || ownerless_page_image_is_native_support_state(page, page_size);
    bool external_snapshot_pin_active = false;
    if (ownerless_test_fails_native_support_page_publish(native_support_page)) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    if (!native_support_page) {
        external_snapshot_pin_active = publish_ownerless_snapshot_boundary_if_needed(
            hook,
            space_id,
            page_no,
            visible_lsn,
            page_size
        );
    }
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_BOUNDARY_NS,
        stage_start_ns
    );
    pause_for_ownerless_test_fault("page-publish-before-append");

    stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    const std::uint64_t page_checksum =
        proof_only_publish ? 0U : mylite_ownerless_page_log_checksum_page(page, page_size);
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_PAGE_LOG_CHECKSUM_NS,
        stage_start_ns
    );
    stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    const bool external_snapshot_lineage_active =
        external_snapshot_pin_active ||
        g_runtime.ownerless_runtime_consumed_external_snapshot_page_version_wal.load(
            std::memory_order_relaxed
        );
    const int append_result = append_ownerless_page_version(
        hook,
        space_id,
        page_no,
        page_lsn,
        visible_lsn,
        page,
        page_size,
        page_checksum,
        native_support_page,
        external_snapshot_lineage_active,
        publish_flags,
        &record_offset
    );
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_APPEND_NS,
        stage_start_ns
    );
    if (append_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    pause_for_ownerless_test_fault("page-publish-after-append");
    if (native_support_page) {
        ownerless_database_perf_add(
            OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_INDEX_SKIPPED_NATIVE_SUPPORT,
            1U
        );
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }

    stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    const int index_result = mylite_ownerless_page_index_publish(
        hook->page_index,
        hook->page_index_size,
        hook->owner_id,
        hook->owner_generation,
        space_id,
        page_no,
        visible_lsn,
        page_lsn,
        record_offset
    );
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PAGE_PUBLISH_INDEX_NS,
        stage_start_ns
    );
    if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        mylite_ownerless_innodb_note_external_page_observed(space_id, page_no, visible_lsn);
    }
    return ownerless_innodb_lock_result_from_page_index_result(index_result);
}

int ownerless_innodb_page_read_hook(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags,
    void *ctx
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_PAGE_READ_TOTAL_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_CALLS, 1U);
    if (ctx == nullptr || page == nullptr || page_capacity == 0U || max_commit_lsn == 0U ||
        out_page_size == nullptr || out_page_lsn == nullptr || out_commit_lsn == nullptr ||
        out_record_flags == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    *out_record_flags = 0U;

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->page_index == nullptr || hook->page_index_size == 0U || hook->page_log_fd < 0 ||
        hook->page_log_offset == 0U || hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    if (!hook->page_log_reads_enabled) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }

    if (mylite_ownerless_page_log_begin_read(hook->page_log_fd) != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    const int result = ownerless_innodb_page_read_locked(
        hook,
        space_id,
        page_no,
        max_commit_lsn,
        page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn,
        out_record_flags
    );
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        g_runtime.ownerless_runtime_consumed_page_version_wal.store(
            true,
            std::memory_order_relaxed
        );
        if (ownerless_runtime_has_external_page_version_pin(g_runtime)) {
            g_runtime.ownerless_runtime_consumed_external_snapshot_page_version_wal.store(
                true,
                std::memory_order_relaxed
            );
        }
        std::uint64_t latest_lsn = 0;
        std::uint64_t visible_lsn = 0;
        if (g_runtime.concurrency_checkpoint_fd >= 0 && read_concurrency_checkpoint_lsn(
                                                            g_runtime.concurrency_checkpoint_fd,
                                                            &latest_lsn,
                                                            &visible_lsn
                                                        )) {
            if (visible_lsn != 0U && latest_lsn == visible_lsn && *out_commit_lsn >= visible_lsn) {
                g_runtime.ownerless_runtime_consumed_current_page_version_wal.store(
                    true,
                    std::memory_order_relaxed
                );
            }
        }
    }
    mylite_ownerless_page_log_end_read(hook->page_log_fd);
    return result;
}

std::uint32_t ownerless_innodb_page_version_flags(const std::uint32_t page_log_flags) {
    std::uint32_t flags = 0U;
    if ((page_log_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_SNAPSHOT_BOUNDARY) != 0U) {
        flags |= MYLITE_OWNERLESS_INNODB_PAGE_VERSION_SNAPSHOT_BOUNDARY;
    }
    if ((page_log_flags & MYLITE_OWNERLESS_PAGE_LOG_RECORD_EXTERNAL_SNAPSHOT_LINEAGE) != 0U) {
        flags |= MYLITE_OWNERLESS_INNODB_PAGE_VERSION_EXTERNAL_SNAPSHOT_LINEAGE;
    }
    return flags;
}

int ownerless_innodb_skip_external_page_refresh_hook(void *ctx) {
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_CALLS, 1U);
    if (ctx == nullptr) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_UNMAPPED, 1U);
        return 0;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->process_registry == nullptr || hook->process_registry_size == 0U ||
        hook->page_pin_registry == nullptr || hook->page_pin_registry_size == 0U ||
        hook->redo_state == nullptr ||
        hook->redo_state_size <
            k_concurrency_redo_state_visible_lsn_offset + sizeof(std::uint64_t) ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_UNMAPPED, 1U);
        return 0;
    }
    if (!hook->page_versioning_enabled && hook->page_log_reads_enabled) {
        return 0;
    }

    const std::uint64_t active_count =
        mylite_ownerless_process_registry_active_count(hook->process_registry);
    const std::uint64_t registry_generation =
        mylite_ownerless_process_registry_generation(hook->process_registry);
    if (active_count != 1U) {
        ownerless_database_perf_add(
            OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_ACTIVE_COUNT,
            1U
        );
        return 0;
    }
    if (registry_generation != hook->owner_generation) {
        ownerless_database_perf_add(
            OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_GENERATION,
            1U
        );
        return 0;
    }
    const std::uint64_t active_pin_count =
        mylite_ownerless_page_pin_registry_active_count(hook->page_pin_registry);
    if (active_pin_count != 0U) {
        std::uint32_t owner_active_pin_count = 0;
        const int owner_pin_count_result = mylite_ownerless_page_pin_registry_owner_active_count(
            hook->page_pin_registry,
            hook->page_pin_registry_size,
            hook->owner_id,
            hook->owner_id,
            hook->owner_generation,
            &owner_active_pin_count
        );
        if (owner_pin_count_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK ||
            active_pin_count > owner_active_pin_count) {
            ownerless_database_perf_add(
                OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_ACTIVE_PINS,
                1U
            );
            return 0;
        }
    }
    const auto *redo_state = static_cast<unsigned char *>(hook->redo_state);
    const std::uint64_t latest_lsn =
        load_shared64(redo_state, k_concurrency_redo_state_latest_lsn_offset);
    const std::uint64_t visible_lsn =
        load_shared64(redo_state, k_concurrency_redo_state_visible_lsn_offset);
    if (latest_lsn == 0U && visible_lsn == 0U) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_BLOCKED_BASELINE, 1U);
        return 0;
    }
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_SINGLE_OWNER_SKIP_ALLOWED, 1U);
    return 1;
}

std::size_t ownerless_page_log_negative_cache_slot(std::uint32_t space_id, std::uint32_t page_no) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(space_id);
    mix(page_no);
    return static_cast<std::size_t>(hash % k_ownerless_page_log_negative_cache_size);
}

bool ownerless_page_log_negative_cache_absence_lookup(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    std::uint64_t index_generation,
    std::uint64_t log_generation,
    std::uint64_t snapshot_end_offset,
    std::uint64_t *out_scan_start_offset
) {
    if (out_scan_start_offset != nullptr) {
        *out_scan_start_offset = 0U;
    }
    if (hook == nullptr || out_scan_start_offset == nullptr || max_commit_lsn == 0U ||
        index_generation == 0U || snapshot_end_offset == 0U) {
        return false;
    }

    const std::size_t slot = ownerless_page_log_negative_cache_slot(space_id, page_no);
    std::lock_guard<std::mutex> guard(ownerless_page_log_negative_cache_mutex);
    const OwnerlessPageLogNegativeCacheEntry &entry = hook->page_log_negative_cache[slot];
    if (!entry.valid || entry.space_id != space_id || entry.page_no != page_no ||
        entry.index_generation != index_generation || entry.max_commit_lsn < max_commit_lsn ||
        entry.log_generation != log_generation || entry.covered_end_offset == 0U) {
        return false;
    }
    if (entry.covered_end_offset >= snapshot_end_offset) {
        return true;
    }
    *out_scan_start_offset = entry.covered_end_offset;
    return false;
}

void ownerless_page_log_negative_cache_store(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    std::uint64_t index_generation,
    std::uint64_t log_generation,
    std::uint64_t covered_end_offset
) {
    if (hook == nullptr || max_commit_lsn == 0U || index_generation == 0U ||
        covered_end_offset == 0U) {
        return;
    }

    const std::size_t slot = ownerless_page_log_negative_cache_slot(space_id, page_no);
    std::lock_guard<std::mutex> guard(ownerless_page_log_negative_cache_mutex);
    OwnerlessPageLogNegativeCacheEntry &entry = hook->page_log_negative_cache[slot];
    const bool same_entry = entry.valid && entry.space_id == space_id && entry.page_no == page_no;
    const bool same_index_entry = same_entry && entry.index_generation == index_generation;
    const bool same_log_entry = same_entry && entry.log_generation == log_generation;
    entry.valid = true;
    entry.space_id = space_id;
    entry.page_no = page_no;
    entry.index_generation = index_generation;
    entry.max_commit_lsn =
        same_index_entry ? std::max(entry.max_commit_lsn, max_commit_lsn) : max_commit_lsn;
    entry.log_generation = log_generation;
    entry.covered_end_offset = same_log_entry
                                   ? std::max(entry.covered_end_offset, covered_end_offset)
                                   : covered_end_offset;
}

int ownerless_innodb_page_read_locked(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    std::uint32_t *out_record_flags
) {
    if (out_record_flags != nullptr) {
        *out_record_flags = 0U;
    }
    std::uint64_t record_offset = 0;
    std::uint64_t index_page_lsn = 0;
    std::uint64_t index_commit_lsn = 0;
    std::uint64_t index_generation = 0;
    std::uint64_t stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    const int index_result = mylite_ownerless_page_index_find_with_generation(
        hook->page_index,
        hook->page_index_size,
        hook->owner_id,
        hook->owner_generation,
        space_id,
        page_no,
        max_commit_lsn,
        &record_offset,
        &index_page_lsn,
        &index_commit_lsn,
        &index_generation
    );
    ownerless_database_perf_add_elapsed(OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_NS, stage_start_ns);
    if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        stage_start_ns =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        std::uint32_t page_log_flags = 0U;
        const int read_result = mylite_ownerless_page_log_read_page_under_read_lock_at_with_flags(
            hook->page_log_fd,
            hook->page_log_offset,
            record_offset,
            space_id,
            page_no,
            page,
            page_capacity,
            out_page_size,
            out_page_lsn,
            out_commit_lsn,
            &page_log_flags
        );
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_DIRECT_NS,
            stage_start_ns
        );
        if (read_result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
            if (*out_page_lsn == index_page_lsn && *out_commit_lsn == index_commit_lsn) {
                // The page index is a cache updated after the WAL append; scan
                // the appended tail before trusting a direct indexed hit.
                std::uint64_t page_log_snapshot_end_offset = 0;
                std::uint64_t page_log_generation = 0;
                const int snapshot_result = mylite_ownerless_page_log_snapshot_under_read_lock_at(
                    hook->page_log_fd,
                    hook->page_log_offset,
                    &page_log_snapshot_end_offset,
                    &page_log_generation
                );
                if (snapshot_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
                    ownerless_database_perf_add(
                        OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_ERRORS,
                        1U
                    );
                    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
                }
                std::uint64_t tail_scan_offset = 0;
                const int extent_result = mylite_ownerless_page_log_record_next_offset_at(
                    hook->page_log_fd,
                    hook->page_log_offset,
                    record_offset,
                    &tail_scan_offset
                );
                if (extent_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
                    ownerless_database_perf_add(
                        OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_ERRORS,
                        1U
                    );
                    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
                }
                if (page_log_snapshot_end_offset > tail_scan_offset) {
                    unsigned char *tail_page_buffer =
                        new (std::nothrow) unsigned char[page_capacity];
                    std::unique_ptr<unsigned char[]> tail_page(tail_page_buffer);
                    if (tail_page == nullptr) {
                        ownerless_database_perf_add(
                            OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_ERRORS,
                            1U
                        );
                        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
                    }
                    ownerless_database_perf_add(
                        OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_CALLS,
                        1U
                    );
                    stage_start_ns = ownerless_database_perf_stats_are_enabled()
                                         ? ownerless_database_perf_now_ns()
                                         : 0U;
                    std::uint32_t tail_page_size = 0;
                    std::uint64_t tail_page_lsn = 0;
                    std::uint64_t tail_commit_lsn = 0;
                    std::uint32_t tail_page_log_flags = 0U;
                    int saw_tail_page_record = 0;
                    const int tail_result =
                        mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at_with_flags(
                            hook->page_log_fd,
                            hook->page_log_offset,
                            tail_scan_offset,
                            page_log_snapshot_end_offset,
                            space_id,
                            page_no,
                            max_commit_lsn,
                            tail_page.get(),
                            page_capacity,
                            &tail_page_size,
                            &tail_page_lsn,
                            &tail_commit_lsn,
                            &tail_page_log_flags,
                            &saw_tail_page_record
                        );
                    ownerless_database_perf_add_elapsed(
                        OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_NS,
                        stage_start_ns
                    );
                    if (tail_result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
                        ownerless_database_perf_add(
                            OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_FOUND,
                            1U
                        );
                        const bool tail_record_is_newer =
                            tail_commit_lsn > index_commit_lsn ||
                            (tail_commit_lsn == index_commit_lsn && tail_page_lsn > index_page_lsn);
                        if (tail_record_is_newer) {
                            std::memcpy(page, tail_page.get(), tail_page_size);
                            *out_page_size = tail_page_size;
                            *out_page_lsn = tail_page_lsn;
                            *out_commit_lsn = tail_commit_lsn;
                            page_log_flags = tail_page_log_flags;
                        }
                        if (out_record_flags != nullptr) {
                            *out_record_flags = ownerless_innodb_page_version_flags(page_log_flags);
                        }
                        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
                    }
                    if (tail_result == MYLITE_OWNERLESS_PAGE_LOG_FULL) {
                        ownerless_database_perf_add(
                            OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_ERRORS,
                            1U
                        );
                        return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
                    }
                    if (tail_result != MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND) {
                        ownerless_database_perf_add(
                            OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_ERRORS,
                            1U
                        );
                        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
                    }
                    ownerless_database_perf_add(
                        OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_MISSES,
                        1U
                    );
                }
                ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_HITS, 1U);
                if (out_record_flags != nullptr) {
                    *out_record_flags = ownerless_innodb_page_version_flags(page_log_flags);
                }
                return MYLITE_OWNERLESS_INNODB_LOCK_OK;
            }
            ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_STALE, 1U);
        } else if (read_result == MYLITE_OWNERLESS_PAGE_LOG_FULL) {
            ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_ERRORS, 1U);
            return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
        } else {
            ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_STALE, 1U);
        }
    } else if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_MISSES, 1U);
    } else if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_SCAN_REQUIRED, 1U);
    } else {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_INDEX_ERRORS, 1U);
        return ownerless_innodb_lock_result_from_page_index_result(index_result);
    }

    bool page_log_snapshot_available = false;
    std::uint64_t page_log_snapshot_end_offset = 0;
    std::uint64_t page_log_generation = 0;
    std::uint64_t page_log_scan_start_offset = 0;
    if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND) {
        const int snapshot_result = mylite_ownerless_page_log_snapshot_under_read_lock_at(
            hook->page_log_fd,
            hook->page_log_offset,
            &page_log_snapshot_end_offset,
            &page_log_generation
        );
        if (snapshot_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
            ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_ERRORS, 1U);
            return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
        }
        page_log_snapshot_available = true;
        const bool negative_cache_hit = ownerless_page_log_negative_cache_absence_lookup(
            hook,
            space_id,
            page_no,
            max_commit_lsn,
            index_generation,
            page_log_generation,
            page_log_snapshot_end_offset,
            &page_log_scan_start_offset
        );
        if (negative_cache_hit) {
            ownerless_database_perf_add(
                OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_NEGATIVE_CACHE_HITS,
                1U
            );
            return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
        }
    }

    // The page index is rebuildable; the WAL scan is authoritative if an indexed
    // offset is stale after checkpoint movement or if the index cannot prove
    // absence for this page-index generation.
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_CALLS, 1U);
    stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    int saw_page_record = 1;
    std::uint32_t page_log_flags = 0U;
    const int result =
        page_log_snapshot_available
            ? mylite_ownerless_page_log_find_latest_in_snapshot_from_under_read_lock_at_with_flags(
                  hook->page_log_fd,
                  hook->page_log_offset,
                  page_log_scan_start_offset,
                  page_log_snapshot_end_offset,
                  space_id,
                  page_no,
                  max_commit_lsn,
                  page,
                  page_capacity,
                  out_page_size,
                  out_page_lsn,
                  out_commit_lsn,
                  &page_log_flags,
                  &saw_page_record
              )
            : mylite_ownerless_page_log_find_latest_under_read_lock_at_with_flags(
                  hook->page_log_fd,
                  hook->page_log_offset,
                  space_id,
                  page_no,
                  max_commit_lsn,
                  page,
                  page_capacity,
                  out_page_size,
                  out_page_lsn,
                  out_commit_lsn,
                  &page_log_flags
              );
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_NS,
        stage_start_ns
    );
    if (result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_FOUND, 1U);
        if (out_record_flags != nullptr) {
            *out_record_flags = ownerless_innodb_page_version_flags(page_log_flags);
        }
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_MISSES, 1U);
        if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND && page_log_snapshot_available &&
            saw_page_record == 0) {
            ownerless_page_log_negative_cache_store(
                hook,
                space_id,
                page_no,
                max_commit_lsn,
                index_generation,
                page_log_generation,
                page_log_snapshot_end_offset
            );
            ownerless_database_perf_add(
                OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_NEGATIVE_CACHE_STORES,
                1U
            );
        }
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_PAGE_READ_WAL_SCAN_ERRORS, 1U);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_lock_result_from_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_NOT_FOUND) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
    }
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    }
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_lock_result_from_page_index_result(int index_result) {
    if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_FULL) {
        return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_runtime_may_delete_shared_file_hook(void *ctx) {
    if (ctx == nullptr) {
        return 1;
    }

    auto *runtime = static_cast<RuntimeState *>(ctx);
    unsigned char *registry = runtime_process_registry(*runtime);
    if (registry == nullptr) {
        return 1;
    }

    return mylite_ownerless_process_registry_active_count(registry) <= 1U ? 1 : 0;
}

unsigned char *runtime_process_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_process_registry_offset + k_concurrency_process_registry_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_process_registry_offset;
}

unsigned char *runtime_process_slot(RuntimeState &runtime) {
    unsigned char *registry = runtime_process_registry(runtime);
    if (registry == nullptr || runtime.concurrency_process_slot_generation == 0U ||
        runtime.concurrency_process_slot_index >= k_concurrency_process_slot_count) {
        return nullptr;
    }
    return registry + k_concurrency_process_registry_header_size +
           (runtime.concurrency_process_slot_index * k_concurrency_process_slot_size);
}

unsigned char *runtime_trx_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_trx_registry_offset + k_concurrency_trx_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_trx_registry_offset;
}

unsigned char *runtime_read_view_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size < k_concurrency_read_view_registry_offset +
                                                   k_concurrency_read_view_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_read_view_registry_offset;
}

unsigned char *runtime_page_pin_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_page_pin_registry_offset + k_concurrency_page_pin_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_page_pin_registry_offset;
}

unsigned char *runtime_innodb_lock_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_innodb_lock_registry_offset +
                k_concurrency_innodb_lock_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_innodb_lock_registry_offset;
}

unsigned char *runtime_autoinc_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_autoinc_registry_offset + k_concurrency_autoinc_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_autoinc_registry_offset;
}

unsigned char *runtime_page_write_lock_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_page_write_lock_registry_offset +
                k_concurrency_page_write_lock_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_page_write_lock_registry_offset;
}

unsigned char *runtime_redo_state(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_redo_state_offset + k_concurrency_redo_state_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_redo_state_offset;
}

unsigned char *runtime_page_index(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_page_index_offset + k_concurrency_page_index_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_page_index_offset;
}

unsigned char *runtime_dictionary_state(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_dictionary_state_offset + k_concurrency_dictionary_state_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_dictionary_state_offset;
}

std::uint32_t ownerless_owner_id_from_slot_index(std::uint32_t slot_index) {
    return slot_index + 1U;
}

#  if defined(__linux__)
bool ownerless_linux_process_is_zombie(pid_t pid) {
    char stat_path[64];
    char stat_buffer[512];
    const int path_length =
        std::snprintf(stat_path, sizeof(stat_path), "/proc/%ld/stat", static_cast<long>(pid));
    if (path_length <= 0 || static_cast<std::size_t>(path_length) >= sizeof(stat_path)) {
        return false;
    }

    const int fd = ::open(stat_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const ssize_t bytes_read = ::read(fd, stat_buffer, sizeof(stat_buffer) - 1U);
    const int saved_errno = errno;
    static_cast<void>(::close(fd));
    errno = saved_errno;
    if (bytes_read <= 0) {
        return false;
    }

    stat_buffer[bytes_read] = '\0';
    const char *close_paren = std::strrchr(stat_buffer, ')');
    if (close_paren == nullptr || close_paren[1] != ' ') {
        return false;
    }
    return close_paren[2] == 'Z';
}
#  endif

int ownerless_process_is_alive(std::uint64_t pid, void *ctx) {
    (void)ctx;
    if (pid == 0U || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return 0;
    }
    const pid_t process_id = static_cast<pid_t>(pid);
    if (::kill(process_id, 0) == 0) {
#  if defined(__linux__)
        if (ownerless_linux_process_is_zombie(process_id)) {
            return 0;
        }
#  endif
        return 1;
    }
    return errno == EPERM ? 1 : 0;
}

int ownerless_process_cleanup_dead_owner_state(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t pid,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    auto *cleanup = static_cast<OwnerlessProcessCleanupContext *>(ctx);
    const std::uint32_t owner_id = ownerless_owner_id_from_slot_index(slot_index);
    const int page_write_release_result =
        ownerless_process_release_owner_page_write_locks(*cleanup, owner_id);
    if (page_write_release_result != MYLITE_OWNERLESS_PROCESS_CLEANUP_OK) {
        return page_write_release_result;
    }
    if (ownerless_process_owner_state_requires_recovery(*cleanup, owner_id)) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_BLOCKED;
    }
    return ownerless_process_cleanup_owner_state(slot_index, slot_generation, pid, ctx);
}

int ownerless_process_cleanup_owner_state(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t pid,
    void *ctx
) {
    (void)pid;
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    auto *cleanup = static_cast<OwnerlessProcessCleanupContext *>(ctx);
    if (cleanup->lock_table == nullptr || cleanup->lock_table_size == 0U ||
        cleanup->trx_registry == nullptr || cleanup->trx_registry_size == 0U ||
        cleanup->read_view_registry == nullptr || cleanup->read_view_registry_size == 0U ||
        cleanup->page_pin_registry == nullptr || cleanup->page_pin_registry_size == 0U ||
        cleanup->page_write_lock_registry == nullptr ||
        cleanup->page_write_lock_registry_size == 0U) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    const std::uint32_t owner_id = ownerless_owner_id_from_slot_index(slot_index);
    std::uint32_t released_entries = 0;
    const int release_result = mylite_ownerless_lock_table_release_owner(
        cleanup->lock_table,
        cleanup->lock_table_size,
        owner_id,
        cleanup->latch_owner_id,
        cleanup->latch_owner_generation,
        &released_entries
    );
    if (release_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_innodb_locks = 0;
    if (cleanup->innodb_lock_registry == nullptr || cleanup->innodb_lock_registry_size == 0U ||
        mylite_ownerless_innodb_lock_registry_release_owner(
            cleanup->innodb_lock_registry,
            cleanup->innodb_lock_registry_size,
            owner_id,
            cleanup->latch_owner_id,
            cleanup->latch_owner_generation,
            &released_innodb_locks
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_page_write_locks = 0;
    if (mylite_ownerless_innodb_lock_registry_release_owner(
            cleanup->page_write_lock_registry,
            cleanup->page_write_lock_registry_size,
            owner_id,
            cleanup->latch_owner_id,
            cleanup->latch_owner_generation,
            &released_page_write_locks
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_transactions = 0;
    const int trx_release_result = mylite_ownerless_trx_registry_release_owner(
        cleanup->trx_registry,
        cleanup->trx_registry_size,
        owner_id,
        cleanup->latch_owner_id,
        cleanup->latch_owner_generation,
        &released_transactions
    );
    if (trx_release_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_pins = 0;
    const int page_pin_release_result = mylite_ownerless_page_pin_registry_release_owner(
        cleanup->page_pin_registry,
        cleanup->page_pin_registry_size,
        owner_id,
        cleanup->latch_owner_id,
        cleanup->latch_owner_generation,
        &released_pins
    );
    if (page_pin_release_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    if (cleanup->redo_state != nullptr &&
        cleanup->redo_state_size >= k_concurrency_redo_state_segment_size) {
        std::uint32_t released_redo = 0U;
        if (mylite_ownerless_redo_state_cleanup_owner(
                cleanup->redo_state,
                cleanup->redo_state_size,
                owner_id,
                slot_generation,
                &released_redo
            ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
            return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
        }
    }

    std::uint32_t released_views = 0;
    const int read_view_release_result = mylite_ownerless_read_view_registry_release_owner(
        cleanup->read_view_registry,
        cleanup->read_view_registry_size,
        owner_id,
        cleanup->latch_owner_id,
        cleanup->latch_owner_generation,
        &released_views
    );
    return read_view_release_result == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
               ? MYLITE_OWNERLESS_PROCESS_CLEANUP_OK
               : MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
}

int ownerless_process_release_owner_page_write_locks(
    OwnerlessProcessCleanupContext &cleanup,
    std::uint32_t owner_id
) {
    if (cleanup.page_write_lock_registry == nullptr ||
        cleanup.page_write_lock_registry_size == 0U) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_page_write_locks = 0;
    if (mylite_ownerless_innodb_lock_registry_release_owner(
            cleanup.page_write_lock_registry,
            cleanup.page_write_lock_registry_size,
            owner_id,
            cleanup.latch_owner_id,
            cleanup.latch_owner_generation,
            &released_page_write_locks
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }
    return MYLITE_OWNERLESS_PROCESS_CLEANUP_OK;
}

bool ownerless_process_owner_state_requires_recovery(
    OwnerlessProcessCleanupContext &cleanup,
    std::uint32_t owner_id
) {
    if (cleanup.lock_table == nullptr || cleanup.lock_table_size == 0U ||
        cleanup.trx_registry == nullptr || cleanup.trx_registry_size == 0U ||
        cleanup.read_view_registry == nullptr || cleanup.read_view_registry_size == 0U ||
        cleanup.page_pin_registry == nullptr || cleanup.page_pin_registry_size == 0U ||
        cleanup.innodb_lock_registry == nullptr || cleanup.innodb_lock_registry_size == 0U ||
        cleanup.page_write_lock_registry == nullptr ||
        cleanup.page_write_lock_registry_size == 0U || cleanup.dictionary_state == nullptr ||
        cleanup.dictionary_state_size == 0U) {
        return true;
    }

    std::uint32_t active_count = 0;
    const int trx_count_result = mylite_ownerless_trx_registry_owner_active_count(
        cleanup.trx_registry,
        cleanup.trx_registry_size,
        owner_id,
        cleanup.latch_owner_id,
        cleanup.latch_owner_generation,
        &active_count
    );
    if (trx_count_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK || active_count > 0U) {
        return true;
    }
    /*
      Dead read-only snapshot readers publish MDL, read-view, and page-version
      pin state, but no transaction/redo/InnoDB lock/dictionary state that
      requires no-live recovery. Let normal dead-owner cleanup release those
      entries so they do not indefinitely block live-peer page-log reclamation.
    */
    const int innodb_count_result = mylite_ownerless_innodb_lock_registry_owner_active_count(
        cleanup.innodb_lock_registry,
        cleanup.innodb_lock_registry_size,
        owner_id,
        cleanup.latch_owner_id,
        cleanup.latch_owner_generation,
        &active_count
    );
    if (innodb_count_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK || active_count > 0U) {
        return true;
    }
    const int page_write_count_result = mylite_ownerless_innodb_lock_registry_owner_active_count(
        cleanup.page_write_lock_registry,
        cleanup.page_write_lock_registry_size,
        owner_id,
        cleanup.latch_owner_id,
        cleanup.latch_owner_generation,
        &active_count
    );
    if (page_write_count_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK || active_count > 0U) {
        return true;
    }
    if (cleanup.redo_state != nullptr &&
        cleanup.redo_state_size >= k_concurrency_redo_state_segment_size) {
        if (mylite_ownerless_redo_state_owner_active_count(
                cleanup.redo_state,
                cleanup.redo_state_size,
                owner_id,
                &active_count
            ) != MYLITE_OWNERLESS_REDO_STATE_OK ||
            active_count > 0U) {
            return true;
        }
        mylite_ownerless_redo_state_snapshot snapshot = {};
        if (mylite_ownerless_redo_state_read_snapshot(
                cleanup.redo_state,
                cleanup.redo_state_size,
                &snapshot
            ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
            return true;
        }
        if (snapshot.latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED &&
            snapshot.latch_owner_id == owner_id) {
            return true;
        }
        if (snapshot.progress_latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED &&
            snapshot.progress_latch_owner_id == owner_id) {
            return true;
        }
    }
    const int dictionary_count_result = mylite_ownerless_dictionary_state_owner_active_count(
        cleanup.dictionary_state,
        cleanup.dictionary_state_size,
        owner_id,
        &active_count
    );
    if (dictionary_count_result != MYLITE_OWNERLESS_DICTIONARY_STATE_OK || active_count > 0U) {
        return true;
    }
    return false;
}

int mylite_result_from_process_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return MYLITE_OK;
    }
    if (registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_FULL ||
        registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_TIMEOUT ||
        registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY) {
        return MYLITE_BUSY;
    }
    return MYLITE_IOERR;
}

bool update_concurrency_shm_state(int shm_fd, std::uint32_t state) {
    std::array<unsigned char, 4> state_bytes = {};
    store_le32(state_bytes.data(), 0, state);
    return write_exact_at(
        shm_fd,
        state_bytes.data(),
        state_bytes.size(),
        static_cast<off_t>(k_concurrency_shm_state_offset)
    );
}

bool write_concurrency_checkpoint_lsn_locked(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
) {
    OwnerlessCheckpointLsnRecord current_record = {};
    bool has_current_record = false;
    if (!read_concurrency_checkpoint_lsn_records(
            checkpoint_fd,
            &current_record,
            &has_current_record
        )) {
        return false;
    }
    return write_concurrency_checkpoint_lsn_with_current_record_locked(
        checkpoint_fd,
        latest_lsn,
        visible_lsn,
        durable,
        current_record,
        has_current_record,
        nullptr
    );
}

bool write_concurrency_checkpoint_lsn_with_current_record_locked(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable,
    const OwnerlessCheckpointLsnRecord &current_record,
    bool has_current_record,
    OwnerlessCheckpointLsnRecord *out_record
) {
    if (visible_lsn > latest_lsn) {
        latest_lsn = visible_lsn;
    }

    if (has_current_record && current_record.latest_lsn == latest_lsn &&
        current_record.visible_lsn == visible_lsn &&
        (!durable ||
         ownerless_checkpoint_lsn_sync_anchor_matches(checkpoint_fd, latest_lsn, visible_lsn))) {
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_NOOP_ELIDED, 1U);
        if (out_record != nullptr) {
            *out_record = current_record;
        }
        return true;
    }
    const std::uint64_t next_generation =
        has_current_record && current_record.generation < UINT64_MAX
            ? current_record.generation + 1U
            : 1U;
    const std::size_t record_index = static_cast<std::size_t>(
        (next_generation - 1U) % k_concurrency_checkpoint_lsn_record_count
    );

    std::array<unsigned char, k_concurrency_checkpoint_lsn_record_size> lsn_record = {};
    build_concurrency_checkpoint_lsn_record(lsn_record, next_generation, latest_lsn, visible_lsn);
    OwnerlessCheckpointLsnRecord written_record = {
        next_generation,
        latest_lsn,
        visible_lsn,
    };
    reset_ownerless_checkpoint_lsn_sync_anchor();
    std::uint64_t stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    bool ok = write_exact_at(
        checkpoint_fd,
        lsn_record.data(),
        lsn_record.size(),
        concurrency_checkpoint_lsn_record_offset(record_index)
    );
    if (ok && !has_current_record) {
        std::array<unsigned char, 16> legacy_payload = {};
        store_le64(legacy_payload.data(), 0U, latest_lsn);
        store_le64(legacy_payload.data(), sizeof(std::uint64_t), visible_lsn);
        ok = write_exact_at(
            checkpoint_fd,
            legacy_payload.data(),
            legacy_payload.size(),
            static_cast<off_t>(k_concurrency_checkpoint_latest_lsn_offset)
        );
    } else if (ok) {
        ownerless_database_perf_add(
            OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_LEGACY_WRITE_ELIDED,
            1U
        );
    }
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_WRITE_NS,
        stage_start_ns
    );
    if (ok && durable) {
        stage_start_ns =
            ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
        ok = sync_fd_data(checkpoint_fd);
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_SYNC_NS,
            stage_start_ns
        );
        if (ok) {
            ownerless_checkpoint_lsn_sync_anchor_store(checkpoint_fd, latest_lsn, visible_lsn);
        }
    }
    if (ok && out_record != nullptr) {
        *out_record = written_record;
    }
    return ok;
}

bool write_concurrency_checkpoint_lsn_with_generation_cache_locked(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable,
    std::uint64_t registry_generation
) {
    std::lock_guard<std::mutex> guard(g_ownerless_checkpoint_lsn_generation_cache_mutex);

    OwnerlessCheckpointLsnRecord current_record = {};
    bool has_current_record = false;
    if (g_ownerless_checkpoint_lsn_generation_cache.fd == checkpoint_fd &&
        g_ownerless_checkpoint_lsn_generation_cache.registry_generation == registry_generation &&
        g_ownerless_checkpoint_lsn_generation_cache.record.generation != 0U) {
        current_record = g_ownerless_checkpoint_lsn_generation_cache.record;
        has_current_record = true;
        ownerless_database_perf_add(
            OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_GENERATION_CACHE_HITS,
            1U
        );
    } else if (!read_concurrency_checkpoint_lsn_records(
                   checkpoint_fd,
                   &current_record,
                   &has_current_record
               )) {
        g_ownerless_checkpoint_lsn_generation_cache = {};
        return false;
    }

    OwnerlessCheckpointLsnRecord updated_record = {};
    const bool ok = write_concurrency_checkpoint_lsn_with_current_record_locked(
        checkpoint_fd,
        latest_lsn,
        visible_lsn,
        durable,
        current_record,
        has_current_record,
        &updated_record
    );
    if (ok && updated_record.generation != 0U) {
        g_ownerless_checkpoint_lsn_generation_cache.fd = checkpoint_fd;
        g_ownerless_checkpoint_lsn_generation_cache.registry_generation = registry_generation;
        g_ownerless_checkpoint_lsn_generation_cache.record = updated_record;
    } else if (!ok) {
        g_ownerless_checkpoint_lsn_generation_cache = {};
    }
    return ok;
}

off_t concurrency_checkpoint_lsn_record_offset(std::size_t index) {
    return static_cast<off_t>(
        k_concurrency_checkpoint_lsn_records_offset +
        static_cast<off_t>(index * k_concurrency_checkpoint_lsn_record_size)
    );
}

void build_concurrency_checkpoint_lsn_record(
    std::array<unsigned char, k_concurrency_checkpoint_lsn_record_size> &record,
    std::uint64_t generation,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn
) {
    if (visible_lsn > latest_lsn) {
        latest_lsn = visible_lsn;
    }
    record.fill(0U);
    std::memcpy(
        record.data() + k_concurrency_checkpoint_lsn_record_magic_offset,
        k_concurrency_checkpoint_lsn_record_magic.data(),
        k_concurrency_checkpoint_lsn_record_magic.size()
    );
    store_le32(
        record.data(),
        k_concurrency_checkpoint_lsn_record_format_offset,
        k_concurrency_checkpoint_lsn_record_format
    );
    store_le64(record.data(), k_concurrency_checkpoint_lsn_record_generation_offset, generation);
    store_le64(record.data(), k_concurrency_checkpoint_lsn_record_latest_lsn_offset, latest_lsn);
    store_le64(record.data(), k_concurrency_checkpoint_lsn_record_visible_lsn_offset, visible_lsn);
    store_le32(
        record.data(),
        k_concurrency_checkpoint_lsn_record_checksum_offset,
        my_crc32c(0, record.data(), k_concurrency_checkpoint_lsn_record_checksum_offset)
    );
}

bool parse_concurrency_checkpoint_lsn_record(
    const std::array<unsigned char, k_concurrency_checkpoint_lsn_record_size> &record,
    OwnerlessCheckpointLsnRecord *out_record,
    bool *out_empty
) {
    if (out_record == nullptr || out_empty == nullptr) {
        return false;
    }
    *out_empty =
        std::all_of(record.begin(), record.end(), [](unsigned char value) { return value == 0U; });
    if (*out_empty) {
        *out_record = {};
        return true;
    }

    if (std::memcmp(
            record.data() + k_concurrency_checkpoint_lsn_record_magic_offset,
            k_concurrency_checkpoint_lsn_record_magic.data(),
            k_concurrency_checkpoint_lsn_record_magic.size()
        ) != 0 ||
        load_le32(record.data(), k_concurrency_checkpoint_lsn_record_format_offset) !=
            k_concurrency_checkpoint_lsn_record_format ||
        load_le32(record.data(), k_concurrency_checkpoint_lsn_record_reserved_offset) != 0U) {
        return false;
    }
    const std::uint32_t stored_checksum =
        load_le32(record.data(), k_concurrency_checkpoint_lsn_record_checksum_offset);
    const std::uint32_t computed_checksum =
        my_crc32c(0, record.data(), k_concurrency_checkpoint_lsn_record_checksum_offset);
    if (stored_checksum != computed_checksum) {
        return false;
    }

    out_record->generation =
        load_le64(record.data(), k_concurrency_checkpoint_lsn_record_generation_offset);
    out_record->latest_lsn =
        load_le64(record.data(), k_concurrency_checkpoint_lsn_record_latest_lsn_offset);
    out_record->visible_lsn =
        load_le64(record.data(), k_concurrency_checkpoint_lsn_record_visible_lsn_offset);
    if (out_record->generation == 0U || out_record->latest_lsn == 0U ||
        out_record->visible_lsn > out_record->latest_lsn) {
        return false;
    }
    return true;
}

bool ownerless_checkpoint_generation_cache_allowed(
    const void *process_registry,
    std::size_t process_registry_size,
    std::uint64_t owner_generation,
    std::uint64_t *out_registry_generation
) {
    if (out_registry_generation == nullptr) {
        return false;
    }
    *out_registry_generation = 0U;
    if (process_registry == nullptr ||
        process_registry_size < MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE ||
        owner_generation == 0U) {
        return false;
    }

    const std::uint64_t active_count =
        mylite_ownerless_process_registry_active_count(process_registry);
    const std::uint64_t registry_generation =
        mylite_ownerless_process_registry_generation(process_registry);
    if (active_count != 1U || registry_generation != owner_generation) {
        return false;
    }
    *out_registry_generation = registry_generation;
    return true;
}

bool update_concurrency_checkpoint_lsn_from_redo_state(
    int checkpoint_fd,
    void *redo_state,
    std::size_t redo_state_size,
    const void *process_registry,
    std::size_t process_registry_size,
    std::uint64_t owner_generation,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_TOTAL_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_CALLS, 1U);
    if (checkpoint_fd < 0 || redo_state == nullptr ||
        redo_state_size < k_concurrency_redo_state_segment_size ||
        (latest_lsn == 0U && visible_lsn == 0U)) {
        return false;
    }
    std::uint64_t stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    if (!acquire_fd_write_lock(
            checkpoint_fd,
            k_concurrency_checkpoint_lock_start,
            k_concurrency_checkpoint_lock_length
        )) {
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_LOCK_NS,
            stage_start_ns
        );
        return false;
    }
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_LOCK_NS,
        stage_start_ns
    );

    mylite_ownerless_redo_state_snapshot snapshot = {};
    bool ok = mylite_ownerless_redo_state_read_snapshot(redo_state, redo_state_size, &snapshot) ==
              MYLITE_OWNERLESS_REDO_STATE_OK;
    if (ok) {
        latest_lsn = std::max(latest_lsn, snapshot.latest_lsn);
        visible_lsn = std::max(visible_lsn, snapshot.visible_lsn);
        visible_lsn = std::max(visible_lsn, snapshot.durable_lsn);
        ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_FILE_READ_ELIDED, 1U);
        std::uint64_t registry_generation = 0U;
        if (ownerless_checkpoint_generation_cache_allowed(
                process_registry,
                process_registry_size,
                owner_generation,
                &registry_generation
            )) {
            ok = write_concurrency_checkpoint_lsn_with_generation_cache_locked(
                checkpoint_fd,
                latest_lsn,
                visible_lsn,
                durable,
                registry_generation
            );
        } else {
            reset_ownerless_checkpoint_lsn_generation_cache();
            ok = write_concurrency_checkpoint_lsn_locked(
                checkpoint_fd,
                latest_lsn,
                visible_lsn,
                durable
            );
        }
    }

    release_fd_lock(
        checkpoint_fd,
        k_concurrency_checkpoint_lock_start,
        k_concurrency_checkpoint_lock_length
    );
    return ok;
}

bool update_concurrency_checkpoint_lsn(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
) {
    OwnerlessDatabasePerfScope perf_scope(OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_TOTAL_NS);
    ownerless_database_perf_add(OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_CALLS, 1U);
    if (checkpoint_fd < 0 || (latest_lsn == 0U && visible_lsn == 0U)) {
        return false;
    }
    reset_ownerless_checkpoint_lsn_generation_cache();
    std::uint64_t stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    if (!acquire_fd_write_lock(
            checkpoint_fd,
            k_concurrency_checkpoint_lock_start,
            k_concurrency_checkpoint_lock_length
        )) {
        ownerless_database_perf_add_elapsed(
            OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_LOCK_NS,
            stage_start_ns
        );
        return false;
    }
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_LOCK_NS,
        stage_start_ns
    );

    std::uint64_t current_latest_lsn = 0;
    std::uint64_t current_visible_lsn = 0;
    stage_start_ns =
        ownerless_database_perf_stats_are_enabled() ? ownerless_database_perf_now_ns() : 0U;
    bool ok =
        read_concurrency_checkpoint_lsn(checkpoint_fd, &current_latest_lsn, &current_visible_lsn);
    ownerless_database_perf_add_elapsed(
        OWNERLESS_DATABASE_PERF_CHECKPOINT_UPDATE_READ_NS,
        stage_start_ns
    );
    if (ok) {
        latest_lsn = std::max(latest_lsn, current_latest_lsn);
        visible_lsn = std::max(visible_lsn, current_visible_lsn);
        ok = write_concurrency_checkpoint_lsn_locked(
            checkpoint_fd,
            latest_lsn,
            visible_lsn,
            durable
        );
    }

    release_fd_lock(
        checkpoint_fd,
        k_concurrency_checkpoint_lock_start,
        k_concurrency_checkpoint_lock_length
    );
    reset_ownerless_checkpoint_lsn_generation_cache();
    return ok;
}

extern "C" int mylite_ownerless_database_test_update_checkpoint_lsn_repeated(
    const char *database_path,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    int durable,
    unsigned repetitions
) {
    if (database_path == nullptr || (latest_lsn == 0U && visible_lsn == 0U) || repetitions == 0U) {
        return MYLITE_MISUSE;
    }
    const std::filesystem::path checkpoint_path = std::filesystem::path(database_path) /
                                                  k_concurrency_dir_name /
                                                  k_concurrency_checkpoint_filename;
    const std::string checkpoint_name = checkpoint_path.string();
    const int checkpoint_fd = ::open(checkpoint_name.c_str(), O_RDWR | O_CLOEXEC);
    if (checkpoint_fd < 0) {
        return MYLITE_IOERR;
    }
    reset_ownerless_checkpoint_lsn_sync_anchor();
    reset_ownerless_checkpoint_lsn_generation_cache();

    int result = MYLITE_OK;
    for (unsigned iteration = 0; iteration < repetitions; ++iteration) {
        if (!update_concurrency_checkpoint_lsn(
                checkpoint_fd,
                latest_lsn,
                visible_lsn,
                durable != 0
            )) {
            result = MYLITE_IOERR;
            break;
        }
    }

    reset_ownerless_checkpoint_lsn_sync_anchor();
    reset_ownerless_checkpoint_lsn_generation_cache();
    if (::close(checkpoint_fd) != 0 && result == MYLITE_OK) {
        result = MYLITE_IOERR;
    }
    return result;
}

bool read_concurrency_checkpoint_lsn(
    int checkpoint_fd,
    std::uint64_t *out_latest_lsn,
    std::uint64_t *out_visible_lsn
) {
    if (checkpoint_fd < 0 || out_latest_lsn == nullptr || out_visible_lsn == nullptr) {
        return false;
    }

    OwnerlessCheckpointLsnRecord record = {};
    bool has_record = false;
    if (!read_concurrency_checkpoint_lsn_records(checkpoint_fd, &record, &has_record)) {
        return false;
    }
    if (has_record) {
        *out_latest_lsn = record.latest_lsn;
        *out_visible_lsn = record.visible_lsn;
        return true;
    }

    return read_concurrency_checkpoint_legacy_lsn(checkpoint_fd, out_latest_lsn, out_visible_lsn);
}

bool read_concurrency_checkpoint_lsn_records(
    int checkpoint_fd,
    OwnerlessCheckpointLsnRecord *out_record,
    bool *out_has_record
) {
    if (checkpoint_fd < 0 || out_record == nullptr || out_has_record == nullptr) {
        return false;
    }
    *out_record = {};
    *out_has_record = false;
    bool saw_nonempty_record = false;
    OwnerlessCheckpointLsnRecord best_record = {};
    bool has_best_record = false;

    for (std::size_t index = 0; index < k_concurrency_checkpoint_lsn_record_count; ++index) {
        std::array<unsigned char, k_concurrency_checkpoint_lsn_record_size> bytes = {};
        ssize_t bytes_read = 0;
        do {
            bytes_read = ::pread(
                checkpoint_fd,
                bytes.data(),
                bytes.size(),
                concurrency_checkpoint_lsn_record_offset(index)
            );
        } while (bytes_read < 0 && errno == EINTR);
        if (bytes_read == 0) {
            continue;
        }
        if (bytes_read != static_cast<ssize_t>(bytes.size())) {
            saw_nonempty_record = true;
            continue;
        }

        OwnerlessCheckpointLsnRecord record = {};
        bool empty_record = false;
        if (!parse_concurrency_checkpoint_lsn_record(bytes, &record, &empty_record)) {
            saw_nonempty_record = true;
            continue;
        }
        if (empty_record) {
            continue;
        }

        saw_nonempty_record = true;
        if (!has_best_record || record.generation > best_record.generation) {
            best_record = record;
            has_best_record = true;
        }
    }

    if (has_best_record) {
        *out_record = best_record;
        *out_has_record = true;
        return true;
    }

    return !saw_nonempty_record;
}

bool read_concurrency_checkpoint_legacy_lsn(
    int checkpoint_fd,
    std::uint64_t *out_latest_lsn,
    std::uint64_t *out_visible_lsn
) {
    if (checkpoint_fd < 0 || out_latest_lsn == nullptr || out_visible_lsn == nullptr) {
        return false;
    }

    std::array<unsigned char, 16> payload = {};
    ssize_t bytes_read = 0;
    do {
        bytes_read = ::pread(
            checkpoint_fd,
            payload.data(),
            payload.size(),
            static_cast<off_t>(k_concurrency_checkpoint_latest_lsn_offset)
        );
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read == 0) {
        *out_latest_lsn = 0U;
        *out_visible_lsn = 0U;
        return true;
    }
    if (bytes_read != static_cast<ssize_t>(payload.size())) {
        return false;
    }

    *out_latest_lsn = load_le64(payload.data(), 0U);
    *out_visible_lsn = load_le64(payload.data(), sizeof(std::uint64_t));
    if (*out_visible_lsn > *out_latest_lsn) {
        *out_latest_lsn = *out_visible_lsn;
    }
    return true;
}

bool read_concurrency_native_file_op_checkpoint_records(
    int checkpoint_fd,
    OwnerlessCheckpointNativeFileOpRecord *out_record,
    bool *out_has_record,
    bool *out_saw_nonempty_record
) {
    if (checkpoint_fd < 0 || out_record == nullptr || out_has_record == nullptr ||
        out_saw_nonempty_record == nullptr) {
        return false;
    }
    *out_record = {};
    *out_has_record = false;
    *out_saw_nonempty_record = false;
    OwnerlessCheckpointNativeFileOpRecord best_record = {};
    bool has_best_record = false;

    for (std::size_t index = 0; index < k_concurrency_checkpoint_native_file_op_record_count;
         ++index) {
        std::array<unsigned char, k_concurrency_checkpoint_native_file_op_record_size> bytes = {};
        ssize_t bytes_read = 0;
        do {
            bytes_read = ::pread(
                checkpoint_fd,
                bytes.data(),
                bytes.size(),
                concurrency_native_file_op_checkpoint_record_offset(index)
            );
        } while (bytes_read < 0 && errno == EINTR);
        if (bytes_read == 0) {
            continue;
        }
        if (bytes_read != static_cast<ssize_t>(bytes.size())) {
            *out_saw_nonempty_record = true;
            continue;
        }

        OwnerlessCheckpointNativeFileOpRecord record = {};
        bool empty_record = false;
        if (!parse_concurrency_native_file_op_checkpoint_record(bytes, &record, &empty_record)) {
            *out_saw_nonempty_record = true;
            continue;
        }
        if (empty_record) {
            continue;
        }

        *out_saw_nonempty_record = true;
        if (!has_best_record || record.generation > best_record.generation) {
            best_record = record;
            has_best_record = true;
        }
    }

    if (has_best_record) {
        *out_record = best_record;
        *out_has_record = true;
    }
    return true;
}

void build_concurrency_native_file_op_checkpoint_record(
    std::array<unsigned char, k_concurrency_checkpoint_native_file_op_record_size> &record,
    std::uint64_t generation,
    bool needed
) {
    record.fill(0U);
    std::memcpy(
        record.data() + k_concurrency_checkpoint_native_file_op_record_magic_offset,
        k_concurrency_checkpoint_native_file_op_record_magic.data(),
        k_concurrency_checkpoint_native_file_op_record_magic.size()
    );
    store_le32(
        record.data(),
        k_concurrency_checkpoint_native_file_op_record_format_offset,
        k_concurrency_checkpoint_native_file_op_record_format
    );
    store_le64(
        record.data(),
        k_concurrency_checkpoint_native_file_op_record_generation_offset,
        generation
    );
    store_le64(
        record.data(),
        k_concurrency_checkpoint_native_file_op_record_needed_offset,
        needed ? 1U : 0U
    );
    store_le32(
        record.data(),
        k_concurrency_checkpoint_native_file_op_record_checksum_offset,
        my_crc32c(0, record.data(), k_concurrency_checkpoint_native_file_op_record_checksum_offset)
    );
}

bool parse_concurrency_native_file_op_checkpoint_record(
    const std::array<unsigned char, k_concurrency_checkpoint_native_file_op_record_size> &record,
    OwnerlessCheckpointNativeFileOpRecord *out_record,
    bool *out_empty
) {
    if (out_record == nullptr || out_empty == nullptr) {
        return false;
    }
    *out_empty =
        std::all_of(record.begin(), record.end(), [](unsigned char value) { return value == 0U; });
    if (*out_empty) {
        *out_record = {};
        return true;
    }

    if (std::memcmp(
            record.data() + k_concurrency_checkpoint_native_file_op_record_magic_offset,
            k_concurrency_checkpoint_native_file_op_record_magic.data(),
            k_concurrency_checkpoint_native_file_op_record_magic.size()
        ) != 0 ||
        load_le32(record.data(), k_concurrency_checkpoint_native_file_op_record_format_offset) !=
            k_concurrency_checkpoint_native_file_op_record_format ||
        load_le32(record.data(), k_concurrency_checkpoint_native_file_op_record_reserved_offset) !=
            0U) {
        return false;
    }
    const std::uint32_t stored_checksum =
        load_le32(record.data(), k_concurrency_checkpoint_native_file_op_record_checksum_offset);
    const std::uint32_t computed_checksum =
        my_crc32c(0, record.data(), k_concurrency_checkpoint_native_file_op_record_checksum_offset);
    if (stored_checksum != computed_checksum) {
        return false;
    }

    out_record->generation =
        load_le64(record.data(), k_concurrency_checkpoint_native_file_op_record_generation_offset);
    const std::uint64_t needed =
        load_le64(record.data(), k_concurrency_checkpoint_native_file_op_record_needed_offset);
    if (out_record->generation == 0U || needed > 1U) {
        return false;
    }
    out_record->needed = needed != 0U;
    return true;
}

off_t concurrency_native_file_op_checkpoint_record_offset(std::size_t index) {
    return static_cast<off_t>(
        k_concurrency_checkpoint_native_file_op_records_offset +
        static_cast<off_t>(index * k_concurrency_checkpoint_native_file_op_record_size)
    );
}

bool write_concurrency_native_file_op_checkpoint_locked(int checkpoint_fd, bool needed) {
    OwnerlessCheckpointNativeFileOpRecord current_record = {};
    bool has_current_record = false;
    bool saw_nonempty_record = false;
    if (!read_concurrency_native_file_op_checkpoint_records(
            checkpoint_fd,
            &current_record,
            &has_current_record,
            &saw_nonempty_record
        )) {
        return false;
    }
    const std::uint64_t next_generation =
        has_current_record && current_record.generation < UINT64_MAX
            ? current_record.generation + 1U
            : 1U;
    const std::size_t record_index = static_cast<std::size_t>(
        (next_generation - 1U) % k_concurrency_checkpoint_native_file_op_record_count
    );

    std::array<unsigned char, k_concurrency_checkpoint_native_file_op_record_size> record = {};
    build_concurrency_native_file_op_checkpoint_record(record, next_generation, needed);
    std::array<unsigned char, sizeof(std::uint64_t)> legacy_payload = {};
    store_le64(legacy_payload.data(), 0U, needed ? 1U : 0U);
    return write_exact_at(
               checkpoint_fd,
               record.data(),
               record.size(),
               concurrency_native_file_op_checkpoint_record_offset(record_index)
           ) &&
           write_exact_at(
               checkpoint_fd,
               legacy_payload.data(),
               legacy_payload.size(),
               static_cast<off_t>(k_concurrency_checkpoint_native_file_op_needed_offset)
           ) &&
           ::fsync(checkpoint_fd) == 0;
}

bool read_concurrency_native_file_op_checkpoint_legacy_needed(int checkpoint_fd, bool *out_needed) {
    if (checkpoint_fd < 0 || out_needed == nullptr) {
        return false;
    }

    std::array<unsigned char, sizeof(std::uint64_t)> payload = {};
    ssize_t bytes_read = 0;
    do {
        bytes_read = ::pread(
            checkpoint_fd,
            payload.data(),
            payload.size(),
            static_cast<off_t>(k_concurrency_checkpoint_native_file_op_needed_offset)
        );
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read == 0) {
        *out_needed = false;
        return true;
    }
    if (bytes_read != static_cast<ssize_t>(payload.size())) {
        return false;
    }

    *out_needed = load_le64(payload.data(), 0U) != 0U;
    return true;
}

bool mark_concurrency_native_file_op_checkpoint_needed(int checkpoint_fd) {
    if (checkpoint_fd < 0) {
        return false;
    }
    if (!acquire_fd_write_lock(
            checkpoint_fd,
            k_concurrency_checkpoint_lock_start,
            k_concurrency_checkpoint_lock_length
        )) {
        return false;
    }

    const bool ok = write_concurrency_native_file_op_checkpoint_locked(checkpoint_fd, true);
    release_fd_lock(
        checkpoint_fd,
        k_concurrency_checkpoint_lock_start,
        k_concurrency_checkpoint_lock_length
    );
    return ok;
}

bool read_concurrency_native_file_op_checkpoint_needed(int checkpoint_fd, bool *out_needed) {
    if (checkpoint_fd < 0 || out_needed == nullptr) {
        return false;
    }
    if (!acquire_fd_write_lock(
            checkpoint_fd,
            k_concurrency_checkpoint_lock_start,
            k_concurrency_checkpoint_lock_length
        )) {
        return false;
    }

    OwnerlessCheckpointNativeFileOpRecord record = {};
    bool has_record = false;
    bool saw_nonempty_record = false;
    bool ok = read_concurrency_native_file_op_checkpoint_records(
        checkpoint_fd,
        &record,
        &has_record,
        &saw_nonempty_record
    );
    if (ok && has_record) {
        *out_needed = record.needed;
    } else if (ok && saw_nonempty_record) {
        *out_needed = true;
    } else if (ok) {
        ok = read_concurrency_native_file_op_checkpoint_legacy_needed(checkpoint_fd, out_needed);
    }

    release_fd_lock(
        checkpoint_fd,
        k_concurrency_checkpoint_lock_start,
        k_concurrency_checkpoint_lock_length
    );
    return ok;
}

bool clear_concurrency_native_file_op_checkpoint_needed(int checkpoint_fd) {
    if (checkpoint_fd < 0) {
        return false;
    }
    if (!acquire_fd_write_lock(
            checkpoint_fd,
            k_concurrency_checkpoint_lock_start,
            k_concurrency_checkpoint_lock_length
        )) {
        return false;
    }

    const bool ok = write_concurrency_native_file_op_checkpoint_locked(checkpoint_fd, false);
    release_fd_lock(
        checkpoint_fd,
        k_concurrency_checkpoint_lock_start,
        k_concurrency_checkpoint_lock_length
    );
    return ok;
}

bool acquire_fd_write_lock(int fd, off_t start, off_t length) {
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    while (::fcntl(fd, F_SETLKW, &lock) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

void release_fd_lock(int fd, off_t start, off_t length) {
    struct flock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    static_cast<void>(::fcntl(fd, F_SETLK, &lock));
}

std::uint64_t current_time_milliseconds(void) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count()
    );
}

bool read_exact_at(int fd, unsigned char *data, std::size_t length, off_t offset) {
    while (length > 0U) {
        const ssize_t bytes_read = ::pread(fd, data, length, offset);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytes_read == 0) {
            return false;
        }
        data += bytes_read;
        length -= static_cast<std::size_t>(bytes_read);
        offset += bytes_read;
    }
    return true;
}

bool write_exact_at(int fd, const unsigned char *data, std::size_t length, off_t offset) {
    while (length > 0U) {
        const ssize_t bytes_written = ::pwrite(fd, data, length, offset);
        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytes_written == 0) {
            return false;
        }
        data += bytes_written;
        length -= static_cast<std::size_t>(bytes_written);
        offset += bytes_written;
    }
    return true;
}

bool sync_fd_data(int fd) {
#  if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    while (::fdatasync(fd) != 0) {
#  else
    while (::fsync(fd) != 0) {
#  endif
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool ownerless_redo_prefix_has_valid_current_header(const unsigned char *prefix) {
    if (!std::equal(
            k_mariadb_physical_redo_header_magic.begin(),
            k_mariadb_physical_redo_header_magic.end(),
            prefix
        )) {
        return false;
    }
    return load_be32(prefix, k_ownerless_redo_header_checksum_offset) ==
           my_crc32c(0, prefix, k_ownerless_redo_header_checksum_offset);
}

bool ownerless_redo_prefix_has_valid_current_checkpoint(const unsigned char *prefix) {
    if (!ownerless_redo_prefix_has_valid_current_header(prefix) ||
        load_be32(prefix, 0) != k_mariadb_redo_format_10_8) {
        return false;
    }
    for (std::size_t index = 4U; index < 8U; ++index) {
        if (prefix[index] != 0U) {
            return false;
        }
    }

    const std::uint64_t first_lsn = load_be64(prefix, 8);
    if (first_lsn < k_ownerless_redo_startup_prefix_size) {
        return false;
    }

    const auto checkpoint_valid = [&](std::size_t offset) {
        const unsigned char *checkpoint = prefix + offset;
        const std::uint64_t checkpoint_lsn =
            load_be64(checkpoint, k_ownerless_redo_checkpoint_lsn_offset);
        const std::uint64_t end_lsn =
            load_be64(checkpoint, k_ownerless_redo_checkpoint_end_lsn_offset);
        if (checkpoint_lsn < first_lsn || end_lsn < checkpoint_lsn) {
            return false;
        }
        const unsigned char *reserved = checkpoint + k_ownerless_redo_checkpoint_reserved_offset;
        if (!std::all_of(
                reserved,
                reserved + k_ownerless_redo_checkpoint_reserved_length,
                [](unsigned char value) { return value == 0U; }
            )) {
            return false;
        }
        return load_be32(checkpoint, k_ownerless_redo_checkpoint_checksum_offset) ==
               my_crc32c(0, checkpoint, k_ownerless_redo_checkpoint_checksum_offset);
    };

    return checkpoint_valid(k_ownerless_redo_checkpoint_1_offset) ||
           checkpoint_valid(k_ownerless_redo_checkpoint_2_offset);
}

using FilesystemPath = std::filesystem::path;

FilesystemPath ownerless_redo_header_backup_path(const FilesystemPath &database_path) {
    return database_path / k_concurrency_dir_name / k_concurrency_redo_header_filename;
}

bool write_ownerless_redo_header_backup(
    const std::filesystem::path &database_path,
    const OwnerlessRedoStartupPrefixSnapshot &snapshot
) {
    if (!snapshot.captured ||
        !ownerless_redo_prefix_has_valid_current_checkpoint(snapshot.prefix.data()) ||
        snapshot.file_size < static_cast<off_t>(k_ownerless_redo_startup_prefix_size)) {
        return false;
    }

    std::array<unsigned char, k_ownerless_redo_header_backup_header_size> header = {};
    std::copy(
        k_concurrency_redo_header_magic.begin(),
        k_concurrency_redo_header_magic.end(),
        header.begin()
    );
    store_le32(
        header.data(),
        k_ownerless_redo_header_backup_format_offset,
        k_ownerless_redo_header_backup_format
    );
    store_le32(
        header.data(),
        k_ownerless_redo_header_backup_header_size_offset,
        static_cast<std::uint32_t>(k_ownerless_redo_header_backup_header_size)
    );
    store_le64(
        header.data(),
        k_ownerless_redo_header_backup_file_size_offset,
        static_cast<std::uint64_t>(snapshot.file_size)
    );
    store_le32(
        header.data(),
        k_ownerless_redo_header_backup_payload_size_offset,
        static_cast<std::uint32_t>(k_ownerless_redo_startup_prefix_size)
    );

    const std::filesystem::path backup_path = ownerless_redo_header_backup_path(database_path);
    const std::string backup_name = backup_path.string();
    const int fd = ::open(backup_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }

    const bool ok = write_exact_at(fd, header.data(), header.size(), 0) &&
                    write_exact_at(
                        fd,
                        snapshot.prefix.data(),
                        k_ownerless_redo_startup_prefix_size,
                        static_cast<off_t>(k_ownerless_redo_header_backup_payload_offset)
                    ) &&
                    ::fsync(fd) == 0;
    static_cast<void>(::close(fd));
    return ok;
}

bool read_ownerless_redo_header_backup(
    const std::filesystem::path &database_path,
    const std::filesystem::path &redo_path,
    off_t redo_file_size,
    OwnerlessRedoStartupPrefixSnapshot &snapshot
) {
    snapshot = {};
    if (redo_file_size < static_cast<off_t>(k_ownerless_redo_startup_prefix_size)) {
        return false;
    }

    const std::filesystem::path backup_path = ownerless_redo_header_backup_path(database_path);
    const std::string backup_name = backup_path.string();
    const int fd = ::open(backup_name.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    const off_t minimum_size = static_cast<off_t>(
        k_ownerless_redo_header_backup_payload_offset + k_ownerless_redo_startup_prefix_size
    );
    struct stat backup_stat = {};
    if (::fstat(fd, &backup_stat) != 0 || backup_stat.st_size < minimum_size) {
        static_cast<void>(::close(fd));
        return false;
    }

    std::array<unsigned char, k_ownerless_redo_header_backup_header_size> header = {};
    std::array<unsigned char, k_ownerless_redo_startup_prefix_size> prefix = {};
    const bool read_ok = read_exact_at(fd, header.data(), header.size(), 0) &&
                         read_exact_at(
                             fd,
                             prefix.data(),
                             k_ownerless_redo_startup_prefix_size,
                             static_cast<off_t>(k_ownerless_redo_header_backup_payload_offset)
                         );
    static_cast<void>(::close(fd));
    if (!read_ok) {
        return false;
    }

    const std::uint64_t backed_redo_size =
        load_le64(header.data(), k_ownerless_redo_header_backup_file_size_offset);
    const off_t backed_redo_file_size = static_cast<off_t>(backed_redo_size);
    const off_t redo_file_size_delta = backed_redo_file_size >= redo_file_size
                                           ? backed_redo_file_size - redo_file_size
                                           : redo_file_size - backed_redo_file_size;
    if (!std::equal(
            k_concurrency_redo_header_magic.begin(),
            k_concurrency_redo_header_magic.end(),
            header.begin()
        ) ||
        load_le32(header.data(), k_ownerless_redo_header_backup_format_offset) !=
            k_ownerless_redo_header_backup_format ||
        load_le32(header.data(), k_ownerless_redo_header_backup_header_size_offset) !=
            k_ownerless_redo_header_backup_header_size ||
        load_le32(header.data(), k_ownerless_redo_header_backup_payload_size_offset) !=
            k_ownerless_redo_startup_prefix_size ||
        backed_redo_file_size < static_cast<off_t>(k_ownerless_redo_startup_prefix_size) ||
        redo_file_size_delta > k_ownerless_redo_backup_file_size_tolerance ||
        !ownerless_redo_prefix_has_valid_current_checkpoint(prefix.data())) {
        return false;
    }

    snapshot.captured = true;
    snapshot.restore_file_size = false;
    snapshot.restore_prefix_size = k_ownerless_redo_startup_prefix_size;
    snapshot.path = redo_path;
    snapshot.prefix = prefix;
    snapshot.file_size = redo_file_size;
    return true;
}

bool ownerless_redo_header_backup_is_valid(const std::filesystem::path &database_path) {
    const std::filesystem::path backup_path = ownerless_redo_header_backup_path(database_path);
    struct stat backup_stat = {};
    if (::stat(backup_path.string().c_str(), &backup_stat) != 0) {
        return false;
    }

    const std::filesystem::path redo_path =
        database_path / k_datadir_name / k_innodb_redo_log_filename;
    struct stat redo_stat = {};
    OwnerlessRedoStartupPrefixSnapshot snapshot = {};
    return ::stat(redo_path.string().c_str(), &redo_stat) == 0 &&
           read_ownerless_redo_header_backup(database_path, redo_path, redo_stat.st_size, snapshot);
}

int capture_ownerless_redo_startup_prefix(
    const std::filesystem::path &database_path,
    OwnerlessRedoStartupPrefixSnapshot &snapshot,
    bool repair_from_backup,
    bool update_backup
) {
    snapshot = {};
    const std::filesystem::path redo_path =
        database_path / k_datadir_name / k_innodb_redo_log_filename;
    const std::string redo_name = redo_path.string();
    const int fd = ::open(redo_name.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? MYLITE_OK : MYLITE_IOERR;
    }

    struct stat redo_stat = {};
    if (::fstat(fd, &redo_stat) != 0) {
        static_cast<void>(::close(fd));
        return MYLITE_IOERR;
    }
    if (redo_stat.st_size < static_cast<off_t>(k_ownerless_redo_startup_prefix_size)) {
        static_cast<void>(::close(fd));
        return MYLITE_OK;
    }

    std::array<unsigned char, k_ownerless_redo_startup_prefix_size> prefix = {};
    if (!read_exact_at(fd, prefix.data(), prefix.size(), 0)) {
        static_cast<void>(::close(fd));
        return MYLITE_IOERR;
    }
    static_cast<void>(::close(fd));

    if (ownerless_redo_prefix_has_valid_current_checkpoint(prefix.data())) {
        snapshot.captured = true;
        snapshot.restore_file_size = true;
        snapshot.restore_prefix_size = k_ownerless_redo_startup_prefix_size;
        snapshot.path = redo_path;
        snapshot.prefix = prefix;
        snapshot.file_size = redo_stat.st_size;
        if (update_backup && !write_ownerless_redo_header_backup(database_path, snapshot)) {
            snapshot = {};
            return MYLITE_IOERR;
        }
        return MYLITE_OK;
    }

    if (repair_from_backup &&
        read_ownerless_redo_header_backup(database_path, redo_path, redo_stat.st_size, snapshot) &&
        !restore_ownerless_redo_startup_prefix(snapshot)) {
        snapshot = {};
        return MYLITE_IOERR;
    }
    return MYLITE_OK;
}

bool restore_ownerless_redo_startup_prefix(const OwnerlessRedoStartupPrefixSnapshot &snapshot) {
    if (!snapshot.captured) {
        return true;
    }

    const std::string redo_name = snapshot.path.string();
    const int fd = ::open(redo_name.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    const std::size_t restore_size = snapshot.restore_prefix_size == 0U
                                         ? k_ownerless_redo_header_prefix_size
                                         : snapshot.restore_prefix_size;
    const bool size_restored =
        !snapshot.restore_file_size || ::ftruncate(fd, snapshot.file_size) == 0;
    const bool restored = size_restored &&
                          write_exact_at(fd, snapshot.prefix.data(), restore_size, 0) &&
                          ::fsync(fd) == 0;
    static_cast<void>(::close(fd));
    return restored;
}

bool restore_ownerless_redo_shutdown_header_if_needed(
    const OwnerlessRedoStartupPrefixSnapshot &snapshot
) {
    if (!snapshot.captured) {
        return true;
    }

    const std::string redo_name = snapshot.path.string();
    const int read_fd = ::open(redo_name.c_str(), O_RDONLY | O_CLOEXEC);
    if (read_fd >= 0) {
        std::array<unsigned char, k_ownerless_redo_startup_prefix_size> current_prefix = {};
        const bool current_prefix_read =
            read_exact_at(read_fd, current_prefix.data(), current_prefix.size(), 0);
        static_cast<void>(::close(read_fd));
        if (current_prefix_read &&
            ownerless_redo_prefix_has_valid_current_checkpoint(current_prefix.data())) {
            return true;
        }
    }

    OwnerlessRedoStartupPrefixSnapshot startup_snapshot = snapshot;
    startup_snapshot.restore_file_size = false;
    startup_snapshot.restore_prefix_size = k_ownerless_redo_startup_prefix_size;
    return restore_ownerless_redo_startup_prefix(startup_snapshot);
}

std::uint32_t load_le32(const unsigned char *bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t load_le64(const unsigned char *bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint32_t load_be32(const unsigned char *bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

std::uint64_t load_be64(const unsigned char *bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

void store_le32(unsigned char *bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU);
    }
}

void store_le64(unsigned char *bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU);
    }
}

std::uint64_t load_shared64(const unsigned char *bytes, std::size_t offset) {
    const auto *value = reinterpret_cast<const std::uint64_t *>(bytes + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

int validate_concurrency_metadata(const std::filesystem::path &metadata_path) {
    std::ifstream metadata(metadata_path, std::ios::binary);
    if (!metadata) {
        return MYLITE_IOERR;
    }

    bool has_format = false;
    bool has_mariadb_base = false;
    bool has_database_uuid = false;
    bool has_concurrency_generation = false;
    bool has_mode = false;
    const std::string mariadb_base_line = std::string("mariadb_base=") + k_mariadb_base_ref;
    for (std::string line; std::getline(metadata, line);) {
        if (line == k_metadata_format_line) {
            has_format = true;
            continue;
        }
        if (line == mariadb_base_line) {
            has_mariadb_base = true;
            continue;
        }
        if (line.rfind("database_uuid=", 0) == 0) {
            has_database_uuid = is_database_uuid(std::string_view(line).substr(14U));
            continue;
        }
        if (line.rfind("concurrency_generation=", 0) == 0) {
            has_concurrency_generation = is_unsigned_decimal(std::string_view(line).substr(23U));
            continue;
        }
        if (line == k_concurrency_mode_line) {
            has_mode = true;
        }
    }
    if (!metadata.eof()) {
        return MYLITE_IOERR;
    }

    return has_format && has_mariadb_base && has_database_uuid && has_concurrency_generation &&
                   has_mode
               ? MYLITE_OK
               : MYLITE_CORRUPT;
}

bool database_directory_is_empty(
    const std::filesystem::path &database_path,
    std::error_code &error
) {
    const std::filesystem::directory_iterator entry(database_path, error);
    if (error) {
        return false;
    }
    return entry == std::filesystem::directory_iterator();
}

int start_runtime(mylite_db &db, unsigned flags, const mylite_open_config *config) {
    EmbeddedOpenPerfScope start_scope(EMBEDDED_OPEN_PERF_START_RUNTIME_TOTAL_NS);
    embedded_open_perf_add(EMBEDDED_OPEN_PERF_START_RUNTIME_CALLS, 1U);
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    const bool ownerless_rw_open = (flags & MYLITE_OPEN_OWNERLESS_RW) != 0U;
    const bool ownerless_runtime_open =
        ownerless_rw_open || (flags & MYLITE_OPEN_SHARED_READONLY) != 0U;
    const int durability = configured_durability(config);
    if (g_runtime.ref_count > 0U) {
        if (g_runtime.database_path != db.database_path) {
            set_error(db, MYLITE_BUSY, "embedded runtime is already open for another database");
            return MYLITE_BUSY;
        }
        if (g_runtime.ownerless_rw_mode != ownerless_runtime_open) {
            set_error(db, MYLITE_BUSY, "embedded runtime is already open with a different mode");
            return MYLITE_BUSY;
        }
        if (g_runtime.readonly_mode != db.readonly_open) {
            set_error(
                db,
                MYLITE_BUSY,
                "embedded runtime is already open with a different access mode"
            );
            return MYLITE_BUSY;
        }
        if (g_runtime.durability != durability) {
            set_error(
                db,
                MYLITE_BUSY,
                "embedded runtime is already open with a different durability policy"
            );
            return MYLITE_BUSY;
        }
        db.ownerless_rw_open = g_runtime.ownerless_rw_mode;
        ++g_runtime.ref_count;
        return MYLITE_OK;
    }

    const bool memory_database = is_memory_database_path(db.database_path);
    const bool ownerless_concurrency_runtime_needed =
        !memory_database &&
        (ownerless_runtime_open || ownerless_concurrency_runtime_files_exist(db.database_path));
    const bool skip_database_lock =
        ownerless_runtime_open || unsafe_disable_database_lock_for_tests();
    int lock_fd = -1;
    if (!memory_database && !skip_database_lock) {
        const std::uint64_t stage_start_ns = embedded_open_perf_start_ns();
        lock_fd = acquire_database_lock(db, db.database_path, config);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_START_DATABASE_LOCK_NS, stage_start_ns);
        if (lock_fd < 0) {
            return db.errcode;
        }
    }

    bool concurrency_mapped = false;
    bool server_initialized = false;
    bool innodb_ownerless_hooks_needed = false;
    bool innodb_ownerless_uncheckpointed_file_recovery_needed = false;
    bool ordinary_native_page_log_reads = false;
    bool ordinary_native_checkpoint_refresh = false;
    std::uint64_t ordinary_native_checkpoint_visible_lsn = 0;
    RuntimeLayout layout = {};
    OwnerlessRedoStartupPrefixSnapshot redo_startup_prefix = {};
    try {
        if (ownerless_concurrency_runtime_needed) {
            std::uint64_t stage_start_ns = embedded_open_perf_start_ns();
            const int concurrency_result = prepare_concurrency_metadata(db.database_path);
            embedded_open_perf_add_elapsed(
                EMBEDDED_OPEN_PERF_START_CONCURRENCY_METADATA_NS,
                stage_start_ns
            );
            if (concurrency_result != MYLITE_OK) {
                release_database_lock(lock_fd);
                set_error(db, concurrency_result, "database concurrency metadata is invalid");
                return concurrency_result;
            }
            stage_start_ns = embedded_open_perf_start_ns();
            const int shared_memory_result =
                prepare_concurrency_shared_memory(db.database_path, !db.readonly_open);
            embedded_open_perf_add_elapsed(
                EMBEDDED_OPEN_PERF_START_SHARED_MEMORY_PREPARE_NS,
                stage_start_ns
            );
            if (shared_memory_result != MYLITE_OK) {
                release_database_lock(lock_fd);
                set_error(
                    db,
                    shared_memory_result,
                    "database concurrency shared memory is invalid"
                );
                return shared_memory_result;
            }
        }

        std::uint64_t stage_start_ns = embedded_open_perf_start_ns();
        layout = create_runtime_layout(db.database_path, config, !skip_database_lock);
        g_runtime.arguments =
            runtime_arguments(layout, ownerless_runtime_open, db.readonly_open, durability);
        g_runtime.argv = mutable_arguments(g_runtime.arguments);
        g_runtime.cleanup_directory = layout.cleanup_directory;
        g_runtime.cleanup_tmp_directory = layout.cleanup_tmp_directory;
        g_runtime.runtime_parent_directory = layout.runtime_parent_directory;
        g_runtime.database_path = db.database_path;
        g_runtime.ownerless_rw_mode = ownerless_runtime_open;
        g_runtime.readonly_mode = db.readonly_open;
        char *groups[] = {const_cast<char *>("server"), const_cast<char *>("embedded"), nullptr};
        embedded_open_perf_add_elapsed(
            EMBEDDED_OPEN_PERF_START_LAYOUT_ARGUMENTS_NS,
            stage_start_ns
        );

        if (ownerless_concurrency_runtime_needed) {
            stage_start_ns = embedded_open_perf_start_ns();
            const int concurrency_runtime_result =
                map_concurrency_shared_memory_for_runtime(db.database_path, g_runtime);
            embedded_open_perf_add_elapsed(
                EMBEDDED_OPEN_PERF_START_MAP_SHARED_MEMORY_NS,
                stage_start_ns
            );
            if (concurrency_runtime_result != MYLITE_OK) {
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(
                    db,
                    concurrency_runtime_result,
                    "database ownerless concurrency runtime is invalid"
                );
                return concurrency_runtime_result;
            }
            concurrency_mapped = true;

            stage_start_ns = embedded_open_perf_start_ns();
            const int page_log_result =
                open_concurrency_page_log_for_runtime(db.database_path, g_runtime);
            embedded_open_perf_add_elapsed(
                EMBEDDED_OPEN_PERF_START_OPEN_PAGE_LOG_NS,
                stage_start_ns
            );
            if (page_log_result != MYLITE_OK) {
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, page_log_result, "database ownerless page log is invalid");
                return page_log_result;
            }
            ordinary_native_page_log_reads =
                !db.readonly_open && ownerless_page_log_has_payload_records(g_runtime);
            g_runtime.ownerless_runtime_started_with_page_version_wal =
                ordinary_native_page_log_reads;
            if (ordinary_native_page_log_reads &&
                ownerless_runtime_has_external_page_version_pin(g_runtime)) {
                g_runtime.ownerless_runtime_consumed_external_snapshot_page_version_wal.store(
                    true,
                    std::memory_order_relaxed
                );
            }
            g_runtime.ownerless_innodb_lock_hook.page_log_reads_enabled =
                ownerless_runtime_open || ordinary_native_page_log_reads;
            g_runtime.ownerless_innodb_lock_hook.page_versioning_enabled = ownerless_runtime_open;
            stage_start_ns = embedded_open_perf_start_ns();
            const int checkpoint_result =
                open_concurrency_checkpoint_for_runtime(db.database_path, g_runtime);
            embedded_open_perf_add_elapsed(
                EMBEDDED_OPEN_PERF_START_OPEN_CHECKPOINT_NS,
                stage_start_ns
            );
            if (checkpoint_result != MYLITE_OK) {
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, checkpoint_result, "database ownerless checkpoint is invalid");
                return checkpoint_result;
            }
            if (!ownerless_runtime_open && !db.readonly_open) {
                std::uint64_t checkpoint_latest_lsn = 0;
                std::uint64_t checkpoint_visible_lsn = 0;
                if (!read_concurrency_checkpoint_lsn(
                        g_runtime.concurrency_checkpoint_fd,
                        &checkpoint_latest_lsn,
                        &checkpoint_visible_lsn
                    )) {
                    unmap_concurrency_shared_memory_for_runtime(g_runtime);
                    concurrency_mapped = false;
                    clear_runtime_state(g_runtime);
                    cleanup_runtime_layout(layout);
                    release_database_lock(lock_fd);
                    set_error(db, MYLITE_IOERR, "database ownerless checkpoint is invalid");
                    return MYLITE_IOERR;
                }
                if (checkpoint_visible_lsn != 0U) {
                    ordinary_native_checkpoint_refresh = true;
                    ordinary_native_checkpoint_visible_lsn = checkpoint_visible_lsn;
                }
            }

            if (ownerless_runtime_open) {
                stage_start_ns = embedded_open_perf_start_ns();
                const int lifecycle_hook_result =
                    install_ownerless_runtime_lifecycle_hooks(g_runtime);
                embedded_open_perf_add_elapsed(
                    EMBEDDED_OPEN_PERF_START_PRE_HOOKS_NS,
                    stage_start_ns
                );
                if (lifecycle_hook_result != MYLITE_OK) {
                    unmap_concurrency_shared_memory_for_runtime(g_runtime);
                    concurrency_mapped = false;
                    clear_runtime_state(g_runtime);
                    cleanup_runtime_layout(layout);
                    release_database_lock(lock_fd);
                    set_error(
                        db,
                        lifecycle_hook_result,
                        "database ownerless runtime hooks are invalid"
                    );
                    return lifecycle_hook_result;
                }
            } else {
                stage_start_ns = embedded_open_perf_start_ns();
                mylite_ownerless_runtime_reset_hooks();
                embedded_open_perf_add_elapsed(
                    EMBEDDED_OPEN_PERF_START_PRE_HOOKS_NS,
                    stage_start_ns
                );
            }

            innodb_ownerless_hooks_needed = ownerless_runtime_open ||
                                            ordinary_native_page_log_reads ||
                                            ordinary_native_checkpoint_refresh;
            if (innodb_ownerless_hooks_needed) {
                stage_start_ns = embedded_open_perf_start_ns();
                const int lock_hook_result = install_ownerless_innodb_lock_hooks(g_runtime);
                embedded_open_perf_add_elapsed(
                    EMBEDDED_OPEN_PERF_START_PRE_HOOKS_NS,
                    stage_start_ns
                );
                if (lock_hook_result != MYLITE_OK) {
                    unmap_concurrency_shared_memory_for_runtime(g_runtime);
                    concurrency_mapped = false;
                    clear_runtime_state(g_runtime);
                    cleanup_runtime_layout(layout);
                    release_database_lock(lock_fd);
                    set_error(db, lock_hook_result, "database ownerless lock hooks are invalid");
                    return lock_hook_result;
                }
            } else {
                stage_start_ns = embedded_open_perf_start_ns();
                mylite_ownerless_innodb_lock_reset_hooks();
                mylite_ownerless_innodb_autoinc_reset_hooks();
                embedded_open_perf_add_elapsed(
                    EMBEDDED_OPEN_PERF_START_PRE_HOOKS_NS,
                    stage_start_ns
                );
            }
            stage_start_ns = embedded_open_perf_start_ns();
            bool native_file_op_checkpoint_needed = false;
            if (!db.readonly_open) {
                static_cast<void>(read_concurrency_native_file_op_checkpoint_needed(
                    g_runtime.concurrency_checkpoint_fd,
                    &native_file_op_checkpoint_needed
                ));
            }
            const bool ownerless_redo_header_backup_available =
                !db.readonly_open && ownerless_redo_header_backup_is_valid(db.database_path);
            innodb_ownerless_uncheckpointed_file_recovery_needed =
                ownerless_runtime_open || ordinary_native_page_log_reads ||
                native_file_op_checkpoint_needed || ownerless_redo_header_backup_available;
            mylite_ownerless_innodb_set_checkpoint_suppression(
                ownerless_runtime_open && !db.readonly_open ? 1 : 0
            );
            mylite_ownerless_innodb_set_relative_file_op_redo_paths(
                ownerless_runtime_open && !db.readonly_open ? 1 : 0
            );
            mylite_ownerless_innodb_set_uncheckpointed_file_rename_recovery(
                innodb_ownerless_uncheckpointed_file_recovery_needed ? 1 : 0
            );
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
            if (!ownerless_runtime_open && !ordinary_native_page_log_reads &&
                !native_file_op_checkpoint_needed && ownerless_redo_header_backup_available) {
                pause_for_ownerless_test_fault("redo-header-backup-recovery-armed");
            }
#  endif
            if (!db.readonly_open && innodb_ownerless_uncheckpointed_file_recovery_needed) {
                const int redo_prefix_result = capture_ownerless_redo_startup_prefix(
                    db.database_path,
                    redo_startup_prefix,
                    true,
                    false
                );
                if (redo_prefix_result != MYLITE_OK) {
                    unmap_concurrency_shared_memory_for_runtime(g_runtime);
                    concurrency_mapped = false;
                    clear_runtime_state(g_runtime);
                    cleanup_runtime_layout(layout);
                    release_database_lock(lock_fd);
                    set_error(
                        db,
                        redo_prefix_result,
                        "database ownerless redo startup prefix is invalid"
                    );
                    return redo_prefix_result;
                }
            }
            mylite_ownerless_innodb_clear_external_page_visibility();
            embedded_open_perf_add_elapsed(
                EMBEDDED_OPEN_PERF_START_REDO_EVIDENCE_NS,
                stage_start_ns
            );
        } else {
            stage_start_ns = embedded_open_perf_start_ns();
            reset_ownerless_runtime_hooks(g_runtime);
            embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_START_PRE_HOOKS_NS, stage_start_ns);
        }

        int bootstrap_lock_fd = -1;
        if (!memory_database && skip_database_lock) {
            const std::filesystem::path concurrency_directory =
                std::filesystem::path(db.database_path) / k_concurrency_dir_name;
            if (!ownerless_runtime_open && unsafe_disable_database_lock_for_tests()) {
                std::error_code error;
                std::filesystem::create_directories(concurrency_directory, error);
                if (error) {
                    if (concurrency_mapped) {
                        unmap_concurrency_shared_memory_for_runtime(g_runtime);
                        concurrency_mapped = false;
                    }
                    clear_runtime_state(g_runtime);
                    cleanup_runtime_layout(layout);
                    release_database_lock(lock_fd);
                    set_error(
                        db,
                        MYLITE_IOERR,
                        "database concurrency directory could not be created"
                    );
                    return MYLITE_IOERR;
                }
            }
            const std::filesystem::path lock_path =
                concurrency_directory / k_concurrency_lock_filename;
            stage_start_ns = embedded_open_perf_start_ns();
            bootstrap_lock_fd = acquire_concurrency_lock(
                lock_path,
                k_system_tables_lock_start,
                k_system_tables_lock_length,
                F_WRLCK,
                k_system_tables_lock_wait_timeout_ms
            );
            embedded_open_perf_add_elapsed(
                EMBEDDED_OPEN_PERF_START_BOOTSTRAP_LOCK_NS,
                stage_start_ns
            );
            if (bootstrap_lock_fd < 0) {
                if (concurrency_mapped) {
                    unmap_concurrency_shared_memory_for_runtime(g_runtime);
                    concurrency_mapped = false;
                }
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, MYLITE_BUSY, "database runtime bootstrap is busy");
                return MYLITE_BUSY;
            }
        }

        stage_start_ns = embedded_open_perf_start_ns();
        const int init_result = mysql_server_init(
            static_cast<int>(g_runtime.argv.size()),
            g_runtime.argv.data(),
            groups
        );
        embedded_open_perf_add_elapsed(
            EMBEDDED_OPEN_PERF_START_MYSQL_SERVER_INIT_NS,
            stage_start_ns
        );
        if (init_result != 0) {
            const bool native_runtime_started = mylite_ownerless_innodb_current_lsn() != 0U ||
                                                mylite_ownerless_innodb_checkpoint_lsn() != 0U;
            int failure_result = MYLITE_ERROR;
            const char *failure_message = "MariaDB embedded runtime initialization failed";
            if (native_runtime_started) {
                mysql_server_end();
            }
            const auto restore_redo_after_startup_failure = [&]() {
                const std::filesystem::path redo_path = std::filesystem::path(db.database_path) /
                                                        k_datadir_name / k_innodb_redo_log_filename;
                struct stat redo_stat = {};
                OwnerlessRedoStartupPrefixSnapshot backup_redo_prefix = {};
                if (::stat(redo_path.string().c_str(), &redo_stat) == 0 &&
                    read_ownerless_redo_header_backup(
                        db.database_path,
                        redo_path,
                        redo_stat.st_size,
                        backup_redo_prefix
                    )) {
                    return restore_ownerless_redo_startup_prefix(backup_redo_prefix);
                }
                return restore_ownerless_redo_startup_prefix(redo_startup_prefix);
            };
            if (!db.readonly_open && !restore_redo_after_startup_failure()) {
                failure_result = MYLITE_IOERR;
                failure_message = "database ownerless redo startup prefix restore failed";
            }
            release_concurrency_lock(
                bootstrap_lock_fd,
                k_system_tables_lock_start,
                k_system_tables_lock_length
            );
            if (concurrency_mapped) {
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
            }
            clear_runtime_state(g_runtime);
            cleanup_runtime_layout(layout);
            release_database_lock(lock_fd);
            set_error(db, failure_result, failure_message);
            return failure_result;
        }
        server_initialized = true;
        if (!memory_database && innodb_ownerless_hooks_needed) {
            db.ownerless_preserve_native_recovery_pages = true;
        }

        if (ownerless_concurrency_runtime_needed) {
            stage_start_ns = embedded_open_perf_start_ns();
            int hook_result = MYLITE_OK;
            if (ownerless_runtime_open) {
                hook_result = install_ownerless_runtime_hooks(g_runtime);
            } else if (innodb_ownerless_hooks_needed) {
                hook_result = install_ownerless_innodb_lock_hooks(g_runtime);
            } else {
                mylite_ownerless_runtime_reset_hooks();
                mylite_ownerless_innodb_lock_reset_hooks();
                mylite_ownerless_innodb_autoinc_reset_hooks();
                mylite_ownerless_read_view_reset_hooks();
                mylite_ownerless_trx_reset_hooks();
                mylite_ownerless_mdl_reset_hooks();
            }
            embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_START_POST_HOOKS_NS, stage_start_ns);
            if (hook_result != MYLITE_OK) {
                mysql_server_end();
                server_initialized = false;
                release_concurrency_lock(
                    bootstrap_lock_fd,
                    k_system_tables_lock_start,
                    k_system_tables_lock_length
                );
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, hook_result, "database ownerless concurrency runtime is invalid");
                return hook_result;
            }
            advance_ownerless_local_trx_horizon(g_runtime);
            if (ordinary_native_page_log_reads || ordinary_native_checkpoint_refresh) {
                std::uint64_t latest_lsn = 0;
                std::uint64_t visible_lsn = 0;
                if (!read_concurrency_checkpoint_lsn(
                        g_runtime.concurrency_checkpoint_fd,
                        &latest_lsn,
                        &visible_lsn
                    )) {
                    mysql_server_end();
                    server_initialized = false;
                    release_concurrency_lock(
                        bootstrap_lock_fd,
                        k_system_tables_lock_start,
                        k_system_tables_lock_length
                    );
                    unmap_concurrency_shared_memory_for_runtime(g_runtime);
                    concurrency_mapped = false;
                    clear_runtime_state(g_runtime);
                    cleanup_runtime_layout(layout);
                    release_database_lock(lock_fd);
                    set_error(db, MYLITE_IOERR, "database ownerless checkpoint is invalid");
                    return MYLITE_IOERR;
                }
                if (visible_lsn != 0U) {
                    if (ordinary_native_checkpoint_visible_lsn > visible_lsn) {
                        visible_lsn = ordinary_native_checkpoint_visible_lsn;
                    }
                    db.ownerless_native_startup_refresh_lsn = visible_lsn;
                    mylite_ownerless_innodb_enable_external_page_visibility(visible_lsn);
                    if (ownerless_runtime_open) {
                        mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_no_skip(
                            visible_lsn
                        );
                    } else {
                        mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_no_skip(
                            visible_lsn
                        );
                    }
                }
            }
        }
        if (innodb_ownerless_uncheckpointed_file_recovery_needed && !db.readonly_open) {
            stage_start_ns = embedded_open_perf_start_ns();
            if (redo_startup_prefix.captured &&
                !write_ownerless_redo_header_backup(db.database_path, redo_startup_prefix)) {
                embedded_open_perf_add_elapsed(
                    EMBEDDED_OPEN_PERF_START_REDO_BACKUP_NS,
                    stage_start_ns
                );
                mysql_server_end();
                server_initialized = false;
                release_concurrency_lock(
                    bootstrap_lock_fd,
                    k_system_tables_lock_start,
                    k_system_tables_lock_length
                );
                if (concurrency_mapped) {
                    unmap_concurrency_shared_memory_for_runtime(g_runtime);
                    concurrency_mapped = false;
                }
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, MYLITE_IOERR, "database ownerless redo startup prefix is invalid");
                return MYLITE_IOERR;
            }
            embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_START_REDO_BACKUP_NS, stage_start_ns);
        }
        release_concurrency_lock(
            bootstrap_lock_fd,
            k_system_tables_lock_start,
            k_system_tables_lock_length
        );

        g_runtime.ref_count = 1;
        g_runtime.lock_fd = lock_fd;
        g_runtime.durability = durability;
        g_runtime.ownerless_rw_mode = ownerless_runtime_open;
        g_runtime.readonly_mode = db.readonly_open;
        stage_start_ns = embedded_open_perf_start_ns();
        const int scheduler_result = start_ownerless_checkpoint_scheduler(g_runtime);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_START_SCHEDULER_NS, stage_start_ns);
        if (scheduler_result != MYLITE_OK) {
            g_runtime.ref_count = 0;
            mysql_server_end();
            server_initialized = false;
            if (concurrency_mapped) {
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
            }
            clear_runtime_state(g_runtime);
            cleanup_runtime_layout(layout);
            release_database_lock(lock_fd);
            set_error(db, scheduler_result, "database ownerless checkpoint scheduler failed");
            return scheduler_result;
        }
        return MYLITE_OK;
    } catch (...) {
        clear_runtime_state(g_runtime);
        if (server_initialized) {
            mysql_server_end();
        }
        if (concurrency_mapped) {
            unmap_concurrency_shared_memory_for_runtime(g_runtime);
        }
        cleanup_runtime_layout(layout);
        release_database_lock(lock_fd);
        throw;
    }
}

int connect_runtime(mylite_db &db) {
    EmbeddedOpenPerfScope connect_scope(EMBEDDED_OPEN_PERF_CONNECT_TOTAL_NS);
    embedded_open_perf_add(EMBEDDED_OPEN_PERF_CONNECT_CALLS, 1U);
    std::uint64_t stage_start_ns = embedded_open_perf_start_ns();
    if (mysql_init(&db.mysql) == nullptr) {
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_CONNECT_MYSQL_INIT_NS, stage_start_ns);
        set_error(db, MYLITE_NOMEM, "MariaDB connection allocation failed");
        return MYLITE_NOMEM;
    }
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_CONNECT_MYSQL_INIT_NS, stage_start_ns);

    stage_start_ns = embedded_open_perf_start_ns();
    const MYSQL *connection =
        mysql_real_connect(&db.mysql, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0);
    embedded_open_perf_add_elapsed(
        EMBEDDED_OPEN_PERF_CONNECT_MYSQL_REAL_CONNECT_NS,
        stage_start_ns
    );
    if (connection == nullptr) {
        set_mariadb_error(db);
        return MYLITE_ERROR;
    }

    db.connected = true;
    return MYLITE_OK;
}

int ensure_core_system_tables(mylite_db &db) {
    EmbeddedOpenPerfScope system_tables_scope(EMBEDDED_OPEN_PERF_SYSTEM_TABLES_TOTAL_NS);
    embedded_open_perf_add(EMBEDDED_OPEN_PERF_SYSTEM_TABLES_CALLS, 1U);
    if (is_memory_database_path(db.database_path)) {
        const std::uint64_t stage_start_ns = embedded_open_perf_start_ns();
        embedded_open_perf_add(EMBEDDED_OPEN_PERF_SYSTEM_TABLES_EXECUTIONS, 1U);
        const int result = execute_core_system_table_statements(db);
        embedded_open_perf_add_elapsed(
            EMBEDDED_OPEN_PERF_SYSTEM_TABLES_STATEMENTS_NS,
            stage_start_ns
        );
        return result;
    }
    if (db.readonly_open) {
        return MYLITE_OK;
    }

    if (g_runtime.core_system_tables_ready.load(std::memory_order_acquire)) {
        return MYLITE_OK;
    }

    const std::lock_guard<std::mutex> guard(g_system_table_mutex);
    if (g_runtime.core_system_tables_ready.load(std::memory_order_acquire)) {
        return MYLITE_OK;
    }

    std::uint64_t stage_start_ns = embedded_open_perf_start_ns();
    int lock_fd = -1;
    if (g_runtime.ownerless_rw_mode) {
        const std::filesystem::path lock_path = std::filesystem::path(db.database_path) /
                                                k_concurrency_dir_name /
                                                k_concurrency_lock_filename;
        lock_fd = acquire_concurrency_lock(
            lock_path,
            k_system_tables_lock_start,
            k_system_tables_lock_length,
            F_WRLCK,
            k_system_tables_lock_wait_timeout_ms
        );
    }
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_SYSTEM_TABLES_LOCK_NS, stage_start_ns);
    if (g_runtime.ownerless_rw_mode && lock_fd < 0) {
        set_error(db, MYLITE_BUSY, "database system table initialization is busy");
        return MYLITE_BUSY;
    }

    stage_start_ns = embedded_open_perf_start_ns();
    embedded_open_perf_add(EMBEDDED_OPEN_PERF_SYSTEM_TABLES_EXECUTIONS, 1U);
    const int result = execute_core_system_table_statements(db);
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_SYSTEM_TABLES_STATEMENTS_NS, stage_start_ns);
    if (lock_fd >= 0) {
        release_concurrency_lock(lock_fd, k_system_tables_lock_start, k_system_tables_lock_length);
    }
    if (result == MYLITE_OK) {
        g_runtime.core_system_tables_ready.store(true, std::memory_order_release);
    }
    return result;
}

int execute_core_system_table_statements(mylite_db &db) {
    int result = execute_system_table_statement(db, k_create_mysql_database_sql);
    if (result != MYLITE_OK) {
        return result;
    }

    result = execute_system_table_statement(db, k_create_proc_table_sql);
    if (result != MYLITE_OK) {
        return result;
    }

    return execute_system_table_statement(db, k_create_procs_priv_table_sql);
}

int execute_system_table_statement(mylite_db &db, const char *sql) {
    if (mysql_query(&db.mysql, sql) != 0) {
        set_mariadb_error(db);
        return MYLITE_ERROR;
    }
    if (drain_remaining_query_results(db) != MYLITE_OK) {
        return MYLITE_ERROR;
    }

    set_ok(db);
    return MYLITE_OK;
}
#endif

void close_connection(mylite_db &db) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (db.connected) {
        mysql_close(&db.mysql);
        db.connected = false;
    }
#else
    (void)db;
#endif
}

void release_runtime(void) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    EmbeddedOpenPerfScope release_scope(EMBEDDED_OPEN_PERF_RELEASE_RUNTIME_TOTAL_NS);
    embedded_open_perf_add(EMBEDDED_OPEN_PERF_RELEASE_RUNTIME_CALLS, 1U);
#endif
    std::unique_lock<std::mutex> lock(g_runtime.mutex);
    if (g_runtime.ref_count == 0U) {
        return;
    }

    --g_runtime.ref_count;
    if (g_runtime.ref_count > 0U) {
        return;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    std::uint64_t stage_start_ns = embedded_open_perf_start_ns();
    stop_ownerless_checkpoint_scheduler(g_runtime, lock);
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_STOP_SCHEDULER_NS, stage_start_ns);

    const bool ownerless_concurrency_runtime_mapped =
        g_runtime.concurrency_shm_fd >= 0 || g_runtime.concurrency_wal_fd >= 0 ||
        g_runtime.concurrency_checkpoint_fd >= 0 || g_runtime.concurrency_shm_mapping != nullptr;
    int startup_lock_fd = -1;
    OwnerlessRedoStartupPrefixSnapshot shutdown_redo_prefix = {};
    bool no_live_ownerless_shutdown = false;
    const bool redo_shutdown_repair_candidate = ownerless_concurrency_runtime_mapped &&
                                                !g_runtime.readonly_mode &&
                                                !is_memory_database_path(g_runtime.database_path);
    if (g_runtime.ownerless_rw_mode && redo_shutdown_repair_candidate) {
        const std::filesystem::path startup_lock_path =
            std::filesystem::path(g_runtime.database_path) / k_concurrency_dir_name /
            k_concurrency_startup_lock_filename;
        stage_start_ns = embedded_open_perf_start_ns();
        startup_lock_fd = acquire_concurrency_lock(
            startup_lock_path,
            k_ownerless_runtime_startup_lock_start,
            k_ownerless_runtime_startup_lock_length,
            F_WRLCK,
            k_system_tables_lock_wait_timeout_ms
        );
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_STARTUP_LOCK_NS, stage_start_ns);
    }
    no_live_ownerless_shutdown =
        startup_lock_fd >= 0 && ownerless_runtime_has_no_live_peers(g_runtime);
    if (g_runtime.ownerless_rw_mode && !no_live_ownerless_shutdown) {
        static_cast<void>(mylite_ownerless_innodb_refresh_to_latest_external_lsn());
        std::uint64_t latest_lsn = 0;
        if (mylite_ownerless_innodb_redo_observe(&latest_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK &&
            latest_lsn != 0U) {
            const std::uint64_t flush_lsn =
                std::max(latest_lsn, mylite_ownerless_innodb_current_lsn());
            mylite_ownerless_innodb_flush_dirty_pages_for_page_writes(flush_lsn);
        }
    }
    if (ownerless_concurrency_runtime_mapped) {
        stage_start_ns = embedded_open_perf_start_ns();
        reclaim_ownerless_page_log_after_native_checkpoint(g_runtime);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_RECLAIM_NS, stage_start_ns);
    }
    if (redo_shutdown_repair_candidate) {
        const std::filesystem::path redo_path = std::filesystem::path(g_runtime.database_path) /
                                                k_datadir_name / k_innodb_redo_log_filename;
        struct stat redo_stat = {};
        OwnerlessRedoStartupPrefixSnapshot startup_redo_prefix = {};
        bool have_startup_redo_prefix = false;
        stage_start_ns = embedded_open_perf_start_ns();
        const int redo_prefix_result = capture_ownerless_redo_startup_prefix(
            g_runtime.database_path,
            shutdown_redo_prefix,
            false,
            false
        );
        if (startup_lock_fd >= 0 && redo_prefix_result == MYLITE_OK &&
            ::stat(redo_path.string().c_str(), &redo_stat) == 0) {
            have_startup_redo_prefix = read_ownerless_redo_header_backup(
                g_runtime.database_path,
                redo_path,
                redo_stat.st_size,
                startup_redo_prefix
            );
        }
        if (have_startup_redo_prefix && !shutdown_redo_prefix.captured) {
            shutdown_redo_prefix = startup_redo_prefix;
        }
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_REDO_CAPTURE_NS, stage_start_ns);
    }
    stage_start_ns = embedded_open_perf_start_ns();
    if (ownerless_concurrency_runtime_mapped) {
        reset_ownerless_native_shutdown_hooks(g_runtime);
        mylite_ownerless_innodb_close_current_read_view();
        mylite_ownerless_innodb_evict_dictionary_cache();
        mylite_ownerless_innodb_evict_clean_external_pages();
    } else {
        reset_ownerless_runtime_hooks(g_runtime);
    }
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_RESET_HOOKS_NS, stage_start_ns);
    const std::uint64_t mysql_shutdown_start_ns = embedded_open_perf_start_ns();
    stage_start_ns = mysql_shutdown_start_ns;
    mysql_thread_end();
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_MYSQL_THREAD_END_NS, stage_start_ns);
    stage_start_ns = embedded_open_perf_start_ns();
    mysql_server_end();
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_MYSQL_SERVER_END_NS, stage_start_ns);
    embedded_open_perf_add_elapsed(
        EMBEDDED_OPEN_PERF_RELEASE_MYSQL_SHUTDOWN_NS,
        mysql_shutdown_start_ns
    );
    if (ownerless_concurrency_runtime_mapped) {
        clear_ownerless_native_hook_contexts(g_runtime);
    }
    const bool retained_ownerless_page_log =
        g_runtime.ownerless_rw_mode && ownerless_page_log_has_uncheckpointed_records(g_runtime);
    const bool restore_shutdown_redo_prefix =
        redo_shutdown_repair_candidate && !retained_ownerless_page_log &&
        (!g_runtime.ownerless_rw_mode || (startup_lock_fd >= 0 && no_live_ownerless_shutdown));
    if (restore_shutdown_redo_prefix) {
        stage_start_ns = embedded_open_perf_start_ns();
        const bool restored =
            restore_ownerless_redo_shutdown_header_if_needed(shutdown_redo_prefix);
        static_cast<void>(restored);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_REDO_RESTORE_NS, stage_start_ns);
    }
    if (startup_lock_fd >= 0) {
        release_concurrency_lock(
            startup_lock_fd,
            k_ownerless_runtime_startup_lock_start,
            k_ownerless_runtime_startup_lock_length
        );
    }
    if (ownerless_concurrency_runtime_mapped) {
        stage_start_ns = embedded_open_perf_start_ns();
        unmap_concurrency_shared_memory_for_runtime(g_runtime);
        embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_UNMAP_NS, stage_start_ns);
    }
#endif
#if MYLITE_WITH_MARIADB_EMBEDDED
    stage_start_ns = embedded_open_perf_start_ns();
#endif
    cleanup_runtime_state(g_runtime);
#if MYLITE_WITH_MARIADB_EMBEDDED
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_CLEANUP_NS, stage_start_ns);
#endif
#if MYLITE_WITH_MARIADB_EMBEDDED
    stage_start_ns = embedded_open_perf_start_ns();
    release_database_lock(g_runtime.lock_fd);
    embedded_open_perf_add_elapsed(EMBEDDED_OPEN_PERF_RELEASE_DATABASE_LOCK_NS, stage_start_ns);
    g_runtime.lock_fd = -1;
#endif

    clear_runtime_state(g_runtime);
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void cleanup_runtime_layout(const RuntimeLayout &layout) {
    remove_directory_if_present(layout.cleanup_tmp_directory);
    remove_directory_if_present(layout.cleanup_directory);
    remove_directory_if_empty(layout.runtime_parent_directory);
}
#endif

void cleanup_runtime_state(RuntimeState &runtime) {
    remove_directory_if_present(runtime.cleanup_tmp_directory);
    remove_directory_if_present(runtime.cleanup_directory);
    remove_directory_if_empty(runtime.runtime_parent_directory);
}

void clear_runtime_state(RuntimeState &runtime) {
    runtime.cleanup_directory.clear();
    runtime.cleanup_tmp_directory.clear();
    runtime.runtime_parent_directory.clear();
    runtime.database_path.clear();
    runtime.argv.clear();
    runtime.arguments.clear();
    runtime.ownerless_rw_mode = false;
    runtime.readonly_mode = false;
#if MYLITE_WITH_MARIADB_EMBEDDED
    runtime.ownerless_checkpoint_scheduler_stop = false;
    runtime.ownerless_runtime_has_local_write = false;
    runtime.ownerless_runtime_started_with_page_version_wal = false;
    runtime.ownerless_runtime_consumed_page_version_wal.store(false, std::memory_order_relaxed);
    runtime.ownerless_runtime_consumed_current_page_version_wal.store(
        false,
        std::memory_order_relaxed
    );
    runtime.ownerless_runtime_consumed_external_snapshot_page_version_wal.store(
        false,
        std::memory_order_relaxed
    );
    runtime.ownerless_active_statement_count = 0;
    runtime.ownerless_active_explicit_transaction_count = 0;
    runtime.ownerless_last_statement_reclaim_attempt = {};
    runtime.ownerless_last_statement_activity = {};
    runtime.core_system_tables_ready.store(false, std::memory_order_release);
#endif
    runtime.durability = MYLITE_DURABILITY_FULL;
}

void remove_directory_if_empty(const std::filesystem::path &directory) {
    if (directory.empty()) {
        return;
    }

    std::error_code error;
    static_cast<void>(std::filesystem::remove(directory, error));
}

#if MYLITE_WITH_MARIADB_EMBEDDED
std::filesystem::path normalize_database_path(const char *path) {
    if (std::strcmp(path, k_memory_database_path) == 0) {
        return std::filesystem::path(k_memory_database_path);
    }
    return std::filesystem::absolute(std::filesystem::path(path));
}

bool is_memory_database_path(const std::filesystem::path &database_path) {
    return database_path == std::filesystem::path(k_memory_database_path);
}

void initialize_database_layout(const std::filesystem::path &database_path) {
    create_layout_directory(database_path / k_datadir_name, "create database data directory");
    create_layout_directory(database_path / k_tmpdir_name, "create database temporary directory");

    const std::filesystem::path metadata_path = database_path / k_meta_filename;
    std::error_code error;
    if (std::filesystem::exists(metadata_path, error)) {
        if (error || !std::filesystem::is_regular_file(metadata_path, error) || error) {
            throw std::filesystem::filesystem_error(
                "validate database metadata",
                metadata_path,
                error ? error : std::make_error_code(std::errc::invalid_argument)
            );
        }
        return;
    }
    if (error) {
        throw std::filesystem::filesystem_error("validate database metadata", metadata_path, error);
    }

    write_database_metadata(metadata_path);
}

void create_layout_directory(const std::filesystem::path &directory, const char *message) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(message, directory, error);
    }
}

void write_database_metadata(const std::filesystem::path &metadata_path) {
    std::ofstream metadata(metadata_path, std::ios::binary | std::ios::trunc);
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "create database metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }

    metadata << k_metadata_format_line << "\n";
    metadata << "mariadb_base=" << k_mariadb_base_ref << "\n";
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "write database metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }
}

void write_concurrency_metadata(const std::filesystem::path &metadata_path) {
    std::ofstream metadata(metadata_path, std::ios::binary | std::ios::trunc);
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "create concurrency metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }

    metadata << k_metadata_format_line << "\n";
    metadata << "mariadb_base=" << k_mariadb_base_ref << "\n";
    metadata << "database_uuid=" << generate_database_uuid() << "\n";
    metadata << "concurrency_generation=0\n";
    metadata << k_concurrency_mode_line << "\n";
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "write concurrency metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }
}

std::string generate_database_uuid(void) {
    constexpr char k_hex_digits[] = "0123456789abcdef";
    std::array<unsigned char, 16> bytes = {};
    fill_database_uuid_bytes(bytes);
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3FU) | 0x80U);

    std::string uuid;
    uuid.reserve(36U);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            uuid.push_back('-');
        }
        uuid.push_back(k_hex_digits[bytes[index] >> 4U]);
        uuid.push_back(k_hex_digits[bytes[index] & 0x0FU]);
    }
    return uuid;
}

void fill_database_uuid_bytes(std::array<unsigned char, 16> &bytes) {
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (random.read(
            reinterpret_cast<char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        )) {
        return;
    }
    fill_database_uuid_bytes_from_fallback(bytes);
}

void fill_database_uuid_bytes_from_fallback(std::array<unsigned char, 16> &bytes) {
    std::uint64_t state = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    state ^= static_cast<std::uint64_t>(::getpid()) << 32U;
    state ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&bytes));

    for (unsigned char &byte : bytes) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        byte = static_cast<unsigned char>(state & 0xFFU);
    }
}

bool is_database_uuid(std::string_view value) {
    if (value.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') {
                return false;
            }
            continue;
        }
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool is_unsigned_decimal(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

RuntimeLayout create_runtime_layout(
    const std::filesystem::path &database_path,
    const mylite_open_config *config,
    bool allow_stale_cleanup
) {
    if (is_memory_database_path(database_path)) {
        return create_memory_runtime_layout(config);
    }
    return create_persistent_runtime_layout(database_path, allow_stale_cleanup);
}

RuntimeLayout create_memory_runtime_layout(const mylite_open_config *config) {
    const std::filesystem::path root = runtime_root(config);
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        throw std::filesystem::filesystem_error("create runtime root", root, error);
    }

    for (int attempt = 0; attempt < k_runtime_directory_attempts; ++attempt) {
        const std::filesystem::path candidate = root / unique_runtime_name();
        if (std::filesystem::create_directory(candidate, error)) {
            RuntimeLayout layout = {};
            layout.cleanup_directory = candidate;
            layout.data_directory = candidate / "data";
            layout.tmp_directory = candidate / "tmp";
            layout.plugin_directory = candidate / "plugins";
            create_runtime_subdirectory(layout.data_directory, "create runtime data directory");
            create_runtime_subdirectory(layout.tmp_directory, "create runtime temporary directory");
            create_runtime_subdirectory(layout.plugin_directory, "create runtime plugin directory");
            return layout;
        }
        if (error) {
            throw std::filesystem::filesystem_error("create runtime directory", candidate, error);
        }
    }

    throw std::filesystem::filesystem_error(
        "create runtime directory",
        root,
        std::make_error_code(std::errc::file_exists)
    );
}

RuntimeLayout create_persistent_runtime_layout(
    const std::filesystem::path &database_path,
    bool allow_stale_cleanup
) {
    const std::filesystem::path run_root = database_path / k_rundir_name;
    const std::filesystem::path tmp_root = database_path / k_tmpdir_name;

    if (allow_stale_cleanup) {
        remove_directory_if_present(run_root);
        remove_directory_contents_if_present(tmp_root);
    }
    create_runtime_subdirectory(run_root, "create database runtime root directory");
    create_runtime_subdirectory(tmp_root, "create database temporary root directory");

    std::error_code error;
    for (int attempt = 0; attempt < k_runtime_directory_attempts; ++attempt) {
        const std::filesystem::path runtime_name = unique_runtime_name();
        const std::filesystem::path run_candidate = run_root / runtime_name;
        if (!std::filesystem::create_directory(run_candidate, error)) {
            if (error) {
                throw std::filesystem::filesystem_error(
                    "create database runtime directory",
                    run_candidate,
                    error
                );
            }
            continue;
        }

        RuntimeLayout layout = {};
        layout.cleanup_directory = run_candidate;
        layout.cleanup_tmp_directory = tmp_root / runtime_name;
        layout.runtime_parent_directory = run_root;
        layout.data_directory = database_path / k_datadir_name;
        layout.tmp_directory = layout.cleanup_tmp_directory;
        layout.plugin_directory = layout.cleanup_directory / k_plugin_directory_name;
        try {
            create_runtime_subdirectory(
                layout.tmp_directory,
                "create database temporary directory"
            );
            create_runtime_subdirectory(
                layout.plugin_directory,
                "create database plugin directory"
            );
        } catch (...) {
            remove_directory_if_present(layout.cleanup_tmp_directory);
            remove_directory_if_present(layout.cleanup_directory);
            throw;
        }
        return layout;
    }

    throw std::filesystem::filesystem_error(
        "create database runtime directory",
        run_root,
        std::make_error_code(std::errc::file_exists)
    );
}

int acquire_database_lock(
    mylite_db &db,
    const std::filesystem::path &database_path,
    const mylite_open_config *config
) {
    const std::filesystem::path lock_path = database_path / k_lock_filename;
    const std::string lock_name = lock_path.string();
    const int lock_fd = ::open(lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        set_error(db, MYLITE_IOERR, "database lock file could not be opened");
        return -1;
    }

    DatabaseLockWait wait = {};
    wait.lock_fd = lock_fd;
    wait.busy_timeout_ms = configured_busy_timeout_ms(config);
    const int lock_result = wait_for_database_lock(wait);
    if (lock_result == MYLITE_OK) {
        return lock_fd;
    }

    release_database_lock(lock_fd);
    if (lock_result == MYLITE_BUSY) {
        set_error(db, MYLITE_BUSY, "database directory is locked by another process");
    } else {
        set_error(db, MYLITE_IOERR, "database lock could not be acquired");
    }
    return -1;
}

int wait_for_database_lock(DatabaseLockWait wait) {
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(wait.busy_timeout_ms);
    unsigned poll_interval_ms = k_lock_poll_initial_interval_ms;
    for (;;) {
        if (::flock(wait.lock_fd, LOCK_EX | LOCK_NB) == 0) {
            return MYLITE_OK;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            return MYLITE_IOERR;
        }
        if (wait.busy_timeout_ms == 0U || std::chrono::steady_clock::now() - start >= timeout) {
            return MYLITE_BUSY;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
        poll_interval_ms = std::min(poll_interval_ms * 2U, k_lock_poll_max_interval_ms);
    }
}

void release_database_lock(int lock_fd) {
    if (lock_fd >= 0) {
        static_cast<void>(::flock(lock_fd, LOCK_UN));
        static_cast<void>(::close(lock_fd));
    }
}

unsigned configured_busy_timeout_ms(const mylite_open_config *config) {
    if (has_config_field(
            config,
            offsetof(mylite_open_config, busy_timeout_ms) + sizeof(config->busy_timeout_ms)
        )) {
        return config->busy_timeout_ms;
    }
    return 0U;
}

bool unsafe_disable_database_lock_for_tests(void) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    const char *value = std::getenv("MYLITE_UNSAFE_DISABLE_DIRECTORY_LOCK_FOR_TESTS");
    return value != nullptr && std::strcmp(value, "1") == 0;
#  else
    return false;
#  endif
}

void pause_for_ownerless_test_fault(const char *fault_name) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    const char *configured_fault = std::getenv("MYLITE_OWNERLESS_TEST_FAULT");
    if (configured_fault == nullptr || std::strcmp(configured_fault, fault_name) != 0) {
        return;
    }

    const char *ready_fd_value = std::getenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD");
    if (ready_fd_value != nullptr) {
        char *end = nullptr;
        const long ready_fd = std::strtol(ready_fd_value, &end, k_decimal_base);
        if (end != ready_fd_value && *end == '\0' && ready_fd >= 0 &&
            ready_fd <= std::numeric_limits<int>::max()) {
            const char value = 'x';
            static_cast<void>(::write(static_cast<int>(ready_fd), &value, sizeof(value)));
            static_cast<void>(::close(static_cast<int>(ready_fd)));
        }
    }

    const char *release_fd_value = std::getenv("MYLITE_OWNERLESS_TEST_FAULT_RELEASE_FD");
    if (release_fd_value != nullptr) {
        char *end = nullptr;
        const long release_fd = std::strtol(release_fd_value, &end, k_decimal_base);
        if (end != release_fd_value && *end == '\0' && release_fd >= 0 &&
            release_fd <= std::numeric_limits<int>::max()) {
            char value = '\0';
            ssize_t bytes_read = -1;
            do {
                bytes_read = ::read(static_cast<int>(release_fd), &value, sizeof(value));
            } while (bytes_read < 0 && errno == EINTR);
            static_cast<void>(::close(static_cast<int>(release_fd)));
            if (bytes_read == sizeof(value)) {
                return;
            }
        }
    }

    for (;;) {
        ::pause();
    }
#  else
    (void)fault_name;
#  endif
}

std::filesystem::path runtime_root(const mylite_open_config *config) {
    if (config != nullptr &&
        has_config_field(
            config,
            offsetof(mylite_open_config, temp_directory) + sizeof(config->temp_directory)
        ) &&
        config->temp_directory != nullptr && config->temp_directory[0] != '\0') {
        return std::filesystem::path(config->temp_directory);
    }
    return std::filesystem::temp_directory_path();
}

std::string unique_runtime_name(void) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    static unsigned counter = 0;
    return "mylite-runtime-" + std::to_string(now) + "-" + std::to_string(++counter);
}

std::string innodb_temp_data_file_path_argument(
    const RuntimeLayout &layout,
    bool ownerless_runtime_open
) {
    if (!ownerless_runtime_open) {
        return k_innodb_temp_data_file_path;
    }

    const std::filesystem::path temp_file =
        layout.tmp_directory / k_innodb_temp_tablespace_filename;
    const std::filesystem::path relative_temp_file =
        temp_file.lexically_relative(layout.data_directory);
    if (relative_temp_file.empty()) {
        return k_innodb_temp_data_file_path;
    }
    return relative_temp_file.generic_string() + ":12M:autoextend";
}

int configured_durability(const mylite_open_config *config) {
    if (has_config_field(
            config,
            offsetof(mylite_open_config, durability) + sizeof(config->durability)
        )) {
        return config->durability;
    }
    return MYLITE_DURABILITY_FULL;
}

const char *innodb_flush_log_at_trx_commit_option(int durability) {
    switch (durability) {
    case MYLITE_DURABILITY_OFF:
        return "0";
    case MYLITE_DURABILITY_NORMAL:
        return "2";
    case MYLITE_DURABILITY_FULL:
        return "1";
    default:
        return "1";
    }
}

void create_runtime_subdirectory(const std::filesystem::path &directory, const char *message) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(message, directory, error);
    }
}

std::vector<std::string> runtime_arguments(
    const RuntimeLayout &layout,
    bool ownerless_runtime_open,
    bool readonly_open,
    int durability
) {
    std::vector<std::string> arguments = {
        "mylite",
        "--no-defaults",
        "--datadir=" + layout.data_directory.string(),
        "--tmpdir=" + layout.tmp_directory.string(),
        "--plugin-dir=" + layout.plugin_directory.string(),
        "--aria-log-dir-path=" + layout.data_directory.string(),
        "--innodb-data-home-dir=" + layout.data_directory.string(),
        "--innodb-log-group-home-dir=" + layout.data_directory.string(),
        "--innodb-undo-directory=" + layout.data_directory.string(),
        "--innodb-tmpdir=" + layout.tmp_directory.string(),
        "--innodb-temp-data-file-path=" +
            innodb_temp_data_file_path_argument(layout, ownerless_runtime_open),
        std::string("--innodb-flush-log-at-trx-commit=") +
            innodb_flush_log_at_trx_commit_option(durability),
        std::string("--innodb-fast-shutdown=") +
            (ownerless_runtime_open && !readonly_open ? "2" : "1"),
        "--innodb-buffer-pool-dump-at-shutdown=OFF",
        "--innodb-buffer-pool-load-at-startup=OFF",
        "--log-output=NONE",
        "--max-digest-length=0",
        "--use-stat-tables=never",
        "--histogram-size=0",
        "--skip-log-bin",
        "--skip-slave-start",
        "--skip-grant-tables",
        "--skip-networking",
#  ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
        "--performance-schema=OFF",
#  endif
        std::string("--lc-messages-dir=") + MYLITE_MARIADB_MESSAGES_DIR,
        std::string("--character-sets-dir=") + MYLITE_MARIADB_CHARSETS_DIR,
    };
    if (readonly_open) {
        arguments.emplace_back("--read-only=ON");
    }
    if (ownerless_runtime_open) {
        arguments.emplace_back("--mylite-ownerless-managed-file-locks");
    } else {
        arguments.emplace_back("--skip-mylite-ownerless-managed-file-locks");
    }
    return arguments;
}

std::vector<char *> mutable_arguments(std::vector<std::string> &arguments) {
    std::vector<char *> argv;
    argv.reserve(arguments.size());
    std::transform(
        arguments.begin(),
        arguments.end(),
        std::back_inserter(argv),
        [](std::string &argument) { return argument.data(); }
    );
    return argv;
}
#endif

void remove_directory_if_present(const std::filesystem::path &directory) {
    if (directory.empty()) {
        return;
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void remove_directory_contents_if_present(const std::filesystem::path &directory) {
    if (directory.empty()) {
        return;
    }

    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return;
    }

    std::filesystem::directory_iterator entry(directory, error);
    const std::filesystem::directory_iterator end;
    for (; entry != end && !error; entry.increment(error)) {
        std::error_code ignored;
        std::filesystem::remove_all(entry->path(), ignored);
    }
}
#endif

int copy_error_message(mylite_db &db, char **errmsg) {
    if (errmsg == nullptr) {
        return db.errcode;
    }

    const std::size_t length = db.errmsg.size();
    char *copy = static_cast<char *>(std::malloc(length + 1U));
    if (copy == nullptr) {
        return MYLITE_NOMEM;
    }

    std::memcpy(copy, db.errmsg.c_str(), length + 1U);
    *errmsg = copy;
    return db.errcode;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void set_ok(mylite_db &db) {
    db.errcode = MYLITE_OK;
    db.extended_errcode = MYLITE_OK;
    db.mariadb_errno = 0;
    db.sqlstate = k_sqlstate_ok;
    db.errmsg = k_not_an_error;
}
#endif

void set_error(mylite_db &db, int code, const char *message) {
    db.errcode = code;
    db.extended_errcode = code;
    db.mariadb_errno = 0;
    db.sqlstate = k_sqlstate_general;
    db.errmsg = message;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &db) {
    db.errcode = MYLITE_ERROR;
    db.extended_errcode = MYLITE_ERROR;
    db.mariadb_errno = mysql_errno(&db.mysql);
    db.sqlstate = mysql_sqlstate(&db.mysql);
    db.errmsg = mysql_error(&db.mysql);
}

void set_mariadb_statement_error(mylite_db &db, MYSQL_STMT *stmt) {
    db.errcode = MYLITE_ERROR;
    db.extended_errcode = MYLITE_ERROR;
    db.mariadb_errno = mysql_stmt_errno(stmt);
    db.sqlstate = mysql_stmt_sqlstate(stmt);
    db.errmsg = mysql_stmt_error(stmt);
}

void set_mariadb_statement_error(mylite_stmt &stmt) {
    set_mariadb_statement_error(*stmt.db, stmt.stmt);
}

int parse_warning_level(const char *level) {
    if (level != nullptr && std::strcmp(level, "Note") == 0) {
        return MYLITE_WARNING_NOTE;
    }
    if (level != nullptr && std::strcmp(level, "Error") == 0) {
        return MYLITE_WARNING_ERROR;
    }
    return MYLITE_WARNING_WARNING;
}
#endif

const char *safe_c_str(const std::string &value) {
    return value.empty() ? "" : value.c_str();
}

bool has_config_field(const mylite_open_config *config, std::size_t field_end) {
    return config != nullptr && config->size >= field_end;
}

} // namespace

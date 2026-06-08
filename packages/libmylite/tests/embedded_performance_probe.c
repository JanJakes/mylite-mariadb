#include <mylite/mylite.h>

#include <errno.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MYLITE_PERF_REMOVE_TREE_MAX_FDS 32
#define MYLITE_PERF_DEFAULT_OPEN_CLOSE_ITERATIONS 5U
#define MYLITE_PERF_DEFAULT_SELECT_ITERATIONS 1000U
#define MYLITE_PERF_DEFAULT_INSERT_ITERATIONS 200U

typedef struct performance_paths {
    char *root;
    char *runtime_root;
    char *database_path;
} performance_paths;

enum page_publish_stat_index {
    PAGE_PUBLISH_STAT_CANDIDATES = 0,
    PAGE_PUBLISH_STAT_PUBLISHED,
    PAGE_PUBLISH_STAT_SKIPPED_UNPUBLISHABLE,
    PAGE_PUBLISH_STAT_SKIPPED_LOCK_ONLY,
    PAGE_PUBLISH_STAT_SKIPPED_NO_SOURCE,
    PAGE_PUBLISH_STAT_SKIPPED_NO_SPACE,
    PAGE_PUBLISH_STAT_SKIPPED_ALLOC,
    PAGE_PUBLISH_STAT_SKIPPED_LSN_MISMATCH,
    PAGE_PUBLISH_STAT_FAILED,
    PAGE_PUBLISH_STAT_TYPE_INDEX,
    PAGE_PUBLISH_STAT_TYPE_UNDO,
    PAGE_PUBLISH_STAT_TYPE_SPACE_METADATA,
    PAGE_PUBLISH_STAT_TYPE_TRX_SYSTEM,
    PAGE_PUBLISH_STAT_TYPE_BLOB,
    PAGE_PUBLISH_STAT_TYPE_OTHER,
    PAGE_PUBLISH_STAT_NATIVE_SUPPORT,
    PAGE_PUBLISH_STAT_SNAPSHOT_BOUNDARY,
    PAGE_PUBLISH_STAT_COUNT
};

enum commit_visibility_stat_index {
    COMMIT_VISIBILITY_STAT_FAST = 0,
    COMMIT_VISIBILITY_STAT_FLUSH,
    COMMIT_VISIBILITY_STAT_FLUSH_RECOVERY_LSN,
    COMMIT_VISIBILITY_STAT_FLUSH_DIRTY_PAGES,
    COMMIT_VISIBILITY_STAT_FLUSH_NO_PAGE_WRITE_TRX,
    COMMIT_VISIBILITY_STAT_FLUSH_DEFERRED_PAGES,
    COMMIT_VISIBILITY_STAT_FLUSH_PUBLISH_FAILED,
    COMMIT_VISIBILITY_STAT_FLUSH_NO_PUBLISHED_PAGES,
    COMMIT_VISIBILITY_STAT_FLUSH_UNPROVEN_STATEMENT,
    COMMIT_VISIBILITY_STAT_LOG_FLUSH_NS,
    COMMIT_VISIBILITY_STAT_TOTAL_NS,
    COMMIT_VISIBILITY_STAT_PUBLISH_TRANSACTION_PAGES_NS,
    COMMIT_VISIBILITY_STAT_PUBLISH_DIRTY_PAGES_NS,
    COMMIT_VISIBILITY_STAT_FLUSH_DIRTY_PAGES_NS,
    COMMIT_VISIBILITY_STAT_PUBLISH_VISIBLE_NS,
    COMMIT_VISIBILITY_STAT_RELEASE_LOCKS_NS,
    COMMIT_VISIBILITY_STAT_COUNT
};

enum database_perf_stat_index {
    DATABASE_PERF_STAT_PAGE_PUBLISH_CALLS = 0,
    DATABASE_PERF_STAT_PAGE_PUBLISH_TOTAL_NS,
    DATABASE_PERF_STAT_PAGE_PUBLISH_BOUNDARY_NS,
    DATABASE_PERF_STAT_PAGE_PUBLISH_APPEND_NS,
    DATABASE_PERF_STAT_PAGE_PUBLISH_INDEX_NS,
    DATABASE_PERF_STAT_PAGES_VISIBLE_CALLS,
    DATABASE_PERF_STAT_PAGES_VISIBLE_TOTAL_NS,
    DATABASE_PERF_STAT_PAGES_VISIBLE_SYNC_NS,
    DATABASE_PERF_STAT_PAGES_VISIBLE_REDO_STATE_NS,
    DATABASE_PERF_STAT_PAGES_VISIBLE_CHECKPOINT_NS,
    DATABASE_PERF_STAT_TABLE_LOCK_ACQUIRE_CALLS,
    DATABASE_PERF_STAT_TABLE_LOCK_ACQUIRE_NS,
    DATABASE_PERF_STAT_TABLE_LOCK_RELEASE_CALLS,
    DATABASE_PERF_STAT_TABLE_LOCK_RELEASE_NS,
    DATABASE_PERF_STAT_RECORD_LOCK_ACQUIRE_CALLS,
    DATABASE_PERF_STAT_RECORD_LOCK_ACQUIRE_NS,
    DATABASE_PERF_STAT_RECORD_LOCK_RELEASE_CALLS,
    DATABASE_PERF_STAT_RECORD_LOCK_RELEASE_NS,
    DATABASE_PERF_STAT_REDO_ENTER_CALLS,
    DATABASE_PERF_STAT_REDO_ENTER_NS,
    DATABASE_PERF_STAT_REDO_OBSERVE_CALLS,
    DATABASE_PERF_STAT_REDO_OBSERVE_NS,
    DATABASE_PERF_STAT_REDO_RESERVE_CALLS,
    DATABASE_PERF_STAT_REDO_RESERVE_NS,
    DATABASE_PERF_STAT_REDO_WRITTEN_CALLS,
    DATABASE_PERF_STAT_REDO_WRITTEN_NS,
    DATABASE_PERF_STAT_REDO_LEAVE_CALLS,
    DATABASE_PERF_STAT_REDO_LEAVE_NS,
    DATABASE_PERF_STAT_PAGE_READ_CALLS,
    DATABASE_PERF_STAT_PAGE_READ_TOTAL_NS,
    DATABASE_PERF_STAT_PAGE_READ_INDEX_NS,
    DATABASE_PERF_STAT_PAGE_READ_INDEX_DIRECT_NS,
    DATABASE_PERF_STAT_PAGE_READ_INDEX_HITS,
    DATABASE_PERF_STAT_PAGE_READ_INDEX_MISSES,
    DATABASE_PERF_STAT_PAGE_READ_INDEX_SCAN_REQUIRED,
    DATABASE_PERF_STAT_PAGE_READ_INDEX_STALE,
    DATABASE_PERF_STAT_PAGE_READ_INDEX_ERRORS,
    DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_CALLS,
    DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_NS,
    DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_FOUND,
    DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_MISSES,
    DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_NEGATIVE_CACHE_HITS,
    DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_NEGATIVE_CACHE_STORES,
    DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_ERRORS,
    DATABASE_PERF_STAT_PREPARED_STEP_CALLS,
    DATABASE_PERF_STAT_PREPARED_STEP_TOTAL_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_PRESSURE_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_RUNTIME_STATEMENT_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_TEMPORARY_TABLE_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_STATEMENT_LOCK_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_REFRESH_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_BIND_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_RESULT_SETUP_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_DICTIONARY_BEGIN_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_SNAPSHOT_PIN_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_MYSQL_EXECUTE_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_POST_STATE_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_DICTIONARY_FINISH_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_AFFECTED_ROWS_NS,
    DATABASE_PERF_STAT_PREPARED_STEP_RECLAIM_NS,
    DATABASE_PERF_STAT_PREPARED_RESET_CALLS,
    DATABASE_PERF_STAT_PREPARED_RESET_TOTAL_NS,
    DATABASE_PERF_STAT_PREPARED_RESET_MYSQL_NS,
    DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_CALLS,
    DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_ALLOWED,
    DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_UNMAPPED,
    DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_ACTIVE_COUNT,
    DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_GENERATION,
    DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_ACTIVE_PINS,
    DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_BASELINE,
    DATABASE_PERF_STAT_COUNT
};

enum page_write_perf_stat_index {
    PAGE_WRITE_PERF_STAT_ENTER_CALLS = 0,
    PAGE_WRITE_PERF_STAT_ENTER_TOTAL_NS,
    PAGE_WRITE_PERF_STAT_ACQUIRE_NS,
    PAGE_WRITE_PERF_STAT_REFRESH_CALLS,
    PAGE_WRITE_PERF_STAT_REFRESH_NS,
    PAGE_WRITE_PERF_STAT_LEAVE_CALLS,
    PAGE_WRITE_PERF_STAT_LEAVE_TOTAL_NS,
    PAGE_WRITE_PERF_STAT_RELEASE_NS,
    PAGE_WRITE_PERF_STAT_PUBLISH_CALLS,
    PAGE_WRITE_PERF_STAT_PUBLISH_TOTAL_NS,
    PAGE_WRITE_PERF_STAT_COUNT
};

enum page_write_refresh_stat_index {
    PAGE_WRITE_REFRESH_STAT_CALLS = 0,
    PAGE_WRITE_REFRESH_STAT_FORCE_CALLS,
    PAGE_WRITE_REFRESH_STAT_CURRENT_VISIBILITY_CALLS,
    PAGE_WRITE_REFRESH_STAT_SKIPPED_UNPUBLISHABLE,
    PAGE_WRITE_REFRESH_STAT_ALLOC_FAILURES,
    PAGE_WRITE_REFRESH_STAT_VISIBILITY_PUSH_CALLS,
    PAGE_WRITE_REFRESH_STAT_VISIBILITY_PUSH_ERRORS,
    PAGE_WRITE_REFRESH_STAT_SPACE_MISSES,
    PAGE_WRITE_REFRESH_STAT_NODE_MISSES,
    PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_CALLS,
    PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_NS,
    PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_HITS,
    PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_MISSES,
    PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_ERRORS,
    PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_IDENTITY_MISMATCH,
    PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_NOT_NEWER,
    PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_CHECKSUM_FAILURES,
    PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_OVERLAYS,
    PAGE_WRITE_REFRESH_STAT_DISK_READ_CALLS,
    PAGE_WRITE_REFRESH_STAT_DISK_READ_NS,
    PAGE_WRITE_REFRESH_STAT_DISK_READ_FAILURES,
    PAGE_WRITE_REFRESH_STAT_DISK_IDENTITY_MISMATCH,
    PAGE_WRITE_REFRESH_STAT_DISK_NOT_NEWER,
    PAGE_WRITE_REFRESH_STAT_DISK_CHECKSUM_FAILURES,
    PAGE_WRITE_REFRESH_STAT_DISK_OVERLAYS,
    PAGE_WRITE_REFRESH_STAT_SPACE_HEADER_REFRESHES,
    PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_HITS,
    PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_MISSES,
    PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_STORES,
    PAGE_WRITE_REFRESH_STAT_COUNT
};

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
    PAGE_LOG_SCAN_PERF_STAT_COUNT
};

enum embedded_open_perf_stat_index {
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
    EMBEDDED_OPEN_PERF_RELEASE_MYSQL_SHUTDOWN_NS,
    EMBEDDED_OPEN_PERF_RELEASE_REDO_RESTORE_NS,
    EMBEDDED_OPEN_PERF_RELEASE_UNMAP_NS,
    EMBEDDED_OPEN_PERF_RELEASE_CLEANUP_NS,
    EMBEDDED_OPEN_PERF_RELEASE_DATABASE_LOCK_NS,
    EMBEDDED_OPEN_PERF_STAT_COUNT
};

void mylite_embedded_open_perf_set_enabled(int enabled);
void mylite_embedded_open_perf_reset(void);
void mylite_embedded_open_perf_read(uint64_t *out_values, size_t value_count);
void mylite_ownerless_innodb_set_page_publish_stats_enabled(int enabled);
void mylite_ownerless_innodb_reset_page_publish_stats(void);
void mylite_ownerless_innodb_read_page_publish_stats(uint64_t *out_values, size_t value_count);
void mylite_ownerless_innodb_set_page_write_perf_stats_enabled(int enabled);
void mylite_ownerless_innodb_reset_page_write_perf_stats(void);
void mylite_ownerless_innodb_read_page_write_perf_stats(uint64_t *out_values, size_t value_count);
void mylite_ownerless_innodb_set_page_write_refresh_stats_enabled(int enabled);
void mylite_ownerless_innodb_reset_page_write_refresh_stats(void);
void mylite_ownerless_innodb_read_page_write_refresh_stats(
    uint64_t *out_values,
    size_t value_count
);
void mylite_ownerless_innodb_set_commit_visibility_stats_enabled(int enabled);
void mylite_ownerless_innodb_reset_commit_visibility_stats(void);
void mylite_ownerless_innodb_read_commit_visibility_stats(uint64_t *out_values, size_t value_count);
void mylite_ownerless_database_set_perf_stats_enabled(int enabled);
void mylite_ownerless_database_reset_perf_stats(void);
void mylite_ownerless_database_read_perf_stats(uint64_t *out_values, size_t value_count);
void mylite_ownerless_page_log_set_append_perf_stats_enabled(int enabled);
void mylite_ownerless_page_log_reset_append_perf_stats(void);
void mylite_ownerless_page_log_read_append_perf_stats(uint64_t *out_values, size_t value_count);
void mylite_ownerless_page_log_set_scan_perf_stats_enabled(int enabled);
void mylite_ownerless_page_log_reset_scan_perf_stats(void);
void mylite_ownerless_page_log_read_scan_perf_stats(uint64_t *out_values, size_t value_count);

static performance_paths make_performance_paths(void);
static char *path_join(const char *directory, const char *name);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *statbuf,
    int typeflag,
    struct FTW *ftwbuf
);
static mylite_open_config open_config(const char *temp_directory, int durability);
static int env_flag(const char *name);
static unsigned env_unsigned(const char *name, unsigned fallback);
static int env_durability(void);
static const char *durability_name(int durability);
static int env_double(const char *name, double *out_value);
static uint64_t monotonic_ns(void);
static double elapsed_seconds(uint64_t start_ns, uint64_t end_ns);
static void emit_ms(const char *name, double seconds, unsigned iterations);
static void emit_rate(const char *name, unsigned iterations, double seconds);
static void emit_page_publish_stats(const char *prefix);
static void emit_commit_visibility_stats(const char *prefix);
static void emit_database_perf_stats(const char *prefix);
static void emit_embedded_open_perf_stats(const char *prefix);
static void emit_page_write_perf_stats(const char *prefix);
static void emit_page_write_refresh_stats(const char *prefix);
static void emit_page_log_append_perf_stats(const char *prefix);
static void emit_page_log_scan_perf_stats(const char *prefix);
static void check_max_ms(const char *env_name, double seconds, unsigned iterations);
static void check_min_rate(const char *env_name, double rate);
static mylite_db *open_database(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config
);
static void close_database(mylite_db *db);
static void exec_ok(mylite_db *db, const char *sql);
static double measure_open_close(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config,
    unsigned iterations
);
static double measure_active_runtime_reconnect(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config,
    unsigned iterations
);
static double measure_direct_select(mylite_db *db, unsigned iterations);
static double measure_prepared_select(mylite_db *db, unsigned iterations);
static double measure_transactional_insert(
    mylite_db *db,
    const char *table_name,
    unsigned rows,
    int reset_page_publish_stats
);
static double measure_autocommit_insert(
    mylite_db *db,
    const char *table_name,
    unsigned rows,
    int reset_page_publish_stats
);

int main(void) {
    performance_paths paths = make_performance_paths();
    const int durability = env_durability();
    mylite_open_config config = open_config(paths.runtime_root, durability);
    const unsigned open_close_iterations = env_unsigned(
        "MYLITE_PERF_OPEN_CLOSE_ITERATIONS",
        MYLITE_PERF_DEFAULT_OPEN_CLOSE_ITERATIONS
    );
    const unsigned select_iterations =
        env_unsigned("MYLITE_PERF_SELECT_ITERATIONS", MYLITE_PERF_DEFAULT_SELECT_ITERATIONS);
    const unsigned insert_iterations =
        env_unsigned("MYLITE_PERF_INSERT_ITERATIONS", MYLITE_PERF_DEFAULT_INSERT_ITERATIONS);
    const int page_publish_stats = env_flag("MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS");
    const unsigned ordinary_flags = MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE;
    const unsigned ownerless_flags =
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE | MYLITE_OPEN_OWNERLESS_RW;
    mylite_db *db;
    uint64_t start_ns;
    uint64_t end_ns;
    double seconds;
    double rate;

    printf("mylite_perf_open_close_iterations=%u\n", open_close_iterations);
    printf("mylite_perf_select_iterations=%u\n", select_iterations);
    printf("mylite_perf_insert_iterations=%u\n", insert_iterations);
    printf("mylite_perf_durability=%s\n", durability_name(durability));
    printf("mylite_perf_ownerless_page_publish_stats=%d\n", page_publish_stats);
    printf("mylite_perf_database_path=%s\n", paths.database_path);

    start_ns = monotonic_ns();
    db = open_database(&paths, ordinary_flags, &config);
    exec_ok(db, "CREATE DATABASE IF NOT EXISTS app");
    exec_ok(db, "CREATE TABLE app.mylite_perf_warmup (id INT PRIMARY KEY) ENGINE=InnoDB");
    close_database(db);
    end_ns = monotonic_ns();
    seconds = elapsed_seconds(start_ns, end_ns);
    emit_ms("mylite_perf_ordinary_cold_create_open_close", seconds, 1U);

    mylite_embedded_open_perf_reset();
    mylite_embedded_open_perf_set_enabled(1);
    seconds = measure_open_close(&paths, ordinary_flags, &config, open_close_iterations);
    mylite_embedded_open_perf_set_enabled(0);
    emit_ms("mylite_perf_ordinary_warm_open_close", seconds, open_close_iterations);
    emit_embedded_open_perf_stats("mylite_perf_ordinary_warm_open_close");
    check_max_ms("MYLITE_PERF_MAX_ORDINARY_WARM_OPEN_CLOSE_MS", seconds, open_close_iterations);

    db = open_database(&paths, ordinary_flags, &config);
    mylite_embedded_open_perf_reset();
    mylite_embedded_open_perf_set_enabled(1);
    seconds =
        measure_active_runtime_reconnect(&paths, ordinary_flags, &config, open_close_iterations);
    mylite_embedded_open_perf_set_enabled(0);
    emit_ms("mylite_perf_ordinary_active_runtime_reconnect", seconds, open_close_iterations);
    emit_embedded_open_perf_stats("mylite_perf_ordinary_active_runtime_reconnect");
    check_max_ms(
        "MYLITE_PERF_MAX_ORDINARY_ACTIVE_RUNTIME_RECONNECT_MS",
        seconds,
        open_close_iterations
    );
    close_database(db);

    mylite_embedded_open_perf_reset();
    mylite_embedded_open_perf_set_enabled(1);
    seconds = measure_open_close(&paths, ownerless_flags, &config, open_close_iterations);
    mylite_embedded_open_perf_set_enabled(0);
    emit_ms("mylite_perf_ownerless_warm_open_close", seconds, open_close_iterations);
    emit_embedded_open_perf_stats("mylite_perf_ownerless_warm_open_close");
    check_max_ms("MYLITE_PERF_MAX_OWNERLESS_WARM_OPEN_CLOSE_MS", seconds, open_close_iterations);

    db = open_database(&paths, ownerless_flags, &config);
    mylite_embedded_open_perf_reset();
    mylite_embedded_open_perf_set_enabled(1);
    seconds =
        measure_active_runtime_reconnect(&paths, ownerless_flags, &config, open_close_iterations);
    mylite_embedded_open_perf_set_enabled(0);
    emit_ms("mylite_perf_ownerless_active_runtime_reconnect", seconds, open_close_iterations);
    emit_embedded_open_perf_stats("mylite_perf_ownerless_active_runtime_reconnect");
    check_max_ms(
        "MYLITE_PERF_MAX_OWNERLESS_ACTIVE_RUNTIME_RECONNECT_MS",
        seconds,
        open_close_iterations
    );
    close_database(db);

    db = open_database(&paths, ordinary_flags, &config);
    seconds = measure_direct_select(db, select_iterations);
    emit_rate("mylite_perf_ordinary_direct_select1", select_iterations, seconds);
    rate = (double)select_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_ORDINARY_DIRECT_SELECT1_OPS", rate);

    seconds = measure_prepared_select(db, select_iterations);
    emit_rate("mylite_perf_ordinary_prepared_select1", select_iterations, seconds);
    rate = (double)select_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_ORDINARY_PREPARED_SELECT1_OPS", rate);

    seconds = measure_transactional_insert(db, "mylite_perf_ordinary_insert", insert_iterations, 0);
    emit_rate("mylite_perf_ordinary_insert_txn", insert_iterations, seconds);
    rate = (double)insert_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_ORDINARY_INSERT_TXN_OPS", rate);

    seconds =
        measure_autocommit_insert(db, "mylite_perf_ordinary_autocommit", insert_iterations, 0);
    emit_rate("mylite_perf_ordinary_insert_autocommit", insert_iterations, seconds);
    rate = (double)insert_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_ORDINARY_AUTOCOMMIT_INSERT_OPS", rate);
    close_database(db);

    db = open_database(&paths, ownerless_flags, &config);
    seconds = measure_direct_select(db, select_iterations);
    emit_rate("mylite_perf_ownerless_direct_select1", select_iterations, seconds);
    rate = (double)select_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_OWNERLESS_DIRECT_SELECT1_OPS", rate);

    seconds = measure_prepared_select(db, select_iterations);
    emit_rate("mylite_perf_ownerless_prepared_select1", select_iterations, seconds);
    rate = (double)select_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_OWNERLESS_PREPARED_SELECT1_OPS", rate);

    if (page_publish_stats) {
        mylite_ownerless_innodb_set_page_publish_stats_enabled(1);
        mylite_ownerless_innodb_set_page_write_perf_stats_enabled(1);
        mylite_ownerless_innodb_set_page_write_refresh_stats_enabled(1);
        mylite_ownerless_innodb_set_commit_visibility_stats_enabled(1);
        mylite_ownerless_database_set_perf_stats_enabled(1);
        mylite_ownerless_page_log_set_append_perf_stats_enabled(1);
        mylite_ownerless_page_log_set_scan_perf_stats_enabled(1);
    }

    seconds = measure_transactional_insert(
        db,
        "mylite_perf_ownerless_insert",
        insert_iterations,
        page_publish_stats
    );
    emit_rate("mylite_perf_ownerless_insert_txn", insert_iterations, seconds);
    if (page_publish_stats) {
        emit_page_publish_stats("mylite_perf_ownerless_insert_txn");
        emit_commit_visibility_stats("mylite_perf_ownerless_insert_txn");
        emit_database_perf_stats("mylite_perf_ownerless_insert_txn");
        emit_page_write_perf_stats("mylite_perf_ownerless_insert_txn");
        emit_page_write_refresh_stats("mylite_perf_ownerless_insert_txn");
        emit_page_log_append_perf_stats("mylite_perf_ownerless_insert_txn");
        emit_page_log_scan_perf_stats("mylite_perf_ownerless_insert_txn");
    }
    rate = (double)insert_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_OWNERLESS_INSERT_TXN_OPS", rate);

    seconds = measure_autocommit_insert(
        db,
        "mylite_perf_ownerless_autocommit",
        insert_iterations,
        page_publish_stats
    );
    emit_rate("mylite_perf_ownerless_insert_autocommit", insert_iterations, seconds);
    if (page_publish_stats) {
        emit_page_publish_stats("mylite_perf_ownerless_insert_autocommit");
        emit_commit_visibility_stats("mylite_perf_ownerless_insert_autocommit");
        emit_database_perf_stats("mylite_perf_ownerless_insert_autocommit");
        emit_page_write_perf_stats("mylite_perf_ownerless_insert_autocommit");
        emit_page_write_refresh_stats("mylite_perf_ownerless_insert_autocommit");
        emit_page_log_append_perf_stats("mylite_perf_ownerless_insert_autocommit");
        emit_page_log_scan_perf_stats("mylite_perf_ownerless_insert_autocommit");
        mylite_ownerless_innodb_set_page_publish_stats_enabled(0);
        mylite_ownerless_innodb_set_page_write_perf_stats_enabled(0);
        mylite_ownerless_innodb_set_page_write_refresh_stats_enabled(0);
        mylite_ownerless_innodb_set_commit_visibility_stats_enabled(0);
        mylite_ownerless_database_set_perf_stats_enabled(0);
        mylite_ownerless_page_log_set_append_perf_stats_enabled(0);
        mylite_ownerless_page_log_set_scan_perf_stats_enabled(0);
    }
    rate = (double)insert_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_OWNERLESS_AUTOCOMMIT_INSERT_OPS", rate);
    close_database(db);

    remove_tree(paths.root);
    free(paths.database_path);
    free(paths.runtime_root);
    free(paths.root);
    return 0;
}

static performance_paths make_performance_paths(void) {
    const char *tmp = getenv("TMPDIR");
    char *template_path;
    performance_paths paths;
    size_t tmp_len;
    const char suffix[] = "/mylite-perf-probe.XXXXXX";

    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    tmp_len = strlen(tmp);
    template_path = (char *)malloc(tmp_len + sizeof(suffix));
    if (template_path == NULL) {
        fprintf(stderr, "failed to allocate performance path\n");
        exit(1);
    }
    memcpy(template_path, tmp, tmp_len);
    memcpy(template_path + tmp_len, suffix, sizeof(suffix));
    if (mkdtemp(template_path) == NULL) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        free(template_path);
        exit(1);
    }

    paths.root = template_path;
    paths.runtime_root = path_join(paths.root, "runtime");
    paths.database_path = path_join(paths.root, "startup-performance.mylite");
    if (mkdir(paths.runtime_root, 0700) != 0) {
        fprintf(stderr, "mkdir %s failed: %s\n", paths.runtime_root, strerror(errno));
        remove_tree(paths.root);
        free(paths.database_path);
        free(paths.runtime_root);
        free(paths.root);
        exit(1);
    }
    return paths;
}

static char *path_join(const char *directory, const char *name) {
    const size_t directory_len = strlen(directory);
    const size_t name_len = strlen(name);
    char *path = (char *)malloc(directory_len + name_len + 2U);
    if (path == NULL) {
        fprintf(stderr, "failed to allocate path\n");
        exit(1);
    }
    memcpy(path, directory, directory_len);
    path[directory_len] = '/';
    memcpy(path + directory_len + 1U, name, name_len + 1U);
    return path;
}

static void remove_tree(const char *path) {
    if (path == NULL) {
        return;
    }
    if (nftw(path, remove_tree_entry, MYLITE_PERF_REMOVE_TREE_MAX_FDS, FTW_DEPTH | FTW_PHYS) != 0) {
        fprintf(stderr, "warning: failed to remove %s: %s\n", path, strerror(errno));
    }
}

static int remove_tree_entry(
    const char *path,
    const struct stat *statbuf,
    int typeflag,
    struct FTW *ftwbuf
) {
    (void)statbuf;
    (void)typeflag;
    (void)ftwbuf;
    if (remove(path) != 0) {
        fprintf(stderr, "warning: remove %s failed: %s\n", path, strerror(errno));
    }
    return 0;
}

static mylite_open_config open_config(const char *temp_directory, int durability) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = durability,
        .temp_directory = temp_directory,
    };
    return config;
}

static int env_flag(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0' || strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 || strcmp(value, "FALSE") == 0 || strcmp(value, "off") == 0 ||
        strcmp(value, "OFF") == 0) {
        return 0;
    }
    return 1;
}

static unsigned env_unsigned(const char *name, unsigned fallback) {
    char *end = NULL;
    const char *value = getenv(name);
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0UL) {
        fprintf(stderr, "%s must be a positive integer\n", name);
        exit(1);
    }
    if (parsed > 10000000UL) {
        fprintf(stderr, "%s is unreasonably large\n", name);
        exit(1);
    }
    return (unsigned)parsed;
}

static int env_durability(void) {
    const char *value = getenv("MYLITE_PERF_DURABILITY");
    if (value == NULL || value[0] == '\0' || strcmp(value, "FULL") == 0 ||
        strcmp(value, "full") == 0 || strcmp(value, "2") == 0) {
        return MYLITE_DURABILITY_FULL;
    }
    if (strcmp(value, "NORMAL") == 0 || strcmp(value, "normal") == 0 || strcmp(value, "1") == 0) {
        return MYLITE_DURABILITY_NORMAL;
    }
    if (strcmp(value, "OFF") == 0 || strcmp(value, "off") == 0 || strcmp(value, "0") == 0) {
        return MYLITE_DURABILITY_OFF;
    }

    fprintf(stderr, "MYLITE_PERF_DURABILITY must be FULL, NORMAL, or OFF\n");
    exit(1);
}

static const char *durability_name(int durability) {
    switch (durability) {
    case MYLITE_DURABILITY_FULL:
        return "FULL";
    case MYLITE_DURABILITY_NORMAL:
        return "NORMAL";
    case MYLITE_DURABILITY_OFF:
        return "OFF";
    default:
        return "UNKNOWN";
    }
}

static int env_double(const char *name, double *out_value) {
    char *end = NULL;
    const char *value = getenv(name);
    double parsed;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0.0) {
        fprintf(stderr, "%s must be a positive number\n", name);
        exit(1);
    }
    *out_value = parsed;
    return 1;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
        exit(1);
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static double elapsed_seconds(uint64_t start_ns, uint64_t end_ns) {
    return (double)(end_ns - start_ns) / 1000000000.0;
}

static void emit_ms(const char *name, double seconds, unsigned iterations) {
    const double average_ms = (seconds * 1000.0) / (double)iterations;
    printf("%s_ms_avg=%.3f\n", name, average_ms);
    printf("%s_seconds_total=%.6f\n", name, seconds);
}

static void emit_rate(const char *name, unsigned iterations, double seconds) {
    const double divisor = seconds > 0.000001 ? seconds : 0.000001;
    printf("%s_iterations=%u\n", name, iterations);
    printf("%s_seconds=%.6f\n", name, seconds);
    printf("%s_ops_per_second=%.2f\n", name, (double)iterations / divisor);
}

static void emit_page_publish_stats(const char *prefix) {
    uint64_t values[PAGE_PUBLISH_STAT_COUNT] = {0};

    mylite_ownerless_innodb_read_page_publish_stats(values, PAGE_PUBLISH_STAT_COUNT);
    printf(
        "%s_page_publish_candidates=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_CANDIDATES]
    );
    printf("%s_page_publish_published=%" PRIu64 "\n", prefix, values[PAGE_PUBLISH_STAT_PUBLISHED]);
    printf(
        "%s_page_publish_skipped_unpublishable=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_SKIPPED_UNPUBLISHABLE]
    );
    printf(
        "%s_page_publish_skipped_lock_only=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_SKIPPED_LOCK_ONLY]
    );
    printf(
        "%s_page_publish_skipped_no_source=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_SKIPPED_NO_SOURCE]
    );
    printf(
        "%s_page_publish_skipped_no_space=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_SKIPPED_NO_SPACE]
    );
    printf(
        "%s_page_publish_skipped_alloc=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_SKIPPED_ALLOC]
    );
    printf(
        "%s_page_publish_skipped_lsn_mismatch=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_SKIPPED_LSN_MISMATCH]
    );
    printf("%s_page_publish_failed=%" PRIu64 "\n", prefix, values[PAGE_PUBLISH_STAT_FAILED]);
    printf(
        "%s_page_publish_type_index=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_TYPE_INDEX]
    );
    printf("%s_page_publish_type_undo=%" PRIu64 "\n", prefix, values[PAGE_PUBLISH_STAT_TYPE_UNDO]);
    printf(
        "%s_page_publish_type_space_metadata=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_TYPE_SPACE_METADATA]
    );
    printf(
        "%s_page_publish_type_trx_system=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_TYPE_TRX_SYSTEM]
    );
    printf("%s_page_publish_type_blob=%" PRIu64 "\n", prefix, values[PAGE_PUBLISH_STAT_TYPE_BLOB]);
    printf(
        "%s_page_publish_type_other=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_TYPE_OTHER]
    );
    printf(
        "%s_page_publish_native_support=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_NATIVE_SUPPORT]
    );
    printf(
        "%s_page_publish_snapshot_boundary=%" PRIu64 "\n",
        prefix,
        values[PAGE_PUBLISH_STAT_SNAPSHOT_BOUNDARY]
    );
}

static void emit_commit_visibility_stats(const char *prefix) {
    uint64_t values[COMMIT_VISIBILITY_STAT_COUNT] = {0};

    mylite_ownerless_innodb_read_commit_visibility_stats(values, COMMIT_VISIBILITY_STAT_COUNT);
    printf("%s_commit_visibility_fast=%" PRIu64 "\n", prefix, values[COMMIT_VISIBILITY_STAT_FAST]);
    printf(
        "%s_commit_visibility_flush=%" PRIu64 "\n",
        prefix,
        values[COMMIT_VISIBILITY_STAT_FLUSH]
    );
    printf(
        "%s_commit_visibility_flush_recovery_lsn=%" PRIu64 "\n",
        prefix,
        values[COMMIT_VISIBILITY_STAT_FLUSH_RECOVERY_LSN]
    );
    printf(
        "%s_commit_visibility_flush_dirty_pages=%" PRIu64 "\n",
        prefix,
        values[COMMIT_VISIBILITY_STAT_FLUSH_DIRTY_PAGES]
    );
    printf(
        "%s_commit_visibility_flush_no_page_write_trx=%" PRIu64 "\n",
        prefix,
        values[COMMIT_VISIBILITY_STAT_FLUSH_NO_PAGE_WRITE_TRX]
    );
    printf(
        "%s_commit_visibility_flush_deferred_pages=%" PRIu64 "\n",
        prefix,
        values[COMMIT_VISIBILITY_STAT_FLUSH_DEFERRED_PAGES]
    );
    printf(
        "%s_commit_visibility_flush_publish_failed=%" PRIu64 "\n",
        prefix,
        values[COMMIT_VISIBILITY_STAT_FLUSH_PUBLISH_FAILED]
    );
    printf(
        "%s_commit_visibility_flush_no_published_pages=%" PRIu64 "\n",
        prefix,
        values[COMMIT_VISIBILITY_STAT_FLUSH_NO_PUBLISHED_PAGES]
    );
    printf(
        "%s_commit_visibility_flush_unproven_statement=%" PRIu64 "\n",
        prefix,
        values[COMMIT_VISIBILITY_STAT_FLUSH_UNPROVEN_STATEMENT]
    );
    printf(
        "%s_commit_visibility_log_flush_ms=%.3f\n",
        prefix,
        (double)values[COMMIT_VISIBILITY_STAT_LOG_FLUSH_NS] / 1000000.0
    );
    printf(
        "%s_commit_visibility_total_ms=%.3f\n",
        prefix,
        (double)values[COMMIT_VISIBILITY_STAT_TOTAL_NS] / 1000000.0
    );
    printf(
        "%s_commit_visibility_publish_transaction_pages_ms=%.3f\n",
        prefix,
        (double)values[COMMIT_VISIBILITY_STAT_PUBLISH_TRANSACTION_PAGES_NS] / 1000000.0
    );
    printf(
        "%s_commit_visibility_publish_dirty_pages_ms=%.3f\n",
        prefix,
        (double)values[COMMIT_VISIBILITY_STAT_PUBLISH_DIRTY_PAGES_NS] / 1000000.0
    );
    printf(
        "%s_commit_visibility_flush_dirty_pages_ms=%.3f\n",
        prefix,
        (double)values[COMMIT_VISIBILITY_STAT_FLUSH_DIRTY_PAGES_NS] / 1000000.0
    );
    printf(
        "%s_commit_visibility_publish_visible_ms=%.3f\n",
        prefix,
        (double)values[COMMIT_VISIBILITY_STAT_PUBLISH_VISIBLE_NS] / 1000000.0
    );
    printf(
        "%s_commit_visibility_release_locks_ms=%.3f\n",
        prefix,
        (double)values[COMMIT_VISIBILITY_STAT_RELEASE_LOCKS_NS] / 1000000.0
    );
}

static void emit_database_perf_stats(const char *prefix) {
    uint64_t values[DATABASE_PERF_STAT_COUNT] = {0};

    mylite_ownerless_database_read_perf_stats(values, DATABASE_PERF_STAT_COUNT);
    printf(
        "%s_page_publish_hook_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_PUBLISH_CALLS]
    );
    printf(
        "%s_page_publish_hook_total_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGE_PUBLISH_TOTAL_NS] / 1000000.0
    );
    printf(
        "%s_page_publish_hook_boundary_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGE_PUBLISH_BOUNDARY_NS] / 1000000.0
    );
    printf(
        "%s_page_publish_hook_append_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGE_PUBLISH_APPEND_NS] / 1000000.0
    );
    printf(
        "%s_page_publish_hook_index_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGE_PUBLISH_INDEX_NS] / 1000000.0
    );
    printf(
        "%s_pages_visible_hook_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGES_VISIBLE_CALLS]
    );
    printf(
        "%s_pages_visible_hook_total_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGES_VISIBLE_TOTAL_NS] / 1000000.0
    );
    printf(
        "%s_pages_visible_hook_sync_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGES_VISIBLE_SYNC_NS] / 1000000.0
    );
    printf(
        "%s_pages_visible_hook_redo_state_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGES_VISIBLE_REDO_STATE_NS] / 1000000.0
    );
    printf(
        "%s_pages_visible_hook_checkpoint_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGES_VISIBLE_CHECKPOINT_NS] / 1000000.0
    );
    printf(
        "%s_table_lock_acquire_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_TABLE_LOCK_ACQUIRE_CALLS]
    );
    printf(
        "%s_table_lock_acquire_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_TABLE_LOCK_ACQUIRE_NS] / 1000000.0
    );
    printf(
        "%s_table_lock_release_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_TABLE_LOCK_RELEASE_CALLS]
    );
    printf(
        "%s_table_lock_release_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_TABLE_LOCK_RELEASE_NS] / 1000000.0
    );
    printf(
        "%s_record_lock_acquire_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_RECORD_LOCK_ACQUIRE_CALLS]
    );
    printf(
        "%s_record_lock_acquire_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_RECORD_LOCK_ACQUIRE_NS] / 1000000.0
    );
    printf(
        "%s_record_lock_release_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_RECORD_LOCK_RELEASE_CALLS]
    );
    printf(
        "%s_record_lock_release_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_RECORD_LOCK_RELEASE_NS] / 1000000.0
    );
    printf(
        "%s_redo_enter_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_REDO_ENTER_CALLS]
    );
    printf(
        "%s_redo_enter_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_REDO_ENTER_NS] / 1000000.0
    );
    printf(
        "%s_redo_observe_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_REDO_OBSERVE_CALLS]
    );
    printf(
        "%s_redo_observe_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_REDO_OBSERVE_NS] / 1000000.0
    );
    printf(
        "%s_redo_reserve_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_REDO_RESERVE_CALLS]
    );
    printf(
        "%s_redo_reserve_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_REDO_RESERVE_NS] / 1000000.0
    );
    printf(
        "%s_redo_written_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_REDO_WRITTEN_CALLS]
    );
    printf(
        "%s_redo_written_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_REDO_WRITTEN_NS] / 1000000.0
    );
    printf(
        "%s_redo_leave_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_REDO_LEAVE_CALLS]
    );
    printf(
        "%s_redo_leave_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_REDO_LEAVE_NS] / 1000000.0
    );
    printf("%s_page_read_calls=%" PRIu64 "\n", prefix, values[DATABASE_PERF_STAT_PAGE_READ_CALLS]);
    printf(
        "%s_page_read_total_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGE_READ_TOTAL_NS] / 1000000.0
    );
    printf(
        "%s_page_read_index_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGE_READ_INDEX_NS] / 1000000.0
    );
    printf(
        "%s_page_read_index_direct_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGE_READ_INDEX_DIRECT_NS] / 1000000.0
    );
    printf(
        "%s_page_read_index_hits=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_INDEX_HITS]
    );
    printf(
        "%s_page_read_index_misses=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_INDEX_MISSES]
    );
    printf(
        "%s_page_read_index_scan_required=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_INDEX_SCAN_REQUIRED]
    );
    printf(
        "%s_page_read_index_stale=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_INDEX_STALE]
    );
    printf(
        "%s_page_read_index_errors=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_INDEX_ERRORS]
    );
    printf(
        "%s_page_read_wal_scan_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_CALLS]
    );
    printf(
        "%s_page_read_wal_scan_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_NS] / 1000000.0
    );
    printf(
        "%s_page_read_wal_scan_found=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_FOUND]
    );
    printf(
        "%s_page_read_wal_scan_misses=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_MISSES]
    );
    printf(
        "%s_page_read_wal_scan_negative_cache_hits=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_NEGATIVE_CACHE_HITS]
    );
    printf(
        "%s_page_read_wal_scan_negative_cache_stores=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_NEGATIVE_CACHE_STORES]
    );
    printf(
        "%s_page_read_wal_scan_errors=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PAGE_READ_WAL_SCAN_ERRORS]
    );
    printf(
        "%s_prepared_step_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PREPARED_STEP_CALLS]
    );
    printf(
        "%s_prepared_step_total_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_TOTAL_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_pressure_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_PRESSURE_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_runtime_statement_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_RUNTIME_STATEMENT_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_temporary_table_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_TEMPORARY_TABLE_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_statement_lock_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_STATEMENT_LOCK_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_refresh_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_REFRESH_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_bind_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_BIND_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_result_setup_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_RESULT_SETUP_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_dictionary_begin_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_DICTIONARY_BEGIN_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_snapshot_pin_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_SNAPSHOT_PIN_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_mysql_execute_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_MYSQL_EXECUTE_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_post_state_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_POST_STATE_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_dictionary_finish_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_DICTIONARY_FINISH_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_affected_rows_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_AFFECTED_ROWS_NS] / 1000000.0
    );
    printf(
        "%s_prepared_step_reclaim_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_STEP_RECLAIM_NS] / 1000000.0
    );
    printf(
        "%s_prepared_reset_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_PREPARED_RESET_CALLS]
    );
    printf(
        "%s_prepared_reset_total_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_RESET_TOTAL_NS] / 1000000.0
    );
    printf(
        "%s_prepared_reset_mysql_ms=%.3f\n",
        prefix,
        (double)values[DATABASE_PERF_STAT_PREPARED_RESET_MYSQL_NS] / 1000000.0
    );
    printf(
        "%s_single_owner_skip_calls=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_CALLS]
    );
    printf(
        "%s_single_owner_skip_allowed=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_ALLOWED]
    );
    printf(
        "%s_single_owner_skip_blocked_unmapped=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_UNMAPPED]
    );
    printf(
        "%s_single_owner_skip_blocked_active_count=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_ACTIVE_COUNT]
    );
    printf(
        "%s_single_owner_skip_blocked_generation=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_GENERATION]
    );
    printf(
        "%s_single_owner_skip_blocked_active_pins=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_ACTIVE_PINS]
    );
    printf(
        "%s_single_owner_skip_blocked_baseline=%" PRIu64 "\n",
        prefix,
        values[DATABASE_PERF_STAT_SINGLE_OWNER_SKIP_BLOCKED_BASELINE]
    );
}

static void emit_embedded_open_perf_value(const char *prefix, const char *name, uint64_t value) {
    printf("%s_open_phase_%s=%" PRIu64 "\n", prefix, name, value);
}

static void emit_embedded_open_perf_ms(
    const char *prefix,
    const char *name,
    uint64_t value_ns,
    uint64_t calls
) {
    const double total_ms = (double)value_ns / 1000000.0;
    const double average_ms = calls > 0U ? total_ms / (double)calls : 0.0;

    printf("%s_open_phase_%s_ms=%.3f\n", prefix, name, total_ms);
    printf("%s_open_phase_%s_ms_avg=%.3f\n", prefix, name, average_ms);
}

static void emit_embedded_open_perf_stats(const char *prefix) {
    uint64_t values[EMBEDDED_OPEN_PERF_STAT_COUNT] = {0};
    uint64_t open_calls;
    uint64_t start_calls;
    uint64_t connect_calls;
    uint64_t system_table_calls;
    uint64_t system_table_executions;
    uint64_t close_calls;
    uint64_t release_calls;

    mylite_embedded_open_perf_read(values, EMBEDDED_OPEN_PERF_STAT_COUNT);

    open_calls = values[EMBEDDED_OPEN_PERF_OPEN_CALLS];
    start_calls = values[EMBEDDED_OPEN_PERF_START_RUNTIME_CALLS];
    connect_calls = values[EMBEDDED_OPEN_PERF_CONNECT_CALLS];
    system_table_calls = values[EMBEDDED_OPEN_PERF_SYSTEM_TABLES_CALLS];
    system_table_executions = values[EMBEDDED_OPEN_PERF_SYSTEM_TABLES_EXECUTIONS];
    close_calls = values[EMBEDDED_OPEN_PERF_CLOSE_CALLS];
    release_calls = values[EMBEDDED_OPEN_PERF_RELEASE_RUNTIME_CALLS];

    emit_embedded_open_perf_value(prefix, "open_calls", open_calls);
    emit_embedded_open_perf_ms(
        prefix,
        "open_total",
        values[EMBEDDED_OPEN_PERF_OPEN_TOTAL_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_validate",
        values[EMBEDDED_OPEN_PERF_OPEN_VALIDATE_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_allocate_normalize",
        values[EMBEDDED_OPEN_PERF_OPEN_ALLOCATE_NORMALIZE_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_runtime_path",
        values[EMBEDDED_OPEN_PERF_OPEN_RUNTIME_PATH_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_prepare_directory",
        values[EMBEDDED_OPEN_PERF_OPEN_PREPARE_DIRECTORY_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_platform_probe",
        values[EMBEDDED_OPEN_PERF_OPEN_PLATFORM_PROBE_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_startup_lock",
        values[EMBEDDED_OPEN_PERF_OPEN_STARTUP_LOCK_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_start_runtime",
        values[EMBEDDED_OPEN_PERF_OPEN_START_RUNTIME_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_connect_runtime",
        values[EMBEDDED_OPEN_PERF_OPEN_CONNECT_RUNTIME_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_system_tables",
        values[EMBEDDED_OPEN_PERF_OPEN_SYSTEM_TABLES_NS],
        open_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "open_dictionary",
        values[EMBEDDED_OPEN_PERF_OPEN_DICTIONARY_NS],
        open_calls
    );

    emit_embedded_open_perf_value(prefix, "start_runtime_calls", start_calls);
    emit_embedded_open_perf_ms(
        prefix,
        "start_runtime_total",
        values[EMBEDDED_OPEN_PERF_START_RUNTIME_TOTAL_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_database_lock",
        values[EMBEDDED_OPEN_PERF_START_DATABASE_LOCK_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_concurrency_metadata",
        values[EMBEDDED_OPEN_PERF_START_CONCURRENCY_METADATA_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_shared_memory_prepare",
        values[EMBEDDED_OPEN_PERF_START_SHARED_MEMORY_PREPARE_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_layout_arguments",
        values[EMBEDDED_OPEN_PERF_START_LAYOUT_ARGUMENTS_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_map_shared_memory",
        values[EMBEDDED_OPEN_PERF_START_MAP_SHARED_MEMORY_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_open_page_log",
        values[EMBEDDED_OPEN_PERF_START_OPEN_PAGE_LOG_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_open_checkpoint",
        values[EMBEDDED_OPEN_PERF_START_OPEN_CHECKPOINT_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_pre_hooks",
        values[EMBEDDED_OPEN_PERF_START_PRE_HOOKS_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_redo_evidence",
        values[EMBEDDED_OPEN_PERF_START_REDO_EVIDENCE_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_bootstrap_lock",
        values[EMBEDDED_OPEN_PERF_START_BOOTSTRAP_LOCK_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_mysql_server_init",
        values[EMBEDDED_OPEN_PERF_START_MYSQL_SERVER_INIT_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_post_hooks",
        values[EMBEDDED_OPEN_PERF_START_POST_HOOKS_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_redo_backup",
        values[EMBEDDED_OPEN_PERF_START_REDO_BACKUP_NS],
        start_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "start_scheduler",
        values[EMBEDDED_OPEN_PERF_START_SCHEDULER_NS],
        start_calls
    );

    emit_embedded_open_perf_value(prefix, "connect_calls", connect_calls);
    emit_embedded_open_perf_ms(
        prefix,
        "connect_total",
        values[EMBEDDED_OPEN_PERF_CONNECT_TOTAL_NS],
        connect_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "connect_mysql_init",
        values[EMBEDDED_OPEN_PERF_CONNECT_MYSQL_INIT_NS],
        connect_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "connect_mysql_real_connect",
        values[EMBEDDED_OPEN_PERF_CONNECT_MYSQL_REAL_CONNECT_NS],
        connect_calls
    );

    emit_embedded_open_perf_value(prefix, "system_table_calls", system_table_calls);
    emit_embedded_open_perf_value(prefix, "system_table_executions", system_table_executions);
    emit_embedded_open_perf_ms(
        prefix,
        "system_tables_total",
        values[EMBEDDED_OPEN_PERF_SYSTEM_TABLES_TOTAL_NS],
        system_table_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "system_tables_lock",
        values[EMBEDDED_OPEN_PERF_SYSTEM_TABLES_LOCK_NS],
        system_table_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "system_tables_statements",
        values[EMBEDDED_OPEN_PERF_SYSTEM_TABLES_STATEMENTS_NS],
        system_table_calls
    );

    emit_embedded_open_perf_value(prefix, "close_calls", close_calls);
    emit_embedded_open_perf_ms(
        prefix,
        "close_total",
        values[EMBEDDED_OPEN_PERF_CLOSE_TOTAL_NS],
        close_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "close_rollback",
        values[EMBEDDED_OPEN_PERF_CLOSE_ROLLBACK_NS],
        close_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "close_connection",
        values[EMBEDDED_OPEN_PERF_CLOSE_CONNECTION_NS],
        close_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "close_release_runtime",
        values[EMBEDDED_OPEN_PERF_CLOSE_RELEASE_RUNTIME_NS],
        close_calls
    );

    emit_embedded_open_perf_value(prefix, "release_runtime_calls", release_calls);
    emit_embedded_open_perf_ms(
        prefix,
        "release_runtime_total",
        values[EMBEDDED_OPEN_PERF_RELEASE_RUNTIME_TOTAL_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_stop_scheduler",
        values[EMBEDDED_OPEN_PERF_RELEASE_STOP_SCHEDULER_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_startup_lock",
        values[EMBEDDED_OPEN_PERF_RELEASE_STARTUP_LOCK_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_reclaim",
        values[EMBEDDED_OPEN_PERF_RELEASE_RECLAIM_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_redo_capture",
        values[EMBEDDED_OPEN_PERF_RELEASE_REDO_CAPTURE_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_reset_hooks",
        values[EMBEDDED_OPEN_PERF_RELEASE_RESET_HOOKS_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_mysql_shutdown",
        values[EMBEDDED_OPEN_PERF_RELEASE_MYSQL_SHUTDOWN_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_redo_restore",
        values[EMBEDDED_OPEN_PERF_RELEASE_REDO_RESTORE_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_unmap",
        values[EMBEDDED_OPEN_PERF_RELEASE_UNMAP_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_cleanup",
        values[EMBEDDED_OPEN_PERF_RELEASE_CLEANUP_NS],
        release_calls
    );
    emit_embedded_open_perf_ms(
        prefix,
        "release_database_lock",
        values[EMBEDDED_OPEN_PERF_RELEASE_DATABASE_LOCK_NS],
        release_calls
    );
}

static void emit_page_write_perf_stats(const char *prefix) {
    uint64_t values[PAGE_WRITE_PERF_STAT_COUNT] = {0};

    mylite_ownerless_innodb_read_page_write_perf_stats(values, PAGE_WRITE_PERF_STAT_COUNT);
    printf(
        "%s_page_write_enter_calls=%" PRIu64 "\n",
        prefix,
        values[PAGE_WRITE_PERF_STAT_ENTER_CALLS]
    );
    printf(
        "%s_page_write_enter_total_ms=%.3f\n",
        prefix,
        (double)values[PAGE_WRITE_PERF_STAT_ENTER_TOTAL_NS] / 1000000.0
    );
    printf(
        "%s_page_write_acquire_ms=%.3f\n",
        prefix,
        (double)values[PAGE_WRITE_PERF_STAT_ACQUIRE_NS] / 1000000.0
    );
    printf(
        "%s_page_write_refresh_calls=%" PRIu64 "\n",
        prefix,
        values[PAGE_WRITE_PERF_STAT_REFRESH_CALLS]
    );
    printf(
        "%s_page_write_refresh_ms=%.3f\n",
        prefix,
        (double)values[PAGE_WRITE_PERF_STAT_REFRESH_NS] / 1000000.0
    );
    printf(
        "%s_page_write_leave_calls=%" PRIu64 "\n",
        prefix,
        values[PAGE_WRITE_PERF_STAT_LEAVE_CALLS]
    );
    printf(
        "%s_page_write_leave_total_ms=%.3f\n",
        prefix,
        (double)values[PAGE_WRITE_PERF_STAT_LEAVE_TOTAL_NS] / 1000000.0
    );
    printf(
        "%s_page_write_release_ms=%.3f\n",
        prefix,
        (double)values[PAGE_WRITE_PERF_STAT_RELEASE_NS] / 1000000.0
    );
    printf(
        "%s_page_write_publish_calls=%" PRIu64 "\n",
        prefix,
        values[PAGE_WRITE_PERF_STAT_PUBLISH_CALLS]
    );
    printf(
        "%s_page_write_publish_total_ms=%.3f\n",
        prefix,
        (double)values[PAGE_WRITE_PERF_STAT_PUBLISH_TOTAL_NS] / 1000000.0
    );
}

static void emit_page_write_refresh_value(const char *prefix, const char *name, uint64_t value) {
    printf("%s_page_write_refresh_detail_%s=%" PRIu64 "\n", prefix, name, value);
}

static void emit_page_write_refresh_ms(const char *prefix, const char *name, uint64_t value) {
    printf("%s_page_write_refresh_detail_%s_ms=%.3f\n", prefix, name, (double)value / 1000000.0);
}

static void emit_page_write_refresh_stats(const char *prefix) {
    uint64_t values[PAGE_WRITE_REFRESH_STAT_COUNT] = {0};

    mylite_ownerless_innodb_read_page_write_refresh_stats(values, PAGE_WRITE_REFRESH_STAT_COUNT);
    emit_page_write_refresh_value(prefix, "calls", values[PAGE_WRITE_REFRESH_STAT_CALLS]);
    emit_page_write_refresh_value(
        prefix,
        "force_calls",
        values[PAGE_WRITE_REFRESH_STAT_FORCE_CALLS]
    );
    emit_page_write_refresh_value(
        prefix,
        "current_visibility_calls",
        values[PAGE_WRITE_REFRESH_STAT_CURRENT_VISIBILITY_CALLS]
    );
    emit_page_write_refresh_value(
        prefix,
        "skipped_unpublishable",
        values[PAGE_WRITE_REFRESH_STAT_SKIPPED_UNPUBLISHABLE]
    );
    emit_page_write_refresh_value(
        prefix,
        "alloc_failures",
        values[PAGE_WRITE_REFRESH_STAT_ALLOC_FAILURES]
    );
    emit_page_write_refresh_value(
        prefix,
        "visibility_push_calls",
        values[PAGE_WRITE_REFRESH_STAT_VISIBILITY_PUSH_CALLS]
    );
    emit_page_write_refresh_value(
        prefix,
        "visibility_push_errors",
        values[PAGE_WRITE_REFRESH_STAT_VISIBILITY_PUSH_ERRORS]
    );
    emit_page_write_refresh_value(
        prefix,
        "space_misses",
        values[PAGE_WRITE_REFRESH_STAT_SPACE_MISSES]
    );
    emit_page_write_refresh_value(
        prefix,
        "node_misses",
        values[PAGE_WRITE_REFRESH_STAT_NODE_MISSES]
    );
    emit_page_write_refresh_value(
        prefix,
        "page_version_read_calls",
        values[PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_CALLS]
    );
    emit_page_write_refresh_ms(
        prefix,
        "page_version_read",
        values[PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_NS]
    );
    emit_page_write_refresh_value(
        prefix,
        "page_version_hits",
        values[PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_HITS]
    );
    emit_page_write_refresh_value(
        prefix,
        "page_version_misses",
        values[PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_MISSES]
    );
    emit_page_write_refresh_value(
        prefix,
        "page_version_errors",
        values[PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_ERRORS]
    );
    emit_page_write_refresh_value(
        prefix,
        "page_version_identity_mismatch",
        values[PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_IDENTITY_MISMATCH]
    );
    emit_page_write_refresh_value(
        prefix,
        "page_version_not_newer",
        values[PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_NOT_NEWER]
    );
    emit_page_write_refresh_value(
        prefix,
        "page_version_checksum_failures",
        values[PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_CHECKSUM_FAILURES]
    );
    emit_page_write_refresh_value(
        prefix,
        "page_version_overlays",
        values[PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_OVERLAYS]
    );
    emit_page_write_refresh_value(
        prefix,
        "disk_read_calls",
        values[PAGE_WRITE_REFRESH_STAT_DISK_READ_CALLS]
    );
    emit_page_write_refresh_ms(prefix, "disk_read", values[PAGE_WRITE_REFRESH_STAT_DISK_READ_NS]);
    emit_page_write_refresh_value(
        prefix,
        "disk_read_failures",
        values[PAGE_WRITE_REFRESH_STAT_DISK_READ_FAILURES]
    );
    emit_page_write_refresh_value(
        prefix,
        "disk_identity_mismatch",
        values[PAGE_WRITE_REFRESH_STAT_DISK_IDENTITY_MISMATCH]
    );
    emit_page_write_refresh_value(
        prefix,
        "disk_not_newer",
        values[PAGE_WRITE_REFRESH_STAT_DISK_NOT_NEWER]
    );
    emit_page_write_refresh_value(
        prefix,
        "disk_checksum_failures",
        values[PAGE_WRITE_REFRESH_STAT_DISK_CHECKSUM_FAILURES]
    );
    emit_page_write_refresh_value(
        prefix,
        "disk_overlays",
        values[PAGE_WRITE_REFRESH_STAT_DISK_OVERLAYS]
    );
    emit_page_write_refresh_value(
        prefix,
        "space_header_refreshes",
        values[PAGE_WRITE_REFRESH_STAT_SPACE_HEADER_REFRESHES]
    );
    emit_page_write_refresh_value(
        prefix,
        "negative_cache_hits",
        values[PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_HITS]
    );
    emit_page_write_refresh_value(
        prefix,
        "negative_cache_misses",
        values[PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_MISSES]
    );
    emit_page_write_refresh_value(
        prefix,
        "negative_cache_stores",
        values[PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_STORES]
    );
}

static void emit_page_log_append_perf_ms(const char *prefix, const char *name, uint64_t value) {
    printf("%s_page_log_append_%s_ms=%.3f\n", prefix, name, (double)value / 1000000.0);
}

static void emit_page_log_append_perf_stats(const char *prefix) {
    uint64_t values[PAGE_LOG_APPEND_PERF_STAT_COUNT] = {0};

    mylite_ownerless_page_log_read_append_perf_stats(values, PAGE_LOG_APPEND_PERF_STAT_COUNT);
    printf(
        "%s_page_log_append_calls=%" PRIu64 "\n",
        prefix,
        values[PAGE_LOG_APPEND_PERF_STAT_CALLS]
    );
    emit_page_log_append_perf_ms(prefix, "total", values[PAGE_LOG_APPEND_PERF_STAT_TOTAL_NS]);
    emit_page_log_append_perf_ms(prefix, "lock", values[PAGE_LOG_APPEND_PERF_STAT_LOCK_NS]);
    emit_page_log_append_perf_ms(prefix, "header", values[PAGE_LOG_APPEND_PERF_STAT_HEADER_NS]);
    emit_page_log_append_perf_ms(prefix, "body", values[PAGE_LOG_APPEND_PERF_STAT_BODY_NS]);
    emit_page_log_append_perf_ms(prefix, "fstat", values[PAGE_LOG_APPEND_PERF_STAT_FSTAT_NS]);
    emit_page_log_append_perf_ms(prefix, "checksum", values[PAGE_LOG_APPEND_PERF_STAT_CHECKSUM_NS]);
    emit_page_log_append_perf_ms(
        prefix,
        "payload_write",
        values[PAGE_LOG_APPEND_PERF_STAT_PAYLOAD_WRITE_NS]
    );
    emit_page_log_append_perf_ms(
        prefix,
        "record_header_write",
        values[PAGE_LOG_APPEND_PERF_STAT_RECORD_HEADER_WRITE_NS]
    );
}

static void emit_page_log_scan_perf_stats(const char *prefix) {
    uint64_t values[PAGE_LOG_SCAN_PERF_STAT_COUNT] = {0};

    mylite_ownerless_page_log_read_scan_perf_stats(values, PAGE_LOG_SCAN_PERF_STAT_COUNT);
    printf("%s_page_log_scan_calls=%" PRIu64 "\n", prefix, values[PAGE_LOG_SCAN_PERF_STAT_CALLS]);
    printf(
        "%s_page_log_scan_record_headers=%" PRIu64 "\n",
        prefix,
        values[PAGE_LOG_SCAN_PERF_STAT_RECORD_HEADERS]
    );
    printf(
        "%s_page_log_scan_page_records=%" PRIu64 "\n",
        prefix,
        values[PAGE_LOG_SCAN_PERF_STAT_PAGE_RECORDS]
    );
    printf(
        "%s_page_log_scan_visible_page_records=%" PRIu64 "\n",
        prefix,
        values[PAGE_LOG_SCAN_PERF_STAT_VISIBLE_PAGE_RECORDS]
    );
    printf("%s_page_log_scan_found=%" PRIu64 "\n", prefix, values[PAGE_LOG_SCAN_PERF_STAT_FOUND]);
    printf(
        "%s_page_log_scan_not_found_no_page_record=%" PRIu64 "\n",
        prefix,
        values[PAGE_LOG_SCAN_PERF_STAT_NOT_FOUND_NO_PAGE_RECORD]
    );
    printf(
        "%s_page_log_scan_not_found_page_record_not_visible=%" PRIu64 "\n",
        prefix,
        values[PAGE_LOG_SCAN_PERF_STAT_NOT_FOUND_PAGE_RECORD_NOT_VISIBLE]
    );
    printf("%s_page_log_scan_errors=%" PRIu64 "\n", prefix, values[PAGE_LOG_SCAN_PERF_STAT_ERRORS]);
}

static void check_max_ms(const char *env_name, double seconds, unsigned iterations) {
    double threshold_ms;
    const double average_ms = (seconds * 1000.0) / (double)iterations;

    if (!env_double(env_name, &threshold_ms)) {
        return;
    }
    if (average_ms > threshold_ms) {
        fprintf(
            stderr,
            "%s exceeded: %.3fms average > %.3fms\n",
            env_name,
            average_ms,
            threshold_ms
        );
        exit(1);
    }
}

static void check_min_rate(const char *env_name, double rate) {
    double threshold_rate;

    if (!env_double(env_name, &threshold_rate)) {
        return;
    }
    if (rate < threshold_rate) {
        fprintf(stderr, "%s missed: %.2f ops/s < %.2f ops/s\n", env_name, rate, threshold_rate);
        exit(1);
    }
}

static mylite_db *open_database(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config
) {
    mylite_db *db = NULL;
    const int result = mylite_open(paths->database_path, &db, flags, config);
    if (result != MYLITE_OK) {
        fprintf(stderr, "mylite_open failed: result=%d\n", result);
        exit(1);
    }
    return db;
}

static void close_database(mylite_db *db) {
    const int result = mylite_close(db);
    if (result != MYLITE_OK) {
        fprintf(stderr, "mylite_close failed: result=%d\n", result);
        exit(1);
    }
}

static void exec_ok(mylite_db *db, const char *sql) {
    const int result = mylite_exec(db, sql, NULL, NULL, NULL);
    if (result != MYLITE_OK) {
        fprintf(
            stderr,
            "SQL failed: %s\nresult=%d mylite_err=%d mariadb_errno=%u sqlstate=%s message=%s\n",
            sql,
            result,
            mylite_errcode(db),
            mylite_mariadb_errno(db),
            mylite_sqlstate(db),
            mylite_errmsg(db)
        );
        exit(1);
    }
}

static double measure_open_close(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config,
    unsigned iterations
) {
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    start_ns = monotonic_ns();
    for (index = 0; index < iterations; ++index) {
        mylite_db *db = open_database(paths, flags, config);
        close_database(db);
    }
    end_ns = monotonic_ns();
    return elapsed_seconds(start_ns, end_ns);
}

static double measure_active_runtime_reconnect(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config,
    unsigned iterations
) {
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    start_ns = monotonic_ns();
    for (index = 0; index < iterations; ++index) {
        mylite_db *db = open_database(paths, flags, config);
        close_database(db);
    }
    end_ns = monotonic_ns();
    return elapsed_seconds(start_ns, end_ns);
}

static double measure_direct_select(mylite_db *db, unsigned iterations) {
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    start_ns = monotonic_ns();
    for (index = 0; index < iterations; ++index) {
        exec_ok(db, "SELECT 1");
    }
    end_ns = monotonic_ns();
    return elapsed_seconds(start_ns, end_ns);
}

static double measure_prepared_select(mylite_db *db, unsigned iterations) {
    const char *tail = NULL;
    mylite_stmt *stmt = NULL;
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    if (mylite_prepare(db, "SELECT 1", MYLITE_NUL_TERMINATED, &stmt, &tail) != MYLITE_OK) {
        fprintf(stderr, "prepare SELECT 1 failed: %s\n", mylite_errmsg(db));
        exit(1);
    }
    if (tail == NULL || tail[0] != '\0') {
        fprintf(stderr, "prepare SELECT 1 left unexpected tail\n");
        exit(1);
    }

    start_ns = monotonic_ns();
    for (index = 0; index < iterations; ++index) {
        if (mylite_step(stmt) != MYLITE_ROW) {
            fprintf(stderr, "prepared SELECT 1 did not return a row\n");
            exit(1);
        }
        if (mylite_column_int64(stmt, 0) != 1) {
            fprintf(stderr, "prepared SELECT 1 returned an unexpected value\n");
            exit(1);
        }
        if (mylite_step(stmt) != MYLITE_DONE) {
            fprintf(stderr, "prepared SELECT 1 did not finish\n");
            exit(1);
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            fprintf(stderr, "prepared SELECT 1 reset failed\n");
            exit(1);
        }
    }
    end_ns = monotonic_ns();
    if (mylite_finalize(stmt) != MYLITE_OK) {
        fprintf(stderr, "finalize SELECT 1 failed\n");
        exit(1);
    }
    return elapsed_seconds(start_ns, end_ns);
}

static double measure_transactional_insert(
    mylite_db *db,
    const char *table_name,
    unsigned rows,
    int reset_page_publish_stats
) {
    char sql[256];
    const char *tail = NULL;
    mylite_stmt *stmt = NULL;
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    (void)snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS app.%s", table_name);
    exec_ok(db, sql);
    (void)snprintf(
        sql,
        sizeof(sql),
        "CREATE TABLE app.%s (id INT PRIMARY KEY, value VARCHAR(32) NOT NULL) ENGINE=InnoDB",
        table_name
    );
    exec_ok(db, sql);
    (void)snprintf(sql, sizeof(sql), "INSERT INTO app.%s (id, value) VALUES (?, ?)", table_name);
    if (mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, &tail) != MYLITE_OK) {
        fprintf(stderr, "prepare insert failed: %s\n", mylite_errmsg(db));
        exit(1);
    }
    if (tail == NULL || tail[0] != '\0') {
        fprintf(stderr, "prepare insert left unexpected tail\n");
        exit(1);
    }
    if (reset_page_publish_stats) {
        mylite_ownerless_innodb_reset_page_publish_stats();
        mylite_ownerless_innodb_reset_page_write_perf_stats();
        mylite_ownerless_innodb_reset_page_write_refresh_stats();
        mylite_ownerless_innodb_reset_commit_visibility_stats();
        mylite_ownerless_database_reset_perf_stats();
        mylite_ownerless_page_log_reset_append_perf_stats();
        mylite_ownerless_page_log_reset_scan_perf_stats();
    }
    exec_ok(db, "START TRANSACTION");
    start_ns = monotonic_ns();
    for (index = 1U; index <= rows; ++index) {
        if (mylite_bind_int64(stmt, 1U, (long long)index) != MYLITE_OK ||
            mylite_bind_text(stmt, 2U, "mylite-perf", MYLITE_NUL_TERMINATED, MYLITE_STATIC) !=
                MYLITE_OK) {
            fprintf(stderr, "insert bind failed\n");
            exit(1);
        }
        if (mylite_step(stmt) != MYLITE_DONE) {
            fprintf(stderr, "insert step failed\n");
            exit(1);
        }
        if (mylite_reset(stmt) != MYLITE_OK || mylite_clear_bindings(stmt) != MYLITE_OK) {
            fprintf(stderr, "insert reset failed\n");
            exit(1);
        }
    }
    exec_ok(db, "COMMIT");
    end_ns = monotonic_ns();
    if (mylite_finalize(stmt) != MYLITE_OK) {
        fprintf(stderr, "finalize insert failed\n");
        exit(1);
    }
    return elapsed_seconds(start_ns, end_ns);
}

static double measure_autocommit_insert(
    mylite_db *db,
    const char *table_name,
    unsigned rows,
    int reset_page_publish_stats
) {
    char sql[256];
    const char *tail = NULL;
    mylite_stmt *stmt = NULL;
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    (void)snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS app.%s", table_name);
    exec_ok(db, sql);
    (void)snprintf(
        sql,
        sizeof(sql),
        "CREATE TABLE app.%s (id INT PRIMARY KEY, value VARCHAR(32) NOT NULL) ENGINE=InnoDB",
        table_name
    );
    exec_ok(db, sql);
    (void)snprintf(sql, sizeof(sql), "INSERT INTO app.%s (id, value) VALUES (?, ?)", table_name);
    if (mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, &tail) != MYLITE_OK) {
        fprintf(stderr, "prepare autocommit insert failed: %s\n", mylite_errmsg(db));
        exit(1);
    }
    if (tail == NULL || tail[0] != '\0') {
        fprintf(stderr, "prepare autocommit insert left unexpected tail\n");
        exit(1);
    }
    if (reset_page_publish_stats) {
        mylite_ownerless_innodb_reset_page_publish_stats();
        mylite_ownerless_innodb_reset_page_write_perf_stats();
        mylite_ownerless_innodb_reset_page_write_refresh_stats();
        mylite_ownerless_innodb_reset_commit_visibility_stats();
        mylite_ownerless_database_reset_perf_stats();
        mylite_ownerless_page_log_reset_append_perf_stats();
        mylite_ownerless_page_log_reset_scan_perf_stats();
    }
    start_ns = monotonic_ns();
    for (index = 1U; index <= rows; ++index) {
        if (mylite_bind_int64(stmt, 1U, (long long)index) != MYLITE_OK ||
            mylite_bind_text(stmt, 2U, "mylite-perf", MYLITE_NUL_TERMINATED, MYLITE_STATIC) !=
                MYLITE_OK) {
            fprintf(stderr, "autocommit insert bind failed\n");
            exit(1);
        }
        if (mylite_step(stmt) != MYLITE_DONE) {
            fprintf(stderr, "autocommit insert step failed\n");
            exit(1);
        }
        if (mylite_reset(stmt) != MYLITE_OK || mylite_clear_bindings(stmt) != MYLITE_OK) {
            fprintf(stderr, "autocommit insert reset failed\n");
            exit(1);
        }
    }
    end_ns = monotonic_ns();
    if (mylite_finalize(stmt) != MYLITE_OK) {
        fprintf(stderr, "finalize autocommit insert failed\n");
        exit(1);
    }
    return elapsed_seconds(start_ns, end_ns);
}

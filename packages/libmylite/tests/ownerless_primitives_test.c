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

#include "ownerless_dictionary_state.h"
#include "ownerless_innodb_lock_registry.h"
#include "ownerless_latch.h"
#include "ownerless_lock_table.h"
#include "ownerless_mdl.h"
#include "ownerless_page_index.h"
#include "ownerless_page_log.h"
#include "ownerless_probe.h"
#include "ownerless_process_registry.h"
#include "ownerless_read_view_registry.h"
#include "ownerless_redo_state.h"
#include "ownerless_tablespace_replay.h"
#include "ownerless_trx_registry.h"
#include "ownerless_wait.h"

#include "ownerless_test_latch_compat.h"

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_PAGE_SIZE 4096
#define MYLITE_TEST_WAIT_TIMEOUT_MS 5000U
#define MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT 4U
#define MYLITE_TEST_LOCK_TABLE_LATCH_OFFSET 24U
#define MYLITE_TEST_INNODB_LOCK_REGISTRY_SLOT_COUNT 8U
#define MYLITE_TEST_INNODB_LOCK_REGISTRY_LATCH_OFFSET 24U
#define MYLITE_TEST_LOCK_HASH 0xAABBCCDDEEFF0011ULL
#define MYLITE_TEST_PROCESS_REGISTRY_SLOT_COUNT 4U
#define MYLITE_TEST_TRX_REGISTRY_SLOT_COUNT 4U
#define MYLITE_TEST_READ_VIEW_REGISTRY_SLOT_COUNT 4U
#define MYLITE_TEST_REDO_STATE_PROGRESS_LATCH_OFFSET 96U
#define MYLITE_TEST_INNODB_PAGE_OFFSET_OFFSET 4U
#define MYLITE_TEST_INNODB_PAGE_LSN_OFFSET 16U
#define MYLITE_TEST_INNODB_PAGE_TYPE_OFFSET 24U
#define MYLITE_TEST_INNODB_PAGE_SPACE_ID_OFFSET 34U
#define MYLITE_TEST_INNODB_PAGE_TYPE_FSP_HEADER 8U

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

typedef struct redo_reserve_thread_context {
    void *state;
    size_t state_size;
    uint64_t *starts;
    size_t offset;
    size_t count;
} redo_reserve_thread_context;

static void test_mmap_shared_visibility_across_processes(void);
static void test_fcntl_byte_range_lock_conflict(void);
static void test_fcntl_byte_range_lock_release_on_process_exit(void);
static void test_mmap_grow_and_remap(void);
static void test_wait_backend_wakes_across_processes(void);
static void test_wait_backend_times_out_without_change(void);
static void test_latch_records_owner_generation_and_wakes_waiter(void);
static void test_latch_reports_dead_owner_without_stealing(void);
static void test_platform_probe_records_required_primitives(void);
static void test_page_log_reads_latest_visible_page(void);
static void test_page_log_uses_payload_offset(void);
static void test_page_log_uses_reader_snapshots(void);
static void test_page_log_tolerates_corrupt_tail_record(void);
static void test_page_log_rejects_corrupt_interior_record(void);
static void test_page_log_checkpoints_retained_records(void);
static void test_page_log_checkpoints_when_all_records_are_safe(void);
static void test_page_log_replays_record_offsets(void);
static void test_tablespace_replay_applies_visible_page_versions(void);
static void test_tablespace_replay_keeps_newer_disk_page(void);
static void test_tablespace_replay_ignores_non_fsp_page_zero_candidates(void);
static void test_tablespace_replay_rejects_missing_tablespace(void);
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
static void test_innodb_lock_registry_same_page_waiter_fairness(void);
static void test_innodb_lock_registry_waits_across_processes(void);
static void test_innodb_lock_registry_references_and_owner_cleanup(void);
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
static void test_dictionary_state_serializes_ddl_generations(void);
static void test_dictionary_state_reports_dead_active_owner(void);
static void test_redo_state_tracks_lsn_and_owner_lifecycle(void);
static void test_redo_state_reserves_ranges_for_same_owner_threads(void);
static void test_redo_state_allows_bounded_fanout_reservations(void);
static void test_redo_state_tracks_contiguous_written_ranges(void);
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
static uint64_t load_test_be64(const uint8_t *bytes, size_t offset);
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
    test_page_log_reads_latest_visible_page();
    test_page_log_uses_payload_offset();
    test_page_log_uses_reader_snapshots();
    test_page_log_tolerates_corrupt_tail_record();
    test_page_log_rejects_corrupt_interior_record();
    test_page_log_checkpoints_retained_records();
    test_page_log_checkpoints_when_all_records_are_safe();
    test_page_log_replays_record_offsets();
    test_tablespace_replay_applies_visible_page_versions();
    test_tablespace_replay_keeps_newer_disk_page();
    test_tablespace_replay_ignores_non_fsp_page_zero_candidates();
    test_tablespace_replay_rejects_missing_tablespace();
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
    test_innodb_lock_registry_same_page_waiter_fairness();
    test_innodb_lock_registry_waits_across_processes();
    test_innodb_lock_registry_references_and_owner_cleanup();
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
    test_dictionary_state_serializes_ddl_generations();
    test_dictionary_state_reports_dead_active_owner();
    test_redo_state_tracks_lsn_and_owner_lifecycle();
    test_redo_state_reserves_ranges_for_same_owner_threads();
    test_redo_state_allows_bounded_fanout_reservations();
    test_redo_state_tracks_contiguous_written_ranges();
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

static void test_page_log_reads_latest_visible_page(void) {
    char *root = make_temp_root();
    char *log_path = path_join(root, "page-log.bin");
    int fd = open_file(log_path);
    uint8_t page_v1[32];
    uint8_t page_v2[32];
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
    assert(out_page_size == sizeof(page_v2));
    assert(out_page_lsn == 190U);
    assert(out_commit_lsn == 200U);
    assert(memcmp(out_page, page_v2, sizeof(page_v2)) == 0);

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
    page_log_retained_records retained = {0};
    uint8_t page_v1[16];
    uint8_t page_v2[16];
    uint8_t other_page[16];
    uint8_t out_page[16];
    uint64_t record_offset = 0;
    uint64_t page_lsn = 0;
    uint64_t commit_lsn = 0;
    uint32_t out_page_size = 0;

    assert(index != NULL);
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
        mylite_ownerless_page_log_checkpoint(
            fd,
            110U,
            capture_page_log_record_for_index_replace,
            &retained
        ) == MYLITE_OWNERLESS_PAGE_LOG_OK
    );
    assert(retained.count == 2U);
    assert(
        mylite_ownerless_page_index_replace(
            index,
            index_size,
            1U,
            10U,
            retained.records,
            retained.count
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );

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
        ) == MYLITE_OWNERLESS_PAGE_INDEX_OK
    );
    assert(record_offset == first_offset);
    assert(page_lsn == 90U);
    assert(commit_lsn == 100U);
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

static void test_tablespace_replay_keeps_newer_disk_page(void) {
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
    assert(innodb_test_page_lsn(out_page) == 300U);
    assert(out_page[128] == 0x70U);

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

    assert(close(log_fd) == 0);
    free(log_path);
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
    assert(record_offset == 4096U);
    assert(page_lsn == 90U);
    assert(commit_lsn == 100U);
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
        ) == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND
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
        ) == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND
    );
    assert(
        mylite_ownerless_page_index_clear(index, index_size, 1U, 10U) ==
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
        ) == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND
    );
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
        ) == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND
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
        ) == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND
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
        ) == MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND
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
            13U,
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
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
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
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
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
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
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
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
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
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
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
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
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
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
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
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
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
            MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(mylite_ownerless_innodb_lock_registry_active_count(registry) == 0U);

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

        close(child_ready[0]);
        signal_pipe(child_ready[1]);
        child_fd = open_file(shm_path);
        child_registry = map_file(child_fd, MYLITE_TEST_PAGE_SIZE);
        assert(
            mylite_ownerless_innodb_lock_registry_wait_until_record_available(
                child_registry,
                MYLITE_TEST_PAGE_SIZE,
                2U,
                301U,
                30U,
                4U,
                8U,
                10U,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
                MYLITE_TEST_WAIT_TIMEOUT_MS
            ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
        );
        assert(mylite_ownerless_innodb_lock_registry_waiting_count(child_registry) == 0U);
        assert(
            mylite_ownerless_innodb_lock_registry_acquire_record(
                child_registry,
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

static void test_redo_state_tracks_contiguous_written_ranges(void) {
    uint8_t state[MYLITE_OWNERLESS_REDO_STATE_SIZE];
    uint64_t written_lsn = 0U;
    uint64_t start_lsn = 0U;
    uint64_t end_lsn = 0U;
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
    assert(
        mylite_ownerless_process_registry_release(
            registry,
            MYLITE_TEST_PAGE_SIZE,
            parent_slot,
            parent_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(mylite_ownerless_process_registry_active_count(registry) == 0U);

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

static uint64_t load_test_be64(const uint8_t *bytes, size_t offset) {
    uint64_t value = 0;

    for (size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
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

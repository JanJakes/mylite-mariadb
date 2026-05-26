#include <mylite/mylite.h>

#include "mylite_ownerless_innodb_lock_hooks.h"
#include "mylite_ownerless_read_view_hooks.h"
#include "mylite_ownerless_trx_hooks.h"
#include "ownerless_innodb_lock_registry.h"
#include "ownerless_page_index.h"
#include "ownerless_process_registry.h"
#include "ownerless_trx_registry.h"

#include "ownerless_test_latch_compat.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_METADATA_LINE_SIZE 128
#define MYLITE_TEST_METADATA "format=1\nmariadb_base=mariadb-11.8.6\n"
#define MYLITE_TEST_CONCURRENCY_SHM_HEADER_SIZE 128
#define MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE 262144
#define MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TABLE_OFFSET 128
#define MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_OFFSET 512
#define MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_HEADER_SIZE 96
#define MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_COUNT 16
#define MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_SIZE 128
#define MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_SIZE                                              \
    (MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_HEADER_SIZE +                                        \
     (MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_COUNT * MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_SIZE))
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_HEADER_SIZE 64
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_COUNT 16
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SIZE 64
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SEGMENT_SIZE                                          \
    (MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_HEADER_SIZE +                                            \
     (MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_COUNT * MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SIZE))
#define MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_OFFSET                                                \
    (MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_OFFSET +                                             \
     MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_SIZE)
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_HEADER_SIZE 96
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_COUNT 6
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_SIZE 64
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_SEGMENT_SIZE                                        \
    (MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_HEADER_SIZE +                                          \
     (MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_COUNT *                                         \
      MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_SIZE))
#define MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_OFFSET                                              \
    (MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_OFFSET +                                                 \
     MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SEGMENT_SIZE)
#define MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_HEADER_SIZE 96
#define MYLITE_TEST_CONCURRENCY_TRX_SLOT_COUNT 64
#define MYLITE_TEST_CONCURRENCY_TRX_SLOT_SIZE 64
#define MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_SIZE                                                  \
    (MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_HEADER_SIZE +                                            \
     (MYLITE_TEST_CONCURRENCY_TRX_SLOT_COUNT * MYLITE_TEST_CONCURRENCY_TRX_SLOT_SIZE))
#define MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_OFFSET                                                \
    (MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_OFFSET +                                               \
     MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_SEGMENT_SIZE)
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_OFFSET                                          \
    (MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_OFFSET + MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_SIZE)
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_HEADER_SIZE 96
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_COUNT 64
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_SIZE 576
#define MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_SIZE                                            \
    (MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_HEADER_SIZE +                                      \
     (MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_COUNT * MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_SIZE))
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET                                        \
    (MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_OFFSET +                                           \
     MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_SIZE)
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_HEADER_SIZE 96
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SLOT_COUNT 1024
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SLOT_SIZE 128
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_SIZE                                          \
    (MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_HEADER_SIZE +                                    \
     (MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SLOT_COUNT *                                             \
      MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SLOT_SIZE))
#define MYLITE_TEST_CONCURRENCY_REDO_STATE_OFFSET                                                  \
    (((MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET +                                       \
       MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_SIZE + 63U) /                                  \
      64U) *                                                                                       \
     64U)
#define MYLITE_TEST_CONCURRENCY_REDO_STATE_SIZE 64
#define MYLITE_TEST_CONCURRENCY_PAGE_INDEX_OFFSET                                                  \
    (MYLITE_TEST_CONCURRENCY_REDO_STATE_OFFSET + MYLITE_TEST_CONCURRENCY_REDO_STATE_SIZE)
#define MYLITE_TEST_CONCURRENCY_PAGE_INDEX_ENTRY_COUNT 1024
#define MYLITE_TEST_CONCURRENCY_PAGE_INDEX_SIZE                                                    \
    (MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +                                                     \
     (MYLITE_TEST_CONCURRENCY_PAGE_INDEX_ENTRY_COUNT * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE))
#define MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET 64
#define MYLITE_TEST_INNODB_LOCK_SLOT_OWNER_ID_OFFSET 8
#define MYLITE_TEST_INNODB_LOCK_SLOT_STATE_OFFSET 12
#define MYLITE_TEST_INNODB_LOCK_SLOT_KIND_OFFSET 16
#define MYLITE_TEST_INNODB_LOCK_SLOT_MODE_OFFSET 20
#define MYLITE_TEST_INNODB_LOCK_SLOT_FLAGS_OFFSET 24
#define MYLITE_TEST_INNODB_LOCK_SLOT_TRX_ID_OFFSET 32
#define MYLITE_TEST_INNODB_LOCK_SLOT_INDEX_ID_OFFSET 48
#define MYLITE_TEST_INNODB_LOCK_SLOT_SPACE_ID_OFFSET 56
#define MYLITE_TEST_INNODB_LOCK_SLOT_PAGE_NO_OFFSET 60
#define MYLITE_TEST_INNODB_LOCK_SLOT_HEAP_NO_OFFSET 64
#define MYLITE_TEST_EXTERNAL_OWNER_ID 16U
#define MYLITE_TEST_EXTERNAL_TRX_ID 900000U
#define MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO 1205U
#define MYLITE_TEST_WAIT_POLL_INTERVAL_US 10000U

typedef struct text_file {
    const char *path;
    const char *contents;
} text_file;

typedef struct innodb_record_lock_key {
    uint64_t index_id;
    uint32_t space_id;
    uint32_t page_no;
    uint32_t heap_no;
    uint32_t mode;
    uint32_t flags;
} innodb_record_lock_key;

typedef struct external_update_thread_args {
    const char *database_path;
    const char *runtime_root;
    const char *sql;
    int result;
    unsigned mariadb_errno;
    int close_result;
} external_update_thread_args;

static void test_open_close_repeatedly(void);
static void test_capabilities(void);
static void test_memory_path_open_close(void);
static void test_readonly_open_fails(void);
static void test_two_handles_share_runtime(void);
static void test_second_database_fails_while_runtime_open(void);
static void test_directory_suffix_is_not_enforced(void);
static void test_existing_empty_directory_with_create_initializes(void);
static void test_existing_empty_directory_without_create_fails(void);
static void test_nonempty_directory_without_metadata_fails(void);
static void test_invalid_metadata_fails(void);
static void test_incomplete_layout_fails(void);
static void test_missing_concurrency_metadata_is_initialized(void);
static void test_invalid_concurrency_metadata_fails(void);
static void test_concurrency_shared_memory_is_grow_only(void);
static void test_dead_ownerless_transaction_rebuilds_shared_state_on_open(void);
static void test_ownerless_trx_registry_tracks_innodb_sql(void);
static void test_ownerless_read_view_registry_tracks_innodb_sql(void);
static void test_ownerless_innodb_lock_registry_tracks_innodb_sql(void);
static void test_stale_run_directory_is_replaced_on_open(void);
static void test_no_defaults_ignores_ambient_option_files(void);
static void test_missing_file_without_create_fails(void);
static void test_existing_file_path_fails(void);
static void test_exclusive_existing_directory_fails(void);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static mylite_open_config open_config(const char *temp_directory);
static void assert_open_database_layout(const char *database_path);
static void assert_closed_database_layout(const char *database_path);
static void assert_metadata_file(const char *metadata_path);
static void assert_concurrency_metadata_file(const char *metadata_path);
static void assert_concurrency_recovery_file(
    const char *file_path,
    const char *metadata_path,
    const char *magic
);
static void assert_concurrency_shared_memory_file(
    const char *shm_path,
    const char *metadata_path,
    unsigned expected_active_processes,
    uint64_t expected_min_registry_generation,
    uint64_t expected_recovery_generation,
    uint64_t expected_min_mdl_generation,
    uint64_t expected_min_trx_generation
);
static uint64_t read_concurrency_registry_generation(const char *database_path);
static uint64_t read_concurrency_trx_header_field(const char *database_path, off_t field_offset);
static uint64_t read_concurrency_read_view_header_field(
    const char *database_path,
    off_t field_offset
);
static uint64_t read_concurrency_innodb_lock_header_field(
    const char *database_path,
    off_t field_offset
);
static void seed_dead_ownerless_transaction(const char *database_path);
static void exec_ok(mylite_db *db, const char *sql);
static void read_concurrency_uuid(const char *metadata_path, char *uuid, size_t uuid_size);
static innodb_record_lock_key read_first_active_innodb_record_lock(const char *database_path);
static void seed_conflicting_innodb_record_lock(
    const char *database_path,
    innodb_record_lock_key key
);
static void release_conflicting_innodb_record_lock(
    const char *database_path,
    innodb_record_lock_key key
);
static void *execute_external_update_in_thread(void *ctx);
static uint64_t wait_for_concurrency_innodb_lock_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
);
static void sleep_microseconds(unsigned microseconds);
static uint32_t read_le32(const unsigned char *bytes);
static uint64_t read_le64(const unsigned char *bytes);
static void write_le32(unsigned char *bytes, uint32_t value);
static int is_uuid(const char *value);
static off_t file_size(const char *path);
static int is_directory_empty(const char *path);
static int is_directory(const char *path);
static int path_exists(const char *path);
static void write_file(text_file file_data);
static void write_file_prefix(const char *path, const char *contents, size_t size);
static void write_shm_state(const char *shm_path, uint32_t state);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);
static void expect_exec_error(mylite_db *db, const char *sql, unsigned mariadb_errno);

int main(void) {
    test_capabilities();
    test_open_close_repeatedly();
    test_memory_path_open_close();
    test_readonly_open_fails();
    test_two_handles_share_runtime();
    test_second_database_fails_while_runtime_open();
    test_directory_suffix_is_not_enforced();
    test_existing_empty_directory_with_create_initializes();
    test_existing_empty_directory_without_create_fails();
    test_nonempty_directory_without_metadata_fails();
    test_invalid_metadata_fails();
    test_incomplete_layout_fails();
    test_missing_concurrency_metadata_is_initialized();
    test_invalid_concurrency_metadata_fails();
    test_concurrency_shared_memory_is_grow_only();
    test_dead_ownerless_transaction_rebuilds_shared_state_on_open();
    test_ownerless_trx_registry_tracks_innodb_sql();
    test_ownerless_read_view_registry_tracks_innodb_sql();
    test_ownerless_innodb_lock_registry_tracks_innodb_sql();
    test_stale_run_directory_is_replaced_on_open();
    test_no_defaults_ignores_ambient_option_files();
    test_missing_file_without_create_fails();
    test_existing_file_path_fails();
    test_exclusive_existing_directory_fails();
    return 0;
}

static void test_capabilities(void) {
    const unsigned long long capabilities = mylite_capabilities();

    assert((capabilities & MYLITE_CAP_SAME_PROCESS_CONCURRENCY) != 0U);
    assert((capabilities & MYLITE_CAP_OWNERLESS_RW) != 0U);
    assert((capabilities & MYLITE_CAP_SHARED_READONLY) == 0U);
    assert(
        (capabilities & ~(MYLITE_CAP_SAME_PROCESS_CONCURRENCY | MYLITE_CAP_SHARED_READONLY |
                          MYLITE_CAP_OWNERLESS_RW)) == 0U
    );
}

static void test_open_close_repeatedly(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "open-close.mylite");
    mylite_open_config config = open_config(runtime_root);
    uint64_t last_registry_generation = 0U;

    assert(mkdir(runtime_root, 0700) == 0);

    for (int i = 0; i < 2; ++i) {
        mylite_db *db = NULL;
        assert(
            mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
            MYLITE_OK
        );
        assert(db != NULL);
        assert(is_directory(database_path));
        assert_open_database_layout(database_path);
        assert(mylite_errcode(db) == MYLITE_OK);
        assert(mylite_extended_errcode(db) == MYLITE_OK);
        assert(mylite_mariadb_errno(db) == 0U);
        assert(strcmp(mylite_sqlstate(db), "00000") == 0);
        assert(strcmp(mylite_errmsg(db), "not an error") == 0);
        assert(mylite_close(db) == MYLITE_OK);
        assert_closed_database_layout(database_path);
        const uint64_t registry_generation = read_concurrency_registry_generation(database_path);
        assert(registry_generation > last_registry_generation);
        last_registry_generation = registry_generation;
        assert(is_directory_empty(runtime_root));
    }

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_memory_path_open_close(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    assert(
        mylite_open(":memory:", &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(db != NULL);
    assert(mylite_close(db) == MYLITE_OK);
    assert(is_directory_empty(runtime_root));

    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_readonly_open_fails(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "readonly.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READONLY, &config) == MYLITE_MISUSE);
    assert(db == NULL);
    assert(!path_exists(database_path));
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_two_handles_share_runtime(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "shared-runtime.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *first = NULL;
    mylite_db *second = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    assert(
        mylite_open(database_path, &first, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(
        mylite_open(database_path, &second, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(first != NULL);
    assert(second != NULL);
    assert(is_directory(database_path));
    assert_open_database_layout(database_path);

    assert(mylite_close(first) == MYLITE_OK);
    assert_open_database_layout(database_path);
    assert(is_directory_empty(runtime_root));
    assert(mylite_close(second) == MYLITE_OK);
    assert_closed_database_layout(database_path);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_second_database_fails_while_runtime_open(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *first_path = path_join(root, "first.mylite");
    char *second_path = path_join(root, "second.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *first = NULL;
    mylite_db *second = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    assert(
        mylite_open(first_path, &first, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(first != NULL);
    assert(is_directory(first_path));
    assert_open_database_layout(first_path);

    assert(
        mylite_open(second_path, &second, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_BUSY
    );
    assert(second == NULL);
    assert(!path_exists(second_path));

    assert(mylite_close(first) == MYLITE_OK);
    assert_closed_database_layout(first_path);
    assert(is_directory_empty(runtime_root));

    free(second_path);
    free(first_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_directory_suffix_is_not_enforced(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "database-without-suffix");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(mylite_ownerless_trx_has_hooks());
    assert(db != NULL);
    assert_open_database_layout(database_path);
    assert(mylite_close(db) == MYLITE_OK);
    assert_closed_database_layout(database_path);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_existing_empty_directory_with_create_initializes(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "empty-create.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mkdir(database_path, 0700) == 0);

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(db != NULL);
    assert_open_database_layout(database_path);
    assert(mylite_close(db) == MYLITE_OK);
    assert_closed_database_layout(database_path);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_existing_empty_directory_without_create_fails(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "empty-no-create.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mkdir(database_path, 0700) == 0);

    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_NOTFOUND);
    assert(db == NULL);
    assert(is_directory_empty(database_path));
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_nonempty_directory_without_metadata_fails(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "missing-metadata.mylite");
    char *data_path = path_join(database_path, "datadir");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mkdir(database_path, 0700) == 0);
    assert(mkdir(data_path, 0700) == 0);

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_CORRUPT
    );
    assert(db == NULL);
    assert(is_directory_empty(runtime_root));

    free(data_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_invalid_metadata_fails(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "invalid-metadata.mylite");
    char *metadata_path = path_join(database_path, "mylite.meta");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mkdir(database_path, 0700) == 0);
    write_file((text_file){.path = metadata_path, .contents = "format=999\n"});

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_CORRUPT
    );
    assert(db == NULL);
    assert(is_directory_empty(runtime_root));

    free(metadata_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_incomplete_layout_fails(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "incomplete-layout.mylite");
    char *metadata_path = path_join(database_path, "mylite.meta");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mkdir(database_path, 0700) == 0);
    write_file((text_file){.path = metadata_path, .contents = MYLITE_TEST_METADATA});

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_CORRUPT
    );
    assert(db == NULL);
    assert(is_directory_empty(runtime_root));

    free(metadata_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_missing_concurrency_metadata_is_initialized(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "upgrade-concurrency.mylite");
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mkdir(database_path, 0700) == 0);
    assert(mkdir(data_path, 0700) == 0);
    assert(mkdir(tmp_path, 0700) == 0);
    write_file((text_file){.path = metadata_path, .contents = MYLITE_TEST_METADATA});

    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert(db != NULL);
    assert_open_database_layout(database_path);
    assert(mylite_close(db) == MYLITE_OK);
    assert_closed_database_layout(database_path);
    assert(is_directory_empty(runtime_root));

    free(tmp_path);
    free(data_path);
    free(metadata_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_invalid_concurrency_metadata_fails(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "invalid-concurrency.mylite");
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *concurrency_path = path_join(database_path, "concurrency");
    char *concurrency_metadata_path = path_join(concurrency_path, "mylite-concurrency.meta");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mkdir(database_path, 0700) == 0);
    assert(mkdir(data_path, 0700) == 0);
    assert(mkdir(tmp_path, 0700) == 0);
    assert(mkdir(concurrency_path, 0700) == 0);
    write_file((text_file){.path = metadata_path, .contents = MYLITE_TEST_METADATA});
    write_file((text_file){
        .path = concurrency_metadata_path,
        .contents = "format=1\n"
                    "mariadb_base=mariadb-11.8.6\n"
                    "database_uuid=not-a-uuid\n"
                    "concurrency_generation=0\n"
                    "mode=exclusive\n",
    });

    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_CORRUPT);
    assert(db == NULL);
    assert(is_directory_empty(runtime_root));

    free(concurrency_metadata_path);
    free(concurrency_path);
    free(tmp_path);
    free(data_path);
    free(metadata_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_concurrency_shared_memory_is_grow_only(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "grow-shm.mylite");
    char *concurrency_path = path_join(database_path, "concurrency");
    char *metadata_path = path_join(concurrency_path, "mylite-concurrency.meta");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(mylite_close(db) == MYLITE_OK);
    assert_concurrency_shared_memory_file(shm_path, metadata_path, 0U, 2U, 0U, 1U, 0U);

    assert(truncate(shm_path, 1) == 0);
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert_concurrency_shared_memory_file(shm_path, metadata_path, 0U, 2U, 0U, 1U, 0U);

    write_file_prefix(shm_path, "bad-shm!", strlen("bad-shm!"));
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert_concurrency_shared_memory_file(shm_path, metadata_path, 0U, 2U, 0U, 1U, 0U);

    assert(truncate(shm_path, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE * 2) == 0);
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert(file_size(shm_path) == MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE * 2);
    assert_concurrency_shared_memory_file(shm_path, metadata_path, 0U, 2U, 0U, 1U, 0U);
    write_shm_state(shm_path, 2U);
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert(file_size(shm_path) == MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE * 2);
    assert_concurrency_shared_memory_file(shm_path, metadata_path, 0U, 2U, 1U, 1U, 0U);
    assert(is_directory_empty(runtime_root));

    free(shm_path);
    free(metadata_path);
    free(concurrency_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_dead_ownerless_transaction_rebuilds_shared_state_on_open(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "dead-ownerless-trx.mylite");
    char *concurrency_path = path_join(database_path, "concurrency");
    char *metadata_path = path_join(concurrency_path, "mylite-concurrency.meta");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(mylite_close(db) == MYLITE_OK);
    seed_dead_ownerless_transaction(database_path);
    assert(read_concurrency_innodb_lock_header_field(database_path, 16) == 1U);

    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert_concurrency_shared_memory_file(shm_path, metadata_path, 1U, 1U, 1U, 1U, 0U);
    assert(mylite_close(db) == MYLITE_OK);
    assert_concurrency_shared_memory_file(shm_path, metadata_path, 0U, 2U, 1U, 1U, 0U);
    assert(is_directory_empty(runtime_root));

    free(shm_path);
    free(metadata_path);
    free(concurrency_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_trx_registry_tracks_innodb_sql(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-trx-hooks.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;
    uint64_t generation_before;
    uint64_t next_before;
    uint64_t next_after_commit;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_trx ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );

    generation_before = read_concurrency_trx_header_field(database_path, 8);
    next_before = read_concurrency_trx_header_field(database_path, 24);
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "INSERT INTO app.ownerless_trx VALUES (1, 10)");
    assert(read_concurrency_trx_header_field(database_path, 16) == 1U);
    exec_ok(db, "COMMIT");
    assert(read_concurrency_trx_header_field(database_path, 16) == 0U);
    next_after_commit = read_concurrency_trx_header_field(database_path, 24);
    assert(next_after_commit > next_before);
    assert(read_concurrency_trx_header_field(database_path, 8) > generation_before);

    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_trx SET value = value + 1 WHERE id = 1");
    assert(read_concurrency_trx_header_field(database_path, 16) == 1U);
    exec_ok(db, "ROLLBACK");
    assert(read_concurrency_trx_header_field(database_path, 16) == 0U);
    assert(read_concurrency_trx_header_field(database_path, 24) > next_after_commit);

    assert(mylite_close(db) == MYLITE_OK);
    assert(!mylite_ownerless_trx_has_hooks());
    assert(read_concurrency_trx_header_field(database_path, 16) == 0U);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_read_view_registry_tracks_innodb_sql(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-read-view-hooks.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;
    uint64_t generation_before;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(mylite_ownerless_read_view_has_hooks());
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_read_view ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_read_view VALUES (1, 10)");

    generation_before = read_concurrency_read_view_header_field(database_path, 8);
    exec_ok(db, "START TRANSACTION WITH CONSISTENT SNAPSHOT");
    assert(read_concurrency_read_view_header_field(database_path, 16) == 1U);
    exec_ok(db, "SELECT value FROM app.ownerless_read_view WHERE id = 1");
    assert(read_concurrency_read_view_header_field(database_path, 16) == 1U);
    exec_ok(db, "COMMIT");
    assert(read_concurrency_read_view_header_field(database_path, 16) == 0U);
    assert(read_concurrency_read_view_header_field(database_path, 8) > generation_before);

    assert(mylite_close(db) == MYLITE_OK);
    assert(!mylite_ownerless_read_view_has_hooks());
    assert(read_concurrency_read_view_header_field(database_path, 16) == 0U);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_ownerless_innodb_lock_registry_tracks_innodb_sql(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "ownerless-innodb-lock-hooks.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;
    innodb_record_lock_key lock_key;
    uint64_t active_locks;
    pthread_t update_thread;
    external_update_thread_args args = {
        .database_path = database_path,
        .runtime_root = runtime_root,
        .sql = "UPDATE app.ownerless_innodb_lock SET value = value + 1 WHERE id = 1",
        .result = MYLITE_ERROR,
        .mariadb_errno = 0U,
        .close_result = MYLITE_ERROR,
    };

    assert(mkdir(runtime_root, 0700) == 0);
    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(mylite_ownerless_innodb_lock_has_hooks());
    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.ownerless_innodb_lock ("
        "id INT NOT NULL PRIMARY KEY, "
        "value INT NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "INSERT INTO app.ownerless_innodb_lock VALUES (1, 10)");

    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_innodb_lock SET value = value + 1 WHERE id = 1");
    active_locks = read_concurrency_innodb_lock_header_field(database_path, 16);
    assert(active_locks > 0U);
    assert(active_locks <= MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SLOT_COUNT);
    exec_ok(db, "COMMIT");
    assert(read_concurrency_innodb_lock_header_field(database_path, 16) == 0U);

    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.ownerless_innodb_lock SET value = value + 1 WHERE id = 1");
    assert(read_concurrency_innodb_lock_header_field(database_path, 16) > 0U);
    lock_key = read_first_active_innodb_record_lock(database_path);
    exec_ok(db, "ROLLBACK");
    assert(read_concurrency_innodb_lock_header_field(database_path, 16) == 0U);

    seed_conflicting_innodb_record_lock(database_path, lock_key);
    assert(pthread_create(&update_thread, NULL, execute_external_update_in_thread, &args) == 0);
    assert(wait_for_concurrency_innodb_lock_waiting_count(database_path, 1U, 5000U) >= 1U);
    release_conflicting_innodb_record_lock(database_path, lock_key);
    assert(pthread_join(update_thread, NULL) == 0);
    assert(args.result == MYLITE_OK);
    assert(args.mariadb_errno == 0U);
    assert(args.close_result == MYLITE_OK);
    assert(wait_for_concurrency_innodb_lock_waiting_count(database_path, 0U, 5000U) == 0U);
    assert(read_concurrency_innodb_lock_header_field(database_path, 16) == 0U);

    seed_conflicting_innodb_record_lock(database_path, lock_key);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 1");
    expect_exec_error(
        db,
        "UPDATE app.ownerless_innodb_lock SET value = value + 1 WHERE id = 1",
        MYLITE_TEST_LOCK_WAIT_TIMEOUT_ERRNO
    );
    assert(wait_for_concurrency_innodb_lock_waiting_count(database_path, 0U, 5000U) == 0U);
    release_conflicting_innodb_record_lock(database_path, lock_key);
    assert(read_concurrency_innodb_lock_header_field(database_path, 16) == 0U);

    assert(mylite_close(db) == MYLITE_OK);
    assert(!mylite_ownerless_innodb_lock_has_hooks());
    assert(read_concurrency_innodb_lock_header_field(database_path, 16) == 0U);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_stale_run_directory_is_replaced_on_open(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "stale-run.mylite");
    char *run_path = path_join(database_path, "run");
    char *stale_path = path_join(run_path, "stale");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(db != NULL);
    assert(mylite_close(db) == MYLITE_OK);
    assert_closed_database_layout(database_path);
    assert(mkdir(run_path, 0700) == 0);
    write_file((text_file){.path = stale_path, .contents = "stale runtime file\n"});

    db = NULL;
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert(db != NULL);
    assert_open_database_layout(database_path);
    assert(!path_exists(stale_path));
    assert(mylite_close(db) == MYLITE_OK);
    assert_closed_database_layout(database_path);
    assert(is_directory_empty(runtime_root));

    free(stale_path);
    free(run_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_no_defaults_ignores_ambient_option_files(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "no-defaults.mylite");
    char *home = path_join(root, "home");
    char *defaults = path_join(home, ".my.cnf");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mkdir(home, 0700) == 0);
    write_file((text_file){
        .path = defaults,
        .contents = "[server]\ndatadir=/path/that/must/not/be/read\nunknown_mylite_probe=1\n",
    });
    assert(setenv("HOME", home, 1) == 0);
    assert(setenv("MYSQL_HOME", home, 1) == 0);

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    assert(db != NULL);
    assert(is_directory(database_path));
    assert_open_database_layout(database_path);
    assert(mylite_close(db) == MYLITE_OK);
    assert_closed_database_layout(database_path);
    assert(is_directory_empty(runtime_root));

    free(defaults);
    free(home);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_missing_file_without_create_fails(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "missing.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_NOTFOUND);
    assert(db == NULL);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_existing_file_path_fails(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "not-a-directory.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    write_file((text_file){.path = database_path, .contents = ""});

    assert(
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_IOERR
    );
    assert(db == NULL);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_exclusive_existing_directory_fails(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "existing.mylite");
    mylite_open_config config = open_config(runtime_root);
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    assert(mkdir(database_path, 0700) == 0);
    assert(
        mylite_open(
            database_path,
            &db,
            MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE,
            &config
        ) == MYLITE_ERROR
    );
    assert(db == NULL);
    assert(is_directory_empty(runtime_root));

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-test.XXXXXX";
    char *root = mkdtemp(template_path);
    assert(root != NULL);

    char *copy = strdup(root);
    assert(copy != NULL);
    return copy;
}

static char *path_join(const char *directory, const char *name) {
    const size_t directory_len = strlen(directory);
    const size_t name_len = strlen(name);
    char *path = (char *)malloc(directory_len + name_len + 2U);
    assert(path != NULL);
    memcpy(path, directory, directory_len);
    path[directory_len] = '/';
    memcpy(path + directory_len + 1U, name, name_len + 1U);
    return path;
}

static mylite_open_config open_config(const char *temp_directory) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = temp_directory,
    };
    return config;
}

static void assert_open_database_layout(const char *database_path) {
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *concurrency_path = path_join(database_path, "concurrency");
    char *concurrency_metadata_path = path_join(concurrency_path, "mylite-concurrency.meta");
    char *concurrency_lock_path = path_join(concurrency_path, "mylite-concurrency.lock");
    char *concurrency_shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    char *concurrency_wal_path = path_join(concurrency_path, "mylite-concurrency.wal");
    char *concurrency_checkpoint_path = path_join(concurrency_path, "mylite-concurrency.ckpt");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");

    assert(path_exists(metadata_path));
    assert_metadata_file(metadata_path);
    assert(is_directory(concurrency_path));
    assert_concurrency_metadata_file(concurrency_metadata_path);
    assert(path_exists(concurrency_lock_path));
    assert_concurrency_recovery_file(concurrency_wal_path, concurrency_metadata_path, "MYLWAL01");
    assert_concurrency_recovery_file(
        concurrency_checkpoint_path,
        concurrency_metadata_path,
        "MYLCKP01"
    );
    assert_concurrency_shared_memory_file(
        concurrency_shm_path,
        concurrency_metadata_path,
        1U,
        1U,
        0U,
        1U,
        0U
    );
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(is_directory(run_path));
    assert(!is_directory_empty(run_path));

    free(run_path);
    free(tmp_path);
    free(data_path);
    free(concurrency_checkpoint_path);
    free(concurrency_wal_path);
    free(concurrency_shm_path);
    free(concurrency_lock_path);
    free(concurrency_metadata_path);
    free(concurrency_path);
    free(metadata_path);
}

static void assert_closed_database_layout(const char *database_path) {
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *concurrency_path = path_join(database_path, "concurrency");
    char *concurrency_metadata_path = path_join(concurrency_path, "mylite-concurrency.meta");
    char *concurrency_lock_path = path_join(concurrency_path, "mylite-concurrency.lock");
    char *concurrency_shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    char *concurrency_wal_path = path_join(concurrency_path, "mylite-concurrency.wal");
    char *concurrency_checkpoint_path = path_join(concurrency_path, "mylite-concurrency.ckpt");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");

    assert(path_exists(metadata_path));
    assert_metadata_file(metadata_path);
    assert(is_directory(concurrency_path));
    assert_concurrency_metadata_file(concurrency_metadata_path);
    assert(path_exists(concurrency_lock_path));
    assert_concurrency_recovery_file(concurrency_wal_path, concurrency_metadata_path, "MYLWAL01");
    assert_concurrency_recovery_file(
        concurrency_checkpoint_path,
        concurrency_metadata_path,
        "MYLCKP01"
    );
    assert_concurrency_shared_memory_file(
        concurrency_shm_path,
        concurrency_metadata_path,
        0U,
        2U,
        0U,
        1U,
        0U
    );
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(!path_exists(run_path));

    free(run_path);
    free(tmp_path);
    free(data_path);
    free(concurrency_checkpoint_path);
    free(concurrency_wal_path);
    free(concurrency_shm_path);
    free(concurrency_lock_path);
    free(concurrency_metadata_path);
    free(concurrency_path);
    free(metadata_path);
}

static void assert_metadata_file(const char *metadata_path) {
    char format_line[MYLITE_TEST_METADATA_LINE_SIZE];
    char base_line[MYLITE_TEST_METADATA_LINE_SIZE];
    FILE *file = fopen(metadata_path, "r");
    assert(file != NULL);
    assert(fgets(format_line, sizeof(format_line), file) == format_line);
    assert(strcmp(format_line, "format=1\n") == 0);
    assert(fgets(base_line, sizeof(base_line), file) == base_line);
    assert(strcmp(base_line, "mariadb_base=mariadb-11.8.6\n") == 0);
    assert(fclose(file) == 0);
}

static void assert_concurrency_metadata_file(const char *metadata_path) {
    char format_line[MYLITE_TEST_METADATA_LINE_SIZE];
    char base_line[MYLITE_TEST_METADATA_LINE_SIZE];
    char uuid_line[MYLITE_TEST_METADATA_LINE_SIZE];
    char generation_line[MYLITE_TEST_METADATA_LINE_SIZE];
    char mode_line[MYLITE_TEST_METADATA_LINE_SIZE];
    FILE *file = fopen(metadata_path, "r");

    assert(file != NULL);
    assert(fgets(format_line, sizeof(format_line), file) == format_line);
    assert(strcmp(format_line, "format=1\n") == 0);
    assert(fgets(base_line, sizeof(base_line), file) == base_line);
    assert(strcmp(base_line, "mariadb_base=mariadb-11.8.6\n") == 0);
    assert(fgets(uuid_line, sizeof(uuid_line), file) == uuid_line);
    assert(strncmp(uuid_line, "database_uuid=", strlen("database_uuid=")) == 0);
    assert(is_uuid(uuid_line + strlen("database_uuid=")));
    assert(fgets(generation_line, sizeof(generation_line), file) == generation_line);
    assert(strcmp(generation_line, "concurrency_generation=0\n") == 0);
    assert(fgets(mode_line, sizeof(mode_line), file) == mode_line);
    assert(strcmp(mode_line, "mode=exclusive\n") == 0);
    assert(fclose(file) == 0);
}

static void assert_concurrency_recovery_file(
    const char *file_path,
    const char *metadata_path,
    const char *magic
) {
    unsigned char header[128];
    char uuid[37];
    FILE *file = fopen(file_path, "rb");

    assert(file_size(file_path) >= (off_t)sizeof(header));
    assert(file != NULL);
    assert(fread(header, 1U, sizeof(header), file) == sizeof(header));
    assert(fclose(file) == 0);

    read_concurrency_uuid(metadata_path, uuid, sizeof(uuid));
    assert(memcmp(header, magic, 8U) == 0);
    assert(read_le32(header + 8U) == 1U);
    assert(read_le32(header + 12U) == sizeof(header));
    assert(read_le32(header + 16U) == 0x01020304U);
    assert(read_le32(header + 20U) == 0U);
    assert(read_le64(header + 24U) == 0U);
    assert(memcmp(header + 64U, uuid, 36U) == 0);
    for (size_t index = 100U; index < sizeof(header); ++index) {
        assert(header[index] == 0U);
    }
}

static void assert_concurrency_shared_memory_file(
    const char *shm_path,
    const char *metadata_path,
    unsigned expected_active_processes,
    uint64_t expected_min_registry_generation,
    uint64_t expected_recovery_generation,
    uint64_t expected_min_mdl_generation,
    uint64_t expected_min_trx_generation
) {
    int fd;
    const unsigned char *page;
    const unsigned char *header;
    const unsigned char *process_segment;
    const unsigned char *wait_segment;
    const unsigned char *mdl_lock_segment;
    const unsigned char *trx_segment;
    const unsigned char *read_view_segment;
    const unsigned char *innodb_lock_segment;
    const unsigned char *redo_segment;
    const unsigned char *page_index_segment;
    const unsigned char *registry;
    const unsigned char *wait_channels;
    const unsigned char *mdl_lock_table;
    const unsigned char *trx_registry;
    const unsigned char *read_view_registry;
    const unsigned char *innodb_lock_registry;
    const unsigned char *page_index;
    char uuid[37];
    const off_t shm_size = file_size(shm_path);
    unsigned active_slots = 0U;

    assert(shm_size >= MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE);
    fd = open(shm_path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    page =
        mmap(NULL, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(page != MAP_FAILED);
    header = page;
    process_segment = page + MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TABLE_OFFSET;
    wait_segment = process_segment + 32U;
    mdl_lock_segment = wait_segment + 32U;
    trx_segment = mdl_lock_segment + 32U;
    read_view_segment = trx_segment + 32U;
    innodb_lock_segment = read_view_segment + 32U;
    redo_segment = innodb_lock_segment + 32U;
    page_index_segment = redo_segment + 32U;
    registry = page + MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_OFFSET;
    wait_channels = page + MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_OFFSET;
    mdl_lock_table = page + MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_OFFSET;
    trx_registry = page + MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_OFFSET;
    read_view_registry = page + MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_OFFSET;
    innodb_lock_registry = page + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET;
    page_index = page + MYLITE_TEST_CONCURRENCY_PAGE_INDEX_OFFSET;

    read_concurrency_uuid(metadata_path, uuid, sizeof(uuid));
    assert(memcmp(header, "MYLSHM01", 8U) == 0);
    assert(read_le32(header + 8U) == 8U);
    assert(read_le32(header + 12U) == 1U);
    assert(read_le32(header + 16U) == MYLITE_TEST_CONCURRENCY_SHM_HEADER_SIZE);
    assert(read_le32(header + 20U) == 0x01020304U);
    assert(read_le32(header + 24U) == 0U);
    assert(read_le32(header + 28U) == (expected_active_processes > 0U ? 2U : 1U));
    assert(read_le64(header + 32U) == (uint64_t)shm_size);
    assert(read_le64(header + 40U) == 0U);
    assert(read_le64(header + 48U) == expected_recovery_generation);
    assert(read_le32(header + 56U) == MYLITE_TEST_CONCURRENCY_SHM_SEGMENT_TABLE_OFFSET);
    assert(read_le32(header + 60U) == 8U);
    assert(memcmp(header + 64U, uuid, 36U) == 0);
    for (size_t index = 100U; index < MYLITE_TEST_CONCURRENCY_SHM_HEADER_SIZE; ++index) {
        assert(header[index] == 0U);
    }

    assert(read_le32(process_segment) == 1U);
    assert(read_le32(process_segment + 4U) == 2U);
    assert(read_le64(process_segment + 8U) == MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_OFFSET);
    assert(
        read_le64(process_segment + 16U) ==
        MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_HEADER_SIZE +
            (MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_COUNT * MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_SIZE)
    );
    assert(read_le64(process_segment + 24U) == 0U);

    assert(read_le32(wait_segment) == 2U);
    assert(read_le32(wait_segment + 4U) == 1U);
    assert(read_le64(wait_segment + 8U) == MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_OFFSET);
    assert(
        read_le64(wait_segment + 16U) ==
        MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_HEADER_SIZE +
            (MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_COUNT * MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SIZE)
    );
    assert(read_le64(wait_segment + 24U) == 0U);

    assert(read_le32(mdl_lock_segment) == 3U);
    assert(read_le32(mdl_lock_segment + 4U) == 2U);
    assert(read_le64(mdl_lock_segment + 8U) == MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_OFFSET);
    assert(
        read_le64(mdl_lock_segment + 16U) ==
        MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_HEADER_SIZE +
            (MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_COUNT *
             MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_SIZE)
    );
    assert(read_le64(mdl_lock_segment + 24U) == 0U);

    assert(read_le32(trx_segment) == 4U);
    assert(read_le32(trx_segment + 4U) == 2U);
    assert(read_le64(trx_segment + 8U) == MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_OFFSET);
    assert(
        read_le64(trx_segment + 16U) ==
        MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_HEADER_SIZE +
            (MYLITE_TEST_CONCURRENCY_TRX_SLOT_COUNT * MYLITE_TEST_CONCURRENCY_TRX_SLOT_SIZE)
    );
    assert(read_le64(trx_segment + 24U) == 0U);

    assert(read_le32(read_view_segment) == 5U);
    assert(read_le32(read_view_segment + 4U) == 2U);
    assert(read_le64(read_view_segment + 8U) == MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_OFFSET);
    assert(read_le64(read_view_segment + 16U) == MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_SIZE);
    assert(read_le64(read_view_segment + 24U) == 0U);

    assert(read_le32(innodb_lock_segment) == 6U);
    assert(read_le32(innodb_lock_segment + 4U) == 3U);
    assert(
        read_le64(innodb_lock_segment + 8U) == MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET
    );
    assert(
        read_le64(innodb_lock_segment + 16U) == MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_SIZE
    );
    assert(read_le64(innodb_lock_segment + 24U) == 0U);

    assert(read_le32(redo_segment) == 7U);
    assert(read_le32(redo_segment + 4U) == 3U);
    assert(read_le64(redo_segment + 8U) == MYLITE_TEST_CONCURRENCY_REDO_STATE_OFFSET);
    assert(read_le64(redo_segment + 16U) == MYLITE_TEST_CONCURRENCY_REDO_STATE_SIZE);
    assert(read_le64(redo_segment + 24U) == 0U);

    assert(read_le32(page_index_segment) == 8U);
    assert(read_le32(page_index_segment + 4U) == 2U);
    assert(read_le64(page_index_segment + 8U) == MYLITE_TEST_CONCURRENCY_PAGE_INDEX_OFFSET);
    assert(read_le64(page_index_segment + 16U) == MYLITE_TEST_CONCURRENCY_PAGE_INDEX_SIZE);
    assert(read_le64(page_index_segment + 24U) == 0U);

    assert(read_le32(registry) == MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_COUNT);
    assert(read_le32(registry + 4U) == MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_SIZE);
    assert(read_le64(registry + 8U) >= expected_min_registry_generation);
    assert(read_le64(registry + 16U) == expected_active_processes);

    for (size_t slot_index = 0; slot_index < MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_COUNT;
         ++slot_index) {
        const unsigned char *slot = registry +
                                    MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_HEADER_SIZE +
                                    (slot_index * MYLITE_TEST_CONCURRENCY_PROCESS_SLOT_SIZE);
        const uint32_t state = read_le32(slot + 8U);
        if (state == 0U) {
            continue;
        }

        ++active_slots;
        assert(read_le64(slot) > 0U);
        assert(state == 1U);
        assert(read_le32(slot + 12U) == 1U);
        assert(read_le64(slot + 16U) == (uint64_t)getpid());
        assert(read_le64(slot + 24U) > 0U);
        assert(read_le64(slot + 32U) == 0U);
        assert(read_le64(slot + 40U) == 0U);
        assert(read_le64(slot + 48U) == 0U);
        assert(read_le64(slot + 56U) == 0U);
        assert(
            read_le64(slot + 64U) == MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_OFFSET +
                                         MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_HEADER_SIZE
        );
        assert(read_le64(slot + 72U) == MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_COUNT);
    }
    assert(active_slots == expected_active_processes);

    assert(read_le32(wait_channels) == MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_COUNT);
    assert(read_le32(wait_channels + 4U) == MYLITE_TEST_CONCURRENCY_WAIT_CHANNEL_SIZE);
    assert(read_le64(wait_channels + 8U) == 0U);

    assert(read_le32(mdl_lock_table) == MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_COUNT);
    assert(read_le32(mdl_lock_table + 4U) == MYLITE_TEST_CONCURRENCY_MDL_LOCK_TABLE_ENTRY_SIZE);
    assert(read_le64(mdl_lock_table + 8U) >= expected_min_mdl_generation);
    assert(read_le64(mdl_lock_table + 16U) == 0U);

    assert(read_le32(trx_registry) == MYLITE_TEST_CONCURRENCY_TRX_SLOT_COUNT);
    assert(read_le32(trx_registry + 4U) == MYLITE_TEST_CONCURRENCY_TRX_SLOT_SIZE);
    assert(read_le64(trx_registry + 8U) >= expected_min_trx_generation);
    assert(read_le64(trx_registry + 16U) == 0U);
    assert(read_le64(trx_registry + 24U) >= 1U);
    assert(read_le64(trx_registry + 32U) == 0U);
    assert(read_le64(trx_registry + 48U) == 0U);

    assert(read_le32(read_view_registry) == MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_COUNT);
    assert(read_le32(read_view_registry + 4U) == MYLITE_TEST_CONCURRENCY_READ_VIEW_SLOT_SIZE);
    assert(read_le64(read_view_registry + 16U) == 0U);

    assert(read_le32(innodb_lock_registry) == MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SLOT_COUNT);
    assert(read_le32(innodb_lock_registry + 4U) == MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SLOT_SIZE);
    assert(read_le64(innodb_lock_registry + 16U) == 0U);
    assert(read_le64(innodb_lock_registry + 24U) == 0U);
    assert(read_le64(innodb_lock_registry + 40U) == 0U);

    assert(read_le32(page_index + 32U) == MYLITE_TEST_CONCURRENCY_PAGE_INDEX_ENTRY_COUNT);
    assert(read_le32(page_index + 36U) == MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
    assert(read_le32(page_index + 40U) == 0U);
    assert(read_le32(page_index + 44U) == 0U);
    assert(read_le64(page_index + 48U) >= 1U);
    assert(munmap((void *)page, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE) == 0);
    assert(close(fd) == 0);
}

static uint64_t read_concurrency_registry_generation(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    assert(
        pread(fd, bytes, sizeof(bytes), MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_OFFSET + 8) ==
        (ssize_t)sizeof(bytes)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_le64(bytes);
}

static uint64_t read_concurrency_trx_header_field(const char *database_path, off_t field_offset) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    assert(
        pread(
            fd,
            bytes,
            sizeof(bytes),
            (off_t)MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_OFFSET + field_offset
        ) == (ssize_t)sizeof(bytes)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_le64(bytes);
}

static uint64_t read_concurrency_read_view_header_field(
    const char *database_path,
    off_t field_offset
) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    assert(
        pread(
            fd,
            bytes,
            sizeof(bytes),
            (off_t)MYLITE_TEST_CONCURRENCY_READ_VIEW_REGISTRY_OFFSET + field_offset
        ) == (ssize_t)sizeof(bytes)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_le64(bytes);
}

static uint64_t read_concurrency_innodb_lock_header_field(
    const char *database_path,
    off_t field_offset
) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    unsigned char bytes[8];
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);

    assert(fd >= 0);
    assert(
        pread(
            fd,
            bytes,
            sizeof(bytes),
            (off_t)MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET + field_offset
        ) == (ssize_t)sizeof(bytes)
    );
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
    return read_le64(bytes);
}

static void seed_dead_ownerless_transaction(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    int fd = open(shm_path, O_RDWR | O_CLOEXEC);
    unsigned char *page;
    unsigned char *registry;
    unsigned char *trx_registry;
    unsigned char *innodb_lock_registry;
    uint32_t process_slot = 0U;
    uint64_t process_generation = 0U;
    uint64_t next_trx_id = 0U;
    uint64_t trx_id = 0U;
    uint32_t trx_slot = 0U;
    uint64_t trx_generation = 0U;

    assert(fd >= 0);
    assert(file_size(shm_path) >= MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE);
    page =
        mmap(NULL, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(page != MAP_FAILED);
    registry = page + MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_OFFSET;
    trx_registry = page + MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_OFFSET;
    innodb_lock_registry = page + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET;
    next_trx_id = read_le64(trx_registry + 24U);

    assert(
        mylite_ownerless_process_registry_allocate(
            registry,
            MYLITE_TEST_CONCURRENCY_PROCESS_REGISTRY_SIZE,
            UINT64_MAX,
            1U,
            0U,
            &process_slot,
            &process_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
    assert(
        mylite_ownerless_trx_registry_begin(
            trx_registry,
            MYLITE_TEST_CONCURRENCY_TRX_REGISTRY_SIZE,
            process_slot + 1U,
            &trx_id,
            &trx_slot,
            &trx_generation
        ) == MYLITE_OWNERLESS_TRX_REGISTRY_OK
    );
    assert(trx_id == next_trx_id);
    assert(trx_slot == 0U);
    assert(process_generation > 0U);
    assert(trx_generation > 0U);
    assert(
        mylite_ownerless_innodb_lock_registry_acquire_table(
            innodb_lock_registry,
            MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_SIZE,
            process_slot + 1U,
            trx_id,
            42U,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_IX,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(msync(page, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE, MS_SYNC) == 0);
    assert(munmap(page, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
}

static innodb_record_lock_key read_first_active_innodb_record_lock(const char *database_path) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    int fd = open(shm_path, O_RDONLY);
    unsigned char *page;
    unsigned char *registry;
    innodb_record_lock_key key = {0};

    assert(fd >= 0);
    page = mmap(NULL, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    assert(page != MAP_FAILED);
    registry = page + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET;

    for (uint32_t slot_index = 0; slot_index < MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SLOT_COUNT;
         ++slot_index) {
        unsigned char *slot = registry + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_HEADER_SIZE +
                              (slot_index * MYLITE_TEST_CONCURRENCY_INNODB_LOCK_SLOT_SIZE);
        const uint32_t state = read_le32(slot + MYLITE_TEST_INNODB_LOCK_SLOT_STATE_OFFSET);
        const uint32_t kind = read_le32(slot + MYLITE_TEST_INNODB_LOCK_SLOT_KIND_OFFSET);

        if (state != MYLITE_OWNERLESS_INNODB_LOCK_STATE_ACTIVE ||
            kind != MYLITE_OWNERLESS_INNODB_LOCK_KIND_RECORD) {
            continue;
        }

        assert(read_le32(slot + MYLITE_TEST_INNODB_LOCK_SLOT_OWNER_ID_OFFSET) != 0U);
        assert(read_le64(slot + MYLITE_TEST_INNODB_LOCK_SLOT_TRX_ID_OFFSET) != 0U);
        key.index_id = read_le64(slot + MYLITE_TEST_INNODB_LOCK_SLOT_INDEX_ID_OFFSET);
        key.space_id = read_le32(slot + MYLITE_TEST_INNODB_LOCK_SLOT_SPACE_ID_OFFSET);
        key.page_no = read_le32(slot + MYLITE_TEST_INNODB_LOCK_SLOT_PAGE_NO_OFFSET);
        key.heap_no = read_le32(slot + MYLITE_TEST_INNODB_LOCK_SLOT_HEAP_NO_OFFSET);
        key.mode = read_le32(slot + MYLITE_TEST_INNODB_LOCK_SLOT_MODE_OFFSET);
        key.flags = read_le32(slot + MYLITE_TEST_INNODB_LOCK_SLOT_FLAGS_OFFSET);
        break;
    }

    assert(munmap(page, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);

    assert(key.index_id != 0U);
    return key;
}

static void seed_conflicting_innodb_record_lock(
    const char *database_path,
    innodb_record_lock_key key
) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    int fd = open(shm_path, O_RDWR);
    unsigned char *page;
    unsigned char *registry;

    assert(fd >= 0);
    page =
        mmap(NULL, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(page != MAP_FAILED);
    registry = page + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET;

    assert(
        mylite_ownerless_innodb_lock_registry_acquire_record(
            registry,
            MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_SIZE,
            MYLITE_TEST_EXTERNAL_OWNER_ID,
            MYLITE_TEST_EXTERNAL_TRX_ID,
            key.index_id,
            key.space_id,
            key.page_no,
            key.heap_no,
            key.mode,
            key.flags,
            0U
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(msync(page, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE, MS_SYNC) == 0);
    assert(munmap(page, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
}

static void release_conflicting_innodb_record_lock(
    const char *database_path,
    innodb_record_lock_key key
) {
    char *concurrency_path = path_join(database_path, "concurrency");
    char *shm_path = path_join(concurrency_path, "mylite-concurrency.shm");
    int fd = open(shm_path, O_RDWR);
    unsigned char *page;
    unsigned char *registry;

    assert(fd >= 0);
    page =
        mmap(NULL, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(page != MAP_FAILED);
    registry = page + MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_OFFSET;

    assert(
        mylite_ownerless_innodb_lock_registry_release_record(
            registry,
            MYLITE_TEST_CONCURRENCY_INNODB_LOCK_REGISTRY_SIZE,
            MYLITE_TEST_EXTERNAL_OWNER_ID,
            MYLITE_TEST_EXTERNAL_TRX_ID,
            key.index_id,
            key.space_id,
            key.page_no,
            key.heap_no,
            key.mode,
            key.flags
        ) == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK
    );
    assert(msync(page, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE, MS_SYNC) == 0);
    assert(munmap(page, MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE) == 0);
    assert(close(fd) == 0);
    free(shm_path);
    free(concurrency_path);
}

static void *execute_external_update_in_thread(void *ctx) {
    external_update_thread_args *args = ctx;
    mylite_open_config config = open_config(args->runtime_root);
    mylite_db *db = NULL;
    char *errmsg = NULL;

    assert(mylite_open(args->database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    exec_ok(db, "SET SESSION innodb_lock_wait_timeout = 10");
    args->result = mylite_exec(db, args->sql, NULL, NULL, &errmsg);
    args->mariadb_errno = mylite_mariadb_errno(db);
    if (errmsg != NULL) {
        mylite_free(errmsg);
    }
    args->close_result = mylite_close(db);
    return NULL;
}

static uint64_t wait_for_concurrency_innodb_lock_waiting_count(
    const char *database_path,
    uint64_t expected_minimum,
    unsigned timeout_ms
) {
    const unsigned iterations = timeout_ms * 1000U / MYLITE_TEST_WAIT_POLL_INTERVAL_US;

    for (unsigned iteration = 0U; iteration <= iterations; ++iteration) {
        const uint64_t waiting_count = read_concurrency_innodb_lock_header_field(
            database_path,
            MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET
        );
        if (expected_minimum == 0U) {
            if (waiting_count == 0U) {
                return waiting_count;
            }
        } else if (waiting_count >= expected_minimum) {
            return waiting_count;
        }
        sleep_microseconds(MYLITE_TEST_WAIT_POLL_INTERVAL_US);
    }
    return read_concurrency_innodb_lock_header_field(
        database_path,
        MYLITE_TEST_CONCURRENCY_INNODB_LOCK_WAITING_COUNT_OFFSET
    );
}

static void sleep_microseconds(unsigned microseconds) {
    struct timespec remaining = {
        .tv_sec = (time_t)(microseconds / 1000000U),
        .tv_nsec = (long)((microseconds % 1000000U) * 1000U),
    };

    while (nanosleep(&remaining, &remaining) != 0) {
        assert(errno == EINTR);
    }
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
}

static void expect_exec_error(mylite_db *db, const char *sql, unsigned mariadb_errno) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) == mariadb_errno);
    assert(errmsg != NULL);
    mylite_free(errmsg);
}

static void read_concurrency_uuid(const char *metadata_path, char *uuid, size_t uuid_size) {
    char line[MYLITE_TEST_METADATA_LINE_SIZE];
    FILE *file = fopen(metadata_path, "r");

    assert(uuid_size >= 37U);
    assert(file != NULL);
    while (fgets(line, sizeof(line), file) == line) {
        if (strncmp(line, "database_uuid=", strlen("database_uuid=")) != 0) {
            continue;
        }
        assert(is_uuid(line + strlen("database_uuid=")));
        memcpy(uuid, line + strlen("database_uuid="), 36U);
        uuid[36] = '\0';
        assert(fclose(file) == 0);
        return;
    }
    assert(fclose(file) == 0);
    assert(0);
}

static uint32_t read_le32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t read_le64(const unsigned char *bytes) {
    uint64_t value = 0U;
    for (size_t index = 0; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }
    return value;
}

static void write_le32(unsigned char *bytes, uint32_t value) {
    for (size_t index = 0; index < 4U; ++index) {
        bytes[index] = (unsigned char)((value >> (index * 8U)) & 0xFFU);
    }
}

static int is_uuid(const char *value) {
    for (size_t index = 0; index < 36U; ++index) {
        const char c = value[index];
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (c != '-') {
                return 0;
            }
            continue;
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return value[36] == '\n' && value[37] == '\0';
}

static off_t file_size(const char *path) {
    struct stat path_stat;

    assert(stat(path, &path_stat) == 0);
    assert(S_ISREG(path_stat.st_mode));
    return path_stat.st_size;
}

static int is_directory_empty(const char *path) {
    DIR *directory = opendir(path);
    assert(directory != NULL);

    int count = 0;
    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }

    assert(closedir(directory) == 0);
    return count == 0;
}

static int is_directory(const char *path) {
    struct stat path_stat;
    if (lstat(path, &path_stat) != 0) {
        fprintf(stderr, "missing directory path: %s\n", path);
        assert(0);
    }
    return S_ISDIR(path_stat.st_mode);
}

static int path_exists(const char *path) {
    struct stat path_stat;
    if (lstat(path, &path_stat) == 0) {
        return 1;
    }
    assert(errno == ENOENT);
    return 0;
}

static void write_file(text_file file_data) {
    FILE *file = fopen(file_data.path, "w");
    assert(file != NULL);
    assert(fputs(file_data.contents, file) >= 0);
    assert(fclose(file) == 0);
}

static void write_file_prefix(const char *path, const char *contents, size_t size) {
    FILE *file = fopen(path, "r+b");
    assert(file != NULL);
    assert(fwrite(contents, 1U, size, file) == size);
    assert(fclose(file) == 0);
}

static void write_shm_state(const char *shm_path, uint32_t state) {
    unsigned char state_bytes[4];
    FILE *file = fopen(shm_path, "r+b");
    write_le32(state_bytes, state);

    assert(file != NULL);
    assert(fseek(file, 28L, SEEK_SET) == 0);
    assert(fwrite(state_bytes, 1U, sizeof(state_bytes), file) == sizeof(state_bytes));
    assert(fclose(file) == 0);
}

static void remove_tree(const char *path) {
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
    return type_flag == FTW_DP ? rmdir(path) : unlink(path);
}

#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_METADATA_LINE_SIZE 128
#define MYLITE_TEST_METADATA "format=1\nmariadb_base=mariadb-11.8.6\n"
#define MYLITE_TEST_CONCURRENCY_SHM_HEADER_SIZE 128
#define MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE 4096

typedef struct text_file {
    const char *path;
    const char *contents;
} text_file;

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
static void assert_concurrency_shared_memory_file(
    const char *shm_path,
    const char *metadata_path
);
static void read_concurrency_uuid(const char *metadata_path, char *uuid, size_t uuid_size);
static uint32_t read_le32(const unsigned char *bytes);
static uint64_t read_le64(const unsigned char *bytes);
static int is_uuid(const char *value);
static off_t file_size(const char *path);
static int is_directory_empty(const char *path);
static int is_directory(const char *path);
static int path_exists(const char *path);
static void write_file(text_file file_data);
static void write_file_prefix(const char *path, const char *contents, size_t size);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

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
    assert((capabilities & MYLITE_CAP_SHARED_READONLY) == 0U);
    assert((capabilities & MYLITE_CAP_OWNERLESS_RW) == 0U);
}

static void test_open_close_repeatedly(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "open-close.mylite");
    mylite_open_config config = open_config(runtime_root);

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
    write_file(
        (text_file){
            .path = concurrency_metadata_path,
            .contents = "format=1\n"
                        "mariadb_base=mariadb-11.8.6\n"
                        "database_uuid=not-a-uuid\n"
                        "concurrency_generation=0\n"
                        "mode=exclusive\n",
        }
    );

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
    assert_concurrency_shared_memory_file(shm_path, metadata_path);

    assert(truncate(shm_path, 1) == 0);
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert_concurrency_shared_memory_file(shm_path, metadata_path);

    write_file_prefix(shm_path, "bad-shm!", strlen("bad-shm!"));
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert_concurrency_shared_memory_file(shm_path, metadata_path);

    assert(truncate(shm_path, 8192) == 0);
    assert(mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK);
    assert(mylite_close(db) == MYLITE_OK);
    assert(file_size(shm_path) == 8192);
    assert_concurrency_shared_memory_file(shm_path, metadata_path);
    assert(is_directory_empty(runtime_root));

    free(shm_path);
    free(metadata_path);
    free(concurrency_path);
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
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");
    char *plugin_path = path_join(run_path, "plugins");

    assert(path_exists(metadata_path));
    assert_metadata_file(metadata_path);
    assert(is_directory(concurrency_path));
    assert_concurrency_metadata_file(concurrency_metadata_path);
    assert(path_exists(concurrency_lock_path));
    assert_concurrency_shared_memory_file(concurrency_shm_path, concurrency_metadata_path);
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(is_directory(run_path));
    assert(is_directory(plugin_path));

    free(plugin_path);
    free(run_path);
    free(tmp_path);
    free(data_path);
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
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");

    assert(path_exists(metadata_path));
    assert_metadata_file(metadata_path);
    assert(is_directory(concurrency_path));
    assert_concurrency_metadata_file(concurrency_metadata_path);
    assert(path_exists(concurrency_lock_path));
    assert_concurrency_shared_memory_file(concurrency_shm_path, concurrency_metadata_path);
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(!path_exists(run_path));

    free(run_path);
    free(tmp_path);
    free(data_path);
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

static void assert_concurrency_shared_memory_file(
    const char *shm_path,
    const char *metadata_path
) {
    unsigned char header[MYLITE_TEST_CONCURRENCY_SHM_HEADER_SIZE];
    char uuid[37];
    FILE *file = fopen(shm_path, "rb");
    const off_t shm_size = file_size(shm_path);

    assert(shm_size >= MYLITE_TEST_CONCURRENCY_SHM_MIN_SIZE);
    assert(file != NULL);
    assert(
        fread(header, 1U, MYLITE_TEST_CONCURRENCY_SHM_HEADER_SIZE, file) ==
        MYLITE_TEST_CONCURRENCY_SHM_HEADER_SIZE
    );
    assert(fclose(file) == 0);

    read_concurrency_uuid(metadata_path, uuid, sizeof(uuid));
    assert(memcmp(header, "MYLSHM01", 8U) == 0);
    assert(read_le32(header + 8U) == 1U);
    assert(read_le32(header + 12U) == 1U);
    assert(read_le32(header + 16U) == MYLITE_TEST_CONCURRENCY_SHM_HEADER_SIZE);
    assert(read_le32(header + 20U) == 0x01020304U);
    assert(read_le32(header + 24U) == 0U);
    assert(read_le32(header + 28U) == 1U);
    assert(read_le64(header + 32U) == (uint64_t)shm_size);
    assert(read_le64(header + 40U) == 0U);
    assert(read_le64(header + 48U) == 0U);
    assert(read_le32(header + 56U) == 0U);
    assert(read_le32(header + 60U) == 0U);
    assert(memcmp(header + 64U, uuid, 36U) == 0);
    for (size_t index = 100U; index < MYLITE_TEST_CONCURRENCY_SHM_HEADER_SIZE; ++index) {
        assert(header[index] == 0U);
    }
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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
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

#include "ownerless_lock_table.h"
#include "ownerless_probe.h"
#include "ownerless_wait.h"

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_PAGE_SIZE 4096
#define MYLITE_TEST_WAIT_TIMEOUT_MS 5000U
#define MYLITE_TEST_LOCK_TABLE_ENTRY_COUNT 4U
#define MYLITE_TEST_LOCK_HASH 0xAABBCCDDEEFF0011ULL

typedef struct byte_range_lock {
    off_t start;
    off_t length;
} byte_range_lock;

static void test_mmap_shared_visibility_across_processes(void);
static void test_fcntl_byte_range_lock_conflict(void);
static void test_fcntl_byte_range_lock_release_on_process_exit(void);
static void test_mmap_grow_and_remap(void);
static void test_wait_backend_wakes_across_processes(void);
static void test_wait_backend_times_out_without_change(void);
static void test_platform_probe_records_required_primitives(void);
static void test_lock_table_waits_for_conflicting_owner_release(void);
static void test_lock_table_conflicting_owner_times_out(void);
static void set_write_lock(int fd, byte_range_lock range);
static int try_write_lock(int fd, byte_range_lock range);
static void unlock_range(int fd, byte_range_lock range);
static int open_file(const char *path);
static void truncate_file(int fd, off_t size);
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
    test_platform_probe_records_required_primitives();
    test_lock_table_waits_for_conflicting_owner_release();
    test_lock_table_conflicting_owner_times_out();
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
        wait_result = mylite_ownerless_wait_for_change(
            child_word,
            0U,
            MYLITE_TEST_WAIT_TIMEOUT_MS
        );
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

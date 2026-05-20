#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32

typedef struct open_database_paths {
    const char *database_path;
    const char *runtime_root;
} open_database_paths;

typedef struct child_lock_pipes {
    int ready_write_fd;
    int release_read_fd;
} child_lock_pipes;

typedef enum child_close_mode {
    CHILD_EXIT_UNCLEANLY = 0,
    CHILD_CLOSE_CLEANLY = 1,
} child_close_mode;

static void test_cross_process_open_fails_while_directory_locked(void);
static void test_stale_lock_file_does_not_block_reopen(void);
static void initialize_database(open_database_paths paths);
static void hold_database_until_released(
    open_database_paths paths,
    child_lock_pipes pipes,
    child_close_mode close_mode
);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void assert_open_busy(open_database_paths paths);
static void exec_ok(mylite_db *db, const char *sql);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void signal_pipe(int pipe_fd);
static void wait_for_pipe(int pipe_fd);
static void wait_for_child(pid_t child);
static void assert_database_closed_layout(const char *database_path);
static int is_directory(const char *path);
static int is_directory_empty(const char *path);
static int path_exists(const char *path);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

int main(void) {
    test_cross_process_open_fails_while_directory_locked();
    test_stale_lock_file_does_not_block_reopen();
    return 0;
}

static void test_cross_process_open_fails_while_directory_locked(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "locked.mylite");
    char *run_path = path_join(database_path, "run");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t child;
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_database_until_released(
            paths,
            (child_lock_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            },
            CHILD_CLOSE_CLEANLY
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);
    assert(is_directory(run_path));
    assert_open_busy(paths);
    assert(is_directory(run_path));
    signal_pipe(release_pipe[1]);
    wait_for_child(child);

    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(mylite_close(db) == MYLITE_OK);

    free(run_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void test_stale_lock_file_does_not_block_reopen(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "stale-lock.mylite");
    char *lock_path = path_join(database_path, "mylite.lock");
    char *run_path = path_join(database_path, "run");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t child;
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_database_until_released(
            paths,
            (child_lock_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            },
            CHILD_EXIT_UNCLEANLY
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);
    assert(path_exists(lock_path));
    assert(is_directory(run_path));
    signal_pipe(release_pipe[1]);
    wait_for_child(child);
    assert(path_exists(lock_path));
    assert(is_directory(run_path));

    db = open_database(paths, MYLITE_OPEN_READWRITE);
    assert(db != NULL);
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(database_path);
    assert(is_directory_empty(runtime_root));

    free(run_path);
    free(lock_path);
    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void initialize_database(open_database_paths paths) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, "CREATE TABLE app.lock_probe (id INT NOT NULL PRIMARY KEY) ENGINE=MyISAM");
    assert(mylite_close(db) == MYLITE_OK);
    assert_database_closed_layout(paths.database_path);
}

static void hold_database_until_released(
    open_database_paths paths,
    child_lock_pipes pipes,
    child_close_mode close_mode
) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE);

    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    if (close_mode == CHILD_CLOSE_CLEANLY) {
        assert(mylite_close(db) == MYLITE_OK);
    }
    _exit(0);
}

static mylite_db *open_database(open_database_paths paths, unsigned flags) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = paths.runtime_root,
    };
    mylite_db *db = NULL;

    assert(mylite_open(paths.database_path, &db, flags, &config) == MYLITE_OK);
    assert(db != NULL);
    return db;
}

static void assert_open_busy(open_database_paths paths) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = paths.runtime_root,
    };
    mylite_db *db = NULL;

    assert(mylite_open(paths.database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_BUSY);
    assert(db == NULL);
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-locking.XXXXXX";
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

static void assert_database_closed_layout(const char *database_path) {
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *lock_path = path_join(database_path, "mylite.lock");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");

    assert(is_directory(database_path));
    assert(path_exists(metadata_path));
    assert(path_exists(lock_path));
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(!path_exists(run_path));

    free(run_path);
    free(tmp_path);
    free(data_path);
    free(lock_path);
    free(metadata_path);
}

static int is_directory(const char *path) {
    struct stat path_stat;

    return stat(path, &path_stat) == 0 && S_ISDIR(path_stat.st_mode);
}

static int is_directory_empty(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;

    assert(directory != NULL);
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            assert(closedir(directory) == 0);
            return 0;
        }
    }
    assert(errno == 0);
    assert(closedir(directory) == 0);
    return 1;
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

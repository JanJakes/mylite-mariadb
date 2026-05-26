#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_UNSAFE_LOCK_BYPASS_ENV "MYLITE_UNSAFE_DISABLE_DIRECTORY_LOCK_FOR_TESTS"

typedef struct open_database_paths {
    const char *database_path;
    const char *runtime_root;
} open_database_paths;

typedef struct child_pipes {
    int ready_write_fd;
    int release_read_fd;
} child_pipes;

static void test_second_process_cannot_open_without_directory_lock(void);
static void initialize_database(open_database_paths paths);
static void hold_database_with_unsafe_lock_bypass(open_database_paths paths, child_pipes pipes);
static mylite_db *open_database(open_database_paths paths, unsigned flags);
static void assert_unsafe_second_open_fails(open_database_paths paths);
static void run_unsafe_second_open(open_database_paths paths);
static void exec_ok(mylite_db *db, const char *sql);
static void sleep_milliseconds(unsigned milliseconds);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void signal_pipe(int pipe_fd);
static void wait_for_pipe(int pipe_fd);
static void wait_for_child(pid_t child);
static int path_exists(const char *path);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

int main(void) {
    test_second_process_cannot_open_without_directory_lock();
    return 0;
}

static void test_second_process_cannot_open_without_directory_lock(void) {
    char *root = make_temp_root();
    char *runtime_root = path_join(root, "runtime");
    char *database_path = path_join(root, "negative-proof.mylite");
    open_database_paths paths = {.database_path = database_path, .runtime_root = runtime_root};
    int ready_pipe[2];
    int release_pipe[2];
    pid_t child;

    assert(mkdir(runtime_root, 0700) == 0);
    initialize_database(paths);
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        hold_database_with_unsafe_lock_bypass(
            paths,
            (child_pipes){
                .ready_write_fd = ready_pipe[1],
                .release_read_fd = release_pipe[0],
            }
        );
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    wait_for_pipe(ready_pipe[0]);
    assert_unsafe_second_open_fails(paths);
    signal_pipe(release_pipe[1]);
    wait_for_child(child);

    free(database_path);
    free(runtime_root);
    remove_tree(root);
    free(root);
}

static void initialize_database(open_database_paths paths) {
    mylite_db *db = open_database(paths, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE);

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, "CREATE TABLE app.innodb_probe (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB");
    exec_ok(db, "CREATE TABLE app.myisam_probe (id INT NOT NULL PRIMARY KEY) ENGINE=MyISAM");
    exec_ok(db, "CREATE TABLE app.aria_probe (id INT NOT NULL PRIMARY KEY) ENGINE=Aria");
    assert(mylite_close(db) == MYLITE_OK);
}

static void hold_database_with_unsafe_lock_bypass(open_database_paths paths, child_pipes pipes) {
    mylite_db *db;

    assert(setenv(MYLITE_UNSAFE_LOCK_BYPASS_ENV, "1", 1) == 0);
    db = open_database(paths, MYLITE_OPEN_READWRITE);
    signal_pipe(pipes.ready_write_fd);
    wait_for_pipe(pipes.release_read_fd);
    assert(mylite_close(db) == MYLITE_OK);
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

static void assert_unsafe_second_open_fails(open_database_paths paths) {
    pid_t child = fork();

    assert(child >= 0);
    if (child == 0) {
        run_unsafe_second_open(paths);
    }

    for (int attempt = 0; attempt < 50; ++attempt) {
        int child_status = 0;
        const pid_t wait_result = waitpid(child, &child_status, WNOHANG);

        assert(wait_result >= 0);
        if (wait_result == child) {
            assert(WIFEXITED(child_status));
            assert(WEXITSTATUS(child_status) == 0);
            return;
        }
        sleep_milliseconds(100U);
    }

    assert(kill(child, SIGKILL) == 0);
    {
        int child_status = 0;

        assert(waitpid(child, &child_status, 0) == child);
        assert(WIFSIGNALED(child_status));
    }
}

static void run_unsafe_second_open(open_database_paths paths) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = paths.runtime_root,
    };
    mylite_db *db = NULL;

    assert(setenv(MYLITE_UNSAFE_LOCK_BYPASS_ENV, "1", 1) == 0);
    if (mylite_open(paths.database_path, &db, MYLITE_OPEN_READWRITE, &config) == MYLITE_OK) {
        assert(db != NULL);
        assert(mylite_close(db) == MYLITE_OK);
        _exit(2);
    }
    assert(db == NULL);
    _exit(0);
}

static void exec_ok(mylite_db *db, const char *sql) {
    char *errmsg = NULL;

    assert(mylite_exec(db, sql, NULL, NULL, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
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
    char template_path[] = "/tmp/mylite-negative-proof.XXXXXX";
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

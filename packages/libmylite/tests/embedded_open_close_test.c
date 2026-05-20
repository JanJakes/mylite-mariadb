#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void test_open_close_repeatedly(void);
static void test_memory_path_open_close(void);
static void test_readonly_open_fails(void);
static void test_two_handles_share_runtime(void);
static void test_second_database_fails_while_runtime_open(void);
static void test_directory_suffix_is_not_enforced(void);
static void test_no_defaults_ignores_ambient_option_files(void);
static void test_missing_file_without_create_fails(void);
static void test_existing_file_path_fails(void);
static void test_exclusive_existing_directory_fails(void);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static mylite_open_config open_config(const char *temp_directory);
static void assert_open_database_layout(const char *database_path);
static void assert_closed_database_layout(const char *database_path);
static int is_directory_empty(const char *path);
static int is_directory(const char *path);
static int path_exists(const char *path);
static void write_file(const char *path, const char *contents);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

int main(void) {
    test_open_close_repeatedly();
    test_memory_path_open_close();
    test_readonly_open_fails();
    test_two_handles_share_runtime();
    test_second_database_fails_while_runtime_open();
    test_directory_suffix_is_not_enforced();
    test_no_defaults_ignores_ambient_option_files();
    test_missing_file_without_create_fails();
    test_existing_file_path_fails();
    test_exclusive_existing_directory_fails();
    return 0;
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
    write_file(defaults, "[server]\ndatadir=/path/that/must/not/be/read\nunknown_mylite_probe=1\n");
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
    write_file(database_path, "");

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
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");
    char *plugin_path = path_join(run_path, "plugins");

    assert(path_exists(metadata_path));
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(is_directory(run_path));
    assert(is_directory(plugin_path));

    free(plugin_path);
    free(run_path);
    free(tmp_path);
    free(data_path);
    free(metadata_path);
}

static void assert_closed_database_layout(const char *database_path) {
    char *metadata_path = path_join(database_path, "mylite.meta");
    char *data_path = path_join(database_path, "datadir");
    char *tmp_path = path_join(database_path, "tmp");
    char *run_path = path_join(database_path, "run");

    assert(path_exists(metadata_path));
    assert(is_directory(data_path));
    assert(is_directory(tmp_path));
    assert(!path_exists(run_path));

    free(run_path);
    free(tmp_path);
    free(data_path);
    free(metadata_path);
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
    assert(lstat(path, &path_stat) == 0);
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

static void write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);
}

static void remove_tree(const char *path) {
    remove_tree_entry(path);
}

static void remove_tree_entry(const char *path) {
    struct stat path_stat;
    assert(lstat(path, &path_stat) == 0);

    if (S_ISDIR(path_stat.st_mode)) {
        DIR *directory = opendir(path);
        assert(directory != NULL);

        for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char *child = path_join(path, entry->d_name);
            remove_tree_entry(child);
            free(child);
        }

        assert(closedir(directory) == 0);
        assert(rmdir(path) == 0);
        return;
    }

    assert(unlink(path) == 0);
}

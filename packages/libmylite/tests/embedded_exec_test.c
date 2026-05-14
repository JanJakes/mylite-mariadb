#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct select_context {
    int rows;
} select_context;

static void test_select_callback(void);
static void test_callback_abort(void);
static void test_syntax_error_diagnostics(void);
static int select_callback(void *ctx, int column_count, char **values, char **column_names);
static int abort_callback(void *ctx, int column_count, char **values, char **column_names);
static mylite_db *open_database(const char *root, char **filename);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static int is_directory_empty(const char *path);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

int main(void) {
    test_select_callback();
    test_callback_abort();
    test_syntax_error_diagnostics();
    return 0;
}

static void test_select_callback(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    select_context ctx = {.rows = 0};

    assert(
        mylite_exec(db, "SELECT 1 AS one, NULL AS empty", select_callback, &ctx, NULL) == MYLITE_OK
    );
    assert(ctx.rows == 1);
    assert(mylite_changes(db) == 0);
    assert(mylite_last_insert_id(db) == 0U);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_callback_abort(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    int callback_count = 0;
    char *errmsg = NULL;

    assert(mylite_exec(db, "SELECT 1", abort_callback, &callback_count, &errmsg) == MYLITE_ERROR);
    assert(callback_count == 1);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "callback") != NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    mylite_free(errmsg);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_syntax_error_diagnostics(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    char *errmsg = NULL;

    assert(mylite_exec(db, "SELEC broken", NULL, NULL, &errmsg) == MYLITE_ERROR);
    assert(errmsg != NULL);
    assert(strstr(errmsg, "syntax") != NULL || strstr(errmsg, "SQL") != NULL);
    assert(mylite_errcode(db) == MYLITE_ERROR);
    assert(mylite_mariadb_errno(db) != 0U);
    assert(strcmp(mylite_sqlstate(db), "00000") != 0);
    mylite_free(errmsg);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static int select_callback(void *ctx, int column_count, char **values, char **column_names) {
    select_context *select_ctx = (select_context *)ctx;
    assert(column_count == 2);
    assert(strcmp(column_names[0], "one") == 0);
    assert(strcmp(column_names[1], "empty") == 0);
    assert(values[0] != NULL);
    assert(strcmp(values[0], "1") == 0);
    assert(values[1] == NULL);
    ++select_ctx->rows;
    return 0;
}

static int abort_callback(void *ctx, int column_count, char **values, char **column_names) {
    int *callback_count = (int *)ctx;
    (void)column_count;
    (void)values;
    (void)column_names;
    ++*callback_count;
    return 1;
}

static mylite_db *open_database(const char *root, char **filename) {
    char *runtime_root = path_join(root, "runtime");
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = runtime_root,
    };
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    *filename = path_join(root, "exec.mylite");
    assert(
        mylite_open_v2(*filename, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    free(runtime_root);
    return db;
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-exec.XXXXXX";
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

static void remove_tree(const char *path) {
    char *runtime_root = path_join(path, "runtime");
    assert(is_directory_empty(runtime_root));
    free(runtime_root);
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

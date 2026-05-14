#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct engine_context {
    int rows;
    int found_mylite;
    int supported_mylite;
} engine_context;

static void test_show_engines_reports_mylite(void);
static int engine_callback(void *ctx, int column_count, char **values, char **column_names);
static mylite_db *open_database(const char *root, char **filename);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static int is_directory_empty(const char *path);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

int main(void) {
    test_show_engines_reports_mylite();
    return 0;
}

static void test_show_engines_reports_mylite(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    engine_context ctx = {0};
    char *errmsg = NULL;

    assert(mylite_exec(db, "SHOW ENGINES", engine_callback, &ctx, &errmsg) == MYLITE_OK);
    assert(errmsg == NULL);
    assert(ctx.rows > 0);
    assert(ctx.found_mylite);
    assert(ctx.supported_mylite);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static int engine_callback(void *ctx, int column_count, char **values, char **column_names) {
    engine_context *engine_ctx = (engine_context *)ctx;
    (void)column_names;

    assert(column_count >= 2);
    ++engine_ctx->rows;

    if (values[0] == NULL || strcmp(values[0], "MYLITE") != 0) {
        return 0;
    }

    engine_ctx->found_mylite = 1;
    if (values[1] != NULL && strcmp(values[1], "YES") == 0) {
        engine_ctx->supported_mylite = 1;
    }

    return 0;
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
    *filename = path_join(root, "storage-engine.mylite");
    assert(
        mylite_open_v2(*filename, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    free(runtime_root);
    return db;
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-storage-engine.XXXXXX";
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

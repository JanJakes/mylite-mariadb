#include <mylite/mylite.h>

#include "mylite_ownerless_mdl_hooks.h"

#include <assert.h>
#include <ftw.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32

typedef struct mdl_hook_counts {
    unsigned app_posts_acquires;
    unsigned app_posts_releases;
    unsigned app_posts_active;
    unsigned app_posts_shared_acquires;
    unsigned app_posts_upgradable_acquires;
    unsigned app_posts_exclusive_acquires;
    unsigned max_app_posts_active;
} mdl_hook_counts;

static void test_mdl_hooks_balance_table_tickets(void);
static int acquire_mdl_hook(
    const mylite_ownerless_mdl_key_view *key,
    double lock_wait_timeout,
    void *ctx
);
static void release_mdl_hook(const mylite_ownerless_mdl_key_view *key, void *ctx);
static int is_app_posts_table_key(const mylite_ownerless_mdl_key_view *key);
static int string_part_equals(const char *value, unsigned length, const char *expected);
static mylite_db *open_database(const char *root, char **database_path);
static void exec_ok(mylite_db *db, const char *sql);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

int main(void) {
    test_mdl_hooks_balance_table_tickets();
    return 0;
}

static void test_mdl_hooks_balance_table_tickets(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    mdl_hook_counts counts = {0};
    mylite_db *db;

    db = open_database(root, &database_path);
    mylite_ownerless_mdl_set_hooks(acquire_mdl_hook, release_mdl_hook, &counts);
    assert(mylite_ownerless_mdl_has_hooks());

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(db, "CREATE TABLE app.posts (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB");
    exec_ok(db, "INSERT INTO app.posts VALUES (1)");
    exec_ok(db, "SELECT id FROM app.posts WHERE id = 1");
    exec_ok(db, "ALTER TABLE app.posts ADD COLUMN title VARCHAR(64) NULL");
    exec_ok(db, "DROP TABLE app.posts");
    assert(mylite_close(db) == MYLITE_OK);

    assert(counts.app_posts_acquires > 0U);
    assert(counts.app_posts_acquires == counts.app_posts_releases);
    assert(counts.app_posts_shared_acquires > 0U);
    assert(counts.app_posts_upgradable_acquires > 0U);
    assert(counts.app_posts_exclusive_acquires > 0U);
    assert(counts.app_posts_active == 0U);
    assert(counts.max_app_posts_active > 0U);

    mylite_ownerless_mdl_reset_hooks();
    assert(!mylite_ownerless_mdl_has_hooks());

    free(database_path);
    remove_tree(root);
    free(root);
}

static int acquire_mdl_hook(
    const mylite_ownerless_mdl_key_view *key,
    double lock_wait_timeout,
    void *ctx
) {
    mdl_hook_counts *counts = (mdl_hook_counts *)ctx;

    (void)lock_wait_timeout;
    if (is_app_posts_table_key(key)) {
        ++counts->app_posts_acquires;
        ++counts->app_posts_active;
        if (key->ownerless_mode == MYLITE_OWNERLESS_MDL_MODE_SHARED) {
            ++counts->app_posts_shared_acquires;
        } else if (key->ownerless_mode == MYLITE_OWNERLESS_MDL_MODE_UPGRADABLE) {
            ++counts->app_posts_upgradable_acquires;
        } else if (key->ownerless_mode == MYLITE_OWNERLESS_MDL_MODE_EXCLUSIVE) {
            ++counts->app_posts_exclusive_acquires;
        }
        if (counts->app_posts_active > counts->max_app_posts_active) {
            counts->max_app_posts_active = counts->app_posts_active;
        }
    }
    return MYLITE_OWNERLESS_MDL_OK;
}

static void release_mdl_hook(const mylite_ownerless_mdl_key_view *key, void *ctx) {
    mdl_hook_counts *counts = (mdl_hook_counts *)ctx;

    if (is_app_posts_table_key(key)) {
        assert(counts->app_posts_active > 0U);
        --counts->app_posts_active;
        ++counts->app_posts_releases;
    }
}

static int is_app_posts_table_key(const mylite_ownerless_mdl_key_view *key) {
    return key != NULL && key->namespace_id == 2U &&
           string_part_equals(key->database_name, key->database_name_length, "app") &&
           string_part_equals(key->object_name, key->object_name_length, "posts");
}

static int string_part_equals(const char *value, unsigned length, const char *expected) {
    const size_t expected_length = strlen(expected);

    return value != NULL && length == expected_length &&
           memcmp(value, expected, expected_length) == 0;
}

static mylite_db *open_database(const char *root, char **database_path) {
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
    *database_path = path_join(root, "mdl-hooks.mylite");
    assert(
        mylite_open(*database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    free(runtime_root);
    return db;
}

static void exec_ok(mylite_db *db, const char *sql) {
    assert(mylite_exec(db, sql, NULL, NULL, NULL) == MYLITE_OK);
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-mdl-hooks.XXXXXX";
    char *root = mkdtemp(template_path);
    char *copy;

    assert(root != NULL);
    copy = strdup(root);
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

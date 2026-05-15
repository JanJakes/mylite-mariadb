#include <mylite/mylite.h>

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_direct_warning_rows(void);
static void test_prepared_non_result_warnings(void);
static void test_prepared_result_warnings_after_drain(void);
static void test_warning_api_validation(void);
static mylite_stmt *prepare_statement(mylite_db *db, const char *sql);
static mylite_db *open_database(const char *root, char **filename);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static int is_directory_empty(const char *path);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

int main(void) {
    test_direct_warning_rows();
    test_prepared_non_result_warnings();
    test_prepared_result_warnings_after_drain();
    test_warning_api_validation();
    return 0;
}

static void test_direct_warning_rows(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_warning_level level = MYLITE_WARNING_NOTE;
    unsigned code = 0;
    const char *message = NULL;

    assert(
        mylite_exec(db, "SELECT CAST('not-a-number' AS UNSIGNED)", NULL, NULL, NULL) == MYLITE_OK
    );
    assert(mylite_warning_count(db) >= 1U);
    assert(mylite_warning(db, 0U, &level, &code, &message) == MYLITE_OK);
    assert(level == MYLITE_WARNING_WARNING);
    assert(code != 0U);
    assert(message != NULL);
    assert(strstr(message, "not-a-number") != NULL);

    assert(
        mylite_warning(db, mylite_warning_count(db), &level, &code, &message) == MYLITE_NOTFOUND
    );
    assert(mylite_exec(db, "SELECT 1", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_warning_count(db) == 0U);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_prepared_non_result_warnings(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *stmt = NULL;
    mylite_warning_level level = MYLITE_WARNING_NOTE;
    unsigned code = 0;
    const char *message = NULL;

    assert(mylite_exec(db, "CREATE DATABASE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(mylite_exec(db, "USE app", NULL, NULL, NULL) == MYLITE_OK);
    assert(
        mylite_exec(
            db,
            "CREATE TEMPORARY TABLE warning_values (value VARCHAR(2))",
            NULL,
            NULL,
            NULL
        ) == MYLITE_OK
    );

    stmt = prepare_statement(db, "INSERT IGNORE INTO warning_values(value) VALUES (?)");
    assert(
        mylite_bind_text(stmt, 1U, "abcd", MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT) == MYLITE_OK
    );
    int step_result = mylite_step(stmt);
    if (step_result != MYLITE_DONE) {
        fprintf(
            stderr,
            "prepared warning insert failed: result=%d err=%d maria=%u sqlstate=%s message=%s\n",
            step_result,
            mylite_errcode(db),
            mylite_mariadb_errno(db),
            mylite_sqlstate(db),
            mylite_errmsg(db)
        );
    }
    assert(step_result == MYLITE_DONE);
    assert(mylite_warning_count(db) >= 1U);
    assert(mylite_warning(db, 0U, &level, &code, &message) == MYLITE_OK);
    assert(level == MYLITE_WARNING_WARNING);
    assert(code != 0U);
    assert(message != NULL);
    assert(strstr(message, "Data truncated") != NULL || strstr(message, "truncated") != NULL);
    assert(mylite_finalize(stmt) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_prepared_result_warnings_after_drain(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_stmt *stmt = prepare_statement(db, "SELECT CAST(? AS UNSIGNED)");
    mylite_warning_level level = MYLITE_WARNING_NOTE;
    unsigned code = 0;
    const char *message = NULL;

    assert(
        mylite_bind_text(stmt, 1U, "bad-int", MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT) == MYLITE_OK
    );
    assert(mylite_warning_count(db) == 0U);
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_uint64(stmt, 0U) == 0U);
    assert(mylite_warning_count(db) == 0U);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_warning_count(db) >= 1U);
    assert(mylite_warning(db, 0U, &level, &code, &message) == MYLITE_OK);
    assert(level == MYLITE_WARNING_WARNING);
    assert(code != 0U);
    assert(message != NULL);
    assert(strstr(message, "bad-int") != NULL);
    assert(mylite_reset(stmt) == MYLITE_OK);

    assert(mylite_bind_text(stmt, 1U, "12", MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT) == MYLITE_OK);
    assert(mylite_step(stmt) == MYLITE_ROW);
    assert(mylite_column_uint64(stmt, 0U) == 12U);
    assert(mylite_step(stmt) == MYLITE_DONE);
    assert(mylite_warning_count(db) == 0U);
    assert(mylite_finalize(stmt) == MYLITE_OK);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static void test_warning_api_validation(void) {
    char *root = make_temp_root();
    char *filename = NULL;
    mylite_db *db = open_database(root, &filename);
    mylite_warning_level level = MYLITE_WARNING_NOTE;
    unsigned code = 0;
    const char *message = NULL;

    assert(mylite_warning(db, 0U, NULL, &code, &message) == MYLITE_MISUSE);
    assert(mylite_warning(db, 0U, &level, NULL, &message) == MYLITE_MISUSE);
    assert(mylite_warning(db, 0U, &level, &code, NULL) == MYLITE_MISUSE);
    assert(mylite_warning(db, 0U, &level, &code, &message) == MYLITE_NOTFOUND);

    assert(mylite_close(db) == MYLITE_OK);
    free(filename);
    remove_tree(root);
    free(root);
}

static mylite_stmt *prepare_statement(mylite_db *db, const char *sql) {
    mylite_stmt *stmt = NULL;
    assert(mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, NULL) == MYLITE_OK);
    assert(stmt != NULL);
    return stmt;
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
    *filename = path_join(root, "warnings.mylite");
    assert(
        mylite_open_v2(*filename, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    free(runtime_root);
    return db;
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-warnings.XXXXXX";
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

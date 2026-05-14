#include "storage_format.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mylite/storage.h>

static void test_capabilities(void);
static void test_create_empty_database(void);
static void test_missing_file(void);
static void test_rejects_bad_magic(void);
static void test_rejects_bad_checksum(void);
static void test_rejects_newer_format_version(void);
static void test_rejects_bad_catalog_root(void);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static long long file_size(const char *path);
static void flip_file_byte(const char *path, long offset);
static void write_header_format_version(const char *path, unsigned value);

int main(void) {
    test_capabilities();
    test_create_empty_database();
    test_missing_file();
    test_rejects_bad_magic();
    test_rejects_bad_checksum();
    test_rejects_newer_format_version();
    test_rejects_bad_catalog_root();
    return 0;
}

static void test_capabilities(void) {
    const mylite_storage_capabilities capabilities = mylite_storage_get_capabilities();

    assert(strcmp(mylite_storage_engine_name(), MYLITE_STORAGE_ENGINE_NAME) == 0);
    assert(capabilities.size == sizeof(capabilities));
    assert(capabilities.format_version == MYLITE_STORAGE_FORMAT_VERSION);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_FILE_HEADER) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_EMPTY_CATALOG) != 0U);
}

static void test_create_empty_database(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "empty.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_ERROR);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.size == sizeof(header));
    assert(header.format_version == MYLITE_STORAGE_FORMAT_VERSION);
    assert(header.header_version == MYLITE_STORAGE_FORMAT_HEADER_VERSION);
    assert(header.page_size == MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    assert(header.checksum_algorithm == MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64);
    assert(header.catalog_root_page == MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID);
    assert(header.catalog_generation == MYLITE_STORAGE_FORMAT_EMPTY_CATALOG_GENERATION);
    assert(header.free_list_root_page == 0ULL);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT);
    assert(
        file_size(filename) ==
        (long long)(MYLITE_STORAGE_FORMAT_PAGE_SIZE * MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT)
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_missing_file(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "missing.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_NOTFOUND);

    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_bad_magic(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "bad-magic.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    flip_file_byte(filename, MYLITE_STORAGE_FORMAT_HEADER_MAGIC_OFFSET);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_CORRUPT);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_bad_checksum(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "bad-checksum.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    flip_file_byte(filename, MYLITE_STORAGE_FORMAT_HEADER_FLAGS_OFFSET);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_CORRUPT);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_newer_format_version(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "newer-version.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    write_header_format_version(filename, MYLITE_STORAGE_FORMAT_VERSION + 1U);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_UNSUPPORTED);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_bad_catalog_root(void) {
    char *root = make_temp_root();
    char *filename = path_join(root, "bad-catalog.mylite");
    mylite_storage_header header = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    flip_file_byte(
        filename,
        (long)MYLITE_STORAGE_FORMAT_PAGE_SIZE + MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_CORRUPT);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-storage-test.XXXXXX";
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

static long long file_size(const char *path) {
    struct stat path_stat;
    assert(stat(path, &path_stat) == 0);
    return (long long)path_stat.st_size;
}

static void flip_file_byte(const char *path, long offset) {
    FILE *file = fopen(path, "r+b");
    assert(file != NULL);
    assert(fseek(file, offset, SEEK_SET) == 0);
    const int byte = fgetc(file);
    assert(byte != EOF);
    assert(fseek(file, offset, SEEK_SET) == 0);
    assert(fputc(byte ^ 0x01, file) != EOF);
    assert(fclose(file) == 0);
}

static void write_header_format_version(const char *path, unsigned value) {
    FILE *file = fopen(path, "r+b");
    assert(file != NULL);
    assert(fseek(file, (long)MYLITE_STORAGE_FORMAT_HEADER_FORMAT_VERSION_OFFSET, SEEK_SET) == 0);
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        assert(fputc((int)((value >> (unsigned)(i * CHAR_BIT)) & UINT8_MAX), file) != EOF);
    }
    assert(fclose(file) == 0);
}

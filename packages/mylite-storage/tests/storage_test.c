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
static void test_store_and_read_table_definition(void);
static void test_store_large_table_definition(void);
static void test_append_and_read_rows(void);
static void test_append_and_read_large_row_payload(void);
static void test_autoincrement_state(void);
static void test_rejects_corrupt_row_page(void);
static void test_rejects_corrupt_row_payload_page(void);
static void test_rejects_corrupt_autoincrement_page(void);
static void test_drop_table_definition(void);
static void test_rename_table_definition(void);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static long long file_size(const char *path);
static void flip_file_byte(const char *path, long offset);
static void write_header_format_version(const char *path, unsigned value);
static int collect_table(void *ctx, const char *schema_name, const char *table_name);

typedef struct table_list_capture {
    const char *schema_name;
    const char *table_name;
    unsigned count;
} table_list_capture;

int main(void) {
    test_capabilities();
    test_create_empty_database();
    test_missing_file();
    test_rejects_bad_magic();
    test_rejects_bad_checksum();
    test_rejects_newer_format_version();
    test_rejects_bad_catalog_root();
    test_store_and_read_table_definition();
    test_store_large_table_definition();
    test_append_and_read_rows();
    test_append_and_read_large_row_payload();
    test_autoincrement_state();
    test_rejects_corrupt_row_page();
    test_rejects_corrupt_row_payload_page();
    test_rejects_corrupt_autoincrement_page();
    test_drop_table_definition();
    test_rename_table_definition();
    return 0;
}

static void test_capabilities(void) {
    const mylite_storage_capabilities capabilities = mylite_storage_get_capabilities();

    assert(strcmp(mylite_storage_engine_name(), MYLITE_STORAGE_ENGINE_NAME) == 0);
    assert(capabilities.size == sizeof(capabilities));
    assert(capabilities.format_version == MYLITE_STORAGE_FORMAT_VERSION);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_FILE_HEADER) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_EMPTY_CATALOG) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_TABLE_DEFINITIONS) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_TABLE_ROWS) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_AUTOINCREMENT) != 0U);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_BLOB_TEXT_ROWS) != 0U);
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

static void test_store_and_read_table_definition(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "table-definition.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;
    mylite_storage_table_metadata metadata = {
        .size = sizeof(metadata),
    };
    table_list_capture capture = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_ERROR
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.catalog_generation == MYLITE_STORAGE_FORMAT_EMPTY_CATALOG_GENERATION + 1ULL);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 1ULL);
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "missing") == MYLITE_STORAGE_NOTFOUND);
    assert(
        mylite_storage_read_table_definition(
            filename,
            "app",
            "posts",
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == sizeof(definition));
    assert(memcmp(stored_definition, definition, sizeof(definition)) == 0);
    mylite_storage_free(stored_definition);
    assert(
        mylite_storage_read_table_metadata(filename, "app", "posts", &metadata) == MYLITE_STORAGE_OK
    );
    assert(strcmp(metadata.requested_engine_name, "MYLITE") == 0);
    assert(strcmp(metadata.effective_engine_name, "MYLITE") == 0);
    mylite_storage_free(metadata.requested_engine_name);
    mylite_storage_free(metadata.effective_engine_name);

    assert(
        mylite_storage_list_tables(filename, "app", collect_table, &capture) == MYLITE_STORAGE_OK
    );
    assert(capture.count == 1U);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_store_large_table_definition(void) {
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    const size_t definition_size = (payload_capacity * 2U) + 17U;
    char *root = make_temp_root();
    char *filename = path_join(root, "large-table-definition.mylite");
    unsigned char *definition = (unsigned char *)malloc(definition_size);
    assert(definition != NULL);
    for (size_t i = 0U; i < definition_size; ++i) {
        definition[i] = (unsigned char)(i % UINT8_MAX);
    }

    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "large_posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = definition_size,
    };
    mylite_storage_header header = {0};
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 3ULL);
    assert(
        mylite_storage_read_table_definition(
            filename,
            "app",
            "large_posts",
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == definition_size);
    assert(memcmp(stored_definition, definition, definition_size) == 0);

    mylite_storage_free(stored_definition);
    free(definition);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_append_and_read_rows(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char post_row_1[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    static const unsigned char post_row_2[] = {0x00U, 0x02U, 'd', 'e', 'f'};
    static const unsigned char comment_row[] = {0x00U, 0x03U, 'x', 'y', 'z'};
    char *root = make_temp_root();
    char *filename = path_join(root, "table-rows.mylite");
    mylite_storage_table_definition posts_definition = {
        .size = sizeof(posts_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_table_definition comments_definition = {
        .size = sizeof(comments_definition),
        .schema_name = "app",
        .table_name = "comments",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &posts_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(filename, &comments_definition) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "posts", post_row_1, sizeof(post_row_1)) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "comments", comment_row, sizeof(comment_row)) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "posts", post_row_2, sizeof(post_row_2)) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 5ULL);

    assert(mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 2ULL);
    assert(mylite_storage_count_rows(filename, "app", "comments", &row_count) == MYLITE_STORAGE_OK);
    assert(row_count == 1ULL);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_size == sizeof(post_row_1));
    assert(rows.row_count == 2U);
    assert(rows.row_bytes == sizeof(post_row_1) + sizeof(post_row_2));
    assert(rows.row_offsets[0] == 0U);
    assert(rows.row_sizes[0] == sizeof(post_row_1));
    assert(rows.row_offsets[1] == sizeof(post_row_1));
    assert(rows.row_sizes[1] == sizeof(post_row_2));
    assert(memcmp(rows.rows, post_row_1, sizeof(post_row_1)) == 0);
    assert(memcmp(rows.rows + rows.row_size, post_row_2, sizeof(post_row_2)) == 0);

    mylite_storage_free_rowset(&rows);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_append_and_read_large_row_payload(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char small_row[] = {0x00U, 0x01U, 's'};
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    const size_t large_row_size = (payload_capacity * 2U) + 17U;
    unsigned char *large_row = (unsigned char *)malloc(large_row_size);
    assert(large_row != NULL);
    for (size_t i = 0U; i < large_row_size; ++i) {
        large_row[i] = (unsigned char)(i % UINT8_MAX);
    }

    char *root = make_temp_root();
    char *filename = path_join(root, "large-row-payload.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "payloads",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "payloads", small_row, sizeof(small_row)) ==
        MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "payloads", large_row, large_row_size) ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 6ULL);
    assert(mylite_storage_read_rows(filename, "app", "payloads", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_size == 0U);
    assert(rows.row_count == 2U);
    assert(rows.row_bytes == sizeof(small_row) + large_row_size);
    assert(rows.row_offsets[0] == 0U);
    assert(rows.row_sizes[0] == sizeof(small_row));
    assert(rows.row_offsets[1] == sizeof(small_row));
    assert(rows.row_sizes[1] == large_row_size);
    assert(memcmp(rows.rows + rows.row_offsets[0], small_row, sizeof(small_row)) == 0);
    assert(memcmp(rows.rows + rows.row_offsets[1], large_row, large_row_size) == 0);

    mylite_storage_free_rowset(&rows);
    free(large_row);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_autoincrement_state(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "autoincrement-state.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_header header = {0};
    unsigned long long next_value = 0ULL;
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_read_auto_increment(filename, "app", "posts", &next_value) ==
        MYLITE_STORAGE_OK
    );
    assert(next_value == 1ULL);
    assert(
        mylite_storage_advance_auto_increment(filename, "app", "posts", 2ULL) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_advance_auto_increment(filename, "app", "posts", 2ULL) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_advance_auto_increment(filename, "app", "posts", 9ULL) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_read_auto_increment(filename, "app", "posts", &next_value) ==
        MYLITE_STORAGE_OK
    );
    assert(next_value == 9ULL);
    assert(mylite_storage_open_header(filename, &header) == MYLITE_STORAGE_OK);
    assert(header.page_count == MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT + 3ULL);

    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_drop_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    assert(rows.rows == NULL);
    assert(
        mylite_storage_read_auto_increment(filename, "app", "posts", &next_value) ==
        MYLITE_STORAGE_OK
    );
    assert(next_value == 1ULL);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_row_page(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-row-page.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    flip_file_byte(
        filename,
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 3U) + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET)
    );
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_CORRUPT);
    assert(
        mylite_storage_count_rows(filename, "app", "posts", &row_count) == MYLITE_STORAGE_CORRUPT
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_row_payload_page(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    const size_t row_size = payload_capacity + 17U;
    unsigned char *row = (unsigned char *)malloc(row_size);
    assert(row != NULL);
    for (size_t i = 0U; i < row_size; ++i) {
        row[i] = (unsigned char)(i % UINT8_MAX);
    }

    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-row-payload-page.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "payloads",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    unsigned long long row_count = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "payloads", row, row_size) == MYLITE_STORAGE_OK
    );
    flip_file_byte(
        filename,
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 3U) + MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET)
    );
    assert(mylite_storage_read_rows(filename, "app", "payloads", &rows) == MYLITE_STORAGE_CORRUPT);
    assert(
        mylite_storage_count_rows(filename, "app", "payloads", &row_count) == MYLITE_STORAGE_CORRUPT
    );

    free(row);
    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rejects_corrupt_autoincrement_page(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    char *root = make_temp_root();
    char *filename = path_join(root, "corrupt-autoincrement-page.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    unsigned long long next_value = 0ULL;

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_advance_auto_increment(filename, "app", "posts", 2ULL) == MYLITE_STORAGE_OK
    );
    flip_file_byte(
        filename,
        (long)((MYLITE_STORAGE_FORMAT_PAGE_SIZE * 3U) +
               MYLITE_STORAGE_FORMAT_AUTOINCREMENT_NEXT_VALUE_OFFSET)
    );
    assert(
        mylite_storage_read_auto_increment(filename, "app", "posts", &next_value) ==
        MYLITE_STORAGE_CORRUPT
    );
    assert(
        mylite_storage_advance_auto_increment(filename, "app", "posts", 3ULL) ==
        MYLITE_STORAGE_CORRUPT
    );

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_drop_table_definition(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "drop-table.mylite");
    mylite_storage_table_definition table_definition = {
        .size = sizeof(table_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    table_list_capture capture = {0};

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    assert(mylite_storage_drop_table(filename, "app", "posts") == MYLITE_STORAGE_OK);
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_NOTFOUND);
    assert(
        mylite_storage_list_tables(filename, "app", collect_table, &capture) == MYLITE_STORAGE_OK
    );
    assert(capture.count == 0U);
    assert(mylite_storage_drop_table(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);

    assert(mylite_storage_store_table_definition(filename, &table_definition) == MYLITE_STORAGE_OK);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_count == 0U);
    assert(rows.rows == NULL);
    assert(
        mylite_storage_list_tables(filename, "app", collect_table, &capture) == MYLITE_STORAGE_OK
    );
    assert(capture.count == 1U);

    assert(unlink(filename) == 0);
    assert(rmdir(root) == 0);
    free(filename);
    free(root);
}

static void test_rename_table_definition(void) {
    static const unsigned char definition[] = {0x01U, 'f', 'r', 'm', 0x00U};
    static const unsigned char row[] = {0x00U, 0x01U, 'a', 'b', 'c'};
    char *root = make_temp_root();
    char *filename = path_join(root, "rename-table.mylite");
    mylite_storage_table_definition posts_definition = {
        .size = sizeof(posts_definition),
        .schema_name = "app",
        .table_name = "posts",
        .requested_engine_name = "InnoDB",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_table_definition target_definition = {
        .size = sizeof(target_definition),
        .schema_name = "app",
        .table_name = "target_posts",
        .requested_engine_name = "MYLITE",
        .effective_engine_name = "MYLITE",
        .definition = definition,
        .definition_size = sizeof(definition),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_table_metadata metadata = {
        .size = sizeof(metadata),
    };
    unsigned char *stored_definition = NULL;
    size_t stored_definition_size = 0U;
    table_list_capture capture = {
        .schema_name = "blog",
        .table_name = "articles",
    };

    assert(mylite_storage_create_empty(filename) == MYLITE_STORAGE_OK);
    assert(mylite_storage_store_table_definition(filename, &posts_definition) == MYLITE_STORAGE_OK);
    assert(
        mylite_storage_store_table_definition(filename, &target_definition) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_append_row(filename, "app", "posts", row, sizeof(row)) == MYLITE_STORAGE_OK
    );
    assert(
        mylite_storage_rename_table(filename, "app", "missing", "blog", "articles") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_rename_table(filename, "app", "missing", "app", "target_posts") ==
        MYLITE_STORAGE_NOTFOUND
    );
    assert(
        mylite_storage_rename_table(filename, "app", "posts", "app", "target_posts") ==
        MYLITE_STORAGE_ERROR
    );
    assert(
        mylite_storage_rename_table(filename, "app", "posts", "blog", "articles") ==
        MYLITE_STORAGE_OK
    );
    assert(mylite_storage_table_exists(filename, "app", "posts") == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_table_exists(filename, "blog", "articles") == MYLITE_STORAGE_OK);
    assert(mylite_storage_read_rows(filename, "app", "posts", &rows) == MYLITE_STORAGE_NOTFOUND);
    assert(mylite_storage_read_rows(filename, "blog", "articles", &rows) == MYLITE_STORAGE_OK);
    assert(rows.row_size == sizeof(row));
    assert(rows.row_count == 1U);
    assert(memcmp(rows.rows, row, sizeof(row)) == 0);
    mylite_storage_free_rowset(&rows);
    assert(
        mylite_storage_read_table_definition(
            filename,
            "blog",
            "articles",
            &stored_definition,
            &stored_definition_size
        ) == MYLITE_STORAGE_OK
    );
    assert(stored_definition_size == sizeof(definition));
    assert(memcmp(stored_definition, definition, sizeof(definition)) == 0);
    mylite_storage_free(stored_definition);
    assert(
        mylite_storage_read_table_metadata(filename, "blog", "articles", &metadata) ==
        MYLITE_STORAGE_OK
    );
    assert(strcmp(metadata.requested_engine_name, "InnoDB") == 0);
    assert(strcmp(metadata.effective_engine_name, "MYLITE") == 0);
    mylite_storage_free(metadata.requested_engine_name);
    mylite_storage_free(metadata.effective_engine_name);
    assert(
        mylite_storage_list_tables(filename, "blog", collect_table, &capture) == MYLITE_STORAGE_OK
    );
    assert(capture.count == 1U);

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

static int collect_table(void *ctx, const char *schema_name, const char *table_name) {
    table_list_capture *capture = (table_list_capture *)ctx;
    const char *expected_schema_name = capture->schema_name == NULL ? "app" : capture->schema_name;
    const char *expected_table_name = capture->table_name == NULL ? "posts" : capture->table_name;
    assert(strcmp(schema_name, expected_schema_name) == 0);
    assert(strcmp(table_name, expected_table_name) == 0);
    ++capture->count;
    return 0;
}

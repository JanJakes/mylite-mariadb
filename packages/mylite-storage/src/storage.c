#include "storage_format.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mylite_storage_catalog_entry {
    const unsigned char *record;
    unsigned long long table_id;
    unsigned long long definition_root_page;
    unsigned long long definition_size;
} mylite_storage_catalog_entry;

typedef struct mylite_storage_row_page {
    unsigned long long row_id;
    unsigned long long table_id;
    size_t row_size;
    size_t row_count;
    const unsigned char *payload;
    unsigned char *owned_payload;
} mylite_storage_row_page;

typedef struct mylite_storage_autoincrement_page {
    unsigned long long table_id;
    unsigned long long next_value;
} mylite_storage_autoincrement_page;

typedef struct mylite_storage_row_state_page {
    unsigned long long table_id;
    unsigned long long source_row_id;
    unsigned long long replacement_row_id;
    unsigned state_kind;
} mylite_storage_row_state_page;

typedef struct mylite_storage_row_state_entry {
    unsigned long long source_row_id;
    unsigned long long replacement_row_id;
    unsigned state_kind;
} mylite_storage_row_state_entry;

typedef struct mylite_storage_row_state_map {
    mylite_storage_row_state_entry *entries;
    size_t count;
} mylite_storage_row_state_map;

typedef struct mylite_storage_index_entry_page {
    unsigned long long table_id;
    unsigned long long row_id;
    unsigned index_number;
    size_t key_size;
    const unsigned char *key;
} mylite_storage_index_entry_page;

typedef struct mylite_storage_blob_write {
    const unsigned char *payload;
    size_t payload_size;
    unsigned page_type;
} mylite_storage_blob_write;

typedef struct mylite_storage_blob_chain {
    unsigned long long first_page_id;
    size_t payload_size;
    unsigned page_type;
} mylite_storage_blob_chain;

typedef struct mylite_storage_row_write {
    const unsigned char *row;
    size_t row_size;
    unsigned long long overflow_root_page;
} mylite_storage_row_write;

typedef struct mylite_storage_row_write_position {
    unsigned long long row_page_id;
    unsigned long long next_page_id;
} mylite_storage_row_write_position;

typedef struct mylite_storage_index_entry_write {
    unsigned long long first_page_id;
    unsigned long long table_id;
    unsigned long long row_id;
    const mylite_storage_index_entry *index_entries;
    size_t index_entry_count;
} mylite_storage_index_entry_write;

typedef struct mylite_storage_live_row_request {
    unsigned long long table_id;
    unsigned long long row_id;
} mylite_storage_live_row_request;

typedef struct mylite_storage_definition_lengths {
    size_t schema_name_size;
    size_t table_name_size;
    size_t requested_engine_name_size;
    size_t effective_engine_name_size;
} mylite_storage_definition_lengths;

typedef struct mylite_storage_table_identity {
    const char *schema_name;
    const char *table_name;
} mylite_storage_table_identity;

typedef mylite_storage_result (*mylite_storage_row_page_callback)(
    void *ctx,
    const mylite_storage_row_page *row_page
);

static mylite_storage_result path_exists(const char *filename, int *exists);
static mylite_storage_result write_empty_database(FILE *file);
static void initialize_header_page(unsigned char *page);
static void encode_header_page(unsigned char *page, const mylite_storage_header *header);
static void initialize_empty_catalog_page(unsigned char *page);
static void update_catalog_checksum(unsigned char *page);
static mylite_storage_result write_page(FILE *file, const unsigned char *page, size_t size);
static mylite_storage_result close_created_file(FILE *file, const char *filename);
static mylite_storage_result open_existing_file(const char *filename, FILE **out_file);
static mylite_storage_result open_existing_file_for_update(const char *filename, FILE **out_file);
static mylite_storage_result read_header(FILE *file, mylite_storage_header *out_header);
static mylite_storage_result read_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    unsigned char *out_page
);
static mylite_storage_result write_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page
);
static mylite_storage_result decode_header_page(
    const unsigned char *page,
    mylite_storage_header *out_header
);
static mylite_storage_result validate_catalog_root_page(
    FILE *file,
    const mylite_storage_header *header
);
static mylite_storage_result validate_catalog_root_bytes(
    const unsigned char *page,
    const mylite_storage_header *header
);
static mylite_storage_result validate_catalog_records(
    const unsigned char *page,
    const mylite_storage_header *header
);
static mylite_storage_result validate_catalog_record(
    const unsigned char *record,
    size_t available_bytes,
    const mylite_storage_header *header,
    size_t *out_record_size
);
static mylite_storage_result validate_table_definition(
    const mylite_storage_table_definition *definition,
    mylite_storage_definition_lengths *out_lengths
);
static mylite_storage_result validate_index_entries(
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count
);
static mylite_storage_result read_catalog_root(
    FILE *file,
    const mylite_storage_header *header,
    unsigned char *out_page
);
static size_t catalog_used_bytes(const unsigned char *page);
static unsigned long long catalog_record_count(const unsigned char *page);
static mylite_storage_result find_table_record(
    const unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    mylite_storage_catalog_entry *out_entry
);
static mylite_storage_result read_table_metadata_from_record(
    const unsigned char *record,
    mylite_storage_table_metadata *out_metadata
);
static mylite_storage_result remove_table_record(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name
);
static mylite_storage_result rename_table_record(
    unsigned char *catalog_page,
    mylite_storage_table_identity old_identity,
    mylite_storage_table_identity new_identity
);
static int record_matches_table(
    const unsigned char *record,
    const char *schema_name,
    const char *table_name
);
static size_t record_field_offset(const unsigned char *record, unsigned field_index);
static size_t record_field_size(const unsigned char *record, unsigned field_index);
static mylite_storage_result append_table_record(
    unsigned char *catalog_page,
    const mylite_storage_table_definition *definition,
    const mylite_storage_definition_lengths *lengths,
    unsigned long long definition_root_page,
    unsigned long long table_id
);
static mylite_storage_result next_table_id(
    FILE *file,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long *out_table_id
);
static unsigned long long catalog_max_table_id(const unsigned char *catalog_page);
static mylite_storage_result read_max_row_table_id(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long *inout_table_id
);
static mylite_storage_result write_definition_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    const unsigned char *definition,
    size_t definition_size
);
static mylite_storage_result write_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    mylite_storage_blob_write blob
);
static void encode_blob_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long next_page_id,
    const unsigned char *payload,
    size_t payload_size,
    unsigned page_type
);
static mylite_storage_result find_table_id(
    FILE *file,
    const mylite_storage_header *header,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_table_id
);
static void encode_row_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    mylite_storage_row_write row
);
static mylite_storage_result write_row_payload_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const unsigned char *row,
    size_t row_size,
    mylite_storage_row_write_position *out_position
);
static mylite_storage_result write_index_entry_pages(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_index_entry_write write,
    unsigned long long *out_next_page_id
);
static void encode_index_entry_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned long long row_id,
    const mylite_storage_index_entry *index_entry
);
static mylite_storage_result read_index_entry_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_index_entry_page *out_index_entry_page
);
static int is_index_entry_page(const unsigned char *page);
static mylite_storage_result scan_table_row_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_page_callback callback,
    void *ctx
);
static mylite_storage_result read_row_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_row_page *out_row_page
);
static int is_row_page(const unsigned char *page);
static mylite_storage_result validate_live_row(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_row_state_map *row_state_map,
    mylite_storage_live_row_request request,
    mylite_storage_row_page *out_row_page
);
static void encode_row_state_page(
    unsigned char *page,
    unsigned long long page_id,
    const mylite_storage_row_state_page *row_state_page
);
static mylite_storage_result build_row_state_map(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_state_map *out_row_state_map
);
static mylite_storage_result read_row_state_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_row_state_page *out_row_state_page
);
static int is_row_state_page(const unsigned char *page);
static mylite_storage_row_state_entry *find_row_state_entry(
    const mylite_storage_row_state_map *row_state_map,
    unsigned long long row_id
);
static mylite_storage_result set_row_state_entry(
    mylite_storage_row_state_map *row_state_map,
    const mylite_storage_row_state_page *row_state_page
);
static void free_row_state_map(mylite_storage_row_state_map *row_state_map);
static mylite_storage_result append_row_page_to_rowset(
    void *ctx,
    const mylite_storage_row_page *row_page
);
static mylite_storage_result append_row_to_rowset(
    mylite_storage_rowset *rowset,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
);
static mylite_storage_result append_index_entry_to_entryset(
    mylite_storage_index_entryset *entryset,
    const mylite_storage_index_entry_page *entry_page
);
static mylite_storage_result count_row_page(void *ctx, const mylite_storage_row_page *row_page);
static void encode_autoincrement_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned long long next_value
);
static mylite_storage_result read_autoincrement_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_autoincrement_page *out_autoincrement_page
);
static int is_autoincrement_page(const unsigned char *page);
static mylite_storage_result latest_auto_increment_value(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long *out_next_value
);
static mylite_storage_result read_definition_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_catalog_entry *entry,
    unsigned char **out_definition,
    size_t *out_definition_size
);
static mylite_storage_result read_row_payload_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_blob_chain chain,
    unsigned char **out_payload
);
static mylite_storage_result read_blob_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned long long expected_remaining,
    unsigned char *out_payload,
    size_t *inout_written,
    unsigned long long *out_next_page_id,
    unsigned expected_page_type
);
static char *copy_record_field(const unsigned char *record, unsigned field_index);
static mylite_storage_result list_catalog_tables(
    const unsigned char *catalog_page,
    const char *schema_name,
    mylite_storage_table_callback callback,
    void *ctx
);
static unsigned get_u32_le(const unsigned char *page, size_t offset);
static unsigned long long get_u64_le(const unsigned char *page, size_t offset);
static void put_u32_le(unsigned char *page, size_t offset, unsigned value);
static void put_u64_le(unsigned char *page, size_t offset, unsigned long long value);
static uint64_t checksum_page(const unsigned char *page, size_t checksum_offset);

static const unsigned char k_header_magic[8] = {'M', 'Y', 'L', 'I', 'T', 'E', '1', '\0'};
static const unsigned char k_catalog_magic[8] = {'M', 'Y', 'L', 'C', 'A', 'T', '1', '\0'};
static const unsigned char k_blob_magic[8] = {'M', 'Y', 'L', 'B', 'L', 'B', '1', '\0'};
static const unsigned char k_row_magic[8] = {'M', 'Y', 'L', 'R', 'O', 'W', '1', '\0'};
static const unsigned char k_autoincrement_magic[8] = {'M', 'Y', 'L', 'A', 'U', 'T', '1', '\0'};
static const unsigned char k_row_state_magic[8] = {'M', 'Y', 'L', 'R', 'S', 'T', '1', '\0'};
static const unsigned char k_index_magic[8] = {'M', 'Y', 'L', 'I', 'D', 'X', '1', '\0'};

const char *mylite_storage_engine_name(void) {
    return MYLITE_STORAGE_ENGINE_NAME;
}

mylite_storage_capabilities mylite_storage_get_capabilities(void) {
    mylite_storage_capabilities capabilities = {
        .size = sizeof(capabilities),
        .format_version = MYLITE_STORAGE_FORMAT_VERSION,
        .flags = MYLITE_STORAGE_CAPABILITY_FILE_HEADER | MYLITE_STORAGE_CAPABILITY_EMPTY_CATALOG |
                 MYLITE_STORAGE_CAPABILITY_TABLE_DEFINITIONS |
                 MYLITE_STORAGE_CAPABILITY_TABLE_ROWS | MYLITE_STORAGE_CAPABILITY_AUTOINCREMENT |
                 MYLITE_STORAGE_CAPABILITY_BLOB_TEXT_ROWS |
                 MYLITE_STORAGE_CAPABILITY_ROW_LIFECYCLE | MYLITE_STORAGE_CAPABILITY_INDEX_ENTRIES,
    };

    return capabilities;
}

mylite_storage_result mylite_storage_create_empty(const char *filename) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    int exists = 0;
    mylite_storage_result result = path_exists(filename, &exists);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (exists) {
        return MYLITE_STORAGE_ERROR;
    }

    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        return MYLITE_STORAGE_IOERR;
    }

    result = write_empty_database(file);
    if (result != MYLITE_STORAGE_OK) {
        fclose(file);
        remove(filename);
        return result;
    }

    return close_created_file(file, filename);
}

mylite_storage_result mylite_storage_open_header(
    const char *filename,
    mylite_storage_header *out_header
) {
    if (filename == NULL || filename[0] == '\0' || out_header == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_header = (mylite_storage_header){0};

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    result = read_header(file, out_header);
    if (result == MYLITE_STORAGE_OK) {
        result = validate_catalog_root_page(file, out_header);
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }

    if (result != MYLITE_STORAGE_OK) {
        *out_header = (mylite_storage_header){0};
    }
    return result;
}

mylite_storage_result mylite_storage_store_table_definition(
    const char *filename,
    const mylite_storage_table_definition *definition
) {
    if (filename == NULL || filename[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    mylite_storage_definition_lengths lengths = {0};
    mylite_storage_result result = validate_table_definition(definition, &lengths);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result =
            find_table_record(catalog_page, definition->schema_name, definition->table_name, NULL);
        if (result == MYLITE_STORAGE_OK) {
            result = MYLITE_STORAGE_ERROR;
        } else if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
        }
    }

    const size_t blob_payload_size =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    const unsigned long long blob_page_count =
        ((definition->definition_size - 1U) / blob_payload_size) + 1U;
    const unsigned long long first_blob_page = header.page_count;
    unsigned long long table_id = 0ULL;
    if (result == MYLITE_STORAGE_OK && blob_page_count > ULLONG_MAX - header.page_count) {
        result = MYLITE_STORAGE_FULL;
    }
    if (result == MYLITE_STORAGE_OK) {
        result = next_table_id(file, &header, catalog_page, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = append_table_record(catalog_page, definition, &lengths, first_blob_page, table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = write_definition_blob_pages(
            file,
            first_blob_page,
            definition->definition,
            definition->definition_size
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        header.page_count += blob_page_count;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_read_table_definition(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned char **out_definition,
    size_t *out_definition_size
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_definition == NULL ||
        out_definition_size == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_definition = NULL;
    *out_definition_size = 0U;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result =
            read_definition_blob_pages(file, &header, &entry, out_definition, out_definition_size);
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        free(*out_definition);
        *out_definition = NULL;
        *out_definition_size = 0U;
    }
    return result;
}

mylite_storage_result mylite_storage_read_table_metadata(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    mylite_storage_table_metadata *out_metadata
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_metadata == NULL ||
        out_metadata->size < sizeof(*out_metadata)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_metadata = (mylite_storage_table_metadata){
        .size = sizeof(*out_metadata),
    };

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry entry = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = read_table_metadata_from_record(entry.record, out_metadata);
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        free(out_metadata->requested_engine_name);
        free(out_metadata->effective_engine_name);
        *out_metadata = (mylite_storage_table_metadata){
            .size = sizeof(*out_metadata),
        };
    }
    return result;
}

mylite_storage_result mylite_storage_table_exists(
    const char *filename,
    const char *schema_name,
    const char *table_name
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, NULL);
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_drop_table(
    const char *filename,
    const char *schema_name,
    const char *table_name
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = remove_table_record(catalog_page, schema_name, table_name);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_rename_table(
    const char *filename,
    const char *old_schema_name,
    const char *old_table_name,
    const char *new_schema_name,
    const char *new_table_name
) {
    if (filename == NULL || filename[0] == '\0' || old_schema_name == NULL ||
        old_schema_name[0] == '\0' || old_table_name == NULL || old_table_name[0] == '\0' ||
        new_schema_name == NULL || new_schema_name[0] == '\0' || new_table_name == NULL ||
        new_table_name[0] == '\0') {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = rename_table_record(
            catalog_page,
            (mylite_storage_table_identity){
                .schema_name = old_schema_name,
                .table_name = old_table_name,
            },
            (mylite_storage_table_identity){
                .schema_name = new_schema_name,
                .table_name = new_table_name,
            }
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        ++header.catalog_generation;
        put_u64_le(
            catalog_page,
            MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
            header.catalog_generation
        );
        update_catalog_checksum(catalog_page);

        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);

        result = write_page_at(file, header.catalog_root_page, header.page_size, catalog_page);
        if (result == MYLITE_STORAGE_OK) {
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_append_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned char *row,
    size_t row_size
) {
    return mylite_storage_append_row_with_index_entries(
        filename,
        schema_name,
        table_name,
        row,
        row_size,
        NULL,
        0U,
        NULL
    );
}

mylite_storage_result mylite_storage_append_row_with_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    unsigned long long *out_row_id
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || row == NULL || row_size == 0U) {
        return MYLITE_STORAGE_MISUSE;
    }
    if (row_size > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    if (out_row_id != NULL) {
        *out_row_id = 0ULL;
    }
    mylite_storage_result result = validate_index_entries(index_entries, index_entry_count);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    mylite_storage_row_write_position position = {0};
    if (result == MYLITE_STORAGE_OK) {
        result = write_row_payload_pages(file, &header, table_id, row, row_size, &position);
    }
    unsigned long long next_page_id = position.next_page_id;
    if (result == MYLITE_STORAGE_OK) {
        result = write_index_entry_pages(
            file,
            &header,
            (mylite_storage_index_entry_write){
                .first_page_id = next_page_id,
                .table_id = table_id,
                .row_id = position.row_page_id,
                .index_entries = index_entries,
                .index_entry_count = index_entry_count,
            },
            &next_page_id
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        header.page_count = next_page_id;
        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);
        result = write_page_at(
            file,
            MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
            header.page_size,
            header_page
        );
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result == MYLITE_STORAGE_OK && out_row_id != NULL) {
        *out_row_id = position.row_page_id;
    }
    return result;
}

mylite_storage_result mylite_storage_read_rows(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    mylite_storage_rowset *out_rows
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_rows == NULL ||
        out_rows->size < sizeof(*out_rows)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_rows = (mylite_storage_rowset){
        .size = sizeof(*out_rows),
    };

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = scan_table_row_pages(file, &header, table_id, append_row_page_to_rowset, out_rows);
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        mylite_storage_free_rowset(out_rows);
    }
    return result;
}

mylite_storage_result mylite_storage_count_rows(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_row_count
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_row_count == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_row_count = 0ULL;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = scan_table_row_pages(file, &header, table_id, count_row_page, out_row_count);
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_row_count = 0ULL;
    }
    return result;
}

mylite_storage_result mylite_storage_read_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    unsigned char **out_row,
    size_t *out_row_size
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || row_id == 0ULL || out_row == NULL ||
        out_row_size == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_row = NULL;
    *out_row_size = 0U;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_row_page row_page = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = build_row_state_map(file, &header, table_id, &row_state_map);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_live_row(
            file,
            &header,
            &row_state_map,
            (mylite_storage_live_row_request){
                .table_id = table_id,
                .row_id = row_id,
            },
            &row_page
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        unsigned char *row = (unsigned char *)malloc(row_page.row_size);
        if (row == NULL) {
            result = MYLITE_STORAGE_NOMEM;
        } else {
            memcpy(row, row_page.payload, row_page.row_size);
            *out_row = row;
            *out_row_size = row_page.row_size;
        }
    }

    free(row_page.owned_payload);
    free_row_state_map(&row_state_map);
    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        free(*out_row);
        *out_row = NULL;
        *out_row_size = 0U;
    }
    return result;
}

mylite_storage_result mylite_storage_update_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    unsigned long long *out_new_row_id
) {
    return mylite_storage_update_row_with_index_entries(
        filename,
        schema_name,
        table_name,
        row_id,
        row,
        row_size,
        NULL,
        0U,
        out_new_row_id
    );
}

mylite_storage_result mylite_storage_update_row_with_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size,
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count,
    unsigned long long *out_new_row_id
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || row_id == 0ULL || row == NULL ||
        row_size == 0U || out_new_row_id == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }
    if (row_size > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    *out_new_row_id = 0ULL;
    mylite_storage_result result = validate_index_entries(index_entries, index_entry_count);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    FILE *file = NULL;
    result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_row_page old_row_page = {0};
    mylite_storage_row_write_position position = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = build_row_state_map(file, &header, table_id, &row_state_map);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_live_row(
            file,
            &header,
            &row_state_map,
            (mylite_storage_live_row_request){
                .table_id = table_id,
                .row_id = row_id,
            },
            &old_row_page
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        result = write_row_payload_pages(file, &header, table_id, row, row_size, &position);
    }
    if (result == MYLITE_STORAGE_OK && position.next_page_id == ULLONG_MAX) {
        result = MYLITE_STORAGE_FULL;
    }
    unsigned long long next_page_id = position.next_page_id;
    if (result == MYLITE_STORAGE_OK) {
        const mylite_storage_row_state_page row_state = {
            .table_id = table_id,
            .source_row_id = row_id,
            .replacement_row_id = position.row_page_id,
            .state_kind = MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE,
        };
        unsigned char state_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_row_state_page(state_page, next_page_id, &row_state);
        result = write_page_at(file, next_page_id, header.page_size, state_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        ++next_page_id;
        result = write_index_entry_pages(
            file,
            &header,
            (mylite_storage_index_entry_write){
                .first_page_id = next_page_id,
                .table_id = table_id,
                .row_id = position.row_page_id,
                .index_entries = index_entries,
                .index_entry_count = index_entry_count,
            },
            &next_page_id
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        header.page_count = next_page_id;
        unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_header_page(header_page, &header);
        result = write_page_at(
            file,
            MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
            header.page_size,
            header_page
        );
    }
    if (result == MYLITE_STORAGE_OK) {
        *out_new_row_id = position.row_page_id;
    }

    free(old_row_page.owned_payload);
    free_row_state_map(&row_state_map);
    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_new_row_id = 0ULL;
    }
    return result;
}

mylite_storage_result mylite_storage_delete_row(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long row_id
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || row_id == 0ULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_row_page row_page = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = build_row_state_map(file, &header, table_id, &row_state_map);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = validate_live_row(
            file,
            &header,
            &row_state_map,
            (mylite_storage_live_row_request){
                .table_id = table_id,
                .row_id = row_id,
            },
            &row_page
        );
    }
    if (result == MYLITE_STORAGE_OK && header.page_count == ULLONG_MAX) {
        result = MYLITE_STORAGE_FULL;
    }
    if (result == MYLITE_STORAGE_OK) {
        const unsigned long long state_page_id = header.page_count;
        const mylite_storage_row_state_page row_state = {
            .table_id = table_id,
            .source_row_id = row_id,
            .replacement_row_id = 0ULL,
            .state_kind = MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_DELETE,
        };
        unsigned char state_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_row_state_page(state_page, state_page_id, &row_state);
        result = write_page_at(file, state_page_id, header.page_size, state_page);
        if (result == MYLITE_STORAGE_OK) {
            ++header.page_count;
            unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
            encode_header_page(header_page, &header);
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
    }

    free(row_page.owned_payload);
    free_row_state_map(&row_state_map);
    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_read_index_entries(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned index_number,
    mylite_storage_index_entryset *out_entries
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_entries == NULL ||
        out_entries->size < sizeof(*out_entries)) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_entries = (mylite_storage_index_entryset){
        .size = sizeof(*out_entries),
    };

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    mylite_storage_row_state_map row_state_map = {0};
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = build_row_state_map(file, &header, table_id, &row_state_map);
    }
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         result == MYLITE_STORAGE_OK && page_id < header.page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_index_entry_page entry_page = {0};
        result = read_index_entry_page(file, &header, page_id, page, &entry_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            result = MYLITE_STORAGE_OK;
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            break;
        }
        if (entry_page.table_id != table_id || entry_page.index_number != index_number ||
            find_row_state_entry(&row_state_map, entry_page.row_id) != NULL) {
            continue;
        }

        mylite_storage_row_page row_page = {0};
        result = validate_live_row(
            file,
            &header,
            &row_state_map,
            (mylite_storage_live_row_request){
                .table_id = table_id,
                .row_id = entry_page.row_id,
            },
            &row_page
        );
        free(row_page.owned_payload);
        if (result == MYLITE_STORAGE_OK) {
            result = append_index_entry_to_entryset(out_entries, &entry_page);
        }
    }

    free_row_state_map(&row_state_map);
    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        mylite_storage_free_index_entryset(out_entries);
    }
    return result;
}

mylite_storage_result mylite_storage_read_auto_increment(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_next_value
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || out_next_value == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_next_value = 0ULL;

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = latest_auto_increment_value(file, &header, table_id, out_next_value);
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    if (result != MYLITE_STORAGE_OK) {
        *out_next_value = 0ULL;
    }
    return result;
}

mylite_storage_result mylite_storage_advance_auto_increment(
    const char *filename,
    const char *schema_name,
    const char *table_name,
    unsigned long long next_value
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        table_name == NULL || table_name[0] == '\0' || next_value == 0ULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file_for_update(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned long long table_id = 0ULL;
    unsigned long long current_next_value = 0ULL;
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_id(file, &header, schema_name, table_name, &table_id);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = latest_auto_increment_value(file, &header, table_id, &current_next_value);
    }
    if (result == MYLITE_STORAGE_OK && next_value <= current_next_value) {
        if (fclose(file) != 0) {
            return MYLITE_STORAGE_IOERR;
        }
        return MYLITE_STORAGE_OK;
    }
    if (result == MYLITE_STORAGE_OK && header.page_count == ULLONG_MAX) {
        result = MYLITE_STORAGE_FULL;
    }
    if (result == MYLITE_STORAGE_OK) {
        const unsigned long long page_id = header.page_count;
        unsigned char autoincrement_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_autoincrement_page(autoincrement_page, page_id, table_id, next_value);

        result = write_page_at(file, page_id, header.page_size, autoincrement_page);
        if (result == MYLITE_STORAGE_OK) {
            ++header.page_count;
            unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
            encode_header_page(header_page, &header);
            result = write_page_at(
                file,
                MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
                header.page_size,
                header_page
            );
        }
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

mylite_storage_result mylite_storage_list_tables(
    const char *filename,
    const char *schema_name,
    mylite_storage_table_callback callback,
    void *ctx
) {
    if (filename == NULL || filename[0] == '\0' || schema_name == NULL || schema_name[0] == '\0' ||
        callback == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    FILE *file = NULL;
    mylite_storage_result result = open_existing_file(filename, &file);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    mylite_storage_header header = {0};
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    result = read_header(file, &header);
    if (result == MYLITE_STORAGE_OK) {
        result = read_catalog_root(file, &header, catalog_page);
    }
    if (result == MYLITE_STORAGE_OK) {
        result = list_catalog_tables(catalog_page, schema_name, callback, ctx);
    }

    if (fclose(file) != 0 && result == MYLITE_STORAGE_OK) {
        result = MYLITE_STORAGE_IOERR;
    }
    return result;
}

void mylite_storage_free(void *ptr) {
    free(ptr);
}

void mylite_storage_free_rowset(mylite_storage_rowset *rowset) {
    if (rowset == NULL) {
        return;
    }

    free(rowset->rows);
    free(rowset->row_offsets);
    free(rowset->row_sizes);
    free(rowset->row_ids);
    *rowset = (mylite_storage_rowset){
        .size = sizeof(*rowset),
    };
}

void mylite_storage_free_index_entryset(mylite_storage_index_entryset *entryset) {
    if (entryset == NULL) {
        return;
    }

    free(entryset->keys);
    free(entryset->key_offsets);
    free(entryset->key_sizes);
    free(entryset->row_ids);
    *entryset = (mylite_storage_index_entryset){
        .size = sizeof(*entryset),
    };
}

static mylite_storage_result path_exists(const char *filename, int *exists) {
    errno = 0;
    FILE *file = fopen(filename, "rb");
    if (file != NULL) {
        if (fclose(file) != 0) {
            return MYLITE_STORAGE_IOERR;
        }
        *exists = 1;
        return MYLITE_STORAGE_OK;
    }

    if (errno == ENOENT) {
        *exists = 0;
        return MYLITE_STORAGE_OK;
    }

    return MYLITE_STORAGE_IOERR;
}

static mylite_storage_result write_empty_database(FILE *file) {
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    initialize_header_page(header_page);
    initialize_empty_catalog_page(catalog_page);

    mylite_storage_result result = write_page(file, header_page, sizeof(header_page));
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return write_page(file, catalog_page, sizeof(catalog_page));
}

static void initialize_header_page(unsigned char *page) {
    const mylite_storage_header header = {
        .size = sizeof(header),
        .format_version = MYLITE_STORAGE_FORMAT_VERSION,
        .header_version = MYLITE_STORAGE_FORMAT_HEADER_VERSION,
        .page_size = MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        .checksum_algorithm = MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64,
        .flags = 0U,
        .catalog_root_page = MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID,
        .catalog_generation = MYLITE_STORAGE_FORMAT_EMPTY_CATALOG_GENERATION,
        .free_list_root_page = 0ULL,
        .page_count = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT,
    };
    encode_header_page(page, &header);
}

static void encode_header_page(unsigned char *page, const mylite_storage_header *header) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_HEADER_MAGIC_OFFSET,
        k_header_magic,
        sizeof(k_header_magic)
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_VERSION_OFFSET, header->header_version);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_FORMAT_VERSION_OFFSET, header->format_version);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_PAGE_SIZE_OFFSET, header->page_size);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_BYTE_ORDER_OFFSET,
        MYLITE_STORAGE_FORMAT_BYTE_ORDER_MARKER
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_ALGORITHM_OFFSET,
        header->checksum_algorithm
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_FLAGS_OFFSET, header->flags);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CATALOG_ROOT_PAGE_OFFSET,
        header->catalog_root_page
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CATALOG_GENERATION_OFFSET,
        header->catalog_generation
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_FREE_LIST_ROOT_PAGE_OFFSET,
        header->free_list_root_page
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_PAGE_COUNT_OFFSET, header->page_count);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET,
        checksum_page(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET)
    );
}

static void initialize_empty_catalog_page(unsigned char *page) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_CATALOG_MAGIC_OFFSET,
        k_catalog_magic,
        sizeof(k_catalog_magic)
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_PAGE_TYPE_ROOT
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_PAGE_ID_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET,
        MYLITE_STORAGE_FORMAT_EMPTY_CATALOG_GENERATION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    update_catalog_checksum(page);
}

static void update_catalog_checksum(unsigned char *page) {
    put_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET,
        checksum_page(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result write_page(FILE *file, const unsigned char *page, size_t size) {
    if (fwrite(page, 1U, size, file) != size) {
        return MYLITE_STORAGE_IOERR;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result close_created_file(FILE *file, const char *filename) {
    if (fclose(file) == 0) {
        return MYLITE_STORAGE_OK;
    }

    remove(filename);
    return MYLITE_STORAGE_IOERR;
}

static mylite_storage_result open_existing_file(const char *filename, FILE **out_file) {
    errno = 0;
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        return errno == ENOENT ? MYLITE_STORAGE_NOTFOUND : MYLITE_STORAGE_IOERR;
    }

    *out_file = file;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result open_existing_file_for_update(const char *filename, FILE **out_file) {
    errno = 0;
    FILE *file = fopen(filename, "r+b");
    if (file == NULL) {
        return errno == ENOENT ? MYLITE_STORAGE_NOTFOUND : MYLITE_STORAGE_IOERR;
    }

    *out_file = file;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_header(FILE *file, mylite_storage_header *out_header) {
    unsigned char header_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_result result = read_page_at(
        file,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_ID,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE,
        header_page
    );
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    return decode_header_page(header_page, out_header);
}

static mylite_storage_result read_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    unsigned char *out_page
) {
    if (page_size == 0U || page_id > ULLONG_MAX / page_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long offset = page_id * (unsigned long long)page_size;
    if (offset > (unsigned long long)LONG_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        return MYLITE_STORAGE_IOERR;
    }

    const size_t read_count = fread(out_page, 1U, page_size, file);
    if (read_count != page_size) {
        return ferror(file) ? MYLITE_STORAGE_IOERR : MYLITE_STORAGE_CORRUPT;
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    const unsigned char *page
) {
    if (page_size == 0U || page_id > ULLONG_MAX / page_size) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long offset = page_id * (unsigned long long)page_size;
    if (offset > (unsigned long long)LONG_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        return MYLITE_STORAGE_IOERR;
    }
    return write_page(file, page, page_size);
}

static mylite_storage_result decode_header_page(
    const unsigned char *page,
    mylite_storage_header *out_header
) {
    if (memcmp(
            page + MYLITE_STORAGE_FORMAT_HEADER_MAGIC_OFFSET,
            k_header_magic,
            sizeof(k_header_magic)
        ) != 0) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned header_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_FORMAT_VERSION_OFFSET);
    const unsigned page_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_PAGE_SIZE_OFFSET);
    const unsigned byte_order = get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_BYTE_ORDER_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_ALGORITHM_OFFSET);

    if (header_version != MYLITE_STORAGE_FORMAT_HEADER_VERSION ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        page_size != MYLITE_STORAGE_FORMAT_PAGE_SIZE ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }
    if (byte_order != MYLITE_STORAGE_FORMAT_BYTE_ORDER_MARKER) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_OFFSET);
    if (expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long catalog_root_page =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_CATALOG_ROOT_PAGE_OFFSET);
    const unsigned long long catalog_generation =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_CATALOG_GENERATION_OFFSET);
    const unsigned long long page_count =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_PAGE_COUNT_OFFSET);
    if (catalog_root_page == 0ULL || catalog_root_page >= page_count ||
        catalog_generation == 0ULL) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_header = (mylite_storage_header){
        .size = sizeof(*out_header),
        .format_version = format_version,
        .header_version = header_version,
        .page_size = page_size,
        .checksum_algorithm = checksum_algorithm,
        .flags = get_u32_le(page, MYLITE_STORAGE_FORMAT_HEADER_FLAGS_OFFSET),
        .catalog_root_page = catalog_root_page,
        .catalog_generation = catalog_generation,
        .free_list_root_page =
            get_u64_le(page, MYLITE_STORAGE_FORMAT_HEADER_FREE_LIST_ROOT_PAGE_OFFSET),
        .page_count = page_count,
    };
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_catalog_root_page(
    FILE *file,
    const mylite_storage_header *header
) {
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_result result =
        read_page_at(file, header->catalog_root_page, header->page_size, catalog_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return validate_catalog_root_bytes(catalog_page, header);
}

static mylite_storage_result validate_catalog_root_bytes(
    const unsigned char *page,
    const mylite_storage_header *header
) {
    if (memcmp(
            page + MYLITE_STORAGE_FORMAT_CATALOG_MAGIC_OFFSET,
            k_catalog_magic,
            sizeof(k_catalog_magic)
        ) != 0) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_PAGE_TYPE_OFFSET);
    const unsigned page_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_ALGORITHM_OFFSET);
    if (page_type != MYLITE_STORAGE_FORMAT_CATALOG_PAGE_TYPE_ROOT || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    const unsigned long long page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_PAGE_ID_OFFSET);
    const unsigned long long generation =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_GENERATION_OFFSET);
    const unsigned long long next_page =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_NEXT_PAGE_OFFSET);
    const size_t used_bytes = catalog_used_bytes(page);
    if (page_id != header->catalog_root_page || generation != header->catalog_generation ||
        next_page != 0ULL || used_bytes < MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE ||
        used_bytes > MYLITE_STORAGE_FORMAT_PAGE_SIZE) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET);
    if (expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }

    return validate_catalog_records(page, header);
}

static mylite_storage_result validate_catalog_records(
    const unsigned char *page,
    const mylite_storage_header *header
) {
    const size_t used_bytes = catalog_used_bytes(page);
    const unsigned long long record_count = catalog_record_count(page);
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        size_t record_size = 0U;
        mylite_storage_result result =
            validate_catalog_record(page + offset, used_bytes - offset, header, &record_size);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        offset += record_size;
    }

    return offset == used_bytes ? MYLITE_STORAGE_OK : MYLITE_STORAGE_CORRUPT;
}

static mylite_storage_result validate_catalog_record(
    const unsigned char *record,
    size_t available_bytes,
    const mylite_storage_header *header,
    size_t *out_record_size
) {
    if (available_bytes < MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned record_type = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET);
    const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    const size_t schema_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET);
    const size_t table_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET);
    const size_t requested_engine_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_REQUESTED_ENGINE_LENGTH_OFFSET);
    const size_t effective_engine_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_EFFECTIVE_ENGINE_LENGTH_OFFSET);
    if (record_type != MYLITE_STORAGE_FORMAT_RECORD_TYPE_TABLE_DEFINITION ||
        record_size < MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE || record_size > available_bytes ||
        schema_name_size == 0U || table_name_size == 0U || requested_engine_name_size == 0U ||
        effective_engine_name_size == 0U) {
        return MYLITE_STORAGE_CORRUPT;
    }

    size_t expected_size = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    if (schema_name_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += schema_name_size;
    if (table_name_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += table_name_size;
    if (requested_engine_name_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += requested_engine_name_size;
    if (effective_engine_name_size > SIZE_MAX - expected_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    expected_size += effective_engine_name_size;

    const unsigned long long definition_root_page =
        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET);
    const unsigned long long definition_size =
        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET);
    const unsigned long long table_id =
        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET);
    if (record_size != expected_size || definition_size == 0ULL || table_id == 0ULL ||
        definition_root_page <= header->catalog_root_page ||
        definition_root_page >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_record_size = record_size;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_table_definition(
    const mylite_storage_table_definition *definition,
    mylite_storage_definition_lengths *out_lengths
) {
    if (definition == NULL || definition->size < sizeof(*definition) ||
        definition->schema_name == NULL || definition->schema_name[0] == '\0' ||
        definition->table_name == NULL || definition->table_name[0] == '\0' ||
        definition->requested_engine_name == NULL || definition->requested_engine_name[0] == '\0' ||
        definition->effective_engine_name == NULL || definition->effective_engine_name[0] == '\0' ||
        definition->definition == NULL || definition->definition_size == 0U) {
        return MYLITE_STORAGE_MISUSE;
    }

    *out_lengths = (mylite_storage_definition_lengths){
        .schema_name_size = strlen(definition->schema_name),
        .table_name_size = strlen(definition->table_name),
        .requested_engine_name_size = strlen(definition->requested_engine_name),
        .effective_engine_name_size = strlen(definition->effective_engine_name),
    };
    if (out_lengths->schema_name_size > UINT32_MAX || out_lengths->table_name_size > UINT32_MAX ||
        out_lengths->requested_engine_name_size > UINT32_MAX ||
        out_lengths->effective_engine_name_size > UINT32_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result validate_index_entries(
    const mylite_storage_index_entry *index_entries,
    size_t index_entry_count
) {
    const size_t key_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET;
    if (index_entry_count == 0U) {
        return MYLITE_STORAGE_OK;
    }
    if (index_entries == NULL) {
        return MYLITE_STORAGE_MISUSE;
    }

    for (size_t i = 0U; i < index_entry_count; ++i) {
        if (index_entries[i].size < sizeof(index_entries[i]) || index_entries[i].key == NULL ||
            index_entries[i].key_size == 0U) {
            return MYLITE_STORAGE_MISUSE;
        }
        if (index_entries[i].key_size > key_capacity || index_entries[i].key_size > UINT32_MAX) {
            return MYLITE_STORAGE_UNSUPPORTED;
        }
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_catalog_root(
    FILE *file,
    const mylite_storage_header *header,
    unsigned char *out_page
) {
    mylite_storage_result result =
        read_page_at(file, header->catalog_root_page, header->page_size, out_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    return validate_catalog_root_bytes(out_page, header);
}

static size_t catalog_used_bytes(const unsigned char *page) {
    const unsigned used_bytes = get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET);
    if (used_bytes == 0U && catalog_record_count(page) == 0ULL) {
        return MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    }
    return used_bytes;
}

static unsigned long long catalog_record_count(const unsigned char *page) {
    return get_u32_le(page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET);
}

static mylite_storage_result find_table_record(
    const unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name,
    mylite_storage_catalog_entry *out_entry
) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        if (record_matches_table(record, schema_name, table_name)) {
            if (out_entry != NULL) {
                *out_entry = (mylite_storage_catalog_entry){
                    .record = record,
                    .table_id = get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET),
                    .definition_root_page = get_u64_le(
                        record,
                        MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET
                    ),
                    .definition_size =
                        get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
                };
            }
            return MYLITE_STORAGE_OK;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return MYLITE_STORAGE_NOTFOUND;
}

static mylite_storage_result read_table_metadata_from_record(
    const unsigned char *record,
    mylite_storage_table_metadata *out_metadata
) {
    char *requested_engine_name = copy_record_field(record, 2U);
    char *effective_engine_name = copy_record_field(record, 3U);
    if (requested_engine_name == NULL || effective_engine_name == NULL) {
        free(requested_engine_name);
        free(effective_engine_name);
        return MYLITE_STORAGE_NOMEM;
    }

    out_metadata->requested_engine_name = requested_engine_name;
    out_metadata->effective_engine_name = effective_engine_name;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result remove_table_record(
    unsigned char *catalog_page,
    const char *schema_name,
    const char *table_name
) {
    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    size_t new_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    unsigned new_record_count = 0U;
    int removed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_matches_table(record, schema_name, table_name)) {
            removed = 1;
        } else {
            memcpy(new_catalog_page + new_offset, record, record_size);
            new_offset += record_size;
            ++new_record_count;
        }
        old_offset += record_size;
    }

    if (!removed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        new_record_count
    );
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)new_offset
    );
    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result rename_table_record(
    unsigned char *catalog_page,
    mylite_storage_table_identity old_identity,
    mylite_storage_table_identity new_identity
) {
    mylite_storage_result result =
        find_table_record(catalog_page, old_identity.schema_name, old_identity.table_name, NULL);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    result =
        find_table_record(catalog_page, new_identity.schema_name, new_identity.table_name, NULL);
    if (result == MYLITE_STORAGE_OK) {
        return MYLITE_STORAGE_ERROR;
    }
    if (result != MYLITE_STORAGE_NOTFOUND) {
        return result;
    }

    unsigned char new_catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    memcpy(new_catalog_page, catalog_page, sizeof(new_catalog_page));
    memset(
        new_catalog_page + MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE,
        0,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );
    put_u32_le(new_catalog_page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET, 0U);
    put_u32_le(
        new_catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE
    );

    size_t old_offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    int renamed = 0;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + old_offset;
        const size_t record_size = get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
        if (record_matches_table(record, old_identity.schema_name, old_identity.table_name)) {
            char *requested_engine_name = copy_record_field(record, 2U);
            char *effective_engine_name = copy_record_field(record, 3U);
            if (requested_engine_name == NULL || effective_engine_name == NULL) {
                free(requested_engine_name);
                free(effective_engine_name);
                return MYLITE_STORAGE_NOMEM;
            }

            mylite_storage_table_definition definition = {
                .size = sizeof(definition),
                .schema_name = new_identity.schema_name,
                .table_name = new_identity.table_name,
                .requested_engine_name = requested_engine_name,
                .effective_engine_name = effective_engine_name,
                .definition = (const unsigned char *)"",
                .definition_size =
                    get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET),
            };
            mylite_storage_definition_lengths lengths = {
                .schema_name_size = strlen(new_identity.schema_name),
                .table_name_size = strlen(new_identity.table_name),
                .requested_engine_name_size = strlen(requested_engine_name),
                .effective_engine_name_size = strlen(effective_engine_name),
            };
            result = append_table_record(
                new_catalog_page,
                &definition,
                &lengths,
                get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET),
                get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET)
            );
            free(requested_engine_name);
            free(effective_engine_name);
            if (result != MYLITE_STORAGE_OK) {
                return result;
            }
            renamed = 1;
        } else {
            const size_t new_offset = catalog_used_bytes(new_catalog_page);
            if (record_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - new_offset) {
                return MYLITE_STORAGE_FULL;
            }
            memcpy(new_catalog_page + new_offset, record, record_size);
            put_u32_le(
                new_catalog_page,
                MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
                (unsigned)(catalog_record_count(new_catalog_page) + 1ULL)
            );
            put_u32_le(
                new_catalog_page,
                MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
                (unsigned)(new_offset + record_size)
            );
        }
        old_offset += record_size;
    }

    if (!renamed) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    memcpy(catalog_page, new_catalog_page, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    return MYLITE_STORAGE_OK;
}

static int record_matches_table(
    const unsigned char *record,
    const char *schema_name,
    const char *table_name
) {
    const size_t schema_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET);
    const size_t table_name_size =
        get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET);
    const unsigned char *record_schema = record + record_field_offset(record, 0U);
    const unsigned char *record_table = record + record_field_offset(record, 1U);
    return strlen(schema_name) == schema_name_size && strlen(table_name) == table_name_size &&
           memcmp(record_schema, schema_name, schema_name_size) == 0 &&
           memcmp(record_table, table_name, table_name_size) == 0;
}

static size_t record_field_offset(const unsigned char *record, unsigned field_index) {
    size_t offset = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    for (unsigned i = 0U; i < field_index; ++i) {
        offset += record_field_size(record, i);
    }
    return offset;
}

static size_t record_field_size(const unsigned char *record, unsigned field_index) {
    switch (field_index) {
    case 0U:
        return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET);
    case 1U:
        return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET);
    case 2U:
        return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_REQUESTED_ENGINE_LENGTH_OFFSET);
    case 3U:
        return get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_EFFECTIVE_ENGINE_LENGTH_OFFSET);
    default:
        return 0U;
    }
}

static mylite_storage_result append_table_record(
    unsigned char *catalog_page,
    const mylite_storage_table_definition *definition,
    const mylite_storage_definition_lengths *lengths,
    unsigned long long definition_root_page,
    unsigned long long table_id
) {
    size_t record_size = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    if (lengths->schema_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->schema_name_size;
    if (lengths->table_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->table_name_size;
    if (lengths->requested_engine_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->requested_engine_name_size;
    if (lengths->effective_engine_name_size > SIZE_MAX - record_size) {
        return MYLITE_STORAGE_FULL;
    }
    record_size += lengths->effective_engine_name_size;
    if (record_size > UINT32_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t used_bytes = catalog_used_bytes(catalog_page);
    const unsigned long long record_count = catalog_record_count(catalog_page);
    if (record_count >= UINT32_MAX || used_bytes > MYLITE_STORAGE_FORMAT_PAGE_SIZE ||
        record_size > MYLITE_STORAGE_FORMAT_PAGE_SIZE - used_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned char *record = catalog_page + used_bytes;
    memset(record, 0, record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_RECORD_TYPE_TABLE_DEFINITION
    );
    put_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET, (unsigned)record_size);
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_SCHEMA_LENGTH_OFFSET,
        (unsigned)lengths->schema_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_TABLE_LENGTH_OFFSET,
        (unsigned)lengths->table_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_REQUESTED_ENGINE_LENGTH_OFFSET,
        (unsigned)lengths->requested_engine_name_size
    );
    put_u32_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_EFFECTIVE_ENGINE_LENGTH_OFFSET,
        (unsigned)lengths->effective_engine_name_size
    );
    put_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET, table_id);
    put_u64_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_ROOT_PAGE_OFFSET,
        definition_root_page
    );
    put_u64_le(
        record,
        MYLITE_STORAGE_FORMAT_RECORD_DEFINITION_SIZE_OFFSET,
        (unsigned long long)definition->definition_size
    );

    size_t field_offset = MYLITE_STORAGE_FORMAT_RECORD_HEADER_SIZE;
    memcpy(record + field_offset, definition->schema_name, lengths->schema_name_size);
    field_offset += lengths->schema_name_size;
    memcpy(record + field_offset, definition->table_name, lengths->table_name_size);
    field_offset += lengths->table_name_size;
    memcpy(
        record + field_offset,
        definition->requested_engine_name,
        lengths->requested_engine_name_size
    );
    field_offset += lengths->requested_engine_name_size;
    memcpy(
        record + field_offset,
        definition->effective_engine_name,
        lengths->effective_engine_name_size
    );

    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET,
        (unsigned)(record_count + 1ULL)
    );
    put_u32_le(
        catalog_page,
        MYLITE_STORAGE_FORMAT_CATALOG_USED_BYTES_OFFSET,
        (unsigned)(used_bytes + record_size)
    );
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result next_table_id(
    FILE *file,
    const mylite_storage_header *header,
    const unsigned char *catalog_page,
    unsigned long long *out_table_id
) {
    unsigned long long max_table_id = catalog_max_table_id(catalog_page);
    mylite_storage_result result = read_max_row_table_id(file, header, &max_table_id);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (max_table_id == ULLONG_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    *out_table_id = max_table_id + 1ULL;
    return MYLITE_STORAGE_OK;
}

static unsigned long long catalog_max_table_id(const unsigned char *catalog_page) {
    unsigned long long max_table_id = 0ULL;
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        const unsigned long long table_id =
            get_u64_le(record, MYLITE_STORAGE_FORMAT_RECORD_TABLE_ID_OFFSET);
        if (table_id > max_table_id) {
            max_table_id = table_id;
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }

    return max_table_id;
}

static mylite_storage_result read_max_row_table_id(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long *inout_table_id
) {
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_row_page row_page = {0};
        mylite_storage_result result = read_row_page(file, header, page_id, page, &row_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (row_page.table_id > *inout_table_id) {
            *inout_table_id = row_page.table_id;
        }
        free(row_page.owned_payload);
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_definition_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    const unsigned char *definition,
    size_t definition_size
) {
    return write_blob_pages(
        file,
        first_page_id,
        (mylite_storage_blob_write){
            .payload = definition,
            .payload_size = definition_size,
            .page_type = MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_TABLE_DEFINITION,
        }
    );
}

static mylite_storage_result write_blob_pages(
    FILE *file,
    unsigned long long first_page_id,
    mylite_storage_blob_write blob
) {
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    size_t written = 0U;
    unsigned long long page_id = first_page_id;
    while (written < blob.payload_size) {
        const size_t remaining = blob.payload_size - written;
        const size_t chunk_size = remaining < payload_capacity ? remaining : payload_capacity;
        const unsigned long long next_page_id =
            written + chunk_size < blob.payload_size ? page_id + 1ULL : 0ULL;
        unsigned char blob_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_blob_page(
            blob_page,
            page_id,
            next_page_id,
            blob.payload + written,
            chunk_size,
            blob.page_type
        );

        const mylite_storage_result result =
            write_page_at(file, page_id, MYLITE_STORAGE_FORMAT_PAGE_SIZE, blob_page);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        written += chunk_size;
        ++page_id;
    }

    return MYLITE_STORAGE_OK;
}

static void encode_blob_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long next_page_id,
    const unsigned char *payload,
    size_t payload_size,
    unsigned page_type
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET, k_blob_magic, sizeof(k_blob_magic));
    put_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_OFFSET, page_type);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_BLOB_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_NEXT_PAGE_OFFSET, next_page_id);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_SIZE_OFFSET, (unsigned)payload_size);
    memcpy(page + MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET, payload, payload_size);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET,
        checksum_page(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result find_table_id(
    FILE *file,
    const mylite_storage_header *header,
    const char *schema_name,
    const char *table_name,
    unsigned long long *out_table_id
) {
    unsigned char catalog_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_catalog_entry entry = {0};
    mylite_storage_result result = read_catalog_root(file, header, catalog_page);
    if (result == MYLITE_STORAGE_OK) {
        result = find_table_record(catalog_page, schema_name, table_name, &entry);
    }
    if (result == MYLITE_STORAGE_OK) {
        *out_table_id = entry.table_id;
    }
    return result;
}

static void encode_row_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    mylite_storage_row_write row
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(page + MYLITE_STORAGE_FORMAT_ROW_MAGIC_OFFSET, k_row_magic, sizeof(k_row_magic));
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_TABLE_ROWS
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_TABLE_ID_OFFSET, table_id);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_SIZE_OFFSET, (unsigned)row.row_size);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_COUNT_OFFSET, 1U);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_OVERFLOW_ROOT_PAGE_OFFSET, row.overflow_root_page);
    if (row.overflow_root_page == 0ULL) {
        memcpy(page + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET, row.row, row.row_size);
    }
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET,
        checksum_page(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result write_row_payload_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    const unsigned char *row,
    size_t row_size,
    mylite_storage_row_write_position *out_position
) {
    const size_t row_payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    const size_t blob_payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET;
    unsigned long long first_blob_page = 0ULL;
    unsigned long long row_page_id = header->page_count;
    if (row_size > row_payload_capacity) {
        const unsigned long long blob_page_count = ((row_size - 1U) / blob_payload_capacity) + 1U;
        first_blob_page = header->page_count;
        if (blob_page_count > ULLONG_MAX - header->page_count) {
            return MYLITE_STORAGE_FULL;
        }
        row_page_id = header->page_count + blob_page_count;
    }
    if (row_page_id == ULLONG_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    if (first_blob_page != 0ULL) {
        mylite_storage_result result = write_blob_pages(
            file,
            first_blob_page,
            (mylite_storage_blob_write){
                .payload = row,
                .payload_size = row_size,
                .page_type = MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_ROW_PAYLOAD,
            }
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }

    unsigned char row_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    encode_row_page(
        row_page,
        row_page_id,
        table_id,
        (mylite_storage_row_write){
            .row = row,
            .row_size = row_size,
            .overflow_root_page = first_blob_page,
        }
    );

    mylite_storage_result result = write_page_at(file, row_page_id, header->page_size, row_page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    *out_position = (mylite_storage_row_write_position){
        .row_page_id = row_page_id,
        .next_page_id = row_page_id + 1ULL,
    };
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result write_index_entry_pages(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_index_entry_write write,
    unsigned long long *out_next_page_id
) {
    if (write.index_entry_count > ULLONG_MAX - write.first_page_id) {
        return MYLITE_STORAGE_FULL;
    }

    for (size_t i = 0U; i < write.index_entry_count; ++i) {
        const unsigned long long page_id = write.first_page_id + (unsigned long long)i;
        unsigned char index_page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        encode_index_entry_page(
            index_page,
            page_id,
            write.table_id,
            write.row_id,
            write.index_entries + i
        );

        const mylite_storage_result result =
            write_page_at(file, page_id, header->page_size, index_page);
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }

    *out_next_page_id = write.first_page_id + (unsigned long long)write.index_entry_count;
    return MYLITE_STORAGE_OK;
}

static void encode_index_entry_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned long long row_id,
    const mylite_storage_index_entry *index_entry
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(page + MYLITE_STORAGE_FORMAT_INDEX_MAGIC_OFFSET, k_index_magic, sizeof(k_index_magic));
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_TABLE_ID_OFFSET, table_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_ROW_ID_OFFSET, row_id);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_NUMBER_OFFSET, index_entry->index_number);
    put_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_KEY_SIZE_OFFSET, (unsigned)index_entry->key_size);
    memcpy(page + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET, index_entry->key, index_entry->key_size);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET,
        checksum_page(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result read_index_entry_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_index_entry_page *out_index_entry_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (!is_index_entry_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_row_page(page) || is_autoincrement_page(page) || is_row_state_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_OFFSET);
    const unsigned page_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_PAGE_ID_OFFSET);
    const unsigned long long table_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_TABLE_ID_OFFSET);
    const unsigned long long row_id = get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_ROW_ID_OFFSET);
    const size_t key_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_KEY_SIZE_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET);
    const size_t key_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET;
    if (page_type != MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || row_id <= header->catalog_root_page || row_id >= header->page_count ||
        key_size == 0U || key_size > key_capacity || expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_index_entry_page = (mylite_storage_index_entry_page){
        .table_id = table_id,
        .row_id = row_id,
        .index_number = get_u32_le(page, MYLITE_STORAGE_FORMAT_INDEX_NUMBER_OFFSET),
        .key_size = key_size,
        .key = page + MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET,
    };
    return MYLITE_STORAGE_OK;
}

static int is_index_entry_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_INDEX_MAGIC_OFFSET,
               k_index_magic,
               sizeof(k_index_magic)
           ) == 0;
}

static mylite_storage_result scan_table_row_pages(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_page_callback callback,
    void *ctx
) {
    mylite_storage_row_state_map row_state_map = {0};
    mylite_storage_result result = build_row_state_map(file, header, table_id, &row_state_map);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }

    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_row_page row_page = {0};
        result = read_row_page(file, header, page_id, page, &row_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            free_row_state_map(&row_state_map);
            return result;
        }
        if (row_page.table_id == table_id &&
            find_row_state_entry(&row_state_map, row_page.row_id) == NULL) {
            result = callback(ctx, &row_page);
            free(row_page.owned_payload);
            if (result != MYLITE_STORAGE_OK) {
                free_row_state_map(&row_state_map);
                return result;
            }
        } else {
            free(row_page.owned_payload);
        }
    }

    free_row_state_map(&row_state_map);
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_row_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_row_page *out_row_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (!is_row_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_autoincrement_page(page) || is_row_state_page(page) || is_index_entry_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_OFFSET);
    const unsigned page_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_PAGE_ID_OFFSET);
    const unsigned long long table_id = get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_TABLE_ID_OFFSET);
    const size_t row_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_SIZE_OFFSET);
    const size_t row_count = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_RECORD_COUNT_OFFSET);
    const unsigned long long overflow_root_page =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_OVERFLOW_ROOT_PAGE_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET);
    const size_t payload_capacity =
        MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    if (page_type != MYLITE_STORAGE_FORMAT_ROW_PAGE_TYPE_TABLE_ROWS || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || expected_checksum != actual_checksum || row_size == 0U ||
        row_count != 1U) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (overflow_root_page == 0ULL && row_count > payload_capacity / row_size) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (overflow_root_page != 0ULL &&
        (row_count != 1U || overflow_root_page <= header->catalog_root_page ||
         overflow_root_page >= header->page_count)) {
        return MYLITE_STORAGE_CORRUPT;
    }

    unsigned char *owned_payload = NULL;
    const unsigned char *payload = page + MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET;
    if (overflow_root_page != 0ULL) {
        mylite_storage_result payload_result = read_row_payload_blob_pages(
            file,
            header,
            (mylite_storage_blob_chain){
                .first_page_id = overflow_root_page,
                .payload_size = row_size,
                .page_type = MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_ROW_PAYLOAD,
            },
            &owned_payload
        );
        if (payload_result != MYLITE_STORAGE_OK) {
            return payload_result;
        }
        payload = owned_payload;
    }

    *out_row_page = (mylite_storage_row_page){
        .row_id = page_id,
        .table_id = table_id,
        .row_size = row_size,
        .row_count = row_count,
        .payload = payload,
        .owned_payload = owned_payload,
    };
    return MYLITE_STORAGE_OK;
}

static int is_row_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_ROW_MAGIC_OFFSET,
               k_row_magic,
               sizeof(k_row_magic)
           ) == 0;
}

static mylite_storage_result validate_live_row(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_row_state_map *row_state_map,
    mylite_storage_live_row_request request,
    mylite_storage_row_page *out_row_page
) {
    if (request.row_id <= header->catalog_root_page || request.row_id >= header->page_count) {
        return MYLITE_STORAGE_NOTFOUND;
    }
    if (find_row_state_entry(row_state_map, request.row_id) != NULL) {
        return MYLITE_STORAGE_NOTFOUND;
    }

    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_row_page row_page = {0};
    mylite_storage_result result = read_row_page(file, header, request.row_id, page, &row_page);
    if (result == MYLITE_STORAGE_NOTFOUND) {
        return MYLITE_STORAGE_NOTFOUND;
    }
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (row_page.table_id != request.table_id) {
        free(row_page.owned_payload);
        return MYLITE_STORAGE_NOTFOUND;
    }

    *out_row_page = row_page;
    return MYLITE_STORAGE_OK;
}

static void encode_row_state_page(
    unsigned char *page,
    unsigned long long page_id,
    const mylite_storage_row_state_page *row_state_page
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_ROW_STATE_MAGIC_OFFSET,
        k_row_state_magic,
        sizeof(k_row_state_magic)
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_TABLE_ROWS
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_TABLE_ID_OFFSET, row_state_page->table_id);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_SOURCE_ROW_ID_OFFSET,
        row_state_page->source_row_id
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_REPLACEMENT_ROW_ID_OFFSET,
        row_state_page->replacement_row_id
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_OFFSET, row_state_page->state_kind);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET,
        checksum_page(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result build_row_state_map(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    mylite_storage_row_state_map *out_row_state_map
) {
    *out_row_state_map = (mylite_storage_row_state_map){0};
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_row_state_page row_state_page = {0};
        mylite_storage_result result =
            read_row_state_page(file, header, page_id, page, &row_state_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            free_row_state_map(out_row_state_map);
            return result;
        }
        if (row_state_page.table_id != table_id) {
            continue;
        }

        result = set_row_state_entry(out_row_state_map, &row_state_page);
        if (result != MYLITE_STORAGE_OK) {
            free_row_state_map(out_row_state_map);
            return result;
        }
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_row_state_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_row_state_page *out_row_state_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (!is_row_state_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_row_page(page) || is_autoincrement_page(page) || is_index_entry_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_OFFSET);
    const unsigned page_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_ID_OFFSET);
    const unsigned long long table_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_TABLE_ID_OFFSET);
    const unsigned long long source_row_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_SOURCE_ROW_ID_OFFSET);
    const unsigned long long replacement_row_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_REPLACEMENT_ROW_ID_OFFSET);
    const unsigned state_kind = get_u32_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_ROW_STATE_CHECKSUM_OFFSET);

    if (page_type != MYLITE_STORAGE_FORMAT_ROW_STATE_PAGE_TYPE_TABLE_ROWS || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || source_row_id <= header->catalog_root_page ||
        source_row_id >= header->page_count || expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }
    if (state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_DELETE) {
        if (replacement_row_id != 0ULL) {
            return MYLITE_STORAGE_CORRUPT;
        }
    } else if (state_kind == MYLITE_STORAGE_FORMAT_ROW_STATE_KIND_REPLACE) {
        if (replacement_row_id <= header->catalog_root_page ||
            replacement_row_id >= header->page_count || replacement_row_id == source_row_id) {
            return MYLITE_STORAGE_CORRUPT;
        }
    } else {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_row_state_page = (mylite_storage_row_state_page){
        .table_id = table_id,
        .source_row_id = source_row_id,
        .replacement_row_id = replacement_row_id,
        .state_kind = state_kind,
    };
    return MYLITE_STORAGE_OK;
}

static int is_row_state_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_ROW_STATE_MAGIC_OFFSET,
               k_row_state_magic,
               sizeof(k_row_state_magic)
           ) == 0;
}

static mylite_storage_row_state_entry *find_row_state_entry(
    const mylite_storage_row_state_map *row_state_map,
    unsigned long long row_id
) {
    for (size_t i = 0U; i < row_state_map->count; ++i) {
        if (row_state_map->entries[i].source_row_id == row_id) {
            return row_state_map->entries + i;
        }
    }

    return NULL;
}

static mylite_storage_result set_row_state_entry(
    mylite_storage_row_state_map *row_state_map,
    const mylite_storage_row_state_page *row_state_page
) {
    mylite_storage_row_state_entry *entry =
        find_row_state_entry(row_state_map, row_state_page->source_row_id);
    if (entry != NULL) {
        *entry = (mylite_storage_row_state_entry){
            .source_row_id = row_state_page->source_row_id,
            .replacement_row_id = row_state_page->replacement_row_id,
            .state_kind = row_state_page->state_kind,
        };
        return MYLITE_STORAGE_OK;
    }

    if (row_state_map->count == SIZE_MAX) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_count = row_state_map->count + 1U;
    mylite_storage_row_state_entry *entries = (mylite_storage_row_state_entry *)
        realloc(row_state_map->entries, new_count * sizeof(mylite_storage_row_state_entry));
    if (entries == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    row_state_map->entries = entries;
    row_state_map->entries[row_state_map->count] = (mylite_storage_row_state_entry){
        .source_row_id = row_state_page->source_row_id,
        .replacement_row_id = row_state_page->replacement_row_id,
        .state_kind = row_state_page->state_kind,
    };
    row_state_map->count = new_count;
    return MYLITE_STORAGE_OK;
}

static void free_row_state_map(mylite_storage_row_state_map *row_state_map) {
    free(row_state_map->entries);
    *row_state_map = (mylite_storage_row_state_map){0};
}

static mylite_storage_result append_row_page_to_rowset(
    void *ctx,
    const mylite_storage_row_page *row_page
) {
    mylite_storage_rowset *rowset = (mylite_storage_rowset *)ctx;
    for (size_t i = 0U; i < row_page->row_count; ++i) {
        const mylite_storage_result result = append_row_to_rowset(
            rowset,
            row_page->row_id,
            row_page->payload + (i * row_page->row_size),
            row_page->row_size
        );
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_row_to_rowset(
    mylite_storage_rowset *rowset,
    unsigned long long row_id,
    const unsigned char *row,
    size_t row_size
) {
    if (rowset->row_count == SIZE_MAX || row_size > SIZE_MAX - rowset->row_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_row_count = rowset->row_count + 1U;
    const size_t new_row_bytes = rowset->row_bytes + row_size;

    unsigned char *rows = (unsigned char *)realloc(rowset->rows, new_row_bytes);
    if (rows == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    rowset->rows = rows;

    size_t *row_offsets = (size_t *)realloc(rowset->row_offsets, new_row_count * sizeof(size_t));
    if (row_offsets == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    rowset->row_offsets = row_offsets;

    size_t *row_sizes = (size_t *)realloc(rowset->row_sizes, new_row_count * sizeof(size_t));
    if (row_sizes == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    rowset->row_sizes = row_sizes;

    unsigned long long *row_ids =
        (unsigned long long *)realloc(rowset->row_ids, new_row_count * sizeof(unsigned long long));
    if (row_ids == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    rowset->row_ids = row_ids;

    memcpy(rowset->rows + rowset->row_bytes, row, row_size);
    rowset->row_offsets[rowset->row_count] = rowset->row_bytes;
    rowset->row_sizes[rowset->row_count] = row_size;
    rowset->row_ids[rowset->row_count] = row_id;
    rowset->row_count = new_row_count;
    rowset->row_bytes = new_row_bytes;
    if (rowset->row_count == 1U) {
        rowset->row_size = row_size;
    } else if (rowset->row_size != row_size) {
        rowset->row_size = 0U;
    }
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result append_index_entry_to_entryset(
    mylite_storage_index_entryset *entryset,
    const mylite_storage_index_entry_page *entry_page
) {
    if (entryset->entry_count == SIZE_MAX ||
        entry_page->key_size > SIZE_MAX - entryset->key_bytes) {
        return MYLITE_STORAGE_FULL;
    }

    const size_t new_entry_count = entryset->entry_count + 1U;
    const size_t new_key_bytes = entryset->key_bytes + entry_page->key_size;
    if (new_entry_count > SIZE_MAX / sizeof(size_t) ||
        new_entry_count > SIZE_MAX / sizeof(unsigned long long)) {
        return MYLITE_STORAGE_FULL;
    }

    unsigned char *keys = (unsigned char *)realloc(entryset->keys, new_key_bytes);
    if (keys == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->keys = keys;

    size_t *key_offsets =
        (size_t *)realloc(entryset->key_offsets, new_entry_count * sizeof(size_t));
    if (key_offsets == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->key_offsets = key_offsets;

    size_t *key_sizes = (size_t *)realloc(entryset->key_sizes, new_entry_count * sizeof(size_t));
    if (key_sizes == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->key_sizes = key_sizes;

    unsigned long long *row_ids = (unsigned long long *)
        realloc(entryset->row_ids, new_entry_count * sizeof(unsigned long long));
    if (row_ids == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }
    entryset->row_ids = row_ids;

    memcpy(entryset->keys + entryset->key_bytes, entry_page->key, entry_page->key_size);
    entryset->key_offsets[entryset->entry_count] = entryset->key_bytes;
    entryset->key_sizes[entryset->entry_count] = entry_page->key_size;
    entryset->row_ids[entryset->entry_count] = entry_page->row_id;
    entryset->entry_count = new_entry_count;
    entryset->key_bytes = new_key_bytes;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result count_row_page(void *ctx, const mylite_storage_row_page *row_page) {
    unsigned long long *row_count = (unsigned long long *)ctx;
    if (row_page->row_count > ULLONG_MAX - *row_count) {
        return MYLITE_STORAGE_FULL;
    }

    *row_count += (unsigned long long)row_page->row_count;
    return MYLITE_STORAGE_OK;
}

static void encode_autoincrement_page(
    unsigned char *page,
    unsigned long long page_id,
    unsigned long long table_id,
    unsigned long long next_value
) {
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_AUTOINCREMENT_MAGIC_OFFSET,
        k_autoincrement_magic,
        sizeof(k_autoincrement_magic)
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_TYPE_OFFSET,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_TYPE_TABLE_STATE
    );
    put_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_VERSION_OFFSET, 1U);
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_ID_OFFSET, page_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_TABLE_ID_OFFSET, table_id);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_NEXT_VALUE_OFFSET, next_value);
    put_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET, 0ULL);
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET,
        checksum_page(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET)
    );
}

static mylite_storage_result read_autoincrement_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned char *page,
    mylite_storage_autoincrement_page *out_autoincrement_page
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (!is_autoincrement_page(page)) {
        if (memcmp(
                page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
                k_blob_magic,
                sizeof(k_blob_magic)
            ) == 0 ||
            is_row_page(page) || is_row_state_page(page) || is_index_entry_page(page)) {
            return MYLITE_STORAGE_NOTFOUND;
        }
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_TYPE_OFFSET);
    const unsigned page_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_ID_OFFSET);
    const unsigned long long table_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_TABLE_ID_OFFSET);
    const unsigned long long next_value =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_NEXT_VALUE_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_AUTOINCREMENT_CHECKSUM_OFFSET);
    if (page_type != MYLITE_STORAGE_FORMAT_AUTOINCREMENT_PAGE_TYPE_TABLE_STATE ||
        page_version != 1U || format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        table_id == 0ULL || next_value == 0ULL || expected_checksum != actual_checksum) {
        return MYLITE_STORAGE_CORRUPT;
    }

    *out_autoincrement_page = (mylite_storage_autoincrement_page){
        .table_id = table_id,
        .next_value = next_value,
    };
    return MYLITE_STORAGE_OK;
}

static int is_autoincrement_page(const unsigned char *page) {
    return memcmp(
               page + MYLITE_STORAGE_FORMAT_AUTOINCREMENT_MAGIC_OFFSET,
               k_autoincrement_magic,
               sizeof(k_autoincrement_magic)
           ) == 0;
}

static mylite_storage_result latest_auto_increment_value(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long table_id,
    unsigned long long *out_next_value
) {
    *out_next_value = 1ULL;
    for (unsigned long long page_id = MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT;
         page_id < header->page_count;
         ++page_id) {
        unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
        mylite_storage_autoincrement_page autoincrement_page = {0};
        mylite_storage_result result =
            read_autoincrement_page(file, header, page_id, page, &autoincrement_page);
        if (result == MYLITE_STORAGE_NOTFOUND) {
            continue;
        }
        if (result != MYLITE_STORAGE_OK) {
            return result;
        }
        if (autoincrement_page.table_id == table_id) {
            *out_next_value = autoincrement_page.next_value;
        }
    }

    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_definition_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    const mylite_storage_catalog_entry *entry,
    unsigned char **out_definition,
    size_t *out_definition_size
) {
    if (entry->definition_size > SIZE_MAX) {
        return MYLITE_STORAGE_UNSUPPORTED;
    }

    unsigned char *definition = (unsigned char *)malloc((size_t)entry->definition_size);
    if (definition == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    size_t written = 0U;
    unsigned long long page_id = entry->definition_root_page;
    while (written < (size_t)entry->definition_size) {
        unsigned long long next_page_id = 0ULL;
        const mylite_storage_result result = read_blob_page(
            file,
            header,
            page_id,
            entry->definition_size - written,
            definition,
            &written,
            &next_page_id,
            MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_TABLE_DEFINITION
        );
        if (result != MYLITE_STORAGE_OK) {
            free(definition);
            return result;
        }
        if (next_page_id == 0ULL && written < (size_t)entry->definition_size) {
            free(definition);
            return MYLITE_STORAGE_CORRUPT;
        }
        page_id = next_page_id;
    }

    *out_definition = definition;
    *out_definition_size = (size_t)entry->definition_size;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_row_payload_blob_pages(
    FILE *file,
    const mylite_storage_header *header,
    mylite_storage_blob_chain chain,
    unsigned char **out_payload
) {
    unsigned char *payload = (unsigned char *)malloc(chain.payload_size);
    if (payload == NULL) {
        return MYLITE_STORAGE_NOMEM;
    }

    size_t written = 0U;
    unsigned long long page_id = chain.first_page_id;
    while (written < chain.payload_size) {
        unsigned long long next_page_id = 0ULL;
        const mylite_storage_result result = read_blob_page(
            file,
            header,
            page_id,
            chain.payload_size - written,
            payload,
            &written,
            &next_page_id,
            chain.page_type
        );
        if (result != MYLITE_STORAGE_OK) {
            free(payload);
            return result;
        }
        if (next_page_id == 0ULL && written < chain.payload_size) {
            free(payload);
            return MYLITE_STORAGE_CORRUPT;
        }
        page_id = next_page_id;
    }

    *out_payload = payload;
    return MYLITE_STORAGE_OK;
}

static mylite_storage_result read_blob_page(
    FILE *file,
    const mylite_storage_header *header,
    unsigned long long page_id,
    unsigned long long expected_remaining,
    unsigned char *out_payload,
    size_t *inout_written,
    unsigned long long *out_next_page_id,
    unsigned expected_page_type
) {
    if (page_id <= header->catalog_root_page || page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }

    unsigned char page[MYLITE_STORAGE_FORMAT_PAGE_SIZE];
    mylite_storage_result result = read_page_at(file, page_id, header->page_size, page);
    if (result != MYLITE_STORAGE_OK) {
        return result;
    }
    if (memcmp(
            page + MYLITE_STORAGE_FORMAT_BLOB_MAGIC_OFFSET,
            k_blob_magic,
            sizeof(k_blob_magic)
        ) != 0) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned page_type = get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_TYPE_OFFSET);
    const unsigned page_version = get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_VERSION_OFFSET);
    const unsigned format_version =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_FORMAT_VERSION_OFFSET);
    const unsigned checksum_algorithm =
        get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_ALGORITHM_OFFSET);
    const unsigned long long stored_page_id =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAGE_ID_OFFSET);
    const unsigned payload_size = get_u32_le(page, MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_SIZE_OFFSET);
    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_BLOB_CHECKSUM_OFFSET);
    if (page_type != expected_page_type || page_version != 1U ||
        format_version != MYLITE_STORAGE_FORMAT_VERSION ||
        checksum_algorithm != MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64 || stored_page_id != page_id ||
        expected_checksum != actual_checksum ||
        payload_size >
            MYLITE_STORAGE_FORMAT_PAGE_SIZE - MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET ||
        payload_size == 0U || payload_size > expected_remaining) {
        return MYLITE_STORAGE_CORRUPT;
    }

    memcpy(
        out_payload + *inout_written,
        page + MYLITE_STORAGE_FORMAT_BLOB_PAYLOAD_OFFSET,
        payload_size
    );
    *inout_written += payload_size;
    *out_next_page_id = get_u64_le(page, MYLITE_STORAGE_FORMAT_BLOB_NEXT_PAGE_OFFSET);
    if (*out_next_page_id != 0ULL && *out_next_page_id >= header->page_count) {
        return MYLITE_STORAGE_CORRUPT;
    }
    return MYLITE_STORAGE_OK;
}

static char *copy_record_field(const unsigned char *record, unsigned field_index) {
    const size_t field_size = record_field_size(record, field_index);
    char *copy = (char *)malloc(field_size + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, record + record_field_offset(record, field_index), field_size);
    copy[field_size] = '\0';
    return copy;
}

static mylite_storage_result list_catalog_tables(
    const unsigned char *catalog_page,
    const char *schema_name,
    mylite_storage_table_callback callback,
    void *ctx
) {
    size_t offset = MYLITE_STORAGE_FORMAT_CATALOG_HEADER_SIZE;
    const unsigned long long record_count = catalog_record_count(catalog_page);
    for (unsigned long long i = 0ULL; i < record_count; ++i) {
        const unsigned char *record = catalog_page + offset;
        const char *record_schema = (const char *)(record + record_field_offset(record, 0U));
        const size_t record_schema_size = record_field_size(record, 0U);
        if (strlen(schema_name) == record_schema_size &&
            memcmp(record_schema, schema_name, record_schema_size) == 0) {
            char *schema_copy = copy_record_field(record, 0U);
            char *table_copy = copy_record_field(record, 1U);
            if (schema_copy == NULL || table_copy == NULL) {
                free(schema_copy);
                free(table_copy);
                return MYLITE_STORAGE_NOMEM;
            }
            const int callback_result = callback(ctx, schema_copy, table_copy);
            free(schema_copy);
            free(table_copy);
            if (callback_result != 0) {
                return MYLITE_STORAGE_ERROR;
            }
        }
        offset += get_u32_le(record, MYLITE_STORAGE_FORMAT_RECORD_SIZE_OFFSET);
    }
    return MYLITE_STORAGE_OK;
}

static unsigned get_u32_le(const unsigned char *page, size_t offset) {
    unsigned value = 0U;
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        value |= (unsigned)page[offset + i] << (unsigned)(i * CHAR_BIT);
    }
    return value;
}

static unsigned long long get_u64_le(const unsigned char *page, size_t offset) {
    unsigned long long value = 0ULL;
    for (size_t i = 0U; i < sizeof(uint64_t); ++i) {
        value |= (unsigned long long)page[offset + i] << (unsigned)(i * CHAR_BIT);
    }
    return value;
}

static void put_u32_le(unsigned char *page, size_t offset, unsigned value) {
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        page[offset + i] = (unsigned char)((value >> (unsigned)(i * CHAR_BIT)) & UINT8_MAX);
    }
}

static void put_u64_le(unsigned char *page, size_t offset, unsigned long long value) {
    for (size_t i = 0U; i < sizeof(uint64_t); ++i) {
        page[offset + i] = (unsigned char)((value >> (unsigned)(i * CHAR_BIT)) & UINT8_MAX);
    }
}

static uint64_t checksum_page(const unsigned char *page, size_t checksum_offset) {
    uint64_t checksum = UINT64_C(1469598103934665603);
    for (size_t i = 0U; i < MYLITE_STORAGE_FORMAT_PAGE_SIZE; ++i) {
        const unsigned char byte =
            i >= checksum_offset && i < checksum_offset + sizeof(uint64_t) ? 0U : page[i];
        checksum ^= byte;
        checksum *= UINT64_C(1099511628211);
    }
    return checksum;
}

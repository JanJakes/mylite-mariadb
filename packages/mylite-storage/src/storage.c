#include "storage_format.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static mylite_storage_result path_exists(const char *filename, int *exists);
static mylite_storage_result write_empty_database(FILE *file);
static void initialize_header_page(unsigned char *page);
static void initialize_empty_catalog_page(unsigned char *page);
static mylite_storage_result write_page(FILE *file, const unsigned char *page, size_t size);
static mylite_storage_result close_created_file(FILE *file, const char *filename);
static mylite_storage_result open_existing_file(const char *filename, FILE **out_file);
static mylite_storage_result read_header(FILE *file, mylite_storage_header *out_header);
static mylite_storage_result read_page_at(
    FILE *file,
    unsigned long long page_id,
    unsigned page_size,
    unsigned char *out_page
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
static unsigned get_u32_le(const unsigned char *page, size_t offset);
static unsigned long long get_u64_le(const unsigned char *page, size_t offset);
static void put_u32_le(unsigned char *page, size_t offset, unsigned value);
static void put_u64_le(unsigned char *page, size_t offset, unsigned long long value);
static uint64_t checksum_page(const unsigned char *page, size_t checksum_offset);

static const unsigned char k_header_magic[8] = {'M', 'Y', 'L', 'I', 'T', 'E', '1', '\0'};
static const unsigned char k_catalog_magic[8] = {'M', 'Y', 'L', 'C', 'A', 'T', '1', '\0'};

const char *mylite_storage_engine_name(void) {
    return MYLITE_STORAGE_ENGINE_NAME;
}

mylite_storage_capabilities mylite_storage_get_capabilities(void) {
    mylite_storage_capabilities capabilities = {
        .size = sizeof(capabilities),
        .format_version = MYLITE_STORAGE_FORMAT_VERSION,
        .flags = MYLITE_STORAGE_CAPABILITY_FILE_HEADER | MYLITE_STORAGE_CAPABILITY_EMPTY_CATALOG,
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
    memset(page, 0, MYLITE_STORAGE_FORMAT_PAGE_SIZE);
    memcpy(
        page + MYLITE_STORAGE_FORMAT_HEADER_MAGIC_OFFSET,
        k_header_magic,
        sizeof(k_header_magic)
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_HEADER_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_FORMAT_VERSION_OFFSET,
        MYLITE_STORAGE_FORMAT_VERSION
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_SIZE_OFFSET,
        MYLITE_STORAGE_FORMAT_PAGE_SIZE
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_BYTE_ORDER_OFFSET,
        MYLITE_STORAGE_FORMAT_BYTE_ORDER_MARKER
    );
    put_u32_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CHECKSUM_ALGORITHM_OFFSET,
        MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CATALOG_ROOT_PAGE_OFFSET,
        MYLITE_STORAGE_FORMAT_CATALOG_ROOT_PAGE_ID
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_CATALOG_GENERATION_OFFSET,
        MYLITE_STORAGE_FORMAT_EMPTY_CATALOG_GENERATION
    );
    put_u64_le(
        page,
        MYLITE_STORAGE_FORMAT_HEADER_PAGE_COUNT_OFFSET,
        MYLITE_STORAGE_FORMAT_EMPTY_PAGE_COUNT
    );
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
    const unsigned long long record_count =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_RECORD_COUNT_OFFSET);
    const unsigned long long next_page =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_NEXT_PAGE_OFFSET);
    if (page_id != header->catalog_root_page || generation != header->catalog_generation ||
        record_count != 0ULL || next_page != 0ULL) {
        return MYLITE_STORAGE_CORRUPT;
    }

    const unsigned long long expected_checksum =
        get_u64_le(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET);
    const unsigned long long actual_checksum =
        checksum_page(page, MYLITE_STORAGE_FORMAT_CATALOG_CHECKSUM_OFFSET);
    return expected_checksum == actual_checksum ? MYLITE_STORAGE_OK : MYLITE_STORAGE_CORRUPT;
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

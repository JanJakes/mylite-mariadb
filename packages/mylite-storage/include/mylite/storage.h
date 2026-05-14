#ifndef MYLITE_STORAGE_H
#define MYLITE_STORAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_STORAGE_ENGINE_NAME "MYLITE"
#define MYLITE_STORAGE_FORMAT_VERSION 1U
#define MYLITE_STORAGE_CAPABILITY_FILE_HEADER 0x00000001U
#define MYLITE_STORAGE_CAPABILITY_EMPTY_CATALOG 0x00000002U

typedef enum mylite_storage_result { /* NOLINT(performance-enum-size): C ABI enum. */
                                     MYLITE_STORAGE_OK = 0,
                                     MYLITE_STORAGE_ERROR = 1,
                                     MYLITE_STORAGE_NOMEM = 7,
                                     MYLITE_STORAGE_READONLY = 8,
                                     MYLITE_STORAGE_IOERR = 10,
                                     MYLITE_STORAGE_CORRUPT = 11,
                                     MYLITE_STORAGE_NOTFOUND = 12,
                                     MYLITE_STORAGE_MISUSE = 21,
                                     MYLITE_STORAGE_UNSUPPORTED = 22
} mylite_storage_result;

typedef struct mylite_storage_capabilities {
    size_t size;
    unsigned format_version;
    unsigned flags;
} mylite_storage_capabilities;

typedef struct mylite_storage_header {
    size_t size;
    unsigned format_version;
    unsigned header_version;
    unsigned page_size;
    unsigned checksum_algorithm;
    unsigned flags;
    unsigned long long catalog_root_page;
    unsigned long long catalog_generation;
    unsigned long long free_list_root_page;
    unsigned long long page_count;
} mylite_storage_header;

const char *mylite_storage_engine_name(void);
mylite_storage_capabilities mylite_storage_get_capabilities(void);
mylite_storage_result mylite_storage_create_empty(const char *filename);
mylite_storage_result mylite_storage_open_header(
    const char *filename,
    mylite_storage_header *out_header
);

#ifdef __cplusplus
}
#endif

#endif

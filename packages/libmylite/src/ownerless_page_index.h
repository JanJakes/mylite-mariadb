#ifndef MYLITE_OWNERLESS_PAGE_INDEX_H
#define MYLITE_OWNERLESS_PAGE_INDEX_H

#include "ownerless_latch.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_PAGE_INDEX_OK 0
#define MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND 1
#define MYLITE_OWNERLESS_PAGE_INDEX_FULL 2
#define MYLITE_OWNERLESS_PAGE_INDEX_ERROR 3

#define MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE 96U
#define MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE 64U

typedef struct mylite_ownerless_page_index_record {
    uint32_t space_id;
    uint32_t page_no;
    uint64_t commit_lsn;
    uint64_t page_lsn;
    uint64_t record_offset;
} mylite_ownerless_page_index_record;

int mylite_ownerless_page_index_initialize(void *index, size_t index_size, uint32_t entry_count);
int mylite_ownerless_page_index_publish(
    void *index,
    size_t index_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t commit_lsn,
    uint64_t page_lsn,
    uint64_t record_offset
);
int mylite_ownerless_page_index_require_wal_scan(
    void *index,
    size_t index_size,
    uint32_t owner_id,
    uint64_t owner_generation
);
int mylite_ownerless_page_index_clear(
    void *index,
    size_t index_size,
    uint32_t owner_id,
    uint64_t owner_generation
);
int mylite_ownerless_page_index_replace(
    void *index,
    size_t index_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    const mylite_ownerless_page_index_record *records,
    size_t record_count
);
int mylite_ownerless_page_index_find(
    void *index,
    size_t index_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    uint64_t *out_record_offset,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);

#ifdef __cplusplus
}
#endif

#endif

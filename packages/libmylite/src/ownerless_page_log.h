#ifndef MYLITE_OWNERLESS_PAGE_LOG_H
#define MYLITE_OWNERLESS_PAGE_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_PAGE_LOG_OK 0
#define MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND 1
#define MYLITE_OWNERLESS_PAGE_LOG_FULL 2
#define MYLITE_OWNERLESS_PAGE_LOG_ERROR 3

#define MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE 64U
#define MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE 64U

typedef int (*mylite_ownerless_page_log_replay_callback)(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    uint64_t record_offset,
    void *context);

int mylite_ownerless_page_log_initialize(int fd);
int mylite_ownerless_page_log_initialize_at(int fd, uint64_t log_offset);
int mylite_ownerless_page_log_append(
    int fd,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_append_at(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t commit_lsn,
    const void *page,
    uint32_t page_size,
    uint64_t *out_record_offset
);
int mylite_ownerless_page_log_snapshot(int fd, uint64_t *out_snapshot_end_offset);
int mylite_ownerless_page_log_snapshot_at(
    int fd,
    uint64_t log_offset,
    uint64_t *out_snapshot_end_offset
);
int mylite_ownerless_page_log_find_latest(
    int fd,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_find_latest_at(
    int fd,
    uint64_t log_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_find_latest_in_snapshot(
    int fd,
    uint64_t snapshot_end_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_find_latest_in_snapshot_at(
    int fd,
    uint64_t log_offset,
    uint64_t snapshot_end_offset,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_read_record_at(
    int fd,
    uint64_t log_offset,
    uint64_t record_offset,
    void *out_page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn
);
int mylite_ownerless_page_log_replay_at(
    int fd,
    uint64_t log_offset,
    mylite_ownerless_page_log_replay_callback callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint(
    int fd,
    uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    void *context
);
int mylite_ownerless_page_log_checkpoint_at(
    int fd,
    uint64_t log_offset,
    uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    void *context
);

#ifdef __cplusplus
}
#endif

#endif

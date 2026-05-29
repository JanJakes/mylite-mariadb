#include "ownerless_page_log.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
#  define MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS 0
#endif

namespace {

constexpr std::array<unsigned char, 8> k_header_magic = {
    'M',
    'Y',
    'L',
    'P',
    'A',
    'G',
    'E',
    '\0',
};
constexpr std::array<unsigned char, 8> k_record_magic = {
    'M',
    'Y',
    'L',
    'P',
    'G',
    'R',
    'E',
    'C',
};
constexpr std::uint32_t k_format_version = 1;
constexpr std::size_t k_header_magic_offset = 0;
constexpr std::size_t k_header_format_offset = 8;
constexpr std::size_t k_header_size_offset = 12;
constexpr std::size_t k_header_record_header_size_offset = 16;
constexpr std::size_t k_record_magic_offset = 0;
constexpr std::size_t k_record_space_id_offset = 8;
constexpr std::size_t k_record_page_no_offset = 12;
constexpr std::size_t k_record_page_size_offset = 16;
constexpr std::size_t k_record_flags_offset = 20;
constexpr std::size_t k_record_page_lsn_offset = 24;
constexpr std::size_t k_record_commit_lsn_offset = 32;
constexpr std::size_t k_record_payload_size_offset = 40;
constexpr std::size_t k_record_payload_checksum_offset = 48;
constexpr off_t k_append_lock_start = 0;
constexpr off_t k_checkpoint_lock_start = 1;

using PageLogHeader = std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE>;

struct PageRecordHeader {
    std::uint32_t space_id = 0;
    std::uint32_t page_no = 0;
    std::uint32_t page_size = 0;
    std::uint64_t page_lsn = 0;
    std::uint64_t commit_lsn = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t checksum = 0;
};

enum class PayloadStatus {
    Ok,
    Mismatch,
    Error,
};

int validate_or_create_header(int fd, off_t log_offset);
int validate_existing_header(int fd, off_t log_offset);
int append_locked(
    int fd,
    off_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
);
int snapshot_locked(int fd, off_t log_offset, std::uint64_t *out_snapshot_end_offset);
int find_latest_in_snapshot(
    int fd,
    off_t log_offset,
    off_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
);
int replay_in_snapshot(
    int fd,
    off_t log_offset,
    off_t snapshot_end_offset,
    mylite_ownerless_page_log_replay_callback callback,
    void *context
);
int read_record_at_locked(
    int fd,
    off_t log_offset,
    off_t physical_record_offset,
    bool require_page_identity,
    std::uint32_t space_id,
    std::uint32_t page_no,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
);
int checkpoint_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
);
int checkpoint_if_safe_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    int *out_checkpointed
);
bool acquire_append_lock(int fd);
bool acquire_snapshot_lock(int fd);
bool acquire_checkpoint_read_lock(int fd);
bool acquire_checkpoint_write_lock(int fd);
bool acquire_log_lock(int fd, short lock_type, off_t lock_start);
void release_log_lock(int fd, off_t lock_start);
void maybe_pause_for_test_fault(const char *fault_name);
bool read_header(int fd, off_t log_offset, PageLogHeader &header);
bool write_header(int fd, off_t log_offset);
bool header_matches(const PageLogHeader &header);
bool read_record_header(int fd, off_t offset, PageRecordHeader &header);
bool write_record_header(int fd, off_t offset, const PageRecordHeader &header);
bool write_exact_at(int fd, const void *buffer, std::size_t size, off_t offset);
bool read_exact_at(int fd, void *buffer, std::size_t size, off_t offset);
bool sync_file(int fd);
bool next_io_offset(off_t offset, std::size_t progress, off_t *out_offset);
bool offset_adds(off_t offset, std::uint64_t length, off_t *out_offset);
bool record_payload_too_large(std::uint64_t payload_size, std::size_t page_capacity);
PayloadStatus record_payload_status(int fd, off_t payload_offset, const PageRecordHeader &record);
bool record_checksum_matches(const void *page, std::uint64_t payload_size, std::uint64_t checksum);
bool record_is_better(const PageRecordHeader &candidate, const PageRecordHeader &current);
std::uint64_t checksum_bytes(const void *buffer, std::size_t size);
std::uint32_t load32(const unsigned char *bytes, std::size_t offset);
std::uint64_t load64(const unsigned char *bytes, std::size_t offset);
void store32(unsigned char *bytes, std::size_t offset, std::uint32_t value);
void store64(unsigned char *bytes, std::size_t offset, std::uint64_t value);

} // namespace

int mylite_ownerless_page_log_initialize(int fd) {
    return mylite_ownerless_page_log_initialize_at(fd, 0U);
}

int mylite_ownerless_page_log_initialize_at(int fd, std::uint64_t log_offset) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int result = validate_or_create_header(fd, static_cast<off_t>(log_offset));
    release_log_lock(fd, k_append_lock_start);
    return result;
}

int mylite_ownerless_page_log_append(
    int fd,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    return mylite_ownerless_page_log_append_at(
        fd,
        0U,
        space_id,
        page_no,
        page_lsn,
        commit_lsn,
        page,
        page_size,
        out_record_offset
    );
}

int mylite_ownerless_page_log_append_at(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    if (fd < 0 || commit_lsn == 0U || page == nullptr || page_size == 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const auto offset = static_cast<off_t>(log_offset);
    const int header_result = validate_or_create_header(fd, offset);
    const int append_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK ? append_locked(
                                                                                  fd,
                                                                                  offset,
                                                                                  space_id,
                                                                                  page_no,
                                                                                  page_lsn,
                                                                                  commit_lsn,
                                                                                  page,
                                                                                  page_size,
                                                                                  out_record_offset
                                                                              )
                                                                            : header_result;
    release_log_lock(fd, k_append_lock_start);
    return append_result;
}

int mylite_ownerless_page_log_sync(int fd) {
    return mylite_ownerless_page_log_sync_at(fd, 0U);
}

int mylite_ownerless_page_log_sync_at(int fd, std::uint64_t log_offset) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_snapshot_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    int result = validate_existing_header(fd, offset);
    if (result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        result = sync_file(fd) ? MYLITE_OWNERLESS_PAGE_LOG_OK : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    release_log_lock(fd, k_append_lock_start);
    return result;
}

int mylite_ownerless_page_log_snapshot(int fd, std::uint64_t *out_snapshot_end_offset) {
    return mylite_ownerless_page_log_snapshot_at(fd, 0U, out_snapshot_end_offset);
}

int mylite_ownerless_page_log_snapshot_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t *out_snapshot_end_offset
) {
    if (fd < 0 || out_snapshot_end_offset == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const auto offset = static_cast<off_t>(log_offset);
    if (!acquire_snapshot_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    int header_result = validate_existing_header(fd, offset);
    int snapshot_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
                              ? snapshot_locked(fd, offset, out_snapshot_end_offset)
                              : header_result;
    release_log_lock(fd, k_append_lock_start);
    if (snapshot_result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return snapshot_result;
    }

    if (!acquire_append_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    header_result = validate_or_create_header(fd, offset);
    snapshot_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
                          ? snapshot_locked(fd, offset, out_snapshot_end_offset)
                          : header_result;
    release_log_lock(fd, k_append_lock_start);
    return snapshot_result;
}

int mylite_ownerless_page_log_begin_read(int fd) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return acquire_checkpoint_read_lock(fd) ? MYLITE_OWNERLESS_PAGE_LOG_OK
                                            : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

void mylite_ownerless_page_log_end_read(int fd) {
    if (fd >= 0) {
        release_log_lock(fd, k_checkpoint_lock_start);
    }
}

int mylite_ownerless_page_log_find_latest(
    int fd,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    return mylite_ownerless_page_log_find_latest_at(
        fd,
        0U,
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );
}

int mylite_ownerless_page_log_find_latest_at(
    int fd,
    std::uint64_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || out_page == nullptr || page_capacity == 0U || out_page_size == nullptr ||
        out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    std::uint64_t snapshot_end_offset = 0;
    const int snapshot_result =
        mylite_ownerless_page_log_snapshot_at(fd, log_offset, &snapshot_end_offset);
    if (snapshot_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return snapshot_result;
    }
    return mylite_ownerless_page_log_find_latest_in_snapshot_at(
        fd,
        log_offset,
        snapshot_end_offset,
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );
}

int mylite_ownerless_page_log_find_latest_in_snapshot(
    int fd,
    std::uint64_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    return mylite_ownerless_page_log_find_latest_in_snapshot_at(
        fd,
        0U,
        snapshot_end_offset,
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );
}

int mylite_ownerless_page_log_find_latest_in_snapshot_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || out_page == nullptr || page_capacity == 0U || out_page_size == nullptr ||
        out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        snapshot_end_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    const auto end_offset = static_cast<off_t>(snapshot_end_offset);
    if (!acquire_checkpoint_read_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int result = find_latest_in_snapshot(
        fd,
        offset,
        end_offset,
        space_id,
        page_no,
        max_commit_lsn,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );
    release_log_lock(fd, k_checkpoint_lock_start);
    return result;
}

int mylite_ownerless_page_log_read_record_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t record_offset,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || record_offset == 0U || out_page == nullptr || page_capacity == 0U ||
        out_page_size == nullptr || out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        record_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_checkpoint_read_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const int result = read_record_at_locked(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(record_offset),
        false,
        0U,
        0U,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );

    release_log_lock(fd, k_checkpoint_lock_start);
    return result;
}

int mylite_ownerless_page_log_read_page_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t record_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    if (fd < 0 || record_offset == 0U || out_page == nullptr || page_capacity == 0U ||
        out_page_size == nullptr || out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        record_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_checkpoint_read_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const int result = read_record_at_locked(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(record_offset),
        true,
        space_id,
        page_no,
        out_page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );

    release_log_lock(fd, k_checkpoint_lock_start);
    return result;
}

int mylite_ownerless_page_log_replay_at(
    int fd,
    std::uint64_t log_offset,
    mylite_ownerless_page_log_replay_callback callback,
    void *context
) {
    if (fd < 0 || callback == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    std::uint64_t snapshot_end_offset = 0;
    const int snapshot_result =
        mylite_ownerless_page_log_snapshot_at(fd, log_offset, &snapshot_end_offset);
    if (snapshot_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return snapshot_result;
    }
    if (snapshot_end_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_checkpoint_read_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int replay_result = replay_in_snapshot(
        fd,
        static_cast<off_t>(log_offset),
        static_cast<off_t>(snapshot_end_offset),
        callback,
        context
    );
    release_log_lock(fd, k_checkpoint_lock_start);
    return replay_result;
}

int mylite_ownerless_page_log_checkpoint(
    int fd,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    void *context
) {
    return mylite_ownerless_page_log_checkpoint_with_completion_at(
        fd,
        0U,
        safe_commit_lsn,
        retained_record_callback,
        nullptr,
        context
    );
}

int mylite_ownerless_page_log_checkpoint_with_completion(
    int fd,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    return mylite_ownerless_page_log_checkpoint_with_completion_at(
        fd,
        0U,
        safe_commit_lsn,
        retained_record_callback,
        complete_callback,
        context
    );
}

int mylite_ownerless_page_log_checkpoint_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    void *context
) {
    return mylite_ownerless_page_log_checkpoint_with_completion_at(
        fd,
        log_offset,
        safe_commit_lsn,
        retained_record_callback,
        nullptr,
        context
    );
}

int mylite_ownerless_page_log_checkpoint_with_completion_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_checkpoint_write_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        release_log_lock(fd, k_checkpoint_lock_start);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    const int header_result = validate_or_create_header(fd, offset);
    const int checkpoint_result = header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
                                      ? checkpoint_locked(
                                            fd,
                                            offset,
                                            safe_commit_lsn,
                                            retained_record_callback,
                                            complete_callback,
                                            context
                                        )
                                      : header_result;

    release_log_lock(fd, k_append_lock_start);
    release_log_lock(fd, k_checkpoint_lock_start);
    return checkpoint_result;
}

int mylite_ownerless_page_log_checkpoint_if_safe(
    int fd,
    std::uint64_t safe_commit_lsn,
    int *out_checkpointed
) {
    return mylite_ownerless_page_log_checkpoint_if_safe_at(
        fd,
        0U,
        safe_commit_lsn,
        out_checkpointed
    );
}

int mylite_ownerless_page_log_checkpoint_if_safe_at(
    int fd,
    std::uint64_t log_offset,
    std::uint64_t safe_commit_lsn,
    int *out_checkpointed
) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (log_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (out_checkpointed != nullptr) {
        *out_checkpointed = 0;
    }
    if (!acquire_checkpoint_write_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        release_log_lock(fd, k_checkpoint_lock_start);
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    const auto offset = static_cast<off_t>(log_offset);
    const int header_result = validate_or_create_header(fd, offset);
    const int checkpoint_result =
        header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
            ? checkpoint_if_safe_locked(fd, offset, safe_commit_lsn, out_checkpointed)
            : header_result;

    release_log_lock(fd, k_append_lock_start);
    release_log_lock(fd, k_checkpoint_lock_start);
    return checkpoint_result;
}

namespace {

int validate_or_create_header(int fd, off_t log_offset) {
    struct stat file_stat = {};
    if (::fstat(fd, &file_stat) != 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (file_stat.st_size < log_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    off_t header_end = 0;
    if (!offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &header_end)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (file_stat.st_size == log_offset) {
        return write_header(fd, log_offset) ? MYLITE_OWNERLESS_PAGE_LOG_OK
                                            : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (file_stat.st_size < header_end) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageLogHeader header = {};
    return read_header(fd, log_offset, header) && header_matches(header)
               ? MYLITE_OWNERLESS_PAGE_LOG_OK
               : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

int validate_existing_header(int fd, off_t log_offset) {
    struct stat file_stat = {};
    off_t header_end = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &header_end) ||
        file_stat.st_size < header_end) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageLogHeader header = {};
    return read_header(fd, log_offset, header) && header_matches(header)
               ? MYLITE_OWNERLESS_PAGE_LOG_OK
               : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

int append_locked(
    int fd,
    off_t log_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    struct stat file_stat = {};
    off_t records_offset = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    off_t payload_offset = 0;
    if (!offset_adds(
            file_stat.st_size,
            MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
            &payload_offset
        )) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    off_t end_offset = 0;
    if (!offset_adds(payload_offset, page_size, &end_offset)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageRecordHeader record = {};
    record.space_id = space_id;
    record.page_no = page_no;
    record.page_size = page_size;
    record.page_lsn = page_lsn;
    record.commit_lsn = commit_lsn;
    record.payload_size = page_size;
    record.checksum = checksum_bytes(page, page_size);

    if (!write_exact_at(fd, page, page_size, payload_offset) ||
        !write_record_header(fd, file_stat.st_size, record) || ::ftruncate(fd, end_offset) != 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (out_record_offset != nullptr) {
        *out_record_offset = static_cast<std::uint64_t>(file_stat.st_size);
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int snapshot_locked(int fd, off_t log_offset, std::uint64_t *out_snapshot_end_offset) {
    if (out_snapshot_end_offset == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    struct stat file_stat = {};
    off_t records_offset = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    *out_snapshot_end_offset = static_cast<std::uint64_t>(file_stat.st_size);
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int find_latest_in_snapshot(
    int fd,
    off_t log_offset,
    off_t snapshot_end_offset,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    struct stat file_stat = {};
    off_t records_offset = 0;
    const int header_result = validate_existing_header(fd, log_offset);
    if (header_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return header_result;
    }
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset || snapshot_end_offset < records_offset ||
        snapshot_end_offset > file_stat.st_size) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageRecordHeader best = {};
    off_t best_payload_offset = 0;
    for (off_t record_offset = records_offset; record_offset < snapshot_end_offset;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
        if (!offset_adds(
                record_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &payload_offset
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (payload_offset > snapshot_end_offset) {
            break;
        }
        if (!read_record_header(fd, record_offset, record)) {
            break;
        }
        off_t next_record_offset = 0;
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > snapshot_end_offset) {
            break;
        }
        if (record.space_id != space_id || record.page_no != page_no ||
            record.commit_lsn > max_commit_lsn || !record_is_better(record, best)) {
            record_offset = next_record_offset;
            continue;
        }

        const PayloadStatus payload_status = record_payload_status(fd, payload_offset, record);
        if (payload_status == PayloadStatus::Mismatch &&
            next_record_offset == snapshot_end_offset) {
            break;
        }
        if (payload_status != PayloadStatus::Ok) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        best = record;
        best_payload_offset = payload_offset;
        record_offset = next_record_offset;
    }
    if (best.payload_size == 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND;
    }
    if (best.payload_size > page_capacity ||
        best.payload_size > std::numeric_limits<std::uint32_t>::max()) {
        return MYLITE_OWNERLESS_PAGE_LOG_FULL;
    }
    if (!read_exact_at(
            fd,
            out_page,
            static_cast<std::size_t>(best.payload_size),
            best_payload_offset
        ) ||
        checksum_bytes(out_page, static_cast<std::size_t>(best.payload_size)) != best.checksum) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    *out_page_size = static_cast<std::uint32_t>(best.payload_size);
    *out_page_lsn = best.page_lsn;
    *out_commit_lsn = best.commit_lsn;
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int read_record_at_locked(
    int fd,
    off_t log_offset,
    off_t physical_record_offset,
    bool require_page_identity,
    std::uint32_t space_id,
    std::uint32_t page_no,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    int result = validate_existing_header(fd, log_offset);
    if (result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return result;
    }

    struct stat file_stat = {};
    off_t records_offset = 0;
    off_t payload_offset = 0;
    off_t next_record_offset = 0;
    PageRecordHeader record = {};
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        physical_record_offset < records_offset ||
        !offset_adds(
            physical_record_offset,
            MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
            &payload_offset
        ) ||
        payload_offset > file_stat.st_size ||
        !read_record_header(fd, physical_record_offset, record) ||
        !offset_adds(payload_offset, record.payload_size, &next_record_offset) ||
        next_record_offset > file_stat.st_size) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    if (require_page_identity && (record.space_id != space_id || record.page_no != page_no)) {
        return MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND;
    }
    if (record_payload_too_large(record.payload_size, page_capacity)) {
        return MYLITE_OWNERLESS_PAGE_LOG_FULL;
    }
    if (!read_exact_at(
            fd,
            out_page,
            static_cast<std::size_t>(record.payload_size),
            payload_offset
        ) ||
        !record_checksum_matches(out_page, record.payload_size, record.checksum)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    *out_page_size = static_cast<std::uint32_t>(record.payload_size);
    *out_page_lsn = record.page_lsn;
    *out_commit_lsn = record.commit_lsn;
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int replay_in_snapshot(
    int fd,
    off_t log_offset,
    off_t snapshot_end_offset,
    mylite_ownerless_page_log_replay_callback callback,
    void *context
) {
    struct stat file_stat = {};
    off_t records_offset = 0;
    const int header_result = validate_existing_header(fd, log_offset);
    if (header_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return header_result;
    }
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset || snapshot_end_offset < records_offset ||
        snapshot_end_offset > file_stat.st_size) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    for (off_t record_offset = records_offset; record_offset < snapshot_end_offset;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
        off_t next_record_offset = 0;
        if (!offset_adds(
                record_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &payload_offset
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (payload_offset > snapshot_end_offset) {
            break;
        }
        if (!read_record_header(fd, record_offset, record)) {
            break;
        }
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > snapshot_end_offset) {
            break;
        }

        if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
            if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }
        }
        const PayloadStatus payload_status = record_payload_status(fd, payload_offset, record);
        if (payload_status == PayloadStatus::Mismatch &&
            next_record_offset == snapshot_end_offset) {
            break;
        }
        if (payload_status != PayloadStatus::Ok) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }

        const int callback_result = callback(
            record.space_id,
            record.page_no,
            record.page_lsn,
            record.commit_lsn,
            static_cast<std::uint64_t>(record_offset),
            context
        );
        if (callback_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
            return callback_result;
        }
        record_offset = next_record_offset;
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int checkpoint_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    mylite_ownerless_page_log_replay_callback retained_record_callback,
    mylite_ownerless_page_log_checkpoint_complete_callback complete_callback,
    void *context
) {
    struct stat file_stat = {};
    off_t records_offset = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    off_t write_offset = records_offset;
    for (off_t record_offset = records_offset; record_offset < file_stat.st_size;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
        off_t next_record_offset = 0;
        if (!offset_adds(
                record_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &payload_offset
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (payload_offset > file_stat.st_size) {
            break;
        }
        if (!read_record_header(fd, record_offset, record)) {
            break;
        }
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > file_stat.st_size) {
            break;
        }

        if (record.commit_lsn > safe_commit_lsn) {
            if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
                if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
                    return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
                }
            }
            const std::size_t payload_size = static_cast<std::size_t>(record.payload_size);
            std::unique_ptr<unsigned char[]> page(new (std::nothrow) unsigned char[payload_size]);
            if (page == nullptr) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }
            if (!read_exact_at(fd, page.get(), payload_size, payload_offset) ||
                checksum_bytes(page.get(), payload_size) != record.checksum) {
                if (next_record_offset == file_stat.st_size) {
                    break;
                }
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }

            off_t write_payload_offset = 0;
            if (!offset_adds(
                    write_offset,
                    MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                    &write_payload_offset
                ) ||
                !write_record_header(fd, write_offset, record) ||
                !write_exact_at(fd, page.get(), payload_size, write_payload_offset)) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }
            if (retained_record_callback != nullptr) {
                const int callback_result = retained_record_callback(
                    record.space_id,
                    record.page_no,
                    record.page_lsn,
                    record.commit_lsn,
                    static_cast<std::uint64_t>(write_offset),
                    context
                );
                if (callback_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
                    return callback_result;
                }
            }
            if (!offset_adds(write_payload_offset, record.payload_size, &write_offset)) {
                return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
            }
        }
        record_offset = next_record_offset;
    }

    maybe_pause_for_test_fault("checkpoint-before-truncate");
    if (::ftruncate(fd, write_offset) != 0 || !sync_file(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return complete_callback == nullptr ? MYLITE_OWNERLESS_PAGE_LOG_OK : complete_callback(context);
}

int checkpoint_if_safe_locked(
    int fd,
    off_t log_offset,
    std::uint64_t safe_commit_lsn,
    int *out_checkpointed
) {
    struct stat file_stat = {};
    off_t records_offset = 0;
    if (::fstat(fd, &file_stat) != 0 ||
        !offset_adds(log_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE, &records_offset) ||
        file_stat.st_size < records_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    for (off_t record_offset = records_offset; record_offset < file_stat.st_size;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
        off_t next_record_offset = 0;
        if (!offset_adds(
                record_offset,
                MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE,
                &payload_offset
            )) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (payload_offset > file_stat.st_size) {
            break;
        }
        if (!read_record_header(fd, record_offset, record)) {
            break;
        }
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > file_stat.st_size) {
            break;
        }
        const PayloadStatus payload_status = record_payload_status(fd, payload_offset, record);
        if (payload_status == PayloadStatus::Mismatch && next_record_offset == file_stat.st_size) {
            break;
        }
        if (payload_status != PayloadStatus::Ok) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (record.commit_lsn > safe_commit_lsn) {
            return MYLITE_OWNERLESS_PAGE_LOG_OK;
        }
        record_offset = next_record_offset;
    }

    if (file_stat.st_size == records_offset) {
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }
    maybe_pause_for_test_fault("checkpoint-before-truncate");
    if (::ftruncate(fd, records_offset) != 0 || !sync_file(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (out_checkpointed != nullptr) {
        *out_checkpointed = 1;
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

bool acquire_append_lock(int fd) {
    return acquire_log_lock(fd, F_WRLCK, k_append_lock_start);
}

bool acquire_snapshot_lock(int fd) {
    return acquire_log_lock(fd, F_RDLCK, k_append_lock_start);
}

bool acquire_checkpoint_read_lock(int fd) {
    return acquire_log_lock(fd, F_RDLCK, k_checkpoint_lock_start);
}

bool acquire_checkpoint_write_lock(int fd) {
    return acquire_log_lock(fd, F_WRLCK, k_checkpoint_lock_start);
}

bool acquire_log_lock(int fd, short lock_type, off_t lock_start) {
    struct flock lock = {};
    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = lock_start;
    lock.l_len = 1;
    while (::fcntl(fd, F_SETLKW, &lock) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

void release_log_lock(int fd, off_t lock_start) {
    struct flock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = lock_start;
    lock.l_len = 1;
    static_cast<void>(::fcntl(fd, F_SETLK, &lock));
}

void maybe_pause_for_test_fault(const char *fault_name) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    const char *configured_fault = std::getenv("MYLITE_OWNERLESS_TEST_FAULT");
    if (configured_fault == nullptr || std::strcmp(configured_fault, fault_name) != 0) {
        return;
    }

    const char *ready_fd_value = std::getenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD");
    if (ready_fd_value != nullptr) {
        char *end = nullptr;
        const long ready_fd = std::strtol(ready_fd_value, &end, 10);
        if (end != ready_fd_value && *end == '\0' && ready_fd >= 0 &&
            ready_fd <= std::numeric_limits<int>::max()) {
            const char value = 'x';
            static_cast<void>(::write(static_cast<int>(ready_fd), &value, sizeof(value)));
            static_cast<void>(::close(static_cast<int>(ready_fd)));
        }
    }

    for (;;) {
        ::pause();
    }
#else
    (void)fault_name;
#endif
}

bool read_header(int fd, off_t log_offset, PageLogHeader &header) {
    return read_exact_at(fd, header.data(), header.size(), log_offset);
}

bool write_header(int fd, off_t log_offset) {
    PageLogHeader header = {};
    std::memcpy(
        header.data() + k_header_magic_offset,
        k_header_magic.data(),
        k_header_magic.size()
    );
    store32(header.data(), k_header_format_offset, k_format_version);
    store32(header.data(), k_header_size_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    store32(
        header.data(),
        k_header_record_header_size_offset,
        MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE
    );
    return write_exact_at(fd, header.data(), header.size(), log_offset);
}

bool header_matches(const PageLogHeader &header) {
    return std::memcmp(
               header.data() + k_header_magic_offset,
               k_header_magic.data(),
               k_header_magic.size()
           ) == 0 &&
           load32(header.data(), k_header_format_offset) == k_format_version &&
           load32(header.data(), k_header_size_offset) == MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE &&
           load32(header.data(), k_header_record_header_size_offset) ==
               MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE;
}

bool read_record_header(int fd, off_t offset, PageRecordHeader &header) {
    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE> bytes = {};
    if (!read_exact_at(fd, bytes.data(), bytes.size(), offset) ||
        std::memcmp(
            bytes.data() + k_record_magic_offset,
            k_record_magic.data(),
            k_record_magic.size()
        ) != 0) {
        return false;
    }

    header.space_id = load32(bytes.data(), k_record_space_id_offset);
    header.page_no = load32(bytes.data(), k_record_page_no_offset);
    header.page_size = load32(bytes.data(), k_record_page_size_offset);
    header.page_lsn = load64(bytes.data(), k_record_page_lsn_offset);
    header.commit_lsn = load64(bytes.data(), k_record_commit_lsn_offset);
    header.payload_size = load64(bytes.data(), k_record_payload_size_offset);
    header.checksum = load64(bytes.data(), k_record_payload_checksum_offset);
    return header.page_size != 0U && header.page_size == header.payload_size &&
           header.commit_lsn != 0U;
}

bool write_record_header(int fd, off_t offset, const PageRecordHeader &header) {
    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE> bytes = {};
    std::memcpy(bytes.data() + k_record_magic_offset, k_record_magic.data(), k_record_magic.size());
    store32(bytes.data(), k_record_space_id_offset, header.space_id);
    store32(bytes.data(), k_record_page_no_offset, header.page_no);
    store32(bytes.data(), k_record_page_size_offset, header.page_size);
    store32(bytes.data(), k_record_flags_offset, 0U);
    store64(bytes.data(), k_record_page_lsn_offset, header.page_lsn);
    store64(bytes.data(), k_record_commit_lsn_offset, header.commit_lsn);
    store64(bytes.data(), k_record_payload_size_offset, header.payload_size);
    store64(bytes.data(), k_record_payload_checksum_offset, header.checksum);
    return write_exact_at(fd, bytes.data(), bytes.size(), offset);
}

bool write_exact_at(int fd, const void *buffer, std::size_t size, off_t offset) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    std::size_t written = 0;
    while (written < size) {
        off_t io_offset = 0;
        if (!next_io_offset(offset, written, &io_offset)) {
            return false;
        }
        const ssize_t result = ::pwrite(fd, bytes + written, size - written, io_offset);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

bool read_exact_at(int fd, void *buffer, std::size_t size, off_t offset) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    std::size_t read = 0;
    while (read < size) {
        off_t io_offset = 0;
        if (!next_io_offset(offset, read, &io_offset)) {
            return false;
        }
        const ssize_t result = ::pread(fd, bytes + read, size - read, io_offset);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        read += static_cast<std::size_t>(result);
    }
    return true;
}

bool sync_file(int fd) {
    while (::fsync(fd) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool next_io_offset(off_t offset, std::size_t progress, off_t *out_offset) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (progress > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    return offset_adds(offset, static_cast<std::uint64_t>(progress), out_offset);
}

bool offset_adds(off_t offset, std::uint64_t length, off_t *out_offset) {
    if (out_offset == nullptr || offset < 0 ||
        length > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max() - offset)) {
        return false;
    }
    *out_offset = offset + static_cast<off_t>(length);
    return true;
}

bool record_payload_too_large(std::uint64_t payload_size, std::size_t page_capacity) {
    return payload_size > page_capacity || payload_size > std::numeric_limits<std::uint32_t>::max();
}

PayloadStatus record_payload_status(int fd, off_t payload_offset, const PageRecordHeader &record) {
    if constexpr (sizeof(std::size_t) < sizeof(record.payload_size)) {
        if (record.payload_size > std::numeric_limits<std::size_t>::max()) {
            return PayloadStatus::Error;
        }
    }
    const std::size_t payload_size = static_cast<std::size_t>(record.payload_size);
    std::unique_ptr<unsigned char[]> page(new (std::nothrow) unsigned char[payload_size]);
    if (page == nullptr || !read_exact_at(fd, page.get(), payload_size, payload_offset)) {
        return PayloadStatus::Error;
    }
    return record_checksum_matches(page.get(), record.payload_size, record.checksum)
               ? PayloadStatus::Ok
               : PayloadStatus::Mismatch;
}

bool record_checksum_matches(const void *page, std::uint64_t payload_size, std::uint64_t checksum) {
    return checksum_bytes(page, static_cast<std::size_t>(payload_size)) == checksum;
}

bool record_is_better(const PageRecordHeader &candidate, const PageRecordHeader &current) {
    return current.payload_size == 0U || candidate.commit_lsn > current.commit_lsn ||
           (candidate.commit_lsn == current.commit_lsn && candidate.page_lsn > current.page_lsn);
}

std::uint64_t checksum_bytes(const void *buffer, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint32_t load32(const unsigned char *bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint64_t load64(const unsigned char *bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

void store32(unsigned char *bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
}

void store64(unsigned char *bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
}

} // namespace

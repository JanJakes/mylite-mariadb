#include "ownerless_page_log.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

struct PageRecordHeader {
    std::uint32_t space_id = 0;
    std::uint32_t page_no = 0;
    std::uint32_t page_size = 0;
    std::uint64_t page_lsn = 0;
    std::uint64_t commit_lsn = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t checksum = 0;
};

int validate_or_create_header(int fd);
int append_locked(
    int fd,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
);
int find_latest_locked(
    int fd,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *out_page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
);
bool acquire_append_lock(int fd);
void release_append_lock(int fd);
bool read_header(int fd, std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE> &header);
bool write_header(int fd);
bool header_matches(const std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE> &header);
bool read_record_header(int fd, off_t offset, PageRecordHeader &header);
bool write_record_header(int fd, off_t offset, const PageRecordHeader &header);
bool write_exact_at(int fd, const void *buffer, std::size_t size, off_t offset);
bool read_exact_at(int fd, void *buffer, std::size_t size, off_t offset);
bool next_io_offset(off_t offset, std::size_t progress, off_t *out_offset);
bool offset_adds(off_t offset, std::uint64_t length, off_t *out_offset);
bool record_is_better(const PageRecordHeader &candidate, const PageRecordHeader &current);
std::uint64_t checksum_bytes(const void *buffer, std::size_t size);
std::uint32_t load32(const unsigned char *bytes, std::size_t offset);
std::uint64_t load64(const unsigned char *bytes, std::size_t offset);
void store32(unsigned char *bytes, std::size_t offset, std::uint32_t value);
void store64(unsigned char *bytes, std::size_t offset, std::uint64_t value);

} // namespace

int mylite_ownerless_page_log_initialize(int fd) {
    if (fd < 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int result = validate_or_create_header(fd);
    release_append_lock(fd);
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
    if (fd < 0 || commit_lsn == 0U || page == nullptr || page_size == 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int header_result = validate_or_create_header(fd);
    const int append_result =
        header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
            ? append_locked(
                  fd,
                  space_id,
                  page_no,
                  page_lsn,
                  commit_lsn,
                  page,
                  page_size,
                  out_record_offset
              )
            : header_result;
    release_append_lock(fd);
    return append_result;
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
    if (fd < 0 || out_page == nullptr || page_capacity == 0U || out_page_size == nullptr ||
        out_page_lsn == nullptr ||
        out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (!acquire_append_lock(fd)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    const int header_result = validate_or_create_header(fd);
    const int find_result =
        header_result == MYLITE_OWNERLESS_PAGE_LOG_OK
            ? find_latest_locked(
                  fd,
                  space_id,
                  page_no,
                  max_commit_lsn,
                  out_page,
                  page_capacity,
                  out_page_size,
                  out_page_lsn,
                  out_commit_lsn
              )
            : header_result;
    release_append_lock(fd);
    return find_result;
}

namespace {

int validate_or_create_header(int fd) {
    struct stat file_stat = {};
    if (::fstat(fd, &file_stat) != 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (file_stat.st_size == 0) {
        return write_header(fd) ? MYLITE_OWNERLESS_PAGE_LOG_OK
                                : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (file_stat.st_size < static_cast<off_t>(MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE> header = {};
    return read_header(fd, header) && header_matches(header) ? MYLITE_OWNERLESS_PAGE_LOG_OK
                                                             : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

int append_locked(
    int fd,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    const void *page,
    std::uint32_t page_size,
    std::uint64_t *out_record_offset
) {
    struct stat file_stat = {};
    if (::fstat(fd, &file_stat) != 0 ||
        file_stat.st_size < static_cast<off_t>(MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE)) {
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
        !write_record_header(fd, file_stat.st_size, record) ||
        ::ftruncate(fd, end_offset) != 0) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (out_record_offset != nullptr) {
        *out_record_offset = static_cast<std::uint64_t>(file_stat.st_size);
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int find_latest_locked(
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
    struct stat file_stat = {};
    if (::fstat(fd, &file_stat) != 0 ||
        file_stat.st_size < static_cast<off_t>(MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE)) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    PageRecordHeader best = {};
    off_t best_payload_offset = 0;
    for (off_t record_offset = static_cast<off_t>(MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
         record_offset < file_stat.st_size;) {
        PageRecordHeader record = {};
        off_t payload_offset = 0;
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
        off_t next_record_offset = 0;
        if (!offset_adds(payload_offset, record.payload_size, &next_record_offset)) {
            return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
        }
        if (next_record_offset > file_stat.st_size) {
            break;
        }
        if (record.space_id == space_id && record.page_no == page_no &&
            record.commit_lsn <= max_commit_lsn && record_is_better(record, best)) {
            best = record;
            best_payload_offset = payload_offset;
        }
        record_offset = next_record_offset;
    }
    if (best.payload_size == 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND;
    }
    if (best.payload_size > page_capacity ||
        best.payload_size > std::numeric_limits<std::uint32_t>::max()) {
        return MYLITE_OWNERLESS_PAGE_LOG_FULL;
    }
    if (!read_exact_at(fd, out_page, static_cast<std::size_t>(best.payload_size), best_payload_offset) ||
        checksum_bytes(out_page, static_cast<std::size_t>(best.payload_size)) != best.checksum) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    *out_page_size = static_cast<std::uint32_t>(best.payload_size);
    *out_page_lsn = best.page_lsn;
    *out_commit_lsn = best.commit_lsn;
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

bool acquire_append_lock(int fd) {
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;
    while (::fcntl(fd, F_SETLKW, &lock) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

void release_append_lock(int fd) {
    struct flock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;
    static_cast<void>(::fcntl(fd, F_SETLK, &lock));
}

bool read_header(int fd, std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE> &header) {
    return read_exact_at(fd, header.data(), header.size(), 0);
}

bool write_header(int fd) {
    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE> header = {};
    std::memcpy(header.data() + k_header_magic_offset, k_header_magic.data(), k_header_magic.size());
    store32(header.data(), k_header_format_offset, k_format_version);
    store32(header.data(), k_header_size_offset, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE);
    store32(
        header.data(),
        k_header_record_header_size_offset,
        MYLITE_OWNERLESS_PAGE_LOG_RECORD_HEADER_SIZE
    );
    return write_exact_at(fd, header.data(), header.size(), 0);
}

bool header_matches(const std::array<unsigned char, MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE> &header) {
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

#include "ownerless_tablespace_replay.h"

#include "ownerless_page_log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t k_innodb_page_offset_offset = 4;
constexpr std::size_t k_innodb_page_lsn_offset = 16;
constexpr std::size_t k_innodb_page_type_offset = 24;
constexpr std::size_t k_innodb_page_space_id_offset = 34;
constexpr std::uint32_t k_innodb_page_size_min = 4096;
constexpr std::uint32_t k_innodb_page_size_max = 65536;
constexpr std::uint16_t k_innodb_page_type_fsp_header = 8;

class PageRecordMetadata {
  public:
    PageRecordMetadata(
        std::uint32_t space_id,
        std::uint32_t page_no,
        std::uint64_t page_lsn,
        std::uint64_t commit_lsn,
        std::uint64_t record_offset
    )
        : m_space_id(space_id), m_page_no(page_no), m_page_lsn(page_lsn), m_commit_lsn(commit_lsn),
          m_record_offset(record_offset) {}

    std::uint32_t space_id() const {
        return m_space_id;
    }

    std::uint32_t page_no() const {
        return m_page_no;
    }

    std::uint64_t page_lsn() const {
        return m_page_lsn;
    }

    std::uint64_t commit_lsn() const {
        return m_commit_lsn;
    }

    std::uint64_t record_offset() const {
        return m_record_offset;
    }

  private:
    std::uint32_t m_space_id;
    std::uint32_t m_page_no;
    std::uint64_t m_page_lsn;
    std::uint64_t m_commit_lsn;
    std::uint64_t m_record_offset;
};

class PageImage {
  public:
    void assign(
        std::uint32_t space_id,
        std::uint32_t page_no,
        std::uint64_t page_lsn,
        std::uint32_t page_size,
        const unsigned char *bytes
    ) {
        m_space_id = space_id;
        m_page_no = page_no;
        m_page_lsn = page_lsn;
        m_page_size = page_size;
        std::memcpy(m_bytes.data(), bytes, page_size);
    }

    std::uint32_t space_id() const {
        return m_space_id;
    }

    std::uint32_t page_no() const {
        return m_page_no;
    }

    std::uint64_t page_lsn() const {
        return m_page_lsn;
    }

    std::uint32_t page_size() const {
        return m_page_size;
    }

    const std::array<unsigned char, k_innodb_page_size_max> &bytes() const {
        return m_bytes;
    }

  private:
    std::uint32_t m_space_id = 0;
    std::uint32_t m_page_no = 0;
    std::uint64_t m_page_lsn = 0;
    std::uint32_t m_page_size = 0;
    std::array<unsigned char, k_innodb_page_size_max> m_bytes = {};
};

using PageKey = std::pair<std::uint32_t, std::uint32_t>;
using TablespaceKey = std::pair<std::uint32_t, std::uint32_t>;

struct ReplayMetadataContext {
    std::uint64_t visible_lsn = 0;
    std::vector<PageRecordMetadata> records;
};

struct OpenTablespace {
    int fd = -1;
    bool dirty = false;
};

int collect_visible_record_metadata(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
);
bool valid_page_size(std::uint32_t page_size);
bool read_page_image(
    int page_log_fd,
    std::uint64_t page_log_offset,
    const PageRecordMetadata &metadata,
    PageImage &page
);
bool record_metadata_is_better(
    const PageRecordMetadata &candidate,
    const PageRecordMetadata &current
);
bool innodb_page_header_matches(
    const unsigned char *page,
    std::uint32_t page_size,
    std::uint32_t space_id,
    std::uint32_t page_no
);
bool innodb_tablespace_page_zero_matches(
    const unsigned char *page,
    std::uint32_t page_size,
    std::uint32_t space_id
);
bool write_exact_at(int fd, const void *buffer, std::size_t size, off_t offset);
std::uint16_t load_be16(const unsigned char *bytes, std::size_t offset);
std::uint32_t load_be32(const unsigned char *bytes, std::size_t offset);
std::uint64_t load_be64(const unsigned char *bytes, std::size_t offset);

class TablespaceResolver {
  public:
    explicit TablespaceResolver(std::filesystem::path datadir) : m_datadir(std::move(datadir)) {}

    ~TablespaceResolver() {
        for (auto &entry : m_open_files) {
            if (entry.second.fd >= 0) {
                static_cast<void>(::close(entry.second.fd));
            }
        }
    }

    int sync() {
        for (auto &entry : m_open_files) {
            if (entry.second.fd >= 0 && entry.second.dirty) {
                if (::fsync(entry.second.fd) != 0) {
                    return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
                }
                entry.second.dirty = false;
            }
        }
        return MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK;
    }

    int apply(const PageImage &page, bool ignore_missing_tablespaces) {
        const std::filesystem::path *path = resolve(page.space_id(), page.page_size());
        if (path == nullptr) {
            return ignore_missing_tablespaces ? MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK
                                              : MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
        }

        OpenTablespace *file = open(*path);
        if (file == nullptr) {
            return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
        }

        const std::uint64_t offset = static_cast<std::uint64_t>(page.page_no()) * page.page_size();
        const std::uint64_t end_offset = offset + page.page_size();
        if (end_offset < offset || end_offset > static_cast<std::uint64_t>(INT64_MAX)) {
            return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
        }

        std::array<unsigned char, k_innodb_page_size_max> disk_page = {};
        const ssize_t read_result =
            ::pread(file->fd, disk_page.data(), page.page_size(), static_cast<off_t>(offset));
        if (read_result == static_cast<ssize_t>(page.page_size()) &&
            innodb_page_header_matches(
                disk_page.data(),
                page.page_size(),
                page.space_id(),
                page.page_no()
            ) &&
            load_be64(disk_page.data(), k_innodb_page_lsn_offset) == page.page_lsn() &&
            std::memcmp(disk_page.data(), page.bytes().data(), page.page_size()) == 0) {
            return MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK;
        }
        if (read_result < 0) {
            return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
        }
        if (read_result != 0 && read_result != static_cast<ssize_t>(page.page_size())) {
            return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
        }

        struct stat file_stat = {};
        if (::fstat(file->fd, &file_stat) != 0) {
            return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
        }
        if (file_stat.st_size < static_cast<off_t>(end_offset) &&
            ::ftruncate(file->fd, static_cast<off_t>(end_offset)) != 0) {
            return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
        }
        if (!write_exact_at(
                file->fd,
                page.bytes().data(),
                page.page_size(),
                static_cast<off_t>(offset)
            )) {
            return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
        }
        file->dirty = true;
        return MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK;
    }

  private:
    const std::filesystem::path *resolve(std::uint32_t space_id, std::uint32_t page_size) {
        const TablespaceKey key{space_id, page_size};
        auto existing = m_resolved.find(key);
        if (existing != m_resolved.end()) {
            return existing->second.empty() ? nullptr : &existing->second;
        }

        std::filesystem::path resolved_path;
        if (!scan_for_tablespace(space_id, page_size, resolved_path)) {
            m_resolved.emplace(key, std::filesystem::path{});
            return nullptr;
        }
        auto [inserted, _] = m_resolved.emplace(key, std::move(resolved_path));
        return &inserted->second;
    }

    OpenTablespace *open(const std::filesystem::path &path) {
        auto existing = m_open_files.find(path);
        if (existing != m_open_files.end()) {
            return &existing->second;
        }

        const std::string path_name = path.string();
        const int fd = ::open(path_name.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            return nullptr;
        }
        auto [inserted, _] = m_open_files.emplace(path, OpenTablespace{fd, false});
        return &inserted->second;
    }

    bool scan_for_tablespace(
        std::uint32_t space_id,
        std::uint32_t page_size,
        std::filesystem::path &out_path
    ) const {
        if (!valid_page_size(page_size)) {
            return false;
        }

        std::error_code error;
        if (!std::filesystem::is_directory(m_datadir, error) || error) {
            return false;
        }

        bool found = false;
        const std::filesystem::recursive_directory_iterator end;
        std::filesystem::recursive_directory_iterator entries(
            m_datadir,
            std::filesystem::directory_options::skip_permission_denied,
            error
        );
        while (!error && entries != end) {
            const std::filesystem::directory_entry &entry = *entries;
            if (entry.is_regular_file(error) && !error &&
                file_has_tablespace_id(entry.path(), space_id, page_size)) {
                if (found) {
                    return false;
                }
                out_path = entry.path();
                found = true;
            }
            entries.increment(error);
        }
        return !error && found;
    }

    static bool file_has_tablespace_id(
        const std::filesystem::path &path,
        std::uint32_t space_id,
        std::uint32_t page_size
    ) {
        const std::string path_name = path.string();
        const int fd = ::open(path_name.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }

        std::array<unsigned char, k_innodb_page_size_max> page = {};
        const ssize_t read_result = ::pread(fd, page.data(), page_size, 0);
        static_cast<void>(::close(fd));
        return read_result == static_cast<ssize_t>(page_size) &&
               innodb_tablespace_page_zero_matches(page.data(), page_size, space_id);
    }

    std::filesystem::path m_datadir;
    std::map<TablespaceKey, std::filesystem::path> m_resolved;
    std::map<std::filesystem::path, OpenTablespace> m_open_files;
};

int collect_visible_record_metadata(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
) {
    auto *metadata = static_cast<ReplayMetadataContext *>(context);
    if (metadata == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    if (commit_lsn == 0U || commit_lsn > metadata->visible_lsn) {
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }

    try {
        metadata->records.emplace_back(space_id, page_no, page_lsn, commit_lsn, record_offset);
    } catch (...) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

bool valid_page_size(std::uint32_t page_size) {
    return page_size >= k_innodb_page_size_min && page_size <= k_innodb_page_size_max &&
           (page_size & (page_size - 1U)) == 0U;
}

bool read_page_image(
    int page_log_fd,
    std::uint64_t page_log_offset,
    const PageRecordMetadata &metadata,
    PageImage &page
) {
    std::uint32_t page_size = 0;
    std::uint64_t page_lsn = 0;
    std::uint64_t commit_lsn = 0;
    std::array<unsigned char, k_innodb_page_size_max> bytes = {};
    const int read_result = mylite_ownerless_page_log_read_record_at(
        page_log_fd,
        page_log_offset,
        metadata.record_offset(),
        bytes.data(),
        bytes.size(),
        &page_size,
        &page_lsn,
        &commit_lsn
    );
    if (read_result != MYLITE_OWNERLESS_PAGE_LOG_OK || !valid_page_size(page_size) ||
        page_lsn != metadata.page_lsn() || commit_lsn != metadata.commit_lsn() ||
        !innodb_page_header_matches(
            bytes.data(),
            page_size,
            metadata.space_id(),
            metadata.page_no()
        ) ||
        load_be64(bytes.data(), k_innodb_page_lsn_offset) != metadata.page_lsn()) {
        return false;
    }

    page.assign(
        metadata.space_id(),
        metadata.page_no(),
        metadata.page_lsn(),
        page_size,
        bytes.data()
    );
    return true;
}

bool record_metadata_is_better(
    const PageRecordMetadata &candidate,
    const PageRecordMetadata &current
) {
    return candidate.commit_lsn() > current.commit_lsn() ||
           (candidate.commit_lsn() == current.commit_lsn() &&
            candidate.page_lsn() > current.page_lsn());
}

bool innodb_page_header_matches(
    const unsigned char *page,
    std::uint32_t page_size,
    std::uint32_t space_id,
    std::uint32_t page_no
) {
    return page != nullptr && page_size >= k_innodb_page_space_id_offset + sizeof(std::uint32_t) &&
           load_be32(page, k_innodb_page_offset_offset) == page_no &&
           load_be32(page, k_innodb_page_space_id_offset) == space_id;
}

bool innodb_tablespace_page_zero_matches(
    const unsigned char *page,
    std::uint32_t page_size,
    std::uint32_t space_id
) {
    return innodb_page_header_matches(page, page_size, space_id, 0U) &&
           load_be16(page, k_innodb_page_type_offset) == k_innodb_page_type_fsp_header;
}

bool write_exact_at(int fd, const void *buffer, std::size_t size, off_t offset) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    std::size_t written = 0;
    while (written < size) {
        const off_t write_offset = offset + static_cast<off_t>(written);
        const ssize_t result = ::pwrite(fd, bytes + written, size - written, write_offset);
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

std::uint16_t load_be16(const unsigned char *bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1U])
    );
}

std::uint32_t load_be32(const unsigned char *bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

std::uint64_t load_be64(const unsigned char *bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

} // namespace

int mylite_ownerless_tablespace_replay_apply(
    const char *datadir,
    int page_log_fd,
    std::uint64_t page_log_offset,
    std::uint64_t visible_lsn
) {
    return mylite_ownerless_tablespace_replay_apply_with_flags(
        datadir,
        page_log_fd,
        page_log_offset,
        visible_lsn,
        0U
    );
}

int mylite_ownerless_tablespace_replay_apply_with_flags(
    const char *datadir,
    int page_log_fd,
    std::uint64_t page_log_offset,
    std::uint64_t visible_lsn,
    unsigned flags
) {
    if (datadir == nullptr || page_log_fd < 0 ||
        (flags & ~MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES) != 0U) {
        return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
    }
    if (visible_lsn == 0U) {
        return MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK;
    }

    ReplayMetadataContext context = {};
    context.visible_lsn = visible_lsn;
    const int replay_result = mylite_ownerless_page_log_replay_at(
        page_log_fd,
        page_log_offset,
        collect_visible_record_metadata,
        &context
    );
    if (replay_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
    }

    std::map<PageKey, PageRecordMetadata> latest_records;
    for (const PageRecordMetadata &metadata : context.records) {
        const PageKey key{metadata.space_id(), metadata.page_no()};
        auto [existing, inserted] = latest_records.emplace(key, metadata);
        if (!inserted && record_metadata_is_better(metadata, existing->second)) {
            existing->second = metadata;
        }
    }

    TablespaceResolver resolver(datadir);
    const bool ignore_missing_tablespaces =
        (flags & MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES) != 0U;
    for (const auto &entry : latest_records) {
        PageImage page = {};
        if (!read_page_image(page_log_fd, page_log_offset, entry.second, page)) {
            return MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR;
        }

        const int apply_result = resolver.apply(page, ignore_missing_tablespaces);
        if (apply_result != MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK) {
            return apply_result;
        }
    }
    return resolver.sync();
}

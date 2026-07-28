#define MYLITE_OWNERLESS_PLATFORM_IO_IMPLEMENTATION
#include "ownerless_platform_io.h"

#if defined(__APPLE__)
#  undef close
#  undef fcntl
#endif

#if defined(_WIN32)

#  include <fcntl.h>
#  include <io.h>
#  include <share.h>
#  include <windows.h>

#  include <algorithm>
#  include <cerrno>
#  include <climits>
#  include <cstdarg>
#  include <cstdio>
#  include <limits>
#  include <mutex>
#  include <string>
#  include <unordered_map>
#  include <vector>

namespace {

constexpr DWORD k_share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr std::uint64_t k_max_lock_length = std::numeric_limits<std::uint64_t>::max();

std::mutex file_flags_mutex;
std::unordered_map<int, int> file_flags;

void set_errno_from_windows_error(DWORD error) {
    switch (error) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_INVALID_HANDLE:
    case ERROR_INVALID_PARAMETER:
        errno = EINVAL;
        break;
    default:
        errno = EIO;
        break;
    }
}

HANDLE file_handle(int fd) {
    const intptr_t raw_handle = ::_get_osfhandle(fd);
    return raw_handle == -1 ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(raw_handle);
}

std::vector<wchar_t> utf8_path(const char *path) {
    if (path == nullptr) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (length <= 0) {
        set_errno_from_windows_error(GetLastError());
        return {};
    }
    std::vector<wchar_t> wide_path(static_cast<std::size_t>(length));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path.data(), length) !=
        length) {
        set_errno_from_windows_error(GetLastError());
        return {};
    }
    return wide_path;
}

DWORD desired_access(int flags) {
    switch (flags & O_ACCMODE) {
    case O_WRONLY:
        return GENERIC_WRITE;
    case O_RDWR:
        return GENERIC_READ | GENERIC_WRITE;
    default:
        return GENERIC_READ;
    }
}

DWORD creation_disposition(int flags) {
    if ((flags & O_CREAT) != 0) {
        return (flags & O_TRUNC) != 0 ? CREATE_ALWAYS : OPEN_ALWAYS;
    }
    return (flags & O_TRUNC) != 0 ? TRUNCATE_EXISTING : OPEN_EXISTING;
}

void remember_file_flags(int fd, int flags) {
    std::lock_guard<std::mutex> guard(file_flags_mutex);
    file_flags[fd] = flags;
}

void forget_file_flags(int fd) {
    std::lock_guard<std::mutex> guard(file_flags_mutex);
    file_flags.erase(fd);
}

int remembered_file_flags(int fd) {
    std::lock_guard<std::mutex> guard(file_flags_mutex);
    const auto flags = file_flags.find(fd);
    return flags == file_flags.end() ? O_RDWR : flags->second;
}

OVERLAPPED make_overlapped(mylite_ownerless_offset_t offset) {
    OVERLAPPED overlapped = {};
    const auto unsigned_offset = static_cast<std::uint64_t>(offset);
    overlapped.Offset = static_cast<DWORD>(unsigned_offset);
    overlapped.OffsetHigh = static_cast<DWORD>(unsigned_offset >> 32U);
    return overlapped;
}

bool lock_range(
    HANDLE handle,
    mylite_ownerless_offset_t start,
    mylite_ownerless_offset_t length,
    bool exclusive,
    bool nonblocking
) {
    const std::uint64_t lock_length =
        length == 0 ? k_max_lock_length : static_cast<std::uint64_t>(length);
    OVERLAPPED overlapped = make_overlapped(start);
    DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0U;
    if (nonblocking) {
        flags |= LOCKFILE_FAIL_IMMEDIATELY;
    }
    if (LockFileEx(
            handle,
            flags,
            0,
            static_cast<DWORD>(lock_length),
            static_cast<DWORD>(lock_length >> 32U),
            &overlapped
        ) != FALSE) {
        return true;
    }
    set_errno_from_windows_error(GetLastError());
    if (errno == EACCES) {
        errno = EAGAIN;
    }
    return false;
}

bool unlock_range(
    HANDLE handle,
    mylite_ownerless_offset_t start,
    mylite_ownerless_offset_t length
) {
    const std::uint64_t lock_length =
        length == 0 ? k_max_lock_length : static_cast<std::uint64_t>(length);
    OVERLAPPED overlapped = make_overlapped(start);
    if (UnlockFileEx(
            handle,
            0,
            static_cast<DWORD>(lock_length),
            static_cast<DWORD>(lock_length >> 32U),
            &overlapped
        ) != FALSE) {
        return true;
    }
    set_errno_from_windows_error(GetLastError());
    return false;
}

} // namespace

int mylite_ownerless_open(const char *path, int flags, ...) {
    if (path == nullptr || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    const std::vector<wchar_t> wide_path = utf8_path(path);
    if (wide_path.empty()) {
        return -1;
    }
    const DWORD attributes =
        (flags & O_DIRECTORY) != 0 ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(
        wide_path.data(),
        desired_access(flags),
        k_share_mode,
        nullptr,
        creation_disposition(flags),
        attributes,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        set_errno_from_windows_error(GetLastError());
        return -1;
    }

    const int descriptor_flags =
        ((flags & O_ACCMODE) | _O_BINARY) | ((flags & O_CLOEXEC) != 0 ? _O_NOINHERIT : 0);
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), descriptor_flags);
    if (fd < 0) {
        const int saved_errno = errno;
        CloseHandle(handle);
        errno = saved_errno;
        return -1;
    }
    remember_file_flags(fd, flags);
    return fd;
}

int mylite_ownerless_close(int fd) {
    forget_file_flags(fd);
    return ::_close(fd);
}

mylite_ownerless_ssize_t mylite_ownerless_read(int fd, void *data, std::size_t size) {
    const unsigned chunk = static_cast<unsigned>(std::min(size, static_cast<std::size_t>(INT_MAX)));
    return ::_read(fd, data, chunk);
}

mylite_ownerless_ssize_t mylite_ownerless_write(int fd, const void *data, std::size_t size) {
    const unsigned chunk = static_cast<unsigned>(std::min(size, static_cast<std::size_t>(INT_MAX)));
    return ::_write(fd, data, chunk);
}

mylite_ownerless_ssize_t mylite_ownerless_pread(
    int fd,
    void *data,
    std::size_t size,
    mylite_ownerless_offset_t offset
) {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    HANDLE handle = file_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }

    OVERLAPPED overlapped = make_overlapped(offset);
    DWORD transferred = 0;
    const DWORD chunk = static_cast<DWORD>(std::min(size, static_cast<std::size_t>(MAXDWORD)));
    if (ReadFile(handle, data, chunk, &transferred, &overlapped) != FALSE) {
        return static_cast<mylite_ownerless_ssize_t>(transferred);
    }
    const DWORD error = GetLastError();
    if (error == ERROR_HANDLE_EOF) {
        return 0;
    }
    set_errno_from_windows_error(error);
    return -1;
}

mylite_ownerless_ssize_t mylite_ownerless_pwrite(
    int fd,
    const void *data,
    std::size_t size,
    mylite_ownerless_offset_t offset
) {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    HANDLE handle = file_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }

    OVERLAPPED overlapped = make_overlapped(offset);
    DWORD transferred = 0;
    const DWORD chunk = static_cast<DWORD>(std::min(size, static_cast<std::size_t>(MAXDWORD)));
    if (WriteFile(handle, data, chunk, &transferred, &overlapped) != FALSE) {
        return static_cast<mylite_ownerless_ssize_t>(transferred);
    }
    set_errno_from_windows_error(GetLastError());
    return -1;
}

int mylite_ownerless_fsync(int fd) {
    HANDLE handle = file_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (FlushFileBuffers(handle) != FALSE) {
        return 0;
    }
    set_errno_from_windows_error(GetLastError());
    return -1;
}

int mylite_ownerless_fdatasync(int fd) {
    return mylite_ownerless_fsync(fd);
}

int mylite_ownerless_ftruncate(int fd, mylite_ownerless_offset_t size) {
    if (size < 0 || ::_chsize_s(fd, static_cast<__int64>(size)) != 0) {
        if (size < 0) {
            errno = EINVAL;
        }
        return -1;
    }
    return 0;
}

int mylite_ownerless_fstat(int fd, mylite_ownerless_file_info *out_info) {
    if (out_info == nullptr) {
        errno = EINVAL;
        return -1;
    }
    HANDLE handle = file_handle(fd);
    BY_HANDLE_FILE_INFORMATION info = {};
    if (handle == INVALID_HANDLE_VALUE || GetFileInformationByHandle(handle, &info) == FALSE) {
        set_errno_from_windows_error(
            handle == INVALID_HANDLE_VALUE ? ERROR_INVALID_HANDLE : GetLastError()
        );
        return -1;
    }
    const std::uint64_t size =
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
    const std::uint64_t index =
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
    out_info->st_size = static_cast<mylite_ownerless_offset_t>(size);
    out_info->st_dev = info.dwVolumeSerialNumber;
    out_info->st_ino = index;
    out_info->st_mode =
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? _S_IFDIR : _S_IFREG;
    return 0;
}

int mylite_ownerless_stat(const char *path, mylite_ownerless_file_info *out_info) {
    const int fd = mylite_ownerless_open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    const int result = mylite_ownerless_fstat(fd, out_info);
    const int saved_errno = errno;
    mylite_ownerless_close(fd);
    errno = saved_errno;
    return result;
}

void *mylite_ownerless_mmap(
    void *address,
    std::size_t size,
    int protection,
    int flags,
    int fd,
    mylite_ownerless_offset_t offset
) {
    (void)address;
    if (size == 0U || offset < 0 || (flags & MAP_SHARED) == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    HANDLE handle = file_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return MAP_FAILED;
    }

    const DWORD page_protection = (protection & PROT_WRITE) != 0 ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE mapping = CreateFileMappingW(handle, nullptr, page_protection, 0, 0, nullptr);
    if (mapping == nullptr) {
        set_errno_from_windows_error(GetLastError());
        return MAP_FAILED;
    }
    const DWORD access =
        (protection & PROT_WRITE) != 0 ? FILE_MAP_WRITE | FILE_MAP_READ : FILE_MAP_READ;
    const std::uint64_t unsigned_offset = static_cast<std::uint64_t>(offset);
    void *view = MapViewOfFile(
        mapping,
        access,
        static_cast<DWORD>(unsigned_offset >> 32U),
        static_cast<DWORD>(unsigned_offset),
        size
    );
    const DWORD error = view == nullptr ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(mapping);
    if (view != nullptr) {
        return view;
    }
    set_errno_from_windows_error(error);
    return MAP_FAILED;
}

int mylite_ownerless_munmap(void *address, std::size_t size) {
    (void)size;
    if (UnmapViewOfFile(address) != FALSE) {
        return 0;
    }
    set_errno_from_windows_error(GetLastError());
    return -1;
}

int mylite_ownerless_msync(void *address, std::size_t size, int flags) {
    (void)flags;
    if (FlushViewOfFile(address, size) != FALSE) {
        return 0;
    }
    set_errno_from_windows_error(GetLastError());
    return -1;
}

int mylite_ownerless_fcntl(int fd, int command, ...) {
    va_list arguments;
    va_start(arguments, command);
    if (command == F_GETFL) {
        va_end(arguments);
        return remembered_file_flags(fd);
    }
    if (command == F_DUPFD_CLOEXEC) {
        (void)va_arg(arguments, int);
        va_end(arguments);
        const int duplicate = ::_dup(fd);
        if (duplicate >= 0) {
            remember_file_flags(duplicate, remembered_file_flags(fd));
        }
        return duplicate;
    }

    auto *lock = va_arg(arguments, mylite_ownerless_file_lock *);
    va_end(arguments);
    if ((command != F_SETLK && command != F_OFD_SETLK) || lock == nullptr ||
        lock->l_whence != SEEK_SET || lock->l_start < 0 || lock->l_len < 0) {
        errno = EINVAL;
        return -1;
    }
    HANDLE handle = file_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (lock->l_type == F_UNLCK) {
        return unlock_range(handle, lock->l_start, lock->l_len) ? 0 : -1;
    }
    return lock_range(handle, lock->l_start, lock->l_len, lock->l_type == F_WRLCK, true) ? 0 : -1;
}

int mylite_ownerless_flock(int fd, int operation) {
    HANDLE handle = file_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if ((operation & LOCK_UN) != 0) {
        return unlock_range(handle, 0, 0) ? 0 : -1;
    }
    return lock_range(handle, 0, 0, (operation & LOCK_EX) != 0, (operation & LOCK_NB) != 0) ? 0
                                                                                            : -1;
}

std::uint64_t mylite_ownerless_current_process_id(void) {
    return static_cast<std::uint64_t>(GetCurrentProcessId());
}

bool mylite_ownerless_process_id_is_alive(std::uint64_t process_id) {
    if (process_id == 0U || process_id > MAXDWORD) {
        return false;
    }
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE,
        static_cast<DWORD>(process_id)
    );
    if (process == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
}

bool mylite_ownerless_sync_directory_path(const char *path) {
    const std::vector<wchar_t> wide_path = utf8_path(path);
    if (wide_path.empty()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(wide_path.data());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        set_errno_from_windows_error(GetLastError());
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        errno = ENOTDIR;
        return false;
    }
    /*
     * MariaDB's my_sync_dir() is also a no-op outside Linux:
     * Windows has no portable fsync-style directory operation. File data and
     * mappings are flushed explicitly before this metadata ordering point.
     */
    return true;
}

#elif defined(__APPLE__)

#  include <sys/file.h>

#  include <cerrno>
#  include <cstdarg>
#  include <filesystem>
#  include <limits.h>
#  include <map>
#  include <mutex>
#  include <string>
#  include <utility>

namespace {

constexpr const char *k_range_lock_directory_suffix = ".mylite-range-locks";

std::mutex apple_range_lock_mutex;
std::map<std::pair<int, off_t>, int> apple_range_locks;
pid_t apple_range_lock_pid = ::getpid();

void reset_apple_range_locks_after_fork_locked() {
    const pid_t current_pid = ::getpid();
    if (current_pid == apple_range_lock_pid) {
        return;
    }
    /*
     * A forked child shares each inherited flock open-file description with
     * its parent. Close the child's references without LOCK_UN so the parent
     * retains its exclusion boundary.
     */
    for (const auto &lock : apple_range_locks) {
        static_cast<void>(::close(lock.second));
    }
    apple_range_locks.clear();
    apple_range_lock_pid = current_pid;
}

std::string apple_range_lock_directory(int fd) {
    char path[PATH_MAX] = {};
    if (::fcntl(fd, F_GETPATH, path) != 0) {
        return {};
    }
    return std::string(path) + k_range_lock_directory_suffix;
}

int apple_open_range_lock(int fd, off_t start) {
    const std::string directory = apple_range_lock_directory(fd);
    if (directory.empty()) {
        return -1;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        errno = EIO;
        return -1;
    }
    const std::filesystem::path path =
        std::filesystem::path(directory) / ("byte-" + std::to_string(start));
    return ::open(path.string().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
}

} // namespace

int mylite_ownerless_close(int fd) {
    {
        std::lock_guard<std::mutex> guard(apple_range_lock_mutex);
        reset_apple_range_locks_after_fork_locked();
        for (auto lock = apple_range_locks.begin(); lock != apple_range_locks.end();) {
            if (lock->first.first != fd) {
                ++lock;
                continue;
            }
            static_cast<void>(::flock(lock->second, LOCK_UN));
            static_cast<void>(::close(lock->second));
            lock = apple_range_locks.erase(lock);
        }
    }
    return ::close(fd);
}

int mylite_ownerless_fcntl(int fd, int command, ...) {
    va_list arguments;
    va_start(arguments, command);
    if (command == F_GETFL) {
        va_end(arguments);
        return ::fcntl(fd, command);
    }
    if (command == F_DUPFD || command == F_DUPFD_CLOEXEC) {
        const int minimum = va_arg(arguments, int);
        va_end(arguments);
        return ::fcntl(fd, command, minimum);
    }

    auto *lock = va_arg(arguments, struct flock *);
    va_end(arguments);
    const bool is_set_lock = command == F_SETLK
#  if defined(F_OFD_SETLK)
                             || command == F_OFD_SETLK
#  endif
        ;
    if (!is_set_lock || lock == nullptr || lock->l_whence != SEEK_SET || lock->l_start < 0 ||
        lock->l_len != 1 ||
        (lock->l_type != F_RDLCK && lock->l_type != F_WRLCK && lock->l_type != F_UNLCK)) {
        errno = EINVAL;
        return -1;
    }

    const std::pair<int, off_t> key{fd, lock->l_start};
    std::lock_guard<std::mutex> guard(apple_range_lock_mutex);
    reset_apple_range_locks_after_fork_locked();
    const auto existing = apple_range_locks.find(key);
    if (lock->l_type == F_UNLCK) {
        if (existing == apple_range_locks.end()) {
            return 0;
        }
        if (::flock(existing->second, LOCK_UN) != 0) {
            return -1;
        }
        static_cast<void>(::close(existing->second));
        apple_range_locks.erase(existing);
        return 0;
    }

    const int operation = (lock->l_type == F_RDLCK ? LOCK_SH : LOCK_EX) | LOCK_NB;
    if (existing != apple_range_locks.end()) {
        return ::flock(existing->second, operation);
    }
    const int range_fd = apple_open_range_lock(fd, lock->l_start);
    if (range_fd < 0) {
        return -1;
    }
    if (::flock(range_fd, operation) != 0) {
        const int saved_errno = errno;
        static_cast<void>(::close(range_fd));
        errno = saved_errno;
        return -1;
    }
    apple_range_locks.emplace(key, range_fd);
    return 0;
}

void mylite_ownerless_cleanup_range_lock_artifacts(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(std::string(path) + k_range_lock_directory_suffix, error);
}

#endif

#ifndef MYLITE_OWNERLESS_PLATFORM_IO_H
#define MYLITE_OWNERLESS_PLATFORM_IO_H

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)

#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>

using mylite_ownerless_offset_t = std::int64_t;
using mylite_ownerless_ssize_t = std::int64_t;

struct mylite_ownerless_stat_record {
    mylite_ownerless_offset_t st_size;
    std::uint64_t st_dev;
    std::uint64_t st_ino;
    unsigned st_mode;
};

struct mylite_ownerless_flock_record {
    short l_type;
    short l_whence;
    mylite_ownerless_offset_t l_start;
    mylite_ownerless_offset_t l_len;
    int l_pid;
};

using mylite_ownerless_file_info = struct mylite_ownerless_stat_record;
using mylite_ownerless_file_lock = struct mylite_ownerless_flock_record;

#  ifndef O_RDONLY
#    define O_RDONLY _O_RDONLY
#  endif
#  ifndef O_WRONLY
#    define O_WRONLY _O_WRONLY
#  endif
#  ifndef O_RDWR
#    define O_RDWR _O_RDWR
#  endif
#  ifndef O_CREAT
#    define O_CREAT _O_CREAT
#  endif
#  ifndef O_TRUNC
#    define O_TRUNC _O_TRUNC
#  endif
#  ifndef O_ACCMODE
#    define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#  endif
#  ifndef O_CLOEXEC
#    define O_CLOEXEC 0
#  endif
#  ifndef O_DIRECTORY
#    define O_DIRECTORY 0x10000000
#  endif
#  ifndef PROT_READ
#    define PROT_READ 0x1
#  endif
#  ifndef PROT_WRITE
#    define PROT_WRITE 0x2
#  endif
#  ifndef MAP_SHARED
#    define MAP_SHARED 0x1
#  endif
#  ifndef MAP_FAILED
#    define MAP_FAILED ((void *)-1)
#  endif
#  ifndef MS_SYNC
#    define MS_SYNC 0x4
#  endif
#  ifndef F_GETFL
#    define F_GETFL 3
#  endif
#  ifndef F_SETLK
#    define F_SETLK 6
#  endif
#  ifndef F_OFD_SETLK
#    define F_OFD_SETLK F_SETLK
#  endif
#  ifndef F_DUPFD_CLOEXEC
#    define F_DUPFD_CLOEXEC 1030
#  endif
#  ifndef F_RDLCK
#    define F_RDLCK 0
#  endif
#  ifndef F_WRLCK
#    define F_WRLCK 1
#  endif
#  ifndef F_UNLCK
#    define F_UNLCK 2
#  endif
#  ifndef LOCK_SH
#    define LOCK_SH 1
#  endif
#  ifndef LOCK_EX
#    define LOCK_EX 2
#  endif
#  ifndef LOCK_NB
#    define LOCK_NB 4
#  endif
#  ifndef LOCK_UN
#    define LOCK_UN 8
#  endif
#  ifndef S_ISREG
#    define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#  endif
#  ifndef EWOULDBLOCK
#    define EWOULDBLOCK EAGAIN
#  endif

int mylite_ownerless_open(const char *path, int flags, ...);
int mylite_ownerless_close(int fd);
mylite_ownerless_ssize_t mylite_ownerless_read(int fd, void *data, std::size_t size);
mylite_ownerless_ssize_t mylite_ownerless_write(int fd, const void *data, std::size_t size);
mylite_ownerless_ssize_t mylite_ownerless_pread(
    int fd,
    void *data,
    std::size_t size,
    mylite_ownerless_offset_t offset
);
mylite_ownerless_ssize_t mylite_ownerless_pwrite(
    int fd,
    const void *data,
    std::size_t size,
    mylite_ownerless_offset_t offset
);
int mylite_ownerless_fsync(int fd);
int mylite_ownerless_fdatasync(int fd);
int mylite_ownerless_ftruncate(int fd, mylite_ownerless_offset_t size);
int mylite_ownerless_fstat(int fd, mylite_ownerless_file_info *out_info);
int mylite_ownerless_stat(const char *path, mylite_ownerless_file_info *out_info);
void *mylite_ownerless_mmap(
    void *address,
    std::size_t size,
    int protection,
    int flags,
    int fd,
    mylite_ownerless_offset_t offset
);
int mylite_ownerless_munmap(void *address, std::size_t size);
int mylite_ownerless_msync(void *address, std::size_t size, int flags);
int mylite_ownerless_fcntl(int fd, int command, ...);
int mylite_ownerless_flock(int fd, int operation);
std::uint64_t mylite_ownerless_current_process_id(void);
bool mylite_ownerless_process_id_is_alive(std::uint64_t process_id);
bool mylite_ownerless_sync_directory_path(const char *path);

#  if !defined(MYLITE_OWNERLESS_PLATFORM_IO_IMPLEMENTATION)
#    define off_t mylite_ownerless_offset_t
#    define ssize_t mylite_ownerless_ssize_t
#  endif

#else

#  include <cerrno>
#  include <fcntl.h>
#  include <limits>
#  include <signal.h>
#  include <sys/file.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>

using mylite_ownerless_offset_t = off_t;
using mylite_ownerless_ssize_t = ssize_t;
using mylite_ownerless_file_info = struct stat;
using mylite_ownerless_file_lock = struct flock;

inline int mylite_ownerless_open(const char *path, int flags) {
    return ::open(path, flags);
}

inline int mylite_ownerless_open(const char *path, int flags, mode_t mode) {
    return ::open(path, flags, mode);
}

inline mylite_ownerless_ssize_t mylite_ownerless_read(int fd, void *data, std::size_t size) {
    return ::read(fd, data, size);
}

inline mylite_ownerless_ssize_t mylite_ownerless_write(int fd, const void *data, std::size_t size) {
    return ::write(fd, data, size);
}

inline mylite_ownerless_ssize_t mylite_ownerless_pread(
    int fd,
    void *data,
    std::size_t size,
    mylite_ownerless_offset_t offset
) {
    return ::pread(fd, data, size, offset);
}

inline mylite_ownerless_ssize_t mylite_ownerless_pwrite(
    int fd,
    const void *data,
    std::size_t size,
    mylite_ownerless_offset_t offset
) {
    return ::pwrite(fd, data, size, offset);
}

inline int mylite_ownerless_fsync(int fd) {
    return ::fsync(fd);
}

inline int mylite_ownerless_fdatasync(int fd) {
#  if defined(__APPLE__)
    return ::fsync(fd);
#  else
    return ::fdatasync(fd);
#  endif
}

inline int mylite_ownerless_ftruncate(int fd, mylite_ownerless_offset_t size) {
    return ::ftruncate(fd, size);
}

inline int mylite_ownerless_fstat(int fd, mylite_ownerless_file_info *out_info) {
    return ::fstat(fd, out_info);
}

inline int mylite_ownerless_stat(const char *path, mylite_ownerless_file_info *out_info) {
    return ::stat(path, out_info);
}

inline void *mylite_ownerless_mmap(
    void *address,
    std::size_t size,
    int protection,
    int flags,
    int fd,
    mylite_ownerless_offset_t offset
) {
    return ::mmap(address, size, protection, flags, fd, offset);
}

inline int mylite_ownerless_munmap(void *address, std::size_t size) {
    return ::munmap(address, size);
}

inline int mylite_ownerless_msync(void *address, std::size_t size, int flags) {
    return ::msync(address, size, flags);
}

inline int mylite_ownerless_flock(int fd, int operation) {
    return ::flock(fd, operation);
}

inline std::uint64_t mylite_ownerless_current_process_id(void) {
    return static_cast<std::uint64_t>(::getpid());
}

inline bool mylite_ownerless_process_id_is_alive(std::uint64_t process_id) {
    if (process_id == 0U ||
        process_id > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return false;
    }
    if (::kill(static_cast<pid_t>(process_id), 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

inline bool mylite_ownerless_sync_directory_path(const char *path) {
    const int fd = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    while (::fsync(fd) != 0) {
        if (errno != EINTR) {
            ok = false;
            break;
        }
    }
    while (::close(fd) != 0) {
        if (errno != EINTR) {
            ok = false;
            break;
        }
    }
    return ok;
}

#  if defined(__APPLE__)
int mylite_ownerless_close(int fd);
int mylite_ownerless_fcntl(int fd, int command, ...);
void mylite_ownerless_cleanup_range_lock_artifacts(const char *path);
#  else
inline int mylite_ownerless_close(int fd) {
    return ::close(fd);
}

inline int mylite_ownerless_fcntl(int fd, int command) {
    return ::fcntl(fd, command);
}

template <typename Argument>
inline int mylite_ownerless_fcntl(int fd, int command, Argument argument) {
    return ::fcntl(fd, command, argument);
}
#  endif

#endif

#endif

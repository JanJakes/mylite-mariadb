#ifndef MYLITE_OWNERLESS_PLATFORM_IO_H
#define MYLITE_OWNERLESS_PLATFORM_IO_H

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)

#  include <fcntl.h>
#  include <sys/stat.h>

using mylite_ownerless_offset_t = std::int64_t;
using mylite_ownerless_ssize_t = std::int64_t;

struct mylite_ownerless_stat {
    mylite_ownerless_offset_t st_size;
    std::uint64_t st_dev;
    std::uint64_t st_ino;
    unsigned st_mode;
};

struct mylite_ownerless_flock {
    short l_type;
    short l_whence;
    mylite_ownerless_offset_t l_start;
    mylite_ownerless_offset_t l_len;
    int l_pid;
};

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
int mylite_ownerless_fstat(int fd, struct mylite_ownerless_stat *out_info);
int mylite_ownerless_stat(const char *path, struct mylite_ownerless_stat *out_info);
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

#  define open(...) mylite_ownerless_open(__VA_ARGS__)
#  define close(...) mylite_ownerless_close(__VA_ARGS__)
#  define read(...) mylite_ownerless_read(__VA_ARGS__)
#  define write(...) mylite_ownerless_write(__VA_ARGS__)
#  define pread(...) mylite_ownerless_pread(__VA_ARGS__)
#  define pwrite(...) mylite_ownerless_pwrite(__VA_ARGS__)
#  define fsync(...) mylite_ownerless_fsync(__VA_ARGS__)
#  define fdatasync(...) mylite_ownerless_fdatasync(__VA_ARGS__)
#  define ftruncate(...) mylite_ownerless_ftruncate(__VA_ARGS__)
#  define fstat(...) mylite_ownerless_fstat(__VA_ARGS__)
#  define stat mylite_ownerless_stat
#  define mmap(...) mylite_ownerless_mmap(__VA_ARGS__)
#  define munmap(...) mylite_ownerless_munmap(__VA_ARGS__)
#  define msync(...) mylite_ownerless_msync(__VA_ARGS__)
#  define fcntl(...) mylite_ownerless_fcntl(__VA_ARGS__)
#  define flock mylite_ownerless_flock
#  define off_t mylite_ownerless_offset_t
#  define ssize_t mylite_ownerless_ssize_t

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

#    define close(...) mylite_ownerless_close(__VA_ARGS__)
#    define fcntl(...) mylite_ownerless_fcntl(__VA_ARGS__)
#  endif

#endif

#endif

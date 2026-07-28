#include "ownerless_probe.h"

#include "ownerless_platform_io.h"
#include "ownerless_process_registry.h"
#include "ownerless_wait.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#  include <linux/magic.h>
#  include <sys/vfs.h>
#elif defined(__APPLE__)
#  include <sys/mount.h>
#endif

#ifndef MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
#  define MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS 0
#endif

namespace {

constexpr int k_probe_timeout_ms = 5000;
constexpr std::size_t k_probe_page_size = 4096;
constexpr off_t k_probe_page_size_offset = static_cast<off_t>(k_probe_page_size);
constexpr mode_t k_probe_file_mode = 0600;

int run_ownerless_probe(const std::string &root, mylite_ownerless_probe_result *result);
void apply_ownerless_probe_test_failures(mylite_ownerless_probe_result &result);
void compute_ownerless_probe_summary(mylite_ownerless_probe_result &result);
bool probe_mmap_shared_visibility(const std::string &root);
bool probe_byte_range_locks(const std::string &root);
bool probe_lock_release_on_exit(const std::string &root);
bool probe_lock_close_isolation(const std::string &root);
bool probe_grow_remap(const std::string &root);
bool probe_wait_backend(const std::string &root);
bool probe_process_identity();
bool set_write_lock(int fd, off_t start, off_t length);
int try_write_lock(int fd, off_t start, off_t length);
bool unlock_range(int fd, off_t start, off_t length);
int ownerless_lock_command();
std::string make_temp_root(void);
std::string make_temp_root_under(const std::string &parent);
std::string path_join(const std::string &directory, const char *name);
int open_probe_file(const std::string &path);
bool truncate_file(int fd, off_t size);
void *map_file(int fd, std::size_t size);
bool signal_pipe(int pipe_fd);
bool wait_for_pipe(int pipe_fd);
bool wait_for_child_success(pid_t child);
void close_pipe(int pipe_fd);
void cleanup_probe_file(const std::string &path);
void cleanup_probe_root(const std::string &root);

} // namespace

int mylite_ownerless_probe_platform(mylite_ownerless_probe_result *result) {
    if (result == nullptr) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    const std::string root = make_temp_root();
    if (root.empty()) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    const int probe_result = run_ownerless_probe(root, result);
    cleanup_probe_root(root);
    return probe_result;
}

int mylite_ownerless_probe_directory(const char *directory, mylite_ownerless_probe_result *result) {
    if (directory == nullptr || directory[0] == '\0' || result == nullptr) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    const std::string root = make_temp_root_under(directory);
    if (root.empty()) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    const int probe_result = run_ownerless_probe(root, result);
    cleanup_probe_root(root);
    return probe_result;
}

int mylite_ownerless_probe_filesystem_type(const char *directory, uint64_t *out_type) {
    if (directory == nullptr || directory[0] == '\0' || out_type == nullptr) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

#if defined(__linux__)
    struct statfs filesystem = {};
    if (statfs(directory, &filesystem) != 0) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    *out_type = static_cast<std::uint64_t>(filesystem.f_type);
    return MYLITE_OWNERLESS_PROBE_OK;
#else
    (void)directory;
    (void)out_type;
    return MYLITE_OWNERLESS_PROBE_ERROR;
#endif
}

int mylite_ownerless_probe_filesystem(
    const char *directory,
    mylite_ownerless_filesystem_info *out_info
) {
    if (directory == nullptr || directory[0] == '\0' || out_info == nullptr) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    std::memset(out_info, 0, sizeof(*out_info));
    out_info->size = static_cast<std::uint32_t>(sizeof(*out_info));

#if defined(__linux__)
    struct statfs filesystem = {};
    mylite_ownerless_file_info directory_stat = {};
    if (statfs(directory, &filesystem) != 0 ||
        mylite_ownerless_stat(directory, &directory_stat) != 0) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    out_info->is_local = 1U;
    out_info->volume_identity = static_cast<std::uint64_t>(directory_stat.st_dev);
    if (filesystem.f_type == EXT4_SUPER_MAGIC) {
        out_info->kind = MYLITE_OWNERLESS_FILESYSTEM_EXT4;
        std::strncpy(out_info->name, "ext4", sizeof(out_info->name) - 1U);
    } else if (filesystem.f_type == XFS_SUPER_MAGIC) {
        out_info->kind = MYLITE_OWNERLESS_FILESYSTEM_XFS;
        std::strncpy(out_info->name, "xfs", sizeof(out_info->name) - 1U);
    } else if (filesystem.f_type == TMPFS_MAGIC) {
        out_info->kind = MYLITE_OWNERLESS_FILESYSTEM_TMPFS;
        std::strncpy(out_info->name, "tmpfs", sizeof(out_info->name) - 1U);
    } else if (filesystem.f_type == OVERLAYFS_SUPER_MAGIC) {
        out_info->kind = MYLITE_OWNERLESS_FILESYSTEM_OVERLAY;
        std::strncpy(out_info->name, "overlay", sizeof(out_info->name) - 1U);
    } else {
        std::snprintf(
            out_info->name,
            sizeof(out_info->name),
            "linux-0x%llx",
            static_cast<unsigned long long>(filesystem.f_type)
        );
    }
    out_info->is_admitted = out_info->kind != MYLITE_OWNERLESS_FILESYSTEM_UNKNOWN ? 1U : 0U;
    return MYLITE_OWNERLESS_PROBE_OK;
#elif defined(__APPLE__)
    struct statfs filesystem = {};
    mylite_ownerless_file_info directory_stat = {};
    if (statfs(directory, &filesystem) != 0 ||
        mylite_ownerless_stat(directory, &directory_stat) != 0) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    out_info->is_local = (filesystem.f_flags & MNT_LOCAL) != 0U ? 1U : 0U;
    out_info->volume_identity = static_cast<std::uint64_t>(directory_stat.st_dev);
    std::strncpy(out_info->name, filesystem.f_fstypename, sizeof(out_info->name) - 1U);
    if (out_info->is_local != 0U && std::strcmp(filesystem.f_fstypename, "apfs") == 0) {
        out_info->kind = MYLITE_OWNERLESS_FILESYSTEM_APFS;
        out_info->is_admitted = 1U;
    }
    return MYLITE_OWNERLESS_PROBE_OK;
#else
    (void)directory;
    return MYLITE_OWNERLESS_PROBE_ERROR;
#endif
}

int mylite_ownerless_filesystem_type_is_validated_local(uint64_t filesystem_type) {
#if defined(__linux__)
    return filesystem_type == static_cast<std::uint64_t>(EXT4_SUPER_MAGIC) ||
                   filesystem_type == static_cast<std::uint64_t>(XFS_SUPER_MAGIC) ||
                   filesystem_type == static_cast<std::uint64_t>(TMPFS_MAGIC) ||
                   filesystem_type == static_cast<std::uint64_t>(OVERLAYFS_SUPER_MAGIC)
               ? 1
               : 0;
#else
    (void)filesystem_type;
    return 0;
#endif
}

namespace {

int run_ownerless_probe(const std::string &root, mylite_ownerless_probe_result *result) {
    std::memset(result, 0, sizeof(*result));
    result->size = static_cast<std::uint32_t>(sizeof(*result));

    result->mmap_shared_visibility = probe_mmap_shared_visibility(root) ? 1U : 0U;
    result->byte_range_locks = probe_byte_range_locks(root) ? 1U : 0U;
    result->lock_release_on_exit = probe_lock_release_on_exit(root) ? 1U : 0U;
    result->lock_close_isolation = probe_lock_close_isolation(root) ? 1U : 0U;
    result->grow_remap = probe_grow_remap(root) ? 1U : 0U;
    result->wait_backend = probe_wait_backend(root) ? 1U : 0U;
    result->fast_wait_backend = mylite_ownerless_wait_backend_is_fast() != 0 ? 1U : 0U;
    result->process_identity = probe_process_identity() ? 1U : 0U;
    compute_ownerless_probe_summary(*result);
    apply_ownerless_probe_test_failures(*result);
    return MYLITE_OWNERLESS_PROBE_OK;
}

void apply_ownerless_probe_test_failures(mylite_ownerless_probe_result &result) {
#if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    const char *failure = std::getenv("MYLITE_OWNERLESS_TEST_PROBE_FAIL");
    if (failure == nullptr || failure[0] == '\0') {
        return;
    }

    if (std::strcmp(failure, "byte-range-locks") == 0) {
        result.byte_range_locks = 0U;
    } else if (std::strcmp(failure, "lock-release-on-exit") == 0) {
        result.lock_release_on_exit = 0U;
    } else if (std::strcmp(failure, "lock-close-isolation") == 0) {
        result.lock_close_isolation = 0U;
    } else if (std::strcmp(failure, "grow-remap") == 0) {
        result.grow_remap = 0U;
    } else if (std::strcmp(failure, "wait-backend") == 0) {
        result.wait_backend = 0U;
    } else if (std::strcmp(failure, "process-identity") == 0) {
        result.process_identity = 0U;
    } else {
        result.mmap_shared_visibility = 0U;
    }
    compute_ownerless_probe_summary(result);
#else
    (void)result;
#endif
}

void compute_ownerless_probe_summary(mylite_ownerless_probe_result &result) {
    result.required_primitives =
        result.mmap_shared_visibility != 0U && result.byte_range_locks != 0U &&
                result.lock_release_on_exit != 0U && result.lock_close_isolation != 0U &&
                result.grow_remap != 0U && result.wait_backend != 0U &&
                result.process_identity != 0U
            ? 1U
            : 0U;
    result.platform_candidate =
        result.required_primitives != 0U && result.fast_wait_backend != 0U ? 1U : 0U;
}

bool probe_mmap_shared_visibility(const std::string &root) {
    const std::string path = path_join(root, "mmap-shared.bin");
    int parent_to_child[2] = {-1, -1};
    int child_to_parent[2] = {-1, -1};
    const int fd = open_probe_file(path);
    if (fd < 0) {
        return false;
    }
    if (!truncate_file(fd, k_probe_page_size_offset) || pipe(parent_to_child) != 0 ||
        pipe(child_to_parent) != 0) {
        close_pipe(parent_to_child[0]);
        close_pipe(parent_to_child[1]);
        close_pipe(child_to_parent[0]);
        close_pipe(child_to_parent[1]);
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }

    auto *words = static_cast<std::uint32_t *>(map_file(fd, k_probe_page_size));
    if (words == nullptr) {
        close_pipe(parent_to_child[0]);
        close_pipe(parent_to_child[1]);
        close_pipe(child_to_parent[0]);
        close_pipe(child_to_parent[1]);
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }
    words[0] = 0x11223344U;
    static_cast<void>(mylite_ownerless_msync(words, sizeof(words[0]), MS_SYNC));

    const pid_t child = fork();
    if (child < 0) {
        close_pipe(parent_to_child[0]);
        close_pipe(parent_to_child[1]);
        close_pipe(child_to_parent[0]);
        close_pipe(child_to_parent[1]);
        static_cast<void>(mylite_ownerless_munmap(words, k_probe_page_size));
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }
    if (child == 0) {
        close_pipe(parent_to_child[1]);
        close_pipe(child_to_parent[0]);
        if (!wait_for_pipe(parent_to_child[0])) {
            _exit(1);
        }

        const int child_fd = open_probe_file(path);
        if (child_fd < 0) {
            _exit(1);
        }
        auto *child_words = static_cast<std::uint32_t *>(map_file(child_fd, k_probe_page_size));
        if (child_words == nullptr) {
            static_cast<void>(mylite_ownerless_close(child_fd));
            _exit(1);
        }
        const bool ok = child_words[0] == 0x55667788U;
        child_words[1] = 0x99AABBCCU;
        static_cast<void>(mylite_ownerless_msync(child_words, k_probe_page_size, MS_SYNC));
        static_cast<void>(mylite_ownerless_munmap(child_words, k_probe_page_size));
        static_cast<void>(mylite_ownerless_close(child_fd));
        if (!ok || !signal_pipe(child_to_parent[1])) {
            _exit(1);
        }
        _exit(0);
    }

    close_pipe(parent_to_child[0]);
    close_pipe(child_to_parent[1]);
    words[0] = 0x55667788U;
    static_cast<void>(mylite_ownerless_msync(words, sizeof(words[0]), MS_SYNC));
    const bool signal_ok = signal_pipe(parent_to_child[1]);
    bool child_signal_ok = false;
    if (signal_ok) {
        child_signal_ok = wait_for_pipe(child_to_parent[0]);
    } else {
        close_pipe(child_to_parent[0]);
    }
    const bool child_ok = wait_for_child_success(child);
    const bool ok = signal_ok && child_signal_ok && child_ok && words[1] == 0x99AABBCCU;

    static_cast<void>(mylite_ownerless_munmap(words, k_probe_page_size));
    static_cast<void>(mylite_ownerless_close(fd));
    cleanup_probe_file(path);
    return ok;
}

bool probe_byte_range_locks(const std::string &root) {
    const std::string path = path_join(root, "range-lock.bin");
    const int fd = open_probe_file(path);
    if (fd < 0) {
        return false;
    }
    if (!truncate_file(fd, k_probe_page_size_offset) || !set_write_lock(fd, 11, 1)) {
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        static_cast<void>(unlock_range(fd, 11, 1));
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }
    if (child == 0) {
        const int child_fd = open_probe_file(path);
        if (child_fd < 0) {
            _exit(1);
        }
        const int lock_result = try_write_lock(child_fd, 11, 1);
        static_cast<void>(mylite_ownerless_close(child_fd));
        _exit(lock_result == EAGAIN || lock_result == EACCES ? 0 : 1);
    }

    const bool ok = wait_for_child_success(child);
    static_cast<void>(unlock_range(fd, 11, 1));
    static_cast<void>(mylite_ownerless_close(fd));
    cleanup_probe_file(path);
    return ok;
}

bool probe_lock_release_on_exit(const std::string &root) {
    const std::string path = path_join(root, "release-on-exit.bin");
    int ready_pipe[2] = {-1, -1};
    const int fd = open_probe_file(path);
    if (fd < 0) {
        return false;
    }
    if (!truncate_file(fd, k_probe_page_size_offset) || pipe(ready_pipe) != 0) {
        close_pipe(ready_pipe[0]);
        close_pipe(ready_pipe[1]);
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        close_pipe(ready_pipe[0]);
        close_pipe(ready_pipe[1]);
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }
    if (child == 0) {
        close_pipe(ready_pipe[0]);
        const int child_fd = open_probe_file(path);
        if (child_fd < 0 || !set_write_lock(child_fd, 23, 1) || !signal_pipe(ready_pipe[1])) {
            if (child_fd >= 0) {
                static_cast<void>(mylite_ownerless_close(child_fd));
            }
            _exit(1);
        }
        _exit(0);
    }

    close_pipe(ready_pipe[1]);
    const bool ready = wait_for_pipe(ready_pipe[0]);
    const bool child_ok = wait_for_child_success(child);
    const bool released = ready && child_ok && set_write_lock(fd, 23, 1) && unlock_range(fd, 23, 1);
    const bool ok = ready && child_ok && released;

    static_cast<void>(mylite_ownerless_close(fd));
    cleanup_probe_file(path);
    return ok;
}

bool probe_lock_close_isolation(const std::string &root) {
    const std::string path = path_join(root, "lock-close-isolation.bin");
    const int lock_fd = open_probe_file(path);
    const int unrelated_fd = open_probe_file(path);
    if (lock_fd < 0 || unrelated_fd < 0) {
        if (lock_fd >= 0) {
            static_cast<void>(mylite_ownerless_close(lock_fd));
        }
        if (unrelated_fd >= 0) {
            static_cast<void>(mylite_ownerless_close(unrelated_fd));
        }
        cleanup_probe_file(path);
        return false;
    }
    if (!set_write_lock(lock_fd, 41, 1)) {
        static_cast<void>(mylite_ownerless_close(unrelated_fd));
        static_cast<void>(mylite_ownerless_close(lock_fd));
        cleanup_probe_file(path);
        return false;
    }
    static_cast<void>(mylite_ownerless_close(unrelated_fd));

    const pid_t child = fork();
    if (child < 0) {
        static_cast<void>(unlock_range(lock_fd, 41, 1));
        static_cast<void>(mylite_ownerless_close(lock_fd));
        cleanup_probe_file(path);
        return false;
    }
    if (child == 0) {
        const int child_fd = open_probe_file(path);
        if (child_fd < 0) {
            _exit(1);
        }
        const int lock_result = try_write_lock(child_fd, 41, 1);
        static_cast<void>(mylite_ownerless_close(child_fd));
        _exit(lock_result == EACCES || lock_result == EAGAIN ? 0 : 1);
    }

    const bool child_ok = wait_for_child_success(child);
    const bool unlock_ok = unlock_range(lock_fd, 41, 1);
    static_cast<void>(mylite_ownerless_close(lock_fd));
    cleanup_probe_file(path);
    return child_ok && unlock_ok;
}

bool probe_grow_remap(const std::string &root) {
    const std::string path = path_join(root, "grow-remap.bin");
    const int fd = open_probe_file(path);
    if (fd < 0) {
        return false;
    }
    if (!truncate_file(fd, k_probe_page_size_offset)) {
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }

    auto *first_mapping = static_cast<std::uint32_t *>(map_file(fd, k_probe_page_size));
    if (first_mapping == nullptr) {
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }
    first_mapping[0] = 0xCAFEBABEU;
    static_cast<void>(mylite_ownerless_msync(first_mapping, sizeof(first_mapping[0]), MS_SYNC));
    static_cast<void>(mylite_ownerless_munmap(first_mapping, k_probe_page_size));

    if (!truncate_file(fd, k_probe_page_size_offset * 2)) {
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }
    auto *second_mapping = static_cast<std::uint32_t *>(map_file(fd, k_probe_page_size * 2U));
    if (second_mapping == nullptr) {
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }
    const bool ok = second_mapping[0] == 0xCAFEBABEU;
    static_cast<void>(mylite_ownerless_munmap(second_mapping, k_probe_page_size * 2U));
    static_cast<void>(mylite_ownerless_close(fd));
    cleanup_probe_file(path);
    return ok;
}

bool probe_wait_backend(const std::string &root) {
    const std::string path = path_join(root, "wait-backend.bin");
    int child_ready[2] = {-1, -1};
    const int fd = open_probe_file(path);
    if (fd < 0) {
        return false;
    }
    if (!truncate_file(fd, k_probe_page_size_offset) || pipe(child_ready) != 0) {
        close_pipe(child_ready[0]);
        close_pipe(child_ready[1]);
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }

    auto *word = static_cast<mylite_ownerless_wait_word *>(map_file(fd, k_probe_page_size));
    if (word == nullptr) {
        close_pipe(child_ready[0]);
        close_pipe(child_ready[1]);
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }
    mylite_ownerless_wait_store(word, 0U);

    const pid_t child = fork();
    if (child < 0) {
        close_pipe(child_ready[0]);
        close_pipe(child_ready[1]);
        static_cast<void>(mylite_ownerless_munmap(word, k_probe_page_size));
        static_cast<void>(mylite_ownerless_close(fd));
        cleanup_probe_file(path);
        return false;
    }
    if (child == 0) {
        close_pipe(child_ready[0]);
        const int child_fd = open_probe_file(path);
        if (child_fd < 0) {
            _exit(1);
        }
        auto *child_word =
            static_cast<mylite_ownerless_wait_word *>(map_file(child_fd, k_probe_page_size));
        if (child_word == nullptr) {
            static_cast<void>(mylite_ownerless_close(child_fd));
            _exit(1);
        }
        if (!signal_pipe(child_ready[1])) {
            static_cast<void>(mylite_ownerless_munmap(child_word, k_probe_page_size));
            static_cast<void>(mylite_ownerless_close(child_fd));
            _exit(1);
        }
        const int wait_result = mylite_ownerless_wait_for_change(
            child_word,
            0U,
            static_cast<unsigned>(k_probe_timeout_ms)
        );
        const bool ok =
            wait_result == MYLITE_OWNERLESS_WAIT_OK && mylite_ownerless_wait_load(child_word) == 1U;
        static_cast<void>(mylite_ownerless_munmap(child_word, k_probe_page_size));
        static_cast<void>(mylite_ownerless_close(child_fd));
        _exit(ok ? 0 : 1);
    }

    close_pipe(child_ready[1]);
    const bool ready = wait_for_pipe(child_ready[0]);
    bool wake_ok = false;
    if (ready) {
        mylite_ownerless_wait_store(word, 1U);
        wake_ok = mylite_ownerless_wait_wake(word) == MYLITE_OWNERLESS_WAIT_OK;
    }
    const bool child_ok = wait_for_child_success(child);
    const bool ok = ready && wake_ok && child_ok;
    static_cast<void>(mylite_ownerless_munmap(word, k_probe_page_size));
    static_cast<void>(mylite_ownerless_close(fd));
    cleanup_probe_file(path);
    return ok;
}

bool probe_process_identity() {
    mylite_ownerless_process_identity identity = {};
    if (mylite_ownerless_current_process_identity(&identity) !=
        MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return false;
    }
    if (mylite_ownerless_process_identity_is_alive(&identity, nullptr) == 0) {
        return false;
    }

    mylite_ownerless_process_identity stale_identity = identity;
    stale_identity.start_time += 1U;
    return mylite_ownerless_process_identity_is_alive(&stale_identity, nullptr) == 0;
}

bool set_write_lock(int fd, off_t start, off_t length) {
    return try_write_lock(fd, start, length) == 0;
}

int try_write_lock(int fd, off_t start, off_t length) {
    mylite_ownerless_file_lock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;

    if (mylite_ownerless_fcntl(fd, ownerless_lock_command(), &lock) == 0) {
        return 0;
    }
    return errno;
}

bool unlock_range(int fd, off_t start, off_t length) {
    mylite_ownerless_file_lock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;

    return mylite_ownerless_fcntl(fd, ownerless_lock_command(), &lock) == 0;
}

int ownerless_lock_command() {
#if defined(F_OFD_SETLK)
    return F_OFD_SETLK;
#else
    return F_SETLK;
#endif
}

std::string make_temp_root(void) {
    char template_path[] = "/tmp/mylite-ownerless-probe.XXXXXX";
    char *root = mkdtemp(template_path);

    return root == nullptr ? std::string() : std::string(root);
}

std::string make_temp_root_under(const std::string &parent) {
    std::string template_path = parent + "/.mylite-ownerless-probe.XXXXXX";
    std::vector<char> buffer(template_path.begin(), template_path.end());
    buffer.push_back('\0');
    char *root = mkdtemp(buffer.data());

    return root == nullptr ? std::string() : std::string(root);
}

std::string path_join(const std::string &directory, const char *name) {
    return directory + "/" + name;
}

int open_probe_file(const std::string &path) {
    return mylite_ownerless_open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, k_probe_file_mode);
}

bool truncate_file(int fd, off_t size) {
    return mylite_ownerless_ftruncate(fd, size) == 0;
}

void *map_file(int fd, std::size_t size) {
    void *mapping = mylite_ownerless_mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    return mapping == MAP_FAILED ? nullptr : mapping;
}

bool signal_pipe(int pipe_fd) {
    const char value = 'x';
    const ssize_t written = mylite_ownerless_write(pipe_fd, &value, sizeof(value));
    close_pipe(pipe_fd);
    return written == static_cast<ssize_t>(sizeof(value));
}

bool wait_for_pipe(int pipe_fd) {
    pollfd read_poll = {};
    read_poll.fd = pipe_fd;
    read_poll.events = POLLIN;

    int poll_result;
    do {
        poll_result = poll(&read_poll, 1, k_probe_timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result <= 0 || (read_poll.revents & POLLIN) == 0) {
        close_pipe(pipe_fd);
        return false;
    }

    char value = '\0';
    const ssize_t read_size = mylite_ownerless_read(pipe_fd, &value, sizeof(value));
    close_pipe(pipe_fd);
    return read_size == static_cast<ssize_t>(sizeof(value)) && value == 'x';
}

bool wait_for_child_success(pid_t child) {
    int child_status = 0;
    pid_t waited;

    do {
        waited = waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
}

void close_pipe(int pipe_fd) {
    if (pipe_fd >= 0) {
        static_cast<void>(mylite_ownerless_close(pipe_fd));
    }
}

void cleanup_probe_file(const std::string &path) {
#if defined(__APPLE__)
    mylite_ownerless_cleanup_range_lock_artifacts(path.c_str());
#endif
    static_cast<void>(unlink(path.c_str()));
}

void cleanup_probe_root(const std::string &root) {
    static_cast<void>(rmdir(root.c_str()));
}

} // namespace

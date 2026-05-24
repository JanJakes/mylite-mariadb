#include "ownerless_probe.h"

#include "ownerless_wait.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int k_probe_timeout_ms = 5000;
constexpr std::size_t k_probe_page_size = 4096;
constexpr mode_t k_probe_file_mode = 0600;

bool probe_mmap_shared_visibility(const std::string &root);
bool probe_byte_range_locks(const std::string &root);
bool probe_lock_release_on_exit(const std::string &root);
bool probe_grow_remap(const std::string &root);
bool probe_wait_backend(const std::string &root);
bool set_write_lock(int fd, off_t start, off_t length);
int try_write_lock(int fd, off_t start, off_t length);
bool unlock_range(int fd, off_t start, off_t length);
std::string make_temp_root(void);
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

    std::memset(result, 0, sizeof(*result));
    result->size = static_cast<std::uint32_t>(sizeof(*result));

    const std::string root = make_temp_root();
    if (root.empty()) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    result->mmap_shared_visibility = probe_mmap_shared_visibility(root) ? 1U : 0U;
    result->byte_range_locks = probe_byte_range_locks(root) ? 1U : 0U;
    result->lock_release_on_exit = probe_lock_release_on_exit(root) ? 1U : 0U;
    result->grow_remap = probe_grow_remap(root) ? 1U : 0U;
    result->wait_backend = probe_wait_backend(root) ? 1U : 0U;
    result->fast_wait_backend =
        mylite_ownerless_wait_backend_is_fast() != 0 ? 1U : 0U;
    result->required_primitives =
        result->mmap_shared_visibility != 0U && result->byte_range_locks != 0U &&
                result->lock_release_on_exit != 0U && result->grow_remap != 0U &&
                result->wait_backend != 0U
            ? 1U
            : 0U;
    result->platform_candidate =
        result->required_primitives != 0U && result->fast_wait_backend != 0U ? 1U : 0U;

    cleanup_probe_root(root);
    return MYLITE_OWNERLESS_PROBE_OK;
}

namespace {

bool probe_mmap_shared_visibility(const std::string &root) {
    const std::string path = path_join(root, "mmap-shared.bin");
    int parent_to_child[2] = {-1, -1};
    int child_to_parent[2] = {-1, -1};
    const int fd = open_probe_file(path);
    if (fd < 0) {
        return false;
    }
    if (!truncate_file(fd, static_cast<off_t>(k_probe_page_size)) ||
        pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
        close_pipe(parent_to_child[0]);
        close_pipe(parent_to_child[1]);
        close_pipe(child_to_parent[0]);
        close_pipe(child_to_parent[1]);
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }

    auto *words = static_cast<std::uint32_t *>(map_file(fd, k_probe_page_size));
    if (words == nullptr) {
        close_pipe(parent_to_child[0]);
        close_pipe(parent_to_child[1]);
        close_pipe(child_to_parent[0]);
        close_pipe(child_to_parent[1]);
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }
    words[0] = 0x11223344U;
    static_cast<void>(msync(words, sizeof(words[0]), MS_SYNC));

    const pid_t child = fork();
    if (child < 0) {
        close_pipe(parent_to_child[0]);
        close_pipe(parent_to_child[1]);
        close_pipe(child_to_parent[0]);
        close_pipe(child_to_parent[1]);
        static_cast<void>(munmap(words, k_probe_page_size));
        static_cast<void>(close(fd));
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
            static_cast<void>(close(child_fd));
            _exit(1);
        }
        const bool ok = child_words[0] == 0x55667788U;
        child_words[1] = 0x99AABBCCU;
        static_cast<void>(msync(child_words, k_probe_page_size, MS_SYNC));
        static_cast<void>(munmap(child_words, k_probe_page_size));
        static_cast<void>(close(child_fd));
        if (!ok || !signal_pipe(child_to_parent[1])) {
            _exit(1);
        }
        _exit(0);
    }

    close_pipe(parent_to_child[0]);
    close_pipe(child_to_parent[1]);
    words[0] = 0x55667788U;
    static_cast<void>(msync(words, sizeof(words[0]), MS_SYNC));
    const bool signal_ok = signal_pipe(parent_to_child[1]);
    bool child_signal_ok = false;
    if (signal_ok) {
        child_signal_ok = wait_for_pipe(child_to_parent[0]);
    } else {
        close_pipe(child_to_parent[0]);
    }
    const bool child_ok = wait_for_child_success(child);
    const bool ok = signal_ok && child_signal_ok && child_ok && words[1] == 0x99AABBCCU;

    static_cast<void>(munmap(words, k_probe_page_size));
    static_cast<void>(close(fd));
    cleanup_probe_file(path);
    return ok;
}

bool probe_byte_range_locks(const std::string &root) {
    const std::string path = path_join(root, "range-lock.bin");
    const int fd = open_probe_file(path);
    if (fd < 0) {
        return false;
    }
    if (!truncate_file(fd, static_cast<off_t>(k_probe_page_size)) ||
        !set_write_lock(fd, 11, 7)) {
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        static_cast<void>(unlock_range(fd, 11, 7));
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }
    if (child == 0) {
        const int child_fd = open_probe_file(path);
        if (child_fd < 0) {
            _exit(1);
        }
        const int lock_result = try_write_lock(child_fd, 11, 7);
        static_cast<void>(close(child_fd));
        _exit(lock_result == EAGAIN || lock_result == EACCES ? 0 : 1);
    }

    const bool ok = wait_for_child_success(child);
    static_cast<void>(unlock_range(fd, 11, 7));
    static_cast<void>(close(fd));
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
    if (!truncate_file(fd, static_cast<off_t>(k_probe_page_size)) || pipe(ready_pipe) != 0) {
        close_pipe(ready_pipe[0]);
        close_pipe(ready_pipe[1]);
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        close_pipe(ready_pipe[0]);
        close_pipe(ready_pipe[1]);
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }
    if (child == 0) {
        close_pipe(ready_pipe[0]);
        const int child_fd = open_probe_file(path);
        if (child_fd < 0 || !set_write_lock(child_fd, 23, 5) ||
            !signal_pipe(ready_pipe[1])) {
            if (child_fd >= 0) {
                static_cast<void>(close(child_fd));
            }
            _exit(1);
        }
        _exit(0);
    }

    close_pipe(ready_pipe[1]);
    const bool ready = wait_for_pipe(ready_pipe[0]);
    const bool child_ok = wait_for_child_success(child);
    const bool released = ready && child_ok && set_write_lock(fd, 23, 5) &&
                          unlock_range(fd, 23, 5);
    const bool ok = ready && child_ok && released;

    static_cast<void>(close(fd));
    cleanup_probe_file(path);
    return ok;
}

bool probe_grow_remap(const std::string &root) {
    const std::string path = path_join(root, "grow-remap.bin");
    const int fd = open_probe_file(path);
    if (fd < 0) {
        return false;
    }
    if (!truncate_file(fd, static_cast<off_t>(k_probe_page_size))) {
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }

    auto *first_mapping = static_cast<std::uint32_t *>(map_file(fd, k_probe_page_size));
    if (first_mapping == nullptr) {
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }
    first_mapping[0] = 0xCAFEBABEU;
    static_cast<void>(msync(first_mapping, sizeof(first_mapping[0]), MS_SYNC));
    static_cast<void>(munmap(first_mapping, k_probe_page_size));

    if (!truncate_file(fd, static_cast<off_t>(k_probe_page_size * 2U))) {
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }
    auto *second_mapping =
        static_cast<std::uint32_t *>(map_file(fd, k_probe_page_size * 2U));
    if (second_mapping == nullptr) {
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }
    const bool ok = second_mapping[0] == 0xCAFEBABEU;
    static_cast<void>(munmap(second_mapping, k_probe_page_size * 2U));
    static_cast<void>(close(fd));
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
    if (!truncate_file(fd, static_cast<off_t>(k_probe_page_size)) || pipe(child_ready) != 0) {
        close_pipe(child_ready[0]);
        close_pipe(child_ready[1]);
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }

    auto *word = static_cast<mylite_ownerless_wait_word *>(map_file(fd, k_probe_page_size));
    if (word == nullptr) {
        close_pipe(child_ready[0]);
        close_pipe(child_ready[1]);
        static_cast<void>(close(fd));
        cleanup_probe_file(path);
        return false;
    }
    mylite_ownerless_wait_store(word, 0U);

    const pid_t child = fork();
    if (child < 0) {
        close_pipe(child_ready[0]);
        close_pipe(child_ready[1]);
        static_cast<void>(munmap(word, k_probe_page_size));
        static_cast<void>(close(fd));
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
            static_cast<void>(close(child_fd));
            _exit(1);
        }
        if (!signal_pipe(child_ready[1])) {
            static_cast<void>(munmap(child_word, k_probe_page_size));
            static_cast<void>(close(child_fd));
            _exit(1);
        }
        const int wait_result = mylite_ownerless_wait_for_change(
            child_word,
            0U,
            static_cast<unsigned>(k_probe_timeout_ms)
        );
        const bool ok = wait_result == MYLITE_OWNERLESS_WAIT_OK &&
                        mylite_ownerless_wait_load(child_word) == 1U;
        static_cast<void>(munmap(child_word, k_probe_page_size));
        static_cast<void>(close(child_fd));
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
    static_cast<void>(munmap(word, k_probe_page_size));
    static_cast<void>(close(fd));
    cleanup_probe_file(path);
    return ok;
}

bool set_write_lock(int fd, off_t start, off_t length) {
    return try_write_lock(fd, start, length) == 0;
}

int try_write_lock(int fd, off_t start, off_t length) {
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;

    if (fcntl(fd, F_SETLK, &lock) == 0) {
        return 0;
    }
    return errno;
}

bool unlock_range(int fd, off_t start, off_t length) {
    struct flock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;

    return fcntl(fd, F_SETLK, &lock) == 0;
}

std::string make_temp_root(void) {
    char template_path[] = "/tmp/mylite-ownerless-probe.XXXXXX";
    char *root = mkdtemp(template_path);

    return root == nullptr ? std::string() : std::string(root);
}

std::string path_join(const std::string &directory, const char *name) {
    return directory + "/" + name;
}

int open_probe_file(const std::string &path) {
    return open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, k_probe_file_mode);
}

bool truncate_file(int fd, off_t size) {
    return ftruncate(fd, size) == 0;
}

void *map_file(int fd, std::size_t size) {
    void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    return mapping == MAP_FAILED ? nullptr : mapping;
}

bool signal_pipe(int pipe_fd) {
    const char value = 'x';
    const ssize_t written = write(pipe_fd, &value, sizeof(value));
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
    const ssize_t read_size = read(pipe_fd, &value, sizeof(value));
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
        static_cast<void>(close(pipe_fd));
    }
}

void cleanup_probe_file(const std::string &path) {
    static_cast<void>(unlink(path.c_str()));
}

void cleanup_probe_root(const std::string &root) {
    static_cast<void>(rmdir(root.c_str()));
}

} // namespace

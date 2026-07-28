#include "ownerless_probe.h"

#include "ownerless_platform_io.h"
#include "ownerless_process_registry.h"
#include "ownerless_wait.h"

#include <windows.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
#  define MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS 0
#endif

namespace {

constexpr std::size_t k_probe_page_size = 4096;

std::filesystem::path make_probe_root(const std::filesystem::path &parent) {
    std::error_code error;
    for (unsigned attempt = 0; attempt < 32U; ++attempt) {
        const auto name = std::string(".mylite-ownerless-probe-") +
                          std::to_string(mylite_ownerless_current_process_id()) + "-" +
                          std::to_string(GetTickCount64()) + "-" + std::to_string(attempt);
        const std::filesystem::path root = parent / name;
        if (std::filesystem::create_directory(root, error)) {
            return root;
        }
        if (error) {
            return {};
        }
    }
    return {};
}

int open_probe_file(const std::filesystem::path &path) {
    return ::open(path.string().c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}

bool probe_mmap_shared_visibility(const std::filesystem::path &file) {
    const int first_fd = open_probe_file(file);
    if (first_fd < 0 || ::ftruncate(first_fd, k_probe_page_size) != 0) {
        if (first_fd >= 0) {
            ::close(first_fd);
        }
        return false;
    }
    const int second_fd = ::open(file.string().c_str(), O_RDWR | O_CLOEXEC);
    if (second_fd < 0) {
        ::close(first_fd);
        return false;
    }
    void *first =
        ::mmap(nullptr, k_probe_page_size, PROT_READ | PROT_WRITE, MAP_SHARED, first_fd, 0);
    void *second =
        ::mmap(nullptr, k_probe_page_size, PROT_READ | PROT_WRITE, MAP_SHARED, second_fd, 0);
    bool ok = first != MAP_FAILED && second != MAP_FAILED;
    if (ok) {
        auto *first_bytes = static_cast<unsigned char *>(first);
        auto *second_bytes = static_cast<unsigned char *>(second);
        first_bytes[0] = 0x5aU;
        ok = second_bytes[0] == 0x5aU;
        second_bytes[1] = 0xa5U;
        ok = ok && first_bytes[1] == 0xa5U;
        ok = ok && ::msync(first, k_probe_page_size, MS_SYNC) == 0;
    }
    if (first != MAP_FAILED) {
        ::munmap(first, k_probe_page_size);
    }
    if (second != MAP_FAILED) {
        ::munmap(second, k_probe_page_size);
    }
    ::close(second_fd);
    ::close(first_fd);
    return ok;
}

bool set_lock(int fd, short type) {
    struct flock lock = {};
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;
    return ::fcntl(fd, F_OFD_SETLK, &lock) == 0;
}

bool probe_byte_range_locks(const std::filesystem::path &file) {
    const int first = open_probe_file(file);
    const int second = ::open(file.string().c_str(), O_RDWR | O_CLOEXEC);
    if (first < 0 || second < 0) {
        if (first >= 0) {
            ::close(first);
        }
        if (second >= 0) {
            ::close(second);
        }
        return false;
    }
    const bool locked = set_lock(first, F_WRLCK);
    errno = 0;
    const bool conflict = !set_lock(second, F_WRLCK) && (errno == EAGAIN || errno == EACCES);
    const bool unlocked = set_lock(first, F_UNLCK);
    const bool acquired_after_unlock = set_lock(second, F_WRLCK);
    if (acquired_after_unlock) {
        set_lock(second, F_UNLCK);
    }
    ::close(second);
    ::close(first);
    return locked && conflict && unlocked && acquired_after_unlock;
}

bool probe_lock_release_on_close(const std::filesystem::path &file) {
    int first = open_probe_file(file);
    const int second = ::open(file.string().c_str(), O_RDWR | O_CLOEXEC);
    if (first < 0 || second < 0) {
        if (first >= 0) {
            ::close(first);
        }
        if (second >= 0) {
            ::close(second);
        }
        return false;
    }
    const bool locked = set_lock(first, F_WRLCK);
    ::close(first);
    first = -1;
    const bool released = set_lock(second, F_WRLCK);
    if (released) {
        set_lock(second, F_UNLCK);
    }
    ::close(second);
    return locked && released;
}

bool probe_lock_close_isolation(const std::filesystem::path &file) {
    const int lock_fd = open_probe_file(file);
    const int unrelated_fd = ::open(file.string().c_str(), O_RDWR | O_CLOEXEC);
    if (lock_fd < 0 || unrelated_fd < 0) {
        if (lock_fd >= 0) {
            ::close(lock_fd);
        }
        if (unrelated_fd >= 0) {
            ::close(unrelated_fd);
        }
        return false;
    }
    const bool locked = set_lock(lock_fd, F_WRLCK);
    ::close(unrelated_fd);
    const int peer_fd = ::open(file.string().c_str(), O_RDWR | O_CLOEXEC);
    errno = 0;
    const bool retained =
        peer_fd >= 0 && !set_lock(peer_fd, F_WRLCK) && (errno == EAGAIN || errno == EACCES);
    set_lock(lock_fd, F_UNLCK);
    if (peer_fd >= 0) {
        ::close(peer_fd);
    }
    ::close(lock_fd);
    return locked && retained;
}

bool probe_grow_remap(const std::filesystem::path &file) {
    const int fd = open_probe_file(file);
    if (fd < 0 || ::ftruncate(fd, k_probe_page_size) != 0) {
        if (fd >= 0) {
            ::close(fd);
        }
        return false;
    }
    void *first = ::mmap(nullptr, k_probe_page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (first == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    static_cast<unsigned char *>(first)[0] = 0x6dU;
    const bool first_sync = ::msync(first, k_probe_page_size, MS_SYNC) == 0;
    ::munmap(first, k_probe_page_size);
    const bool grew = ::ftruncate(fd, k_probe_page_size * 2U) == 0;
    void *second =
        ::mmap(nullptr, k_probe_page_size * 2U, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    const bool remapped = second != MAP_FAILED && static_cast<unsigned char *>(second)[0] == 0x6dU;
    if (second != MAP_FAILED) {
        ::munmap(second, k_probe_page_size * 2U);
    }
    ::close(fd);
    return first_sync && grew && remapped;
}

bool probe_process_identity(void) {
    mylite_ownerless_process_identity identity = {};
    return mylite_ownerless_current_process_identity(&identity) ==
               MYLITE_OWNERLESS_PROCESS_REGISTRY_OK &&
           identity.pid == mylite_ownerless_current_process_id() && identity.start_time != 0U &&
           identity.boot_id_hash != 0U &&
           mylite_ownerless_process_identity_is_alive(&identity, nullptr) != 0;
}

bool probe_wait_backend(void) {
    mylite_ownerless_wait_word word = {};
    mylite_ownerless_wait_store(&word, 1U);
    std::thread writer([&word]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        mylite_ownerless_wait_store(&word, 2U);
        static_cast<void>(mylite_ownerless_wait_wake(&word));
    });
    const int wait_result = mylite_ownerless_wait_for_change(&word, 1U, 1000U);
    writer.join();
    return wait_result == MYLITE_OWNERLESS_WAIT_OK && mylite_ownerless_wait_load(&word) == 2U;
}

void apply_test_failures(mylite_ownerless_probe_result &result) {
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
#else
    (void)result;
#endif
}

int run_probe(const std::filesystem::path &root, mylite_ownerless_probe_result *result) {
    if (result == nullptr) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    *result = {};
    result->size = sizeof(*result);
    result->mmap_shared_visibility = probe_mmap_shared_visibility(root / "mmap.probe") ? 1U : 0U;
    result->byte_range_locks = probe_byte_range_locks(root / "locks.probe") ? 1U : 0U;
    result->lock_release_on_exit = probe_lock_release_on_close(root / "release.probe") ? 1U : 0U;
    result->lock_close_isolation = probe_lock_close_isolation(root / "isolation.probe") ? 1U : 0U;
    result->grow_remap = probe_grow_remap(root / "grow.probe") ? 1U : 0U;
    result->wait_backend = probe_wait_backend() ? 1U : 0U;
    result->fast_wait_backend = mylite_ownerless_wait_backend_is_fast() != 0 ? 1U : 0U;
    result->process_identity = probe_process_identity() ? 1U : 0U;
    apply_test_failures(*result);
    result->required_primitives =
        result->mmap_shared_visibility != 0U && result->byte_range_locks != 0U &&
                result->lock_release_on_exit != 0U && result->lock_close_isolation != 0U &&
                result->grow_remap != 0U && result->wait_backend != 0U &&
                result->process_identity != 0U
            ? 1U
            : 0U;
    result->platform_candidate =
        result->required_primitives != 0U && result->fast_wait_backend != 0U ? 1U : 0U;
    return MYLITE_OWNERLESS_PROBE_OK;
}

int probe_under(const std::filesystem::path &parent, mylite_ownerless_probe_result *result) {
    const std::filesystem::path root = make_probe_root(parent);
    if (root.empty()) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    const int probe_result = run_probe(root, result);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return probe_result;
}

std::uint64_t hash_volume_root(const wchar_t *root, DWORD serial) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const wchar_t *character = root; *character != 0; ++character) {
        hash ^= static_cast<std::uint16_t>(*character);
        hash *= 1099511628211ULL;
    }
    hash ^= serial;
    hash *= 1099511628211ULL;
    return hash == 0U ? 1U : hash;
}

} // namespace

int mylite_ownerless_probe_platform(mylite_ownerless_probe_result *result) {
    std::error_code error;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
    return error ? MYLITE_OWNERLESS_PROBE_ERROR : probe_under(temp, result);
}

int mylite_ownerless_probe_directory(const char *directory, mylite_ownerless_probe_result *result) {
    if (directory == nullptr || directory[0] == '\0') {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    return probe_under(directory, result);
}

int mylite_ownerless_probe_filesystem(
    const char *directory,
    mylite_ownerless_filesystem_info *out_info
) {
    if (directory == nullptr || directory[0] == '\0' || out_info == nullptr) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    *out_info = {};
    out_info->size = sizeof(*out_info);

    const int directory_length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, directory, -1, nullptr, 0);
    if (directory_length <= 0) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    std::vector<wchar_t> wide_directory(static_cast<std::size_t>(directory_length));
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            directory,
            -1,
            wide_directory.data(),
            directory_length
        ) != directory_length) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    std::array<wchar_t, 32768> full_path = {};
    std::array<wchar_t, 32768> volume_root = {};
    const DWORD full_size = GetFullPathNameW(
        wide_directory.data(),
        static_cast<DWORD>(full_path.size()),
        full_path.data(),
        nullptr
    );
    if (full_size == 0U || full_size >= full_path.size() ||
        GetVolumePathNameW(
            full_path.data(),
            volume_root.data(),
            static_cast<DWORD>(volume_root.size())
        ) == FALSE) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }

    const UINT drive_type = GetDriveTypeW(volume_root.data());
    out_info->is_local =
        drive_type != DRIVE_REMOTE && drive_type != DRIVE_NO_ROOT_DIR && drive_type != DRIVE_UNKNOWN
            ? 1U
            : 0U;

    std::array<wchar_t, MYLITE_OWNERLESS_FILESYSTEM_NAME_SIZE> filesystem_name = {};
    DWORD serial = 0;
    if (GetVolumeInformationW(
            volume_root.data(),
            nullptr,
            0,
            &serial,
            nullptr,
            nullptr,
            filesystem_name.data(),
            static_cast<DWORD>(filesystem_name.size())
        ) == FALSE) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            filesystem_name.data(),
            -1,
            out_info->name,
            static_cast<int>(sizeof(out_info->name)),
            nullptr,
            nullptr
        ) <= 0) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    out_info->volume_identity = hash_volume_root(volume_root.data(), serial);
    if (_wcsicmp(filesystem_name.data(), L"NTFS") == 0) {
        out_info->kind = MYLITE_OWNERLESS_FILESYSTEM_NTFS;
        out_info->is_admitted = out_info->is_local;
    }
    return MYLITE_OWNERLESS_PROBE_OK;
}

int mylite_ownerless_probe_filesystem_type(const char *directory, uint64_t *out_type) {
    if (out_type == nullptr) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    mylite_ownerless_filesystem_info info = {};
    if (mylite_ownerless_probe_filesystem(directory, &info) != MYLITE_OWNERLESS_PROBE_OK) {
        return MYLITE_OWNERLESS_PROBE_ERROR;
    }
    *out_type = info.kind;
    return MYLITE_OWNERLESS_PROBE_OK;
}

int mylite_ownerless_filesystem_type_is_validated_local(uint64_t filesystem_type) {
    return filesystem_type == MYLITE_OWNERLESS_FILESYSTEM_NTFS ? 1 : 0;
}

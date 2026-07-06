#include "ownerless_process_registry.h"

#include "ownerless_latch.h"
#include "ownerless_wait.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

namespace {

constexpr std::size_t k_header_slot_count_offset = 0;
constexpr std::size_t k_header_slot_size_offset = 4;
constexpr std::size_t k_header_generation_offset = 8;
constexpr std::size_t k_header_active_count_offset = 16;
constexpr std::size_t k_header_latch_offset = 24;
constexpr std::size_t k_slot_generation_offset = 0;
constexpr std::size_t k_slot_state_offset = 8;
constexpr std::size_t k_slot_open_mode_offset = 12;
constexpr std::size_t k_slot_pid_offset = 16;
constexpr std::size_t k_slot_heartbeat_offset = 24;
constexpr std::size_t k_slot_shm_generation_offset = 32;
constexpr std::size_t k_slot_start_time_offset = 40;
constexpr std::size_t k_slot_boot_id_hash_offset = 48;
constexpr std::uint32_t k_bootstrap_latch_owner_id = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t k_fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t k_fnv_prime = 1099511628211ULL;

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
int acquire_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
);
void release_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
);
int allocate_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    mylite_ownerless_process_identity identity,
    std::uint32_t open_mode,
    std::uint64_t shm_generation,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
);
int release_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
);
int heartbeat_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t heartbeat
);
int cleanup_dead_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *alive_ctx,
    mylite_ownerless_process_cleanup_callback cleanup,
    void *cleanup_ctx,
    std::uint32_t *out_cleaned_slots
);
std::uint64_t live_count_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *ctx
);
mylite_ownerless_process_identity slot_identity(const unsigned char *slot);
bool read_process_start_time(std::uint64_t pid, std::uint64_t *out_start_time);
bool read_current_boot_id_hash(std::uint64_t *out_boot_id_hash);
bool process_is_zombie(std::uint64_t pid);
std::uint64_t hash_bytes(const char *bytes, std::size_t size);
void clear_slot_locked(unsigned char *registry, unsigned char *slot);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
bool registry_size_fits(std::uint32_t slot_count);
bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size);
std::uint32_t slot_count(const unsigned char *registry);
unsigned char *slot_at(unsigned char *registry, std::uint32_t index);
mylite_ownerless_latch *registry_latch(unsigned char *registry);
std::uint32_t load32(const unsigned char *base, std::size_t offset);
std::uint64_t load64(const unsigned char *base, std::size_t offset);
void store32(unsigned char *base, std::size_t offset, std::uint32_t value);
void store64(unsigned char *base, std::size_t offset, std::uint64_t value);

} // namespace

std::size_t mylite_ownerless_process_registry_size(std::uint32_t slot_count) {
    if (!registry_size_fits(slot_count)) {
        return 0U;
    }
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(slot_count) * MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE);
}

int mylite_ownerless_process_identity_for_pid(
    std::uint64_t pid,
    mylite_ownerless_process_identity *out_identity
) {
    if (out_identity == nullptr || pid == 0U ||
        pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    std::uint64_t start_time = 0;
    std::uint64_t boot_id_hash = 0;
    if (!read_process_start_time(pid, &start_time) || !read_current_boot_id_hash(&boot_id_hash) ||
        start_time == 0U || boot_id_hash == 0U) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    out_identity->pid = pid;
    out_identity->start_time = start_time;
    out_identity->boot_id_hash = boot_id_hash;
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
}

int mylite_ownerless_current_process_identity(mylite_ownerless_process_identity *out_identity) {
    return mylite_ownerless_process_identity_for_pid(
        static_cast<std::uint64_t>(::getpid()),
        out_identity
    );
}

int mylite_ownerless_process_identity_is_alive(
    const mylite_ownerless_process_identity *identity,
    void *ctx
) {
    (void)ctx;
    if (identity == nullptr || identity->pid == 0U || identity->start_time == 0U ||
        identity->boot_id_hash == 0U ||
        identity->pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return 0;
    }

    std::uint64_t current_boot_id_hash = 0;
    if (read_current_boot_id_hash(&current_boot_id_hash) &&
        current_boot_id_hash != identity->boot_id_hash) {
        return 0;
    }

    std::uint64_t current_start_time = 0;
    if (read_process_start_time(identity->pid, &current_start_time)) {
        return current_start_time == identity->start_time && !process_is_zombie(identity->pid);
    }

    const pid_t process_id = static_cast<pid_t>(identity->pid);
    if (::kill(process_id, 0) == 0) {
        return 1;
    }
    return errno == EPERM ? 1 : 0;
}

int mylite_ownerless_process_registry_initialize(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_count
) {
    const std::size_t registry_size = mylite_ownerless_process_registry_size(slot_count);
    if (mapping == nullptr || registry_size == 0U || mapping_size < registry_size) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    std::memset(registry, 0, registry_size);
    store32(registry, k_header_slot_count_offset, slot_count);
    store32(registry, k_header_slot_size_offset, MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE);
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
}

int mylite_ownerless_process_registry_allocate(
    void *mapping,
    std::size_t mapping_size,
    mylite_ownerless_process_identity identity,
    std::uint32_t open_mode,
    std::uint64_t shm_generation,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || identity.pid == 0U ||
        identity.start_time == 0U || identity.boot_id_hash == 0U || open_mode == 0U ||
        out_slot_index == nullptr || out_slot_generation == nullptr) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        k_bootstrap_latch_owner_id,
        identity.start_time,
        wait_deadline(5000U)
    );
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    const int allocate_result = allocate_locked(
        registry,
        mapping_size,
        identity,
        open_mode,
        shm_generation,
        out_slot_index,
        out_slot_generation
    );
    release_registry_latch(registry, k_bootstrap_latch_owner_id, identity.start_time);
    return allocate_result;
}

int mylite_ownerless_process_registry_release(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || slot_generation == 0U) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const std::uint32_t owner_id = slot_index + 1U;
    const int latch_result =
        acquire_registry_latch(registry, owner_id, slot_generation, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    const int release_result = release_locked(registry, mapping_size, slot_index, slot_generation);
    release_registry_latch(registry, owner_id, slot_generation);
    return release_result;
}

int mylite_ownerless_process_registry_heartbeat(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t heartbeat
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || slot_generation == 0U ||
        heartbeat == 0U) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const std::uint32_t owner_id = slot_index + 1U;
    const int latch_result =
        acquire_registry_latch(registry, owner_id, slot_generation, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    const int heartbeat_result =
        heartbeat_locked(registry, mapping_size, slot_index, slot_generation, heartbeat);
    release_registry_latch(registry, owner_id, slot_generation);
    return heartbeat_result;
}

int mylite_ownerless_process_registry_cleanup_dead(
    void *mapping,
    std::size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *ctx,
    std::uint32_t *out_cleaned_slots
) {
    return mylite_ownerless_process_registry_cleanup_dead_with_callback(
        mapping,
        mapping_size,
        is_alive,
        ctx,
        nullptr,
        nullptr,
        out_cleaned_slots
    );
}

int mylite_ownerless_process_registry_cleanup_dead_with_callback(
    void *mapping,
    std::size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *alive_ctx,
    mylite_ownerless_process_cleanup_callback cleanup,
    void *cleanup_ctx,
    std::uint32_t *out_cleaned_slots
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || is_alive == nullptr ||
        out_cleaned_slots == nullptr) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result =
        acquire_registry_latch(registry, k_bootstrap_latch_owner_id, 1U, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    const int cleanup_result = cleanup_dead_locked(
        registry,
        mapping_size,
        is_alive,
        alive_ctx,
        cleanup,
        cleanup_ctx,
        out_cleaned_slots
    );
    release_registry_latch(registry, k_bootstrap_latch_owner_id, 1U);
    return cleanup_result;
}

std::uint64_t mylite_ownerless_process_registry_active_count(const void *mapping) {
    if (mapping == nullptr) {
        return 0U;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_active_count_offset);
}

std::uint64_t mylite_ownerless_process_registry_generation(const void *mapping) {
    if (mapping == nullptr) {
        return 0U;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    return load64(registry, k_header_generation_offset);
}

int mylite_ownerless_process_registry_live_count(
    void *mapping,
    std::size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *ctx,
    std::uint64_t *out_live_count
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || is_alive == nullptr ||
        out_live_count == nullptr) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result =
        acquire_registry_latch(registry, k_bootstrap_latch_owner_id, 1U, wait_deadline(5000U));
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    *out_live_count = live_count_locked(registry, mapping_size, is_alive, ctx);
    release_registry_latch(registry, k_bootstrap_latch_owner_id, 1U);
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
}

namespace {

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

int acquire_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
) {
    const int latch_result = mylite_ownerless_latch_acquire(
        registry_latch(registry),
        owner_id,
        owner_generation,
        nullptr,
        nullptr,
        remaining_timeout_ms(deadline)
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
    }
    return latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT
               ? MYLITE_OWNERLESS_PROCESS_REGISTRY_TIMEOUT
               : MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
}

void release_registry_latch(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    static_cast<void>(
        mylite_ownerless_latch_release(registry_latch(registry), owner_id, owner_generation)
    );
}

int allocate_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    mylite_ownerless_process_identity identity,
    std::uint32_t open_mode,
    std::uint64_t shm_generation,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation
) {
    const std::uint32_t count = slot_count(registry);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != 0U) {
            continue;
        }

        const std::uint64_t generation = load64(registry, k_header_generation_offset) + 1U;
        store64(slot, k_slot_generation_offset, generation);
        store32(slot, k_slot_open_mode_offset, open_mode);
        store64(slot, k_slot_pid_offset, identity.pid);
        store64(slot, k_slot_heartbeat_offset, generation);
        store64(slot, k_slot_shm_generation_offset, shm_generation);
        store64(slot, k_slot_start_time_offset, identity.start_time);
        store64(slot, k_slot_boot_id_hash_offset, identity.boot_id_hash);
        store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE);
        store64(registry, k_header_generation_offset, generation);
        store64(
            registry,
            k_header_active_count_offset,
            load64(registry, k_header_active_count_offset) + 1U
        );
        *out_slot_index = index;
        *out_slot_generation = generation;
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
    }
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_FULL;
}

int release_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (slot_index >= slot_count(registry)) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND;
    }
    unsigned char *slot = slot_at(registry, slot_index);
    if (static_cast<std::size_t>(slot + MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE - registry) >
            mapping_size ||
        load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE ||
        load64(slot, k_slot_generation_offset) != slot_generation) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND;
    }

    clear_slot_locked(registry, slot);
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
}

int heartbeat_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t heartbeat
) {
    if (slot_index >= slot_count(registry)) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND;
    }
    unsigned char *slot = slot_at(registry, slot_index);
    if (static_cast<std::size_t>(slot + MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE - registry) >
            mapping_size ||
        load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE ||
        load64(slot, k_slot_generation_offset) != slot_generation) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND;
    }

    store64(slot, k_slot_heartbeat_offset, heartbeat);
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
}

int cleanup_dead_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *alive_ctx,
    mylite_ownerless_process_cleanup_callback cleanup,
    void *cleanup_ctx,
    std::uint32_t *out_cleaned_slots
) {
    std::uint32_t cleaned_slots = 0U;
    const std::uint32_t count = slot_count(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) != MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE) {
            continue;
        }
        const mylite_ownerless_process_identity identity = slot_identity(slot);
        if (is_alive(&identity, alive_ctx) != 0) {
            continue;
        }
        if (cleanup != nullptr) {
            const int cleanup_result =
                cleanup(index, load64(slot, k_slot_generation_offset), &identity, cleanup_ctx);
            if (cleanup_result == MYLITE_OWNERLESS_PROCESS_CLEANUP_BLOCKED) {
                *out_cleaned_slots = cleaned_slots;
                return MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY;
            }
            if (cleanup_result != MYLITE_OWNERLESS_PROCESS_CLEANUP_OK) {
                return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
            }
        }
        clear_slot_locked(registry, slot);
        ++cleaned_slots;
    }

    *out_cleaned_slots = cleaned_slots;
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
}

std::uint64_t live_count_locked(
    unsigned char *registry,
    std::size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *ctx
) {
    std::uint64_t live_count = 0U;
    const std::uint32_t count = slot_count(registry);

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        const mylite_ownerless_process_identity identity = slot_identity(slot);
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE &&
            is_alive(&identity, ctx) != 0) {
            ++live_count;
        }
    }
    return live_count;
}

mylite_ownerless_process_identity slot_identity(const unsigned char *slot) {
    mylite_ownerless_process_identity identity = {};
    identity.pid = load64(slot, k_slot_pid_offset);
    identity.start_time = load64(slot, k_slot_start_time_offset);
    identity.boot_id_hash = load64(slot, k_slot_boot_id_hash_offset);
    return identity;
}

bool read_process_start_time(std::uint64_t pid, std::uint64_t *out_start_time) {
    if (out_start_time == nullptr || pid == 0U ||
        pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return false;
    }

#if defined(__linux__)
    char stat_path[64];
    char stat_buffer[512];
    const int path_length = std::snprintf(
        stat_path,
        sizeof(stat_path),
        "/proc/%llu/stat",
        static_cast<unsigned long long>(pid)
    );
    if (path_length <= 0 || static_cast<std::size_t>(path_length) >= sizeof(stat_path)) {
        return false;
    }

    const int fd = ::open(stat_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const ssize_t bytes_read = ::read(fd, stat_buffer, sizeof(stat_buffer) - 1U);
    const int saved_errno = errno;
    static_cast<void>(::close(fd));
    errno = saved_errno;
    if (bytes_read <= 0) {
        return false;
    }
    stat_buffer[bytes_read] = '\0';

    const char *cursor = std::strrchr(stat_buffer, ')');
    if (cursor == nullptr || cursor[1] != ' ') {
        return false;
    }
    cursor += 2;
    for (unsigned field = 3U; field <= 22U; ++field) {
        while (*cursor == ' ') {
            ++cursor;
        }
        const char *field_start = cursor;
        while (*cursor != '\0' && *cursor != ' ') {
            ++cursor;
        }
        if (field == 22U) {
            char *end = nullptr;
            errno = 0;
            const unsigned long long value = std::strtoull(field_start, &end, 10);
            if (errno != 0 || end != cursor || value == 0ULL) {
                return false;
            }
            *out_start_time = static_cast<std::uint64_t>(value);
            return true;
        }
        if (*cursor == '\0') {
            return false;
        }
    }
#else
    (void)pid;
#endif

    return false;
}

bool read_current_boot_id_hash(std::uint64_t *out_boot_id_hash) {
    if (out_boot_id_hash == nullptr) {
        return false;
    }

#if defined(__linux__)
    const int fd = ::open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    char buffer[128];
    const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer));
    const int saved_errno = errno;
    static_cast<void>(::close(fd));
    errno = saved_errno;
    if (bytes_read <= 0) {
        return false;
    }

    *out_boot_id_hash = hash_bytes(buffer, static_cast<std::size_t>(bytes_read));
    return *out_boot_id_hash != 0U;
#else
    return false;
#endif
}

bool process_is_zombie(std::uint64_t pid) {
    if (pid == 0U || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return false;
    }

#if defined(__linux__)
    char stat_path[64];
    char stat_buffer[512];
    const int path_length = std::snprintf(
        stat_path,
        sizeof(stat_path),
        "/proc/%llu/stat",
        static_cast<unsigned long long>(pid)
    );
    if (path_length <= 0 || static_cast<std::size_t>(path_length) >= sizeof(stat_path)) {
        return false;
    }

    const int fd = ::open(stat_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const ssize_t bytes_read = ::read(fd, stat_buffer, sizeof(stat_buffer) - 1U);
    const int saved_errno = errno;
    static_cast<void>(::close(fd));
    errno = saved_errno;
    if (bytes_read <= 0) {
        return false;
    }
    stat_buffer[bytes_read] = '\0';

    const char *close_paren = std::strrchr(stat_buffer, ')');
    return close_paren != nullptr && close_paren[1] == ' ' && close_paren[2] == 'Z';
#else
    (void)pid;
    return false;
#endif
}

std::uint64_t hash_bytes(const char *bytes, std::size_t size) {
    std::uint64_t hash = k_fnv_offset_basis;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<unsigned char>(bytes[index]);
        hash *= k_fnv_prime;
    }
    return hash == 0U ? k_fnv_offset_basis : hash;
}

void clear_slot_locked(unsigned char *registry, unsigned char *slot) {
    const std::uint64_t generation = load64(registry, k_header_generation_offset) + 1U;
    std::memset(slot, 0, MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE);
    store64(slot, k_slot_generation_offset, generation);
    store64(registry, k_header_generation_offset, generation);
    store64(
        registry,
        k_header_active_count_offset,
        load64(registry, k_header_active_count_offset) - 1U
    );
}

unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<unsigned>(std::max<std::chrono::milliseconds::rep>(remaining.count(), 1));
}

bool registry_size_fits(std::uint32_t slot_count) {
    const std::size_t max_slots =
        (std::numeric_limits<std::size_t>::max() - MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE) /
        MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE;
    return static_cast<std::size_t>(slot_count) <= max_slots;
}

bool mapping_can_hold_registry(const void *mapping, std::size_t mapping_size) {
    if (mapping == nullptr || mapping_size < MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE) {
        return false;
    }
    const auto *registry = static_cast<const unsigned char *>(mapping);
    const std::uint32_t count = slot_count(registry);
    const std::size_t registry_size = mylite_ownerless_process_registry_size(count);
    return count > 0U &&
           load32(registry, k_header_slot_size_offset) ==
               MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE &&
           registry_size > 0U && mapping_size >= registry_size;
}

std::uint32_t slot_count(const unsigned char *registry) {
    return load32(registry, k_header_slot_count_offset);
}

unsigned char *slot_at(unsigned char *registry, std::uint32_t index) {
    return registry + MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE +
           (static_cast<std::size_t>(index) * MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE);
}

mylite_ownerless_latch *registry_latch(unsigned char *registry) {
    return reinterpret_cast<mylite_ownerless_latch *>(registry + k_header_latch_offset);
}

std::uint32_t load32(const unsigned char *base, std::size_t offset) {
    const auto *value = reinterpret_cast<const std::uint32_t *>(base + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

std::uint64_t load64(const unsigned char *base, std::size_t offset) {
    const auto *value = reinterpret_cast<const std::uint64_t *>(base + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

void store32(unsigned char *base, std::size_t offset, std::uint32_t value) {
    auto *target = reinterpret_cast<std::uint32_t *>(base + offset);
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

void store64(unsigned char *base, std::size_t offset, std::uint64_t value) {
    auto *target = reinterpret_cast<std::uint64_t *>(base + offset);
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

} // namespace

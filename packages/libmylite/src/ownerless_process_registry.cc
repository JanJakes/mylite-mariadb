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

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <libproc.h>
#  include <signal.h>
#  include <sys/proc.h>
#  include <sys/sysctl.h>
#  include <sys/time.h>
#  include <unistd.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <unistd.h>
#endif

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
constexpr std::uint32_t k_bootstrap_latch_owner_flag = 0x80000000U;
constexpr std::uint32_t k_bootstrap_latch_owner_pid_mask = 0x7fffffffU;
#if defined(_WIN32)
constexpr std::uint64_t k_windows_process_identity_epoch = 0x57494e46494c4554ULL;
#endif
#if defined(__linux__) || defined(__APPLE__)
constexpr std::uint64_t k_fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t k_fnv_prime = 1099511628211ULL;
#endif

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
int acquire_registry_latch(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
);
bool repair_registry_locked(unsigned char *registry, std::size_t mapping_size);
bool generation_can_allocate(const unsigned char *registry);
bool open_mode_is_valid(std::uint32_t open_mode);
bool open_modes_are_compatible(std::uint32_t requested_mode, std::uint32_t active_mode);
std::uint32_t bootstrap_latch_owner_id(const mylite_ownerless_process_identity &identity);
int finish_registry_operation(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
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
#if defined(__linux__) || defined(__APPLE__)
std::uint64_t hash_bytes(const char *bytes, std::size_t size);
#endif
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
    if (
        out_identity == nullptr || pid == 0U
#if defined(_WIN32)
        || pid > MAXDWORD
#else
        || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())
#endif
    ) {
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
#if defined(_WIN32)
    const std::uint64_t pid = GetCurrentProcessId();
#else
    const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
#endif
    return mylite_ownerless_process_identity_for_pid(pid, out_identity);
}

int mylite_ownerless_process_identity_is_alive(
    const mylite_ownerless_process_identity *identity,
    void *ctx
) {
    (void)ctx;
    if (identity == nullptr || identity->pid == 0U || identity->start_time == 0U ||
        identity->boot_id_hash == 0U ||
#if defined(_WIN32)
        identity->pid > MAXDWORD) {
#else
        identity->pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
#endif
        return 0;
    }

    std::uint64_t current_boot_id_hash = 0;
    if (read_current_boot_id_hash(&current_boot_id_hash) &&
        current_boot_id_hash != identity->boot_id_hash) {
        return 0;
    }

    std::uint64_t current_start_time = 0;
    if (read_process_start_time(identity->pid, &current_start_time)) {
        if (current_start_time == identity->start_time && !process_is_zombie(identity->pid)) {
            return 1;
        }
        return 0;
    }

#if defined(_WIN32)
    return 0;
#else
    const pid_t process_id = static_cast<pid_t>(identity->pid);
    if (::kill(process_id, 0) == 0) {
        return 1;
    }
    return errno == EPERM ? 1 : 0;
#endif
}

int mylite_ownerless_process_registry_latch_owner_is_alive(
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    void *ctx
) {
    if (ctx == nullptr || owner_id == 0U) {
        return -1;
    }
    const auto *liveness =
        static_cast<const mylite_ownerless_process_registry_liveness_context *>(ctx);
    if (!mapping_can_hold_registry(liveness->mapping, liveness->mapping_size)) {
        return -1;
    }
    const auto is_alive = liveness->is_alive != nullptr
                              ? liveness->is_alive
                              : mylite_ownerless_process_identity_is_alive;

    if ((owner_id & k_bootstrap_latch_owner_flag) != 0U) {
        const std::uint64_t pid = owner_id & k_bootstrap_latch_owner_pid_mask;
        mylite_ownerless_process_identity identity = {};
        if (mylite_ownerless_process_identity_for_pid(pid, &identity) !=
            MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
            return 0;
        }
        /*
         * A mismatch can be PID reuse or the ACQUIRING publication gap. Only
         * current PID identity liveness proves whether this owner is dead.
         */
        return is_alive(&identity, liveness->is_alive_ctx) != 0 ? 1 : 0;
    }

    const auto *registry = static_cast<const unsigned char *>(liveness->mapping);
    const std::uint32_t count = slot_count(registry);
    if (owner_id > count) {
        /* Legacy bootstrap owner IDs cannot be tied to a process safely. */
        return -1;
    }
    const unsigned char *slot = slot_at(const_cast<unsigned char *>(registry), owner_id - 1U);
    const std::uint32_t state = load32(slot, k_slot_state_offset);
    if (state != MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE) {
        return 0;
    }
    const std::uint64_t slot_generation = load64(slot, k_slot_generation_offset);
    const mylite_ownerless_process_identity identity = slot_identity(slot);
    if (load32(slot, k_slot_state_offset) != state ||
        load64(slot, k_slot_generation_offset) != slot_generation) {
        return 1;
    }
    const int alive = is_alive(&identity, liveness->is_alive_ctx) != 0 ? 1 : 0;
    if (owner_generation == 0U || owner_generation == slot_generation) {
        return alive;
    }
    /* A live reused slot is ambiguous; fail closed until coordinated cleanup. */
    return alive;
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
    if (out_slot_index == nullptr || out_slot_generation == nullptr) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }
    *out_slot_index = 0U;
    *out_slot_generation = 0U;
    if (!mapping_can_hold_registry(mapping, mapping_size) || identity.pid == 0U ||
        identity.start_time == 0U || identity.boot_id_hash == 0U ||
        !open_mode_is_valid(open_mode)) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

    auto *registry = static_cast<unsigned char *>(mapping);
    const std::uint32_t bootstrap_owner_id = bootstrap_latch_owner_id(identity);
    if (bootstrap_owner_id == 0U) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }
    const int latch_result = acquire_registry_latch(
        registry,
        mapping_size,
        bootstrap_owner_id,
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
    return finish_registry_operation(
        registry,
        bootstrap_owner_id,
        identity.start_time,
        allocate_result,
        allocate_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
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
    mylite_ownerless_process_identity identity = {};
    if (mylite_ownerless_current_process_identity(&identity) !=
        MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }
    const std::uint32_t owner_id = bootstrap_latch_owner_id(identity);
    const int latch_result = acquire_registry_latch(
        registry,
        mapping_size,
        owner_id,
        identity.start_time,
        wait_deadline(5000U)
    );
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    const int release_result = release_locked(registry, mapping_size, slot_index, slot_generation);
    return finish_registry_operation(
        registry,
        owner_id,
        identity.start_time,
        release_result,
        release_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
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
    const int latch_result = acquire_registry_latch(
        registry,
        mapping_size,
        owner_id,
        slot_generation,
        wait_deadline(5000U)
    );
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    const int heartbeat_result =
        heartbeat_locked(registry, mapping_size, slot_index, slot_generation, heartbeat);
    return finish_registry_operation(
        registry,
        owner_id,
        slot_generation,
        heartbeat_result,
        heartbeat_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
    );
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
    *out_cleaned_slots = 0U;

    auto *registry = static_cast<unsigned char *>(mapping);
    mylite_ownerless_process_identity identity = {};
    if (mylite_ownerless_current_process_identity(&identity) !=
        MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }
    const std::uint32_t owner_id = bootstrap_latch_owner_id(identity);
    const int latch_result = acquire_registry_latch(
        registry,
        mapping_size,
        owner_id,
        identity.start_time,
        wait_deadline(5000U)
    );
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
    return finish_registry_operation(
        registry,
        owner_id,
        identity.start_time,
        cleanup_result,
        *out_cleaned_slots != 0U
    );
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
    *out_live_count = 0U;

    auto *registry = static_cast<unsigned char *>(mapping);
    mylite_ownerless_process_identity identity = {};
    if (mylite_ownerless_current_process_identity(&identity) !=
        MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }
    const std::uint32_t owner_id = bootstrap_latch_owner_id(identity);
    const int latch_result = acquire_registry_latch(
        registry,
        mapping_size,
        owner_id,
        identity.start_time,
        wait_deadline(5000U)
    );
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    *out_live_count = live_count_locked(registry, mapping_size, is_alive, ctx);
    return finish_registry_operation(
        registry,
        owner_id,
        identity.start_time,
        MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_process_registry_finish_bootstrap_pending_release(
    void *mapping,
    std::size_t mapping_size,
    mylite_ownerless_process_identity identity
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) || identity.pid == 0U ||
        identity.start_time == 0U || identity.boot_id_hash == 0U) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }
    const std::uint32_t owner_id = bootstrap_latch_owner_id(identity);
    if (owner_id == 0U) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }
    auto *registry = static_cast<unsigned char *>(mapping);
    const int latch_result = acquire_registry_latch(
        registry,
        mapping_size,
        owner_id,
        identity.start_time,
        wait_deadline(5000U)
    );
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    return finish_registry_operation(
        registry,
        owner_id,
        identity.start_time,
        MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        false
    );
}

int mylite_ownerless_process_registry_finish_slot_pending_release(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t slot_index,
    std::uint64_t slot_generation
) {
    if (!mapping_can_hold_registry(mapping, mapping_size) ||
        slot_index >= slot_count(static_cast<const unsigned char *>(mapping)) ||
        slot_generation == 0U) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }
    auto *registry = static_cast<unsigned char *>(mapping);
    const std::uint32_t owner_id = slot_index + 1U;
    const int latch_result = acquire_registry_latch(
        registry,
        mapping_size,
        owner_id,
        slot_generation,
        wait_deadline(5000U)
    );
    if (latch_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return latch_result;
    }
    return finish_registry_operation(
        registry,
        owner_id,
        slot_generation,
        MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        false
    );
}

namespace {

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

int acquire_registry_latch(
    unsigned char *registry,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::chrono::steady_clock::time_point deadline
) {
    mylite_ownerless_process_registry_liveness_context liveness = {
        registry,
        mapping_size,
        mylite_ownerless_process_identity_is_alive,
        nullptr,
    };
    mylite_ownerless_latch_dead_owner dead_owner = {};
    const int latch_result = mylite_ownerless_latch_acquire_recoverable(
        registry_latch(registry),
        owner_id,
        owner_generation,
        mylite_ownerless_process_registry_latch_owner_is_alive,
        &liveness,
        remaining_timeout_ms(deadline),
        &dead_owner
    );
    if (latch_result == MYLITE_OWNERLESS_LATCH_OK) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
    }
    if (latch_result == MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED) {
        if (!repair_registry_locked(registry, mapping_size)) {
            static_cast<void>(mylite_ownerless_latch_mark_not_recoverable(
                registry_latch(registry),
                owner_id,
                owner_generation
            ));
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_OWNER_DEAD;
        }
        if (mylite_ownerless_latch_mark_consistent(
                registry_latch(registry),
                owner_id,
                owner_generation
            ) != MYLITE_OWNERLESS_LATCH_OK) {
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
        }
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_OK;
    }
    if (latch_result == MYLITE_OWNERLESS_LATCH_OWNER_DEAD ||
        latch_result == MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_OWNER_DEAD;
    }
    return latch_result == MYLITE_OWNERLESS_LATCH_TIMEOUT
               ? MYLITE_OWNERLESS_PROCESS_REGISTRY_TIMEOUT
               : MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
}

bool repair_registry_locked(unsigned char *registry, std::size_t mapping_size) {
    const std::uint32_t count = slot_count(registry);
    std::uint64_t active_count = 0U;
    std::uint64_t generation = load64(registry, k_header_generation_offset);
    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            return false;
        }
        const std::uint32_t state = load32(slot, k_slot_state_offset);
        const std::uint64_t slot_generation = load64(slot, k_slot_generation_offset);
        generation = std::max(generation, slot_generation);
        if (state == 0U) {
            continue;
        }
        const mylite_ownerless_process_identity identity = slot_identity(slot);
        if (state != MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE || slot_generation == 0U ||
            !open_mode_is_valid(load32(slot, k_slot_open_mode_offset)) || identity.pid == 0U ||
            identity.start_time == 0U || identity.boot_id_hash == 0U) {
            return false;
        }
        for (std::uint32_t prior = 0; prior < index; ++prior) {
            const unsigned char *prior_slot = slot_at(registry, prior);
            if (load32(prior_slot, k_slot_state_offset) == MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE &&
                load64(prior_slot, k_slot_generation_offset) == slot_generation) {
                return false;
            }
        }
        ++active_count;
    }
    if (active_count > std::numeric_limits<std::uint64_t>::max() - 1U ||
        generation > std::numeric_limits<std::uint64_t>::max() - active_count - 1U) {
        return false;
    }
    store64(registry, k_header_generation_offset, generation + 1U);
    store64(registry, k_header_active_count_offset, active_count);
    return true;
}

std::uint32_t bootstrap_latch_owner_id(const mylite_ownerless_process_identity &identity) {
    if (identity.pid == 0U || identity.pid > k_bootstrap_latch_owner_pid_mask) {
        return 0U;
    }
    return k_bootstrap_latch_owner_flag | static_cast<std::uint32_t>(identity.pid);
}

int finish_registry_operation(
    unsigned char *registry,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    int operation_result,
    bool operation_applied
) {
    const int release_result =
        mylite_ownerless_latch_release(registry_latch(registry), owner_id, owner_generation);
    if (release_result == MYLITE_OWNERLESS_LATCH_OK) {
        return operation_result;
    }
    if (release_result == MYLITE_OWNERLESS_LATCH_RELEASE_PENDING) {
        return operation_applied ? MYLITE_OWNERLESS_PROCESS_REGISTRY_APPLIED_RELEASE_PENDING
                                 : MYLITE_OWNERLESS_PROCESS_REGISTRY_RELEASE_PENDING;
    }
    return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
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
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
        }
        const std::uint32_t state = load32(slot, k_slot_state_offset);
        if (state == 0U) {
            continue;
        }
        if (state != MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE) {
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
        }
        if (open_mode == MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE) {
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY;
        }
        const std::uint32_t active_mode = load32(slot, k_slot_open_mode_offset);
        if (!open_mode_is_valid(active_mode)) {
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
        }
        if (!open_modes_are_compatible(open_mode, active_mode)) {
            return MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY;
        }
    }

    if (!generation_can_allocate(registry)) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }
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
        store64(registry, k_header_generation_offset, generation);
        store64(
            registry,
            k_header_active_count_offset,
            load64(registry, k_header_active_count_offset) + 1U
        );
        store32(slot, k_slot_state_offset, MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE);
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

    if (load64(registry, k_header_generation_offset) == std::numeric_limits<std::uint64_t>::max()) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
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
    std::uint64_t dead_slots = 0U;
    const std::uint32_t count = slot_count(registry);
    *out_cleaned_slots = 0U;

    for (std::uint32_t index = 0; index < count; ++index) {
        unsigned char *slot = slot_at(registry, index);
        if (static_cast<std::size_t>(
                slot + MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE - registry
            ) > mapping_size) {
            break;
        }
        if (load32(slot, k_slot_state_offset) == MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE) {
            const mylite_ownerless_process_identity identity = slot_identity(slot);
            if (is_alive(&identity, alive_ctx) == 0) {
                ++dead_slots;
            }
        }
    }
    if (dead_slots >
        std::numeric_limits<std::uint64_t>::max() - load64(registry, k_header_generation_offset)) {
        return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
    }

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
                *out_cleaned_slots = cleaned_slots;
                return MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR;
            }
        }
        clear_slot_locked(registry, slot);
        ++cleaned_slots;
        *out_cleaned_slots = cleaned_slots;
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
    if (
        out_start_time == nullptr || pid == 0U
#if defined(_WIN32)
        || pid > MAXDWORD
#else
        || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())
#endif
    ) {
        return false;
    }

#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return false;
    }
    FILETIME creation = {};
    FILETIME exit = {};
    FILETIME kernel = {};
    FILETIME user = {};
    const bool queried = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!queried) {
        return false;
    }
    *out_start_time =
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
    return *out_start_time != 0U;
#elif defined(__linux__)
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
#elif defined(__APPLE__)
    proc_bsdinfo info = {};
    const int bytes = proc_pidinfo(
        static_cast<int>(pid),
        PROC_PIDTBSDINFO,
        0,
        &info,
        static_cast<int>(sizeof(info))
    );
    if (bytes != static_cast<int>(sizeof(info))) {
        return false;
    }
    const std::uint64_t seconds = static_cast<std::uint64_t>(info.pbi_start_tvsec);
    const std::uint64_t microseconds = static_cast<std::uint64_t>(info.pbi_start_tvusec);
    if (seconds == 0U || microseconds >= 1000000U ||
        seconds > (std::numeric_limits<std::uint64_t>::max() - microseconds) / 1000000U) {
        return false;
    }
    *out_start_time = seconds * 1000000U + microseconds;
    return *out_start_time != 0U;
#else
    (void)pid;
#endif

    return false;
}

bool read_current_boot_id_hash(std::uint64_t *out_boot_id_hash) {
    if (out_boot_id_hash == nullptr) {
        return false;
    }

#if defined(_WIN32)
    /*
     * Windows process creation times are absolute FILETIME values. Unlike
     * Linux start ticks, they already distinguish PID reuse across boots, so
     * the epoch marker only identifies that interpretation of start_time.
     */
    *out_boot_id_hash = k_windows_process_identity_epoch;
    return true;
#elif defined(__linux__)
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
#elif defined(__APPLE__)
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    timeval boot_time = {};
    std::size_t boot_time_size = sizeof(boot_time);
    if (sysctl(mib, 2, &boot_time, &boot_time_size, nullptr, 0) != 0 ||
        boot_time_size != sizeof(boot_time)) {
        return false;
    }
    *out_boot_id_hash = hash_bytes(reinterpret_cast<const char *>(&boot_time), sizeof(boot_time));
    return *out_boot_id_hash != 0U;
#else
    return false;
#endif
}

bool process_is_zombie(std::uint64_t pid) {
    if (
        pid == 0U
#if defined(_WIN32)
        || pid > MAXDWORD
#else
        || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())
#endif
    ) {
        return false;
    }

#if defined(_WIN32)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return true;
    }
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait != WAIT_TIMEOUT;
#elif defined(__linux__)
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
#elif defined(__APPLE__)
    proc_bsdinfo info = {};
    const int bytes = proc_pidinfo(
        static_cast<int>(pid),
        PROC_PIDTBSDINFO,
        0,
        &info,
        static_cast<int>(sizeof(info))
    );
    return bytes == static_cast<int>(sizeof(info)) && info.pbi_status == SZOMB;
#else
    (void)pid;
    return false;
#endif
}

#if defined(__linux__) || defined(__APPLE__)
std::uint64_t hash_bytes(const char *bytes, std::size_t size) {
    std::uint64_t hash = k_fnv_offset_basis;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<unsigned char>(bytes[index]);
        hash *= k_fnv_prime;
    }
    return hash == 0U ? k_fnv_offset_basis : hash;
}
#endif

void clear_slot_locked(unsigned char *registry, unsigned char *slot) {
    const std::uint64_t generation = load64(registry, k_header_generation_offset) + 1U;
    /* State is the publication word; clear it before touching payload fields. */
    store32(slot, k_slot_state_offset, 0U);
    std::memset(slot, 0, MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE);
    store64(slot, k_slot_generation_offset, generation);
    store64(registry, k_header_generation_offset, generation);
    store64(
        registry,
        k_header_active_count_offset,
        load64(registry, k_header_active_count_offset) - 1U
    );
}

bool generation_can_allocate(const unsigned char *registry) {
    const std::uint64_t generation = load64(registry, k_header_generation_offset);
    const std::uint64_t active_count = load64(registry, k_header_active_count_offset);
    return active_count <= std::numeric_limits<std::uint64_t>::max() - 2U &&
           generation <= std::numeric_limits<std::uint64_t>::max() - active_count - 2U;
}

bool open_mode_is_valid(std::uint32_t open_mode) {
    return open_mode == MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE ||
           open_mode == MYLITE_OWNERLESS_PROCESS_OPEN_MODE_SHARED_READONLY ||
           open_mode == MYLITE_OWNERLESS_PROCESS_OPEN_MODE_OWNERLESS_RW;
}

bool open_modes_are_compatible(std::uint32_t requested_mode, std::uint32_t active_mode) {
    return requested_mode != MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE &&
           active_mode != MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE;
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

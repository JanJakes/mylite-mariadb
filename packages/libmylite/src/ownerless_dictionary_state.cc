#include "ownerless_dictionary_state.h"

#include "ownerless_wait.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::size_t k_generation_offset = 0;
constexpr std::size_t k_active_owner_id_offset = 8;
constexpr std::size_t k_active_owner_generation_offset = 16;
constexpr std::size_t k_active_owner_pid_offset = 24;
constexpr std::size_t k_wake_word_offset = 32;
constexpr std::size_t k_recoverable_kind_offset = 36;
constexpr std::size_t k_recoverable_owner_id_offset = 40;
constexpr std::size_t k_recoverable_owner_generation_offset = 48;

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
int wait_for_inactive_owner(
    unsigned char *state,
    mylite_ownerless_dictionary_state_alive_callback is_alive,
    void *alive_ctx,
    std::chrono::steady_clock::time_point deadline
);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
bool mapping_can_hold_state(const void *mapping, std::size_t mapping_size);
mylite_ownerless_wait_word *wake_word(unsigned char *state);
std::uint32_t load32(const unsigned char *base, std::size_t offset);
std::uint64_t load64(const unsigned char *base, std::size_t offset);
void store32(unsigned char *base, std::size_t offset, std::uint32_t value);
void store64(unsigned char *base, std::size_t offset, std::uint64_t value);
bool valid_recovery_kind(std::uint32_t recovery_kind);
void clear_recoverable_state(unsigned char *state);
bool compare_exchange32(
    unsigned char *base,
    std::size_t offset,
    std::uint32_t *expected,
    std::uint32_t desired
);
std::uint64_t add64(unsigned char *base, std::size_t offset, std::uint64_t value);

} // namespace

int mylite_ownerless_dictionary_state_initialize(void *mapping, std::size_t mapping_size) {
    if (!mapping_can_hold_state(mapping, mapping_size)) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    std::memset(mapping, 0, MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE);
    return MYLITE_OWNERLESS_DICTIONARY_STATE_OK;
}

int mylite_ownerless_dictionary_state_begin_ddl(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t owner_pid,
    unsigned timeout_ms,
    std::uint64_t *out_generation
) {
    if (!mapping_can_hold_state(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || owner_pid == 0U) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    auto *state = static_cast<unsigned char *>(mapping);
    const auto deadline = wait_deadline(timeout_ms);
    for (;;) {
        const std::uint64_t current_generation = load64(state, k_generation_offset);
        if ((current_generation & 1U) != 0U || load32(state, k_active_owner_id_offset) != 0U) {
            const int wait_result = wait_for_inactive_owner(state, nullptr, nullptr, deadline);
            if (wait_result != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
                return wait_result;
            }
            continue;
        }

        std::uint32_t expected_owner = 0U;
        if (compare_exchange32(state, k_active_owner_id_offset, &expected_owner, owner_id)) {
            store64(state, k_active_owner_generation_offset, owner_generation);
            store64(state, k_active_owner_pid_offset, owner_pid);
            clear_recoverable_state(state);
            const std::uint64_t generation = add64(state, k_generation_offset, 1U);
            if (out_generation != nullptr) {
                *out_generation = generation;
            }
            static_cast<void>(mylite_ownerless_wait_wake(wake_word(state)));
            return (generation & 1U) != 0U ? MYLITE_OWNERLESS_DICTIONARY_STATE_OK
                                           : MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
        }

        const int wait_result = wait_for_inactive_owner(state, nullptr, nullptr, deadline);
        if (wait_result != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
            return wait_result;
        }
    }
}

int mylite_ownerless_dictionary_state_finish_ddl(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t *out_generation
) {
    if (!mapping_can_hold_state(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    auto *state = static_cast<unsigned char *>(mapping);
    if (load32(state, k_active_owner_id_offset) != owner_id ||
        load64(state, k_active_owner_generation_offset) != owner_generation) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    if ((load64(state, k_generation_offset) & 1U) == 0U) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    const std::uint64_t generation = add64(state, k_generation_offset, 1U);
    clear_recoverable_state(state);
    store64(state, k_active_owner_pid_offset, 0U);
    store64(state, k_active_owner_generation_offset, 0U);
    store32(state, k_active_owner_id_offset, 0U);
    mylite_ownerless_wait_store(
        wake_word(state),
        mylite_ownerless_wait_load(wake_word(state)) + 1U
    );
    static_cast<void>(mylite_ownerless_wait_wake(wake_word(state)));
    if (out_generation != nullptr) {
        *out_generation = generation;
    }
    return (generation & 1U) == 0U ? MYLITE_OWNERLESS_DICTIONARY_STATE_OK
                                   : MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
}

int mylite_ownerless_dictionary_state_mark_recoverable(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t recovery_kind
) {
    if (!mapping_can_hold_state(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || !valid_recovery_kind(recovery_kind)) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    auto *state = static_cast<unsigned char *>(mapping);
    if (load32(state, k_active_owner_id_offset) != owner_id ||
        load64(state, k_active_owner_generation_offset) != owner_generation ||
        (load64(state, k_generation_offset) & 1U) == 0U) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    store32(state, k_recoverable_owner_id_offset, owner_id);
    store64(state, k_recoverable_owner_generation_offset, owner_generation);
    store32(state, k_recoverable_kind_offset, recovery_kind);
    return MYLITE_OWNERLESS_DICTIONARY_STATE_OK;
}

int mylite_ownerless_dictionary_state_recover_dead_owner(
    void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t recovery_kind,
    std::uint64_t *out_generation
) {
    if (!mapping_can_hold_state(mapping, mapping_size) || owner_id == 0U ||
        owner_generation == 0U || !valid_recovery_kind(recovery_kind)) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    auto *state = static_cast<unsigned char *>(mapping);
    if (load32(state, k_active_owner_id_offset) != owner_id ||
        load64(state, k_active_owner_generation_offset) != owner_generation ||
        load32(state, k_recoverable_kind_offset) != recovery_kind ||
        load32(state, k_recoverable_owner_id_offset) != owner_id ||
        load64(state, k_recoverable_owner_generation_offset) != owner_generation ||
        (load64(state, k_generation_offset) & 1U) == 0U) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    const std::uint64_t generation = add64(state, k_generation_offset, 1U);
    clear_recoverable_state(state);
    store64(state, k_active_owner_pid_offset, 0U);
    store64(state, k_active_owner_generation_offset, 0U);
    store32(state, k_active_owner_id_offset, 0U);
    mylite_ownerless_wait_store(
        wake_word(state),
        mylite_ownerless_wait_load(wake_word(state)) + 1U
    );
    static_cast<void>(mylite_ownerless_wait_wake(wake_word(state)));
    if (out_generation != nullptr) {
        *out_generation = generation;
    }
    return (generation & 1U) == 0U ? MYLITE_OWNERLESS_DICTIONARY_STATE_OK
                                   : MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
}

int mylite_ownerless_dictionary_state_wait_ready(
    void *mapping,
    std::size_t mapping_size,
    mylite_ownerless_dictionary_state_alive_callback is_alive,
    void *alive_ctx,
    unsigned timeout_ms,
    std::uint64_t *out_generation
) {
    if (!mapping_can_hold_state(mapping, mapping_size)) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    auto *state = static_cast<unsigned char *>(mapping);
    const auto deadline = wait_deadline(timeout_ms);
    for (;;) {
        const std::uint64_t generation = load64(state, k_generation_offset);
        if ((generation & 1U) == 0U && load32(state, k_active_owner_id_offset) == 0U) {
            if (out_generation != nullptr) {
                *out_generation = generation;
            }
            return MYLITE_OWNERLESS_DICTIONARY_STATE_OK;
        }

        const int wait_result = wait_for_inactive_owner(state, is_alive, alive_ctx, deadline);
        if (wait_result != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
            return wait_result;
        }
    }
}

int mylite_ownerless_dictionary_state_owner_active_count(
    const void *mapping,
    std::size_t mapping_size,
    std::uint32_t owner_id,
    std::uint32_t *out_active_count
) {
    if (!mapping_can_hold_state(mapping, mapping_size) || out_active_count == nullptr) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    const auto *state = static_cast<const unsigned char *>(mapping);
    *out_active_count = load32(state, k_active_owner_id_offset) == owner_id ? 1U : 0U;
    return MYLITE_OWNERLESS_DICTIONARY_STATE_OK;
}

int mylite_ownerless_dictionary_state_read_snapshot(
    const void *mapping,
    std::size_t mapping_size,
    mylite_ownerless_dictionary_state_snapshot *out_snapshot
) {
    if (!mapping_can_hold_state(mapping, mapping_size) || out_snapshot == nullptr) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
    }

    const auto *state = static_cast<const unsigned char *>(mapping);
    out_snapshot->generation = load64(state, k_generation_offset);
    out_snapshot->active_owner_id = load32(state, k_active_owner_id_offset);
    out_snapshot->active_owner_generation = load64(state, k_active_owner_generation_offset);
    out_snapshot->active_owner_pid = load64(state, k_active_owner_pid_offset);
    return MYLITE_OWNERLESS_DICTIONARY_STATE_OK;
}

namespace {

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

int wait_for_inactive_owner(
    unsigned char *state,
    mylite_ownerless_dictionary_state_alive_callback is_alive,
    void *alive_ctx,
    std::chrono::steady_clock::time_point deadline
) {
    const std::uint64_t pid = load64(state, k_active_owner_pid_offset);
    if (pid != 0U && is_alive != nullptr && is_alive(pid, alive_ctx) == 0) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_BUSY;
    }

    const std::uint32_t expected_wake = mylite_ownerless_wait_load(wake_word(state));
    const unsigned timeout_ms = remaining_timeout_ms(deadline);
    if (timeout_ms == 0U) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_TIMEOUT;
    }
    const int wait_result =
        mylite_ownerless_wait_for_change(wake_word(state), expected_wake, timeout_ms);
    if (wait_result == MYLITE_OWNERLESS_WAIT_OK) {
        return MYLITE_OWNERLESS_DICTIONARY_STATE_OK;
    }
    return wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT ? MYLITE_OWNERLESS_DICTIONARY_STATE_TIMEOUT
                                                        : MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR;
}

unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<unsigned>(std::max<std::chrono::milliseconds::rep>(remaining.count(), 1));
}

bool mapping_can_hold_state(const void *mapping, std::size_t mapping_size) {
    return mapping != nullptr && mapping_size >= MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE;
}

mylite_ownerless_wait_word *wake_word(unsigned char *state) {
    return reinterpret_cast<mylite_ownerless_wait_word *>(state + k_wake_word_offset);
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

bool valid_recovery_kind(std::uint32_t recovery_kind) {
    return recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_LIKE ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_SELECT ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_LIKE ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_SELECT ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TRUNCATE_TABLE ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_FORCE_REBUILD ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ROW_FORMAT_DYNAMIC ||
           recovery_kind ==
               MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_8 ||
           recovery_kind ==
               MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_1 ||
           recovery_kind ==
               MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_2 ||
           recovery_kind ==
               MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_4 ||
           recovery_kind ==
               MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_16 ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_CHARSET_CONVERT ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_VIEW ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_VIEW ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_VIEW ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_VIEW ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TRIGGER ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TRIGGER ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE_IF_EXISTS ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_SCHEMA ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_SCHEMA ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_SCHEMA ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_SCHEMA_IF_NOT_EXISTS ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_SCHEMA_IF_EXISTS ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_VIEW ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_INDEX_IDEMPOTENT_CREATE ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_INDEX_IDEMPOTENT_DROP ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMMENT ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_COLUMN_SET_DEFAULT ||
           recovery_kind == MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_IF_NOT_EXISTS;
}

void clear_recoverable_state(unsigned char *state) {
    store32(state, k_recoverable_kind_offset, MYLITE_OWNERLESS_DICTIONARY_RECOVERY_NONE);
    store64(state, k_recoverable_owner_generation_offset, 0U);
    store32(state, k_recoverable_owner_id_offset, 0U);
}

bool compare_exchange32(
    unsigned char *base,
    std::size_t offset,
    std::uint32_t *expected,
    std::uint32_t desired
) {
    auto *target = reinterpret_cast<std::uint32_t *>(base + offset);
    return __atomic_compare_exchange_n(
        target,
        expected,
        desired,
        false,
        __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE
    );
}

std::uint64_t add64(unsigned char *base, std::size_t offset, std::uint64_t value) {
    auto *target = reinterpret_cast<std::uint64_t *>(base + offset);
    return __atomic_add_fetch(target, value, __ATOMIC_ACQ_REL);
}

} // namespace

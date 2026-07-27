#include "ownerless_mdl.h"

#include "ownerless_lock_table.h"

#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::uint64_t k_fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t k_fnv_prime = 1099511628211ULL;
constexpr std::uint64_t k_legacy_session_id = std::numeric_limits<std::uint64_t>::max();

bool key_parts_are_valid(
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
);
std::uint64_t hash_byte(std::uint64_t hash, unsigned char byte);
std::uint64_t hash_bytes(std::uint64_t hash, const char *value);

} // namespace

std::uint64_t mylite_ownerless_mdl_key_hash(
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
) {
    if (!key_parts_are_valid(mdl_namespace, database_name, object_name)) {
        return 0U;
    }

    std::uint64_t hash = k_fnv_offset_basis;
    hash = hash_byte(hash, static_cast<unsigned char>(mdl_namespace));
    hash = hash_bytes(hash, database_name);
    hash = hash_bytes(hash, object_name);
    return hash == 0U ? 1U : hash;
}

int mylite_ownerless_mdl_acquire_shared(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_mdl_acquire_mode(
        lock_table,
        lock_table_size,
        owner_id,
        owner_generation,
        mdl_namespace,
        database_name,
        object_name,
        MYLITE_OWNERLESS_LOCK_TABLE_SHARED,
        timeout_ms
    );
}

int mylite_ownerless_mdl_acquire_upgradable(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_mdl_acquire_mode(
        lock_table,
        lock_table_size,
        owner_id,
        owner_generation,
        mdl_namespace,
        database_name,
        object_name,
        MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE,
        timeout_ms
    );
}

int mylite_ownerless_mdl_acquire_exclusive(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_mdl_acquire_mode(
        lock_table,
        lock_table_size,
        owner_id,
        owner_generation,
        mdl_namespace,
        database_name,
        object_name,
        MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE,
        timeout_ms
    );
}

int mylite_ownerless_mdl_acquire_mode(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    std::uint32_t mode,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_mdl_acquire_mode_for_session_with_options(
        lock_table,
        lock_table_size,
        owner_id,
        owner_generation,
        k_legacy_session_id,
        mdl_namespace,
        database_name,
        object_name,
        mode,
        nullptr,
        timeout_ms
    );
}

int mylite_ownerless_mdl_acquire_mode_for_session(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    std::uint32_t mode,
    std::uint64_t timeout_ms
) {
    return mylite_ownerless_mdl_acquire_mode_for_session_with_options(
        lock_table,
        lock_table_size,
        owner_id,
        owner_generation,
        session_id,
        mdl_namespace,
        database_name,
        object_name,
        mode,
        nullptr,
        timeout_ms
    );
}

int mylite_ownerless_mdl_acquire_mode_for_session_with_options(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    std::uint32_t mode,
    const mylite_ownerless_mdl_wait_options *wait_options,
    std::uint64_t timeout_ms
) {
    const std::uint64_t key_hash =
        mylite_ownerless_mdl_key_hash(mdl_namespace, database_name, object_name);
    if (key_hash == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    if (wait_options != nullptr && wait_options->reclassify_from_mode != 0U) {
        return mylite_ownerless_lock_table_reclassify_mode_for_session(
            lock_table,
            lock_table_size,
            key_hash,
            owner_id,
            owner_generation,
            session_id,
            wait_options->reclassify_from_mode,
            mode
        );
    }
    return mylite_ownerless_lock_table_acquire_mode_for_session_with_scheduling(
        lock_table,
        lock_table_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        mode,
        wait_options == nullptr || wait_options->bypass_queued_waiters == 0U ? 0 : 1,
        wait_options == nullptr ? MYLITE_OWNERLESS_LOCK_TABLE_DEFAULT_DEADLOCK_WEIGHT
                                : wait_options->deadlock_weight,
        wait_options == nullptr ? std::numeric_limits<std::uint64_t>::max()
                                : wait_options->max_write_lock_count,
        wait_options == nullptr ? nullptr : wait_options->is_cancelled,
        wait_options == nullptr ? nullptr : wait_options->cancel_context,
        timeout_ms
    );
}

int mylite_ownerless_mdl_release_shared(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
) {
    return mylite_ownerless_mdl_release_mode(
        lock_table,
        lock_table_size,
        owner_id,
        owner_generation,
        mdl_namespace,
        database_name,
        object_name,
        MYLITE_OWNERLESS_LOCK_TABLE_SHARED
    );
}

int mylite_ownerless_mdl_release_upgradable(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
) {
    return mylite_ownerless_mdl_release_mode(
        lock_table,
        lock_table_size,
        owner_id,
        owner_generation,
        mdl_namespace,
        database_name,
        object_name,
        MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE
    );
}

int mylite_ownerless_mdl_release_exclusive(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
) {
    return mylite_ownerless_mdl_release_mode(
        lock_table,
        lock_table_size,
        owner_id,
        owner_generation,
        mdl_namespace,
        database_name,
        object_name,
        MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE
    );
}

int mylite_ownerless_mdl_release_mode(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    std::uint32_t mode
) {
    return mylite_ownerless_mdl_release_mode_for_session(
        lock_table,
        lock_table_size,
        owner_id,
        owner_generation,
        k_legacy_session_id,
        mdl_namespace,
        database_name,
        object_name,
        mode
    );
}

int mylite_ownerless_mdl_release_mode_for_session(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    std::uint64_t session_id,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    std::uint32_t mode
) {
    const std::uint64_t key_hash =
        mylite_ownerless_mdl_key_hash(mdl_namespace, database_name, object_name);
    if (key_hash == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    return mylite_ownerless_lock_table_release_mode_for_session(
        lock_table,
        lock_table_size,
        key_hash,
        owner_id,
        owner_generation,
        session_id,
        mode
    );
}

namespace {

bool key_parts_are_valid(
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
) {
    return (mdl_namespace == MYLITE_OWNERLESS_MDL_NAMESPACE_SCHEMA ||
            mdl_namespace == MYLITE_OWNERLESS_MDL_NAMESPACE_TABLE) &&
           database_name != nullptr && object_name != nullptr;
}

std::uint64_t hash_byte(std::uint64_t hash, unsigned char byte) {
    hash ^= byte;
    hash *= k_fnv_prime;
    return hash;
}

std::uint64_t hash_bytes(std::uint64_t hash, const char *value) {
    const std::size_t length = std::strlen(value);
    for (std::size_t index = 0; index < length; ++index) {
        hash = hash_byte(hash, static_cast<unsigned char>(value[index]));
    }
    return hash_byte(hash, 0U);
}

} // namespace

#include "ownerless_mdl.h"

#include "ownerless_lock_table.h"

#include <cstdint>
#include <cstring>

namespace {

constexpr std::uint64_t k_fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t k_fnv_prime = 1099511628211ULL;

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
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    unsigned timeout_ms
) {
    const std::uint64_t key_hash =
        mylite_ownerless_mdl_key_hash(mdl_namespace, database_name, object_name);
    if (key_hash == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    return mylite_ownerless_lock_table_acquire_shared(
        lock_table,
        lock_table_size,
        key_hash,
        owner_id,
        timeout_ms
    );
}

int mylite_ownerless_mdl_acquire_exclusive(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name,
    unsigned timeout_ms
) {
    const std::uint64_t key_hash =
        mylite_ownerless_mdl_key_hash(mdl_namespace, database_name, object_name);
    if (key_hash == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    return mylite_ownerless_lock_table_acquire_exclusive(
        lock_table,
        lock_table_size,
        key_hash,
        owner_id,
        timeout_ms
    );
}

int mylite_ownerless_mdl_release_shared(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
) {
    const std::uint64_t key_hash =
        mylite_ownerless_mdl_key_hash(mdl_namespace, database_name, object_name);
    if (key_hash == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    return mylite_ownerless_lock_table_release_shared(
        lock_table,
        lock_table_size,
        key_hash,
        owner_id
    );
}

int mylite_ownerless_mdl_release_exclusive(
    void *lock_table,
    std::size_t lock_table_size,
    std::uint32_t owner_id,
    std::uint32_t mdl_namespace,
    const char *database_name,
    const char *object_name
) {
    const std::uint64_t key_hash =
        mylite_ownerless_mdl_key_hash(mdl_namespace, database_name, object_name);
    if (key_hash == 0U) {
        return MYLITE_OWNERLESS_LOCK_TABLE_ERROR;
    }
    return mylite_ownerless_lock_table_release_exclusive(
        lock_table,
        lock_table_size,
        key_hash,
        owner_id
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

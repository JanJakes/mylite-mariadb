#ifndef MYLITE_OWNERLESS_AUTOINC_REGISTRY_H
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK 0
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL 1
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_ERROR 2

#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE 64U
#define MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE 32U

size_t mylite_ownerless_autoinc_registry_size(uint32_t slot_count);
int mylite_ownerless_autoinc_registry_initialize(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_count
);
int mylite_ownerless_autoinc_registry_read_or_seed(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t table_id,
    uint64_t seed_next_value,
    uint64_t *out_next_value
);
int mylite_ownerless_autoinc_registry_publish(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t table_id,
    uint64_t next_value
);

#ifdef __cplusplus
}
#endif

#endif

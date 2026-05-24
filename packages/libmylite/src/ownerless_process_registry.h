#ifndef MYLITE_OWNERLESS_PROCESS_REGISTRY_H
#define MYLITE_OWNERLESS_PROCESS_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_PROCESS_REGISTRY_OK 0
#define MYLITE_OWNERLESS_PROCESS_REGISTRY_FULL 1
#define MYLITE_OWNERLESS_PROCESS_REGISTRY_NOT_FOUND 2
#define MYLITE_OWNERLESS_PROCESS_REGISTRY_TIMEOUT 3
#define MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR 4

#define MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE 64U
#define MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE 128U
#define MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE 1U

size_t mylite_ownerless_process_registry_size(uint32_t slot_count);
int mylite_ownerless_process_registry_initialize(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_count
);
int mylite_ownerless_process_registry_allocate(
    void *mapping,
    size_t mapping_size,
    uint64_t pid,
    uint32_t open_mode,
    uint64_t shm_generation,
    uint32_t *out_slot_index,
    uint64_t *out_slot_generation
);
int mylite_ownerless_process_registry_release(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_index,
    uint64_t slot_generation
);
uint64_t mylite_ownerless_process_registry_active_count(const void *mapping);

#ifdef __cplusplus
}
#endif

#endif

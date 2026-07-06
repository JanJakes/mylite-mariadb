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
#define MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY 5

#define MYLITE_OWNERLESS_PROCESS_CLEANUP_OK 0
#define MYLITE_OWNERLESS_PROCESS_CLEANUP_BLOCKED 1
#define MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR -1

#define MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE 96U
#define MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE 128U
#define MYLITE_OWNERLESS_PROCESS_STATE_ACTIVE 1U

typedef struct mylite_ownerless_process_identity {
    uint64_t pid;
    uint64_t start_time;
    uint64_t boot_id_hash;
} mylite_ownerless_process_identity;

typedef int (*mylite_ownerless_process_alive_callback)(
    const mylite_ownerless_process_identity *identity,
    void *ctx
);
typedef int (*mylite_ownerless_process_cleanup_callback)(
    uint32_t slot_index,
    uint64_t slot_generation,
    const mylite_ownerless_process_identity *identity,
    void *ctx
);

int mylite_ownerless_process_identity_for_pid(
    uint64_t pid,
    mylite_ownerless_process_identity *out_identity
);
int mylite_ownerless_current_process_identity(mylite_ownerless_process_identity *out_identity);
int mylite_ownerless_process_identity_is_alive(
    const mylite_ownerless_process_identity *identity,
    void *ctx
);
size_t mylite_ownerless_process_registry_size(uint32_t slot_count);
int mylite_ownerless_process_registry_initialize(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_count
);
int mylite_ownerless_process_registry_allocate(
    void *mapping,
    size_t mapping_size,
    mylite_ownerless_process_identity identity,
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
int mylite_ownerless_process_registry_heartbeat(
    void *mapping,
    size_t mapping_size,
    uint32_t slot_index,
    uint64_t slot_generation,
    uint64_t heartbeat
);
int mylite_ownerless_process_registry_cleanup_dead(
    void *mapping,
    size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *ctx,
    uint32_t *out_cleaned_slots
);
int mylite_ownerless_process_registry_cleanup_dead_with_callback(
    void *mapping,
    size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *alive_ctx,
    mylite_ownerless_process_cleanup_callback cleanup,
    void *cleanup_ctx,
    uint32_t *out_cleaned_slots
);
uint64_t mylite_ownerless_process_registry_active_count(const void *mapping);
uint64_t mylite_ownerless_process_registry_generation(const void *mapping);
int mylite_ownerless_process_registry_live_count(
    void *mapping,
    size_t mapping_size,
    mylite_ownerless_process_alive_callback is_alive,
    void *ctx,
    uint64_t *out_live_count
);

#ifdef __cplusplus
}
#endif

#endif

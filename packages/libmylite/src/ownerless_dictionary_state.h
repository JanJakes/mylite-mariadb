#ifndef MYLITE_OWNERLESS_DICTIONARY_STATE_H
#define MYLITE_OWNERLESS_DICTIONARY_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_DICTIONARY_STATE_OK 0
#define MYLITE_OWNERLESS_DICTIONARY_STATE_BUSY 1
#define MYLITE_OWNERLESS_DICTIONARY_STATE_TIMEOUT 2
#define MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR 3

#define MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE 64U

typedef int (*mylite_ownerless_dictionary_state_alive_callback)(uint64_t pid, void *ctx);

typedef struct mylite_ownerless_dictionary_state_snapshot {
    uint64_t generation;
    uint32_t active_owner_id;
    uint64_t active_owner_generation;
    uint64_t active_owner_pid;
} mylite_ownerless_dictionary_state_snapshot;

int mylite_ownerless_dictionary_state_initialize(void *mapping, size_t mapping_size);
int mylite_ownerless_dictionary_state_begin_ddl(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t owner_pid,
    unsigned int timeout_ms,
    uint64_t *out_generation
);
int mylite_ownerless_dictionary_state_finish_ddl(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t *out_generation
);
int mylite_ownerless_dictionary_state_wait_ready(
    void *mapping,
    size_t mapping_size,
    mylite_ownerless_dictionary_state_alive_callback is_alive,
    void *alive_ctx,
    unsigned int timeout_ms,
    uint64_t *out_generation
);
int mylite_ownerless_dictionary_state_owner_active_count(
    const void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t *out_active_count
);
int mylite_ownerless_dictionary_state_read_snapshot(
    const void *mapping,
    size_t mapping_size,
    mylite_ownerless_dictionary_state_snapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif

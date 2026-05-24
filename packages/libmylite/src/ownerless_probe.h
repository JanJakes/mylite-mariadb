#ifndef MYLITE_OWNERLESS_PROBE_H
#define MYLITE_OWNERLESS_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_PROBE_OK 0
#define MYLITE_OWNERLESS_PROBE_ERROR 1

typedef struct mylite_ownerless_probe_result {
    uint32_t size;
    uint32_t mmap_shared_visibility;
    uint32_t byte_range_locks;
    uint32_t lock_release_on_exit;
    uint32_t grow_remap;
    uint32_t wait_backend;
    uint32_t fast_wait_backend;
    uint32_t required_primitives;
    uint32_t platform_candidate;
} mylite_ownerless_probe_result;

int mylite_ownerless_probe_platform(mylite_ownerless_probe_result *result);

#ifdef __cplusplus
}
#endif

#endif

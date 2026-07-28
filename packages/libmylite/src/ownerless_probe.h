#ifndef MYLITE_OWNERLESS_PROBE_H
#define MYLITE_OWNERLESS_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_PROBE_OK 0
#define MYLITE_OWNERLESS_PROBE_ERROR 1
#define MYLITE_OWNERLESS_FILESYSTEM_NAME_SIZE 32

typedef enum mylite_ownerless_filesystem_kind {
    MYLITE_OWNERLESS_FILESYSTEM_UNKNOWN = 0,
    MYLITE_OWNERLESS_FILESYSTEM_EXT4 = 1,
    MYLITE_OWNERLESS_FILESYSTEM_XFS = 2,
    MYLITE_OWNERLESS_FILESYSTEM_TMPFS = 3,
    MYLITE_OWNERLESS_FILESYSTEM_OVERLAY = 4,
    MYLITE_OWNERLESS_FILESYSTEM_APFS = 5,
    MYLITE_OWNERLESS_FILESYSTEM_NTFS = 6
} mylite_ownerless_filesystem_kind;

typedef struct mylite_ownerless_filesystem_info {
    uint32_t size;
    uint32_t kind;
    uint32_t is_local;
    uint32_t is_admitted;
    uint64_t volume_identity;
    char name[MYLITE_OWNERLESS_FILESYSTEM_NAME_SIZE];
} mylite_ownerless_filesystem_info;

typedef struct mylite_ownerless_probe_result {
    uint32_t size;
    uint32_t mmap_shared_visibility;
    uint32_t byte_range_locks;
    uint32_t lock_release_on_exit;
    uint32_t lock_close_isolation;
    uint32_t grow_remap;
    uint32_t wait_backend;
    uint32_t fast_wait_backend;
    uint32_t process_identity;
    uint32_t required_primitives;
    uint32_t platform_candidate;
} mylite_ownerless_probe_result;

int mylite_ownerless_probe_platform(mylite_ownerless_probe_result *result);
int mylite_ownerless_probe_directory(const char *directory, mylite_ownerless_probe_result *result);
int mylite_ownerless_probe_filesystem(
    const char *directory,
    mylite_ownerless_filesystem_info *out_info
);
int mylite_ownerless_probe_filesystem_type(const char *directory, uint64_t *out_type);
int mylite_ownerless_filesystem_type_is_validated_local(uint64_t filesystem_type);

#ifdef __cplusplus
}
#endif

#endif

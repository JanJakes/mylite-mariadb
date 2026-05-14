#ifndef MYLITE_STORAGE_H
#define MYLITE_STORAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_STORAGE_ENGINE_NAME "MYLITE"
#define MYLITE_STORAGE_FORMAT_VERSION 0U
#define MYLITE_STORAGE_CAPABILITY_STUB 0x00000001U

typedef struct mylite_storage_capabilities {
    size_t size;
    unsigned format_version;
    unsigned flags;
} mylite_storage_capabilities;

const char *mylite_storage_engine_name(void);
mylite_storage_capabilities mylite_storage_get_capabilities(void);

#ifdef __cplusplus
}
#endif

#endif

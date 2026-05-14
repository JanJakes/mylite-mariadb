#include <mylite/storage.h>

const char *mylite_storage_engine_name(void) {
    return MYLITE_STORAGE_ENGINE_NAME;
}

mylite_storage_capabilities mylite_storage_get_capabilities(void) {
    mylite_storage_capabilities capabilities = {
        .size = sizeof(capabilities),
        .format_version = MYLITE_STORAGE_FORMAT_VERSION,
        .flags = MYLITE_STORAGE_CAPABILITY_STUB,
    };

    return capabilities;
}

#include <assert.h>
#include <string.h>

#include <mylite/storage.h>

int main(void) {
    const mylite_storage_capabilities capabilities = mylite_storage_get_capabilities();

    assert(strcmp(mylite_storage_engine_name(), MYLITE_STORAGE_ENGINE_NAME) == 0);
    assert(capabilities.size == sizeof(capabilities));
    assert(capabilities.format_version == MYLITE_STORAGE_FORMAT_VERSION);
    assert((capabilities.flags & MYLITE_STORAGE_CAPABILITY_STUB) != 0U);

    return 0;
}

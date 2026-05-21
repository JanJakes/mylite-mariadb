#include <mylite/mylite.h>

typedef struct mylite_php_probe_module {
    const char *(*version)(void);
    int (*open)(const char *, mylite_db **, unsigned, const mylite_open_config *);
    int (*close)(mylite_db *);
} mylite_php_probe_module;

static const mylite_php_probe_module probe_module = {
    mylite_version,
    mylite_open,
    mylite_close,
};

#ifdef _WIN32
#  define MYLITE_PHP_PROBE_EXPORT __declspec(dllexport)
#else
#  define MYLITE_PHP_PROBE_EXPORT __attribute__((visibility("default")))
#endif

// The audit probe intentionally exports this one PHP-shaped entry point.
// NOLINTNEXTLINE(misc-use-internal-linkage)
MYLITE_PHP_PROBE_EXPORT void *get_module(void) {
    return (void *)&probe_module;
}

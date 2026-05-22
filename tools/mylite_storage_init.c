#include <mylite/storage.h>

#include <stdio.h>

static const char *result_name(mylite_storage_result result);

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: mylite-storage-init <database.mylite>\n");
        return 2;
    }

    const mylite_storage_result result = mylite_storage_create_empty(argv[1]);
    if (result != MYLITE_STORAGE_OK) {
        fprintf(
            stderr,
            "Could not create empty MyLite storage file '%s': %s\n",
            argv[1],
            result_name(result)
        );
        return 1;
    }

    return 0;
}

static const char *result_name(mylite_storage_result result) {
    switch (result) {
    case MYLITE_STORAGE_OK:
        return "ok";
    case MYLITE_STORAGE_ERROR:
        return "error";
    case MYLITE_STORAGE_BUSY:
        return "busy";
    case MYLITE_STORAGE_NOMEM:
        return "nomem";
    case MYLITE_STORAGE_READONLY:
        return "readonly";
    case MYLITE_STORAGE_IOERR:
        return "ioerr";
    case MYLITE_STORAGE_CORRUPT:
        return "corrupt";
    case MYLITE_STORAGE_NOTFOUND:
        return "notfound";
    case MYLITE_STORAGE_FULL:
        return "full";
    case MYLITE_STORAGE_MISUSE:
        return "misuse";
    case MYLITE_STORAGE_UNSUPPORTED:
        return "unsupported";
    }

    return "unknown";
}

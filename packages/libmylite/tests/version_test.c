#include <mylite/mylite.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    const char *version = mylite_version();

    if (strcmp(version, MYLITE_VERSION_STRING) != 0) {
        fprintf(stderr, "expected %s, got %s\n", MYLITE_VERSION_STRING, version);
        return 1;
    }

    return 0;
}

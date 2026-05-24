#ifndef MYLITE_OWNERLESS_WAIT_H
#define MYLITE_OWNERLESS_WAIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_WAIT_OK 0
#define MYLITE_OWNERLESS_WAIT_TIMEOUT 1
#define MYLITE_OWNERLESS_WAIT_ERROR 2

typedef struct mylite_ownerless_wait_word {
    uint32_t value;
} mylite_ownerless_wait_word;

uint32_t mylite_ownerless_wait_load(const mylite_ownerless_wait_word *word);
void mylite_ownerless_wait_store(mylite_ownerless_wait_word *word, uint32_t value);
int mylite_ownerless_wait_for_change(
    mylite_ownerless_wait_word *word,
    uint32_t expected,
    unsigned timeout_ms
);
int mylite_ownerless_wait_wake(mylite_ownerless_wait_word *word);
const char *mylite_ownerless_wait_backend_name(void);
int mylite_ownerless_wait_backend_is_fast(void);

#ifdef __cplusplus
}
#endif

#endif

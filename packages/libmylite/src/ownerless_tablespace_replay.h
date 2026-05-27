#ifndef MYLITE_OWNERLESS_TABLESPACE_REPLAY_H
#define MYLITE_OWNERLESS_TABLESPACE_REPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK 0
#define MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR 1

int mylite_ownerless_tablespace_replay_apply(
    const char *datadir,
    int page_log_fd,
    uint64_t page_log_offset,
    uint64_t visible_lsn
);

#ifdef __cplusplus
}
#endif

#endif

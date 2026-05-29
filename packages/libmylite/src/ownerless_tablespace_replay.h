#ifndef MYLITE_OWNERLESS_TABLESPACE_REPLAY_H
#define MYLITE_OWNERLESS_TABLESPACE_REPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK 0
#define MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR 1
#define MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES 0x1U

int mylite_ownerless_tablespace_replay_apply(
    const char *datadir,
    int page_log_fd,
    uint64_t page_log_offset,
    uint64_t visible_lsn
);
int mylite_ownerless_tablespace_replay_apply_with_flags(
    const char *datadir,
    int page_log_fd,
    uint64_t page_log_offset,
    uint64_t visible_lsn,
    unsigned flags
);

#ifdef __cplusplus
}
#endif

#endif

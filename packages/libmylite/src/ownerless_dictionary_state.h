#ifndef MYLITE_OWNERLESS_DICTIONARY_STATE_H
#define MYLITE_OWNERLESS_DICTIONARY_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLITE_OWNERLESS_DICTIONARY_STATE_OK 0
#define MYLITE_OWNERLESS_DICTIONARY_STATE_BUSY 1
#define MYLITE_OWNERLESS_DICTIONARY_STATE_TIMEOUT 2
#define MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR 3

#define MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE 64U

#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_NONE 0U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE 1U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_LIKE 2U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_SELECT 3U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE 4U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_LIKE 5U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_SELECT 6U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE 7U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TRUNCATE_TABLE 8U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE 9U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_FORCE_REBUILD 10U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ROW_FORMAT_DYNAMIC 11U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_8 12U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_1 13U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_2 14U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_4 15U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_16 16U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_CHARSET_CONVERT 17U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_VIEW 18U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_VIEW 19U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_VIEW 20U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_VIEW 21U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TRIGGER 22U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TRIGGER 23U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE_IF_EXISTS 24U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_SCHEMA 25U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_SCHEMA 26U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_SCHEMA 27U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_SCHEMA_IF_NOT_EXISTS 28U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_SCHEMA_IF_EXISTS 29U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_VIEW 30U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_INDEX_IDEMPOTENT_CREATE 31U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_INDEX_IDEMPOTENT_DROP 32U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMMENT 33U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_COLUMN_SET_DEFAULT 34U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_IF_NOT_EXISTS 35U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_COLUMN_IDEMPOTENT_ADD 36U
#define MYLITE_OWNERLESS_DICTIONARY_RECOVERY_COLUMN_IDEMPOTENT_DROP 37U

typedef int (*mylite_ownerless_dictionary_state_alive_callback)(uint64_t pid, void *ctx);

typedef struct mylite_ownerless_dictionary_state_snapshot {
    uint64_t generation;
    uint32_t active_owner_id;
    uint64_t active_owner_generation;
    uint64_t active_owner_pid;
} mylite_ownerless_dictionary_state_snapshot;

int mylite_ownerless_dictionary_state_initialize(void *mapping, size_t mapping_size);
int mylite_ownerless_dictionary_state_begin_ddl(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t owner_pid,
    unsigned int timeout_ms,
    uint64_t *out_generation
);
int mylite_ownerless_dictionary_state_finish_ddl(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint64_t *out_generation
);
int mylite_ownerless_dictionary_state_mark_recoverable(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t recovery_kind
);
int mylite_ownerless_dictionary_state_recover_dead_owner(
    void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint64_t owner_generation,
    uint32_t recovery_kind,
    uint64_t *out_generation
);
int mylite_ownerless_dictionary_state_wait_ready(
    void *mapping,
    size_t mapping_size,
    mylite_ownerless_dictionary_state_alive_callback is_alive,
    void *alive_ctx,
    unsigned int timeout_ms,
    uint64_t *out_generation
);
int mylite_ownerless_dictionary_state_owner_active_count(
    const void *mapping,
    size_t mapping_size,
    uint32_t owner_id,
    uint32_t *out_active_count
);
int mylite_ownerless_dictionary_state_read_snapshot(
    const void *mapping,
    size_t mapping_size,
    mylite_ownerless_dictionary_state_snapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif

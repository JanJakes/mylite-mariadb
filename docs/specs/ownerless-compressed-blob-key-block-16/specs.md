# Ownerless Compressed BLOB Key-Block 16

## Problem

The compressed BLOB key-block matrix covered `KEY_BLOCK_SIZE=1`, `2`, `4`, and
`8`, but left `16` planned because it equals the current embedded InnoDB page
size. MariaDB accepts `KEY_BLOCK_SIZE=16` when it fits the configured page-size
maximum, so MyLite needs direct evidence that the ownerless compressed BLOB
pressure lifecycle still works for that native compressed page size.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `create_table_info_t::create_options_are_invalid()` accepts
  `KEY_BLOCK_SIZE` values `1`, `2`, `4`, `8`, and `16` when the value is within
  the configured InnoDB page-size maximum.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `create_table_info_t::create_table_def()` applies the requested compressed
  key-block size only to `ROW_FORMAT=COMPRESSED`.
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` as native compressed BLOB
  page types.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  already scans each key-block matrix table's `.ibd` file with a stride matching
  that table's requested key-block size.

## Scope And Non-Goals

In scope:

- Add `app.ownerless_compressed_blob_kb16` to the
  `compressed-blob-key-block-matrix` selector.
- Verify the existing prepared-BLOB, live-snapshot, retained-WAL,
  post-release checkpoint, ownerless/native reopen, forced `.shm` rebuild, and
  native `ZBLOB`/`ZBLOB2` evidence path for `KEY_BLOCK_SIZE=16`.
- Update compatibility and cross-process concurrency docs so the matrix records
  all MariaDB-accepted key-block values for the current profile.

Out of scope:

- Changing compressed-row-format production behavior.
- Compressed row-format DDL transition matrices.
- Encryption, crash injection, and external oracle stress.

## Design

Extend the existing key-block case table with:

```c
{.table_name = "ownerless_compressed_blob_kb16", .key_block_size = 16U}
```

The existing selector then creates a fifth compressed table, inserts the same
deterministic low-compressibility payloads through prepared bindings, scans the
closed `.ibd` file at 16 KiB boundaries for native compressed BLOB page types,
updates each payload while an ownerless repeatable-read snapshot remains live,
and validates final state through ownerless/native reopen before and after
forced `.shm` rebuild.

## Compatibility Impact

No SQL behavior changes. The slice closes the deterministic compressed
`KEY_BLOCK_SIZE` matrix for MariaDB-accepted values in the current embedded
profile.

## Directory And Lifecycle Impact

No directory layout changes. The selector creates one additional native InnoDB
file-per-table tablespace inside the MyLite database directory and exercises the
existing ownerless WAL, checkpoint, and shared-memory rebuild lifecycle.

## Native Storage Impact

No storage-format changes. The test adds native evidence that
`KEY_BLOCK_SIZE=16` compressed external BLOB pages remain durable and readable
across ownerless pressure, checkpoint, and reopen boundaries.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test and documentation
coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `compressed-blob-key-block-matrix` selector.
- Build and run the same focused selector in `ownerless-test-hooks`.
- Run the ownerless SQL CTest shard containing the selector in both presets.
- Run adjacent compressed BLOB pressure stress coverage.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- `KEY_BLOCK_SIZE=16` table creation succeeds with `ROW_FORMAT=COMPRESSED`.
- The closed `ownerless_compressed_blob_kb16.ibd` file contains native
  `ZBLOB`/`ZBLOB2` page evidence at the 16 KiB scan stride.
- A live repeatable-read snapshot observes original BLOB aggregates while peer
  ownerless writers update the 16 KiB table and the rest of the matrix.
- Final aggregates and native compressed BLOB page evidence survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This remains deterministic in-process coverage; compressed-row-format DDL,
  crash injection, encryption, and external oracle stress remain separate work.

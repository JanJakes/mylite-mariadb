# Ownerless Row-Format DDL Crash

## Problem Statement

Ownerless row-format coverage proves an already-open peer refreshes after
another ownerless process rebuilds an InnoDB table from `ROW_FORMAT=COMPACT` to
`ROW_FORMAT=DYNAMIC`. It does not yet kill a writer after MariaDB/InnoDB
completes the native row-format rebuild but before MyLite publishes ownerless
dictionary finish.

This slice adds focused crash-boundary evidence for that supported native
table-option rebuild path.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:5844` through
  `mariadb/sql/sql_yacc.yy:5848` parses `ROW_FORMAT` table options and marks
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/handler.h:638` defines `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_table.cc:11114` through
  `mariadb/sql/sql_table.cc:11123` preserves the old row type when ALTER has no
  explicit row type and marks explicit ALTER row types as
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/storage/innobase/handler/handler0alter.cc:1541` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1556` requires an InnoDB
  table rebuild when ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11234` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11342` validates explicit
  InnoDB row-format and key-block option combinations.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11808` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11845` maps SQL row types to
  native InnoDB record formats including `COMPACT` and `DYNAMIC`.
- `mariadb/storage/innobase/handler/i_s.cc:4377` through
  `mariadb/storage/innobase/handler/i_s.cc:4383` defines exposed InnoDB row
  format values, and `mariadb/storage/innobase/handler/i_s.cc:4447` through
  `mariadb/storage/innobase/handler/i_s.cc:4471` derives and stores
  `INNODB_SYS_TABLES.ROW_FORMAT`.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database and create `app.ownerless_row_format_base`
  as an InnoDB table with `ROW_FORMAT=COMPACT`,
- insert rows with payload bytes large enough to prove copied content survives
  the rebuild,
- start a live ownerless peer so crashed-writer cleanup remains busy,
- start a writer that executes
  `ALTER TABLE app.ownerless_row_format_base ROW_FORMAT=DYNAMIC` under the
  existing `dictionary-before-finish` hook,
- kill the writer at the hook after native MariaDB/InnoDB DDL completes but
  before ownerless dictionary finish,
- prove an ownerless opener returns `MYLITE_BUSY` while the live peer remains,
- release the peer and reopen ownerless read/write to rebuild volatile
  coordination,
- verify recovered metadata exposes `INNODB_SYS_TABLES.ROW_FORMAT = 'Dynamic'`
  and `information_schema.tables.row_format = 'Dynamic'`,
- verify retained rows and payload lengths, then insert another row,
- verify the final state survives ownerless reopen, native exclusive reopen,
  forced `.shm` rebuild, and native exclusive reopen after rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for a completed
  `ROW_FORMAT=COMPACT` to `ROW_FORMAT=DYNAMIC` rebuild,
- live-peer cleanup-busy behavior and no-live rebuild,
- ownerless/native reopen of recovered row-format metadata, retained rows, and
  post-recovery writes.

Out of scope:

- compressed row-format or `KEY_BLOCK_SIZE` crash recovery,
- redundant row-format, encryption, page compression, external-directory,
  partition, tablespace import/discard, or general tablespace variants,
- randomized DDL oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for existing
ownerless `ROW_FORMAT=DYNAMIC` compatibility by proving that a writer death at
MyLite's dictionary publication boundary preserves the completed native
row-format rebuild.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises existing file-per-table native storage,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

The covered DDL uses MariaDB/InnoDB's native row-format ALTER machinery. MyLite
does not reinterpret the rebuilt table contents; it proves no-live ownerless
recovery rebuilds volatile coordination around the completed native rebuild.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-row-format-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata shows `ROW_FORMAT = 'Dynamic'` through InnoDB and SQL
  information schema.
- Retained row payloads and aggregate values survive recovery.
- Post-recovery writes succeed.
- Ownerless and ordinary native reopen observe the same row-format state before
  and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic ordinary row-format crash coverage, not the full
  storage-option matrix.
- Compressed row-format and `KEY_BLOCK_SIZE` crash recovery remain separate
  candidate slices because they add compressed-page evidence and stricter
  native storage constraints.
- Full external MariaDB/RQG long-running stress remains planned.

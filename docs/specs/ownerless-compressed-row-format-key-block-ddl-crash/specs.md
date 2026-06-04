# Ownerless Compressed Row-Format Key-Block DDL Crash

## Problem Statement

Ownerless compressed row-format key-block coverage proves an already-open peer
refreshes after another ownerless process rebuilds an InnoDB table with
`ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4`, and verifies 4 KiB compressed BLOB
page evidence after reopen. The crash matrix does not yet kill that writer
after MariaDB/InnoDB completes the native compressed rebuild but before MyLite
publishes ownerless dictionary finish.

This slice adds crash-boundary evidence for that focused key-block rebuild path.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:5844` through
  `mariadb/sql/sql_yacc.yy:5848` parses `ROW_FORMAT` table options and marks
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_yacc.yy:5904` through
  `mariadb/sql/sql_yacc.yy:5907` parses `KEY_BLOCK_SIZE` and records the key
  block size.
- `mariadb/sql/sql_table.cc:11114` through
  `mariadb/sql/sql_table.cc:11123` preserves the old row type when ALTER has no
  explicit row type and marks explicit ALTER row types as
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/storage/innobase/handler/handler0alter.cc:1541` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1556` requires an InnoDB
  table rebuild when ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11257` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11308` accepts compressed
  key-block values including `4` and validates file-per-table requirements.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11776` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11831` maps compressed row
  format and key-block options to native compressed record format state.
- `mariadb/storage/innobase/include/fil0fil.h:1318` through
  `mariadb/storage/innobase/include/fil0fil.h:1321` defines native
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` compressed BLOB pages.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database and create
  `app.ownerless_compressed_row_format_kb4` as an InnoDB table with
  `ROW_FORMAT=DYNAMIC`,
- insert deterministic prepared `LONGBLOB` rows large enough to produce native
  4 KiB compressed external-value pages after rebuild,
- start a live ownerless peer so crashed-writer cleanup remains busy,
- start a writer that executes
  `ALTER TABLE app.ownerless_compressed_row_format_kb4 ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4`
  under the existing `dictionary-before-finish` hook,
- kill the writer at the hook after native MariaDB/InnoDB DDL completes but
  before ownerless dictionary finish,
- prove an ownerless opener returns `MYLITE_BUSY` while the live peer remains,
- release the peer and reopen ownerless read/write to rebuild volatile
  coordination,
- verify recovered metadata exposes `INNODB_SYS_TABLES.ROW_FORMAT =
  'Compressed'` and `information_schema.tables.row_format = 'Compressed'`,
- verify retained prepared BLOB rows and aggregate values, then insert another
  prepared BLOB row,
- verify the final compressed metadata, row data, and 4 KiB native ZBLOB page
  evidence survive ownerless reopen, native exclusive reopen, forced `.shm`
  rebuild, and native exclusive reopen after rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for a completed
  `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4` rebuild,
- live-peer cleanup-busy behavior and no-live rebuild,
- ownerless/native reopen of recovered compressed row-format metadata, retained
  prepared BLOB rows, 4 KiB ZBLOB page evidence, and post-recovery writes.

Out of scope:

- exhaustive `KEY_BLOCK_SIZE` crash variants for 1 KiB, 2 KiB, 8 KiB, or
  16 KiB in this selector,
- redundant row-format, encryption, page compression, external-directory,
  partition, tablespace import/discard, or general tablespace variants,
- randomized DDL oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for existing
ownerless compressed key-block compatibility by proving that a writer death at
MyLite's dictionary publication boundary preserves the completed native
compressed rebuild and 4 KiB long-value page state.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises existing file-per-table native storage,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

The covered DDL uses MariaDB/InnoDB's native compressed row-format
`KEY_BLOCK_SIZE=4` ALTER machinery. MyLite does not reinterpret compressed
native table pages; it proves no-live ownerless recovery rebuilds volatile
coordination around the completed native rebuild and preserves native ZBLOB page
evidence.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-compressed-row-format-key-block-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata shows `ROW_FORMAT = 'Compressed'` through InnoDB and SQL
  information schema.
- Retained prepared BLOB rows and aggregate values survive recovery.
- Native 4 KiB compressed ZBLOB page evidence remains present after recovery.
- Post-recovery writes succeed.
- Ownerless and ordinary native reopen observe the same compressed row-format
  state before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic `KEY_BLOCK_SIZE=4` compressed row-format crash coverage,
  not the full compressed storage-option matrix.
- Exhaustive key-block crash coverage and full external MariaDB/RQG long-running
  stress remain planned.

# Ownerless Compressed Row-Format Low Key-Block DDL

## Problem Statement

Ownerless compressed row-format DDL coverage already proves the default 8 KiB
compressed rebuild, focused 4 KiB plus 16 KiB key-block peer refresh, and
focused 4 KiB plus 16 KiB dictionary-boundary crash recovery. The create-time
compressed BLOB matrix also proves 1 KiB and 2 KiB compressed native BLOB pages
are valid in the current embedded profile, but those low key-block values lack
matching ownerless DDL refresh and crash-boundary evidence.

This slice extends the focused key-block DDL handoff and hook crash matrix to
`KEY_BLOCK_SIZE=1` and `KEY_BLOCK_SIZE=2`.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:5844` through
  `mariadb/sql/sql_yacc.yy:5848` parses `ROW_FORMAT` table options and marks
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_yacc.yy:5904` through
  `mariadb/sql/sql_yacc.yy:5907` parses `KEY_BLOCK_SIZE` and records the key
  block size.
- `mariadb/storage/innobase/handler/handler0alter.cc:1541` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1556` requires an InnoDB
  table rebuild when ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11534` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11673` validates compressed
  `KEY_BLOCK_SIZE` values, row-format compatibility, and file-per-table
  requirements.
- `mariadb/storage/innobase/handler/ha_innodb.cc:12048` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:12134` maps compressed
  row-format and key-block options into native InnoDB compressed table state.
- `mariadb/storage/innobase/include/fil0fil.h:1318` through
  `mariadb/storage/innobase/include/fil0fil.h:1321` defines native
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` compressed BLOB pages.

## Design

Extend the existing `compressed-row-format-key-block-ddl` ownerless handoff:

1. The child ownerless process creates
   `app.ownerless_compressed_row_format_kb1` and
   `app.ownerless_compressed_row_format_kb2` with `ROW_FORMAT=DYNAMIC`.
2. Each table receives deterministic prepared `LONGBLOB` rows that already
   produce native compressed external-value pages in create-time matrix tests.
3. The already-open parent verifies `Dynamic` InnoDB metadata and row
   aggregates before the rebuild.
4. The child runs
   `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1` and
   `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=2`.
5. The parent verifies `Compressed` metadata, inserts a third prepared BLOB
   row through each rebuilt table, and checks final aggregates.
6. Reopen assertions verify final metadata and native `ZBLOB`/`ZBLOB2` page
   evidence by scanning the rebuilt `.ibd` file with the requested key-block
   size.

Extend the existing unsafe hook crash harness with two direct selectors:

- `dictionary-compressed-row-format-key-block-1-crash`
- `dictionary-compressed-row-format-key-block-2-crash`

Each selector kills the writer at `dictionary-before-finish` after native
MariaDB/InnoDB compressed rebuild completion and before MyLite ownerless
dictionary publication, then reuses the existing no-live recovery assertions.

## Scope And Non-Goals

In scope:

- peer-refresh coverage for `KEY_BLOCK_SIZE=1` and `KEY_BLOCK_SIZE=2`
  compressed table-copy rebuilds,
- dictionary-boundary crash recovery for `KEY_BLOCK_SIZE=1` and
  `KEY_BLOCK_SIZE=2`,
- retained prepared BLOB rows, post-recovery writes, native compressed metadata,
  and 1 KiB plus 2 KiB ZBLOB page evidence,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- page compression, encryption, table `TABLESPACE`, partition, external
  directory, `DISCARD/IMPORT TABLESPACE`, or general tablespace variants,
- randomized DDL oracle execution or long-running external MariaDB/RQG stress,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens the existing partial
ownerless compressed DDL compatibility claim by proving the low key-block
values that MariaDB accepts for compressed InnoDB tables also survive MyLite
ownerless peer refresh, writer death at the dictionary publication boundary,
and ownerless/native reopen.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable table and compressed BLOB
page state remains in the MyLite database directory. The tests exercise
existing file-per-table native storage, ownerless live-peer cleanup-busy
behavior, no-live coordination rebuild, forced `.shm` rebuild, and native
exclusive reopen.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. MyLite does not reinterpret
compressed table pages; the tests prove ownerless recovery and refresh preserve
the completed native compressed rebuild and page evidence at 1 KiB and 2 KiB
key-block sizes.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The diff is focused on test coverage and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused peer-refresh selector:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test compressed-row-format-key-block-ddl`.
- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused hook selectors:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-compressed-row-format-key-block-1-crash`
  and
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-compressed-row-format-key-block-2-crash`.
- Run adjacent 4 KiB and 16 KiB hook selectors to guard the widened shared
  harness.
- Run focused ownerless embedded/hook/stress verification, production-build
  guards, `format-check`, and `git diff --check`.

## Acceptance Criteria

- The already-open peer observes `Dynamic` metadata before the low-key-block
  rebuild and `Compressed` metadata after it.
- Existing rows remain readable after each rebuild.
- The already-open peer can insert a prepared BLOB row after each rebuild.
- The hook selectors reach `dictionary-before-finish` and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata, retained rows, post-recovery writes, and native 1 KiB
  plus 2 KiB ZBLOB page evidence survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic low key-block DDL coverage, not the full compressed
  storage-option matrix.
- External MariaDB/RQG long-running stress remains planned.

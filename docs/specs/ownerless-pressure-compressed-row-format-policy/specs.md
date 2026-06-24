# Ownerless Pressure Compressed Row-Format Policy

## Problem Statement

Ownerless active-reader pressure coverage already blocks representative DML,
dictionary DDL, storage rebuilds, ordinary `ROW_FORMAT=DYNAMIC`, and separate
compressed row-format peer-refresh/crash slices. The pressure-policy selector
still lacked a compressed `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE` ALTER, leaving
a small gap between compressed DDL correctness evidence and WAL-pressure
throttling evidence.

This slice extended the existing `active-reader-pressure-write-policy`
selector with one representative compressed key-block rebuild at
`KEY_BLOCK_SIZE=8`. The follow-up
`ownerless-pressure-compressed-key-block-matrix` slice now broadens the same
pressure evidence to the MariaDB-accepted `KEY_BLOCK_SIZE=1`, `2`, `4`, `8`,
and `16` set.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `ROW_FORMAT` and `KEY_BLOCK_SIZE` table
  options into ALTER TABLE metadata.
- `mariadb/storage/innobase/handler/handler0alter.cc:1541` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1556` requires an InnoDB
  table rebuild when ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11534` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11673` validates compressed
  row-format and key-block compatibility, including the accepted
  `KEY_BLOCK_SIZE` values and file-per-table requirement.
- `mariadb/storage/innobase/include/fil0fil.h:1318` through
  `mariadb/storage/innobase/include/fil0fil.h:1321` defines native compressed
  external-value `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` pages.
- `packages/libmylite/src/database.cc:14374` runs
  `enforce_ownerless_page_log_limit_policy()` before ownerless statement
  locking, dictionary refresh, or native MariaDB execution for SQL statements
  classified as writes.

## Design

Reuse the retained-WAL setup in
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create `app.ownerless_pressure_compressed_row_format_variant` as
   `ROW_FORMAT=DYNAMIC`; the follow-up matrix creates companion `kb1`, `kb2`,
   `kb4`, and `kb16` tables with the same starting state.
2. Insert two prepared `LONGBLOB` rows into each table using the existing
   deterministic compressed BLOB payload helper.
3. Hold a repeatable-read snapshot in a peer ownerless process and retain
   page-version WAL at the configured soft limit.
4. Assert each
   `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=<n>` in the covered
   key-block set returns `MYLITE_BUSY` with the pressure-limit diagnostic.
5. Verify each blocked statement leaves InnoDB/table row-format metadata at
   `Dynamic` and leaves row, value, and payload aggregates unchanged.
6. Release the reader, run the same ALTERs successfully, insert a third
   prepared BLOB row into each rebuilt table, and verify compressed metadata,
   final aggregates, and native `ZBLOB`/`ZBLOB2` page evidence.
7. Reuse the final pressure-policy reopen helper so ownerless/native reopen
   before and after forced `.shm` rebuild prove the compressed final state.

## Scope And Non-Goals

In scope:

- Compressed table-copy rebuild pressure-policy cases at
  `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1`, `2`, `4`, `8`, and `16`, with the
  original `KEY_BLOCK_SIZE=8` representative retained as the stable table name.
- Blocked-state metadata and aggregate assertions.
- Post-release success, prepared BLOB write, compressed metadata, native
  compressed-page evidence, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Exhaustive compressed storage-option cross products beyond the bounded
  `KEY_BLOCK_SIZE=1`/`2`/`4`/`8`/`16` pressure-policy matrix.
- Page compression, encryption, table `TABLESPACE`, partition, external
  directory, or `DISCARD/IMPORT TABLESPACE` variants.
- Production SQL classifier changes unless the focused selector exposes a
  bug.
- SQL-level table-lock fault injection and external MariaDB/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice adds evidence that MariaDB-valid
compressed row-format table rebuilds in the bounded key-block matrix are
throttled before native InnoDB metadata or file state changes while retained
page-version WAL is at the configured ownerless pressure limit.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The selector exercises native
file-per-table InnoDB storage inside the MyLite database directory, retained
ownerless page-version WAL, checkpoint cleanup after reader release, and forced
shared-memory rebuild of final state.

## Native Storage Impact

Native InnoDB remains responsible for the compressed rebuild after pressure
clears. MyLite does not reinterpret compressed pages; the final scan only
verifies native `ZBLOB`/`ZBLOB2` evidence in the rebuilt `.ibd` file.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The diff is test and documentation coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused production selector:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test active-reader-pressure-write-policy`.
- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused hook-build selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test active-reader-pressure-write-policy`.
- Run adjacent ownerless pressure trace/stress checks.
- Run production-build guards, `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Each compressed ALTER returns `MYLITE_BUSY` while an active reader pins
  retained WAL at the configured ownerless pressure limit.
- The blocked ALTERs leave their tables in `Dynamic` row format with unchanged
  rows and payload bytes.
- After reader release, the same ALTERs succeed and final state shows
  compressed metadata, retained/prepared payload rows, native compressed BLOB
  pages for each covered key-block size, ownerless/native reopen, and forced
  `.shm` rebuild.

## Risks And Follow-Up

- This is a bounded key-block pressure matrix, not an exhaustive compressed
  storage-option matrix.
- Broader DDL/file-lifecycle recovery, native redo/checkpoint reconciliation,
  SQL table-lock callback fault injection, and external MariaDB/RQG stress
  remain separate ownerless concurrency gaps.

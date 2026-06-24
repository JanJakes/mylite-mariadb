# Ownerless Pressure Compressed Key-Block Matrix

## Problem Statement

Ownerless active-reader pressure already throttles a representative compressed
row-format rebuild at `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`. Recent
compressed row-format refresh and crash slices widened correctness evidence to
MariaDB's full accepted InnoDB compressed key-block set:
`KEY_BLOCK_SIZE=1`, `2`, `4`, `8`, and `16`. The pressure-policy selector
should cover the same key-block set so retained page-version WAL pressure is
proven to stop each rebuild before native metadata or file changes begin.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:5904` through
  `mariadb/sql/sql_yacc.yy:5908` parses table `KEY_BLOCK_SIZE` options into
  `HA_CREATE_USED_KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/handler0alter.cc:1541` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1556` requires an InnoDB
  table rebuild when an ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11550` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11593` accepts compressed
  `KEY_BLOCK_SIZE` values `1`, `2`, `4`, `8`, and `16` when compatible with
  file-per-table and the configured page size.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11603` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11615` validates
  `ROW_FORMAT=COMPRESSED` against temporary-table, read-only-compressed, and
  file-per-table constraints.
- `mariadb/storage/innobase/include/fil0fil.h:1318` through
  `mariadb/storage/innobase/include/fil0fil.h:1321` defines native compressed
  external-value `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` pages.
- `packages/libmylite/src/database.cc:14833` through
  `packages/libmylite/src/database.cc:14852` enforces the ownerless
  page-version WAL pressure limit before write-class SQL reaches normal
  statement execution.

## Design

Broaden
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()` with a
small matrix of compressed row-format rebuild tables:

- `app.ownerless_pressure_compressed_row_format_kb1`
- `app.ownerless_pressure_compressed_row_format_kb2`
- `app.ownerless_pressure_compressed_row_format_kb4`
- `app.ownerless_pressure_compressed_row_format_variant`
- `app.ownerless_pressure_compressed_row_format_kb16`

Each table starts as `ROW_FORMAT=DYNAMIC` with two deterministic prepared
`LONGBLOB` rows. While a repeatable-read ownerless peer pins retained
page-version WAL at the configured pressure limit, each
`ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=<n>` must return
`MYLITE_BUSY` with the pressure-limit diagnostic. The blocked-state checks
must prove metadata stays `Dynamic` and row/value/payload aggregates stay at
their pre-ALTER values.

After reader release, the same ALTERs should succeed for every key-block size.
The test then inserts a third prepared `LONGBLOB` row into each rebuilt table,
verifies compressed metadata and final aggregates, scans each `.ibd` file for
native `ZBLOB`/`ZBLOB2` page evidence at the expected compressed page size, and
reuses the pressure-policy reopen helper to cover ownerless/native reopen
before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Pressure-policy coverage for compressed table-copy rebuilds at
  `KEY_BLOCK_SIZE=1`, `2`, `4`, `8`, and `16`.
- Pre-native busy diagnostics under retained WAL pressure.
- Blocked-state metadata and aggregate assertions.
- Post-release success, prepared BLOB writes, compressed metadata, native
  compressed-page evidence, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Page compression, encryption, table `TABLESPACE`, partition, external
  directory, and `DISCARD/IMPORT TABLESPACE` variants.
- New SQL classifier behavior unless the broadened selector exposes a bug.
- SQL-level table-lock fault injection.
- External MariaDB/RQG randomized pressure execution.

## Compatibility Impact

No SQL behavior is newly enabled. The slice adds evidence that every
MariaDB-accepted compressed key-block rebuild shape in the bounded matrix is
throttled before native InnoDB metadata or file state changes while retained
page-version WAL is at the configured ownerless pressure limit.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The selector exercises native
file-per-table InnoDB storage inside the MyLite database directory, retained
ownerless page-version WAL, checkpoint cleanup after reader release, final
ownerless/native reopen, and forced shared-memory rebuild of final state.

## Native Storage Impact

Native InnoDB remains responsible for the compressed rebuild after pressure
clears. MyLite does not reinterpret compressed pages; the final scan verifies
native `ZBLOB`/`ZBLOB2` evidence in each rebuilt `.ibd` file.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The diff is test and documentation coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused production selector:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test active-reader-pressure-write-policy`.
- Run the focused production case-table route:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_active_reader_pressure_limit_blocks_write_classes`.
- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the focused hook-build selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test active-reader-pressure-write-policy`.
- Run adjacent ownerless stress or embedded ownerless CTest subsets when time
  and flake state allow.
- Run `format-check-prod`, `git diff --check`, and staged diff checks.

## Acceptance Criteria

- All five compressed ALTERs return `MYLITE_BUSY` while an active reader pins
  retained WAL at the configured ownerless pressure limit.
- Each blocked ALTER leaves its table in `Dynamic` row format with unchanged
  rows and payload bytes.
- After reader release, each ALTER succeeds and final state shows compressed
  metadata, retained/prepared payload rows, native compressed BLOB pages,
  ownerless/native reopen, and forced `.shm` rebuild.

## Risks And Follow-Up

- This is a bounded compressed key-block matrix, not a full storage-option
  cross product.
- Broader native redo/checkpoint recovery, DDL/file-lifecycle crash windows,
  SQL table-lock callback fault injection, and external MariaDB/RQG stress
  remain separate ownerless concurrency gaps.

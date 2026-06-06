# Ownerless Compressed Row-Format Key-Block DDL

## Problem

Ownerless compressed row-format DDL coverage proves one rebuild from
`ROW_FORMAT=DYNAMIC` to `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`. The
compressed BLOB pressure matrix now covers create-time 1 KiB, 2 KiB, 4 KiB,
8 KiB, and 16 KiB compressed page sizes, but ownerless DDL still needs focused
evidence that non-default compressed key-block rebuilds refresh already-open
peers and survive native reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ROW_FORMAT` and `KEY_BLOCK_SIZE` table
  options and marks them in `HA_CREATE_INFO`.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `create_table_info_t::create_options_are_invalid()` accepts
  `KEY_BLOCK_SIZE` values including `4`, `8`, and `16` when compressed tables
  are writable and file-per-table is allowed.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `ha_innobase::check_if_incompatible_data()` treats explicit
  `HA_CREATE_USED_ROW_FORMAT` changes and any `HA_CREATE_USED_KEY_BLOCK_SIZE`
  as incompatible in-place data, forcing a table rebuild.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `alter_options_need_rebuild()` treats `ROW_FORMAT` and `KEY_BLOCK_SIZE`
  alter options as rebuild-driving table options.
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` for compressed-table
  external BLOB pages.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector,
  `compressed-row-format-key-block-ddl`.
- Create two `ROW_FORMAT=DYNAMIC` InnoDB tables with deterministic prepared
  `LONGBLOB` payloads.
- Rebuild them from a peer process with
  `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4` and
  `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16`.
- Verify an already-open ownerless peer observes the transition to
  `Compressed`, reads existing rows, and inserts another prepared BLOB row.
- Verify final rows, metadata, and 4 KiB plus 16 KiB native compressed BLOB
  page evidence through ownerless/native reopen before and after forced `.shm`
  rebuild.

Out of scope:

- Exhaustive `KEY_BLOCK_SIZE` DDL coverage for 1 KiB and 2 KiB.
- Redundant row format, page compression, table encryption, and compressed DDL
  option combinations.
- Crash injection during compressed rebuild.
- External MariaDB/RQG DDL oracles.

## Design

The selector mirrors the existing `compressed-row-format-ddl` handoff with two
different compressed page sizes and independent table names:

1. A child ownerless process creates
   `app.ownerless_compressed_row_format_kb4` and
   `app.ownerless_compressed_row_format_kb16` with `ROW_FORMAT=DYNAMIC` and two
   deterministic prepared `LONGBLOB` rows each.
2. The already-open parent verifies `Dynamic` metadata plus row, value, length,
   and first-byte aggregates for both tables.
3. The child runs
   `ALTER TABLE app.ownerless_compressed_row_format_kb4
   ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4` and
   `ALTER TABLE app.ownerless_compressed_row_format_kb16
   ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16`.
4. The parent verifies `Compressed` metadata, inserts a third prepared BLOB
   row into each rebuilt table, and checks final aggregates.
5. Reopen assertions verify final metadata and native `ZBLOB`/`ZBLOB2` page
   evidence by scanning the `.ibd` files at 4 KiB and 16 KiB page boundaries.

## Compatibility Impact

SQL behavior is unchanged. The slice strengthens partial ownerless DDL
evidence for compressed InnoDB table-option rebuilds while keeping exhaustive
compressed DDL option matrices planned.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises the native file-per-table
`.ibd` rebuild inside the MyLite database directory and the existing ownerless
concurrency files.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. The test verifies that the final
rebuilt table uses compressed native long-value pages at the requested key
block size.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `compressed-row-format-key-block-ddl`.
- Run adjacent DDL selectors: `compressed-row-format-ddl`, `row-format-ddl`,
  `online-ddl-options`, and `compressed-blob-key-block-matrix`.
- Build and run focused `compressed-row-format-key-block-ddl` in
  `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL label, hook ownerless SQL
  subset, ownerless stress, `format-check`, `git diff --check`, and cached
  diff checks before commit.

## Acceptance Criteria

- The already-open peer observes `Dynamic` metadata before the rebuild and
  `Compressed` metadata after the `KEY_BLOCK_SIZE=4` and `KEY_BLOCK_SIZE=16`
  rebuilds.
- Existing rows remain readable after the compressed rebuild.
- The already-open peer can insert a prepared BLOB row after the rebuild.
- Final rows, compressed metadata, and 4 KiB plus 16 KiB `ZBLOB`/`ZBLOB2` page
  evidence survive ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Follow-Up

- This is one additional compressed key-block DDL rebuild shape, not an
  exhaustive compressed table DDL matrix.
- Crash injection, encryption, page compression, SQL-level table-lock fault
  injection, and external MariaDB/RQG DDL stress remain separate gaps.

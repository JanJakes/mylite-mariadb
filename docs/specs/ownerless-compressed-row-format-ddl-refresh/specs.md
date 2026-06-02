# Ownerless Compressed Row Format DDL Refresh

## Problem

Ownerless row-format DDL coverage proves a `ROW_FORMAT=COMPACT` table can be
rebuilt as `ROW_FORMAT=DYNAMIC` and remain visible to an already-open peer.
The remaining compressed-table variant is a distinct InnoDB table option path:
`ROW_FORMAT=COMPRESSED` depends on `KEY_BLOCK_SIZE`, creates compressed native
pages, and uses compressed external BLOB page types when large values are
stored off page.

MyLite needs focused evidence that an ownerless peer observes a compressed
row-format rebuild, can continue writing through the rebuilt table, and that
the final compressed native table survives ownerless/native reopen before and
after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ROW_FORMAT` and `KEY_BLOCK_SIZE` table
  options for `CREATE TABLE` and `ALTER TABLE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` validates
  `ROW_FORMAT=COMPRESSED`, valid `KEY_BLOCK_SIZE` values, and file-per-table
  requirements.
- `mariadb/storage/innobase/handler/handler0alter.cc` treats explicit row
  format and key-block-size options as rebuild-driving alter options.
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` for compressed-table
  external BLOB pages.
- `information_schema.INNODB_SYS_TABLES.ROW_FORMAT` and
  `information_schema.TABLES.ROW_FORMAT` expose the native compressed row
  format after the rebuild.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector, `compressed-row-format-ddl`.
- Create an InnoDB table with `ROW_FORMAT=DYNAMIC` and deterministic prepared
  `LONGBLOB` payloads.
- Rebuild that table from another ownerless process with
  `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`.
- Verify an already-open ownerless peer observes the row-format transition to
  `Compressed`, reads existing rows, and inserts a new prepared BLOB row after
  the rebuild.
- Verify final rows, metadata, and native compressed BLOB page evidence through
  ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Every `KEY_BLOCK_SIZE`, compressed table DDL option combinations, redundant
  row format, page compression, encryption, crash injection during compressed
  rebuild, durable DDL file-lifecycle metadata for every DDL class, and
  external randomized DDL oracles.

## Design

The selector mirrors the existing row-format DDL handoff:

1. A child ownerless process creates
   `app.ownerless_compressed_row_format_base` with `ROW_FORMAT=DYNAMIC` and
   inserts two rows through prepared binary BLOB bindings.
2. The already-open parent verifies `INNODB_SYS_TABLES.ROW_FORMAT = 'Dynamic'`
   and reads row count, value sum, BLOB length, and first-byte aggregates.
3. The child runs
   `ALTER TABLE app.ownerless_compressed_row_format_base
   ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`.
4. The parent verifies `INNODB_SYS_TABLES.ROW_FORMAT = 'Compressed'`,
   `information_schema.TABLES.ROW_FORMAT = 'Compressed'`, inserts a third
   prepared BLOB row, and checks final aggregates.
5. Reopen helper assertions verify final compressed metadata, rows, and
   `FIL_PAGE_TYPE_ZBLOB`/`ZBLOB2` page presence through ownerless/native
   reopen before and after forced `.shm` rebuild.

No production change is expected if existing ownerless DDL generation refresh
and native file lifecycle handling already cover this rebuild path.

## Compatibility Impact

SQL behavior is unchanged. The slice strengthens partial ownerless DDL
compatibility for compressed InnoDB table-option rebuilds while keeping the
broader compressed DDL matrix and durable lifecycle metadata gaps explicit.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises the existing native
file-per-table `.ibd` file and ownerless concurrency files inside the MyLite
database directory.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. The test verifies compressed
native page evidence after the rebuild instead of adding MyLite storage-format
logic.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `compressed-row-format-ddl`.
- Run adjacent DDL selectors: `row-format-ddl`, `charset-convert-ddl`,
  `table-comment-ddl`, `force-rebuild-ddl`, and `compressed-blob-page-pressure`.
- Build and run focused `compressed-row-format-ddl` in `ownerless-test-hooks`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The already-open peer observes the initial `Dynamic` row format and the
  rebuilt `Compressed` row format.
- Existing rows remain readable after the compressed rebuild.
- The already-open peer can insert a prepared BLOB row after the rebuild.
- Final rows, compressed metadata, and ZBLOB page evidence survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This is one compressed row-format rebuild shape, not a full compressed table
  DDL matrix.
- Crash injection during compressed rebuild, durable DDL file-lifecycle
  metadata for every native DDL class, SQL-level table-lock fault injection,
  and external MariaDB/RQG DDL stress remain separate gaps.

# Ownerless Compressed Row-Format 16K Key-Block DDL

## Problem

Ownerless compressed row-format DDL refresh coverage already includes the
default 8 KiB compressed rebuild and a non-default 4 KiB key-block rebuild.
The create-time compressed BLOB matrix proves 16 KiB compressed native BLOB
pages are valid in the current embedded profile, but the DDL refresh path lacks
matching evidence that an already-open ownerless peer observes a 16 KiB
compressed table-option rebuild and that the final native table survives
reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ROW_FORMAT` and `KEY_BLOCK_SIZE` table
  options for `CREATE TABLE` and `ALTER TABLE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `create_table_info_t::create_options_are_invalid()` accepts
  `KEY_BLOCK_SIZE=16` for compressed InnoDB tables when compressed tables are
  writable and file-per-table is allowed.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `ha_innobase::check_if_incompatible_data()` treats explicit row-format and
  key-block-size changes as incompatible in-place data.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `alter_options_need_rebuild()` treats `ROW_FORMAT` and `KEY_BLOCK_SIZE`
  alter options as rebuild-driving table options.
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` for compressed-table
  external BLOB pages.

## Scope And Non-Goals

In scope:

- Extend the focused ownerless SQL selector,
  `compressed-row-format-key-block-ddl`.
- Add a second `ROW_FORMAT=DYNAMIC` InnoDB table with deterministic prepared
  `LONGBLOB` payloads.
- Rebuild that table from a peer process with
  `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16`.
- Verify an already-open ownerless peer observes the transition to
  `Compressed`, reads retained rows, and inserts another prepared BLOB row.
- Verify final rows, metadata, and 16 KiB native compressed BLOB page evidence
  through ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Exhaustive 1 KiB and 2 KiB compressed row-format DDL refresh coverage.
- Page compression, encryption, tablespace options, partitioned tables, and
  external MariaDB/RQG DDL oracles.

## Design

The existing `compressed-row-format-key-block-ddl` handoff gains a second table
next to the 4 KiB path:

1. The child ownerless process creates
   `app.ownerless_compressed_row_format_kb16` with `ROW_FORMAT=DYNAMIC` and
   two deterministic prepared `LONGBLOB` rows.
2. The already-open parent verifies `Dynamic` metadata plus row, value, length,
   and first-byte aggregates.
3. The child runs
   `ALTER TABLE app.ownerless_compressed_row_format_kb16
   ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16`.
4. The parent verifies `Compressed` metadata, inserts a third prepared BLOB
   row, and checks final aggregates.
5. Reopen assertions verify final metadata and native `ZBLOB`/`ZBLOB2` page
   evidence by scanning the `.ibd` file at 16 KiB page boundaries.

## Compatibility Impact

SQL behavior is unchanged. The slice strengthens partial ownerless DDL
evidence for compressed InnoDB table-option rebuilds while keeping exhaustive
compressed DDL matrices and crash expansion planned.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises the native file-per-table
`.ibd` rebuild inside the MyLite database directory and the existing ownerless
concurrency files.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. The test verifies that the final
rebuilt table uses compressed native long-value pages at the requested 16 KiB
key-block size.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `compressed-row-format-key-block-ddl`.
- Build and run focused `compressed-row-format-key-block-ddl` in
  `ownerless-test-hooks`.
- Run the relevant ownerless cross-process SQL shards, adjacent ownerless DDL
  stress, `format-check`, and `git diff --check`.

## Acceptance Criteria

- The already-open peer observes `Dynamic` metadata before the 16 KiB rebuild
  and `Compressed` metadata after it.
- Existing rows remain readable after the compressed rebuild.
- The already-open peer can insert a prepared BLOB row after the rebuild.
- Final rows, compressed metadata, and 16 KiB `ZBLOB`/`ZBLOB2` page evidence
  survive ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This is 16 KiB refresh coverage. Hook-build crash injection for the same
  key-block size is covered separately by
  `ownerless-compressed-row-format-key-block-16-ddl-crash`.
- 1 KiB and 2 KiB compressed row-format DDL refresh, storage option
  combinations, SQL-level table-lock fault injection, and external MariaDB/RQG
  DDL stress remain separate gaps.

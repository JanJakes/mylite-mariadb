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

- Exhaustive `KEY_BLOCK_SIZE`, compressed table DDL option combinations,
  redundant row format, page compression, encryption, crash injection during
  compressed rebuild, durable DDL file-lifecycle metadata for every DDL class,
  and external randomized DDL oracles. Focused `KEY_BLOCK_SIZE=4` and
  `KEY_BLOCK_SIZE=16` compressed rebuilds are covered separately by
  `ownerless-compressed-row-format-key-block-ddl`.

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

The compressed rebuild exposed a production refresh boundary beyond the test
selector. A `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` rebuild can change the
tablespace page-0 flags and physical page size observed through the already-open
peer's `fil_space_t`, while older page-version WAL records remain keyed only by
`(space_id, page_no)`. After such a table-copy rebuild, a stale page-version
record from the old table image must not be overlaid onto the rebuilt compressed
table, because old clustered pages can still reference now-invalid compressed
BLOB continuation pages.

The peer refresh path therefore handles dictionary-generation changes before
the statement's page-version read decision. When a generation change is
observed, the handle releases any page-version pin, closes the current InnoDB
read view, clears external page observations, resets its page-version/native
read watermarks, refreshes external space headers, and evicts clean external
pages. Space-header refresh reads page 0 with up to `UNIV_PAGE_SIZE_MAX` bytes
and derives the new physical page size from the refreshed flags before updating
the native `fil_space_t` and file-page counts. The handle then stays in a
conservative native-read mode for subsequent post-peer-DDL statements instead
of immediately re-enabling page-version reads over table pages whose rebuild
generation is not encoded in the page-version WAL key.

## Compatibility Impact

SQL behavior is unchanged. The slice strengthens partial ownerless DDL
compatibility for compressed InnoDB table-option rebuilds by making already-open
peers refresh dictionary and space-header state before using the rebuilt table.
It also explicitly trades some post-peer-DDL page-version acceleration for
correct native reads until page-version invalidation can become
rebuild-generation-aware.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises the existing native
file-per-table `.ibd` file and ownerless concurrency files inside the MyLite
database directory.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. MyLite refreshes the native
InnoDB space header from the rebuilt page 0 and avoids applying stale
page-version table-page images across the dictionary-generation boundary. The
test verifies compressed native page evidence after the rebuild instead of
adding MyLite storage-format logic.

## Public API Impact

No public API changes.

## Binary Size Impact

The production code change is in existing ownerless refresh paths and does not
add dependencies or new public entry points.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `php-embedded-prod`.
- Run focused `sql-case
  test_ownerless_compressed_row_format_ddl_refreshes_peer_dictionary`.
- Run focused `compressed-row-format-ddl` and
  `compressed-row-format-key-block-ddl`.
- Run adjacent DDL selectors: `row-format-ddl`, `charset-convert-ddl`,
  `table-comment-ddl`, `force-rebuild-ddl`, and `compressed-blob-page-pressure`.
- Build and run focused compressed row-format crash selectors in
  `ownerless-test-hooks` when the hook preset is available.
- Run `format-check`, production-build guard scripts, `git diff --check`, and
  cached diff checks.

## Acceptance Criteria

- The already-open peer observes the initial `Dynamic` row format and the
  rebuilt `Compressed` row format.
- Existing rows remain readable after the compressed rebuild.
- The already-open peer can insert a prepared BLOB row after the rebuild.
- Final rows, compressed metadata, and ZBLOB page evidence survive
  ownerless/native reopen before and after forced `.shm` rebuild.
- A dictionary-generation refresh updates space-header page-size metadata and
  prevents stale pre-rebuild page-version records from being overlaid on the
  rebuilt compressed table.

## Risks And Follow-Up

- This is one compressed row-format rebuild shape, not a full compressed table
  DDL matrix. Focused `KEY_BLOCK_SIZE=4` and `KEY_BLOCK_SIZE=16` compressed
  rebuilds are covered separately by
  `ownerless-compressed-row-format-key-block-ddl`.
- Crash injection during compressed rebuild, durable DDL file-lifecycle
  metadata for every native DDL class, SQL-level table-lock fault injection,
  full external replay of compressed row-format DDL traces, and external
  MariaDB/RQG DDL stress remain separate gaps.
- Conservative native reads after peer DDL are intentionally broader than the
  compressed-row-format case. A future per-space or per-table rebuild-generation
  stamp in the page-version index could recover more page-version read
  acceleration without allowing old table images to cross rebuild boundaries.

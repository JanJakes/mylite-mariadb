# Ownerless Compressed Row-Format 16K Key-Block DDL Crash

## Problem

Ownerless compressed row-format DDL refresh now proves
`ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16` refreshes already-open
peers, preserves prepared `LONGBLOB` rows, and leaves native 16 KiB compressed
BLOB page evidence after reopen. Crash recovery still needs focused evidence
that a writer killed after native InnoDB completes that 16 KiB compressed
rebuild but before MyLite dictionary finish recovers to the completed native
state.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ROW_FORMAT` and `KEY_BLOCK_SIZE` table
  options for `ALTER TABLE`.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `alter_options_need_rebuild()` treats `ROW_FORMAT` and `KEY_BLOCK_SIZE`
  alter options as rebuild-driving table options.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `create_table_info_t::create_options_are_invalid()` accepts
  `KEY_BLOCK_SIZE=16` for compressed InnoDB tables when compressed tables are
  writable and file-per-table is allowed.
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` for compressed-table
  external BLOB pages.

## Scope And Non-Goals

In scope:

- Add a hook-build selector,
  `dictionary-compressed-row-format-key-block-16-crash`.
- Create `app.ownerless_compressed_row_format_kb16` as
  `ROW_FORMAT=DYNAMIC` with deterministic prepared `LONGBLOB` rows.
- Kill the writer at `dictionary-before-finish` after it executes
  `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16`.
- Verify live-peer cleanup remains busy, no-live ownerless recovery observes
  the completed compressed rebuild, and post-recovery prepared BLOB writes
  succeed.
- Verify ownerless/native reopen before and after forced `.shm` rebuild plus
  native 16 KiB ZBLOB page evidence.

Out of scope:

- 1 KiB and 2 KiB compressed key-block crash variants.
- Page compression, encryption, tablespace options, partitioned tables,
  SQL-level table-lock fault injection, and external MariaDB/RQG DDL oracles.

## Design

Reuse the existing compressed key-block crash choreography through a
parameterized helper:

1. Set up the 16 KiB table as `ROW_FORMAT=DYNAMIC` with two deterministic
   prepared BLOB rows.
2. Keep a live ownerless peer open while a writer executes the 16 KiB
   compressed rebuild under `dictionary-before-finish`.
3. Kill the writer at the hook and verify another ownerless open returns
   `MYLITE_BUSY` while the live peer remains.
4. Release the peer, reopen ownerless read/write to rebuild volatile
   coordination, verify compressed metadata and retained rows, then insert a
   third prepared BLOB row.
5. Verify the final state through ownerless and native reopen before and after
   forced `.shm` rebuild, including 16 KiB ZBLOB page evidence.

## Compatibility Impact

No SQL behavior changes. The slice strengthens partial ownerless DDL crash
coverage for an already-supported compressed InnoDB table-option rebuild.

## Directory And Lifecycle Impact

No directory layout changes. Durable state remains in the MyLite database
directory; the selector exercises native file-per-table rebuild recovery,
ownerless live-peer cleanup gating, no-live rebuild, and forced shared-memory
rebuild.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. MyLite verifies recovered native
metadata and compressed BLOB page evidence instead of interpreting or rewriting
compressed table pages itself.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused hook test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused `dictionary-compressed-row-format-key-block-crash`.
- Run focused `dictionary-compressed-row-format-key-block-16-crash`.
- Run the relevant hook ownerless SQL shard or hook crash-tail selector.
- Run adjacent embedded refresh selector, `format-check`, and
  `git diff --check`.

## Acceptance Criteria

- The 16 KiB focused selector reaches the dictionary fault hook and does not
  hang.
- Live-peer cleanup remains busy until the live peer exits.
- Recovered metadata shows `ROW_FORMAT = 'Compressed'`.
- Retained prepared BLOB rows and post-recovery writes survive ownerless/native
  reopen before and after forced `.shm` rebuild.
- Native 16 KiB compressed ZBLOB page evidence remains present after recovery.

## Risks And Follow-Up

- This is deterministic 16 KiB crash coverage, not an exhaustive compressed
  storage-option matrix.
- 1 KiB and 2 KiB key-block crash coverage and long-running external
  MariaDB/RQG stress remain planned.

# Ownerless Created Tablespace Replay

## Problem Statement

Ownerless stale-reader rebuild coverage now includes dropped, renamed,
truncated, force-rebuilt, multi-renamed, and schema-dropped file-per-table
states. Those selectors cover tablespaces that existed before the stale reader
published its snapshot pin.

MyLite also needs focused evidence for the opposite final-state shape: a native
InnoDB file-per-table table created while a stale repeatable-read ownerless
reader pins page-version WAL. A later no-live ownerless rebuild must preserve
the created table's `.frm`, `.ibd`, metadata, and rows instead of treating
retained reader-boundary WAL as stale native file lifecycle truth.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `mysql_create_table()` obtains the metadata locks for `SQLCOM_CREATE_TABLE`
    and delegates execution to `mysql_create_table_no_lock()`.
  - `mysql_create_table_no_lock()` builds the table path and calls
    `create_table_impl()`.
  - `create_table_impl()` writes the `.frm` image and calls
    `ha_create_table()` for non-temporary engine creation.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::create()` initializes InnoDB create-table state, starts the
    DDL transaction when it owns the transaction, locks the data dictionary,
    creates the table definition and indexes, commits, writes redo through the
    commit LSN, and updates the in-memory dictionary cache.
  - `create_table_info_t::create_table_def()` builds the InnoDB dictionary
    table definition and calls `row_create_table_for_mysql()` for durable
    non-temporary tables.
- `mariadb/storage/innobase/row/row0mysql.cc`
  - `row_create_table_for_mysql()` runs the InnoDB create-table execution graph
    under a dictionary transaction and rolls back on error.
- `mariadb/storage/innobase/dict/dict0crea.cc` and
  `mariadb/storage/innobase/fil/fil0fil.cc`
  - The native create path builds dictionary rows, initializes the new
    tablespace header, logs `FILE_CREATE`, and creates the file-per-table
    `.ibd`.
- `packages/libmylite/src/database.cc`
  - No-live stale-reader rebuilds checkpoint retained reader-boundary WAL
    instead of replaying it when the remaining shared-memory state is
    read-view/page-pin evidence without native writer recovery evidence.
- `packages/libmylite/src/ownerless_tablespace_replay.cc`
  - Product page replay resolves existing native tablespaces by InnoDB page-0
    space id and skips missing tablespaces only in product recovery mode.

## Design

Add a focused ownerless SQL selector,
`created-tablespace-replay`, beside the existing stale-reader replay
selectors:

1. Initialize a MyLite directory and assert the page-version WAL is
   checkpointed.
2. Start a peer repeatable-read snapshot pin before the new table exists.
3. Create `app.ownerless_created_replay` as an InnoDB file-per-table table.
4. Insert and update large payload rows so retained WAL includes dirty pages
   for the newly created tablespace.
5. Verify the writer closes while the stale reader keeps retained WAL live.
6. Kill the stale reader and verify ownerless/native reopen, forced `.shm`
   rebuild, and native reopen all preserve the created table metadata, native
   files, row counts, aggregates, and checkpointed WAL.

## Scope

In scope:

- Product SQL evidence for no-live stale-reader rebuild after an InnoDB table
  is created while a stale snapshot pin is active.
- Final-state verification through ownerless and native exclusive reopen before
  and after forced `.shm` rebuild.
- Native `.frm` and `.ibd` presence checks for the created table.

Out of scope:

- Crash injection inside `CREATE TABLE` or file creation.
- Reconstructing a missing created `.ibd` from page-version WAL.
- Durable file lifecycle metadata for every DDL class.
- External MariaDB/RQG oracle execution.
- SQL-level table-lock wait fault injection; prior explored SQL shapes stopped
  before the ownerless table-wait callback.

## Compatibility Impact

SQL behavior is unchanged. The slice expands the current partial ownerless
DDL/file-lifecycle recovery evidence to include a table created under a stale
snapshot pin. Full ownerless DDL/file-lifecycle recovery remains partial until
durable lifecycle metadata, broader native redo/checkpoint reconciliation, and
external oracle stress exist.

## DDL Metadata Routing Impact

The selector uses MariaDB's existing `CREATE TABLE` routing and MyLite's
ownerless dictionary generation boundary. It verifies that metadata for the
new table is present after no-live stale-reader rebuild and after forced
volatile `.shm` recreation.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes the existing
`datadir/app/ownerless_created_replay.frm`,
`datadir/app/ownerless_created_replay.ibd`,
`concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

No storage format changes. MariaDB's native created `.frm` and InnoDB
file-per-table `.ibd` remain the final storage authority when no live writer
recovery evidence exists. Retained reader WAL is checkpointed during no-live
stale-reader rebuild instead of overriding the created native file state.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds SQL test coverage and
documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `created-tablespace-replay` in `embedded-dev`.
- Run adjacent stale-reader replay selectors in `embedded-dev`.
- Build and run the same focused and adjacent selectors in
  `ownerless-test-hooks`.
- Run relevant ownerless stress and hygiene checks: `format-check`,
  `git diff --check`, cached diff checks, and temp/process cleanup checks.

## Acceptance Criteria

- The created table's `.frm` and `.ibd` are absent before the stale reader
  starts and present after `CREATE TABLE`.
- Retained page-version WAL remains after the writer closes while the stale
  reader pin is live.
- After killing the reader, ownerless reopen succeeds and checkpoints retained
  reader-boundary WAL.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the created table, 16 rows, `SUM(id)=136`, `SUM(value)=1376`, and
  64,000 payload bytes.
- Existing baseline table state remains readable.

## Risks And Open Questions

- This proves the final native created-file state when files are present; it
  does not prove reconstruction of a missing created tablespace.
- It remains focused SQL coverage, not a replacement for durable DDL
  lifecycle metadata or crash-in-action external oracle stress.

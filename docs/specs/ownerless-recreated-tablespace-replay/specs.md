# Ownerless Recreated Tablespace Replay

## Problem Statement

Ownerless stale-reader replay coverage now proves dropped, created, renamed,
truncated, force-rebuilt, multi-renamed, and schema-dropped file-per-table
final states. A common remaining DDL file-lifecycle shape is same-name
recreate: one process drops an existing InnoDB table and creates a new table
at the same SQL name while an older ownerless reader keeps retained
page-version WAL alive.

MyLite needs focused evidence that a no-live stale-reader rebuild preserves
the recreated table's final native metadata and rows, and does not replay old
same-name page images over the new tablespace.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `mysql_rm_table()` takes table-name locks for ordinary `DROP TABLE` and
    delegates to `mysql_rm_table_no_locks()`.
  - `mysql_rm_table_no_locks()` removes cached table definitions, records DDL
    log entries, calls `ha_delete_table()`, and removes the `.frm` when engine
    deletion succeeds or the engine table no longer exists.
  - `mysql_create_table()` and `mysql_create_table_no_lock()` route the later
    same-name `CREATE TABLE` through `create_table_impl()`, which writes a new
    `.frm` and calls `ha_create_table()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::delete_table()` loads the InnoDB dictionary table, locks
    child and table state, drops dictionary rows/statistics, commits the DDL
    transaction, closes deleted file handles, and writes redo through the drop
    commit LSN.
  - `ha_innobase::create()` creates the new dictionary table, commits the DDL
    transaction, updates dictionary cache state, and writes redo through the
    create commit LSN.
- `mariadb/storage/innobase/row/row0mysql.cc`
  - `row_create_table_for_mysql()` runs the InnoDB create-table execution
    graph under a dictionary transaction.
- `packages/libmylite/src/database.cc`
  - No-live stale-reader rebuilds checkpoint retained reader-boundary WAL when
    the remaining shared-memory state is stale read-view/page-pin evidence
    without native writer recovery evidence.
- `packages/libmylite/src/ownerless_tablespace_replay.cc`
  - Product page replay resolves existing native tablespaces by InnoDB page-0
    space id and skips unresolved tablespaces only in product recovery mode.

## Design

Add a focused ownerless SQL selector,
`recreated-tablespace-replay`, beside the existing stale-reader replay
selectors:

1. Create an initial InnoDB file-per-table table with large rows, then close
   cleanly so the WAL is checkpointed.
2. Start a peer repeatable-read snapshot pin.
3. Update the initial table while the stale pin is live, then `DROP TABLE`.
4. Recreate the same SQL table name with a different final column shape.
5. Insert and update rows in the recreated table.
6. Verify retained WAL remains while the stale reader is live.
7. Kill the reader and verify ownerless/native reopen, forced `.shm` rebuild,
   and native reopen all observe the recreated table shape and final rows.

## Scope

In scope:

- Product SQL evidence for same-name `DROP TABLE` plus `CREATE TABLE` under a
  stale ownerless snapshot pin.
- Changed final table shape to make stale old-table replay visible.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Crash injection between drop and recreate.
- Reconstructing a missing recreated `.ibd`.
- Durable file lifecycle metadata for every DDL class.
- External MariaDB/RQG oracle execution.
- SQL-level table-lock wait fault injection; prior SQL shapes stopped before
  the ownerless table-wait callback.

## Compatibility Impact

SQL behavior is unchanged. The slice expands partial DDL/file-lifecycle
recovery evidence to include same-name file/dictionary reuse under stale-reader
retained WAL. Full ownerless DDL/file-lifecycle recovery remains partial until
durable lifecycle metadata, broader redo/checkpoint reconciliation, and
external oracle stress exist.

## DDL Metadata Routing Impact

The selector uses MariaDB's native drop and create routing through MyLite's
ownerless DDL generation boundary. The final table adds a `generation` column
that did not exist in the dropped table, so ownerless/native reopen must see
the recreated metadata rather than stale old metadata.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes the existing
`datadir/app/ownerless_recreated_replay.frm`,
`datadir/app/ownerless_recreated_replay.ibd`,
`concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

No storage format changes. MariaDB's final same-name `.frm` and InnoDB `.ibd`
remain the storage authority when no live writer recovery evidence exists.
Retained old-table page-version WAL must not override the recreated native
tablespace state.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds SQL test coverage and
documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `recreated-tablespace-replay` in `embedded-dev`.
- Run adjacent stale-reader replay selectors in `embedded-dev`.
- Build and run the same focused and adjacent selectors in
  `ownerless-test-hooks`.
- Run DDL stress and hygiene checks: `format-check`, `git diff --check`,
  cached diff checks, and cleanup checks.

## Acceptance Criteria

- The initial table exists and checkpoints cleanly before the stale reader
  starts.
- The writer drops and recreates the same SQL table name while retained WAL
  remains live.
- The final table has the recreated `generation` column, 3 rows,
  `SUM(id)=306`, `SUM(value)=2109`, `SUM(generation)=6`, and 12,000 payload
  bytes through ownerless/native reopen before and after forced `.shm` rebuild.
- The page-version WAL is checkpointed after no-live ownerless recovery.

## Risks And Open Questions

- This proves a present-file same-name recreate final state. It does not prove
  crash recovery while the name exists with no matching native `.ibd`.
- The broader durable file lifecycle protocol and external oracle stress remain
  open work.

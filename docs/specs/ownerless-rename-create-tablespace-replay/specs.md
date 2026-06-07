# Ownerless Rename-Create Tablespace Replay

## Problem Statement

Ownerless stale-reader replay coverage proves final states for dropped,
renamed, truncated, recreated, multi-renamed, schema-dropped, and
`CREATE OR REPLACE TABLE` file-per-table objects. A sharper DDL file-lifecycle
shape remains: one process updates an InnoDB table while a stale ownerless
reader pins page-version WAL, renames that original table away, then creates a
different table at the original SQL name.

MyLite needs focused evidence that no-live stale-reader replay preserves both
native tablespaces and does not apply old same-path page versions from the
renamed-away table to the newly-created table at the original name.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` routes SQL `RENAME TABLE` through the storage
    engine rename hook and updates the `.frm` path used for MariaDB metadata.
  - `mysql_create_table()` and `mysql_create_table_no_lock()` route the later
    original-name `CREATE TABLE` through `create_table_impl()`, which writes a
    new `.frm` and calls `ha_create_table()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::rename_table()` delegates user-table rename work to
    `row_rename_table_for_mysql()`.
  - `ha_innobase::create()` prepares and creates a new InnoDB dictionary table
    through `create_table_info_t::prepare_create_table()` and
    `create_table_info_t::create_table()`.
- `mariadb/storage/innobase/row/row0mysql.cc`
  - `row_create_table_for_mysql()` creates an InnoDB table under a dictionary
    transaction.
  - `row_rename_table_for_mysql()` performs InnoDB table rename and dictionary
    cache updates for SQL rename.
- `mariadb/storage/innobase/dict/dict0dict.cc`
  - `dict_table_rename_in_cache()` updates the in-memory dictionary table name
    during InnoDB rename.
- `packages/libmylite/src/database.cc`
  - No-live ownerless startup checkpoints retained reader-boundary WAL when
    the remaining shared-memory state is stale read-view/page-pin evidence and
    no native writer recovery evidence remains.
- `packages/libmylite/src/ownerless_tablespace_replay.cc`
  - Product page replay resolves existing native tablespaces by InnoDB page-0
    space id and skips unresolved tablespaces only in product recovery mode.

## Design

Add a focused ownerless SQL selector,
`rename-create-tablespace-replay`, beside the existing stale-reader replay
selectors:

1. Create `app.ownerless_rename_create_replay` with large rows and a secondary
   index, record its InnoDB `INNODB_SYS_TABLES.SPACE` value, and verify its
   `.ibd` page-0 space id.
2. Close cleanly so the ownerless WAL is checkpointed.
3. Start a peer repeatable-read snapshot pin.
4. Update the original table while the stale pin is live.
5. Rename it to `app.ownerless_rename_create_replay_moved`.
6. Create a different table at `app.ownerless_rename_create_replay`, with a
   new `generation` column and a different secondary index.
7. Insert and update rows in both the moved original table and the new
   original-name table.
8. Assert the moved table still has the original space id, the new
   original-name table has a different nonzero space id, and both `.ibd`
   page-0 space ids match MariaDB dictionary metadata.
9. Verify retained WAL remains while the stale reader is live.
10. Kill the reader and verify ownerless/native reopen, forced `.shm` rebuild,
    and native reopen all observe both final tables, rows, metadata, files, and
    space-id identities.

## Scope

In scope:

- Product SQL evidence for `RENAME TABLE` plus a new same-name `CREATE TABLE`
  under a stale ownerless snapshot pin.
- Distinct final table shapes and indexes so stale metadata or page replay is
  visible.
- Native page-0 space-id evidence that replay keeps the moved original
  tablespace separate from the newly-created original-name tablespace.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Crash injection between rename and create.
- Reconstructing a missing moved or newly-created `.ibd`.
- Durable file-lifecycle metadata for every DDL class.
- External MariaDB/RQG oracle execution.
- SQL-level table-lock wait fault injection; prior SQL shapes stopped before
  the ownerless table-wait callback.

## Compatibility Impact

SQL behavior is unchanged. The slice expands partial DDL/file-lifecycle
recovery evidence to include original-name path reuse while the original
tablespace remains present under a moved name. Full ownerless DDL/file
lifecycle recovery remains partial until durable lifecycle metadata, broader
redo/checkpoint reconciliation, and external oracle stress exist.

## DDL Metadata Routing Impact

The selector uses MariaDB's native rename and create routing through MyLite's
ownerless DDL generation boundary. The final original-name table has a
`generation` column and replacement index that the moved original table does
not have, so ownerless/native reopen must refresh both final metadata states.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes existing files inside the
MyLite database directory:

- `datadir/app/ownerless_rename_create_replay.frm`
- `datadir/app/ownerless_rename_create_replay.ibd`
- `datadir/app/ownerless_rename_create_replay_moved.frm`
- `datadir/app/ownerless_rename_create_replay_moved.ibd`
- `concurrency/mylite-concurrency.wal`
- `concurrency/mylite-concurrency.shm`

## Native Storage Impact

No storage format changes. MariaDB's final `.frm` files, InnoDB `.ibd` files,
and InnoDB dictionary metadata remain the storage authority when no live writer
recovery evidence exists. Retained old-page WAL must not override a different
native tablespace whose path reuses the original table name.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds SQL test coverage and
documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `php-embedded-dev`.
- Run focused `rename-create-tablespace-replay` in `php-embedded-dev`.
- Run adjacent stale-reader replay selectors in `php-embedded-dev`:
  `renamed-tablespace-replay`, `recreated-tablespace-replay`, and
  `multi-rename-tablespace-replay`.
- Run the focused ownerless cross-process CTest shard or direct selector set
  as runtime permits.
- Run `format-check`, `git diff --check`, and cleanup checks before commit.

## Acceptance Criteria

- The original table checkpoints cleanly before the stale reader starts.
- The writer updates the original table, renames it away, creates a different
  table at the original SQL name, and writes both final tables while retained
  WAL remains live.
- The moved table exists with 13 rows, `SUM(id)=792`, `SUM(value)=1922`, and
  52,000 payload bytes, and its `ownerless_rename_create_value_idx` index works.
- The new original-name table exists with the `generation` column, 3 rows,
  `SUM(id)=306`, `SUM(value)=2109`, `SUM(generation)=6`, and 12,000 payload
  bytes, and its `ownerless_rename_create_generation_idx` index works.
- The moved table's InnoDB dictionary `SPACE` equals the original table's
  space id. The new original-name table's `SPACE` is nonzero and different
  from the original table's space id.
- Every final ownerless/native reopen observes `.ibd` page-0 space ids matching
  `INNODB_SYS_TABLES.SPACE` for both final tables.
- The page-version WAL is checkpointed after no-live ownerless recovery.

## Risks And Open Questions

- This proves present-file rename-plus-create final state. It does not prove
  crash recovery while either final `.ibd` is missing or only partially
  created.
- The broader durable file-lifecycle protocol and external oracle stress remain
  open work.

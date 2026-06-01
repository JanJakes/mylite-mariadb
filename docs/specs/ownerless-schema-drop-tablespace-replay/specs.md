# Ownerless Schema Drop Tablespace Replay

## Problem Statement

Ownerless stale-reader rebuild coverage already proves retained reader-boundary
WAL does not resurrect dropped, renamed, or truncated file-per-table tables.
`DROP DATABASE` is a broader file-lifecycle shape: MariaDB enumerates every
table in a schema, drops those native objects, removes schema option files, and
then removes the schema directory.

MyLite needs focused evidence that retained page-version records for a table
inside a dropped schema are treated as stale reader-boundary WAL during no-live
`.shm` rebuild, not as authority to recreate the dropped schema or table.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_db.cc`
  - `mysql_rm_db_internal()` drives `DROP DATABASE`, finds table names in the
    schema directory, locks them, calls `mysql_rm_table_no_locks()`, invokes
    `drop_database_objects()`, removes the schema option file, and removes the
    schema directory with `rm_dir_w_symlink()`.
  - `drop_database_objects()` calls `ha_drop_database()` so storage engines can
    remove database-scoped objects before the directory is deleted.
- `mariadb/sql/handler.cc`
  - `ha_drop_database()` dispatches storage-engine database-drop callbacks.
  - `ha_delete_table()` dispatches each table deletion to the owning engine.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::delete_table()` handles InnoDB table deletion for both
    ordinary `DROP TABLE` and tables dropped as part of `DROP DATABASE`.
- `mariadb/storage/innobase/dict/drop.cc`
  - `trx_t::drop_table()` deletes InnoDB dictionary rows for the table,
    columns, indexes, and fields.
  - `trx_t::commit(std::vector<pfs_os_file_t> &deleted)` removes dropped
    tables from `dict_sys` and calls `fil_delete_tablespace()` for file-backed
    spaces after the dictionary operation commits.
- `mariadb/storage/innobase/fil/fil0fil.cc`
  - `fil_delete_tablespace()` deletes the non-system tablespace and associated
    `.ibd` file.
- `packages/libmylite/src/database.cc`
  - no-live ownerless recovery checkpoints retained reader-boundary WAL before
    rebuilding `.shm` when the remaining volatile state is stale reader
    evidence without native writer recovery evidence.

## Design

Add a focused ownerless SQL selector,
`schema-drop-tablespace-replay`, alongside the existing dropped, renamed, and
truncated tablespace replay selectors:

1. Create a schema and an InnoDB file-per-table table with large rows.
2. Start a peer repeatable-read snapshot pin.
3. Update the schema-owned table while the pin is live so page-version WAL
   retains reader-boundary records.
4. `DROP DATABASE` the schema and verify the schema, table metadata, `.frm`,
   and `.ibd` paths are absent while retained WAL remains.
5. Kill the reader so no live ownerless process remains and `.shm` contains
   stale reader state.
6. Reopen ownerless, ordinary native, forced `.shm` rebuild ownerless, and
   ordinary native again, verifying the schema and table remain absent and the
   WAL is checkpointed.

## Scope

In scope:

- Product SQL evidence for stale-reader no-live rebuild after `DROP DATABASE`
  of a schema that contained an updated InnoDB file-per-table table.
- Directory lifecycle assertions for schema directory, `.frm`, and `.ibd`
  absence.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.
- Compatibility and ownerless-concurrency documentation updates.

Out of scope:

- Durable DDL file-lifecycle metadata for every create, drop, rename,
  truncate, import, discard, partition, or schema edge case.
- SQL-level table-lock wait fault injection; prior explored SQL shapes did not
  reach the ownerless table-wait callback.
- External MariaDB/RQG DDL oracles.
- Background or live-peer checkpoint scheduling changes.

## Compatibility Impact

SQL behavior is unchanged. The slice expands the current partial DDL/file
lifecycle recovery evidence: retained page images for a table in a dropped
schema do not recreate the schema or table during no-live stale-reader rebuild.
Full DDL/file-lifecycle recovery remains partial until durable lifecycle
metadata and broader randomized/oracle coverage exist.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes existing
`datadir/<schema>/`, `datadir/<schema>/*.frm`,
`datadir/<schema>/*.ibd`, `concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

No storage format changes. The final MariaDB native state after
`DROP DATABASE` remains the authority when no live ownerless writer recovery
evidence exists. Retained reader WAL is checkpointed, not replayed, during
no-live stale-reader rebuild.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds SQL test coverage and
documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `schema-drop-tablespace-replay` in `embedded-dev`.
- Run adjacent dropped/renamed/truncated tablespace replay selectors in
  `embedded-dev`.
- Build and run the same focused and adjacent selectors in
  `ownerless-test-hooks`.
- Run the adjacent schema lifecycle selector.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- Retained page-version WAL exists after `DROP DATABASE` while the reader pin
  is live.
- After killing the reader, no-live ownerless reopen succeeds and checkpoints
  retained reader-boundary WAL.
- The dropped schema is absent from `information_schema.schemata`.
- The dropped schema has no `information_schema.tables` rows.
- The schema directory, `.frm`, and `.ibd` paths remain absent.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the same final absent-schema state.

## Risks And Open Questions

- This is focused SQL evidence for one schema-drop shape, not durable
  file-lifecycle metadata for every DDL class.
- MariaDB may change internal `DROP DATABASE` sequencing, so the test is framed
  around observable final schema/table absence, file absence, retained WAL, and
  successful no-live rebuild.

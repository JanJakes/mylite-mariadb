# Ownerless Dropped Tablespace Replay

## Problem Statement

Ownerless no-live recovery treats visible page-version WAL records as the
authority for pages that may have been flushed by a different process. That
replay path can resolve existing single-file InnoDB tablespaces by page-0
space id, but DDL can remove a file-per-table tablespace after earlier page
records for that space are retained by an active snapshot pin.

The current product behavior deliberately skips retained page-version records
whose tablespace no longer exists during no-live replay. That skip needs
focused SQL evidence: a dropped table must remain absent after stale
coordination is rebuilt, and the missing `.ibd` must not make recovery fail.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_DROP_TABLE` dispatches to `mysql_rm_table()`.
- `mariadb/sql/sql_table.cc`
  - `mysql_rm_table()` takes table-name metadata locks for ordinary
    `DROP TABLE`.
  - `mysql_rm_table_no_locks()` calls `ha_delete_table()` after removing the
    table from the table definition cache and recording DDL-log state.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::delete_table()` locks the InnoDB dictionary/table state,
    checks foreign-key blockers, calls `trx_t::drop_table()`, commits the DDL
    transaction, and closes deleted file handles after commit.
- `mariadb/storage/innobase/dict/drop.cc`
  - `trx_t::drop_table()` deletes InnoDB dictionary rows for the table,
    columns, indexes, and fields.
  - `trx_t::commit(std::vector<pfs_os_file_t> &deleted)` removes dropped
    tables from `dict_sys` and calls `fil_delete_tablespace()` for file-backed
    spaces after the dictionary operation commits.
- `mariadb/storage/innobase/fil/fil0fil.cc`
  - `fil_delete_tablespace()` deletes the InnoDB tablespace and associated
    `.ibd` file for non-system tablespaces.
- `packages/libmylite/src/ownerless_tablespace_replay.cc`
  - product replay can apply visible page-version records to existing
    tablespaces and can ignore unresolved tablespaces when called with
    `MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES`.
- `packages/libmylite/src/database.cc`
  - `.shm` rebuild calls product replay with the ignore-missing flag, then
    rebuilds ownerless shared-memory segments from durable WAL/checkpoint
    state.

## Design

Add a focused ownerless SQL selector, `dropped-tablespace-replay`, that:

1. Creates an InnoDB file-per-table table and commits enough rows to create
   user data-page WAL records.
2. Starts a separate ownerless process with a repeatable-read snapshot pin.
3. Updates the table while the snapshot is live, then drops the table.
4. Verifies the dropped table's `.frm` and `.ibd` files are absent while the
   retained page-version WAL is still not checkpointed.
5. Kills the snapshot owner so `.shm` contains stale ownerless state and no
   live process remains.
6. Opens the database ownerless again, forcing no-live recovery/rebuild to
   skip retained records for the missing tablespace.
7. Verifies ownerless and ordinary native exclusive reopens, including after
   forced `.shm` deletion, preserve the final absent-table state and leave the
   WAL checkpointed.

## Scope

In scope:

- Product SQL evidence for no-live replay skipping a missing dropped
  file-per-table tablespace.
- Directory/file lifecycle assertions for `.frm` and `.ibd` absence.
- Compatibility and ownerless-concurrency documentation updates.

Out of scope:

- Durable file lifecycle metadata that can distinguish every create, drop,
  rename, truncate, import, discard, or partition file state.
- Recovery of records for tablespaces moved through unsupported external path
  clauses.
- Changing production replay logic unless the focused test exposes a defect.
- External MariaDB/RQG DDL oracles.

## Compatibility Impact

SQL behavior is unchanged. The slice adds evidence for an existing partial
ownerless guarantee: dropped InnoDB tables remain dropped across no-live
ownerless recovery even when retained WAL still contains records for the
missing tablespace. Full DDL/file-lifecycle recovery remains partial.

## Directory And Lifecycle Impact

No new durable files or directory layout changes are introduced. The test
observes the existing `datadir/app/*.frm`, `datadir/app/*.ibd`,
`concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

The slice relies on MariaDB's native `DROP TABLE` flow to delete the
file-per-table `.ibd` after the InnoDB dictionary operation commits. Product
no-live replay remains conservative: it applies records to uniquely resolved
existing tablespaces and skips records for tablespaces that are absent.

## Public API Impact

No public API changes.

## Binary Size Impact

No new dependency or production code path is expected. The change adds one
focused test selector and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `dropped-tablespace-replay` in `embedded-dev`.
- Run adjacent recovery selectors in `embedded-dev`.
- Build and run the focused/adjacent selectors in `ownerless-test-hooks`.
- Run full embedded and hook ownerless SQL CTest coverage.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- The dropped table is absent from `information_schema.tables`.
- The dropped table's `.frm` and `.ibd` files are absent from `datadir/app/`.
- Retained page-version WAL is present while the live snapshot pin exists.
- After killing the snapshot owner, a no-live ownerless reopen succeeds,
  checkpoints the WAL, and preserves the dropped-table absence.
- Ordinary native exclusive reopen and forced `.shm` rebuild preserve the same
  final state.

## Risks And Open Questions

- This is still evidence for one missing-tablespace class, not a replacement
  for durable DDL file-lifecycle metadata.
- MariaDB may legitimately change which page types a particular `DROP TABLE`
  writes. The test is framed around observable product behavior: retained WAL,
  successful no-live rebuild, absent files, and absent metadata.

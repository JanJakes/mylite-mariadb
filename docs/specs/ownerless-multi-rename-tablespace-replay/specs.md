# Ownerless Multi-Rename Tablespace Replay

## Problem Statement

Ownerless stale-reader rebuild coverage now includes dropped, cross-schema
renamed, truncated, force-rebuilt, and schema-dropped file-per-table shapes.
The ordinary ownerless multi-rename cycle selector proves already-open peer
dictionary refresh for a three-pair `RENAME TABLE` swap, but it does not hold a
stale snapshot reader while retained page-version WAL remains.

MyLite needs focused evidence that retained reader-boundary WAL from multiple
tablespaces does not replay stale page images after a no-live stale-reader
`.shm` rebuild when one `RENAME TABLE` statement swaps two native InnoDB table
names through a temporary name.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `rename_tables()` treats every two `TABLE_LIST` entries as an old/new pair
    and processes the list as one `RENAME TABLE` statement.
  - The DDL log can revert ordinary table renames if a later pair fails.
- `mariadb/sql/sql_parse.cc`
  - `check_rename_table()` walks the same old/new pairs before execution.
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` calls the storage-engine rename path and then
    renames the SQL `.frm` metadata file for each pair.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::rename_table()` drives the InnoDB DDL transaction and calls
    `innobase_rename_table()`.
- `mariadb/storage/innobase/row/row0mysql.cc` and
  `mariadb/storage/innobase/dict/dict0dict.cc`
  - InnoDB dictionary and file-per-table tablespace names are updated through
    the native rename path.
- `packages/libmylite/src/database.cc`
  - No-live stale-reader rebuilds checkpoint retained reader-boundary WAL
    instead of replaying it when the remaining shared-memory state is
    read-view/page-pin evidence without native writer recovery evidence.

## Design

Add a focused ownerless SQL selector,
`multi-rename-tablespace-replay`, beside the existing stale-reader replay
selectors:

1. Create two InnoDB file-per-table tables with large row payloads.
2. Start a peer repeatable-read snapshot pin.
3. Update both tables while the pin is live so retained WAL contains records
   for two tablespaces.
4. Execute one three-pair rename cycle:
   `left TO tmp, right TO left, tmp TO right`.
5. Insert one row through each final table name to prove the swapped names are
   usable after the DDL.
6. Verify retained page-version WAL remains after clean writer close.
7. Kill the reader and verify ownerless/native reopen, forced `.shm` rebuild,
   and native reopen all preserve the swapped final state, absent temporary
   name, native files, and checkpointed WAL.

## Scope

In scope:

- Product SQL evidence for no-live stale-reader rebuild after one same-schema
  multi-pair InnoDB `RENAME TABLE` cycle.
- Multiple retained tablespace page-version records under one stale snapshot
  pin.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Cross-schema multi-rename cycles.
- Foreign-key multi-rename semantics, which have separate peer-refresh
  coverage.
- Crash or error injection inside the multi-rename statement.
- Durable DDL file-lifecycle metadata for every rename/rebuild/drop class.
- SQL-level table-lock wait fault injection; prior SQL shapes did not reach
  the ownerless table-wait callback.

## Compatibility Impact

SQL behavior is unchanged. The slice expands the current partial DDL/file
lifecycle recovery evidence to include a multi-tablespace rename swap under
retained reader-boundary WAL. Full DDL/file-lifecycle recovery remains partial
until durable lifecycle metadata and broader randomized or oracle-backed
coverage exist.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes existing
`datadir/app/*.frm`, `datadir/app/*.ibd`,
`concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

No storage format changes. MariaDB native InnoDB rename state remains the final
authority when no live ownerless writer recovery evidence exists. Retained
reader WAL is checkpointed, not replayed, during no-live stale-reader rebuild.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds SQL test coverage and
documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `multi-rename-tablespace-replay` in `embedded-dev`.
- Run adjacent dropped/renamed/truncated/force-rebuild/schema-drop replay
  selectors in `embedded-dev`.
- Build and run the same focused and adjacent selectors in
  `ownerless-test-hooks`.
- Run embedded and hook ownerless SQL CTest coverage, ownerless stress,
  `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Retained page-version WAL exists after the multi-pair rename cycle while the
  reader pin is live.
- After killing the reader, no-live ownerless reopen succeeds and checkpoints
  retained reader-boundary WAL.
- Final `left` contains the updated former `right` rows plus the post-rename
  insert.
- Final `right` contains the updated former `left` rows plus the post-rename
  insert.
- The temporary table name and native files remain absent.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the same final swapped-table state.

## Risks And Open Questions

- This is focused evidence for one same-schema swap shape, not a complete
  atomic rename rollback or crash-recovery proof.
- MariaDB may alter internal rename ordering, so the test asserts observable
  final table contents, metadata, file presence, retained WAL, and successful
  no-live stale-reader rebuild rather than specific internal DDL log details.

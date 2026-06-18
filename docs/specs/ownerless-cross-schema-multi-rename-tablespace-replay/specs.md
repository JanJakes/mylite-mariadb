# Ownerless Cross-Schema Multi-Rename Tablespace Replay

## Problem Statement

Ownerless stale-reader rebuild coverage includes dropped, renamed, truncated,
force-rebuilt, same-schema multi-rename, and schema-dropped file-per-table
shapes. The same-schema multi-rename selector proves retained reader-boundary
WAL across a three-pair `RENAME TABLE` cycle, but cross-schema multi-pair rename
movement remained outside that focused replay evidence.

MyLite needs bounded evidence that retained reader-boundary WAL from multiple
tablespaces does not replay stale page images after no-live `.shm` rebuild when
one `RENAME TABLE` statement moves InnoDB file-per-table tablespaces between
schema directories through a temporary name.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc::rename_tables()` treats every two `TABLE_LIST`
  entries as one old/new pair and processes all pairs as one statement.
- `mariadb/sql/sql_table.cc::mysql_rename_table()` calls the storage-engine
  rename path and then renames the SQL `.frm` metadata file.
- `mariadb/storage/innobase/handler/ha_innodb.cc::ha_innobase::rename_table()`
  drives the InnoDB DDL transaction and native table rename.
- `mariadb/storage/innobase/row/row0mysql.cc` and
  `mariadb/storage/innobase/dict/dict0dict.cc` update native InnoDB dictionary
  and file-per-table tablespace names.
- `packages/libmylite/src/database.cc` no-live stale-reader rebuilds checkpoint
  retained reader-boundary WAL instead of replaying it when remaining shared
  state is read-view/page-pin evidence without native writer recovery evidence.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector,
  `cross-schema-multi-rename-tablespace-replay`.
- Create one InnoDB table in `app` and one InnoDB table in another schema.
- Hold a stale repeatable-read snapshot pin while both tables are updated.
- Execute one three-pair cross-schema rename cycle:
  `app.left TO schema.tmp`, `schema.right TO app.left`,
  `schema.tmp TO schema.right`.
- Verify retained WAL remains until the reader dies, then verify ownerless and
  native reopen before and after forced `.shm` rebuild observe the swapped final
  state and absent temporary table.

Out of scope:

- Rename rollback fault injection or crash injection inside the multi-rename
  statement.
- Foreign-key rename behavior, which has separate same-schema and cross-schema
  multi-rename coverage.
- Durable DDL file-lifecycle metadata for every rename/rebuild/drop class.
- SQL-level table-lock wait fault injection; prior SQL shapes did not reach the
  ownerless table-wait callback.

## Design

The selector follows the same retained-WAL harness shape as the same-schema
multi-rename replay:

1. Initialize a normal ownerless database and assert the page-version WAL is
   checkpointed.
2. Create schema `ownerless_cross_rename_schema`.
3. Create `app.ownerless_cross_multi_replay_left` and
   `ownerless_cross_rename_schema.ownerless_cross_multi_replay_right` with
   large payload rows.
4. Start a peer repeatable-read snapshot pin.
5. Update both tables so retained WAL covers both tablespaces.
6. Execute one cross-schema three-pair `RENAME TABLE` cycle through
   `ownerless_cross_rename_schema.ownerless_cross_multi_replay_tmp`.
7. Insert a post-rename row through each final table name.
8. Verify the writer close retains WAL while the reader is alive.
9. Kill the reader and verify ownerless/native reopen, forced `.shm` rebuild,
   and native reopen all preserve final rows, metadata, native files, absent
   temporary files, and checkpointed WAL.

## Compatibility Impact

No SQL behavior changes. The slice broadens DDL/file-lifecycle recovery
evidence for a MariaDB-compatible cross-schema multi-pair `RENAME TABLE` shape
under retained ownerless reader WAL. Full DDL/file-lifecycle recovery remains
partial.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes existing
`datadir/app/*.frm`, `datadir/app/*.ibd`,
`datadir/ownerless_cross_rename_schema/*.frm`,
`datadir/ownerless_cross_rename_schema/*.ibd`,
`concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

No native storage format changes. MariaDB remains responsible for the native
InnoDB rename operation. MyLite verifies that retained reader-boundary WAL is
checkpointed, not replayed as stale pages, after the stale reader dies.

## Build, Size, License, And Dependencies

No production dependency, license, build-profile, or binary-size changes. The
slice adds first-party SQL test coverage and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` in `php-embedded-prod`.
- Run focused `cross-schema-multi-rename-tablespace-replay`.
- Run adjacent stale-reader replay selectors for same-schema multi-rename,
  cross-schema multi-drop, and cross-schema rename.
- Build and run the focused selector under `ownerless-test-hooks`.
- Run focused adjacent stale-reader replay selectors directly from the
  production ownerless SQL harness.
- Run `tools/check-ci-production-builds`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  cross-schema-multi-rename-tablespace-replay` passed.
- Focused adjacent production selectors passed:
  `cross-schema-multi-rename-tablespace-replay`,
  `multi-rename-tablespace-replay`,
  `cross-schema-multi-drop-tablespace-replay`, and
  `renamed-tablespace-replay`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  cross-schema-multi-rename-tablespace-replay` passed.
- `tools/check-ci-production-builds` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Retained page-version WAL exists after the cross-schema multi-pair rename
  cycle while the reader pin is live.
- After killing the reader, no-live ownerless reopen succeeds and checkpoints
  retained reader-boundary WAL.
- Final `app.ownerless_cross_multi_replay_left` contains the updated former
  schema table rows plus the post-rename insert.
- Final `ownerless_cross_rename_schema.ownerless_cross_multi_replay_right`
  contains the updated former app table rows plus the post-rename insert.
- The temporary table name and native files remain absent.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the same final swapped-table state.

## Risks And Follow-Up

- This is focused evidence for one cross-schema swap shape, not complete atomic
  rename rollback or crash-recovery coverage.
- Broader DDL/file-lifecycle recovery and randomized external DDL oracle stress
  remain planned.

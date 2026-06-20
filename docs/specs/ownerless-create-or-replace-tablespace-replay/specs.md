# Ownerless CREATE OR REPLACE Tablespace Replay

## Problem Statement

Ownerless stale-reader replay coverage proves dropped, created, recreated,
renamed, truncated, force-rebuilt, multi-rename, and schema-drop
file-per-table final states. `CREATE OR REPLACE TABLE` is a distinct MariaDB
DDL shape: it removes the old table and creates a same-name replacement inside
one SQL statement.

MyLite needs focused evidence that no-live stale-reader replay preserves the
replacement InnoDB tablespace when retained page-version WAL still contains old
same-name page images, rather than replaying stale pages from the replaced
tablespace into the new file.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` accepts `create_or_replace ... TABLE`.
- `mariadb/sql/sql_table.cc` `log_drop_table()` documents the failed
  `CREATE OR REPLACE TABLE` cleanup path when the old table was removed but
  replacement creation failed.
- `mariadb/sql/sql_table.cc` `mysql_create_table_no_lock()` handles existing
  normal tables with `options.or_replace()` by deleting table statistics and
  removing the old table through `mysql_rm_table_no_locks()` before replacement
  creation continues.
- `mariadb/sql/sql_table.cc` then creates the replacement table through the
  same native create path and has a separate locked-table reconnect branch for
  `CREATE OR REPLACE TABLE`; ownerless SQL rejects locked-table mode, so this
  slice covers ordinary ownerless replacement.
- `packages/libmylite/src/ownerless_tablespace_replay.cc` resolves native
  file-per-table pages by InnoDB page-0 space id before applying page-version
  records.
- `packages/libmylite/src/database.cc` calls ownerless tablespace replay with
  `MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES` during
  no-live ownerless recovery, and separately discards retained stale-reader
  page WAL when the remaining state is reader-only evidence without native
  writer recovery evidence.

## Design

Add a focused ownerless SQL selector,
`create-or-replace-tablespace-replay`, beside the existing stale-reader
tablespace replay selectors:

1. Create `app.ownerless_replace_replay` as an InnoDB file-per-table table with
   an old column and old secondary index.
2. Insert large rows, verify native `.frm`/`.ibd` files, and record the initial
   `INFORMATION_SCHEMA.INNODB_SYS_TABLES.SPACE` plus page-0 space id.
3. Start a separate ownerless process with a repeatable-read snapshot pin.
4. Update the old table while the snapshot pin is live so retained WAL contains
   old tablespace page versions.
5. Execute `CREATE OR REPLACE TABLE app.ownerless_replace_replay` with a new
   definition, new column, and new secondary index.
6. Verify the replacement rowset starts empty, write replacement rows, and
   verify the replacement space id differs from the initial space id.
7. Kill the snapshot owner so no live process remains.
8. Verify ownerless reopen, ordinary native exclusive reopen, forced `.shm`
   rebuild ownerless reopen, and native exclusive reopen all preserve the
   replacement definition, rows, native files, and new space id while
   checkpointing retained WAL.

## Scope

In scope:

- Product SQL evidence for no-live stale-reader replay over same-name
  `CREATE OR REPLACE TABLE` replacement.
- Old-column and old-index absence after replacement.
- New-column and new-index metadata, replacement rows, and native file presence.
- Page-0 space-id identity checks proving the replacement tablespace, not the
  old same-name tablespace, remains durable.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Foreign-key, trigger, generated-column, partition, special-index, or
  unsupported-storage-option replacement variants.
- Broader failed replacement cleanup variants beyond the representative
  old-table-removal boundary covered by
  `docs/specs/ownerless-create-or-replace-after-drop-crash/specs.md`.
- SQL locked-table mode, which ownerless SQL rejects.
- External MariaDB/RQG DDL stress.

## Compatibility Impact

SQL behavior is unchanged. The slice strengthens existing partial ownerless
`CREATE OR REPLACE TABLE` evidence by proving the final replacement
file-per-table state after no-live stale-reader replay. Full DDL/file-lifecycle
recovery remains partial until durable lifecycle metadata, broader native
redo/checkpoint reconciliation, live-peer DDL/tablespace crash recovery, and
external oracle stress exist.

## DDL Metadata Routing Impact

The selector uses MariaDB's native `CREATE OR REPLACE TABLE` routing through
MyLite's ownerless dictionary generation boundary. It verifies the replacement
table through `INFORMATION_SCHEMA.TABLES`, replacement columns through
`INFORMATION_SCHEMA.COLUMNS`, replacement index metadata through
`INFORMATION_SCHEMA.STATISTICS`, and replacement InnoDB space identity through
`INFORMATION_SCHEMA.INNODB_SYS_TABLES`.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes the existing
`datadir/app/ownerless_replace_replay.frm`,
`datadir/app/ownerless_replace_replay.ibd`,
`concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

No storage format changes. MariaDB's final native replacement `.ibd` file is
the storage authority when no live writer recovery evidence exists. Retained
page-version records for the old same-name tablespace must not be applied to
the replacement tablespace with a different InnoDB space id.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The slice adds focused SQL test coverage and documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `create-or-replace-tablespace-replay` in `embedded-dev`.
- Run adjacent stale-reader replay selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded and hook ownerless SQL CTest shard containing the new
  full-suite case.
- Run the relevant ownerless stress DDL/DML selector if available.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- The initial table exists, has old metadata, and checkpoints cleanly before
  the stale reader starts.
- Retained page-version WAL exists after `CREATE OR REPLACE TABLE` while the
  reader pin is live.
- The replacement table has a new InnoDB space id distinct from the original.
- The old column and old secondary index are absent after replacement.
- The new column, new secondary index, replacement rows, and native `.frm` and
  `.ibd` files persist.
- After killing the reader, no-live ownerless reopen succeeds and checkpoints
  retained reader-boundary WAL.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the same replacement-table final state.

## Evidence

Focused embedded selector coverage passed:

```text
create-or-replace-tablespace-replay
```

Adjacent stale-reader replay selectors also passed in the same embedded build:

```text
dropped-tablespace-replay
multi-drop-tablespace-replay
cross-schema-multi-drop-tablespace-replay
renamed-tablespace-replay
truncated-tablespace-replay
schema-drop-tablespace-replay
force-rebuild-tablespace-replay
multi-rename-tablespace-replay
created-tablespace-replay
ctas-post-create-dml
recreated-tablespace-replay
create-or-replace-tablespace-replay
```

The embedded ownerless SQL CTest shard containing the new full-suite case
passed:

```text
ctest --preset embedded-dev -R '^libmylite\.ownerless-cross-process-sql\.4$' --output-on-failure
```

Hook-build focused selector coverage passed:

```text
build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test create-or-replace-tablespace-replay
```

The hook ownerless SQL CTest shard containing the new full-suite case passed:

```text
ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-cross-process-sql\.5$' --output-on-failure
```

Relevant DDL stress and static checks passed:

```text
ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure
cmake --build --preset dev --target format-check
git diff --check
```

## Risks And Open Questions

- This proves a successful same-name replacement final state. A later focused
  after-drop crash slice covers the representative old-table drop before
  replacement creation; broader variants remain open.
- The broader durable file-lifecycle protocol and external oracle stress remain
  open work.

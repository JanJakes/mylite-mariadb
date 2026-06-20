# Ownerless CREATE OR REPLACE CTAS Tablespace Replay

## Problem Statement

Ownerless stale-reader replay coverage proves plain same-name
`CREATE OR REPLACE TABLE` replacement and ordinary `CREATE TABLE ... AS SELECT`
creation. MariaDB also accepts `CREATE OR REPLACE TABLE ... AS SELECT`, which
combines old-table removal, replacement table creation, and row population in a
single SQL statement.

MyLite needs focused evidence that no-live stale-reader replay preserves the
populated CTAS replacement table when retained page-version WAL still contains
old same-name page images from the replaced table.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `create_or_replace opt_temporary TABLE`
  before `create_body`, and `create_body` includes the optional
  `opt_create_select` CTAS grammar.
- `mariadb/sql/sql_table.cc` removes an existing normal table for
  `options.or_replace()` through `mysql_rm_table_no_locks()` before continuing
  replacement creation.
- `mariadb/sql/sql_table.cc` `mysql_create_table_no_lock()` delegates physical
  table creation to `create_table_impl()`, and the CTAS path goes through the
  create-table-select flow.
- `mariadb/sql/sql_insert.cc` contains the create-table-select open and lock
  phases used after the destination table is created.
- `packages/libmylite/src/ownerless_tablespace_replay.cc` resolves native
  file-per-table pages by InnoDB page-0 space id before applying page-version
  records.
- `packages/libmylite/src/database.cc` no-live ownerless recovery can
  checkpoint retained stale-reader WAL when the remaining state is reader-only
  evidence without native writer recovery evidence.

## Design

Add a focused ownerless SQL selector,
`create-or-replace-ctas-tablespace-replay`, beside the stale-reader tablespace
replay selectors:

1. Create a source InnoDB table and an existing same-name target table with an
   old column, old secondary index, and large rows.
2. Record the target table's initial InnoDB `SPACE` and page-0 space id.
3. Start a separate ownerless process with a repeatable-read snapshot pin.
4. Update the old target while the snapshot pin is live so retained WAL
   contains old target tablespace page versions.
5. Execute `CREATE OR REPLACE TABLE target ENGINE=InnoDB AS SELECT ...` from
   the source table.
6. Verify the CTAS replacement is populated by the same statement, the old
   column and old index are absent, and the replacement space id differs from
   the initial target space id.
7. Apply small post-create DML to the replacement table so final state includes
   both CTAS-populated rows and later writes.
8. Kill the snapshot owner so no live process remains.
9. Verify ownerless reopen, ordinary native exclusive reopen, forced `.shm`
   rebuild ownerless reopen, and native exclusive reopen all preserve the
   replacement rows, metadata, native files, and new space id while
   checkpointing retained WAL.

## Scope

In scope:

- Product SQL evidence for no-live stale-reader replay over
  `CREATE OR REPLACE TABLE ... AS SELECT`.
- Old target column and index absence after CTAS replacement.
- Replacement CTAS rows plus post-create DML rows.
- Source table survival.
- Page-0 space-id identity checks proving stale old target pages are not
  replayed into the populated replacement tablespace.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Failed CTAS replacement cleanup between old-target removal, destination table
  creation, and row population.
- CTAS replacement with foreign keys, triggers, generated columns, partitioning,
  unsupported storage options, or special indexes.
- SQL locked-table mode, which ownerless SQL rejects.
- External MariaDB/RQG CTAS DDL stress.

## Compatibility Impact

SQL behavior is unchanged. The slice strengthens existing partial ownerless
`CREATE OR REPLACE TABLE` and CTAS evidence by proving the combined replacement
and populated destination final state after no-live stale-reader replay. Full
DDL/file-lifecycle recovery remains partial until durable lifecycle metadata,
broader native redo/checkpoint reconciliation, live-peer DDL/tablespace crash
recovery, and external oracle stress exist.

## DDL Metadata Routing Impact

The selector uses MariaDB's native `CREATE OR REPLACE TABLE ... AS SELECT`
routing through MyLite's ownerless dictionary generation boundary. It verifies
the source and replacement tables through `INFORMATION_SCHEMA.TABLES`,
replacement columns through `INFORMATION_SCHEMA.COLUMNS`, old-index absence
through `INFORMATION_SCHEMA.STATISTICS`, and replacement InnoDB space identity
through `INFORMATION_SCHEMA.INNODB_SYS_TABLES`.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes:

- `datadir/app/ownerless_replace_ctas_source_replay.frm`
- `datadir/app/ownerless_replace_ctas_source_replay.ibd`
- `datadir/app/ownerless_replace_ctas_replay.frm`
- `datadir/app/ownerless_replace_ctas_replay.ibd`
- `concurrency/mylite-concurrency.wal`
- `concurrency/mylite-concurrency.shm`

## Native Storage Impact

No storage format changes. MariaDB's final populated replacement `.ibd` file is
the storage authority when no live writer recovery evidence exists. Retained
page-version records for the old same-name target tablespace must not be
applied to the CTAS replacement tablespace with a different InnoDB space id.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The slice adds focused SQL test coverage and documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `create-or-replace-ctas-tablespace-replay` in `embedded-dev`.
- Run adjacent stale-reader replay selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded and hook ownerless SQL CTest shard containing the new
  full-suite case.
- Run the relevant ownerless DDL stress selector.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- The old target table exists, has old metadata, and checkpoints cleanly before
  the stale reader starts.
- Retained page-version WAL exists after `CREATE OR REPLACE TABLE ... AS
  SELECT` while the reader pin is live.
- The CTAS replacement table has a new InnoDB space id distinct from the old
  target table.
- The old target column and old secondary index are absent after replacement.
- The replacement rows, post-create DML rows, source table, and native `.frm`
  and `.ibd` files persist.
- After killing the reader, no-live ownerless reopen succeeds and checkpoints
  retained reader-boundary WAL.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the same populated replacement-table final state.

## Evidence

Focused embedded selector coverage passed:

```text
create-or-replace-ctas-tablespace-replay
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
create-or-replace-ctas-tablespace-replay
```

The embedded ownerless SQL CTest shard containing the new full-suite case
passed:

```text
ctest --preset embedded-dev -R '^libmylite\.ownerless-cross-process-sql\.6$' --output-on-failure
```

Hook-build focused selector coverage passed:

```text
build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test create-or-replace-ctas-tablespace-replay
```

The hook ownerless SQL CTest shard containing the new full-suite case passed:

```text
ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-cross-process-sql\.6$' --output-on-failure
```

Relevant DDL stress passed:

```text
ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure
```

Static checks passed:

```text
cmake --build --preset dev --target format-check
git diff --check
```

## Risks And Open Questions

- This proves a successful same-name CTAS replacement final state. The
  representative plain after-drop boundary is covered separately by
  `docs/specs/ownerless-create-or-replace-after-drop-crash/specs.md`; CTAS
  cleanup after destination create and during row population remains open.
- The broader durable file-lifecycle protocol and external oracle stress remain
  open work.

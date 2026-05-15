# Failed CREATE OR REPLACE Rollback

## Problem

`CREATE OR REPLACE TABLE` uses MariaDB's drop-then-create flow. MyLite now
covers successful routed replacement, but an embedded single-file runtime must
also prove that representative failed replacements do not leave the old table
missing from SQL visibility or the durable catalog. Importers and migration
tools often depend on failed refreshes preserving the previous table.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/structs.h:591-621` defines `DDL_options_st::OPT_OR_REPLACE`
  and `DDL_options_st::or_replace()`.
- `mariadb/sql/structs.h:603-607` preserves `OPT_OR_REPLACE` for
  `CREATE TABLE ... LIKE` through `DDL_options_st::create_like_options()`.
- `mariadb/sql/sql_table.cc:4772-4822` drops an existing base table during
  OR REPLACE with `mysql_rm_table_no_locks()`, records
  `table_was_deleted`, and restarts statement transactions for CTAS.
- `mariadb/sql/sql_table.cc:mysql_create_like_table()` rejects non-temporary
  self-LIKE OR REPLACE before publishing a replacement definition.
- `mariadb/sql/sql_insert.cc:select_create::create_table_from_items()` routes
  OR REPLACE CTAS through the no-lock create path before row population.
- `packages/libmylite/src/database.cc:is_storage_outer_checkpoint_sql()`
  wraps `CREATE`, `ALTER`, `DROP`, `RENAME`, and `TRUNCATE` in a file-backed
  statement checkpoint for direct SQL.
- `packages/libmylite/src/database.cc:mylite_exec()` rolls back the active
  checkpoint when `mysql_query()` fails.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_begin_statement_checkpoint()`
  reuses an already-active outer checkpoint instead of creating a nested
  handler checkpoint.

## Scope

- Failed `CREATE OR REPLACE TABLE target LIKE target` over a routed MyLite
  table.
- Failed `CREATE OR REPLACE TABLE target (...)` where replacement creation
  reaches the MyLite handler but the replacement definition is unsupported.
- Failed `CREATE OR REPLACE TABLE target (...) SELECT ...` where replacement
  row population fails after the replacement target has been created.
- Old target table visibility, requested-engine metadata, rows, indexes,
  autoincrement state, close/reopen visibility, and durable-sidecar gates.

## Non-Goals

- Exhaustive OR REPLACE error matrices, lock-table interactions, temporary
  replacements, binlog behavior, views, triggers, partitions, foreign keys, or
  unsupported source objects.
- Physical compaction of pages made unreachable by replacement attempts.
- SQL transaction, savepoint, or multi-statement rollback semantics.

## Design

Use the existing statement-checkpoint path:

1. `libmylite` begins a storage statement checkpoint before executing
   file-backed `CREATE OR REPLACE TABLE`.
2. MariaDB may reject the statement before dropping the target, or it may drop
   the old target and begin creating or populating the replacement.
3. If MariaDB returns an error, `libmylite` rolls back the storage checkpoint.
4. The MyLite catalog, rows, index entries, and autoincrement pages visible
   before the statement become visible again.

No production code change is expected unless coverage exposes a checkpoint or
handler lifecycle bug.

## Compatibility Impact

Representative failed OR REPLACE rollback moves from planned to partial for
routed MyLite tables. The claim is intentionally bounded to direct file-backed
statements using the current outer statement checkpoint; full SQL transaction
and savepoint semantics remain planned.

## DDL Metadata Routing Impact

The old target catalog record must remain visible after a failed replacement,
including requested-engine metadata. Replacement catalog records created after
the checkpoint must become invisible after rollback.

## Single-File And Embedded Lifecycle Impact

Failed replacements must leave durable state in the primary `.mylite` file only,
with no persistent MariaDB `.frm`, engine, or log sidecars. Close/reopen must
discover the original target without rehydrated runtime schema directories.

## Public API And File-Format Impact

No public `libmylite` API change and no storage file-format change.

## Storage-Engine Routing Impact

The covered old target uses routed MyLite storage with requested `MyISAM`
metadata, while replacement attempts use routed `ENGINE=InnoDB` and unsupported
index-class rejection through the MyLite handler.

## Binary-Size And Dependency Impact

No dependency is added. Binary-size impact should be limited to storage-smoke
test code unless a checkpoint bug needs a production fix.

## Test And Verification Plan

- Add storage-engine smoke coverage that:
  - creates an old target with rows, supported indexes, and autoincrement
    state;
  - verifies failed self-LIKE OR REPLACE preserves the old target;
  - verifies failed unsupported-index replacement preserves the old target;
  - verifies failed duplicate-key replacement CTAS preserves the old target;
  - inserts a new row after the failed replacements to prove autoincrement
    state is still the old target's state;
  - verifies close/reopen visibility and durable sidecar gates.
- Run targeted storage-smoke tests, compatibility harness reports, format,
  tidy, diff, shell checks, and full preset gates.

## Acceptance Criteria

- The old target remains SQL-visible after covered failed OR REPLACE
  statements.
- The old target catalog metadata, rows, supported indexes, and autoincrement
  state remain visible before and after close/reopen.
- Docs stop describing representative failed OR REPLACE rollback as entirely
  planned, while broader rollback and SQL transaction semantics remain planned.

## Implementation Status

Implemented in storage-engine smoke coverage:

- Failed self-LIKE OR REPLACE preserves the old target.
- Failed unsupported-index replacement creation preserves the old target.
- Failed duplicate-key replacement CTAS preserves the old target.
- The old target's requested-engine metadata, rows, supported indexes, and
  autoincrement state remain visible before and after close/reopen without
  durable MariaDB sidecars.

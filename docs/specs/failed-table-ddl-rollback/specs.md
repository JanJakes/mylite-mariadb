# Failed Table DDL Rollback

## Problem

MyLite has representative failed `CREATE OR REPLACE TABLE` rollback coverage,
but broader table DDL can also make partial durable changes before MariaDB
returns an error. Multi-table `DROP TABLE` can drop an earlier table and report
a later missing table. Multi-table `RENAME TABLE` can rename an earlier table
and fail on a later pair, causing MariaDB to revert through its DDL log.

MyLite must prove its statement checkpoint keeps the single `.mylite` file
consistent across those representative failed table-DDL paths.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:1145-1248` routes `DROP TABLE` through
  `mysql_rm_table()` and then `mysql_rm_table_no_locks()`.
- `mariadb/sql/sql_table.cc:1420-1464` iterates each table in a multi-table
  drop.
- `mariadb/sql/sql_table.cc:1630-1644` records missing tables as errors while
  the multi-table drop loop continues.
- `mariadb/sql/sql_table.cc:1687-1714` calls engine drop hooks and marks a
  table as dropped before later errors may be reported.
- `mariadb/sql/sql_table.cc:1923-1932` reports collected unknown-table errors
  at the end of the drop operation.
- `mariadb/sql/sql_rename.cc:56-222` runs `mysql_rename_tables()`, then
  reverts failed normal-table renames through MariaDB's DDL log.
- `mariadb/sql/sql_rename.cc:276-327` checks each rename source and target
  pair, reporting missing sources or existing targets.
- `mariadb/sql/sql_rename.cc:505-562` processes rename pairs sequentially and
  returns an error after reverting temporary renames locally.
- `packages/libmylite/src/database.cc:is_storage_outer_checkpoint_sql()` wraps
  `CREATE`, `ALTER`, `DROP`, `RENAME`, and `TRUNCATE` in a MyLite storage
  statement checkpoint for file-backed direct SQL.
- `packages/libmylite/src/database.cc:mylite_exec()` rolls back the checkpoint
  when `mysql_query()` returns an error.

## Scope

- Failed multi-table `DROP TABLE existing, missing` over a routed MyLite table.
- Failed multi-table `RENAME TABLE old TO new, missing TO other` after the
  first rename pair has succeeded.
- Preservation of old table visibility, requested/effective engine metadata,
  rows, supported indexes, close/reopen discovery, and sidecar gates.

## Non-Goals

- General DDL transaction support.
- SQL transactions, savepoints, or user-visible `ROLLBACK`.
- Temporary table DDL rollback.
- Views, triggers, routines, foreign keys, partitions, or cross-schema rename
  matrices.
- Crash-safe logical rollback if the process dies while restoring a failed
  statement checkpoint.
- Physical compaction of pages made unreachable by failed DDL attempts.

## Design

No production code change is expected. The slice should exercise the existing
statement-checkpoint flow:

1. `libmylite` begins a checkpoint before file-backed `DROP` or `RENAME`.
2. MariaDB enters its normal table-DDL path and may mutate MyLite catalog state.
3. MariaDB returns an error for a later table or rename pair.
4. `libmylite` rolls back the checkpoint before returning the SQL error.
5. MyLite-visible durable state matches the statement-start state.

## Compatibility Impact

Representative failed table-DDL rollback moves from broad planned rollback work
into covered partial behavior for routed MyLite base tables. The compatibility
claim remains narrower than MariaDB transactional DDL: explicit SQL
transactions and savepoints are still unsupported.

## DDL Metadata Routing Impact

Failed `DROP TABLE` must leave the dropped table's catalog record visible.
Failed `RENAME TABLE` must leave the original catalog name visible and the
intermediate target name absent. Requested engine metadata must be preserved.

## Single-File And Embedded Lifecycle Impact

The slice must not introduce new durable companions. Failed table DDL must
leave only the primary `.mylite` file, with no durable MariaDB `.frm`, engine,
or log sidecars. Close/reopen must discover the preserved tables from the
catalog without a runtime schema directory.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

The covered tables use routed `ENGINE=InnoDB` metadata with effective `MYLITE`
storage. The behavior should apply to other routed requested engines through
the same checkpoint path, but this slice covers one representative engine.

## Build, Size, And Dependencies

No dependency or build-profile change. Binary-size impact should be zero unless
coverage exposes a production bug.

## Test And Verification Plan

- Add storage-engine smoke coverage that:
  - creates routed tables with rows and supported indexes;
  - verifies failed `DROP TABLE existing, missing` keeps the existing table,
    rows, index reads, and catalog metadata;
  - verifies failed `RENAME TABLE old TO new, missing TO other` keeps the old
    name and rejects the intermediate new name;
  - verifies close/reopen discovery and sidecar gates.
- Run focused storage-smoke coverage, statement-rollback and routed DDL/DML
  harness reports, format, tidy, diff, shell checks, and full preset gates.

## Acceptance Criteria

- Covered failed `DROP TABLE` preserves the original table before and after
  close/reopen.
- Covered failed `RENAME TABLE` preserves the original table name, rows,
  indexes, and metadata before and after close/reopen.
- The compatibility matrix, roadmap, storage architecture, and harness
  descriptions identify representative failed table-DDL rollback as covered.
- Broader SQL transaction and DDL rollback work remains planned.

## Risks And Open Questions

- MariaDB's own DDL log reverts some failed rename paths before MyLite's
  checkpoint rollback runs. This test still matters because MyLite's catalog
  and storage state must remain consistent with that SQL-layer behavior.
- This coverage does not prove crash-safe logical statement undo. A durable
  statement/WAL design is still required for that claim.

# Ownerless Secondary-Index DDL Crash

## Problem Statement

Ownerless unsafe-hook coverage already kills completed dictionary DDL before the
ownerless dictionary generation is published for create-table, rename,
truncate, drop-table, and drop-schema boundaries. Remaining DDL crash evidence
did not cover secondary-index metadata, even though MariaDB maps standalone
`CREATE INDEX` and `DROP INDEX` through `ALTER TABLE` and InnoDB updates native
table/index metadata before MyLite publishes the ownerless dictionary boundary.

This slice adds focused coverage for killed `CREATE INDEX` and `DROP INDEX`
writers after the native DDL succeeds but before ownerless dictionary finish.
Secondary-index rename and ignored/not-ignored metadata crash coverage is
tracked separately in
`docs/specs/ownerless-index-metadata-crash/specs.md`.
The follow-up
`docs/specs/ownerless-index-metadata-live-recovery/specs.md` promotes the
ordinary create/drop selectors to live-peer recovery with native
file-operation marker retention until final no-live drain.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_parse.cc:4255` through `mariadb/sql/sql_parse.cc:4290`
  route `SQLCOM_CREATE_INDEX` and `SQLCOM_DROP_INDEX` by preparing
  `Alter_info` and calling `mysql_alter_table()`.
- `mariadb/sql/sql_table.cc:10682` through `mariadb/sql/sql_table.cc:10703`
  documents that `mysql_alter_table()` is used for `ALTER TABLE` and for
  `CREATE|DROP INDEX`, and that index changes generate a new table definition
  instead of taking the rename-only shortcut.
- `mariadb/sql/sql_table.cc:8047` through `mariadb/sql/sql_table.cc:8165`
  describe the in-place ALTER path, including handler support decisions and
  metadata lock handling for operations that do not copy the table.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  unsafe dictionary fault helpers that arm
  `MYLITE_OWNERLESS_TEST_FAULT=dictionary-before-finish`, signal a parent pipe
  at the hook, and expect the child to be killed before SQL returns.

## Design

Add unsafe-hook SQL selectors for present-index and absent-index recovery.

The present-index selector:

- initialize an ownerless database and create an InnoDB table with rows,
- start a live ownerless peer so recovery must preserve live-peer state,
- start a writer that executes
  `CREATE INDEX ownerless_index_crash_value_idx ON app.ownerless_index_crash_base(value)`
  under the existing `dictionary-before-finish` test fault,
- kill the writer at the hook,
- recover through a new ownerless opener while the live peer still exists,
- prove the native file-operation marker remains set while the peer is live,
- release the peer and prove final no-live recovery drains the marker,
- verify the recovered secondary index through `INFORMATION_SCHEMA.STATISTICS`
  and a `FORCE INDEX` query,
- insert another row through the recovered table, then verify the same final
  state through ownerless reopen, native exclusive reopen, forced `.shm`
  rebuild, and native exclusive reopen after rebuild.

The absent-index selector follows the same peer/fault/reopen pattern but starts
with an existing secondary index, executes
`DROP INDEX ownerless_index_drop_crash_value_idx ON app.ownerless_index_drop_crash_base`,
and verifies the recovered state has no `INFORMATION_SCHEMA.STATISTICS` row for
that index, rejects `FORCE INDEX`, accepts later writes, and preserves the
absent-index table through ownerless/native reopen before and after forced
`.shm` rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed standalone
  secondary-index create and drop,
- ownerless live-peer recovery behavior,
- final no-live marker drain plus ownerless rebuild and native reopen of
  recovered present-index and
  absent-index metadata.

Out of scope:

- randomized DDL crash exploration,
- `FULLTEXT`, `SPATIAL`, partition, tablespace detach/import, or
  directory-option index classes that ownerless mode already rejects or leaves
  planned,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No new SQL feature is enabled. The slice strengthens the evidence behind the
existing ownerless claim that ordinary InnoDB secondary-index DDL remains
recoverable across a killed writer at the MyLite dictionary publication
boundary.

## Directory And Lifecycle Impact

No file layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises the existing ownerless `.shm` rebuild,
process-slot cleanup, live-peer dictionary-generation recovery, final no-live
marker drain, and native exclusive reopen lifecycle.

## Native Storage Impact

The covered DDL uses MariaDB/InnoDB native secondary-index metadata for a
file-per-table InnoDB table. MyLite does not reinterpret or rewrite that native
metadata; it only proves ownerless recovery rebuilds volatile coordination
around the completed native DDL.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-secondary-index-crash`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-secondary-index-drop-crash`.
- Run the hook crash-tail selector.
- Run the relevant ownerless hook shard and embedded ownerless shard.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- Recovery succeeds while another ownerless peer remains live.
- The native file-operation marker remains set while that peer is live and
  drains after final no-live recovery.
- The recovered index appears in `INFORMATION_SCHEMA.STATISTICS`.
- `FORCE INDEX` reads succeed before and after an additional recovered write.
- The recovered dropped index is absent from `INFORMATION_SCHEMA.STATISTICS`.
- `FORCE INDEX` fails after recovered drop-index metadata removal.
- Ownerless and ordinary native reopen observe the same table/index state before
  and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This remains deterministic crash coverage, not randomized DDL oracle
  execution.
- Secondary-index rename and ignored/not-ignored live recovery is handled by
  the ownerless index metadata live-recovery slice.
- SQL-level table-lock fault injection remains planned because previously
  explored SQL shapes time out before reaching MyLite's ownerless table-wait
  callback.

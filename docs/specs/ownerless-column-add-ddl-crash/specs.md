# Ownerless Column-Add DDL Crash

## Problem Statement

Ownerless DDL crash coverage now covers create-table, rename, secondary-index
create/drop, truncate, drop-table, and drop-schema boundaries. Existing
ownerless peer-refresh coverage also verifies column add/modify/rename/drop
ALTERs. The remaining gap is the crash boundary between a completed native
table-definition ALTER and ownerless dictionary finish for column metadata.

This slice adds focused coverage for a killed `ALTER TABLE ... ADD COLUMN`
writer after MariaDB/InnoDB completes the native table-definition change but
before MyLite publishes the ownerless dictionary finish boundary. The follow-up
ownerless-column-add-live-recovery slice promotes this focused selector from
cleanup-busy/no-live recovery to live-peer recovery through the native
file-operation recovery lane while retaining the checkpoint marker until
no-live drain.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:8529` through `mariadb/sql/sql_table.cc:8565`
  documents how ALTER parse output is transformed into a new CREATE/ALTER
  table definition, including added, dropped, or modified columns and keys.
- `mariadb/sql/sql_table.cc:10682` through `mariadb/sql/sql_table.cc:10703`
  documents that `mysql_alter_table()` uses `create_list` to generate a new
  FRM/table definition when fields or indexes change.
- `mariadb/sql/sql_table.cc:11570` through `mariadb/sql/sql_table.cc:11720`
  prepares the altered table definition, fills `Alter_inplace_info`, asks the
  handler whether in-place ALTER is supported, and enforces requested algorithm
  and lock compatibility.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  dictionary fault helpers that arm
  `MYLITE_OWNERLESS_TEST_FAULT=dictionary-before-finish`, signal the parent at
  the hook, and expect the child to be killed before SQL returns.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database and create an InnoDB table with `id` and
  `value` columns,
- insert rows and verify the `note` column is absent,
- start a live ownerless peer so crashed-writer cleanup exercises the live-peer
  recovery path,
- start a writer that executes
  `ALTER TABLE app.ownerless_column_add_crash_base ADD COLUMN note INT NOT NULL DEFAULT 7`
  under the existing `dictionary-before-finish` test fault,
- kill the writer at the hook,
- prove an ownerless opener can recover while the live peer remains and the
  native file-operation marker stays set,
- release the peer and reopen ownerless read/write to drain the marker and
  rebuild volatile coordination,
- verify `INFORMATION_SCHEMA.COLUMNS` exposes the recovered `note` column and
  default,
- verify existing rows read the default value, a later insert can omit `note`
  and receives the same default, and the final state survives ownerless reopen,
  native exclusive reopen, forced `.shm` rebuild, and native exclusive reopen
  after rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed ordinary
  `ALTER TABLE ... ADD COLUMN`,
- live-peer recovery with native file-operation marker retention until no-live
  drain,
- ownerless/native reopen of recovered column metadata and row/default values.

Out of scope:

- crash coverage for every column ALTER variant,
- generated-column, foreign-key, partition, external directory, or tablespace
  detach/import DDL classes,
- randomized DDL oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for existing
ownerless compatibility with MariaDB table-definition ALTERs by proving the
native result remains recoverable when the writer dies at MyLite's dictionary
publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises the existing ownerless process cleanup,
dictionary-generation recovery, `.shm` rebuild, and native exclusive reopen
lifecycle.

## Native Storage Impact

The covered DDL uses MariaDB/InnoDB's native ALTER TABLE machinery and table
definition files. MyLite does not reinterpret the native table metadata; it
proves ownerless reopen rebuilds volatile coordination around the completed
native ALTER.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-add-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer can recover the ownerless dictionary boundary while the native
  file-operation marker stays set until no-live drain.
- Recovered metadata exposes `note INT NOT NULL DEFAULT 7`.
- Existing rows and later inserts observe the default value.
- Ownerless and ordinary native reopen observe the same table definition and
  rows before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic column-add crash coverage, not exhaustive column ALTER
  crash exploration.
- Broader DDL/file-lifecycle classes and full external randomized oracle stress
  remain planned.
- SQL-level table-lock fault injection remains planned because explored SQL
  shapes time out before reaching MyLite's ownerless table-wait callback.

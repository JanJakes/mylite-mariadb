# Ownerless Column-Drop DDL Crash

## Problem Statement

Ownerless DDL crash coverage now covers table create, rename, secondary-index
create/drop, column add, truncate, table drop, and schema drop boundaries.
Ownerless peer-refresh coverage also verifies ordinary column add, modify,
rename, and drop ALTERs. The remaining gap in the column-drop class is the crash
boundary between a completed native table-definition ALTER and ownerless
dictionary finish.

This slice adds focused coverage for a killed `ALTER TABLE ... DROP COLUMN`
writer after MariaDB/InnoDB completes the native table-definition change but
before MyLite publishes the ownerless dictionary finish boundary.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8029` through `mariadb/sql/sql_yacc.yy:8038`
  parses `DROP COLUMN` into an `Alter_drop::COLUMN` item in
  `Alter_info::drop_list` and marks `ALTER_PARSER_DROP_COLUMN`.
- `mariadb/sql/sql_table.cc:6395` through `mariadb/sql/sql_table.cc:6428`
  handles `DROP COLUMN IF EXISTS` by removing missing-column drops from
  `Alter_info::drop_list` while preserving real column drops.
- `mariadb/sql/sql_table.cc:8529` through `mariadb/sql/sql_table.cc:8565`
  documents how ALTER parse output is transformed into the new CREATE/ALTER
  table definition.
- `mariadb/sql/sql_table.cc:8692` through `mariadb/sql/sql_table.cc:8734`
  builds the new field list by copying table fields that are not matched by a
  `drop_list` `Alter_drop::COLUMN` entry.
- `mariadb/sql/sql_table.cc:10682` through `mariadb/sql/sql_table.cc:10703`
  documents that `mysql_alter_table()` uses `create_list` to generate the new
  FRM/table definition when fields or indexes change.
- `mariadb/sql/sql_table.cc:11570` through `mariadb/sql/sql_table.cc:11720`
  prepares the altered table definition, fills `Alter_inplace_info`, asks the
  handler whether in-place ALTER is supported, and enforces requested algorithm
  and lock compatibility.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database and create an InnoDB table with `id`,
  `value`, and `note` columns,
- insert rows and verify the `note` column and aggregate values are visible,
- start a live ownerless peer so crashed-writer cleanup remains busy,
- start a writer that executes
  `ALTER TABLE app.ownerless_column_drop_crash_base DROP COLUMN note` under the
  existing `dictionary-before-finish` test fault,
- kill the writer at the hook,
- prove an ownerless opener returns `MYLITE_BUSY` while the live peer remains,
- release the peer and reopen ownerless read/write to rebuild volatile
  coordination,
- verify `INFORMATION_SCHEMA.COLUMNS` no longer exposes `note`,
- verify reads that reference `note` fail, surviving rows retain the expected
  `value` aggregate, later inserts into the remaining columns work, and the
  final state survives ownerless reopen, native exclusive reopen, forced `.shm`
  rebuild, and native exclusive reopen after rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed ordinary
  `ALTER TABLE ... DROP COLUMN`,
- live-peer cleanup-busy behavior and no-live rebuild,
- ownerless/native reopen of recovered absent-column metadata and row values.

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
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-drop-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata no longer exposes the dropped `note` column.
- Reads referencing `note` fail and writes to remaining columns still work.
- Ownerless and ordinary native reopen observe the same table definition and
  rows before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic column-drop crash coverage, not exhaustive column ALTER
  crash exploration.
- Broader DDL/file-lifecycle classes and full external randomized oracle stress
  remain planned.
- SQL-level table-lock fault injection remains planned because explored SQL
  shapes time out before reaching MyLite's ownerless table-wait callback.

# Ownerless Column-Rename DDL Crash

## Problem Statement

Ownerless DDL crash coverage now covers table create, table rename,
secondary-index create/drop, column add/drop/modify, truncate, table drop, and
schema drop boundaries. Ownerless peer-refresh coverage also verifies ordinary
column rename ALTERs. The remaining bounded gap in the column-rename class is
the crash boundary between a completed native column rename and ownerless
dictionary finish.

This slice adds focused coverage for a killed
`ALTER TABLE ... RENAME COLUMN` writer after MariaDB/InnoDB completes the native
table-definition change but before MyLite publishes the ownerless dictionary
finish boundary.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8014` through `mariadb/sql/sql_yacc.yy:8028`
  parses `CHANGE`/`MODIFY` column ALTER clauses into changed field
  definitions; `CHANGE` also marks `ALTER_RENAME_COLUMN`.
- `mariadb/sql/sql_yacc.yy:8122` through `mariadb/sql/sql_yacc.yy:8126`
  parses `RENAME COLUMN old TO new` into `Alter_column` entries via
  `LEX::add_alter_list()`.
- `mariadb/sql/sql_table.cc:6362` through `mariadb/sql/sql_table.cc:6388`
  handles `ALTER/RENAME COLUMN IF EXISTS` by removing missing-column alter-list
  entries and clearing parser flags when nothing remains.
- `mariadb/sql/sql_table.cc:7040` through `mariadb/sql/sql_table.cc:7057`
  strips parser-only `ALTER_RENAME_COLUMN` before handler flag calculation
  because renamed columns are represented as concrete column-name changes.
- `mariadb/sql/sql_table.cc:8672` through `mariadb/sql/sql_table.cc:8675`
  initializes rename-expression context for the altered table definition.
- `mariadb/sql/sql_table.cc:8831` through `mariadb/sql/sql_table.cc:8877`
  detects rename entries in `alter_list`, records the old name in
  `Create_field::change`, writes the new column name, and updates period column
  metadata when needed.
- `mariadb/sql/sql_table.cc:8887` through `mariadb/sql/sql_table.cc:8915`
  rewrites virtual column expressions, check constraints, and default
  expressions to use the renamed column.
- `mariadb/sql/sql_table.cc:9528` through `mariadb/sql/sql_table.cc:9536`
  rewrites retained CHECK constraints and marks the table for reopen after a
  column rename.
- `mariadb/sql/sql_table.cc:11867` through `mariadb/sql/sql_table.cc:11875`
  collects renamed fields before ALTER data-copy cleanup when handler metadata
  did not already collect them.
- `mariadb/sql/handler.cc:5847` through `mariadb/sql/handler.cc:5860` treats
  `ALTER_COLUMN_NAME`/`ALTER_RENAME_COLUMN` as inplace-offline operations for
  the old handler API path.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database and create an InnoDB table with
  `note VARCHAR(24) NOT NULL DEFAULT 'old'`,
- insert rows and verify the old column metadata and aggregate values are
  visible,
- start a live ownerless peer so the recovered dictionary boundary is tested
  while another owner remains open,
- start a writer that executes
  `ALTER TABLE app.ownerless_column_rename_crash_base RENAME COLUMN note TO renamed_note`
  under the existing `dictionary-before-finish` test fault,
- kill the writer at the hook,
- reopen ownerless read/write while the peer remains live to recover the
  ownerless dictionary boundary,
- verify `INFORMATION_SCHEMA.COLUMNS` exposes `renamed_note`, no longer exposes
  `note`, and keeps the original type/default metadata,
- verify old-name reads fail, new-name reads and writes succeed, existing row
  values survive, and the final state survives ownerless reopen, native
  exclusive reopen, forced `.shm` rebuild, and native exclusive reopen after
  rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed ordinary
  `ALTER TABLE ... RENAME COLUMN`,
- live-peer recovery with native file-operation marker retention until no-live
  drain,
- ownerless/native reopen of recovered renamed-column metadata, defaults, and
  row values.

Out of scope:

- crash coverage for every column ALTER variant,
- generated-column or CHECK-expression rename crash coverage,
- missing-column `RENAME COLUMN IF EXISTS` no-op crash recovery, which is
  covered by `docs/specs/ownerless-column-if-exists-ddl-crash/specs.md`,
- foreign-key, partition, external directory, or tablespace detach/import DDL
  classes,
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
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-rename-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer can recover the ownerless dictionary boundary while the native
  file-operation marker stays set until no-live drain.
- Recovered metadata exposes `renamed_note` with the original type/default and
  does not expose `note`.
- Existing values survive, old-name reads fail, new-name reads/writes succeed,
  and omitted-column inserts use the preserved default.
- Ownerless and ordinary native reopen observe the same table definition and
  rows before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic column-rename crash coverage, not exhaustive column
  ALTER crash exploration.
- Generated-column/CHECK-expression rename crash coverage, broader DDL/file
  lifecycle classes, and full external randomized oracle stress remain planned;
  missing-column `IF EXISTS` no-op recovery is tracked separately in
  `ownerless-column-if-exists-ddl-crash`.
- SQL-level table-lock fault injection remains planned because explored SQL
  shapes time out before reaching MyLite's ownerless table-wait callback.

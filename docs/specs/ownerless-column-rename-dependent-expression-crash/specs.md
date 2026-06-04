# Ownerless Column-Rename Dependent-Expression Crash

## Problem Statement

Ownerless column-rename crash coverage proves a completed native
`ALTER TABLE ... RENAME COLUMN` remains recoverable when the writer dies before
ownerless dictionary finish. MariaDB's column-rename machinery also rewrites
dependent expressions, including generated-column expressions and CHECK
constraints. Those dependent-expression rewrites need crash-boundary evidence
instead of relying only on the simpler renamed-column metadata case.

This slice adds focused coverage for a killed column-rename writer after
MariaDB/InnoDB completes a native rename that rewrites generated-column and
CHECK expressions but before MyLite publishes ownerless dictionary finish.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8122` through `mariadb/sql/sql_yacc.yy:8126`
  parses `RENAME COLUMN old TO new` into `Alter_column` entries via
  `LEX::add_alter_list()`.
- `mariadb/sql/sql_table.cc:8831` through `mariadb/sql/sql_table.cc:8877`
  detects rename entries in `alter_list`, records the old name in
  `Create_field::change`, writes the new column name, and populates the rename
  context for dependent expressions.
- `mariadb/sql/sql_table.cc:8887` through `mariadb/sql/sql_table.cc:8915`
  rewrites virtual column expressions, column CHECK constraints, and default
  expressions to use the renamed column.
- `mariadb/sql/sql_table.cc:9528` through `mariadb/sql/sql_table.cc:9536`
  rewrites retained table CHECK constraints and marks the table for reopen
  after a column rename.
- `mariadb/sql/item.cc:832` through `mariadb/sql/item.cc:852` implements
  `Item_field::rename_fields_processor()`, replacing expression field names
  whose database/table/name matches a renamed `Create_field`.
- `mariadb/mysql-test/main/alter_table_combinations.test:163` through
  `mariadb/mysql-test/main/alter_table_combinations.test:174` covers a
  generated-column ALTER combination that renames a base column and updates a
  generated expression to reference the new column.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database and create an InnoDB table with
  `base_value`, `adjust_value`, a stored generated column
  `stored_sum AS (base_value + adjust_value)`, a virtual generated column
  `virtual_product AS (base_value * adjust_value)`, and CHECK constraints that
  reference `base_value`,
- insert rows and verify the original generated aggregates and CHECK rejection,
- start a live ownerless peer so crashed-writer cleanup remains busy,
- start a writer that executes
  `ALTER TABLE app.ownerless_column_rename_expr_crash_base RENAME COLUMN base_value TO renamed_base`
  under the existing `dictionary-before-finish` fault,
- kill the writer at the hook,
- prove an ownerless opener returns `MYLITE_BUSY` while the live peer remains,
- release the peer and reopen ownerless read/write to rebuild volatile
  coordination,
- verify `base_value` is absent, `renamed_base` is present, old-name reads fail,
  generated stored/virtual values remain correct, and CHECK constraints reject
  invalid rows using the renamed column,
- verify later inserts through `renamed_base` recompute both generated columns
  and the final state survives ownerless reopen, native exclusive reopen,
  forced `.shm` rebuild, and native exclusive reopen after rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed column rename with
  generated-column and CHECK dependent expressions,
- live-peer cleanup-busy behavior and no-live rebuild,
- ownerless/native reopen of recovered renamed-column metadata and dependent
  expression behavior.

Out of scope:

- cross-column default-expression rename coverage,
- generated-column or CHECK expression exhaustiveness,
- foreign-key, partition, external directory, or tablespace detach/import DDL
  classes,
- randomized DDL oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for existing
ownerless compatibility with MariaDB table-definition ALTERs by proving that
dependent expressions rewritten by native MariaDB remain usable after a writer
dies at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises the existing ownerless process cleanup,
dictionary-generation recovery, `.shm` rebuild, and native exclusive reopen
lifecycle.

## Native Storage Impact

The covered DDL uses MariaDB/InnoDB's native ALTER TABLE machinery and table
definition files. MyLite does not reinterpret generated-column or CHECK
metadata; it proves ownerless reopen rebuilds volatile coordination around the
completed native ALTER.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-rename-expression-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata exposes `renamed_base` and does not expose `base_value`.
- Stored and virtual generated-column values remain correct after recovery and
  after new inserts through the renamed column.
- CHECK constraints still reject invalid rows after recovery.
- Ownerless and ordinary native reopen observe the same dependent-expression
  behavior before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic dependent-expression coverage, not exhaustive
  generated-column or CHECK rewrite exploration.
- Cross-column default-expression rename coverage remains out of scope pending
  focused MariaDB syntax evidence.
- Broader DDL/file lifecycle classes and full external randomized oracle stress
  remain planned.

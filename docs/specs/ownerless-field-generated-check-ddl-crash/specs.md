# Ownerless Field And Generated CHECK DDL Crash

## Problem Statement

Ownerless CHECK crash coverage already proves table-level
`ALTER TABLE ... ADD CONSTRAINT ... CHECK` and `DROP CONSTRAINT` recovery.
The remaining CHECK ADD gap is narrower: if a writer dies after MariaDB adds a
field-level CHECK expression and a CHECK expression that depends on a generated
column, but before MyLite publishes ownerless dictionary finish, no-live
recovery must preserve the completed native table definition and enforcement
state.

This slice adds deterministic crash-boundary evidence for field-level and
generated-column CHECK ADD metadata.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:6195` through
  `mariadb/sql/sql_yacc.yy:6208` parses `CHECK (...)` into a
  `Virtual_column_info`.
- `mariadb/sql/sql_yacc.yy:6244` through
  `mariadb/sql/sql_yacc.yy:6250` attaches optional field-level CHECK
  expressions to the parsed `Create_field`.
- `mariadb/sql/sql_yacc.yy:7999` through
  `mariadb/sql/sql_yacc.yy:8012` parses `ALTER TABLE ... MODIFY ... CHECK`
  and `ALTER TABLE ... ADD CONSTRAINT ... CHECK`, recording
  `ALTER_CHANGE_COLUMN` or `ALTER_ADD_CHECK_CONSTRAINT`.
- `mariadb/sql/sql_table.cc:4034` through
  `mariadb/sql/sql_table.cc:4098` validates generated, default,
  field-level CHECK, and table-level CHECK expressions.
- `mariadb/sql/sql_show.cc:2428` through
  `mariadb/sql/sql_show.cc:2438` prints field-level CHECK expressions inline
  in `SHOW CREATE TABLE`.
- `mariadb/sql/sql_show.cc:2601` through
  `mariadb/sql/sql_show.cc:2630` prints table-level CHECK expressions after
  fields and keys.
- `mariadb/sql/sql_show.cc:7668` through
  `mariadb/sql/sql_show.cc:7708` exposes both field-level and table-level
  CHECK expressions through `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`, with
  `LEVEL` set from `VCOL_CHECK_FIELD` versus `VCOL_CHECK_TABLE`.
- `mariadb/sql/table.cc:6616` through `mariadb/sql/table.cc:6658` enforces
  field and table CHECK constraints in `TABLE::verify_constraints()` and
  reports `ER_CONSTRAINT_FAILED`.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database with
  `app.ownerless_field_check_crash_base`, two rows, and a virtual generated
  column `generated_total AS (value + adjust_value)`,
- verify the table starts with no CHECK metadata,
- start a writer that executes
  `ALTER TABLE ... MODIFY value INT NOT NULL CHECK (value > 0), ADD
  CONSTRAINT ownerless_check_generated_total CHECK (generated_total >= value)`
  under the existing `dictionary-before-finish` hook,
- kill the writer after MariaDB DDL completes but before ownerless dictionary
  finish while a live ownerless peer keeps cleanup busy,
- release the peer and reopen ownerless read/write to rebuild volatile
  coordination,
- verify recovered `INFORMATION_SCHEMA.CHECK_CONSTRAINTS` contains the
  column-level `value` CHECK and the table-level generated-column CHECK,
- verify CHECK enforcement rejects invalid field-level and generated-column
  values with errno 4025 and accepts a valid row,
- verify ownerless and native exclusive reopen before and after forced `.shm`
  rebuild observe the same generated values, CHECK metadata, and enforcement.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed field-level CHECK
  ADD metadata,
- crash-at-dictionary-before-finish coverage for completed table-level CHECK
  ADD metadata that references an existing virtual generated column,
- live-peer cleanup-busy behavior and no-live rebuild,
- recovered CHECK metadata and enforcement through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- field-level CHECK DROP recovery,
- generated-column CHECK DROP recovery,
- generated-column creation recovery, already covered by generated-column DDL
  crash slices,
- partitioned tables, `check_constraint_checks=OFF`, and concurrent CHECK DDL
  conflicts,
- randomized DDL/RQG oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless CHECK
compatibility evidence by proving that a writer death at MyLite's dictionary
publication boundary preserves completed MariaDB field-level CHECK metadata and
generated-column CHECK enforcement.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises MariaDB table-definition storage,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

CHECK constraints and generated-column definitions are SQL-layer table metadata
over an InnoDB table. MyLite does not reinterpret the expressions; it proves
ownerless recovery rebuilds volatile coordination around MariaDB's completed
native table definition.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-field-generated-check-crash`.
- Run adjacent CHECK ADD/DROP crash selectors.
- Run the affected ownerless hook SQL shard.
- Build the non-hook embedded target.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata includes one column-level `value` CHECK and one
  table-level `ownerless_check_generated_total` CHECK in
  `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`.
- Invalid field-level and generated-column values fail with MariaDB errno 4025
  after recovery, while valid writes succeed.
- Ownerless and ordinary native reopen observe the same generated values and
  CHECK-constrained state before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic CHECK ADD crash coverage for one representative
  field-level and generated-column shape.
- CHECK DROP recovery for field-level/generated-column shapes and randomized
  CHECK DDL crash variants remain planned.
- Full external MariaDB/RQG long-running DDL stress remains planned.

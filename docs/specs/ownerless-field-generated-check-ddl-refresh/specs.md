# Ownerless Field And Generated CHECK DDL Refresh

## Problem Statement

Ownerless table-level CHECK refresh coverage proves an already-open peer sees
`ALTER TABLE ... ADD CONSTRAINT ... CHECK` and later `DROP CONSTRAINT`.
Field-level CHECK metadata and CHECK expressions that depend on generated
columns use the same MariaDB table-definition machinery but were still only
covered by crash recovery. Already-open ownerless peers must refresh those
metadata shapes too.

This slice adds deterministic peer-refresh coverage for field-level CHECK ADD
and DROP plus a table-level generated-column CHECK ADD and DROP.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:6195` through
  `mariadb/sql/sql_yacc.yy:6208` parses `CHECK (...)` into a
  `Virtual_column_info`.
- `mariadb/sql/sql_yacc.yy:6244` through
  `mariadb/sql/sql_yacc.yy:6250` attaches optional field-level CHECK
  expressions to parsed fields.
- `mariadb/sql/sql_yacc.yy:7999` through
  `mariadb/sql/sql_yacc.yy:8012` parses `ALTER TABLE ... MODIFY ... CHECK`
  and `ALTER TABLE ... ADD CONSTRAINT ... CHECK`.
- `mariadb/sql/sql_yacc.yy:8040` through
  `mariadb/sql/sql_yacc.yy:8049` parses `ALTER TABLE ... DROP CONSTRAINT`.
- `mariadb/sql/sql_table.cc:4034` through
  `mariadb/sql/sql_table.cc:4098` validates generated, field-level CHECK, and
  table-level CHECK expressions.
- `mariadb/sql/sql_show.cc:7668` through
  `mariadb/sql/sql_show.cc:7708` exposes CHECK metadata through
  `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`, using `LEVEL='Column'` for
  field-level checks and `LEVEL='Table'` for table-level checks.
- `mariadb/sql/table.cc:6616` through `mariadb/sql/table.cc:6658` enforces
  field and table CHECK constraints through `TABLE::verify_constraints()` and
  reports `ER_CONSTRAINT_FAILED`.

## Design

Add one ownerless SQL selector:

- keep a parent ownerless handle open before the table exists,
- have a child ownerless process create
  `app.ownerless_field_check_alter` with a virtual generated column
  `generated_total AS (value + adjust_value)`,
- have the child execute
  `ALTER TABLE ... MODIFY value INT NOT NULL CHECK (value > 0), ADD
  CONSTRAINT ownerless_check_generated_total CHECK (generated_total >= value)`,
- verify the already-open parent sees the generated column, the column-level
  `value` CHECK, and the table-level generated-column CHECK through
  `INFORMATION_SCHEMA`,
- verify invalid field-level and generated-column writes fail with errno 4025,
  while a valid row succeeds,
- have the child drop both CHECK constraints by modifying `value` without the
  field CHECK and dropping `ownerless_check_generated_total`,
- verify the already-open parent sees both CHECK entries as absent and can
  insert rows that would have failed while the constraints existed,
- verify final generated values and absent CHECK metadata through
  ownerless/native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- live ownerless peer refresh for representative field-level CHECK ADD/DROP,
- live ownerless peer refresh for a representative generated-column table CHECK
  ADD/DROP,
- final ownerless/native reopen and forced `.shm` rebuild state.

Out of scope:

- randomized CHECK DDL generation,
- partitioned tables, `check_constraint_checks=OFF`, and concurrent CHECK DDL
  conflicts,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless CHECK
compatibility evidence by proving already-open peers refresh MariaDB field-level
CHECK metadata and generated-column CHECK enforcement after another process
changes the native table definition.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises MariaDB table-definition storage,
ownerless dictionary refresh, ownerless/native reopen, and forced `.shm`
rebuild lifecycle.

## Native Storage Impact

CHECK constraints and generated-column definitions are SQL-layer table metadata
over an InnoDB table. MyLite does not reinterpret the expressions; it proves
ownerless peer refresh reopens MariaDB's completed native table definition.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `embedded-dev`.
- Run the focused selector:
  `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test field-generated-check-ddl`.
- Run the adjacent table-level `check-constraint-ddl` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the affected ownerless SQL shard.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The already-open parent sees the column-level `value` CHECK with
  `LEVEL='Column'`.
- The already-open parent sees the generated-column table CHECK with
  `LEVEL='Table'`.
- Invalid field-level and generated-column values fail with MariaDB errno 4025
  while the constraints exist.
- Formerly invalid rows succeed after the child drops both CHECK constraints.
- Ownerless and ordinary native reopen observe the same generated values and
  absent-CHECK state before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic live refresh coverage for one representative
  field-level and generated-column CHECK shape.
- Crash recovery for the same representative ADD and DROP shapes is covered by
  `docs/specs/ownerless-field-generated-check-ddl-crash/specs.md` and
  `docs/specs/ownerless-field-generated-check-drop-ddl-crash/specs.md`.
- Randomized CHECK DDL generation and long-running external MariaDB/RQG stress
  remain planned.

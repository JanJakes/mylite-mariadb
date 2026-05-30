# Ownerless CHECK Constraint DDL Refresh

## Problem

Ownerless DDL coverage proves peer refresh for several table-definition
classes, but it does not yet prove `ALTER TABLE` CHECK constraint add/drop
refresh across already-open ownerless peers. CHECK constraints are SQL-layer
table metadata that change user-visible write enforcement without introducing a
separate InnoDB foreign-key object or secondary index.

MyLite needs bounded evidence that an ownerless peer can observe CHECK
constraints added by another process, enforce them through MariaDB's native SQL
constraint path, observe the constraints being dropped, and keep the final
post-drop state durable through ownerless/native reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE ... ADD CONSTRAINT ...
  CHECK (...)`, `ALTER TABLE ... ADD CONSTRAINT IF NOT EXISTS ... CHECK (...)`,
  and `ALTER TABLE ... DROP CONSTRAINT ...`, setting
  `ALTER_ADD_CHECK_CONSTRAINT` or `ALTER_DROP_CHECK_CONSTRAINT`.
- `mariadb/sql/sql_table.cc` validates and names table CHECK constraints via
  `check_expression()` and `fix_constraints_names()`, handles
  `ADD CONSTRAINT IF NOT EXISTS`, copies retained constraints during ALTER, and
  removes dropped `Alter_drop::CHECK_CONSTRAINT` entries from the table
  definition.
- `mariadb/sql/table.cc:TABLE::verify_constraints()` evaluates field and table
  CHECK expressions on write when `check_constraint_checks` is enabled and
  reports `ER_CONSTRAINT_FAILED` / errno 4025 on violation.
- `mariadb/sql/sql_show.cc` includes table-level CHECK constraints in
  `SHOW CREATE TABLE` and exposes `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`
  records with `CONSTRAINT_SCHEMA`, `TABLE_NAME`, `CONSTRAINT_NAME`, `LEVEL`,
  and `CHECK_CLAUSE`.
- `packages/libmylite/src/database.cc` treats ownerless `ALTER TABLE` as
  dictionary DDL: it publishes an odd dictionary generation while DDL is active
  and a stable even generation after execution, causing peers to refresh SQL
  table metadata and evict unused InnoDB dictionary entries before proceeding.

## Scope And Non-Goals

- Add a focused ownerless selector for table-level `ALTER TABLE ... ADD
  CONSTRAINT ... CHECK` followed by `ALTER TABLE ... DROP CONSTRAINT`.
- Verify an already-open ownerless peer observes the added constraints through
  `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`.
- Verify invalid rows fail with MariaDB errno 4025 while the constraints exist.
- Verify valid rows still succeed while the constraints exist.
- Verify the same already-open peer observes the constraints as absent after
  peer `DROP CONSTRAINT`, and can then insert the formerly invalid row shape.
- Verify final rows and absent-CHECK metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.
- Do not cover field-level CHECK constraints, generated-column CHECK
  expressions, partitioning, `check_constraint_checks=OFF`, concurrent CHECK
  DDL conflicts, crash recovery during CHECK ALTER, or SQL-level table-lock
  fault injection.

## Design

- Add `check-constraint-ddl` to
  `mylite_ownerless_cross_process_sql_test`.
- A child ownerless process creates an InnoDB table without CHECK constraints,
  inserts a baseline row, then adds two named table-level CHECK constraints:
  one on a numeric value and one on a label length expression.
- The parent keeps an ownerless handle open before those ALTERs, observes both
  constraints in `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`, verifies each
  constraint can reject an invalid row with errno 4025, and inserts a valid
  row.
- The child drops both named CHECK constraints. The parent observes metadata
  absence and inserts the formerly invalid row shape.
- Final helper assertions verify row totals, the post-drop invalid-shape row,
  and absent CHECK metadata through ownerless/native reopen before and after
  forced shared-memory rebuild.

## Compatibility Impact

This extends ownerless DDL evidence to representative table-level CHECK
constraint ALTER behavior. It does not claim the full CHECK constraint matrix
or crash/fault coverage during constraint ALTER.

## Directory And Lifecycle Impact

No new files or layout changes. The slice exercises existing MariaDB table
definition metadata and ownerless dictionary-generation refresh under the
MyLite-owned database directory.

## Native Storage Impact

No native storage format changes. The test intentionally uses MariaDB's native
CHECK constraint metadata and enforcement paths over an InnoDB table.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `check-constraint-ddl` selector.
- Build and run the focused `check-constraint-ddl` selector in
  `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage, ownerless stress, and the
  hook ownerless SQL label.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Already-open ownerless peers see CHECK constraints added by another process.
- Invalid writes fail with errno 4025 while the constraints exist and valid
  writes succeed.
- Already-open ownerless peers see the constraints removed after peer
  `DROP CONSTRAINT`.
- The formerly invalid row shape can be inserted after drop.
- Final rows and absent-CHECK state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- Field-level CHECK constraints and CHECK expressions tied to generated columns
  remain separate DDL coverage.
- Crash recovery during CHECK ALTER and external oracle stress remain broader
  DDL/recovery work.

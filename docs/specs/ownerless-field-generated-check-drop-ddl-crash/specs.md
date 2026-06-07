# Ownerless Field And Generated CHECK DROP DDL Crash

## Problem Statement

Field-level and generated-column CHECK ADD crash coverage proves no-live
recovery preserves completed CHECK metadata after a writer dies before MyLite
publishes ownerless dictionary finish. The matching DROP boundary also needs
evidence: if MariaDB removes the field-level CHECK and generated-column CHECK
metadata but the writer dies before ownerless dictionary finish, recovery must
not resurrect stale CHECK enforcement.

This slice adds deterministic crash-boundary evidence for field-level and
generated-column CHECK DROP metadata.

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
  and table-level CHECK ADD clauses.
- `mariadb/sql/sql_yacc.yy:8040` through
  `mariadb/sql/sql_yacc.yy:8049` parses `ALTER TABLE ... DROP CONSTRAINT`.
- `mariadb/sql/sql_table.cc:4034` through
  `mariadb/sql/sql_table.cc:4098` validates generated, field-level CHECK, and
  table-level CHECK expressions.
- `mariadb/sql/sql_table.cc:9471` through
  `mariadb/sql/sql_table.cc:9486` removes dropped table-level CHECK
  constraints while copying retained CHECK definitions during ALTER.
- `mariadb/sql/sql_show.cc:7668` through
  `mariadb/sql/sql_show.cc:7708` exposes CHECK metadata through
  `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`, using `LEVEL='Column'` for
  field-level checks and `LEVEL='Table'` for table-level checks.
- `mariadb/sql/table.cc:6616` through `mariadb/sql/table.cc:6658` enforces
  field and table CHECK constraints through `TABLE::verify_constraints()` and
  reports `ER_CONSTRAINT_FAILED`.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database with
  `app.ownerless_field_check_drop_crash_base`, a column-level
  `CHECK (value > 0)`, a virtual generated column
  `generated_total AS (value + adjust_value)`, a table-level
  `CHECK (generated_total >= value)`, and two valid rows,
- verify both CHECK metadata entries exist and reject invalid field-level and
  generated-column values,
- start a writer that executes
  `ALTER TABLE ... MODIFY value INT NOT NULL, DROP CONSTRAINT
  ownerless_check_generated_drop_total` under the existing
  `dictionary-before-finish` hook,
- kill the writer after MariaDB DDL completes but before ownerless dictionary
  finish while a live peer keeps cleanup busy,
- reopen ownerless read/write after no-live recovery,
- verify both CHECK metadata entries are absent,
- verify formerly invalid field-level and generated-column values now insert
  successfully,
- verify ownerless and native exclusive reopen before and after forced `.shm`
  rebuild observe the same absent-CHECK metadata and generated values.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed field-level CHECK
  DROP metadata,
- crash-at-dictionary-before-finish coverage for completed generated-column
  table CHECK DROP metadata,
- live-peer cleanup-busy behavior and no-live rebuild,
- recovered absent CHECK metadata and post-drop writes through
  ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- randomized CHECK DDL generation,
- partitioned tables, `check_constraint_checks=OFF`, and concurrent CHECK DDL
  conflicts,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless CHECK
compatibility evidence by proving that a writer death at MyLite's dictionary
publication boundary preserves completed MariaDB CHECK DROP state for
field-level and generated-column CHECK shapes.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises MariaDB table-definition storage,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

CHECK constraints and generated-column definitions are SQL-layer table metadata
over an InnoDB table. MyLite does not reinterpret the expressions; it proves
ownerless recovery rebuilds volatile coordination around MariaDB's completed
native table definition after the CHECK definitions have been removed.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-field-generated-check-drop-crash`.
- Run adjacent field/generated CHECK ADD crash and table-level CHECK DROP
  crash selectors.
- Run the affected ownerless hook SQL shard.
- Build the non-hook embedded target.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata excludes both the column-level `value` CHECK and the
  table-level `ownerless_check_generated_drop_total` CHECK from
  `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`.
- Formerly invalid field-level and generated-column values succeed after
  recovery.
- Ownerless and ordinary native reopen observe the same generated values and
  absent-CHECK state before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic CHECK DROP crash coverage for one representative
  field-level and generated-column shape.
- Randomized CHECK DDL generation and long-running external MariaDB/RQG stress
  remain planned.

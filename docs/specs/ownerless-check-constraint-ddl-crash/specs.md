# Ownerless CHECK Constraint DDL Crash

## Problem Statement

Ownerless CHECK constraint refresh coverage proves already-open peers observe
`ALTER TABLE ... ADD CONSTRAINT ... CHECK` and later `DROP CONSTRAINT`. The
crash boundary still needed focused evidence: if a writer dies after MariaDB
has accepted native table-definition changes but before MyLite publishes
ownerless dictionary finish, no-live recovery must preserve the completed CHECK
metadata and enforcement state.

This slice adds deterministic crash-boundary evidence for completed CHECK
constraint ADD DDL.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8005` through
  `mariadb/sql/sql_yacc.yy:8012` parses `ALTER TABLE ... ADD CONSTRAINT ...
  CHECK (...)` and records `ALTER_ADD_CHECK_CONSTRAINT`.
- `mariadb/sql/sql_yacc.yy:8040` through
  `mariadb/sql/sql_yacc.yy:8049` parses `ALTER TABLE ... DROP CONSTRAINT`
  as `Alter_drop::CHECK_CONSTRAINT` and records
  `ALTER_DROP_CHECK_CONSTRAINT`.
- `mariadb/sql/sql_table.cc:4034` through
  `mariadb/sql/sql_table.cc:4051` validates field generated, default, and
  field-level CHECK expressions during table definition processing.
- `mariadb/sql/sql_table.cc:4055` through
  `mariadb/sql/sql_table.cc:4098` validates table-level CHECK constraints,
  including duplicate constraint-name detection.
- `mariadb/sql/sql_table.cc:6438` through
  `mariadb/sql/sql_table.cc:6449` resolves requested CHECK constraint drops
  against the current table CHECK constraint list.
- `mariadb/sql/sql_table.cc:11163` through
  `mariadb/sql/sql_table.cc:11221` rewrites `DROP CONSTRAINT` requests that
  actually target foreign keys or unique keys and retains CHECK drops as
  `ALTER_DROP_CHECK_CONSTRAINT`.
- `mariadb/sql/table.cc:6616` through `mariadb/sql/table.cc:6658` enforces
  table and field CHECK constraints through `TABLE::verify_constraints()` and
  reports `ER_CONSTRAINT_FAILED`.
- `mariadb/sql/sql_show.cc:10835` through `mariadb/sql/sql_show.cc:10838`
  exposes `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`.
- `mariadb/sql/handler.h:720` through `mariadb/sql/handler.h:721` defines the
  CHECK ADD/DROP ALTER flags used by SQL and handler layers.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database with
  `app.ownerless_check_crash_base` and two CHECK-satisfying rows,
- verify the table starts without the target CHECK metadata,
- keep a live ownerless peer open while a later ownerless opener finishes
  dictionary recovery,
- start a writer that executes
  `ALTER TABLE app.ownerless_check_crash_base ADD CONSTRAINT ... CHECK ...`
  for two named table-level CHECK constraints under the existing
  `dictionary-before-finish` hook,
- kill the writer after MariaDB DDL completes but before ownerless dictionary
  finish,
- prove ownerless recovery succeeds while the live peer remains open and the
  native file-operation marker stays retained,
- release the peer and prove final no-live recovery drains the marker,
- verify recovered `information_schema.check_constraints` metadata contains
  both table-level CHECK constraints,
- verify CHECK enforcement rejects invalid rows with errno 4025 and accepts a
  valid row,
- verify ownerless and native exclusive reopen before and after forced `.shm`
  rebuild observe the same CHECK metadata, retained rows, and enforcement.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed table-level CHECK
  constraint ADD DDL,
- live-peer cleanup-busy behavior and no-live rebuild,
- recovered CHECK metadata and enforcement through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- partitioned tables, `check_constraint_checks=OFF`, and concurrent CHECK DDL
  conflicts,
- randomized DDL/RQG oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for existing
ownerless CHECK compatibility by proving that a writer death at MyLite's
dictionary publication boundary preserves completed native CHECK ADD metadata
and write-time enforcement.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises MariaDB table-definition storage,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

CHECK constraints are SQL-layer table-definition metadata over an InnoDB table.
MyLite does not reinterpret the CHECK expressions; it proves ownerless recovery
rebuilds volatile coordination around MariaDB's completed native table
definition.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-check-constraint-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- Live-peer recovery succeeds while the native file-operation marker remains
  retained until final no-live recovery.
- Recovered metadata includes both named CHECK constraints in
  `information_schema.check_constraints` with table-level scope.
- Invalid rows fail with MariaDB errno 4025 after recovery, while valid writes
  succeed.
- Ownerless and ordinary native reopen observe the same CHECK-constrained state
  before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic CHECK ADD crash coverage; DROP CHECK crash recovery is
  covered separately by
  `docs/specs/ownerless-check-constraint-drop-ddl-crash/specs.md`.
- Live-peer recovery for standalone CHECK ADD/DROP is covered by
  `docs/specs/ownerless-check-constraint-live-recovery/specs.md`.
- Field-level and generated-expression CHECK ADD crash coverage is covered
  separately by
  `docs/specs/ownerless-field-generated-check-ddl-crash/specs.md`.
- Randomized CHECK DDL crash variants remain separate candidate slices.
- Full external MariaDB/RQG long-running DDL stress remains planned.

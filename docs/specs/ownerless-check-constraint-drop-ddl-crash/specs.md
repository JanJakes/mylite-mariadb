# Ownerless CHECK Constraint DROP DDL Crash

## Problem Statement

Ownerless CHECK constraint refresh coverage proves already-open peers observe
`ALTER TABLE ... ADD CONSTRAINT ... CHECK` and later `DROP CONSTRAINT`. CHECK
ADD crash coverage proves a writer death after native table-definition mutation
but before ownerless dictionary finish preserves completed CHECK metadata and
enforcement. The matching DROP boundary also needs focused evidence: if a
writer dies after MariaDB removes CHECK metadata but before MyLite publishes
ownerless dictionary finish, no-live recovery must preserve the completed DROP
state instead of resurrecting stale CHECK metadata.

This slice adds deterministic crash-boundary evidence for completed CHECK
constraint DROP DDL.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8040` through
  `mariadb/sql/sql_yacc.yy:8049` parses `ALTER TABLE ... DROP CONSTRAINT`
  as `Alter_drop::CHECK_CONSTRAINT` and records
  `ALTER_DROP_CHECK_CONSTRAINT`.
- `mariadb/sql/sql_table.cc:6438` through
  `mariadb/sql/sql_table.cc:6449` resolves requested CHECK constraint drops
  against the current table CHECK constraint list.
- `mariadb/sql/sql_table.cc:9471` through
  `mariadb/sql/sql_table.cc:9486` removes dropped table-level CHECK
  constraints while copying retained table CHECK definitions during ALTER.
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
  `app.ownerless_check_drop_crash_base`, two named table-level CHECK
  constraints, and two CHECK-satisfying rows,
- verify the table starts with both target CHECK constraints and rejects an
  invalid row with MariaDB errno 4025,
- keep a live ownerless peer open so crashed-writer cleanup remains busy,
- start a writer that executes
  `ALTER TABLE app.ownerless_check_drop_crash_base DROP CONSTRAINT ...` for
  both named table-level CHECK constraints under the existing
  `dictionary-before-finish` hook,
- kill the writer after MariaDB DDL completes but before ownerless dictionary
  finish,
- prove ownerless open returns `MYLITE_BUSY` while the live peer remains,
- release the peer and reopen ownerless read/write to rebuild volatile
  coordination,
- verify recovered `information_schema.check_constraints` metadata no longer
  contains either CHECK constraint,
- verify the formerly invalid row shape now succeeds,
- verify ownerless and native exclusive reopen before and after forced `.shm`
  rebuild observe the same absent-CHECK metadata and rows.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed table-level CHECK
  constraint DROP DDL,
- live-peer cleanup-busy behavior and no-live rebuild,
- recovered absent CHECK metadata and post-drop writes through ownerless/native
  reopen before and after forced `.shm` rebuild.

Out of scope:

- field-level CHECK constraints, generated-column CHECK expressions,
  partitioned tables, `check_constraint_checks=OFF`, and concurrent CHECK DDL
  conflicts,
- randomized DDL/RQG oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for existing
ownerless CHECK compatibility by proving that a writer death at MyLite's
dictionary publication boundary preserves completed native CHECK DROP metadata
and the resulting absence of write-time CHECK enforcement.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises MariaDB table-definition storage,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

CHECK constraints are SQL-layer table-definition metadata over an InnoDB table.
MyLite does not reinterpret the CHECK expressions; it proves ownerless recovery
rebuilds volatile coordination around MariaDB's completed native table
definition after the CHECK definitions have been removed.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-check-constraint-drop-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata excludes both named CHECK constraints from
  `information_schema.check_constraints`.
- Formerly invalid rows succeed after recovery.
- Ownerless and ordinary native reopen observe the same post-DROP state before
  and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic CHECK DROP crash coverage; CHECK ADD crash recovery is
  covered separately by `docs/specs/ownerless-check-constraint-ddl-crash/specs.md`.
- Field-level and generated-expression CHECK ADD crash coverage is covered by
  `docs/specs/ownerless-field-generated-check-ddl-crash/specs.md`.
- Field-level/generated-expression CHECK DROP and randomized CHECK DDL crash
  variants remain separate candidate slices.
- Full external MariaDB/RQG long-running DDL stress remains planned.

# Ownerless Cyclic Foreign-Key Truncate Negative Proof

## Problem Statement

Ownerless truncate recovery now covers plain, implicit, child-only foreign-key,
self-referencing foreign-key, and generated-column child foreign-key
`TRUNCATE TABLE` shapes. The remaining cyclic foreign-key truncate item should
not be treated as a positive live recovery boundary unless MariaDB can reach
native truncate/recreate.

MariaDB's SQL-layer foreign-key parent check rejects truncating a table that is
referenced by a different table. In a cyclic multi-table foreign-key graph,
each table is a parent for at least one non-self-referencing child table, so
`TRUNCATE TABLE` should fail before native InnoDB file lifecycle begins. MyLite
needs focused ownerless evidence that this error path remains explicit, leaves
both native file-operation markers clear, preserves cyclic metadata and rows,
and does not require dead-owner dictionary recovery.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_truncate.cc:119` implements
  `fk_truncate_illegal_if_parent()`. It returns success only when the target
  table is not referenced by a foreign key or when every referencing
  relationship is self-referencing.
- `mariadb/sql/sql_truncate.cc:150-160` treats any referencing child table
  whose schema/table name differs from the target as
  `ER_TRUNCATE_ILLEGAL_FK`.
- `mariadb/sql/sql_truncate.cc:230-240` calls that parent-FK check before
  storage-engine truncate for ordinary `FOREIGN_KEY_CHECKS=1` statements.
- `mariadb/sql/sql_truncate.cc:510-532` reaches `dd_recreate_table()` or
  `handler_truncate()` only after table locking and preflight checks succeed.
- Existing ownerless cyclic foreign-key SQL coverage already creates two-table,
  three-table, and nullable cyclic graphs and proves referential actions,
  metadata refresh, ownerless/native reopen, and forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Add a focused production selector for cyclic FK truncate rejection.
- Create a two-table cyclic InnoDB graph under ownerless read/write mode.
- Attempt `TRUNCATE TABLE` on each table and require MariaDB errno
  `ER_TRUNCATE_ILLEGAL_FK`.
- Verify both native file-operation markers remain clear and ownerless WAL
  stays checkpointed.
- Verify cyclic rows, referential metadata, and enforcement remain intact
  through ownerless reopen, forced `.shm` rebuild, ordinary native reopen, and
  a follow-up valid cyclic insert/delete.

Out of scope:

- Positive crash recovery for cyclic FK truncate under default FK checks;
  MariaDB does not reach native truncate in that mode.
- `FOREIGN_KEY_CHECKS=0` truncate behavior, covered separately by
  `docs/specs/ownerless-unchecked-cyclic-foreign-key-truncate-live-recovery/specs.md`.
- Partition or temporary-table truncate behavior.
- Broader randomized DDL/RQG coverage.

## Design

Add selector `cyclic-foreign-key-truncate-policy`. The test creates two
InnoDB tables with a cycle:

```sql
CREATE TABLE app.ownerless_fk_truncate_cycle_a (
  id INT NOT NULL PRIMARY KEY,
  b_id INT NULL,
  value INT NOT NULL,
  INDEX ownerless_fk_truncate_cycle_a_b_idx (b_id)
) ENGINE=InnoDB;

CREATE TABLE app.ownerless_fk_truncate_cycle_b (
  id INT NOT NULL PRIMARY KEY,
  a_id INT NOT NULL,
  value INT NOT NULL,
  INDEX ownerless_fk_truncate_cycle_b_a_idx (a_id),
  CONSTRAINT ownerless_fk_truncate_cycle_b_a
    FOREIGN KEY (a_id)
    REFERENCES app.ownerless_fk_truncate_cycle_a (id)
) ENGINE=InnoDB;

ALTER TABLE app.ownerless_fk_truncate_cycle_a
  ADD CONSTRAINT ownerless_fk_truncate_cycle_a_b
  FOREIGN KEY (b_id)
  REFERENCES app.ownerless_fk_truncate_cycle_b (id);
```

After inserting one valid cycle, the test checkpoints clean state, attempts to
truncate both tables under ownerless read/write mode, and asserts the MariaDB
error is the foreign-key truncate rejection. The test then verifies no native
file-operation marker was set, no page-version WAL was retained, cyclic rows
and metadata remain present, and ordinary FK enforcement still rejects
missing-parent rows.

No product code or recovery classifier change is expected. If the test shows
that MyLite reaches native file lifecycle before the MariaDB error, that would
be a product bug to fix before the slice can close.

## Compatibility Impact

This records MariaDB-compatible negative behavior for an unsupported positive
recovery boundary. It narrows the truncate roadmap by moving cyclic FK truncate
from "positive recovery planned" to "covered MariaDB pre-truncate error path"
for default ownerless SQL.

## Directory And Lifecycle Impact

No directory layout changes. The test proves the error path leaves no durable
ownerless native file-operation marker and requires no dead-owner recovery
work.

## Native Storage Impact

Native InnoDB truncate/recreate should not run. Existing cyclic rows and
foreign-key metadata remain native InnoDB state.

## Public API, Build, Size, License

No public API, dependency, license, or binary-profile changes. The slice adds
one focused ownerless SQL selector, one CTest entry, and documentation.

## Test And Verification Plan

- Add selector `cyclic-foreign-key-truncate-policy`.
- Register production CTest
  `libmylite.ownerless-cyclic-foreign-key-truncate-policy`.
- Run the selector directly in `php-embedded-prod` and `ownerless-test-hooks`.
- Run adjacent cyclic FK selectors:
  `cyclic-foreign-key` and `cyclic-foreign-key-variants`.
- Run adjacent FK truncate hook selectors to ensure positive truncate recovery
  remains intact.
- Run ownerless DDL stress smoke, production build guards, format check, and
  `git diff --check`.

## Acceptance Criteria

- `TRUNCATE TABLE` on both cyclic FK tables fails with MariaDB errno
  `ER_TRUNCATE_ILLEGAL_FK`.
- Both native file-operation markers remain clear after the failed statements.
- Ownerless WAL remains checkpointed.
- Cyclic rows and `information_schema.referential_constraints` metadata remain
  intact.
- Ownerless reopen, forced `.shm` rebuild, and ordinary native reopen preserve
  the same state and allow valid cyclic insert/delete work.
- Positive FK truncate recovery selectors continue to pass.

## Verification Results

Completed on 2026-06-27 with `php-embedded-prod`, `ownerless-test-hooks`, and
`ownerless-stress` builds:

- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test cyclic-foreign-key-truncate-policy`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cyclic-foreign-key-truncate-policy$' --output-on-failure`
- Production adjacent selectors:
  `cyclic-foreign-key` and `cyclic-foreign-key-variants`.
- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test cyclic-foreign-key-truncate-policy`
- Hook adjacent CTests:
  `libmylite.ownerless-dictionary-foreign-key-child-truncate-crash` and
  `libmylite.ownerless-dictionary-foreign-key-truncate-variants-crash`.
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- Guards:
  `tools/check-ci-production-builds`,
  `tools.ci-production-builds`, `format-check`, and `git diff --check`.

## Risks And Follow-Up

- `FOREIGN_KEY_CHECKS=0` reaches a native truncate boundary and is covered by
  `docs/specs/ownerless-unchecked-cyclic-foreign-key-truncate-live-recovery/specs.md`.
- Temporary-table truncate and broader DDL file-lifecycle recovery remain open.
- Broader native redo/checkpoint reconciliation, active-reader pressure
  crash/oracle breadth, and external MariaDB/RQG stress remain ownerless
  completion work.

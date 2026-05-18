# Autoincrement Grouped ODKU CHECK-Constraint Errors

## Goal

Cover grouped later-in-key `AUTO_INCREMENT` allocation when
`INSERT ... ON DUPLICATE KEY UPDATE` (ODKU) reaches a duplicate-update branch
that fails a `CHECK` constraint. MyLite must roll back rows inserted earlier in
the same statement and keep grouped allocation tied to the live per-prefix
maximum.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:2214-2294` handles the ODKU duplicate-update
  branch by evaluating update values, checking view/table constraints, and only
  then calling `ha_update_row()`.
- `mariadb/sql/table.cc:6616-6667` implements `TABLE::verify_constraints()` and
  returns `VIEW_CHECK_ERROR` when a non-ignored `CHECK` constraint evaluates
  false or raises an error.
- `mariadb/storage/mylite/ha_mylite.cc:1805-1865` reserves one generated value
  at a time for grouped autoincrement definitions.
- `mariadb/storage/mylite/ha_mylite.cc:3293-3365` derives grouped next values
  from live index entries in the current prefix.

## Scope

- Durable MyLite-routed grouped autoincrement tables requested as
  `ENGINE=InnoDB` and `ENGINE=MyISAM`.
- Direct multi-row `INSERT ... VALUES ... ON DUPLICATE KEY UPDATE` where an
  earlier row inserts, a later row enters the duplicate-update branch, and the
  update expression violates a named `CHECK` constraint.
- Prepared multi-row `INSERT ... VALUES ... ON DUPLICATE KEY UPDATE` with the
  same `CHECK` failure shape.
- Direct source-driven `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` with the
  same `CHECK` failure shape.
- Close/reopen allocation from live prefix maxima after the failed statements.

## Non-Goals

- Trigger, view, partition, offset/increment, integer-width, multi-table, or
  exhaustive `CHECK` expression matrices.
- Changing first-key autoincrement ODKU reservation behavior.
- Native MyISAM/InnoDB sidecar storage behavior.
- Size-profile reduction work.

## Compatibility Impact

This narrows the broader grouped ODKU expression-error matrix gap. When a
duplicate-update branch fails a table `CHECK` constraint after an earlier row
was inserted, MyLite rolls back the visible row and index changes, leaves the
duplicate target row unchanged, and derives the next grouped value from the
current live prefix maximum.

The claim remains representative. Trigger, view, partition, offset/increment,
integer-width, multi-table, and exhaustive `CHECK` expression variants remain
planned.

## Design

No production change is expected. The tests rely on MariaDB constraint
verification and MyLite statement checkpoints. The grouped autoincrement path
recomputes the next value from live index entries after rollback, so failed
duplicate-update constraint checks do not leave a durable first-key-style
reservation gap.

The test uses a named table-level `CHECK (score <= 100)`. The attempted
duplicate insert row uses `score = 99`, which is valid before duplicate-key
handling. The ODKU branch then evaluates `score = VALUES(score) + 2`, producing
`101` and failing the constraint on the duplicate-update path.

## File Lifecycle

No file-format or companion-file change is introduced. Durable table state
remains in the primary `.mylite` file and tests keep the existing no durable
sidecar checks.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct and prepared SQL execution plus close/reopen lifecycle checks.

## Storage-Engine Routing

Coverage uses requested `ENGINE=InnoDB` and `ENGINE=MyISAM`, which route to
MyLite storage in the storage-smoke profile. Native engine sidecars are not
created or claimed.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add direct grouped ODKU `CHECK` failure coverage.
- Add prepared grouped ODKU `CHECK` failure coverage.
- Add source-driven grouped ODKU `CHECK` failure coverage.
- Verify inserted rows from failed statements are rolled back, duplicate target
  rows are restored, secondary index entries are not left behind, next generated
  values resume from live prefix maxima, and close/reopen preserves those
  values.
- Run the focused storage-engine smoke test, statement-rollback,
  check-constraint, and routed-DDL/DML compatibility harness groups, shell
  syntax checks, reject-file cleanup checks, and `git diff --check`.

## Acceptance Criteria

- Failed direct grouped ODKU `CHECK` updates remove rows inserted earlier in the
  same statement.
- Failed prepared grouped ODKU `CHECK` updates remove rows inserted earlier in
  the same statement.
- Failed source-driven grouped ODKU `CHECK` updates remove rows inserted
  earlier in the same statement.
- Duplicate target rows remain unchanged after the constraint failure.
- Generated grouped values resume from live per-prefix maxima before and after
  close/reopen.

## Risks And Open Questions

- Trigger and view ODKU paths may follow distinct SQL-layer ordering and remain
  separate work.
- This remains representative `CHECK` failure coverage, not an exhaustive
  constraint expression matrix.

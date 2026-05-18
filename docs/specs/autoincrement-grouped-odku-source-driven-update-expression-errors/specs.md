# Autoincrement Grouped ODKU Source-Driven Update-Expression Errors

## Goal

Cover source-driven `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` (ODKU)
failure paths on routed grouped later-in-key `AUTO_INCREMENT` tables when a
duplicate-update expression errors after an earlier source row has inserted.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:2214-2351` attempts `ha_write_row()` first and,
  on duplicate-key errors, evaluates the ODKU update list before calling
  `ha_update_row()` for the conflicting row.
- `mariadb/sql/sql_insert.cc:4476-4529` executes selected source rows for
  `INSERT ... SELECT`, stores selected values into the target record, writes
  the row, and resets the autoincrement field before the next selected row.
- `mariadb/sql/sql_insert.cc:4708-4748` rolls back `select_insert`
  result-set failures, including partially inserted rows from the same
  source-driven statement.
- `mariadb/storage/mylite/ha_mylite.cc:1805-1865` reserves only one generated
  value at a time for grouped autoincrement definitions.
- `mariadb/storage/mylite/ha_mylite.cc:3293-3365` derives grouped next values
  from live index entries in the current prefix.

## Scope

- Durable MyLite-routed grouped autoincrement tables requested as
  `ENGINE=InnoDB` and `ENGINE=MyISAM`.
- Direct source-driven grouped ODKU where the first selected row inserts, a
  later selected row duplicates an existing key, and the ODKU update
  expression fails with a scalar-subquery cardinality error.
- Prepared source-driven grouped ODKU with the same failure shape.
- Close/reopen allocation from live prefix maxima after the failed statements.

## Non-Goals

- Exhaustive grouped ODKU trigger, view, partition, offset/increment,
  integer-width, or prepared parameter-shape matrices.
- Source-read failures that occur before the target handler is called, which
  are covered separately in
  `docs/specs/autoincrement-grouped-odku-source-read-errors/specs.md`.
- Native MyISAM/InnoDB sidecar storage behavior.
- Changing first-key autoincrement ODKU reservation behavior.
- Size-profile reduction work.

## Compatibility Impact

This narrows the grouped ODKU source-driven error gap. When a source-driven
grouped ODKU statement reaches the duplicate-update branch and the update
expression fails after an earlier selected row inserted, MyLite rolls back the
inserted row and restores the duplicate target row. Because grouped
autoincrement is derived from live per-prefix maxima rather than a durable
reserved tail, the next generated value resumes from the current live prefix
maximum.

The claim remains representative. Broader trigger, view, offset/increment,
integer-width, and prepared parameter-shape matrices remain planned.

## Design

No production change is expected. Existing statement checkpoints roll back row
and index changes. The grouped autoincrement path recomputes the next value
from live index entries after rollback, so failed source-driven statements do
not leave first-key-style reservation gaps.

The test uses a scalar subquery that returns more than one row in the ODKU
update expression. That creates a deterministic SQL-layer error after the
duplicate branch starts, without relying on disabled server or native-engine
surfaces.

## File Lifecycle

No file-format or companion-file change is introduced. Durable table state
remains in the primary `.mylite` file and the tests keep the existing no
durable sidecar checks.

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

- Add storage-engine smoke coverage for direct source-driven grouped ODKU
  update-expression failure after one successful generated source row.
- Add prepared source-driven grouped ODKU coverage for the same failure shape.
- Verify inserted rows from the failed statement are rolled back, duplicate
  target rows are restored, next generated values resume from live prefix
  maxima, and close/reopen preserves those values.
- Run the focused storage-engine test, statement-rollback and routed-DDL/DML
  compatibility harness groups, shell syntax checks, reject-file cleanup
  checks, and `git diff --check`.

## Acceptance Criteria

- Failed direct source-driven grouped ODKU removes rows inserted earlier in the
  same statement.
- Failed prepared source-driven grouped ODKU removes rows inserted earlier in
  the same statement.
- Duplicate-update expression errors leave the duplicate target row unchanged.
- Generated grouped values resume from live per-prefix maxima before and after
  close/reopen.

## Risks And Open Questions

- Source-read failures before target handler write are covered separately;
  triggers, views, and generated-column update expressions may follow different
  SQL-layer ordering and remain separate work.

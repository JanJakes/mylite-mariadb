# Autoincrement Grouped ODKU Source-Read Errors

## Goal

Cover representative source-read failure paths for source-driven
`INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` (ODKU) on routed grouped
later-in-key `AUTO_INCREMENT` tables, where a selected row errors before the
target handler is called after an earlier source row has inserted.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_select.cc:23773-23967` drives selected rows through the
  query executor and reports result-send errors back to the caller.
- `mariadb/sql/sql_insert.cc:4476-4504` handles each selected row in
  `select_insert::send_data()`, stores selected values first, and only calls
  `write->write_record()` if value storage succeeds.
- `mariadb/sql/sql_insert.cc:4540-4555` evaluates selected values into the
  target record in `select_insert::store_values()`, so expression errors in the
  selected row occur before the target handler writes that row.
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
- Direct source-driven grouped ODKU where the first selected row inserts and a
  later selected row fails while evaluating a scalar subquery in the SELECT
  list before the target handler is called.
- Prepared source-driven grouped ODKU with the same failure shape.
- Close/reopen allocation from live prefix maxima after the failed statements.

## Non-Goals

- Exhaustive trigger, view, partition, offset/increment, integer-width,
  generated-column, or prepared parameter-shape matrices.
- Duplicate-update expression errors, which are covered separately in
  `docs/specs/autoincrement-grouped-odku-source-driven-update-expression-errors/specs.md`.
- Native MyISAM/InnoDB sidecar storage behavior.
- Changing first-key autoincrement source-driven reservation behavior.
- Size-profile reduction work.

## Compatibility Impact

This narrows the grouped ODKU source-read error gap. When source evaluation
fails after an earlier selected row inserted, MyLite rolls back the inserted row
and leaves the duplicate target row unchanged. Because grouped autoincrement is
derived from live per-prefix maxima rather than a durable reserved tail, the
next generated value resumes from the current live prefix maximum.

The claim remains representative. Broader trigger, view, generated-column,
offset/increment, integer-width, and prepared parameter-shape matrices remain
planned.

## Design

No production change is expected. Existing statement checkpoints roll back row
and index changes published before the source-read error. The grouped
autoincrement path recomputes the next value from live index entries after
rollback, so failed source-driven statements do not leave first-key-style
reservation gaps.

The test uses a correlated scalar subquery in the SELECT list. It returns one
row for the first source row and more than one row for the second source row.
That creates a deterministic SQL-layer source-read error before the target
handler writes the second row, while still requiring rollback of the first row.

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

- Add storage-engine smoke coverage for direct source-driven grouped ODKU
  source-read failure after one successful generated source row.
- Add prepared source-driven grouped ODKU coverage for the same failure shape.
- Verify inserted rows from the failed statement are rolled back, duplicate
  target rows are unchanged, next generated values resume from live prefix
  maxima, and close/reopen preserves those values.
- Run the focused storage-engine test, statement-rollback and routed-DDL/DML
  compatibility harness groups, shell syntax checks, reject-file cleanup
  checks, and `git diff --check`.

## Acceptance Criteria

- Failed direct source-driven grouped ODKU source reads remove rows inserted
  earlier in the same statement.
- Failed prepared source-driven grouped ODKU source reads remove rows inserted
  earlier in the same statement.
- Source-read errors leave the pre-existing duplicate target row unchanged.
- Generated grouped values resume from live per-prefix maxima before and after
  close/reopen.

## Risks And Open Questions

- Trigger, view, generated-column, offset/increment, integer-width, and broader
  prepared-parameter variants may follow different SQL-layer ordering and
  remain separate work.

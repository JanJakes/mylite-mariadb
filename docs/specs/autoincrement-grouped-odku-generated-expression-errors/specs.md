# Autoincrement Grouped ODKU Generated-Expression Errors

## Goal

Cover grouped later-in-key `AUTO_INCREMENT` allocation when
`INSERT ... ON DUPLICATE KEY UPDATE` (ODKU) reaches a duplicate-update branch
that fails while evaluating a generated-column expression. MyLite must roll
back rows inserted earlier in the same statement and keep grouped allocation
tied to the live per-prefix maximum.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:2214-2294` handles the duplicate-update branch by
  evaluating the update list before calling `ha_update_row()`.
- `mariadb/sql/table.cc:9243-9490` evaluates generated fields with
  `TABLE::update_virtual_fields()` and returns an error when
  `save_in_field()` leaves the thread in an error state.
- `mariadb/storage/mylite/ha_mylite.cc:1805-1865` reserves one generated
  value at a time for grouped autoincrement definitions.
- `mariadb/storage/mylite/ha_mylite.cc:3293-3365` derives grouped next values
  from live index entries in the current prefix.

## Scope

- Durable MyLite-routed grouped autoincrement tables requested as
  `ENGINE=InnoDB` and `ENGINE=MyISAM`.
- Direct multi-row `INSERT ... VALUES ... ON DUPLICATE KEY UPDATE` where an
  earlier row inserts, a later row enters the duplicate-update branch, and a
  strict stored generated expression overflows.
- Prepared multi-row `INSERT ... VALUES ... ON DUPLICATE KEY UPDATE` with the
  same generated-expression failure shape.
- Direct source-driven `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` with
  the same generated-expression failure shape.
- Close/reopen allocation from live prefix maxima after the failed statements.

## Non-Goals

- Trigger, view, partition, offset/increment, integer-width, multi-table, or
  exhaustive generated-expression matrices.
- Changing first-key autoincrement ODKU reservation behavior.
- Native MyISAM/InnoDB sidecar storage behavior.
- Size-profile reduction work.

## Compatibility Impact

This narrows the broader grouped ODKU expression-error matrix gap. When a
duplicate-update branch fails while evaluating a generated column after an
earlier row was inserted, MyLite rolls back the visible row and index changes,
leaves the duplicate target row unchanged, and derives the next grouped value
from the current live prefix maximum.

The claim remains representative. Trigger, view, partition, offset/increment,
integer-width, multi-table, and exhaustive generated-expression variants remain
planned.

## Design

No production change is expected. The tests rely on existing MariaDB
generated-column evaluation and MyLite statement checkpoints. The grouped
autoincrement path recomputes the next value from live index entries after
rollback, so failed generated-expression updates do not leave a durable
first-key-style reservation gap.

The test uses a strict stored generated `TINYINT` value computed as
`score + 1`; updating the duplicate row to `score = 127` overflows the
generated column deterministically.

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

- Add direct grouped ODKU generated-expression failure coverage.
- Add prepared grouped ODKU generated-expression failure coverage.
- Add source-driven grouped ODKU generated-expression failure coverage.
- Verify inserted rows from failed statements are rolled back, duplicate
  target rows are restored, generated-column index entries are not left behind,
  next generated values resume from live prefix maxima, and close/reopen
  preserves those values.
- Run the focused storage-engine smoke test, statement-rollback and
  routed-DDL/DML compatibility harness groups, shell syntax checks,
  reject-file cleanup checks, and `git diff --check`.

## Acceptance Criteria

- Failed direct grouped ODKU generated-expression updates remove rows inserted
  earlier in the same statement.
- Failed prepared grouped ODKU generated-expression updates remove rows
  inserted earlier in the same statement.
- Failed source-driven grouped ODKU generated-expression updates remove rows
  inserted earlier in the same statement.
- Duplicate target rows remain unchanged after the generated-expression error.
- Generated grouped values resume from live per-prefix maxima before and after
  close/reopen.

## Risks And Open Questions

- Trigger and view ODKU paths may follow distinct SQL-layer ordering and remain
  separate work.
- This remains representative strict generated-expression coverage, not an
  exhaustive generated-column expression matrix.

# Autoincrement Grouped Failed-DML Matrices

## Goal

Broaden failed-DML `AUTO_INCREMENT` coverage for grouped later-in-key
definitions beyond ODKU-specific failures. The slice covers representative
mixed-row insert and update failures where grouped allocation must resume from
live per-prefix maxima rather than preserving first-key-style reserved tails or
rolled-back or ignored explicit high values.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:4372-4518` handles explicit autoincrement values,
  calls the engine to reserve generated values, and treats not-first-in-index
  autoincrement definitions as one-row reservations.
- `mariadb/sql/sql_update.cc:2388-2433` processes matching update rows one at
  a time, verifies SQL-layer conditions, and calls `ha_update_row()` for rows
  that reach handler publication.
- `mariadb/storage/mylite/ha_mylite.cc:1805-1865` routes grouped
  autoincrement reads to the live grouped lookup and reserves only one value at
  a time for grouped definitions.
- `mariadb/storage/mylite/ha_mylite.cc:2040-2138` rejects duplicate and FK
  update failures before publishing the failing row and preserves explicit
  autoincrement pages only after a row passes those checks.
- `mariadb/storage/mylite/ha_mylite.cc:3293-3365` derives grouped next values
  from live index entries in the current prefix.
- `mariadb/storage/mylite/ha_mylite.cc:6135-6164` can advance table-local
  autoincrement pages from explicit values, but grouped allocation ignores
  those pages and uses live prefix maxima.

## Scope

- Durable MyLite-routed grouped autoincrement tables requested as
  `ENGINE=InnoDB` and `ENGINE=MyISAM`.
- Direct and prepared mixed `INSERT IGNORE` statements where generated rows
  surround an ignored duplicate row in the same grouped prefix.
- Direct and prepared ordered multi-row `UPDATE` statements where an earlier
  row attempts an explicit high grouped autoincrement value, a later row fails
  a duplicate-key update, and statement rollback restores visible rows.
- Direct and prepared `UPDATE IGNORE` statements where a duplicate-key skip
  attempts an explicit high grouped autoincrement value.
- Close/reopen allocation from live prefix maxima after the covered statements.

## Non-Goals

- Exhaustive trigger, view, partition, offset/increment, integer-width,
  generated-column, multi-table, or broader `UPDATE IGNORE` matrices.
- First-key table-local autoincrement reservation behavior, which is covered by
  `docs/specs/autoincrement-failed-dml-matrices/specs.md` and
  `docs/specs/autoincrement-prior-success-failed-update-matrices/specs.md`.
- ODKU-specific grouped failure paths, which are covered separately by the
  grouped ODKU specs.
- Native MyISAM/InnoDB sidecar storage behavior.
- Size-profile reduction work.

## Compatibility Impact

This narrows the grouped failed-DML matrix gap. For grouped later-in-key
definitions, generated values are based on live rows in the exact key prefix.
Ignored duplicate insert and update rows do not create durable first-key-style
reserved tail gaps, and explicit high values from rolled-back update rows do
not affect the next generated grouped value after statement rollback.

The claim remains representative. Broader trigger, view, generated-column,
offset/increment, integer-width, multi-table, and `UPDATE IGNORE` matrices
remain planned.

## Design

No production change is expected. The tests rely on existing statement
checkpoints for visible row/index rollback and on grouped autoincrement lookup
from live index entries. The important compatibility decision is explicit:
grouped definitions do not use preserved table-local autoincrement pages to
choose the next generated value.

The update tests intentionally make an earlier row reach handler publication
with an explicit high value before a later row fails a duplicate-key check. If
grouped allocation accidentally used preserved table-local state, the next
generated row would jump to the high value. Correct MyLite grouped behavior
continues from the live prefix maximum after rollback.

The `UPDATE IGNORE` tests make one row attempt an explicit high grouped value
and a duplicate secondary-key value in the same update. MariaDB skips the row,
so MyLite must leave the row image unchanged and derive the next generated id
from the live prefix maximum rather than the attempted high value.

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

- Add direct and prepared mixed grouped `INSERT IGNORE` coverage proving visible
  ids stay consecutive and the next generated id follows the live prefix
  maximum.
- Add direct and prepared grouped prior-success failed-update coverage proving
  explicit high values are rolled back for allocation purposes.
- Add direct and prepared grouped `UPDATE IGNORE` coverage proving skipped
  duplicate updates do not publish attempted explicit high values.
- Verify rows, duplicate-key targets, next generated values, and close/reopen
  persistence.
- Run the focused storage-engine test, statement-rollback and routed-DDL/DML
  compatibility harness groups, shell syntax checks, reject-file cleanup
  checks, and `git diff --check`.

## Acceptance Criteria

- Mixed grouped `INSERT IGNORE` leaves successful rows visible with consecutive
  per-prefix ids around the ignored duplicate row.
- Failed grouped prior-success explicit high updates restore row images and do
  not make the next generated value jump to the attempted high range.
- Grouped `UPDATE IGNORE` skips leave row images unchanged and do not make the
  next generated value jump to the attempted high range.
- Direct and prepared execution cover both `InnoDB` and `MyISAM` routed engine
  declarations.
- Generated grouped values resume from live per-prefix maxima before and after
  close/reopen.

## Risks And Open Questions

- Trigger, view, generated-column, multi-table, offset/increment, integer-width,
  and broader `UPDATE IGNORE` variants may follow different SQL-layer ordering
  and remain separate work.

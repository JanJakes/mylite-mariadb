# Autoincrement Grouped ODKU Failed DML

## Goal

Cover failed `INSERT ... ON DUPLICATE KEY UPDATE` (ODKU) statements on routed
grouped later-in-key `AUTO_INCREMENT` tables. MyLite must roll back visible row
changes while keeping grouped allocation tied to the live per-prefix maximum.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:2214-2351` attempts `ha_write_row()` first and,
  on duplicate-key errors, evaluates the ODKU update list and calls
  `ha_update_row()` for the conflicting row.
- `mariadb/sql/sql_insert.cc:2350-2401` reports non-ignored insert/update
  errors through `on_ha_error()` so the statement aborts.
- `mariadb/sql/sql_insert.cc:4476-4529` executes selected source rows for
  `INSERT ... SELECT` and resets the autoincrement field between source rows.
- `mariadb/storage/mylite/ha_mylite.cc:1805-1865` reserves only one value at a
  time for grouped definitions.
- `mariadb/storage/mylite/ha_mylite.cc:3293-3365` derives grouped next values
  from live index entries in the current prefix.

## Scope

- Durable MyLite-routed grouped autoincrement tables requested as
  `ENGINE=MyISAM` and `ENGINE=InnoDB`.
- Direct multi-row `INSERT ... VALUES ... ON DUPLICATE KEY UPDATE` where an
  earlier row inserts, a later duplicate row enters the update branch, and that
  update fails a secondary unique-key check.
- Direct `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` with the same failure
  shape.
- Prepared multi-row `INSERT ... VALUES ... ON DUPLICATE KEY UPDATE` with the
  same failure shape.
- Close/reopen allocation from live prefix maxima after the failed statements.

## Non-Goals

- Exhaustive grouped ODKU expression, trigger, view, partition,
  offset/increment, integer-width, or prepared parameter-shape matrices.
- Source-read errors that occur before the target handler is called.
- Native MyISAM/Aria/InnoDB sidecar storage behavior.
- Changing first-key autoincrement ODKU reservation behavior.
- Size-profile reduction work.

## Compatibility Impact

This closes the representative grouped ODKU error-path gap. Unlike first-key
durable autoincrement ODKU, grouped allocation does not publish a table-local
reserved tail. When a grouped ODKU statement fails after earlier row
publication, the statement rollback removes those rows and the next generated
value resumes from the current live prefix maximum.

The claim remains representative. Broader matrices stay planned for triggers,
views, generated-column update expressions, offsets, integer widths, prepared
parameter-shape expansion, and source-read error paths. Source-driven
update-expression errors are covered separately in
`docs/specs/autoincrement-grouped-odku-source-driven-update-expression-errors/specs.md`.

## Design

No production change is expected. The existing statement checkpoint rolls back
visible row and index changes. The grouped autoincrement path does not advance
a durable table-local counter; it recomputes from live index entries on the
next statement.

## File Lifecycle

No file-format change is required. Durable table state remains in the primary
`.mylite` file and the tests keep the existing no durable sidecar checks.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct and prepared SQL execution plus close/reopen lifecycle checks.

## Storage-Engine Routing

Coverage uses requested `ENGINE=MyISAM` and `ENGINE=InnoDB`, which route to
MyLite storage in the storage-smoke profile. Native engine sidecars are not
created or claimed.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for failed grouped multi-row `VALUES` ODKU
  after one successful generated insert.
- Add storage-engine smoke coverage for failed grouped `INSERT ... SELECT`
  ODKU after one successful generated source row.
- Add prepared grouped multi-row `VALUES` ODKU failure coverage.
- Verify the earlier inserted rows are rolled back, duplicate target rows are
  restored, next generated values resume from live prefix maxima, and
  close/reopen preserves those values.
- Run the focused storage-engine smoke test, statement-rollback and
  routed-DDL/DML compatibility harness groups, shell syntax checks, and
  `git diff --check`.

## Acceptance Criteria

- Failed grouped multi-row ODKU removes rows inserted earlier in the same
  statement.
- Later duplicate-update failures do not change the duplicate target row.
- Direct `VALUES`, direct `INSERT ... SELECT`, and prepared `VALUES` variants
  resume from live per-prefix maxima rather than a first-key-style reserved
  tail gap.
- Close/reopen keeps grouped allocation based on durable live rows.

## Risks And Open Questions

- Trigger, view, generated-column update-expression, and source-read variants
  may have distinct SQL-layer ordering and remain planned.

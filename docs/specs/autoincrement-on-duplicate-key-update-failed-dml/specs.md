# Autoincrement ON DUPLICATE KEY UPDATE Failed DML

## Goal

Cover failed multi-row `INSERT ... ON DUPLICATE KEY UPDATE` (ODKU) statements
where an earlier generated row is inserted successfully, then a later duplicate
row enters the update branch and fails. MyLite must roll back visible rows while
preserving the generated autoincrement reservation boundary.

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
- `mariadb/sql/handler.cc:4432-4489` uses the multi-value insert row estimate
  for `INSERT ... VALUES` reservations and growing unknown-row-count intervals
  for `INSERT ... SELECT`.
- `mariadb/storage/mylite/ha_mylite.cc:1805-1878` publishes durable first-key
  generated reservations before row publication and marks them for rollback
  preservation.
- `mariadb/storage/mylite/ha_mylite.cc:1900-2005` checks duplicates after
  generated reservation publication, allowing the SQL ODKU branch to run.
- `mariadb/storage/mylite/ha_mylite.cc:2030-2134` applies duplicate-update
  rows through the ordinary update path and can reject later duplicate-key
  conflicts before publishing that row.

## Scope

- Durable MyLite-routed first-key autoincrement tables requested as
  `ENGINE=InnoDB`.
- Multi-row `INSERT ... VALUES ... ON DUPLICATE KEY UPDATE` where the first row
  inserts, the second row duplicates an existing key, and the duplicate-update
  list fails a secondary unique-key check.
- `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` with the same successful
  earlier source row and later duplicate-update failure shape.
- Close/reopen persistence for the preserved next values.

## Non-Goals

- Exhaustive ODKU expression, trigger, view, partition, offset/increment,
  integer-width, or durable insert-id matrices. Representative grouped
  later-in-key failed-update paths are covered separately in
  `docs/specs/autoincrement-grouped-odku-failed-dml/specs.md`.
- `INSERT IGNORE ... ON DUPLICATE KEY UPDATE` and warning diagnostics.
- Native InnoDB old-style autoincrement lock-mode parity.
- Size-profile reduction work.

## Compatibility Impact

The slice broadens MyLite's ODKU autoincrement claim from successful
duplicate-update branches to failed duplicate-update branches after earlier row
publication. The failed statement removes rows inserted earlier by the same
statement, restores updated row images, and keeps MariaDB/InnoDB-style
non-gapless generated reservation boundaries for the next insert.

This remains representative. Broader ODKU matrices stay planned for triggers,
views, source errors, offsets, integer-width boundaries, and durable insert-id
behavior. Representative grouped later-in-key failed-update paths are covered
separately.

## Design

No production change is expected. Existing generated reservation publication
already marks the active statement checkpoint for autoincrement preservation
before duplicate checks. If a later ODKU update branch fails, statement rollback
restores row and index visibility while preserving generated reservation pages
that MariaDB already requested.

The `INSERT ... VALUES` case verifies the multi-value reservation boundary. The
`INSERT ... SELECT` case verifies the unknown-row-count path, where the second
source row grows the reservation interval before the duplicate-update branch
fails.

## File Lifecycle

No file-format change is required. Durable table state remains in the primary
`.mylite` file. The tests assert no durable sidecars after close and after
reopen.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct SQL execution and close/reopen lifecycle checks.

## Storage-Engine Routing

Coverage uses requested `ENGINE=InnoDB`, which routes to MyLite storage in the
default embedded profile. Omitted/default, MyISAM, and Aria first-key tables
share the same durable MyLite path but are not repeated here.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for failed multi-row `INSERT ... VALUES`
  ODKU after one successful generated insert.
- Add storage-engine smoke coverage for failed `INSERT ... SELECT` ODKU after
  one successful generated source row.
- Verify the earlier inserted rows are rolled back, the duplicate target rows
  are restored, the next generated values resume after the preserved
  reservation boundaries, and close/reopen keeps those values.
- Run the focused storage-engine test, statement-rollback and transaction
  compatibility harness groups, shell syntax checks, `git diff --check`, and
  the dev, embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- Failed multi-row ODKU removes rows inserted earlier in the same statement.
- Later duplicate-update failures do not change the duplicate target row.
- `INSERT ... VALUES` and `INSERT ... SELECT` ODKU failures preserve their
  generated reservation boundaries.
- Close/reopen resumes from the preserved next values.
- Roadmap and compatibility docs distinguish this representative error-path
  coverage from broader ODKU matrices.

## Risks And Open Questions

- Other ODKU update failures, especially trigger, view, generated-column, and
  additional grouped-autoincrement variants, may follow different SQL-layer
  paths.
- This does not compare native InnoDB lock modes or public insert-id behavior
  for durable routed ODKU statements.
